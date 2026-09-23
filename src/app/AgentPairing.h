#pragma once

#include "app/AgentTls.h"

#include <atomic>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace app::agent::pairing {

enum class WorkerStatus {
    Unpaired,
    PendingApproval,
    Paired,
};

struct Invitation {
    // Opaque, one-use secret; display or transfer directly, never log or persist.
    std::string code;
    std::uint64_t expires_at_unix = 0;
};

struct WorkerInfo {
    std::string id;
    std::string label;
    WorkerStatus status = WorkerStatus::Unpaired;
    bool revoked = false;
};

class Leader final {
public:
    static std::optional<Leader> Open(const std::filesystem::path& state_root,
                                      std::string server_name,
                                      std::string* error = nullptr);
    Leader(Leader&&) noexcept;
    Leader& operator=(Leader&&) noexcept;
    ~Leader();
    Leader(const Leader&) = delete;
    Leader& operator=(const Leader&) = delete;

    // The caller owns a distinct enrollment listener; this consumes one accepted
    // enrollment socket, not an operational TlsChannel socket.
    bool HandleEnrollmentConnection(
        TlsChannel::NativeSocket socket, std::string* error = nullptr,
        const std::atomic<bool>* canceled = nullptr);
    // The opaque single-use code is returned to the operator; never log or persist it.
    std::optional<Invitation> IssueInvitation(
        std::chrono::seconds lifetime = std::chrono::minutes(5),
        std::string* error = nullptr);
    std::vector<WorkerInfo> Workers() const;
    bool Approve(const std::string& worker_id, std::string* error = nullptr);
    bool Reject(const std::string& worker_id, std::string* error = nullptr);
    // The coordinator must close that worker's existing channel after revoking.
    bool Revoke(const std::string& worker_id, std::string* error = nullptr);

    // Shared operational-listener options contain only approved, non-revoked pins.
    std::optional<TlsChannel::Options> OptionsForLeader(
        std::string* error = nullptr) const;
    // Resolve the authenticated pin from an established operational channel.
    std::optional<WorkerInfo> WorkerForPeer(
        const TlsChannel::PeerFingerprint& peer_spki) const;
    std::string LeaderId() const;
    std::uint64_t LeaderEpoch() const;
    // Provision independently to workers before issuing invitation codes.
    TlsChannel::PeerFingerprint EnrollmentPeerFingerprint() const;
    // Creates or loads a dedicated P-256 update signer in the protected
    // leader pairing directory; this key is independent of the enrollment CA.
    bool InitializeUpdateSigner(std::string& public_key_pem,
                                std::string& fingerprint_sha256,
                                std::string* error = nullptr);
    // Signs a canonical update-offer payload only for an approved worker.
    bool SignUpdateOffer(const std::string& worker_id,
                         const std::vector<std::uint8_t>& payload,
                         std::vector<std::uint8_t>& signature,
                         std::string& signer_public_key_pem,
                         std::string* error = nullptr) const;



private:
    struct Impl;
    explicit Leader(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// Verifies a detached P-256 signature over SHA-256(message), and returns the
// public key's DER-SPKI SHA-256 fingerprint for comparison with admin policy.
bool VerifyDetachedUpdateSignature(
    const std::string& public_key_pem,
    const std::vector<std::uint8_t>& message,
    const std::vector<std::uint8_t>& signature,
    TlsChannel::PeerFingerprint& fingerprint,
    std::string* error = nullptr);
class Worker final {
public:
    static std::optional<Worker> Open(const std::filesystem::path& state_root,
                                      std::string* error = nullptr);
    // Service-owned Windows pairing storage, accessible only to the local
    // elevated administrator and the installed restricted service account.
    static std::optional<Worker> OpenMachine(const std::filesystem::path& state_root,
                                             std::string* error = nullptr);
    Worker(Worker&&) noexcept;
    Worker& operator=(Worker&&) noexcept;
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    WorkerStatus Status() const noexcept;
    std::string WorkerId() const;
    std::string LeaderId() const;
    bool IsPairedTo(const std::string& worker_id,
                    const std::string& leader_id) const;
    std::uint64_t LeaderEpoch() const noexcept;
    // Removes the local identity; the operator must revoke leader-side trust separately.
    bool Forget(std::string* error = nullptr);

    // Redeem uses a caller-connected enrollment socket with server-authenticated
    // TLS. expected_leader_spki must be independently provisioned and match the
    // invitation; leader_server_name is the operator-configured TLS name.
    bool Redeem(TlsChannel::NativeSocket socket, const std::string& invitation_code,
                const TlsChannel::PeerFingerprint& expected_leader_spki,
                const std::string& leader_server_name, std::string label,
                std::string* error = nullptr);
    // Polls approval on a fresh connected enrollment socket using the same pin.
    bool CheckApproval(TlsChannel::NativeSocket socket,
                       const std::string& invitation_code,
                       const TlsChannel::PeerFingerprint& expected_leader_spki,
                       const std::string& leader_server_name,
                       std::string* error = nullptr);

    // Empty until approval is verified and the signed worker certificate is saved;
    // address and port are operator-configured operational endpoint values.
    std::optional<TlsChannel::ClientOptions> ClientOptionsForLeader(
        std::string address, std::string server_name, std::uint16_t port,
        std::string* error = nullptr) const;

private:
    static std::optional<Worker> OpenWithMode(const std::filesystem::path& state_root,
                                               std::string* error, bool machine);
    struct Impl;
    explicit Worker(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace app::agent::pairing
