#include "app/AgentPairing.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace agent = app::agent;
namespace pairing = app::agent::pairing;
using Socket = agent::TlsChannel::NativeSocket;

namespace {

int failures = 0;

void check(bool ok, const char* message) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", message);
    if (!ok) ++failures;
}

#ifdef _WIN32
using RawSocket = SOCKET;
constexpr RawSocket kBadSocket = INVALID_SOCKET;
void close_socket(RawSocket socket) { if (socket != kBadSocket) closesocket(socket); }
#else
using RawSocket = int;
constexpr RawSocket kBadSocket = -1;
void close_socket(RawSocket socket) { if (socket != kBadSocket) ::close(socket); }
#endif

Socket native_socket(RawSocket socket) {
    return static_cast<Socket>(socket);
}

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

struct ExchangeResult {
    bool client = false;
    bool server = false;
};

template <typename ClientOperation>
ExchangeResult exchange(pairing::Leader& leader, ClientOperation&& client_operation,
                        bool expected_success = true) {
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
    if (client_ok != expected_success || server_ok != expected_success)
        std::fprintf(stderr, "enrollment exchange mismatch (client=%d: %s; server=%d: %s)\n",
                     client_ok, client_error.c_str(), server_ok, server_error.c_str());
    return {client_ok, server_ok};
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

int run(const fs::path& root) {
    auto verify = [&](bool ok, const char* message) { check(ok, message); };
    const fs::path leader_root = root / "leader";
    const fs::path worker_root = root / "worker";
    const fs::path second_worker_root = root / "second-worker";
    const fs::path expired_worker_root = root / "expired-worker";
    fs::create_directories(leader_root);
    fs::create_directories(worker_root);
    fs::create_directories(second_worker_root);
    fs::create_directories(expired_worker_root);

    std::string error;
    auto leader = pairing::Leader::Open(leader_root, "leader.local", &error);
    auto worker = pairing::Worker::Open(worker_root, &error);
    auto second_worker = pairing::Worker::Open(second_worker_root, &error);
    auto expired_worker = pairing::Worker::Open(expired_worker_root, &error);
    if (!leader || !worker || !second_worker || !expired_worker) {
        verify(false, "opens protected leader and worker identities");
        return 1;
    }
    const auto duplicate_leader = pairing::Leader::Open(leader_root, "leader.local", &error);
    const auto duplicate_worker = pairing::Worker::Open(worker_root, &error);
    verify(!duplicate_leader && !duplicate_worker,
           "exclusive storage locks reject a second live leader or worker");


    const auto pin = leader->EnrollmentPeerFingerprint();
    const std::string leader_id = leader->LeaderId();
    const std::uint64_t epoch = leader->LeaderEpoch();
    verify(!leader_id.empty() && epoch == 1 && pin != agent::TlsChannel::PeerFingerprint{},
           "creates a stable leader identity and enrollment pin");
    verify(!leader->OptionsForLeader(), "keeps the operational listener closed without approved workers");

    auto invitation = leader->IssueInvitation();
    verify(invitation.has_value(), "issues an expiring one-use invitation");
    if (!invitation) return 1;
    auto wrong_pin = pin;
    wrong_pin[0] ^= 0x80;
    verify(!worker->Redeem(agent::TlsChannel::kInvalidSocket, invitation->code,
                           wrong_pin, "leader.local", "worker-a") &&
               worker->Status() == pairing::WorkerStatus::Unpaired,
           "rejects an invitation when the independently provisioned leader pin differs");
    verify(!worker->Redeem(agent::TlsChannel::kInvalidSocket, invitation->code,
                           pin, "other.local", "worker-a") &&
               worker->Status() == pairing::WorkerStatus::Unpaired,
           "invitation is bound to the leader's configured TLS name");

    const auto enrolled = exchange(*leader, [&](Socket socket, std::string* error) {
        return worker->Redeem(socket, invitation->code, pin, "leader.local", "worker-a", error);
    });
    verify(enrolled.client && enrolled.server &&
               worker->Status() == pairing::WorkerStatus::PendingApproval &&
               leader->Workers().size() == 1 &&
               leader->Workers().front().status == pairing::WorkerStatus::PendingApproval,
           "enrollment creates only a pending worker identity");
    const std::string worker_id = worker->WorkerId();
    worker.reset();
    auto recovered_pending = pairing::Worker::Open(worker_root, &error);
    verify(recovered_pending &&
               recovered_pending->Status() == pairing::WorkerStatus::PendingApproval &&
               recovered_pending->WorkerId() == worker_id,
           "restart preserves the local identity while approval is pending");
    if (!recovered_pending) return 1;
    worker = std::move(recovered_pending);
    verify(!leader->OptionsForLeader() &&
               !worker->ClientOptionsForLeader("127.0.0.1", "leader.local", 7443),
           "pending enrollment cannot open an operational channel");

    const auto reused = exchange(*leader, [&](Socket socket, std::string* error) {
        return second_worker->Redeem(socket, invitation->code, pin,
                                     "leader.local", "worker-b", error);
    }, false);
    const auto pending = exchange(*leader, [&](Socket socket, std::string* error) {
        return worker->CheckApproval(socket, invitation->code, pin, "leader.local", error);
    });
    verify(!reused.client && !reused.server &&
               second_worker->Status() == pairing::WorkerStatus::Unpaired,
           "a consumed invitation cannot enroll a second worker");
    verify(pending.client && pending.server &&
               worker->Status() == pairing::WorkerStatus::PendingApproval,
           "approval polling preserves pending state until the leader approves");


    const bool leader_approved = leader->Approve(worker_id, &error);
    if (!leader_approved)
        std::fprintf(stderr, "leader approval failed: %s\n", error.c_str());
    verify(leader_approved, "leader approval persists a worker certificate");
    verify(!leader->Approve(worker_id, &error), "approval cannot be applied twice");
    const auto approved = exchange(*leader, [&](Socket socket, std::string* error) {
        return worker->Redeem(socket, invitation->code, pin,
                              "leader.local", "worker-a", error);
    });
    const auto client_options = worker->ClientOptionsForLeader("127.0.0.1", "leader.local", 7443);
    const auto leader_options = leader->OptionsForLeader();
    agent::TlsChannel::PeerFingerprint worker_pin{};
    if (leader_options && !leader_options->approved_peer_spki_sha256.empty())
        worker_pin = leader_options->approved_peer_spki_sha256.front();
    const auto paired_info = leader->WorkerForPeer(worker_pin);
    verify(approved.client && approved.server &&
               worker->Status() == pairing::WorkerStatus::Paired && client_options &&
               client_options->tls.enabled && client_options->tls.paired &&
               client_options->tls.expected_peer_spki_sha256 == pin && leader_options &&
               leader_options->approved_peer_spki_sha256.size() == 1 && paired_info &&
               paired_info->id == worker_id &&
               paired_info->status == pairing::WorkerStatus::Paired && !paired_info->revoked,
           "approval enables only the pinned operational TLS identity");

    worker.reset();
    leader.reset();
    auto recovered_worker = pairing::Worker::Open(worker_root, &error);
    auto recovered_leader = pairing::Leader::Open(leader_root, "leader.local", &error);
    verify(recovered_worker && recovered_worker->Status() == pairing::WorkerStatus::Paired &&
               recovered_worker->WorkerId() == worker_id && recovered_leader &&
               recovered_leader->LeaderId() == leader_id &&
               recovered_leader->LeaderEpoch() == epoch,
           "restart preserves worker identity and leader epoch");
    if (!recovered_worker || !recovered_leader) return 1;
    worker = std::move(recovered_worker);
    leader = std::move(recovered_leader);

    verify(leader->Revoke(worker_id) && leader->LeaderEpoch() == epoch &&
               !leader->OptionsForLeader(),
           "revocation removes the worker pin without rotating the leader epoch");
    const auto revoked_info = leader->WorkerForPeer(worker_pin);
    verify(revoked_info && revoked_info->revoked &&
               revoked_info->status == pairing::WorkerStatus::Unpaired,
           "revoked certificates are reported as non-paired");
    verify(worker->Forget() && worker->Status() == pairing::WorkerStatus::Unpaired &&
               !worker->ClientOptionsForLeader("127.0.0.1", "leader.local", 7443),
           "explicit local forget removes persisted worker credentials");

    auto replacement_invitation = leader->IssueInvitation();
    bool replacement_ok = false;
    bool replacement_server_ok = false;
    if (replacement_invitation) {
        const auto replacement = exchange(*leader, [&](Socket socket, std::string* error) {
            return worker->Redeem(socket, replacement_invitation->code, pin,
                                  "leader.local", "worker-a", error);
        });
        replacement_ok = replacement.client &&
                         worker->Status() == pairing::WorkerStatus::PendingApproval;
        replacement_server_ok = replacement.server;
    }
    verify(replacement_ok && replacement_server_ok && leader->Workers().size() == 2 &&
               leader->Workers().front().revoked,
           "a forgotten worker can re-pair with a new identity after leader revocation");

    auto expiring_invitation = leader->IssueInvitation(std::chrono::seconds(1));
    if (!expiring_invitation) {
        verify(false, "creates a short-lived invitation for expiry check");
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        const auto expired = exchange(*leader, [&](Socket socket, std::string* error) {
            return expired_worker->Redeem(socket, expiring_invitation->code, pin,
                                          "leader.local", "worker-expired", error);
        }, false);
    }
    return 0;
}

}  // namespace

int main() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("spirula-agent-pairing-" + std::to_string(nonce));
    try {
        NetworkScope network;
        fs::create_directories(root);
        run(root);
        std::error_code error;
        fs::remove_all(root, error);
        if (error) {
            check(false, "removes temporary pairing state");
            ++failures;
        }
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::printf("FAIL pairing scenario: %s\n", error.what());
        std::error_code cleanup_error;
        fs::remove_all(root, cleanup_error);
        return 1;
    }
}
