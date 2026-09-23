#pragma once

#include "app/AgentPairing.h"
#include "app/AgentWire.h"
#include "config/TrainConfig.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sfm {
struct AutoRequest;
}

namespace app::agent {

class LeaderServer final {
public:
    struct Options {
        std::filesystem::path state_root;
        std::string bind_address;
        std::uint16_t operational_port = 0;
        std::uint16_t enrollment_port = 0;
        std::string server_name;
    };

    struct WorkerSnapshot {
        std::string id;
        std::string label;
        pairing::WorkerStatus pairing_status = pairing::WorkerStatus::Unpaired;
        bool revoked = false;
        bool connected = false;
        bool ready = false;
        wire::Status status;
        std::uint64_t last_seen_unix_ms = 0;
        std::string error;
    };

    enum class CommandState : std::uint8_t {
        Pending,
        Applied,
        Rejected,
        Failed,
        Expired,
        Uncertain,
        Disconnected,
    };

    struct CommandResult {
        std::string worker_id;
        std::string command_id;
        std::uint64_t sequence = 0;
        wire::CommandAction action = wire::CommandAction::Pause;
        std::string target_job_id;
        bool confirmed = false;
        std::uint64_t issued_at_ms = 0;
        std::uint64_t expires_at_ms = 0;
        CommandState state = CommandState::Failed;
        wire::AcknowledgmentOutcome acknowledgment =
            wire::AcknowledgmentOutcome::Failed;
        bool acknowledgment_received = false;
    };
    enum class FeatureJobState : std::uint8_t {
        Staging,
        Queued,
        Offered,
        TransferringInputs,
        Running,
        Unknown,
        ReceivingOutput,
        Succeeded,
        Failed,
        Interrupted,
        Rejected,
        Superseded,
    };

    struct FeatureJobSnapshot {
        std::string job_id;
        std::string attempt_id;
        std::string worker_id;
        std::filesystem::path result_root;
        FeatureJobState state = FeatureJobState::Queued;
        std::string error;
        std::optional<double> progress;
    };

    enum class ReconstructionJobState : std::uint8_t {
        Staging,
        Queued,
        Offered,
        TransferringInputs,
        Running,
        Unknown,
        ReceivingOutput,
        Succeeded,
        Failed,
        Interrupted,
        Rejected,
        Superseded,
    };

    struct ReconstructionJobSnapshot {
        std::string job_id;
        std::string attempt_id;
        std::string worker_id;
        std::string paired_leader_id;
        std::uint64_t paired_leader_epoch = 0;
        std::string required_build;
        std::string source_manifest_sha256;
        std::string input_identity_sha256;
        std::filesystem::path result_root;
        ReconstructionJobState state = ReconstructionJobState::Queued;
        std::string error;
        bool current = true;
    };

    enum class TrainingJobState : std::uint8_t {
        Staging,
        Queued,
        Offered,
        TransferringInputs,
        Running,
        Unknown,
        ReceivingOutput,
        Succeeded,
        Failed,
        Interrupted,
        Rejected,
        Superseded,
    };

    struct TrainingJobSnapshot {
        std::string job_id;
        std::string attempt_id;
        std::string worker_id;
        std::string paired_leader_id;
        std::uint64_t paired_leader_epoch = 0;
        std::string required_build;
        std::string input_identity_sha256;
        std::filesystem::path result_root;
        std::filesystem::path checkpoint_dir;
        TrainConfig resume_config;
        std::string returned_checkpoint;
        TrainingJobState state = TrainingJobState::Queued;
        std::string error;
        bool current = true;
    };

    bool SubmitTraining(const std::string& worker_id,
                        const std::string& job_id,
                        TrainConfig config,
                        const std::string& preset,
                        const std::filesystem::path& resume_checkpoint,
                        std::uint64_t disk_budget_bytes,
                        std::string* error = nullptr);
    bool SupersedeTraining(const std::string& job_id,
                           std::string* error = nullptr);
    std::vector<TrainingJobSnapshot> TrainingJobs() const;

    bool SubmitReconstruction(const std::string& worker_id,
                             const std::string& job_id,
                             sfm::AutoRequest request,
                             const std::filesystem::path& source_manifest,
                             std::uint64_t disk_budget_bytes,
                             std::string* error = nullptr);
    bool SupersedeReconstruction(const std::string& job_id,
                                std::string* error = nullptr);
    std::vector<ReconstructionJobSnapshot> ReconstructionJobs() const;


    static std::unique_ptr<LeaderServer> Start(Options options,
                                               std::string* error = nullptr);
    ~LeaderServer();
    LeaderServer(const LeaderServer&) = delete;
    LeaderServer& operator=(const LeaderServer&) = delete;

    void Stop() noexcept;
    bool Running() const noexcept;
    std::uint16_t OperationalPort() const noexcept;
    std::uint16_t EnrollmentPort() const noexcept;

    std::optional<pairing::Invitation> IssueInvitation(
        std::chrono::seconds lifetime = std::chrono::minutes(5),
        std::string* error = nullptr);
    bool Approve(const std::string& worker_id, std::string* error = nullptr);
    bool Reject(const std::string& worker_id, std::string* error = nullptr);
    bool Revoke(const std::string& worker_id, std::string* error = nullptr);
    TlsChannel::PeerFingerprint EnrollmentPeerFingerprint() const;
    std::string UpdateSignerFingerprint() const;
    std::vector<pairing::WorkerInfo> Workers() const;
    std::vector<WorkerSnapshot> Snapshot() const;

    // Confirmation-required commands need the explicit overload: Stop and
    // force service restart cannot be issued through the compatibility API.
    std::optional<CommandResult> SendCommand(
        const std::string& worker_id, wire::CommandAction action,
        std::chrono::milliseconds ttl = std::chrono::seconds(30),
        std::string* error = nullptr);
    std::optional<CommandResult> SendCommand(
        const std::string& worker_id, wire::CommandAction action,
        const std::string& target_job_id, bool confirmed,
        std::chrono::milliseconds ttl = std::chrono::seconds(30),
        std::string* error = nullptr);
    std::optional<CommandResult> RequestReboot(
        const std::string& worker_id, bool confirmed,
        std::string* error = nullptr);
    std::optional<CommandResult> RequestUpdate(
        const std::string& worker_id,
        const std::filesystem::path& package_file,
        update::PackageManifest manifest, std::uint64_t security_version,
        bool confirmed, std::string* error = nullptr);
    std::vector<CommandResult> CommandSnapshots() const;
    bool SubmitFeatureShard(const std::string& worker_id,
                            const std::string& job_id,
                            const std::filesystem::path& plan_path,
                            const std::filesystem::path& request_path,
                            const std::filesystem::path& image_root,
                            const std::filesystem::path& mask_root,
                            std::uint64_t disk_budget_bytes,
                            std::string* error = nullptr);
    bool SupersedeFeatureShard(const std::string& job_id,
                               std::string* error = nullptr);
    std::vector<FeatureJobSnapshot> FeatureJobs() const;

private:
    struct Impl;
    explicit LeaderServer(std::shared_ptr<Impl> impl) noexcept;
    std::shared_ptr<Impl> impl_;
};

}  // namespace app::agent
