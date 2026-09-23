#include "app/AgentState.h"
#include "data/Json.h"

#include <exception>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
namespace agent = app::agent;

namespace {

template <typename F>
bool throws(F&& action) {
    try {
        action();
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

std::string read_text(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

}  // namespace

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", message);
        if (!ok) ++failures;
    };

    fs::path root = fs::temp_directory_path() /
        ("spirula-agent-state-" + std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        fs::create_directories(root);
        root = fs::canonical(root);
        const fs::path missing_root = root / "missing";
        check(throws([&] { (void)agent::load_state(missing_root); }) &&
                  !fs::exists(missing_root),
              "loading never creates an implicit state directory");
        const agent::State fresh = agent::load_state(root);
        check(!fresh.maintenance && !fresh.paused && fresh.accepted.empty(),
              "fresh explicit root starts online with no accepted assignments");

        agent::State state;
        const std::string command_fingerprint(64, 'a');
        state.maintenance = true;
        state.paused = true;
        agent::accept_assignment(state, "job-1", "att-1");
        check(throws([&] {
                  agent::transition_assignment(state, "att-1",
                                               agent::AttemptState::Succeeded);
              }), "accepted assignment cannot skip the in-progress transition");
        agent::transition_assignment(state, "att-1", agent::AttemptState::InProgress);
        agent::accept_assignment(state, "job-1", "att-2");
        agent::transition_assignment(state, "att-2", agent::AttemptState::InProgress);
        agent::transition_assignment(state, "att-2", agent::AttemptState::Succeeded);

        agent::begin_command(state, "leader-1", 1, 1, "cmd-1",
                             command_fingerprint);
        check(agent::find_command(state, 1) &&
                  agent::find_command(state, 1)->outcome ==
                      agent::CommandOutcome::Pending,
              "command is pending before any side effect");
        agent::finish_command(state, 1, agent::CommandOutcome::Applied);
        check(throws([&] {
                  agent::begin_command(state, "leader-1", 1, 1, "cmd-1",
                                       command_fingerprint);
              }), "retrying an acknowledged sequence cannot re-execute it");
        check(throws([&] {
                  agent::begin_command(state, "other-leader", 1, 2, "cmd-2",
                                       command_fingerprint);
              }), "another leader cannot reuse the paired command history");
        check(throws([&] {
                  agent::accept_assignment(state, "job-1", "att-1");
              }), "attempt IDs cannot be accepted twice");

        agent::save_state(root, state);
        agent::State recovered = agent::load_state(root);
        check(recovered.maintenance && recovered.paused,
              "recovery preserves independent maintenance and pause state");
        check(recovered.accepted.size() == 2 &&
                  recovered.accepted[0].state == agent::AttemptState::Interrupted &&
                  recovered.accepted[1].state == agent::AttemptState::Succeeded,
              "recovery interrupts only in-progress attempts and preserves terminal attempts");
        agent::State force_interrupted;
        agent::accept_assignment(force_interrupted, "job-force", "attempt-force");
        agent::transition_assignment(force_interrupted, "attempt-force",
                                     agent::AttemptState::InProgress);
        agent::transition_assignment(force_interrupted, "attempt-force",
                                     agent::AttemptState::Interrupted);
        check(force_interrupted.accepted.front().state ==
                  agent::AttemptState::Interrupted,
              "live force interruption preserves a terminal interrupted attempt");
        check(recovered.last_command_sequence == 1 &&
                  agent::find_command(recovered, 1) &&
                  agent::find_command(recovered, 1)->outcome ==
                      agent::CommandOutcome::Applied &&
                  agent::find_command(recovered, 1)->command_fingerprint ==
                      command_fingerprint,
              "command watermark and outcome survive recovery");
        agent::begin_command(recovered, "leader-1", 1, 2, "cmd-2",
                             command_fingerprint);
        agent::save_state(root, recovered);
        agent::State pending = agent::load_state(root);
        check(agent::find_command(pending, 2) &&
                  agent::find_command(pending, 2)->outcome ==
                      agent::CommandOutcome::Pending &&
                  throws([&] {
                      agent::begin_command(pending, "leader-1", 1, 2, "cmd-2",
                                           command_fingerprint);
                  }), "crash after intent persistence cannot repeat a command");
        agent::finish_command(pending, 2, agent::CommandOutcome::Rejected);
        agent::begin_command(pending, "leader-1", 1, 3, "cmd-3",
                             command_fingerprint, "stop", "job-9", true);
        agent::finish_command(pending, 3, agent::CommandOutcome::Accepted);
        agent::save_state(root, pending);
        agent::State accepted = agent::load_state(root);
        check(agent::find_command(accepted, 3) &&
                  agent::find_command(accepted, 3)->outcome ==
                      agent::CommandOutcome::Accepted &&
                  agent::find_command(accepted, 3)->command_action == "stop" &&
                  agent::find_command(accepted, 3)->target_job_id == "job-9" &&
                  agent::find_command(accepted, 3)->confirmed,
              "accepted targeted Stop outcome and binding survive recovery");
        for (std::uint64_t sequence = 4;
             sequence <= agent::kMaxRecentCommands + 3; ++sequence) {
            agent::begin_command(accepted, "leader-1", 1, sequence,
                                 "cmd-" + std::to_string(sequence),
                                 command_fingerprint);
            agent::finish_command(accepted, sequence,
                                  agent::CommandOutcome::Rejected);
        }
        check(agent::find_command(accepted, 3) &&
                  agent::find_command(accepted, 3)->outcome ==
                      agent::CommandOutcome::Accepted,
              "history eviction preserves asynchronous command outcomes");
        agent::finish_command(accepted, 3, agent::CommandOutcome::Applied);
        const std::uint64_t restart_sequence =
            accepted.last_command_sequence + 1;
        agent::begin_command(accepted, "leader-1", 1, restart_sequence,
                             "restart-safe", command_fingerprint,
                             "restart_service");
        agent::finish_command(accepted, restart_sequence,
                              agent::CommandOutcome::Accepted);
        check(agent::set_restart_intent(accepted, restart_sequence, false) &&
                  !agent::set_restart_intent(accepted, restart_sequence, false),
              "duplicate safe restart intent is idempotent");
        agent::save_state(root, accepted);
        accepted = agent::load_state(root);
        check(accepted.restart_intent &&
                  accepted.restart_intent->command_id == "restart-safe" &&
                  accepted.restart_intent->sequence == restart_sequence &&
                  !accepted.restart_intent->force &&
                  agent::find_command(accepted, restart_sequence)->outcome ==
                      agent::CommandOutcome::Accepted,
              "pending restart intent and Accepted outcome survive recovery");
        agent::complete_restart_intent(accepted, restart_sequence);
        agent::save_state(root, accepted);
        accepted = agent::load_state(root);
        check(!accepted.restart_intent &&
                  agent::find_command(accepted, restart_sequence)->outcome ==
                      agent::CommandOutcome::Applied,
              "only a subsequent host startup settles restart as Applied");

        const std::uint64_t force_sequence = accepted.last_command_sequence + 1;
        check(throws([&] {
                  agent::begin_command(
                      accepted, "leader-1", 1, force_sequence, "restart-force",
                      command_fingerprint, "force_restart_service", "", false);
              }) && accepted.last_command_sequence + 1 == force_sequence,
              "force restart cannot enter the journal without confirmation");
        agent::begin_command(accepted, "leader-1", 1, force_sequence,
                             "restart-force", command_fingerprint,
                             "force_restart_service", "", true);
        agent::finish_command(accepted, force_sequence,
                              agent::CommandOutcome::Accepted);
        check(agent::set_restart_intent(accepted, force_sequence, true),
              "confirmed force restart creates a durable intent");
        agent::save_state(root, accepted);
        accepted = agent::load_state(root);
        check(accepted.restart_intent && accepted.restart_intent->force,
              "force confirmation remains bound to its recovered intent");
        agent::complete_restart_intent(accepted, force_sequence);
        check(agent::find_command(accepted, force_sequence)->outcome ==
                  agent::CommandOutcome::Applied,
              "confirmed force restart settles after host recovery");
        check(!agent::find_command(accepted, 1) &&
                  throws([&] {
                      agent::begin_command(accepted, "leader-1", 1, 1, "cmd-1",
                                           command_fingerprint);
                  }), "evicted command IDs remain rejected by the watermark");
        agent::rebind_command_leader(accepted, "leader-2", 2);
        check(accepted.maintenance && accepted.paused &&
                  accepted.recent_commands.empty() &&
                  accepted.last_command_sequence == 0,
              "explicit re-pair preserves scheduling while resetting replay scope");

        const fs::path state_file = root / "agent-state.json";
        {
            std::ofstream output(state_file, std::ios::binary | std::ios::trunc);
            output << "{\"schema_version\":3,\"command_leader_id\":\"leader-1\","
                      "\"command_leader_epoch\":1,\"last_command_sequence\":1,"
                      "\"maintenance\":false,\"paused\":false,\"accepted\":[],"
                      "\"recent_commands\":[{\"command_id\":\"legacy-command\","
                      "\"sequence\":1,\"command_fingerprint\":\""
                   << std::string(64, 'a')
                   << "\",\"outcome\":\"applied\"}]}";
        }
        agent::State migrated = agent::load_state(root);
        check(agent::find_command(migrated, 1) &&
                  agent::find_command(migrated, 1)->command_action.empty(),
              "schema v3 replay records migrate without invented command scope");
        agent::save_state(root, migrated);
        const JsonValue persisted = json_parse(read_text(state_file));
        const JsonValue* schema = persisted.find("schema_version");
        check(schema && schema->type == JsonValue::Type::Number &&
                  schema->num == 5,
              "migrated state persists in the current schema");
        {
            std::ofstream output(state_file, std::ios::binary | std::ios::trunc);
            output << "{\"schema_version\":1,";
        }
        check(throws([&] { (void)agent::load_state(root); }) &&
                  read_text(state_file) == "{\"schema_version\":1,",
              "truncated state fails closed without resetting the persisted file");
    } catch (const std::exception& error) {
        std::printf("FAIL unexpected exception: %s\n", error.what());
        ++failures;
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    return failures ? 1 : 0;
}
