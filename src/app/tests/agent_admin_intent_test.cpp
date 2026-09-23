#include "app/AgentAdminIntent.h"

#include "app/AgentConfig.h"
#include "app/AgentPairing.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace admin = app::agent::admin;
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

std::array<Socket, 2> connected_pair() {
    RawSocket listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    RawSocket client = kBadSocket;
    RawSocket server = kBadSocket;
    const auto fail = [&] {
        close_socket(listener);
        close_socket(client);
        close_socket(server);
        throw std::runtime_error("loopback socket setup failed");
    };
    if (listener == kBadSocket) fail();
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener, 1) != 0)
        fail();
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

template <typename ClientOperation>
bool exchange(pairing::Leader& leader, ClientOperation&& client_operation) {
    const auto sockets = connected_pair();
    bool server_ok = false;
    std::thread server([&] {
        server_ok = leader.HandleEnrollmentConnection(sockets[1]);
    });
    bool client_ok = false;
    try {
        client_ok = client_operation(sockets[0]);
    } catch (...) {
        client_ok = false;
    }
    server.join();
    return server_ok && client_ok;
}

int failures = 0;

void expect(bool condition, const char* message) {
    std::cout << (condition ? "ok  " : "BAD ") << message << '\n';
    if (!condition) ++failures;
}

std::uint64_t unix_now() {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return seconds > 0 ? static_cast<std::uint64_t>(seconds) : 0;
}

void remove_tree(const fs::path& root) {
    std::error_code ignored;
    fs::remove_all(root, ignored);
}
}  // namespace

int main() {
#ifdef _WIN32
    NetworkScope network;
    (void)network;
#endif
    std::error_code ec;
    const fs::path temp = fs::temp_directory_path(ec);
    if (ec) {
        std::cerr << "cannot resolve temporary directory\n";
        return 1;
    }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = temp / ("spirula-admin-intent-" + std::to_string(stamp));
    if (!fs::create_directory(root, ec) || ec) {
        std::cerr << "cannot create temporary test directory: " << ec.message() << '\n';
        return 1;
    }

    const fs::path leader_root = root / "leader";
    const fs::path worker_root = root / "worker";
    if (!fs::create_directory(leader_root, ec) || ec ||
        !fs::create_directory(worker_root, ec) || ec) {
        std::cerr << "cannot create pairing test directories: " << ec.message() << '\n';
        remove_tree(root);
        return 1;
    }

    std::string error;
    auto leader = pairing::Leader::Open(leader_root, "leader.local", &error);
    auto worker = pairing::Worker::Open(worker_root, &error);
    if (!leader || !worker) {
        std::cerr << "cannot initialize pairing identities: " << error << '\n';
        worker.reset();
        leader.reset();
        remove_tree(root);
        return 1;
    }
    auto invitation = leader->IssueInvitation(std::chrono::minutes(5), &error);
    const auto enrollment_pin = leader->EnrollmentPeerFingerprint();
    const bool enrolled = invitation && exchange(*leader, [&](Socket socket) {
        return worker->Redeem(socket, invitation->code, enrollment_pin,
                              "leader.local", "admin-intent-test");
    });
    if (!enrolled) {
        std::cerr << "worker enrollment failed: " << error << '\n';
        worker.reset();
        leader.reset();
        remove_tree(root);
        return 1;
    }
    const std::string worker_id = worker->WorkerId();
    const bool approved = leader->Approve(worker_id, &error) &&
        exchange(*leader, [&](Socket socket) {
            return worker->Redeem(socket, invitation->code, enrollment_pin,
                                  "leader.local", "admin-intent-test");
        });
    if (!approved) {
        std::cerr << "worker approval failed: " << error << '\n';
        worker.reset();
        leader.reset();
        remove_tree(root);
        return 1;
    }

    std::string signer_pem;
    std::string signer_pin;
    if (!leader->InitializeUpdateSigner(signer_pem, signer_pin, &error)) {
        std::cerr << "cannot initialize independent update signer: " << error << '\n';
        worker.reset();
        leader.reset();
        remove_tree(root);
        return 1;
    }

    const std::uint64_t now = unix_now();
    admin::Intent intent;
    intent.operation = admin::Operation::Reboot;
    intent.worker_id = worker_id;
    intent.leader_id = leader->LeaderId();
    intent.leader_epoch = leader->LeaderEpoch();
    intent.intent_id = "intent-test-001";
    intent.issued_at_unix = now;
    intent.expires_at_unix = now + 120;
    auto signed_intent = admin::SignIntent(*leader, intent, &error);
    expect(signed_intent.has_value(), "approved leader signs a bounded reboot intent");

    app::agent::Config policy;
    policy.allow_reboot = true;
    policy.leader_id = leader->LeaderId();
    policy.update_signer_sha256 = signer_pin;
    if (signed_intent) {
        expect(admin::VerifyIntent(policy, *signed_intent, now, &error),
               "pinned signer and reboot grant authorize the intent");

        app::agent::Config denied_policy = policy;
        denied_policy.allow_reboot = false;
        expect(!admin::VerifyIntent(denied_policy, *signed_intent, now, &error),
               "reboot intent is denied without its local grant");

        app::agent::Config wrong_pin = policy;
        wrong_pin.update_signer_sha256[0] =
            wrong_pin.update_signer_sha256[0] == '0' ? '1' : '0';
        expect(!admin::VerifyIntent(wrong_pin, *signed_intent, now, &error),
               "intent is denied when its signer does not match the admin pin");

        admin::Intent changed = *signed_intent;
        changed.signature.back() ^= 1;
        expect(!admin::VerifyIntent(policy, changed, now, &error),
               "tampered detached signature is rejected");

        changed = *signed_intent;
        changed.worker_id[0] = changed.worker_id[0] == '0' ? '1' : '0';
        expect(!admin::VerifyIntent(policy, changed, now, &error),
               "intent signed for another worker is rejected");

        expect(!admin::VerifyIntent(policy, *signed_intent,
                                    signed_intent->expires_at_unix, &error),
               "expired intent is rejected");

        changed = *signed_intent;
        changed.operation = admin::Operation::Activate;
        changed.update_id = "update-test-001";
        changed.package_sha256 = std::string(64, 'a');
        changed.security_version = 1;
        admin::Intent unversioned = changed;
        unversioned.security_version = 0;
        unversioned.signer_public_key_pem.clear();
        unversioned.signature.clear();
        expect(!admin::SignIntent(*leader, unversioned, &error),
               "activation without a positive security version cannot be signed");
        app::agent::Config activation_policy = policy;
        activation_policy.allow_remote_update = true;
        expect(!admin::VerifyIntent(activation_policy, changed, now, &error),
               "signature cannot be reused across operation domains");
    }

    worker.reset();
    leader.reset();
    remove_tree(root);
    std::cout << (failures ? "FAILED" : "PASSED") << '\n';
    return failures ? 1 : 0;
}
