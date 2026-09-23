#include "app/AgentConfig.h"
#include "app/AgentUpdatePackage.h"
#include "app/AgentPairing.h"
#include "core/Sha256.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <ctime>
#include <stdexcept>
#include <thread>
#include <array>
#include <cstdint>
#include <iterator>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace update = app::agent::update;
namespace pairing = app::agent::pairing;
using Socket = app::agent::TlsChannel::NativeSocket;

namespace {
#ifdef _WIN32
using RawSocket = SOCKET;
constexpr RawSocket kBadSocket = INVALID_SOCKET;
void close_socket(RawSocket socket) {
    if (socket != kBadSocket) closesocket(socket);
}
#else
using RawSocket = int;
constexpr RawSocket kBadSocket = -1;
void close_socket(RawSocket socket) {
    if (socket != kBadSocket) ::close(socket);
}
#endif

Socket native_socket(RawSocket socket) { return static_cast<Socket>(socket); }

std::array<Socket, 2> connected_pair() {
    RawSocket listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    RawSocket client = kBadSocket;
    RawSocket server = kBadSocket;
    auto fail = [&] {
        close_socket(listener);
        close_socket(client);
        close_socket(server);
        throw std::runtime_error("loopback socket setup failed");
    };
    if (listener == kBadSocket) fail();
    int reuse = 1;
#ifdef _WIN32
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse)) != 0) fail();
#else
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) fail();
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener, 1) != 0) fail();
#ifdef _WIN32
    int address_size = sizeof(address);
#else
    socklen_t address_size = sizeof(address);
#endif
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_size) != 0)
        fail();
    client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == kBadSocket ||
        ::connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
        fail();
    server = ::accept(listener, nullptr, nullptr);
    if (server == kBadSocket) fail();
    close_socket(listener);
    return {native_socket(client), native_socket(server)};
}

struct NetworkScope {
#ifdef _WIN32
    NetworkScope() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            throw std::runtime_error("Winsock startup failed");
    }
    ~NetworkScope() { WSACleanup(); }
#else
    NetworkScope() = default;
#endif
};

template <typename ClientOperation>
bool exchange(pairing::Leader& leader, ClientOperation&& client_operation) {
    const auto sockets = connected_pair();
    bool server_ok = false;
    std::string server_error;
    std::thread server([&] {
        try {
            server_ok = leader.HandleEnrollmentConnection(sockets[1], &server_error);
        } catch (...) {
            server_ok = false;
        }
    });
    bool client_ok = false;
    std::string client_error;
    try {
        client_ok = client_operation(sockets[0], &client_error);
    } catch (...) {
        client_ok = false;
    }
    server.join();
    if (!client_ok || !server_ok)
        std::cerr << "enrollment exchange failed (client=" << client_ok << ": "
                  << client_error << "; server=" << server_ok << ": "
                  << server_error << ")\n";
    return server_ok && client_ok;
}

std::string fingerprint_hex(
    const app::agent::TlsChannel::PeerFingerprint& fingerprint) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(fingerprint.size() * 2, '0');
    for (std::size_t i = 0; i < fingerprint.size(); ++i) {
        result[i * 2] = digits[fingerprint[i] >> 4];
        result[i * 2 + 1] = digits[fingerprint[i] & 15];
    }
    return result;
}

int failures = 0;

void expect(bool condition, const char* message) {
    std::cout << (condition ? "ok  " : "BAD ") << message << '\n';
    if (!condition) ++failures;
}

bool write_file(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    return !out.fail();
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

std::string sha256(const std::string& bytes) {
    spirula::Sha256 hash;
    hash.update(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    return hash.hex();
}

void remove_test_tree(const fs::path& root, std::error_code& ec) {
#ifdef _WIN32
    std::wstring path = root.native();
    if (path.rfind(L"\\\\?\\", 0) != 0) {
        path = path.rfind(L"\\\\", 0) == 0
            ? L"\\\\?\\UNC\\" + path.substr(2) : L"\\\\?\\" + path;
    }
    fs::remove_all(fs::path(path), ec);
#else
    fs::remove_all(root, ec);
#endif
}

}  // namespace

int main() {
    NetworkScope network;
    (void)network;
    std::error_code ec;
    const fs::path temp = fs::canonical(fs::temp_directory_path(ec), ec);
    if (ec) {
        std::cerr << "cannot resolve temporary directory\n";
        return 1;
    }
    fs::path root;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt != 32; ++attempt) {
        root = temp / ("spirula-update-package-" + std::to_string(stamp) + "-" +
                       std::to_string(attempt));
        if (fs::create_directory(root, ec)) break;
        if (ec != std::errc::file_exists) {
            std::cerr << "cannot create test directory: " << ec.message() << '\n';
            return 1;
        }
        ec.clear();
        root.clear();
    }
    if (root.empty()) {
        std::cerr << "cannot reserve test directory\n";
        return 1;
    }

    const fs::path install = root / "install";
    const fs::path storage = root / "storage";
    const fs::path package_root = root / "package";
    const fs::path leader_root = root / "leader";
    const fs::path worker_root = root / "worker";
    const fs::path unpaired_root = root / "unpaired-worker";
    const bool directories_created =
        fs::create_directory(install, ec) &&
        fs::create_directory(storage, ec) &&
        fs::create_directory(package_root, ec) &&
        fs::create_directory(leader_root, ec) &&
        fs::create_directory(worker_root, ec) &&
        fs::create_directory(unpaired_root, ec);
    if (!directories_created) {
        std::cerr << "cannot create test directories: " << ec.message() << '\n';
        remove_test_tree(root, ec);
        return 1;
    }

    const fs::path executable = install /
#ifdef _WIN32
        "spirula.exe";
#else
        "spirula";
#endif
    const fs::path source = package_root / "worker-package.bin";
    const std::string known_good = "retained known-good executable";
    const std::string approved_bytes = "verified worker executable payload";
    const bool files_created = write_file(executable, known_good) &&
                               write_file(source, approved_bytes);
    expect(files_created, "test files created");
    if (!files_created) {
        remove_test_tree(root, ec);
        return 1;
    }

    std::string pairing_error;
    auto leader = pairing::Leader::Open(leader_root, "leader.local", &pairing_error);
    auto worker = pairing::Worker::Open(worker_root, &pairing_error);
    if (!leader || !worker) {
        expect(false, "protected leader and worker identities opened");
        worker.reset();
        leader.reset();
        remove_test_tree(root, ec);
        return 1;
    }
    auto invitation = leader->IssueInvitation(std::chrono::minutes(5), &pairing_error);
    const auto enrollment_pin = leader->EnrollmentPeerFingerprint();
    const bool enrolled = invitation && exchange(*leader, [&](Socket socket, std::string* error) {
        return worker->Redeem(socket, invitation->code, enrollment_pin,
                              "leader.local", "update-test-worker", error);
    });
    expect(enrolled && worker->Status() == pairing::WorkerStatus::PendingApproval,
           "enrollment creates a pending worker identity");
    if (!enrolled) {
        worker.reset();
        leader.reset();
        remove_test_tree(root, ec);
        return 1;
    }
    const std::string worker_id = worker->WorkerId();
    const bool leader_approved = leader->Approve(worker_id, &pairing_error);
    if (!leader_approved)
        std::cerr << "leader approval failed: " << pairing_error << '\n';
    const bool approved = leader_approved &&
        exchange(*leader, [&](Socket socket, std::string* error) {
            return worker->Redeem(socket, invitation->code, enrollment_pin,
                                  "leader.local", "update-test-worker", error);
        });
    expect(approved && worker->Status() == pairing::WorkerStatus::Paired,
           "only the leader-approved worker becomes eligible for updates");
    if (!approved) {
        worker.reset();
        leader.reset();
        remove_test_tree(root, ec);
        return 1;
    }

    std::string signer_fingerprint;
    std::string signer_public_key_pem;
    const bool signer_initialized = leader->InitializeUpdateSigner(
        signer_public_key_pem, signer_fingerprint, &pairing_error);
    expect(signer_initialized && !signer_public_key_pem.empty(),
           "dedicated update signer is explicitly initialized");
    expect(signer_fingerprint != fingerprint_hex(enrollment_pin),
           "update signer identity is independent of enrollment TLS identity");
    if (!signer_initialized) {
        worker.reset();
        leader.reset();
        remove_test_tree(root, ec);
        return 1;
    }
    std::string persisted_public_key_pem;
    std::string persisted_fingerprint;
    const bool signer_persisted = leader->InitializeUpdateSigner(
        persisted_public_key_pem, persisted_fingerprint, &pairing_error);
    expect(signer_persisted &&
               persisted_public_key_pem == signer_public_key_pem &&
               persisted_fingerprint == signer_fingerprint,
           "update signer identity is persisted across initialization");

    app::agent::service::Configuration configuration;
    configuration.executable = executable;
    configuration.storage_root = storage;
    const update::PlatformIdentity platform = update::CurrentPlatformIdentity();
    update::PackageManifest manifest;
    manifest.os = platform.os;
    manifest.architecture = platform.architecture;
    manifest.build = platform.build;
    manifest.release = "test-release-1";
    manifest.size = static_cast<std::uint64_t>(approved_bytes.size());
    manifest.sha256 = sha256(approved_bytes);

    app::agent::Config policy;
    policy.allow_remote_update = true;
    policy.leader_id = leader->LeaderId();
    policy.update_signer_sha256 = signer_fingerprint;
    const std::time_t now = std::time(nullptr);
    auto offer = update::SignPackageOffer(
        *leader, worker_id,
        now > 0 ? static_cast<std::uint64_t>(now) + 3600 : 0,
        "update-0001", manifest, &pairing_error);
    expect(offer.has_value(), "approved leader signs a bounded package offer");
    if (!offer) {
        worker.reset();
        leader.reset();
        remove_test_tree(root, ec);
        return 1;
    }

    app::agent::Config disabled_policy;
    expect(!update::AuthorizePackageOffer(
               disabled_policy, *worker, *offer, {}, &pairing_error),
           "remote updates are denied by default");
    {
        auto unpaired_worker = pairing::Worker::Open(unpaired_root, &pairing_error);
        expect(unpaired_worker.has_value(), "unpaired worker identity opens");
        if (unpaired_worker)
            expect(!update::AuthorizePackageOffer(
                       policy, *unpaired_worker, *offer, {}, &pairing_error),
                   "unpaired worker cannot authorize a signed offer");
    }

    auto reject_offer = [&](update::PackageOffer altered, const char* message) {
        expect(!update::AuthorizePackageOffer(
                   policy, *worker, altered, {}, &pairing_error), message);
    };
    update::PackageOffer changed = *offer;
    changed.manifest.os = platform.os == "windows" ? "linux" : "windows";
    reject_offer(changed, "tampered platform is rejected");
    changed = *offer;
    changed.manifest.architecture =
        platform.architecture == "x86_64" ? "aarch64" : "x86_64";
    reject_offer(changed, "tampered architecture is rejected");
    changed = *offer;
    changed.manifest.build = "tampered-build";
    reject_offer(changed, "tampered build/version is rejected");
    changed = *offer;
    changed.manifest.release = "tampered-release";
    reject_offer(changed, "tampered release version is rejected");
    changed = *offer;
    changed.manifest.sha256[0] =
        changed.manifest.sha256[0] == '0' ? '1' : '0';
    reject_offer(changed, "tampered package hash is rejected");
    changed = *offer;
    ++changed.expires_at_unix;
    reject_offer(changed, "tampered expiration is rejected");
    changed = *offer;
    changed.signature.back() ^= 1;
    reject_offer(changed, "tampered detached signature is rejected");
    changed = *offer;
    changed.worker_id[0] = changed.worker_id[0] == '0' ? '1' : '0';
    reject_offer(changed, "offer for a foreign worker is rejected");
    changed = *offer;
    changed.leader_id[0] = changed.leader_id[0] == '0' ? '1' : '0';
    reject_offer(changed, "offer from a foreign leader is rejected");

    app::agent::Config wrong_pin = policy;
    wrong_pin.update_signer_sha256[0] =
        wrong_pin.update_signer_sha256[0] == '0' ? '1' : '0';
    expect(!update::AuthorizePackageOffer(
               wrong_pin, *worker, *offer, {}, &pairing_error),
           "wrong admin-pinned signer is rejected");
    auto expired = update::SignPackageOffer(
        *leader, worker_id, 1, "expired-update", manifest, &pairing_error);
    expect(expired && !update::AuthorizePackageOffer(
               policy, *worker, *expired, {}, &pairing_error),
           "expired signed offer is rejected");
    const std::vector<std::string> consumed{"update-0001"};
    expect(!update::AuthorizePackageOffer(
               policy, *worker, *offer, consumed, &pairing_error),
           "caller-provided consumed update IDs reject replay");

    auto authorized = update::AuthorizePackageOffer(
        policy, *worker, *offer, {}, &pairing_error);
    expect(authorized.has_value(),
           "valid offer passes signer, policy, expiry, and pairing checks");
    if (!authorized) {
        worker.reset();
        leader.reset();
        remove_test_tree(root, ec);
        return 1;
    }
    const update::StageResult denied_stage = update::StageUpdatePackage(
        configuration, disabled_policy, *worker, package_root,
        "worker-package.bin", *authorized);
    expect(!denied_stage &&
               denied_stage.error == update::StageError::InvalidArgument &&
               denied_stage.staged_executable.empty(),
           "disabled local policy prevents staging after authorization");

    expect(write_file(source, "tampered worker executable payload"),
           "tampered source written");
    const update::StageResult tampered_stage = update::StageUpdatePackage(
        configuration, policy, *worker, package_root,
        "worker-package.bin", *authorized);
    expect(!tampered_stage && tampered_stage.error == update::StageError::Integrity &&
               tampered_stage.staged_executable.empty(),
           "package hash remains an integrity check after authorization");

    expect(write_file(source, approved_bytes), "approved source restored");
    const update::StageResult staged = update::StageUpdatePackage(
        configuration, policy, *worker, package_root,
        "worker-package.bin", *authorized);
    if (!staged)
        std::cerr << "StageUpdatePackage failed ("
                  << static_cast<int>(staged.error) << "): "
                  << staged.message << '\n';
    expect(static_cast<bool>(staged) && !staged.staged_executable.empty(),
           "authorized compatible package is staged without activation");
    if (staged) {
        expect(read_file(staged.staged_executable) == approved_bytes,
               "returned staged executable has signed bytes");
        expect(read_file(executable) == known_good,
               "retained installed executable is unchanged");
    }

    fs::path staged_path;
    if (staged) staged_path = staged.staged_executable;
#ifdef _WIN32
    if (!staged_path.empty())
        SetFileAttributesW(staged_path.c_str(), FILE_ATTRIBUTE_NORMAL);
#endif
    worker.reset();
    leader.reset();
    remove_test_tree(root, ec);
    expect(!ec, "temporary files cleaned up");
    std::cout << (failures ? "FAILED" : "PASSED") << '\n';
    return failures ? 1 : 0;
}
