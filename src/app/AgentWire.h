#pragma once

#include "app/AgentAdminIntent.h"
#include "app/AgentTransfer.h"
#include "app/AgentUpdatePackage.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace app::agent::wire {

inline constexpr std::uint32_t kSchemaVersion = 5;
inline constexpr std::size_t kMaxMessageBytes = 1024 * 1024;
inline constexpr std::size_t kMaxCommandIdBytes = 64;
inline constexpr std::size_t kMaxLeaderIdBytes = 64;
inline constexpr std::size_t kMaxBuildBytes = 128;
inline constexpr std::size_t kMaxPlatformBytes = 64;
inline constexpr std::size_t kMaxGpuBytes = 128;
inline constexpr std::size_t kMaxTargetJobIdBytes = 128;
inline constexpr std::size_t kMaxFeatureIdBytes = 128;
inline constexpr std::size_t kMaxManifestEntries = 4096;
inline constexpr std::size_t kMaxManifestPathBytes = 4096;
inline constexpr std::size_t kMaxPortableTextBytes = 4096;
inline constexpr std::size_t kMaxCheckpointBasenameBytes = 30;
inline constexpr std::size_t kMaxAdminPublicKeyPemBytes = 16 * 1024;
inline constexpr std::size_t kMaxAdminSignatureBytes = 256;
inline constexpr std::uint64_t kMaxRestartCommandLifetimeMs =
    5ULL * 60 * 1000;
inline constexpr std::uint64_t kMaxPortableOfferLifetimeMs =
    24ULL * 60 * 60 * 1000;
enum class Error : std::uint8_t {
    None,
    MessageTooLarge,
    MalformedJson,
    InvalidUtf8,
    DuplicateField,
    UnknownField,
    UnsupportedVersion,
    InvalidField,
    NumericOverflow,
    InvalidCombination,
    ExpiredCommand,
    InvalidTime,
    ExpiredFeatureOffer,
    ExpiredPortableOffer,
};
enum class ConnectionState : std::uint8_t { Connected, Reconnecting, Disconnected };
enum class CompatibilityState : std::uint8_t { Unknown, Compatible, Incompatible };
enum class SchedulingState : std::uint8_t { Accepting, Pausing, Paused, Stopped };
enum class ActivityState : std::uint8_t { Idle, Feature, Reconstruction, Training };
enum class HealthState : std::uint8_t { Healthy, Degraded, Unhealthy };
enum class Capability : std::uint8_t { Feature, Reconstruction, Training };
enum class CommandAction : std::uint8_t {
    Pause, Resume, Stop, Maintenance, Online, RestartService, ForceRestartService,
    RebootMachine, ActivateUpdate
};
enum class AcknowledgmentOutcome : std::uint8_t { Accepted, Rejected, Completed, Failed };

struct Status {
    ConnectionState connection = ConnectionState::Connected;
    CompatibilityState compatibility = CompatibilityState::Unknown;
    SchedulingState scheduling = SchedulingState::Accepting;
    ActivityState activity = ActivityState::Idle;
    HealthState health = HealthState::Healthy;
    std::vector<Capability> capabilities;
    std::string build;
    std::string platform;
    std::string gpu;
    bool maintenance = false;
    bool online = true;
};

struct Command {
    std::string command_id;
    std::string leader_id;
    std::uint64_t leader_epoch = 0;
    std::uint64_t sequence = 0;
    CommandAction action = CommandAction::Pause;
    std::string target_job_id;  // Empty except for an optionally targeted Stop.
    bool confirmed = false;
    std::uint64_t issued_at_ms = 0;
    std::uint64_t expires_at_ms = 0;
    std::optional<admin::Intent> admin_intent;
    std::optional<update::PackageOffer> package_offer;
};

struct Acknowledgment {
    std::string command_id;
    std::string leader_id;
    std::uint64_t leader_epoch = 0;
    std::uint64_t sequence = 0;
    CommandAction action = CommandAction::Pause;
    AcknowledgmentOutcome outcome = AcknowledgmentOutcome::Failed;
};

struct FeatureOffer {
    std::string job_id;
    std::string attempt_id;
    std::string plan_digest;
    std::string request_digest;
    std::string required_build;
    std::uint64_t expires_at_ms = 0;
    std::vector<app::agent::TransferFile> inputs;
};

struct FeatureDecision {
    enum class Step : std::uint8_t { Offer, Inputs, Output };
    enum class Decision : std::uint8_t { Accepted, Rejected, Committed };

    std::string job_id;
    std::string attempt_id;
    Step step = Step::Offer;
    Decision decision = Decision::Accepted;
    std::string reason;
};

struct FeatureResult {
    enum class Outcome : std::uint8_t { Succeeded, Failed, Interrupted };

    std::string job_id;
    std::string attempt_id;
    Outcome outcome = Outcome::Succeeded;
    std::vector<app::agent::TransferFile> outputs;
    std::string error;
};

enum class PortableWorkload : std::uint8_t { Reconstruction, Training };

struct PortableOffer {
    PortableWorkload workload = PortableWorkload::Reconstruction;
    std::string job_id;
    std::string attempt_id;
    std::string input_identity_sha256;
    std::string required_build;
    std::uint64_t expires_at_ms = 0;
    std::vector<app::agent::TransferFile> inputs;
};

struct PortableDecision {
    enum class Step : std::uint8_t { Offer, Inputs, Output };
    enum class Decision : std::uint8_t { Accepted, Rejected, Committed };

    PortableWorkload workload = PortableWorkload::Reconstruction;
    std::string job_id;
    std::string attempt_id;
    Step step = Step::Offer;
    Decision decision = Decision::Accepted;
    std::string reason;
};

struct PortableResult {
    enum class Outcome : std::uint8_t { Succeeded, Failed, Interrupted };

    PortableWorkload workload = PortableWorkload::Reconstruction;
    std::string job_id;
    std::string attempt_id;
    Outcome outcome = Outcome::Succeeded;
    std::vector<app::agent::TransferFile> outputs;
    std::string output_metadata;
    std::string error;
};

using Payload = std::variant<Status, Command, Acknowledgment, FeatureOffer,
                             FeatureDecision, FeatureResult, PortableOffer,
                             PortableDecision, PortableResult>;


struct Message {
    std::uint32_t schema_version = kSchemaVersion;
    Payload payload;
};

bool IsValid(ConnectionState value) noexcept;
bool IsValid(CompatibilityState value) noexcept;
bool IsValid(SchedulingState value) noexcept;
bool IsValid(ActivityState value) noexcept;
bool IsValid(HealthState value) noexcept;
bool IsValid(Capability value) noexcept;
bool IsValid(CommandAction value) noexcept;
bool IsValid(AcknowledgmentOutcome value) noexcept;

bool IsValid(PortableWorkload value) noexcept;
Error Validate(const Message& message, std::uint64_t now_unix_ms) noexcept;
Error Encode(const Message& message, std::uint64_t now_unix_ms,
             std::string& json);
Error Decode(const std::string& json, std::uint64_t now_unix_ms,
             Message& message);

}  // namespace app::agent::wire
