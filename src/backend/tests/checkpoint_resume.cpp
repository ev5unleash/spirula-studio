// CPU checkpoint selection and archive-boundary checks.

#include "checkpoint/Resume.h"
#include "core/CheckpointIO.h"
#include "config/TrainConfigJson.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
namespace fs = std::filesystem;

namespace {

int failures = 0;
#define CHECK(condition, ...)                                                \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::fprintf(stderr, "  FAIL: " __VA_ARGS__);                  \
            std::fprintf(stderr, "\n");                                    \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

struct TempDir {
    fs::path path;
    TempDir() {
        for (uint64_t attempt = 0;; ++attempt) {
            const auto stamp =
                std::chrono::steady_clock::now().time_since_epoch().count();
            path = fs::temp_directory_path() /
                   ("spirula_ckpt_resume_" + std::to_string(stamp) + "_" +
                    std::to_string(attempt));
            if (fs::create_directory(path)) return;
        }
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string state_json(int step, bool full = true) {
    return "{\"step\":" + std::to_string(step) +
           ",\"full_resume\":" + (full ? "1" : "0") + "}";
}

void write_config(const fs::path& path, const std::string& data,
                  const char* extra = "") {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    out << "{\"data\":\"" << data << "\",\"num_iterations\":30000"
        << extra << "}\n";
}

void write_npy_member(std::ostream& out, const std::string& name,
                      const std::string& descr = "<f4",
                      const std::vector<uint8_t>& payload = {0, 0, 0, 0}) {
    const std::string header = ckpt::npy_header(
        descr.c_str(), payload.size() / (size_t)(descr[2] - '0'));
    const size_t member_size = header.size() + payload.size();
    ckpt::tar_header(out, name + ".npy", member_size);
    out.write(header.data(), (std::streamsize)header.size());
    if (!payload.empty())
        out.write((const char*)payload.data(), (std::streamsize)payload.size());
    ckpt::tar_pad(out, member_size);
}

void write_checkpoint(const fs::path& dir, int step, bool full = true,
                      bool with_config = true) {
    fs::create_directories(dir);
    std::ofstream out(dir / "state.tar", std::ios::binary);
    if (!out) throw std::runtime_error("cannot write state.tar");
    const std::string state = state_json(step, full);
    ckpt::tar_write_bytes(out, "state.json", state.data(), state.size());
    if (full) {
        write_npy_member(out, "world.means");
        write_npy_member(out, "world.opacities");
    }
    ckpt::tar_finish(out);
    out.close();
    if (with_config) write_config(dir / "config.json", "dataset");
}

std::vector<uint8_t> bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

void write_bytes(const fs::path& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write((const char*)data.data(), (std::streamsize)data.size());
}

template <typename Fn>
bool throws(const Fn& fn) {
    try {
        fn();
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

void selection_and_configs() {
    TempDir tmp;
    const fs::path run = tmp.path / "run";
    const fs::path old = run / "step-000000001.ckpt";
    const fs::path newer = run / "step-000000002.ckpt";
    write_checkpoint(old, 1);
    write_checkpoint(newer, 2);
    write_config(old / "config.json", "snapshot-data");
    write_config(run / "config.json", "root-data");

    auto truncated = bytes(newer / "state.tar");
    truncated.resize(truncated.size() - 1);
    write_bytes(newer / "state.tar", truncated);
    fs::create_directories(run / ".checkpoint-3-0.tmp");
    fs::create_directories(run / "step-000000003.ckpt.tmp");

    const auto r = ckpt::resolve_checkpoint(run);
    CHECK(r.ckpt_dir == fs::absolute(old), "newest valid checkpoint was not selected");
    CHECK(r.config_path == fs::absolute(old / "config.json"),
          "local checkpoint config did not win");

    TrainConfig cli;
    cli.resume = (newer).string();
    std::set<std::string> explicit_flags{"num_iterations"};
    cli.num_iterations = 30000;
    CHECK(throws([&] { (void)ckpt::build_resume_config(cli, "", explicit_flags); }),
          "explicit unusable checkpoint substituted an older one");

    write_checkpoint(tmp.path / "step-1000000000.ckpt", 1000000000);
    CHECK(ckpt::checkpoint_step("step-999999999.ckpt") == 999999999,
          "nine-digit step parse failed");
    CHECK(ckpt::checkpoint_step("step-1000000000.ckpt") == 1000000000,
          "ten-digit step parse failed");
    CHECK(ckpt::checkpoint_step("step-000000001.ckpt.tmp") == -1,
          "malformed checkpoint suffix accepted");
}

void archive_boundaries() {
    TempDir tmp;
    const fs::path good = tmp.path / "step-000000001.ckpt";
    write_checkpoint(good, 1);
    CHECK(ckpt::validate_checkpoint(good, true).find("step") != nullptr,
          "valid archive rejected");

    {
        auto data = bytes(good / "state.tar");
        data.resize(data.size() - 512);
        write_bytes(tmp.path / "truncated-terminator.tar", data);
        std::ifstream in(tmp.path / "truncated-terminator.tar", std::ios::binary);
        CHECK(throws([&] { (void)ckpt::tar_index(in); }),
              "truncated tar terminator accepted");
    }
    {
        auto data = bytes(good / "state.tar");
        data[0] ^= 1;
        write_bytes(tmp.path / "bad-checksum.tar", data);
        std::ifstream in(tmp.path / "bad-checksum.tar", std::ios::binary);
        CHECK(throws([&] { (void)ckpt::tar_index(in); }),
              "corrupt tar checksum accepted");
    }
    {
        std::ofstream out(tmp.path / "duplicate.tar", std::ios::binary);
        const std::string state = state_json(1);
        ckpt::tar_write_bytes(out, "state.json", state.data(), state.size());
        ckpt::tar_write_bytes(out, "state.json", state.data(), state.size());
        ckpt::tar_finish(out);
        out.close();
        std::ifstream in(tmp.path / "duplicate.tar", std::ios::binary);
        CHECK(throws([&] { (void)ckpt::tar_index(in); }),
              "duplicate tar member accepted");
    }
    {
        std::ofstream out(tmp.path / "truncated-npy.tar", std::ios::binary);
        const std::string state = state_json(1);
        ckpt::tar_write_bytes(out, "state.json", state.data(), state.size());
        const std::string magic("\x93NUMPY", 6);
        ckpt::tar_header(out, "world.means.npy", magic.size());
        out.write(magic.data(), (std::streamsize)magic.size());
        ckpt::tar_pad(out, magic.size());
        ckpt::tar_finish(out);
        out.close();
        fs::path bad = tmp.path / "step-000000002.ckpt";
        fs::create_directories(bad);
        fs::copy_file(tmp.path / "truncated-npy.tar", bad / "state.tar");
        CHECK(throws([&] { (void)ckpt::validate_checkpoint(bad, false); }),
              "truncated NPY header accepted");
    }
    CHECK(throws([&] { (void)ckpt::validate_checkpoint(good, true); }) == false,
          "valid octal archive regressed");
}

void config_fallbacks() {
    TempDir tmp;
    const fs::path run = tmp.path / "run";
    const fs::path ckpt = run / "step-000000004.ckpt";
    write_checkpoint(ckpt, 4);
    std::error_code ec;
    fs::remove(ckpt / "config.json", ec);
    write_config(run / "config.json", "legacy-data");
    auto r = ckpt::resolve_checkpoint(run);
    CHECK(r.config_path == fs::absolute(run / "config.json"),
          "legacy root config was not selected");

    write_config(ckpt / "config.json", "snapshot-data");
    r = ckpt::resolve_checkpoint(run);
    CHECK(r.config_path == fs::absolute(ckpt / "config.json"),
          "checkpoint config snapshot did not override root");
    {
        std::ofstream bad(ckpt / "config.json", std::ios::trunc);
        bad << "not json";
    }
    CHECK(throws([&] { (void)ckpt::resolve_checkpoint(run); }),
          "corrupt local config incorrectly fell back to root");

    write_config(ckpt / "config.json", "snapshot-data");
    TrainConfig cli;
    cli.resume = run.string();
    cli.num_iterations = 30000;
    const auto rebuilt = ckpt::build_resume_config(
        cli, "", {"num_iterations"});
    CHECK(rebuilt.num_iterations == 30000,
          "explicit default-valued override was lost");
    CHECK(rebuilt.resume == fs::absolute(ckpt).string(),
          "resume path was not pinned to selected checkpoint");
}

}  // namespace

int main() {
    try {
        selection_and_configs();
        archive_boundaries();
        config_fallbacks();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "checkpoint_resume: %s\n", e.what());
        return 1;
    }
    std::printf("%s\n", failures ? "checkpoint_resume: FAILURES"
                                  : "checkpoint_resume: ok");
    return failures ? 1 : 0;
}
