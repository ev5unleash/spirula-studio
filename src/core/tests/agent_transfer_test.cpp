#include "app/AgentTransfer.h"
#include "core/Sha256.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <stdexcept>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace agent = app::agent;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
    if (!condition) ++failures;
}

std::string digest(const std::string& bytes) {
    spirula::Sha256 hash;
    hash.update(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    return hash.hex();
}

void write_file(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("test setup write failed");
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

struct Link {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::vector<std::uint8_t>> frames[2];
    bool closed[2] = {false, false};
    std::size_t sent = 0;
    std::size_t fail_after = static_cast<std::size_t>(-1);
    bool corrupt_first_chunk = false;
};

struct Endpoint {
    Link* link = nullptr;
    int side = 0;
};

bool send_frame(void* context, const std::uint8_t* bytes, std::size_t size,
                std::string*) {
    auto& endpoint = *static_cast<Endpoint*>(context);
    Link& link = *endpoint.link;
    std::lock_guard<std::mutex> lock(link.mutex);
    if (endpoint.side == 0) {
        if (link.sent >= link.fail_after) {
            link.closed[endpoint.side] = true;
            link.ready.notify_all();
            return false;
        }
        ++link.sent;
    }
    std::vector<std::uint8_t> frame(bytes, bytes + size);
    if (endpoint.side == 0 && link.corrupt_first_chunk && frame.size() > 9 &&
        frame[0] == 'C') {
        frame[9] ^= 0x80;
        link.corrupt_first_chunk = false;
    }
    link.frames[endpoint.side].push_back(std::move(frame));
    link.ready.notify_all();
    return true;
}

bool receive_frame(void* context, std::vector<std::uint8_t>& bytes,
                   std::string*) {
    auto& endpoint = *static_cast<Endpoint*>(context);
    Link& link = *endpoint.link;
    std::unique_lock<std::mutex> lock(link.mutex);
    const int source = 1 - endpoint.side;
    link.ready.wait(lock, [&] {
        return !link.frames[source].empty() || link.closed[source];
    });
    if (link.frames[source].empty()) return false;
    bytes = std::move(link.frames[source].front());
    link.frames[source].pop_front();
    return true;
}

struct PairResult {
    agent::TransferResult sent;
    agent::TransferResult received;
};

PairResult transfer_pair(Link& link, const fs::path& source,
                          const fs::path& destination,
                          const std::vector<agent::TransferFile>& manifest) {
    Endpoint sender{&link, 0}, receiver{&link, 1};
    const agent::detail::FrameTransport from_sender{&sender, send_frame, receive_frame};
    const agent::detail::FrameTransport from_receiver{&receiver, send_frame, receive_frame};
    PairResult result;
    std::thread receive([&] {
        result.received = agent::detail::ReceiveArtifacts(
            from_receiver, destination, manifest, 4 * 1024 * 1024);
    });
    result.sent = agent::detail::SendArtifacts(
        from_sender, source, manifest, 4 * 1024 * 1024);
    receive.join();
    return result;
}

}  // namespace

int main() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    // Keep staging paths short under CTest's deep Windows scratch directory.
    const fs::path root = fs::temp_directory_path() /
        ("t-" + std::to_string(nonce));
    const fs::path traversal_target = root.parent_path() /
        (root.filename().string() + "-outside.bin");
    try {
        const std::string contents = [&] {
            std::string value(180000, '\0');
            for (std::size_t i = 0; i < value.size(); ++i)
                value[i] = static_cast<char>(i % 251);
            return value;
        }();
        const fs::path source = root / "source";
        const fs::path interrupted = root / "interrupted-stage";
        const fs::path corrupt = root / "corrupt-stage";
        fs::create_directories(source);
        fs::create_directories(interrupted);
        fs::create_directories(corrupt);
        write_file(source / "artifacts" / "model.bin", contents);
        const std::vector<agent::TransferFile> manifest = {{
            "artifacts/model.bin", contents.size(), digest(contents)}};
        write_file(traversal_target, "untouched");

        agent::TransferFile traversal{"../outside.bin", 0, digest("")};
        Link unused;
        Endpoint endpoint{&unused, 0};
        const auto invalid = agent::detail::SendArtifacts(
            {&endpoint, send_frame, receive_frame}, source, {traversal}, 1024);
        check(invalid.error == agent::TransferError::InvalidManifest,
              "rejects relative-path escape before framing");
        const std::string hostile_manifest =
            "{\"v\":1,\"op\":\"manifest\",\"files\":[{\"path\":\"../outside.bin\","
            "\"size\":\"0\",\"sha256\":\"" + digest("") + "\"}]}";
        Link hostile_link;
        hostile_link.frames[0].emplace_back(hostile_manifest.begin(),
                                             hostile_manifest.end());
        Endpoint hostile_receiver{&hostile_link, 1};
        const auto hostile = agent::detail::ReceiveArtifacts(
            {&hostile_receiver, send_frame, receive_frame}, interrupted,
            manifest, 4 * 1024 * 1024);
        check(hostile.error == agent::TransferError::InvalidManifest &&
              read_file(traversal_target) == "untouched",
              "receiver rejects a remote traversal manifest");

        const std::string empty_digest = digest("");
        const fs::path large_manifest_root = root / "large-manifest-stage";
        fs::create_directories(large_manifest_root);
        std::vector<agent::TransferFile> large_manifest;
        large_manifest.reserve(4096);
        for (std::size_t i = 0; i < 4096; ++i)
            large_manifest.push_back({
                "artifacts/empty-" + std::to_string(i) + ".bin", 0, empty_digest});
        Link large_manifest_link;
        large_manifest_link.closed[0] = true;
        Endpoint large_manifest_receiver{&large_manifest_link, 1};
        const auto large_manifest_result = agent::detail::ReceiveArtifacts(
            {&large_manifest_receiver, send_frame, receive_frame},
            large_manifest_root, large_manifest, 20 * 1024 * 1024);
        check(large_manifest_result.error == agent::TransferError::Interrupted &&
              fs::is_empty(large_manifest_root),
              "allows 4096 empty files within a realistic metadata budget");

        const fs::path flood_root = root / "metadata-flood-stage";
        fs::create_directories(flood_root);
        std::string flood_manifest =
            "{\"v\":1,\"op\":\"manifest\",\"files\":[";
        for (std::size_t i = 0; i < 2048; ++i) {
            const std::string path =
                "artifacts/group-" + std::to_string(i) + "/empty.bin";
            if (i) flood_manifest += ',';
            flood_manifest += "{\"path\":\"" + path +
                "\",\"size\":\"0\",\"sha256\":\"" + empty_digest + "\"}";
        }
        flood_manifest += "]}";
        Link flood_link;
        flood_link.frames[0].emplace_back(flood_manifest.begin(),
                                           flood_manifest.end());
        Endpoint flood_receiver{&flood_link, 1};
        const auto flooded = agent::detail::ReceiveArtifacts(
            {&flood_receiver, send_frame, receive_frame}, flood_root,
            manifest, 1024 * 1024);
        check(flooded.error == agent::TransferError::QuotaExceeded &&
              fs::is_empty(flood_root),
              "rejects zero-payload metadata floods before creating staging artifacts");
        const fs::path quota_root = root / "quota-stage";
        fs::create_directories(quota_root);
        write_file(quota_root / ".agent-transfer-v1" /
                       (std::string(64, '0') + ".part"),
                   std::string(64 * 1024, 'x'));
        const std::string valid_manifest =
            "{\"v\":1,\"op\":\"manifest\",\"files\":[{\"path\":\"artifacts/model.bin\","
            "\"size\":\"" + std::to_string(contents.size()) + "\",\"sha256\":\"" +
            digest(contents) + "\"}]}";
        Link quota_link;
        quota_link.frames[0].emplace_back(valid_manifest.begin(), valid_manifest.end());
        Endpoint quota_receiver{&quota_link, 1};
        const auto over_budget = agent::detail::ReceiveArtifacts(
            {&quota_receiver, send_frame, receive_frame}, quota_root, manifest,
            contents.size() + 32 * 1024);
        check(over_budget.error == agent::TransferError::QuotaExceeded,
              "stale partial bytes are charged against the transfer disk budget");
        const fs::path tiny_root = root / "tiny-stage";
        fs::create_directories(tiny_root);
        Link tiny_link;
        tiny_link.frames[0].emplace_back(valid_manifest.begin(), valid_manifest.end());
        const std::string file_notice =
            "{\"v\":1,\"op\":\"file\",\"path\":\"artifacts/model.bin\"}";
        tiny_link.frames[0].emplace_back(file_notice.begin(), file_notice.end());
        std::vector<std::uint8_t> short_chunk(10, 0);
        short_chunk[0] = 'C';
        short_chunk[9] = static_cast<std::uint8_t>(contents[0]);
        tiny_link.frames[0].push_back(std::move(short_chunk));
        tiny_link.closed[0] = true;
        Endpoint tiny_receiver{&tiny_link, 1};
        const auto undersized = agent::detail::ReceiveArtifacts(
            {&tiny_receiver, send_frame, receive_frame}, tiny_root, manifest,
            4 * 1024 * 1024);
        check(undersized.error == agent::TransferError::Protocol &&
              !fs::exists(tiny_root / "artifacts" / "model.bin"),
              "rejects a progressing undersized chunk before writing or publication");

        Link interrupted_link;
        interrupted_link.fail_after = 3; // manifest, file notice, one data chunk
        PairResult first = transfer_pair(interrupted_link, source, interrupted, manifest);
        check(first.sent.error == agent::TransferError::Interrupted &&
              first.received.error == agent::TransferError::Interrupted,
              "interrupted transfer preserves a resumable partial");
        check(!fs::exists(interrupted / "artifacts" / "model.bin"),
              "interrupted bytes are not published as a complete file");
        bool has_resumable_bytes = false;
        for (const auto& entry : fs::directory_iterator(
                 interrupted / ".agent-transfer-v1")) {
            std::error_code partial_error;
            const auto size = fs::file_size(entry.path(), partial_error);
            has_resumable_bytes |= !partial_error && size > 0 &&
                                   size < contents.size();
        }
        check(has_resumable_bytes,
              "interruption retains a nonempty, bounded partial offset");

        Link resume_link;
        PairResult resumed = transfer_pair(resume_link, source, interrupted, manifest);
        check(static_cast<bool>(resumed.sent) && static_cast<bool>(resumed.received),
              "resumed transfer completes on both endpoints");
        check(read_file(interrupted / "artifacts" / "model.bin") == contents,
              "published file matches the complete source bytes");

        Link corrupt_link;
        corrupt_link.corrupt_first_chunk = true;
        PairResult corrupted = transfer_pair(corrupt_link, source, corrupt, manifest);
        check(corrupted.sent.error == agent::TransferError::Integrity &&
              corrupted.received.error == agent::TransferError::Integrity,
              "receiver rejects a corrupted payload by full-file digest");
        check(!fs::exists(corrupt / "artifacts" / "model.bin"),
              "corrupt payload is never published");

        const fs::path outside = root / "outside";
        const fs::path linked_root = root / "linked-stage";
        fs::create_directories(outside);
        fs::create_directories(linked_root);
        write_file(outside / "keep.bin", "untouched");
        std::error_code ec;
        fs::create_hard_link(outside / "keep.bin", linked_root / "keep.bin", ec);
        if (!ec) {
            const std::string replacement = "replacement";
            write_file(source / "keep.bin", replacement);
            const std::vector<agent::TransferFile> linked_manifest = {{
                "keep.bin", replacement.size(), digest(replacement)}};
            Link hardlink_link;
            PairResult linked = transfer_pair(hardlink_link, source, linked_root,
                                               linked_manifest);
            check(linked.received.error == agent::TransferError::Filesystem &&
                  read_file(outside / "keep.bin") == "untouched",
                  "hard-linked destination is rejected without modifying its target");
        }

        const fs::path symlink_root = root / "symlink-stage";
        const fs::path symlink_target = root / "symlink-target";
        fs::create_directories(symlink_root);
        fs::create_directories(symlink_target);
        write_file(symlink_target / "keep.bin", "untouched");
        ec.clear();
        fs::create_directory_symlink(symlink_target, symlink_root / "escape", ec);
        if (!ec) {
            const std::string replacement = "replacement";
            write_file(source / "escape" / "keep.bin", replacement);
            const std::vector<agent::TransferFile> symlink_manifest = {{
                "escape/keep.bin", replacement.size(), digest(replacement)}};
            Link symlink_link;
            PairResult linked = transfer_pair(symlink_link, source, symlink_root,
                                               symlink_manifest);
            check(linked.received.error == agent::TransferError::Filesystem &&
                  read_file(symlink_target / "keep.bin") == "untouched",
                  "symlinked parent is rejected without escaping the staging root");
        }
    } catch (const std::exception& error) {
        std::printf("FAIL unexpected exception: %s\n", error.what());
        ++failures;
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::remove(traversal_target, ec);
    return failures ? 1 : 0;
}
