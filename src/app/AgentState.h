#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace app::agent {

enum class AttemptState {
    Accepted,
    InProgress,
    Succeeded,
    Failed,
    Interrupted,
};

enum class CommandOutcome {
    Pending,
    Accepted,
    Applied,
    Rejected,
    Failed,
};

struct AcceptedAssignment {
    AcceptedAssignment(std::string job, std::string attempt,
                       AttemptState initial_state = AttemptState::Accepted)
        : job_id(std::move(job)), attempt_id(std::move(attempt)),
          state(initial_state) {}

    std::string job_id;
    std::string attempt_id;
    AttemptState state;
};

struct CommandRecord {
    CommandRecord(std::string id, std::uint64_t number,
                  std::string fingerprint, CommandOutcome result,
                  std::string action = {}, std::string target = {},
                  bool is_confirmed = false)
        : command_id(std::move(id)), sequence(number),
          command_fingerprint(std::move(fingerprint)), outcome(result),
          command_action(std::move(action)), target_job_id(std::move(target)),
          confirmed(is_confirmed) {}

    std::string command_id;
    std::uint64_t sequence;
    std::string command_fingerprint;
    CommandOutcome outcome;
    std::string command_action;
    std::string target_job_id;
    bool confirmed = false;
};

struct RestartIntent {
    std::string command_id;
    std::uint64_t sequence = 0;
    bool force = false;
};

struct State {
    std::string command_leader_id;
    std::uint64_t command_leader_epoch = 0;
    std::uint64_t last_command_sequence = 0;
    bool maintenance = false;
    bool paused = false;  // Durable pause request; maintenance remains independent.
    std::vector<AcceptedAssignment> accepted;
    std::deque<CommandRecord> recent_commands;
    std::optional<RestartIntent> restart_intent;
};

// State is deliberately small and service-owned; these bounds also cap parser
// work and the amount of data written to disk.
inline constexpr std::size_t kMaxAcceptedAttempts = 128;
inline constexpr std::size_t kMaxRecentCommands = 256;
inline constexpr std::size_t kMaxStateIdentifierBytes = 128;

// The root must be an existing absolute directory. A missing state file is a
// fresh online state; malformed state and all I/O errors throw.
State load_state(const std::filesystem::path& state_root);
void save_state(const std::filesystem::path& state_root, const State& state);
// Elevated administrator access to the Windows service-owned state file.
// Ordinary load_state can recover an interrupted attempt by writing a file.
State load_machine_state(const std::filesystem::path& state_root);
void save_machine_state(const std::filesystem::path& state_root, const State& state);

// Interrupted can be produced by force-stop or recovery from a live process.
void accept_assignment(State& state, std::string job_id, std::string attempt_id);
void transition_assignment(State& state, const std::string& attempt_id,
                           AttemptState next);
// Only call after a separately authenticated, persisted re-pair.
void rebind_command_leader(State& state, std::string leader_id,
                           std::uint64_t leader_epoch);

// Persist Pending before any command side effect. A replay of an evicted
// sequence is rejected by the durable watermark, never re-executed.
void begin_command(State& state, std::string leader_id,
                   std::uint64_t leader_epoch, std::uint64_t sequence,
                   std::string command_id, std::string command_fingerprint,
                   std::string action = {}, std::string target_job_id = {},
                   bool confirmed = false);
void finish_command(State& state, std::uint64_t sequence,
                    CommandOutcome outcome);
const CommandRecord* find_command(const State& state,
                                  std::uint64_t sequence) noexcept;
// This intent follows a durably quiesced restart request across process exit.
// The next verified service host settles its Accepted command as Applied.
bool set_restart_intent(State& state, std::uint64_t sequence, bool force);
void complete_restart_intent(State& state, std::uint64_t sequence);

}  // namespace app::agent
