#include "app/AgentClient.h"
#include "app/AgentAdminBroker.h"
#include "app/AgentServiceManager.h"
#include "app/AgentFeatureWorker.h"
#include "app/AppPaths.h"

#include "app/AgentPairing.h"
#include "app/AgentTls.h"
#include "app/AgentWire.h"
#include "core/FilesystemPath.h"
#include "core/Sha256.h"
#include "data/Json.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace app::agent {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr auto kHeartbeat = 5s;
constexpr auto kPairingRefresh = 5s;
constexpr auto kInitialBackoff = 250ms;
constexpr auto kMaximumBackoff = 30s;
constexpr std::uint32_t kConnectTimeoutMs = 5'000;
constexpr std::uint32_t kHandshakeTimeoutMs = 5'000;
constexpr std::uint32_t kIoTimeoutMs = 1'000;

std::uint64_t unix_millis() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return milliseconds < 0 ? 0 : static_cast<std::uint64_t>(milliseconds);
}

bool stopping(StopRequested stop_requested, void* context) noexcept {
    return stop_requested && stop_requested(context);
}
bool managed_restart_host(bool machine_managed) {
#ifdef _WIN32
    return machine_managed;
#else
    (void)machine_managed;
    try {
        return service::is_managed_service_process();
    } catch (...) {
        return false;
    }
#endif
}

bool wait_until(StopRequested stop_requested, void* context,
                Clock::time_point deadline) {
    while (!stopping(stop_requested, context)) {
        const auto now = Clock::now();
        if (now >= deadline) return true;
        std::this_thread::sleep_for(
            std::min(std::chrono::duration_cast<Clock::duration>(100ms),
                     deadline - now));
    }
    return false;
}

struct PairingIdentity {
    pairing::WorkerStatus status = pairing::WorkerStatus::Unpaired;
    std::string worker_id;
    std::string leader_id;
    std::uint64_t leader_epoch = 0;
};

struct PairingSnapshot {
    PairingIdentity identity;
    std::optional<TlsChannel::ClientOptions> options;
};

pairing::Worker open_worker(const std::filesystem::path& state_root,
                            bool machine_managed) {
    std::string error;
    std::optional<pairing::Worker> worker;
#ifdef _WIN32
    worker = machine_managed
                 ? pairing::Worker::OpenMachine(state_root, &error)
                 : pairing::Worker::Open(state_root, &error);
#else
    (void)machine_managed;
    worker = pairing::Worker::Open(state_root, &error);
#endif
    if (!worker)
        throw std::runtime_error("worker pairing unavailable: " + error);
    return std::move(*worker);
}

PairingIdentity identity_of(const pairing::Worker& worker) {
    return {worker.Status(), worker.WorkerId(), worker.LeaderId(),
            worker.LeaderEpoch()};
}

PairingIdentity read_pairing_identity(
    const std::filesystem::path& state_root, bool machine_managed) {
    const pairing::Worker worker = open_worker(state_root, machine_managed);
    return identity_of(worker);
}

PairingSnapshot read_pairing_snapshot(
    const std::filesystem::path& state_root, const Config& config,
    bool machine_managed) {
    const pairing::Worker worker = open_worker(state_root, machine_managed);
    PairingSnapshot snapshot;
    snapshot.identity = identity_of(worker);
    if (snapshot.identity.status == pairing::WorkerStatus::Paired) {
        std::string error;
        snapshot.options = worker.ClientOptionsForLeader(
            config.leader_address, config.leader_server_name,
            config.leader_port, &error);
        if (!snapshot.options)
            throw std::runtime_error("paired worker credentials unavailable: " + error);
    }
    return snapshot;
}

bool same_pairing(const PairingIdentity& expected,
                  const PairingIdentity& current) {
    return expected.status == pairing::WorkerStatus::Paired &&
           current.status == pairing::WorkerStatus::Paired &&
           expected.worker_id == current.worker_id &&
           expected.leader_id == current.leader_id &&
           expected.leader_epoch == current.leader_epoch;
}

void increase_backoff(std::chrono::milliseconds& backoff) noexcept {
    backoff = std::min(backoff * 2,
                       std::chrono::duration_cast<std::chrono::milliseconds>(
                           kMaximumBackoff));
}

bool administrative_action(wire::CommandAction action) noexcept {
    return action == wire::CommandAction::RebootMachine ||
           action == wire::CommandAction::ActivateUpdate;
}

bool administrative_action(const std::string& action) noexcept {
    return action == "reboot_machine" || action == "activate_update";
}

bool allowed_action(wire::CommandAction action) noexcept {
    switch (action) {
        case wire::CommandAction::Maintenance:
        case wire::CommandAction::Online:
        case wire::CommandAction::Pause:
        case wire::CommandAction::Resume:
        case wire::CommandAction::Stop:
        case wire::CommandAction::RestartService:
        case wire::CommandAction::ForceRestartService:
        case wire::CommandAction::RebootMachine:
        case wire::CommandAction::ActivateUpdate:
            return true;
    }
    return false;
}

const char* action_name(wire::CommandAction action) noexcept {
    switch (action) {
        case wire::CommandAction::Maintenance: return "maintenance";
        case wire::CommandAction::Online: return "online";
        case wire::CommandAction::Pause: return "pause";
        case wire::CommandAction::Resume: return "resume";
        case wire::CommandAction::Stop: return "stop";
        case wire::CommandAction::RestartService: return "restart_service";
        case wire::CommandAction::ForceRestartService:
            return "force_restart_service";
        case wire::CommandAction::RebootMachine: return "reboot_machine";
        case wire::CommandAction::ActivateUpdate: return "activate_update";
    }
    return "";
}

wire::CommandAction action_from_name(const std::string& action) {
    if (action == "maintenance") return wire::CommandAction::Maintenance;
    if (action == "online") return wire::CommandAction::Online;
    if (action == "pause") return wire::CommandAction::Pause;
    if (action == "resume") return wire::CommandAction::Resume;
    if (action == "stop") return wire::CommandAction::Stop;
    if (action == "restart_service")
        return wire::CommandAction::RestartService;
    if (action == "force_restart_service")
        return wire::CommandAction::ForceRestartService;
    if (action == "reboot_machine") return wire::CommandAction::RebootMachine;
    if (action == "activate_update") return wire::CommandAction::ActivateUpdate;
    throw std::runtime_error("persisted command action is unsupported");
}

void recover_pending_commands(const std::filesystem::path& state_root,
                              State& state) {
    std::vector<std::uint64_t> pending;
    for (const CommandRecord& record : state.recent_commands)
        if (record.outcome == CommandOutcome::Pending &&
            !administrative_action(record.command_action))
            pending.push_back(record.sequence);
    if (pending.empty()) return;

    State recovered = state;
    for (std::uint64_t sequence : pending)
        finish_command(recovered, sequence, CommandOutcome::Failed);
    save_state(state_root, recovered);
    state = std::move(recovered);
}

void bind_command_leader(const std::filesystem::path& state_root,
                         State& state, const PairingIdentity& identity) {
    if (state.command_leader_id.empty() ||
        (state.command_leader_id == identity.leader_id &&
         state.command_leader_epoch == identity.leader_epoch))
        return;
    State rebound = state;
    rebind_command_leader(rebound, identity.leader_id, identity.leader_epoch);
    save_state(state_root, rebound);
    state = std::move(rebound);
}

wire::AcknowledgmentOutcome acknowledgment_outcome(
    CommandOutcome outcome) noexcept {
    switch (outcome) {
        case CommandOutcome::Accepted:
            return wire::AcknowledgmentOutcome::Accepted;
        case CommandOutcome::Applied:
            return wire::AcknowledgmentOutcome::Completed;
        case CommandOutcome::Rejected:
            return wire::AcknowledgmentOutcome::Rejected;
        case CommandOutcome::Failed:
        case CommandOutcome::Pending:
            return wire::AcknowledgmentOutcome::Failed;
    }
    return wire::AcknowledgmentOutcome::Failed;
}

void apply_requested_state(State& state, wire::CommandAction action) {
    switch (action) {
        case wire::CommandAction::Maintenance: state.maintenance = true; break;
        case wire::CommandAction::Online: state.maintenance = false; break;
        case wire::CommandAction::Pause: state.paused = true; break;
        case wire::CommandAction::Resume: state.paused = false; break;
        case wire::CommandAction::Stop:
        case wire::CommandAction::RestartService:
        case wire::CommandAction::ForceRestartService:
        case wire::CommandAction::RebootMachine:
        case wire::CommandAction::ActivateUpdate:
            break;
    }
}

CommandOutcome apply_action(const wire::Command& command, const State& state,
                            FeatureWorker& feature) {
    switch (command.action) {
        case wire::CommandAction::Maintenance:
        case wire::CommandAction::Online:
            return feature.sync_controls(state) ? CommandOutcome::Applied
                                                : CommandOutcome::Failed;
        case wire::CommandAction::Pause:
            if (!feature.sync_controls(state)) return CommandOutcome::Failed;
            return feature.pause_acknowledged() ? CommandOutcome::Applied
                                                : CommandOutcome::Accepted;
        case wire::CommandAction::Resume:
            if (!feature.sync_controls(state)) return CommandOutcome::Failed;
            return feature.resume_acknowledged() ? CommandOutcome::Applied
                                                 : CommandOutcome::Accepted;
        case wire::CommandAction::Stop:
            return command.confirmed
                       ? feature.stop_jobs(command.target_job_id)
                       : CommandOutcome::Rejected;
        case wire::CommandAction::RestartService:
        case wire::CommandAction::ForceRestartService:
        case wire::CommandAction::RebootMachine:
        case wire::CommandAction::ActivateUpdate:
            return CommandOutcome::Rejected;
    }
    return CommandOutcome::Rejected;
}

std::string command_fingerprint(const wire::Command& command) {
    std::string encoded;
    wire::Message message;
    message.payload = command;
    if (wire::Encode(message, command.issued_at_ms, encoded) != wire::Error::None)
        throw std::runtime_error("cannot fingerprint invalid agent command");
    spirula::Sha256 hash;
    hash.update(reinterpret_cast<const std::uint8_t*>(encoded.data()),
                encoded.size());
    return hash.hex();
}

std::uint64_t unix_seconds() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(now).count();
    return seconds < 0 ? 0 : static_cast<std::uint64_t>(seconds);
}

struct AdminCommandResult {
    CommandOutcome outcome = CommandOutcome::Failed;
    bool close_channel = false;
};

bool admin_bindings_match(const wire::Command& command,
                          const PairingIdentity& identity) noexcept {
    if (!command.admin_intent || !command.confirmed ||
        !command.target_job_id.empty() ||
        command.leader_id != identity.leader_id ||
        command.leader_epoch != identity.leader_epoch)
        return false;
    const admin::Intent& intent = *command.admin_intent;
    const bool update =
        command.action == wire::CommandAction::ActivateUpdate;
    if (intent.operation != (update ? admin::Operation::Activate
                                    : admin::Operation::Reboot) ||
        intent.worker_id != identity.worker_id ||
        intent.leader_id != identity.leader_id ||
        intent.leader_epoch != identity.leader_epoch ||
        intent.intent_id != command.command_id ||
        intent.expires_at_unix >
            std::numeric_limits<std::uint64_t>::max() / 1000 ||
        command.expires_at_ms > intent.expires_at_unix * 1000)
        return false;
    if (!update) return !command.package_offer;
    if (!command.package_offer) return false;
    const update::PackageOffer& offer = *command.package_offer;
    return offer.worker_id == identity.worker_id &&
           offer.leader_id == identity.leader_id &&
           offer.update_id == intent.update_id &&
           offer.update_id == command.command_id &&
           offer.manifest.sha256 == intent.package_sha256 &&
           offer.expires_at_unix <=
               std::numeric_limits<std::uint64_t>::max() / 1000 &&
           command.expires_at_ms <= offer.expires_at_unix * 1000;
}

bool authorize_admin_command(
    const Config& policy, const pairing::Worker& paired_worker,
    const wire::Command& command, const State& state,
    std::optional<update::AuthorizedPackageOffer>* authorized,
    std::string& error) {
    if (!command.admin_intent ||
        !admin::VerifyIntent(policy, *command.admin_intent, unix_seconds(),
                             &error))
        return false;
    if (command.action != wire::CommandAction::ActivateUpdate) return true;

    std::vector<std::string> consumed;
    for (const CommandRecord& record : state.recent_commands)
        if (record.command_action == "activate_update" &&
            record.command_id != command.command_id)
            consumed.push_back(record.command_id);
    const auto offer = update::AuthorizePackageOffer(
        policy, paired_worker, *command.package_offer, consumed, &error);
    if (!offer) return false;
    if (authorized) *authorized = *offer;
    return true;
}

bool current_worker_matches(const pairing::Worker& worker,
                            const PairingIdentity& identity) {
    return same_pairing(identity, identity_of(worker)) &&
           worker.IsPairedTo(identity.worker_id, identity.leader_id);
}

bool has_outstanding_admin_command(const State& state,
                                  std::uint64_t except_sequence = 0) {
    return std::any_of(
        state.recent_commands.begin(), state.recent_commands.end(),
        [&](const CommandRecord& record) {
            return record.sequence != except_sequence &&
                   administrative_action(record.command_action) &&
                   (record.outcome == CommandOutcome::Pending ||
                    record.outcome == CommandOutcome::Accepted);
        });
}

CommandOutcome persist_admin_outcome(
    const std::filesystem::path& state_root, State& state,
    FeatureWorker& feature, std::uint64_t sequence, CommandOutcome outcome) {
    const CommandRecord* previous = find_command(state, sequence);
    if (previous && previous->outcome == outcome) return outcome;
    State updated = state;
    finish_command(updated, sequence, outcome);
    save_state(state_root, updated);
    state = std::move(updated);
    (void)feature.sync_controls(state);
    return outcome;
}

std::optional<CommandOutcome> persist_broker_outcome(
    const std::filesystem::path& state_root, State& state,
    FeatureWorker& feature, std::uint64_t sequence,
    const admin::IntentOutcome& outcome) {
    const CommandRecord* record = find_command(state, sequence);
    if (!record) return std::nullopt;
    if (outcome.state == admin::IntentOutcomeState::Unknown) {
        if (record->outcome == CommandOutcome::Accepted ||
            record->outcome == CommandOutcome::Applied)
            return persist_admin_outcome(state_root, state, feature, sequence,
                                         CommandOutcome::Failed);
        return std::nullopt;
    }

    CommandOutcome mapped = CommandOutcome::Failed;
    switch (outcome.state) {
        case admin::IntentOutcomeState::Unknown:
            return std::nullopt;
        case admin::IntentOutcomeState::Pending:
            mapped = CommandOutcome::Accepted;
            break;
        case admin::IntentOutcomeState::Committed:
            mapped = CommandOutcome::Applied;
            break;
        case admin::IntentOutcomeState::RolledBack:
        case admin::IntentOutcomeState::Failed:
            mapped = CommandOutcome::Failed;
            break;
    }
    if (record->outcome == CommandOutcome::Rejected ||
        record->outcome == CommandOutcome::Failed)
        return record->outcome;
    if (record->outcome == CommandOutcome::Applied &&
        mapped != CommandOutcome::Applied)
        mapped = CommandOutcome::Failed;
    return persist_admin_outcome(state_root, state, feature, sequence, mapped);
}

bool query_admin_outcome(const std::string& intent_id,
                         admin::IntentOutcome& outcome) {
    const service::Result result =
        admin::query_intent_outcome(intent_id, outcome);
    return result.success;
}

TransferResult receive_update_package(
    TlsChannel& channel, const std::filesystem::path& storage_root,
    const Config& policy, const wire::Command& command,
    bool previously_authorized) {
    const update::PackageManifest& package =
        command.package_offer->manifest;
    std::uint64_t budget = policy.disk_budget_bytes;
    if (previously_authorized)
        budget = std::max(budget, package.size + 64 * 1024);
    if (!budget || package.size > budget)
        return {TransferError::QuotaExceeded,
                "update package exceeds the worker storage budget"};
    const std::vector<TransferFile> manifest{
        {"package.bin", package.size, package.sha256}};
    return ReceiveArtifacts(
        channel,
        spirula::NativeFilesystemPath(
            storage_root / "agent-update-incoming" / command.command_id),
        manifest, budget);
}

void log_unsupported_admin_operation() {
    std::fprintf(
        stderr,
        "agent administrative commands are supported only by the installed "
        "Windows worker service\n");
}

AdminCommandResult process_admin_command(
    const std::filesystem::path& state_root,
    const std::filesystem::path& storage_root, const Config& policy,
    State& state, const wire::Command& command, FeatureWorker& feature,
    bool machine_managed, const PairingIdentity& identity,
    bool command_expired, TlsChannel& channel) {
    const bool activation =
        command.action == wire::CommandAction::ActivateUpdate;
    const auto reject = [&](CommandOutcome outcome = CommandOutcome::Rejected) {
        return AdminCommandResult{outcome, activation};
    };
    const std::string fingerprint = command_fingerprint(command);
    const CommandRecord* previous = find_command(state, command.sequence);
    const bool existing = previous != nullptr;
    if (existing) {
        if (previous->command_id != command.command_id ||
            previous->command_fingerprint != fingerprint ||
            previous->command_action != action_name(command.action))
            return reject();
    } else {
        if (command.sequence <= state.last_command_sequence ||
            has_outstanding_admin_command(state) ||
            std::any_of(state.recent_commands.begin(),
                        state.recent_commands.end(),
                        [&](const CommandRecord& record) {
                            return record.command_id == command.command_id ||
                                   record.outcome == CommandOutcome::Accepted;
                        }))
            return reject();
    }
    if (!admin_bindings_match(command, identity)) return reject();

#ifndef _WIN32
    (void)state_root;
    (void)storage_root;
    (void)policy;
    (void)feature;
    (void)machine_managed;
    log_unsupported_admin_operation();
    return reject();
#else
    if (!machine_managed || !managed_restart_host(machine_managed)) {
        log_unsupported_admin_operation();
        return reject();
    }

    {
        const pairing::Worker paired = open_worker(state_root, machine_managed);
        if (!current_worker_matches(paired, identity)) return reject();
    }

    if (!existing) {
        if (command_expired || command.expires_at_ms <= unix_millis())
            return reject();
        std::string error;
        const pairing::Worker paired = open_worker(state_root, machine_managed);
        if (!current_worker_matches(paired, identity) ||
            !authorize_admin_command(policy, paired, command, state, nullptr,
                                     error))
            return reject();
        if (activation &&
            (!policy.disk_budget_bytes ||
             command.package_offer->manifest.size >
                 policy.disk_budget_bytes))
            return reject();
    }

    if (!existing) {
        State pending = state;
        begin_command(pending, command.leader_id, command.leader_epoch,
                      command.sequence, command.command_id, fingerprint,
                      action_name(command.action), {}, command.confirmed);
        save_state(state_root, pending);
        state = std::move(pending);
        (void)feature.sync_controls(state);
    }

    if (activation) {
        bool previously_authorized = existing;
        if (!previously_authorized) {
            admin::IntentOutcome reserved;
            previously_authorized =
                query_admin_outcome(command.command_id, reserved) &&
                reserved.state != admin::IntentOutcomeState::Unknown;
        }
        const TransferResult transfer = receive_update_package(
            channel, storage_root, policy, command, previously_authorized);
        if (!transfer) {
            admin::IntentOutcome broker;
            if (query_admin_outcome(command.command_id, broker)) {
                const auto reconciled = persist_broker_outcome(
                    state_root, state, feature, command.sequence, broker);
                if (reconciled) return {*reconciled, true};
                const CommandRecord* record =
                    find_command(state, command.sequence);
                if (broker.state == admin::IntentOutcomeState::Unknown &&
                    transfer.error != TransferError::Interrupted && record &&
                    record->outcome == CommandOutcome::Pending)
                    return {persist_admin_outcome(
                                state_root, state, feature, command.sequence,
                                CommandOutcome::Failed),
                            true};
            }
            const CommandRecord* record = find_command(state, command.sequence);
            return {record ? record->outcome : CommandOutcome::Pending, true};
        }
        if (!same_pairing(identity,
                          read_pairing_identity(state_root, machine_managed)))
            return {CommandOutcome::Rejected, true};
    }

    admin::IntentOutcome broker_outcome;
    if (!query_admin_outcome(command.command_id, broker_outcome))
        return {find_command(state, command.sequence)->outcome, true};
    if (broker_outcome.state != admin::IntentOutcomeState::Unknown) {
        const auto result = persist_broker_outcome(
            state_root, state, feature, command.sequence, broker_outcome);
        return {result.value_or(CommandOutcome::Rejected), false};
    }
    if (existing) {
        const CommandOutcome previous =
            find_command(state, command.sequence)->outcome;
        if (previous != CommandOutcome::Pending) {
            const auto result = persist_broker_outcome(
                state_root, state, feature, command.sequence, broker_outcome);
            return {result.value_or(previous), false};
        }
    }

    const auto reject_pending = [&](CommandOutcome outcome) {
        return AdminCommandResult{
            persist_admin_outcome(state_root, state, feature,
                                  command.sequence, outcome),
            false};
    };
    if (command_expired || command.expires_at_ms <= unix_millis())
        return reject_pending(CommandOutcome::Rejected);

    std::string error;
    std::optional<update::AuthorizedPackageOffer> authorized;
    {
        pairing::Worker paired = open_worker(state_root, machine_managed);
        if (!current_worker_matches(paired, identity) ||
            !authorize_admin_command(policy, paired, command, state,
                                     activation ? &authorized : nullptr,
                                     error))
            return reject_pending(CommandOutcome::Rejected);
        if (activation) {
            const service::Status installed = service::query_status();
            if (installed.state != service::State::Running ||
                !installed.configuration ||
                installed.configuration->storage_root.lexically_normal() !=
                    storage_root.lexically_normal() ||
                installed.configuration->state_root.lexically_normal() !=
                    state_root.lexically_normal())
                return reject_pending(CommandOutcome::Failed);
            const update::StageResult staged = update::StageUpdatePackage(
                *installed.configuration, policy, paired,
                spirula::NativeFilesystemPath(
                    storage_root / "agent-update-incoming" /
                    command.command_id),
                "package.bin", *authorized);
            if (!staged) return reject_pending(CommandOutcome::Failed);
        }
    }

    if (command.expires_at_ms <= unix_millis())
        return reject_pending(CommandOutcome::Rejected);
    (void)feature.sync_controls(state);
    if (feature.restart_jobs(false, state) != CommandOutcome::Applied)
        return reject_pending(CommandOutcome::Failed);

    pairing::Worker current = open_worker(state_root, machine_managed);
    if (!current_worker_matches(current, identity) ||
        !admin::VerifyIntent(policy, *command.admin_intent, unix_seconds(),
                             &error) ||
        (activation &&
         !authorize_admin_command(policy, current, command, state,
                                  &authorized, error)))
        return reject_pending(CommandOutcome::Rejected);

    admin::IntentOutcome before_submit;
    if (!query_admin_outcome(command.command_id, before_submit))
        return {CommandOutcome::Pending, true};
    if (before_submit.state != admin::IntentOutcomeState::Unknown) {
        const auto result = persist_broker_outcome(
            state_root, state, feature, command.sequence, before_submit);
        return {result.value_or(CommandOutcome::Rejected), false};
    }
    if (command.expires_at_ms <= unix_millis())
        return reject_pending(CommandOutcome::Rejected);

    const service::Result submitted = admin::submit_intent(
        *command.admin_intent,
        activation ? &*command.package_offer : nullptr);
    if (!submitted.success) {
        admin::IntentOutcome after_failure;
        if (!query_admin_outcome(command.command_id, after_failure))
            return {CommandOutcome::Pending, true};
        if (after_failure.state != admin::IntentOutcomeState::Unknown) {
            const auto result = persist_broker_outcome(
                state_root, state, feature, command.sequence, after_failure);
            return {result.value_or(CommandOutcome::Rejected), false};
        }
        return reject_pending(CommandOutcome::Failed);
    }
    return {persist_admin_outcome(state_root, state, feature,
                                  command.sequence, CommandOutcome::Accepted),
            false};
#endif
}

CommandOutcome process_command(const std::filesystem::path& state_root,
                               State& state, const wire::Command& command,
                               FeatureWorker& feature, bool machine_managed) {
    const std::string fingerprint = command_fingerprint(command);
    if (command.sequence <= state.last_command_sequence) {
        const CommandRecord* previous = find_command(state, command.sequence);
        if (!previous || previous->command_id != command.command_id ||
            previous->command_fingerprint != fingerprint)
            return CommandOutcome::Rejected;
        if (previous->outcome == CommandOutcome::Pending) {
            State recovered = state;
            finish_command(recovered, command.sequence, CommandOutcome::Failed);
            save_state(state_root, recovered);
            state = std::move(recovered);
            previous = find_command(state, command.sequence);
        }
        return previous ? previous->outcome : CommandOutcome::Rejected;
    }

    for (const CommandRecord& previous : state.recent_commands)
        if (previous.command_id == command.command_id)
            return CommandOutcome::Rejected;
    // Keep one command in flight until its asynchronous outcome is durable.
    if (std::any_of(state.recent_commands.begin(), state.recent_commands.end(),
                    [](const CommandRecord& record) {
                        return record.outcome == CommandOutcome::Accepted ||
                               (record.outcome == CommandOutcome::Pending &&
                                administrative_action(record.command_action));
                    })) {
        State rejected = state;
        begin_command(rejected, command.leader_id, command.leader_epoch,
                      command.sequence, command.command_id, fingerprint,
                      action_name(command.action), command.target_job_id,
                      command.confirmed);
        finish_command(rejected, command.sequence, CommandOutcome::Rejected);
        save_state(state_root, rejected);
        state = std::move(rejected);
        return CommandOutcome::Rejected;
    }

    State pending = state;
    begin_command(pending, command.leader_id, command.leader_epoch,
                  command.sequence, command.command_id, fingerprint,
                  action_name(command.action), command.target_job_id,
                  command.confirmed);
    save_state(state_root, pending);
    state = std::move(pending);
    const State original_intent = state;
    if (command.expires_at_ms <= unix_millis()) {
        State rejected = state;
        finish_command(rejected, command.sequence, CommandOutcome::Rejected);
        save_state(state_root, rejected);
        state = std::move(rejected);
        return CommandOutcome::Rejected;
    }

    State requested = state;
    apply_requested_state(requested, command.action);
    if (requested.paused != state.paused ||
        requested.maintenance != state.maintenance) {
        save_state(state_root, requested);
        state = std::move(requested);
    }
    if (command.expires_at_ms <= unix_millis()) {
        State rejected = state;
        rejected.paused = original_intent.paused;
        rejected.maintenance = original_intent.maintenance;
        finish_command(rejected, command.sequence, CommandOutcome::Rejected);
        save_state(state_root, rejected);
        state = std::move(rejected);
        return CommandOutcome::Rejected;
    }

    const bool restart =
        command.action == wire::CommandAction::RestartService ||
        command.action == wire::CommandAction::ForceRestartService;
    if (restart) (void)feature.sync_controls(state);
    CommandOutcome outcome = CommandOutcome::Rejected;
    if (restart) {
        if (managed_restart_host(machine_managed))
            outcome = feature.restart_jobs(
                command.action == wire::CommandAction::ForceRestartService,
                state);
    } else {
        outcome = apply_action(command, state, feature);
    }
    if (restart) {
        try {
            State completed = state;
            const bool accepted = outcome == CommandOutcome::Applied;
            finish_command(completed, command.sequence,
                           accepted ? CommandOutcome::Accepted : outcome);
            if (accepted)
                set_restart_intent(
                    completed, command.sequence,
                    command.action == wire::CommandAction::ForceRestartService);
            save_state(state_root, completed);
            state = std::move(completed);
            if (!accepted) (void)feature.sync_controls(state);
            return accepted ? CommandOutcome::Accepted : outcome;
        } catch (...) {
            State failed = state;
            try {
                finish_command(failed, command.sequence, CommandOutcome::Failed);
                save_state(state_root, failed);
            } catch (...) {
            }
            state = std::move(failed);
            (void)feature.sync_controls(state);
            return CommandOutcome::Failed;
        }
    }
    State completed = state;
    finish_command(completed, command.sequence, outcome);
    save_state(state_root, completed);
    state = std::move(completed);
    (void)feature.sync_controls(state);
    return outcome;
}

bool send_message(TlsChannel& channel, const wire::Message& message) {
    std::string json;
    const std::uint64_t now = unix_millis();
    if (!now || wire::Encode(message, now, json) != wire::Error::None)
        return false;
    return channel.SendFrame(
        reinterpret_cast<const std::uint8_t*>(json.data()), json.size());
}

bool send_status(TlsChannel& channel, const wire::Status& status) {
    return send_message(channel, wire::Message{wire::kSchemaVersion, status});
}

bool send_acknowledgment(TlsChannel& channel, const wire::Command& command,
                         CommandOutcome outcome) {
    wire::Acknowledgment acknowledgment;
    acknowledgment.command_id = command.command_id;
    acknowledgment.leader_id = command.leader_id;
    acknowledgment.leader_epoch = command.leader_epoch;
    acknowledgment.sequence = command.sequence;
    acknowledgment.action = command.action;
    acknowledgment.outcome = acknowledgment_outcome(outcome);
    return send_message(channel, wire::Message{wire::kSchemaVersion,
                                                std::move(acknowledgment)});
}
CommandOutcome settle_accepted_command(
    const std::filesystem::path& state_root, State& state,
    FeatureWorker& feature, std::uint64_t sequence, bool& settled) {
    settled = false;
    const CommandRecord* record = find_command(state, sequence);
    if (!record || record->outcome != CommandOutcome::Accepted)
        return record ? record->outcome : CommandOutcome::Rejected;
    bool complete = false;
    if (record->command_action == "pause")
        complete = feature.pause_acknowledged();
    else if (record->command_action == "resume")
        complete = feature.resume_acknowledged();
    else if (record->command_action == "stop")
        complete = feature.stop_complete(record->target_job_id);
    if (!complete) return CommandOutcome::Accepted;

    State applied = state;
    finish_command(applied, sequence, CommandOutcome::Applied);
    save_state(state_root, applied);
    state = std::move(applied);
    settled = true;
    (void)feature.sync_controls(state);
    return CommandOutcome::Applied;
}

wire::Command command_for_record(const State& state,
                                 const CommandRecord& record) {
    wire::Command command;
    command.command_id = record.command_id;
    command.leader_id = state.command_leader_id;
    command.leader_epoch = state.command_leader_epoch;
    command.sequence = record.sequence;
    command.action = action_from_name(record.command_action);
    command.target_job_id = record.target_job_id;
    command.confirmed = record.confirmed;
    return command;
}
bool resend_restart_acknowledgments(TlsChannel& channel, const State& state,
                                   FeatureWorker& feature) {
    std::vector<std::uint64_t> completed;
    for (const CommandRecord& record : state.recent_commands)
        if (record.outcome == CommandOutcome::Applied &&
            (record.command_action == "restart_service" ||
             record.command_action == "force_restart_service"))
            completed.push_back(record.sequence);
    if (completed.empty()) return true;
    if (!send_status(channel, feature.status(state))) return false;
    for (std::uint64_t sequence : completed) {
        const CommandRecord* record = find_command(state, sequence);
        if (!record ||
            !send_acknowledgment(channel, command_for_record(state, *record),
                                 CommandOutcome::Applied))
            return false;
    }
    return true;
}

bool poll_admin_records(const std::filesystem::path& state_root,
                       State& state, FeatureWorker& feature,
                       bool machine_managed, bool reject_unknown_pending) {
    std::vector<std::uint64_t> sequences;
    for (const CommandRecord& record : state.recent_commands)
        if (administrative_action(record.command_action) &&
            (record.outcome == CommandOutcome::Pending ||
             record.outcome == CommandOutcome::Accepted ||
             record.outcome == CommandOutcome::Applied))
            sequences.push_back(record.sequence);
    for (std::uint64_t sequence : sequences) {
        const CommandRecord* record = find_command(state, sequence);
        if (!record) continue;
#ifndef _WIN32
        (void)machine_managed;
        log_unsupported_admin_operation();
        (void)persist_admin_outcome(state_root, state, feature, sequence,
                                    CommandOutcome::Failed);
#else
        if (!machine_managed) {
            log_unsupported_admin_operation();
            (void)persist_admin_outcome(state_root, state, feature, sequence,
                                        CommandOutcome::Failed);
            continue;
        }
        admin::IntentOutcome outcome;
        if (!query_admin_outcome(record->command_id, outcome)) return false;
        if (outcome.state == admin::IntentOutcomeState::Unknown &&
            record->outcome == CommandOutcome::Pending &&
            reject_unknown_pending) {
            (void)persist_admin_outcome(state_root, state, feature, sequence,
                                        CommandOutcome::Rejected);
        } else {
            (void)persist_broker_outcome(state_root, state, feature, sequence,
                                         outcome);
        }
#endif
    }
    return true;
}

bool reconcile_admin_before_rebind(
    const std::filesystem::path& state_root, State& state,
    FeatureWorker& feature, bool machine_managed) {
    if (!poll_admin_records(state_root, state, feature, machine_managed, true))
        return false;
    return !has_outstanding_admin_command(state);
}

bool reconcile_accepted_commands(
    const std::filesystem::path& state_root, State& state,
    FeatureWorker& feature, bool machine_managed, TlsChannel& channel,
    std::map<std::uint64_t, CommandOutcome>& admin_acknowledgments) {
    std::vector<std::uint64_t> sequences;
    for (const CommandRecord& record : state.recent_commands)
        if (administrative_action(record.command_action) ||
            record.outcome == CommandOutcome::Accepted)
            sequences.push_back(record.sequence);

    for (std::uint64_t sequence : sequences) {
        const CommandRecord* record = find_command(state, sequence);
        if (!record) continue;
        if (administrative_action(record->command_action)) {
            CommandOutcome outcome = record->outcome;
            if (outcome == CommandOutcome::Pending ||
                outcome == CommandOutcome::Accepted ||
                outcome == CommandOutcome::Applied) {
                admin::IntentOutcome broker;
                if (!query_admin_outcome(record->command_id, broker)) {
                    if (outcome != CommandOutcome::Accepted) continue;
                } else {
                    const auto reconciled = persist_broker_outcome(
                        state_root, state, feature, sequence, broker);
                    if (!reconciled) continue;
                    outcome = *reconciled;
                }
            }
            if (outcome == CommandOutcome::Pending) continue;
            const auto sent = admin_acknowledgments.find(sequence);
            if (sent != admin_acknowledgments.end() &&
                sent->second == outcome)
                continue;
            record = find_command(state, sequence);
            if (!record || !send_acknowledgment(
                               channel, command_for_record(state, *record),
                               outcome))
                return false;
            admin_acknowledgments[sequence] = outcome;
            continue;
        }

        if (record->outcome != CommandOutcome::Accepted) continue;
        bool settled = false;
        const CommandOutcome outcome = settle_accepted_command(
            state_root, state, feature, sequence, settled);
        if (!settled) continue;
        record = find_command(state, sequence);
        if (!record || !send_acknowledgment(
                           channel, command_for_record(state, *record), outcome))
            return false;
    }
    return true;
}

bool receive_message(TlsChannel& channel, wire::Message& message) {
    std::vector<std::uint8_t> frame;
    if (!channel.ReceiveFrame(frame)) return false;
    const std::string json(frame.begin(), frame.end());
    const std::uint64_t now = unix_millis();
    return now && wire::Decode(json, now, message) == wire::Error::None;
}

bool receive_control_message(TlsChannel& channel, wire::Message& message,
                             bool& command_expired) {
    command_expired = false;
    std::vector<std::uint8_t> frame;
    if (!channel.ReceiveFrame(frame)) return false;
    const std::string json(frame.begin(), frame.end());
    const std::uint64_t now = unix_millis();
    if (!now) return false;
    const wire::Error error = wire::Decode(json, now, message);
    if (error == wire::Error::None) return true;
    if (error != wire::Error::ExpiredCommand) return false;

    try {
        const JsonValue root = json_parse(json);
        const JsonValue* type = root.find("type");
        const JsonValue* issued = root.find("issued_at_ms");
        if (!type || type->type != JsonValue::Type::String ||
            type->str != "command" || !issued ||
            issued->type != JsonValue::Type::Number ||
            !std::isfinite(issued->num) || issued->num < 0 ||
            issued->num > 9007199254740991.0 ||
            std::floor(issued->num) != issued->num)
            return false;
        wire::Message delayed;
        if (wire::Decode(json, static_cast<std::uint64_t>(issued->num),
                         delayed) != wire::Error::None)
            return false;
        const auto* command = std::get_if<wire::Command>(&delayed.payload);
        if (!command || !administrative_action(command->action))
            return false;
        message = std::move(delayed);
        command_expired = true;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}


bool valid_leader_status(const wire::Status& status) noexcept {
    return status.connection == wire::ConnectionState::Connected &&
           status.scheduling == wire::SchedulingState::Stopped &&
           status.capabilities.empty();
}

bool matches_feature_decision(const wire::FeatureDecision& decision,
                              const wire::FeatureOffer& offer,
                              wire::FeatureDecision::Step step) noexcept {
    return decision.job_id == offer.job_id &&
           decision.attempt_id == offer.attempt_id && decision.step == step;
}

bool matches_portable_decision(
    const wire::PortableDecision& decision, const wire::PortableOffer& offer,
    wire::PortableDecision::Step step) noexcept {
    return decision.workload == offer.workload &&
           decision.job_id == offer.job_id &&
           decision.attempt_id == offer.attempt_id && decision.step == step;
}

bool handle_feature_offer(
    TlsChannel& channel, const std::filesystem::path& state_root,
    bool machine_managed, State& state, FeatureWorker& feature,
    const PairingIdentity& identity, const wire::FeatureOffer& offer,
    StopRequested stop_requested, void* stop_context) {
    wire::FeatureDecision offer_decision;
    {
        const pairing::Worker verified = open_worker(state_root, machine_managed);
        if (!same_pairing(identity, identity_of(verified))) return false;
        offer_decision = feature.accept_offer(offer, state, identity.leader_id,
                                               identity.leader_epoch);
        if (!send_message(channel, wire::Message{wire::kSchemaVersion,
                                                  offer_decision}))
            return false;
    }
    if (offer_decision.decision !=
            wire::FeatureDecision::Decision::Accepted ||
        stopping(stop_requested, stop_context))
        return true;

    const TransferResult transfer = feature.receive_inputs(channel, offer);
    if (transfer.error == TransferError::Interrupted ||
        stopping(stop_requested, stop_context))
        return false;
    std::string error = transfer.message;
    const bool verified_inputs = transfer && feature.verify_inputs(offer, error);
    {
        const pairing::Worker verified = open_worker(state_root, machine_managed);
        if (!same_pairing(identity, identity_of(verified))) return false;
        const wire::FeatureDecision input_decision =
            verified_inputs ? feature.accept_inputs(offer, state)
                            : feature.reject_inputs(offer, state, error);
        if (!send_message(channel, wire::Message{wire::kSchemaVersion,
                                                  input_decision}))
            return false;
    }
    return true;
}

bool handle_feature_result(
    TlsChannel& channel, const std::filesystem::path& state_root,
    bool machine_managed, FeatureWorker& feature,
    const PairingIdentity& identity,
    const feature_worker_detail::FeatureOfferRecord& record,
    const wire::FeatureResult& result, StopRequested stop_requested,
    void* stop_context) {
    {
        const pairing::Worker verified = open_worker(state_root, machine_managed);
        if (!same_pairing(identity, identity_of(verified)) ||
            !send_message(channel, wire::Message{wire::kSchemaVersion, result}))
            return false;
    }
    wire::Message reply;
    if (!receive_message(channel, reply)) return false;
    const auto* initial = std::get_if<wire::FeatureDecision>(&reply.payload);
    if (!initial ||
        !matches_feature_decision(*initial, record.offer,
                                  wire::FeatureDecision::Step::Output))
        return false;
    wire::FeatureDecision confirmation = *initial;
    if (initial->decision == wire::FeatureDecision::Decision::Accepted) {
        if (result.outcome != wire::FeatureResult::Outcome::Succeeded ||
            result.outputs.empty() || stopping(stop_requested, stop_context))
            return false;
        {
            const pairing::Worker verified = open_worker(state_root,
                                                          machine_managed);
            if (!same_pairing(identity, identity_of(verified))) return false;
        }
        if (!feature.send_outputs(channel, record.offer)) return false;
        {
            const pairing::Worker verified = open_worker(state_root,
                                                          machine_managed);
            if (!same_pairing(identity, identity_of(verified))) return false;
        }
        if (stopping(stop_requested, stop_context) ||
            !receive_message(channel, reply))
            return false;
        const auto* final = std::get_if<wire::FeatureDecision>(&reply.payload);
        if (!final ||
            !matches_feature_decision(*final, record.offer,
                                      wire::FeatureDecision::Step::Output) ||
            (final->decision != wire::FeatureDecision::Decision::Committed &&
             final->decision != wire::FeatureDecision::Decision::Rejected))
            return false;
        confirmation = *final;
    } else if (initial->decision ==
               wire::FeatureDecision::Decision::Rejected) {
        // The leader explicitly declined this result; no artifact transfer.
    } else if (initial->decision ==
                   wire::FeatureDecision::Decision::Committed &&
               result.outcome != wire::FeatureResult::Outcome::Succeeded) {
        // Failed/interrupted attempts have no artifacts to transfer.
    } else {
        return false;
    }

    {
        const pairing::Worker verified = open_worker(state_root, machine_managed);
        if (!same_pairing(identity, identity_of(verified))) return false;
        feature.confirm_output(record.offer, confirmation);
    }
    feature.cleanup_confirmed_output(record.offer);
    return true;
}

bool handle_portable_offer(
    TlsChannel& channel, const std::filesystem::path& state_root,
    bool machine_managed, State& state, FeatureWorker& feature,
    const PairingIdentity& identity, const wire::PortableOffer& offer,
    StopRequested stop_requested, void* stop_context) {
    wire::PortableDecision offer_decision;
    {
        const pairing::Worker verified = open_worker(state_root, machine_managed);
        if (!same_pairing(identity, identity_of(verified))) return false;
        offer_decision = feature.accept_portable_offer(
            offer, state, identity.leader_id, identity.leader_epoch);
        if (!send_message(channel,
                          wire::Message{wire::kSchemaVersion, offer_decision}))
            return false;
    }
    if (offer_decision.decision !=
            wire::PortableDecision::Decision::Accepted ||
        stopping(stop_requested, stop_context))
        return true;

    const TransferResult transfer =
        feature.receive_portable_inputs(channel, offer);
    if (transfer.error == TransferError::Interrupted ||
        stopping(stop_requested, stop_context))
        return false;
    std::string error = transfer.message;
    const bool verified_inputs =
        transfer && feature.verify_portable_inputs(offer, error);
    {
        const pairing::Worker verified = open_worker(state_root, machine_managed);
        if (!same_pairing(identity, identity_of(verified))) return false;
        const wire::PortableDecision input_decision =
            verified_inputs ? feature.accept_portable_inputs(offer, state)
                            : feature.reject_portable_inputs(offer, error);
        if (!send_message(channel,
                          wire::Message{wire::kSchemaVersion, input_decision}))
            return false;
    }
    return true;
}

bool handle_portable_result(
    TlsChannel& channel, const std::filesystem::path& state_root,
    bool machine_managed, FeatureWorker& feature,
    const PairingIdentity& identity, const wire::PortableResult& result,
    StopRequested stop_requested, void* stop_context) {
    const std::optional<wire::PortableOffer> offer =
        feature.portable_offer_for_result(result, identity.leader_id,
                                          identity.leader_epoch);
    if (!offer) return false;
    {
        const pairing::Worker verified = open_worker(state_root, machine_managed);
        if (!same_pairing(identity, identity_of(verified)) ||
            !send_message(channel, wire::Message{wire::kSchemaVersion, result}))
            return false;
    }
    wire::Message reply;
    if (!receive_message(channel, reply)) return false;
    const auto* initial = std::get_if<wire::PortableDecision>(&reply.payload);
    if (!initial ||
        !matches_portable_decision(*initial, *offer,
                                   wire::PortableDecision::Step::Output))
        return false;
    wire::PortableDecision confirmation = *initial;
    if (initial->decision == wire::PortableDecision::Decision::Accepted) {
        const bool training =
            offer->workload == wire::PortableWorkload::Training;
        if (result.outcome != wire::PortableResult::Outcome::Succeeded ||
            result.outputs.empty() ||
            (training ? result.output_metadata.empty()
                      : !result.output_metadata.empty()) ||
            stopping(stop_requested, stop_context))
            return false;
        {
            const pairing::Worker verified = open_worker(state_root,
                                                          machine_managed);
            if (!same_pairing(identity, identity_of(verified))) return false;
        }
        const TransferResult transfer = feature.send_portable_outputs(
            channel, *offer, identity.leader_id, identity.leader_epoch);
        if (!transfer || stopping(stop_requested, stop_context)) return false;
        {
            const pairing::Worker verified = open_worker(state_root,
                                                          machine_managed);
            if (!same_pairing(identity, identity_of(verified))) return false;
        }
        if (!receive_message(channel, reply)) return false;
        const auto* final = std::get_if<wire::PortableDecision>(&reply.payload);
        if (!final ||
            !matches_portable_decision(*final, *offer,
                                       wire::PortableDecision::Step::Output) ||
            (final->decision != wire::PortableDecision::Decision::Committed &&
             final->decision != wire::PortableDecision::Decision::Rejected))
            return false;
        confirmation = *final;
    } else if (initial->decision == wire::PortableDecision::Decision::Rejected) {
        if (result.outcome != wire::PortableResult::Outcome::Succeeded)
            return false;
    } else if (initial->decision != wire::PortableDecision::Decision::Committed) {
        return false;
    }

    {
        const pairing::Worker verified = open_worker(state_root, machine_managed);
        if (!same_pairing(identity, identity_of(verified))) return false;
        feature.confirm_portable_output(*offer, confirmation,
                                        identity.leader_id,
                                        identity.leader_epoch);
    }
    feature.cleanup_confirmed_portable_output(*offer);
    return true;
}

// All TLS I/O stays serialized; transfers and local verification run without a
// pairing lock, while each authorization and its decision remain locked.
void serve_channel(TlsChannel& channel, const Config& config,
                   const std::filesystem::path& state_root,
                   const std::filesystem::path& storage_root,
                   bool machine_managed, State& state, FeatureWorker& feature,
                   const PairingIdentity& identity,
                   StopRequested stop_requested, void* stop_context,
                   std::chrono::milliseconds& backoff) {
    bool restart_acknowledgments_sent = false;
    std::map<std::uint64_t, CommandOutcome> admin_acknowledgments;
    while (!stopping(stop_requested, stop_context)) {
        if (!same_pairing(identity,
                          read_pairing_identity(state_root, machine_managed)))
            return;
        if (!feature.bind_portable_leader(identity.leader_id,
                                          identity.leader_epoch))
            return;
        feature.refresh(state);
        feature.sync_controls(state);
        if (!restart_acknowledgments_sent) {
            if (!resend_restart_acknowledgments(channel, state, feature))
                return;
            restart_acknowledgments_sent = true;
        }
        if (!reconcile_accepted_commands(
                state_root, state, feature, machine_managed, channel,
                admin_acknowledgments))
            return;
        const auto pending = feature.pending_result(state);
        bool result_for_leader = false;
        feature_worker_detail::FeatureOfferRecord record;
        if (pending) {
            std::string error;
            if (!feature_worker_detail::LoadFeatureOfferRecord(
                    storage_root / "feature-shards", pending->job_id,
                    pending->attempt_id, record, error))
                return;
            result_for_leader =
                record.leader_id == identity.leader_id &&
                record.leader_epoch == identity.leader_epoch &&
                record.offer.job_id == pending->job_id &&
                record.offer.attempt_id == pending->attempt_id;
        }
        const auto portable_pending =
            result_for_leader
                ? std::optional<wire::PortableResult>{}
                : feature.pending_portable_result(identity.leader_id,
                                                  identity.leader_epoch);

        const Clock::time_point next_heartbeat = Clock::now() + kHeartbeat;
        if (result_for_leader) {
            if (!handle_feature_result(channel, state_root, machine_managed,
                                       feature, identity, record, *pending,
                                       stop_requested, stop_context))
                return;
            backoff = kInitialBackoff;
        } else if (portable_pending) {
            if (!send_status(channel, feature.status(state)) ||
                stopping(stop_requested, stop_context) ||
                !handle_portable_result(channel, state_root, machine_managed,
                                        feature, identity, *portable_pending,
                                        stop_requested, stop_context))
                return;
            backoff = kInitialBackoff;
        } else {
            if (!send_status(channel, feature.status(state))) return;
            if (stopping(stop_requested, stop_context)) return;
            wire::Message message;
            bool command_expired = false;
            if (!receive_control_message(channel, message, command_expired) ||
                stopping(stop_requested, stop_context))
                return;

            if (const auto* command =
                    std::get_if<wire::Command>(&message.payload)) {
                if (!allowed_action(command->action) ||
                    command->leader_id != identity.leader_id ||
                    command->leader_epoch != identity.leader_epoch)
                    return;
                if (administrative_action(command->action)) {
                    {
                        const pairing::Worker verified =
                            open_worker(state_root, machine_managed);
                        if (!same_pairing(identity, identity_of(verified)))
                            return;
                    }
                    const AdminCommandResult result = process_admin_command(
                        state_root, storage_root, config, state, *command,
                        feature, machine_managed, identity, command_expired,
                        channel);
                    if (result.close_channel) return;
                    if (!send_acknowledgment(channel, *command,
                                             result.outcome))
                        return;
                    admin_acknowledgments[command->sequence] = result.outcome;
                } else {
                    const pairing::Worker verified =
                        open_worker(state_root, machine_managed);
                    if (!same_pairing(identity, identity_of(verified))) return;
                    CommandOutcome outcome = process_command(
                        state_root, state, *command, feature,
                        machine_managed);
                    if (outcome == CommandOutcome::Accepted) {
                        bool settled = false;
                        outcome = settle_accepted_command(
                            state_root, state, feature, command->sequence,
                            settled);
                    }
                    if (!send_acknowledgment(channel, *command, outcome))
                        return;
                }
                if (state.restart_intent) return;
                backoff = kInitialBackoff;
            } else if (const auto* status =
                           std::get_if<wire::Status>(&message.payload)) {
                if (!valid_leader_status(*status)) return;
                backoff = kInitialBackoff;
            } else if (const auto* offer =
                           std::get_if<wire::FeatureOffer>(&message.payload)) {
                if (!handle_feature_offer(channel, state_root, machine_managed,
                                          state, feature, identity, *offer,
                                          stop_requested, stop_context))
                    return;
                backoff = kInitialBackoff;
            } else if (const auto* portable_offer =
                           std::get_if<wire::PortableOffer>(&message.payload)) {
                if (!handle_portable_offer(
                        channel, state_root, machine_managed, state, feature,
                        identity, *portable_offer, stop_requested,
                        stop_context))
                    return;
                backoff = kInitialBackoff;
            } else {
                return;
            }
        }
        if (!reconcile_accepted_commands(
                state_root, state, feature, machine_managed, channel,
                admin_acknowledgments))
            return;
        if (!wait_until(stop_requested, stop_context, next_heartbeat)) return;
    }
}

}  // namespace

HostExit RunAgentClient(const Config& config,
                    const std::filesystem::path& state_root,
                    const std::filesystem::path& storage_root,
                    bool machine_managed, State& state,
                    StopRequested stop_requested, void* stop_context,
                    WorkerReadyCallback ready_callback,
                    void* ready_context) {
    if (!stop_requested)
        throw std::invalid_argument("agent client requires a stop predicate");
    if (storage_root.empty())
        throw std::invalid_argument("agent client requires a storage root");
    if (state.restart_intent && !managed_restart_host(machine_managed))
        throw std::runtime_error(
            "pending restart intent requires the installed service host");

    FeatureWorker feature(config, state_root, storage_root, app::exe_path(),
                          state);
    if (state.restart_intent &&
        feature.status(state).health == wire::HealthState::Unhealthy)
        throw std::runtime_error(
            "agent scheduler recovery is unhealthy; restart intent remains pending");
    auto backoff = std::chrono::duration_cast<std::chrono::milliseconds>(
        kInitialBackoff);
    bool readiness_reported = false;
    while (!stopping(stop_requested, stop_context)) {
        (void)poll_admin_records(state_root, state, feature,
                                machine_managed, false);
        feature.sync_controls(state);
        PairingSnapshot snapshot =
            read_pairing_snapshot(state_root, config, machine_managed);
        if (snapshot.identity.status != pairing::WorkerStatus::Paired) {
            if (!wait_until(stop_requested, stop_context,
                            Clock::now() + kPairingRefresh))
                return HostExit::Stopped;
            continue;
        }

        const PairingIdentity& identity = snapshot.identity;
        if (identity.worker_id.empty() || identity.leader_id.empty() ||
            !identity.leader_epoch)
            throw std::runtime_error("paired worker identity is incomplete");
        if (state.restart_intent) {
            if (!managed_restart_host(machine_managed))
                throw std::runtime_error(
                    "verified host recovery is not ready to settle restart intent");
            feature.refresh(state);
            if (feature.status(state).health == wire::HealthState::Unhealthy)
                throw std::runtime_error(
                    "agent scheduler recovery is unhealthy; restart intent remains pending");
            State settled = state;
            const std::uint64_t sequence = settled.restart_intent->sequence;
            complete_restart_intent(settled, sequence);
            save_state(state_root, settled);
            state = std::move(settled);
        }
        if (!state.command_leader_id.empty() &&
            (state.command_leader_id != identity.leader_id ||
             state.command_leader_epoch != identity.leader_epoch) &&
            !reconcile_admin_before_rebind(state_root, state, feature,
                                           machine_managed)) {
            if (!wait_until(stop_requested, stop_context,
                            Clock::now() + kPairingRefresh))
                return HostExit::Stopped;
            continue;
        }
        bind_command_leader(state_root, state, identity);
        recover_pending_commands(state_root, state);
        feature.sync_controls(state);

        TlsChannel::ClientOptions options = std::move(*snapshot.options);
        options.tls.handshake_timeout_ms = kHandshakeTimeoutMs;
        options.tls.io_timeout_ms = kIoTimeoutMs;
        options.connect_timeout_ms = kConnectTimeoutMs;

        if (!same_pairing(identity,
                          read_pairing_identity(state_root, machine_managed))) {
            if (!wait_until(stop_requested, stop_context,
                            Clock::now() + kInitialBackoff))
                return HostExit::Stopped;
            continue;
        }
        if (ready_callback && !readiness_reported) {
            const wire::Status readiness = feature.status(state);
            if (!ready_callback(readiness, ready_context))
                throw std::runtime_error(
                    "broker rejected restricted worker initialization readiness");
            readiness_reported = true;
        }
        std::string connect_error;
        std::optional<TlsChannel> channel =
            TlsChannel::ConnectClient(options, &connect_error);
        if (!channel) {
            if (!wait_until(stop_requested, stop_context,
                            Clock::now() + backoff))
                return HostExit::Stopped;
            increase_backoff(backoff);
            continue;
        }

        const auto peer = channel->PeerSpkiSha256();
        if (!peer || *peer != options.tls.expected_peer_spki_sha256) {
            channel->Close();
            increase_backoff(backoff);
        } else {
            serve_channel(*channel, config, state_root, storage_root,
                          machine_managed, state, feature, identity,
                          stop_requested, stop_context, backoff);
            channel->Close();
            if (state.restart_intent)
                return HostExit::RestartRequested;
        }
        if (stopping(stop_requested, stop_context))
            return HostExit::Stopped;
        if (!wait_until(stop_requested, stop_context, Clock::now() + backoff))
            return HostExit::Stopped;
        increase_backoff(backoff);
    }
    return HostExit::Stopped;
}

}  // namespace app::agent
