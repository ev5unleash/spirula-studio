#include "app/AgentWire.h"

#include <cstdio>
#include <string>
#include <variant>

namespace {

using app::agent::admin::Intent;
using app::agent::admin::Operation;
using app::agent::update::PackageManifest;
using app::agent::update::PackageOffer;
using app::agent::wire::Acknowledgment;
using app::agent::wire::AcknowledgmentOutcome;
using app::agent::wire::ActivityState;
using app::agent::wire::Capability;
using app::agent::wire::Command;
using app::agent::wire::CommandAction;
using app::agent::wire::CompatibilityState;
using app::agent::wire::ConnectionState;
using app::agent::wire::Decode;
using app::agent::wire::Encode;
using app::agent::wire::Error;
using app::agent::wire::HealthState;
using app::agent::wire::Message;
using app::agent::wire::SchedulingState;
using app::agent::wire::Status;
using app::agent::wire::kMaxRestartCommandLifetimeMs;
using app::agent::wire::FeatureDecision;
using app::agent::wire::FeatureOffer;
using app::agent::wire::FeatureResult;
using app::agent::wire::PortableDecision;
using app::agent::wire::PortableOffer;
using app::agent::wire::PortableResult;
using app::agent::wire::PortableWorkload;
using app::agent::TransferFile;

int failures = 0;

void check(bool condition, const char* name) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", name);
    if (!condition) ++failures;
}

Error rejects(const std::string& json, std::uint64_t now_ms) {
    Message message;
    return Decode(json, now_ms, message);
}

bool replace_once(std::string& text, const std::string& from,
                  const std::string& to) {
    const std::size_t at = text.find(from);
    if (at == std::string::npos) return false;
    text.replace(at, from.size(), to);
    return true;
}

}  // namespace

int main() {
    constexpr std::uint64_t now_ms = 1'700'000'000'000ULL;

    Status status;
    status.connection = ConnectionState::Connected;
    status.compatibility = CompatibilityState::Compatible;
    status.scheduling = SchedulingState::Accepting;
    status.activity = ActivityState::Training;
    status.health = HealthState::Healthy;
    status.capabilities = {Capability::Feature, Capability::Reconstruction,
                           Capability::Training};
    status.build = "spirula-1.2.3";
    status.platform = "Windows 11 x64";
    status.gpu = "AMD Radeon RX 7900 XTX";
    status.online = true;
    Message status_message;
    status_message.payload = status;

    std::string json;
    check(Encode(status_message, now_ms, json) == Error::None,
          "status encodes");
    Message decoded_status;
    check(Decode(json, now_ms, decoded_status) == Error::None,
          "status decodes");
    const auto* roundtrip_status = std::get_if<Status>(&decoded_status.payload);
    check(roundtrip_status &&
              roundtrip_status->connection == status.connection &&
              roundtrip_status->compatibility == status.compatibility &&
              roundtrip_status->scheduling == status.scheduling &&
              roundtrip_status->activity == status.activity &&
              roundtrip_status->health == status.health &&
              roundtrip_status->capabilities == status.capabilities &&
              roundtrip_status->build == status.build &&
              roundtrip_status->platform == status.platform &&
              roundtrip_status->gpu == status.gpu &&
              roundtrip_status->maintenance == status.maintenance &&
              roundtrip_status->online == status.online,
          "status roundtrips all axes and advertisements");
    Status pausing_status = status;
    pausing_status.scheduling = SchedulingState::Pausing;
    Message pausing_message;
    pausing_message.payload = pausing_status;
    check(Encode(pausing_message, now_ms, json) == Error::None &&
              Decode(json, now_ms, decoded_status) == Error::None &&
              std::get_if<Status>(&decoded_status.payload) &&
              std::get<Status>(decoded_status.payload).scheduling ==
                  SchedulingState::Pausing,
          "worker status exposes an unacknowledged pause request");
    Status invalid_enum = status;
    invalid_enum.connection = static_cast<ConnectionState>(255);
    Message invalid_enum_message;
    invalid_enum_message.payload = invalid_enum;
    check(Encode(invalid_enum_message, now_ms, json) == Error::InvalidField,
          "invalid enum values are rejected");

    Status oversized_string = status;
    oversized_string.build =
        std::string(app::agent::wire::kMaxBuildBytes + 1, 'b');
    Message oversized_string_message;
    oversized_string_message.payload = oversized_string;
    check(Encode(oversized_string_message, now_ms, json) == Error::InvalidField,
          "oversized metadata is rejected");

    Command command;
    command.command_id = "command-42";
    command.leader_id = "leader-7f3a";
    command.leader_epoch = 4;
    command.sequence = 42;
    command.target_job_id.clear();
    command.confirmed = false;
    command.action = CommandAction::Pause;
    command.issued_at_ms = now_ms - 1'000;
    command.expires_at_ms = now_ms + 60'000;
    Message command_message;
    command_message.payload = command;
    check(Encode(command_message, now_ms, json) == Error::None,
          "command encodes");
    Message decoded_command;
    check(Decode(json, now_ms, decoded_command) == Error::None,
          "command decodes");
    const auto* roundtrip_command = std::get_if<Command>(&decoded_command.payload);
    check(roundtrip_command &&
              roundtrip_command->command_id == command.command_id &&
              roundtrip_command->leader_id == command.leader_id &&
              roundtrip_command->leader_epoch == command.leader_epoch &&
              roundtrip_command->sequence == command.sequence &&
              roundtrip_command->action == command.action &&
              roundtrip_command->target_job_id == command.target_job_id &&
              roundtrip_command->confirmed == command.confirmed &&
              roundtrip_command->issued_at_ms == command.issued_at_ms &&
              roundtrip_command->expires_at_ms == command.expires_at_ms,
          "command roundtrips durable replay identity, target and expiry");
    const std::string ordinary_command_json = json;
    check(ordinary_command_json.find("\"admin_intent\"") == std::string::npos &&
              ordinary_command_json.find("\"package_offer\"") == std::string::npos,
          "ordinary commands omit signed admin payloads");
    const CommandAction mode_actions[] = {
        CommandAction::Maintenance, CommandAction::Online,
        CommandAction::RestartService, CommandAction::ForceRestartService,
    };
    for (CommandAction action : mode_actions) {
        Command mode_command = command;
        mode_command.action = action;
        mode_command.confirmed =
            action == CommandAction::ForceRestartService;
        Message mode_message;
        mode_message.payload = mode_command;
        Message decoded_mode;
        const bool roundtrips =
            Encode(mode_message, now_ms, json) == Error::None &&
            Decode(json, now_ms, decoded_mode) == Error::None;
        const auto* roundtrip_mode = std::get_if<Command>(&decoded_mode.payload);
        const char* description =
            action == CommandAction::Online
                ? "online remains distinct from resume"
                : action == CommandAction::Maintenance
                      ? "maintenance command roundtrips"
                      : action == CommandAction::RestartService
                            ? "safe restart command roundtrips"
                            : "confirmed force restart command roundtrips";
        check(roundtrips && roundtrip_mode &&
                  roundtrip_mode->action == action &&
                  roundtrip_mode->confirmed == mode_command.confirmed,
              description);
    }
    Command invalid_restart = command;
    invalid_restart.action = CommandAction::RestartService;
    invalid_restart.confirmed = false;
    invalid_restart.target_job_id = "job-1";
    Message invalid_restart_message;
    invalid_restart_message.payload = invalid_restart;
    check(Encode(invalid_restart_message, now_ms, json) ==
              Error::InvalidCombination,
          "safe restart rejects a target job");
    invalid_restart.target_job_id.clear();
    invalid_restart.confirmed = true;
    invalid_restart_message.payload = invalid_restart;
    check(Encode(invalid_restart_message, now_ms, json) ==
              Error::InvalidCombination,
          "safe restart rejects explicit confirmation");
    invalid_restart.action = CommandAction::ForceRestartService;
    invalid_restart.target_job_id.clear();
    invalid_restart.confirmed = false;
    invalid_restart_message.payload = invalid_restart;
    check(Encode(invalid_restart_message, now_ms, json) ==
              Error::InvalidCombination,
          "force restart requires explicit confirmation");
    invalid_restart.confirmed = true;
    invalid_restart.expires_at_ms =
        invalid_restart.issued_at_ms + kMaxRestartCommandLifetimeMs + 1;
    invalid_restart_message.payload = invalid_restart;
    check(Encode(invalid_restart_message, now_ms, json) ==
              Error::InvalidCombination,
          "restart command delivery expiry is bounded");
    const std::string admin_leader_id(64, 'a');
    const std::string admin_worker_id(64, 'b');
    const std::string package_digest(64, 'c');
    const std::string signer_pem =
        "-----BEGIN PUBLIC KEY-----\nAQID\n-----END PUBLIC KEY-----\n";
    const std::vector<std::uint8_t> signature = {
        0x30, 0x06, 0x02, 0x01, 0x00, 0x02, 0x01, 0x01,
    };
    const std::uint64_t now_unix = now_ms / 1000;

    Intent reboot_intent;
    reboot_intent.operation = Operation::Reboot;
    reboot_intent.worker_id = admin_worker_id;
    reboot_intent.leader_id = admin_leader_id;
    reboot_intent.leader_epoch = command.leader_epoch;
    reboot_intent.intent_id = "reboot-1";
    reboot_intent.issued_at_unix = now_unix - 30;
    reboot_intent.expires_at_unix = now_unix + 240;
    reboot_intent.signer_public_key_pem = signer_pem;
    reboot_intent.signature = signature;

    Command reboot_command = command;
    reboot_command.action = CommandAction::RebootMachine;
    reboot_command.leader_id = admin_leader_id;
    reboot_command.confirmed = true;
    reboot_command.issued_at_ms = now_ms - 1'000;
    reboot_command.expires_at_ms = now_ms + 30'000;
    reboot_command.admin_intent = reboot_intent;
    const auto encode_command = [&](const Command& candidate) {
        Message message;
        message.payload = candidate;
        return Encode(message, now_ms, json);
    };
    check(encode_command(reboot_command) == Error::None,
          "signed reboot command encodes");
    const std::string reboot_json = json;
    Message decoded_reboot;
    const bool reboot_decodes =
        Decode(reboot_json, now_ms, decoded_reboot) == Error::None;
    const Command* roundtrip_reboot =
        std::get_if<Command>(&decoded_reboot.payload);
    check(reboot_decodes && roundtrip_reboot &&
              roundtrip_reboot->action == CommandAction::RebootMachine &&
              roundtrip_reboot->confirmed && roundtrip_reboot->admin_intent &&
              !roundtrip_reboot->package_offer &&
              roundtrip_reboot->admin_intent->operation == Operation::Reboot &&
              roundtrip_reboot->admin_intent->worker_id == reboot_intent.worker_id &&
              roundtrip_reboot->admin_intent->leader_id == reboot_intent.leader_id &&
              roundtrip_reboot->admin_intent->leader_epoch == reboot_intent.leader_epoch &&
              roundtrip_reboot->admin_intent->intent_id == reboot_intent.intent_id &&
              roundtrip_reboot->admin_intent->issued_at_unix ==
                  reboot_intent.issued_at_unix &&
              roundtrip_reboot->admin_intent->expires_at_unix ==
                  reboot_intent.expires_at_unix &&
              roundtrip_reboot->admin_intent->signer_public_key_pem == signer_pem &&
              roundtrip_reboot->admin_intent->signature == signature,
          "signed reboot intent and binary signature roundtrip exactly");

    PackageOffer package_offer;
    package_offer.worker_id = admin_worker_id;
    package_offer.leader_id = admin_leader_id;
    package_offer.expires_at_unix = now_unix + 120;
    package_offer.update_id = "update-2026-09";
    package_offer.manifest = PackageManifest{
        "windows", "x86_64", "spirula-1.2.3", "2026.09.23",
        4096, package_digest,
    };
    package_offer.signer_public_key_pem = signer_pem;
    package_offer.signature = signature;
    Intent activate_intent = reboot_intent;
    activate_intent.operation = Operation::Activate;
    activate_intent.intent_id = "activate-1";
    activate_intent.update_id = package_offer.update_id;
    activate_intent.package_sha256 = package_offer.manifest.sha256;
    activate_intent.security_version = 7;

    Command activate_command = reboot_command;
    activate_command.action = CommandAction::ActivateUpdate;
    activate_command.admin_intent = activate_intent;
    activate_command.package_offer = package_offer;
    check(encode_command(activate_command) == Error::None,
          "signed activation command encodes");
    const std::string activate_json = json;
    Message decoded_activate;
    const bool activate_decodes =
        Decode(activate_json, now_ms, decoded_activate) == Error::None;
    const Command* roundtrip_activate =
        std::get_if<Command>(&decoded_activate.payload);
    check(activate_decodes && roundtrip_activate &&
              activate_json.find("\"package_bytes\"") == std::string::npos &&
              roundtrip_activate->action == CommandAction::ActivateUpdate &&
              roundtrip_activate->confirmed && roundtrip_activate->admin_intent &&
              roundtrip_activate->package_offer &&
              roundtrip_activate->admin_intent->operation == Operation::Activate &&
              roundtrip_activate->admin_intent->worker_id == activate_intent.worker_id &&
              roundtrip_activate->admin_intent->leader_id == activate_intent.leader_id &&
              roundtrip_activate->admin_intent->leader_epoch ==
                  activate_intent.leader_epoch &&
              roundtrip_activate->admin_intent->intent_id == activate_intent.intent_id &&
              roundtrip_activate->admin_intent->issued_at_unix ==
                  activate_intent.issued_at_unix &&
              roundtrip_activate->admin_intent->expires_at_unix ==
                  activate_intent.expires_at_unix &&
              roundtrip_activate->admin_intent->update_id ==
                  activate_intent.update_id &&
              roundtrip_activate->admin_intent->package_sha256 ==
                  activate_intent.package_sha256 &&
              roundtrip_activate->admin_intent->security_version ==
                  activate_intent.security_version &&
              roundtrip_activate->admin_intent->signer_public_key_pem == signer_pem &&
              roundtrip_activate->admin_intent->signature == signature &&
              roundtrip_activate->package_offer->worker_id == package_offer.worker_id &&
              roundtrip_activate->package_offer->leader_id == package_offer.leader_id &&
              roundtrip_activate->package_offer->expires_at_unix ==
                  package_offer.expires_at_unix &&
              roundtrip_activate->package_offer->update_id == package_offer.update_id &&
              roundtrip_activate->package_offer->manifest.os ==
                  package_offer.manifest.os &&
              roundtrip_activate->package_offer->manifest.architecture ==
                  package_offer.manifest.architecture &&
              roundtrip_activate->package_offer->manifest.build ==
                  package_offer.manifest.build &&
              roundtrip_activate->package_offer->manifest.release ==
                  package_offer.manifest.release &&
              roundtrip_activate->package_offer->manifest.size ==
                  package_offer.manifest.size &&
              roundtrip_activate->package_offer->manifest.sha256 ==
                  package_offer.manifest.sha256 &&
              roundtrip_activate->package_offer->signer_public_key_pem == signer_pem &&
              roundtrip_activate->package_offer->signature == signature,
          "signed activation intent and offer roundtrip every field");

    Command bare_admin = command;
    bare_admin.leader_id = admin_leader_id;
    bare_admin.action = CommandAction::RebootMachine;
    bare_admin.confirmed = true;
    check(encode_command(bare_admin) == Error::InvalidCombination,
          "bare reboot action is rejected");
    bare_admin.action = CommandAction::ActivateUpdate;
    check(encode_command(bare_admin) == Error::InvalidCombination,
          "bare activation action is rejected");
    Command forged_ordinary = command;
    forged_ordinary.admin_intent = reboot_intent;
    check(encode_command(forged_ordinary) == Error::InvalidCombination,
          "ordinary command cannot carry admin privilege data");
    Command unconfirmed_reboot = reboot_command;
    unconfirmed_reboot.confirmed = false;
    check(encode_command(unconfirmed_reboot) == Error::InvalidCombination,
          "signed reboot requires explicit confirmation");
    Command unconfirmed_activate = activate_command;
    unconfirmed_activate.confirmed = false;
    check(encode_command(unconfirmed_activate) == Error::InvalidCombination,
          "signed activation requires explicit confirmation");
    Command target_reboot = reboot_command;
    target_reboot.target_job_id = "job-1";
    check(encode_command(target_reboot) == Error::InvalidCombination,
          "signed reboot cannot target a stop job");

    Command wrong_operation = reboot_command;
    wrong_operation.admin_intent = activate_intent;
    check(encode_command(wrong_operation) == Error::InvalidCombination,
          "reboot rejects an activation intent");
    Command wrong_leader = reboot_command;
    wrong_leader.admin_intent->leader_epoch += 1;
    check(encode_command(wrong_leader) == Error::InvalidCombination,
          "admin intent epoch must match the command");
    Command mismatched_leader = reboot_command;
    mismatched_leader.leader_id = std::string(64, 'd');
    check(encode_command(mismatched_leader) == Error::InvalidCombination,
          "admin intent leader must match the command");
    Command wrong_identity = activate_command;
    wrong_identity.package_offer->worker_id = std::string(64, 'd');
    check(encode_command(wrong_identity) == Error::InvalidCombination,
          "activation offer worker must match its intent");
    wrong_identity = activate_command;
    wrong_identity.package_offer->leader_id = std::string(64, 'd');
    check(encode_command(wrong_identity) == Error::InvalidCombination,
          "activation offer leader must match its intent");
    wrong_identity = activate_command;
    wrong_identity.package_offer->update_id = "update-other";
    check(encode_command(wrong_identity) == Error::InvalidCombination,
          "activation offer update ID must match its intent");
    wrong_identity = activate_command;
    wrong_identity.package_offer->manifest.sha256 = std::string(64, 'd');
    check(encode_command(wrong_identity) == Error::InvalidCombination,
          "activation offer digest must match its intent");
    Command reboot_with_offer = reboot_command;
    reboot_with_offer.package_offer = package_offer;
    check(encode_command(reboot_with_offer) == Error::InvalidCombination,
          "reboot rejects an update offer");
    Command activation_without_offer = activate_command;
    activation_without_offer.package_offer.reset();
    check(encode_command(activation_without_offer) == Error::InvalidCombination,
          "activation requires a signed package offer");
    Command activation_without_intent = activate_command;
    activation_without_intent.admin_intent.reset();
    check(encode_command(activation_without_intent) == Error::InvalidCombination,
          "activation requires a signed admin intent");

    Command late_reboot = reboot_command;
    late_reboot.expires_at_ms = reboot_intent.expires_at_unix * 1000 + 1;
    check(encode_command(late_reboot) == Error::InvalidCombination,
          "admin command cannot outlive its signed intent");
    Command late_activation = activate_command;
    late_activation.expires_at_ms = package_offer.expires_at_unix * 1000 + 1;
    check(encode_command(late_activation) == Error::InvalidCombination,
          "activation command cannot outlive its package offer");
    Command long_admin_command = reboot_command;
    long_admin_command.issued_at_ms = now_ms - kMaxRestartCommandLifetimeMs;
    long_admin_command.expires_at_ms =
        long_admin_command.issued_at_ms + kMaxRestartCommandLifetimeMs + 1;
    check(encode_command(long_admin_command) == Error::InvalidCombination,
          "admin command lifetime is bounded to five minutes");

    Command oversized_pem = reboot_command;
    oversized_pem.admin_intent->signer_public_key_pem.assign(
        app::agent::wire::kMaxAdminPublicKeyPemBytes + 1, 'x');
    check(encode_command(oversized_pem) == Error::InvalidField,
          "admin public key text is bounded");
    Command oversized_signature = activate_command;
    oversized_signature.package_offer->signature.resize(
        app::agent::wire::kMaxAdminSignatureBytes + 1);
    check(encode_command(oversized_signature) == Error::InvalidField,
          "detached signature bytes are bounded");
    Command unsigned_reboot = reboot_command;
    unsigned_reboot.admin_intent->signature.clear();
    check(encode_command(unsigned_reboot) == Error::InvalidField,
          "admin action rejects an empty detached signature");
    Command unsafe_admin_time = reboot_command;
    unsafe_admin_time.admin_intent->issued_at_unix =
        9'007'199'254'740'991ULL / 1000 + 1;
    check(encode_command(unsafe_admin_time) == Error::NumericOverflow,
          "admin seconds cannot overflow millisecond conversion");
    Command oversized_package = activate_command;
    oversized_package.package_offer->manifest.size =
        app::agent::update::kMaxPackageBytes + 1;
    check(encode_command(oversized_package) == Error::InvalidField,
          "package offer carries no package bytes and bounds its size");

    std::string unknown_intent_field = reboot_json;
    check(replace_once(unknown_intent_field,
                       "\"intent_id\": \"reboot-1\",",
                       "\"intent_id\": \"reboot-1\",\n        \"unexpected\": true,") &&
              rejects(unknown_intent_field, now_ms) == Error::UnknownField,
          "unknown nested admin fields are rejected");
    std::string duplicate_intent_field = reboot_json;
    check(replace_once(duplicate_intent_field,
                       "\"intent_id\": \"reboot-1\",",
                       "\"intent_id\": \"reboot-1\",\n        \"intent_id\": \"reboot-1\",") &&
              rejects(duplicate_intent_field, now_ms) == Error::DuplicateField,
          "duplicate nested admin fields are rejected");
    std::string malformed_signature = reboot_json;
    check(replace_once(malformed_signature, "3006020100020101",
                       "300602010002010g") &&
              rejects(malformed_signature, now_ms) == Error::InvalidField,
          "non-hex detached signature data is rejected");
    std::string tampered_activation = activate_json;
    check(replace_once(tampered_activation,
                       "\"package_sha256\": \"" + package_digest + "\"",
                       "\"package_sha256\": \"" + std::string(64, 'd') + "\"") &&
              rejects(tampered_activation, now_ms) == Error::InvalidCombination,
          "tampered signed package digest cannot match its offer");
    std::string unknown_manifest_field = activate_json;
    check(replace_once(unknown_manifest_field,
                       "\"release\": \"2026.09.23\",",
                       "\"release\": \"2026.09.23\",\n            \"extra\": 1,") &&
              rejects(unknown_manifest_field, now_ms) == Error::UnknownField,
          "unknown nested package manifest fields are rejected");
    std::string duplicate_manifest_field = activate_json;
    check(replace_once(duplicate_manifest_field,
                       "\"sha256\": \"" + package_digest + "\"",
                       "\"sha256\": \"" + package_digest +
                           "\",\n                \"sha256\": \"" +
                           package_digest + "\"") &&
              rejects(duplicate_manifest_field, now_ms) == Error::DuplicateField,
          "duplicate package manifest fields are rejected");

    std::string bare_reboot_json = ordinary_command_json;
    check(replace_once(bare_reboot_json, "\"action\": \"pause\"",
                       "\"action\": \"reboot_machine\"") &&
              replace_once(bare_reboot_json, "\"confirmed\": false",
                           "\"confirmed\": true") &&
              rejects(bare_reboot_json, now_ms) == Error::InvalidField,
          "wire parser requires nested intent for reboot");
    std::string bare_activate_json = ordinary_command_json;
    check(replace_once(bare_activate_json, "\"action\": \"pause\"",
                       "\"action\": \"activate_update\"") &&
              replace_once(bare_activate_json, "\"confirmed\": false",
                           "\"confirmed\": true") &&
              rejects(bare_activate_json, now_ms) == Error::InvalidField,
          "wire parser requires intent and offer for activation");



    Acknowledgment acknowledgment;
    acknowledgment.command_id = command.command_id;
    acknowledgment.leader_id = command.leader_id;
    acknowledgment.leader_epoch = command.leader_epoch;
    acknowledgment.sequence = command.sequence;
    acknowledgment.action = CommandAction::Online;
    acknowledgment.outcome = AcknowledgmentOutcome::Completed;
    Message acknowledgment_message;
    acknowledgment_message.payload = acknowledgment;
    check(Encode(acknowledgment_message, now_ms, json) == Error::None,
          "acknowledgment encodes");
    Message decoded_acknowledgment;
    check(Decode(json, now_ms, decoded_acknowledgment) == Error::None,
          "acknowledgment decodes");
    const auto* roundtrip_acknowledgment =
        std::get_if<Acknowledgment>(&decoded_acknowledgment.payload);
    check(roundtrip_acknowledgment &&
              roundtrip_acknowledgment->command_id == acknowledgment.command_id &&
              roundtrip_acknowledgment->leader_id == acknowledgment.leader_id &&
              roundtrip_acknowledgment->leader_epoch == acknowledgment.leader_epoch &&
              roundtrip_acknowledgment->sequence == acknowledgment.sequence &&
              roundtrip_acknowledgment->action == acknowledgment.action &&
              roundtrip_acknowledgment->outcome == acknowledgment.outcome,
          "acknowledgment echoes identity and outcome");
    acknowledgment.outcome = AcknowledgmentOutcome::Accepted;
    acknowledgment_message.payload = acknowledgment;
    check(Encode(acknowledgment_message, now_ms, json) == Error::None &&
              Decode(json, now_ms, decoded_acknowledgment) == Error::None &&
              std::get_if<Acknowledgment>(&decoded_acknowledgment.payload) &&
              std::get<Acknowledgment>(decoded_acknowledgment.payload).outcome ==
                  AcknowledgmentOutcome::Accepted,
          "asynchronous command acceptance roundtrips distinctly");

    FeatureOffer offer;
    offer.job_id = "job-17";
    offer.attempt_id = "attempt.2";
    offer.plan_digest = std::string(64, 'a');
    offer.request_digest = std::string(64, 'b');
    offer.required_build = "spirula-1.2.3";
    offer.expires_at_ms = now_ms + 60'000;
    offer.inputs = {
        {"images/frame-001.jpg", 1024, std::string(64, 'c')},
        {"features/index.bin", 4096, std::string(64, 'd')},
    };
    Message offer_message;
    offer_message.payload = offer;
    check(Encode(offer_message, now_ms, json) == Error::None,
          "feature offer encodes");
    const std::string offer_json = json;
    Message decoded_offer;
    check(Decode(offer_json, now_ms, decoded_offer) == Error::None,
          "feature offer decodes");
    const auto* roundtrip_offer =
        std::get_if<FeatureOffer>(&decoded_offer.payload);
    check(roundtrip_offer &&
              roundtrip_offer->job_id == offer.job_id &&
              roundtrip_offer->attempt_id == offer.attempt_id &&
              roundtrip_offer->plan_digest == offer.plan_digest &&
              roundtrip_offer->request_digest == offer.request_digest &&
              roundtrip_offer->required_build == offer.required_build &&
              roundtrip_offer->expires_at_ms == offer.expires_at_ms &&
              roundtrip_offer->inputs.size() == offer.inputs.size() &&
              roundtrip_offer->inputs[0].path == offer.inputs[0].path &&
              roundtrip_offer->inputs[0].size == offer.inputs[0].size &&
              roundtrip_offer->inputs[0].sha256 == offer.inputs[0].sha256 &&
              roundtrip_offer->inputs[1].path == offer.inputs[1].path &&
              roundtrip_offer->inputs[1].size == offer.inputs[1].size &&
              roundtrip_offer->inputs[1].sha256 == offer.inputs[1].sha256,
          "feature offer roundtrips assignment and input manifest");
    check(rejects(offer_json, offer.expires_at_ms) == Error::ExpiredFeatureOffer,
          "expired feature offers are rejected");
    FeatureOffer stale_offer = offer;
    stale_offer.expires_at_ms = now_ms;
    Message stale_offer_message;
    stale_offer_message.payload = stale_offer;
    check(Encode(stale_offer_message, now_ms, json) ==
              Error::ExpiredFeatureOffer,
          "encoding a stale offer is rejected");

    std::string escaped_path = offer_json;
    check(replace_once(escaped_path, "images/frame-001.jpg", "../outside.jpg") &&
              rejects(escaped_path, now_ms) == Error::InvalidField,
          "manifest traversal paths are rejected");
    std::string reserved_path = offer_json;
    check(replace_once(reserved_path, "images/frame-001.jpg", "images/CON.txt") &&
              rejects(reserved_path, now_ms) == Error::InvalidField,
          "reserved Windows manifest names are rejected");
    std::string drive_path = offer_json;
    check(replace_once(drive_path, "images/frame-001.jpg", "C:/worker/frame.jpg") &&
              rejects(drive_path, now_ms) == Error::InvalidField,
          "drive-qualified manifest paths are rejected");
    std::string backslash_path = offer_json;
    check(replace_once(backslash_path, "images/frame-001.jpg",
                       "images\\\\outside.jpg") &&
              rejects(backslash_path, now_ms) == Error::InvalidField,
          "backslash manifest paths are rejected");
    std::string folded_collision = offer_json;
    check(replace_once(folded_collision, "features/index.bin",
                       "IMAGES/FRAME-001.JPG") &&
              rejects(folded_collision, now_ms) == Error::InvalidField,
          "case-folded manifest path collisions are rejected");
    std::string nested_duplicate = offer_json;
    const std::size_t digest_key = nested_duplicate.find("\"sha256\"");
    check(digest_key != std::string::npos,
          "feature manifest has a digest field");
    if (digest_key != std::string::npos)
        nested_duplicate.insert(
            digest_key, "\"path\": \"images/frame-001.jpg\",\n            ");
    check(rejects(nested_duplicate, now_ms) == Error::DuplicateField,
          "duplicate manifest fields are rejected");
    std::string unknown_feature_field = offer_json;
    const std::size_t offer_end = unknown_feature_field.rfind('}');
    check(offer_end != std::string::npos,
          "feature offer JSON has an object terminator");
    if (offer_end != std::string::npos)
        unknown_feature_field.insert(offer_end, ",\"argv\": []");
    check(rejects(unknown_feature_field, now_ms) == Error::UnknownField,
          "unknown feature fields are rejected");
    std::string unsafe_size = offer_json;
    check(replace_once(unsafe_size, "\"size\": 1024",
                       "\"size\": 9007199254740992") &&
              rejects(unsafe_size, now_ms) == Error::NumericOverflow,
          "manifest sizes above the safe integer limit are rejected");
    std::string unsafe_total = offer_json;
    check(replace_once(unsafe_total, "\"size\": 1024",
                       "\"size\": 9007199254740991") &&
              rejects(unsafe_total, now_ms) == Error::NumericOverflow,
          "manifest totals above the safe integer limit are rejected");

    const FeatureDecision::Step feature_steps[] = {
        FeatureDecision::Step::Offer, FeatureDecision::Step::Inputs,
        FeatureDecision::Step::Output,
    };
    const FeatureDecision::Decision feature_decisions[] = {
        FeatureDecision::Decision::Accepted, FeatureDecision::Decision::Rejected,
        FeatureDecision::Decision::Committed,
    };
    for (std::size_t i = 0; i < 3; ++i) {
        FeatureDecision feature_decision;
        feature_decision.job_id = offer.job_id;
        feature_decision.attempt_id = offer.attempt_id;
        feature_decision.step = feature_steps[i];
        feature_decision.decision = feature_decisions[i];
        feature_decision.reason =
            feature_decision.decision == FeatureDecision::Decision::Rejected
                ? "input verification failed"
                : "";
        Message feature_decision_message;
        feature_decision_message.payload = feature_decision;
        Message decoded_feature_decision;
        const bool roundtrips =
            Encode(feature_decision_message, now_ms, json) == Error::None &&
            Decode(json, now_ms, decoded_feature_decision) == Error::None;
        const auto* roundtrip_decision =
            std::get_if<FeatureDecision>(&decoded_feature_decision.payload);
        check(roundtrips && roundtrip_decision &&
                  roundtrip_decision->job_id == feature_decision.job_id &&
                  roundtrip_decision->attempt_id == feature_decision.attempt_id &&
                  roundtrip_decision->step == feature_decision.step &&
                  roundtrip_decision->decision == feature_decision.decision &&
                  roundtrip_decision->reason == feature_decision.reason,
              "feature decisions roundtrip every typed step and outcome");
    }
    FeatureDecision invalid_commit;
    invalid_commit.job_id = offer.job_id;
    invalid_commit.attempt_id = offer.attempt_id;
    invalid_commit.step = FeatureDecision::Step::Inputs;
    invalid_commit.decision = FeatureDecision::Decision::Committed;
    Message invalid_commit_message;
    invalid_commit_message.payload = invalid_commit;
    check(Encode(invalid_commit_message, now_ms, json) ==
              Error::InvalidCombination,
          "only output decisions may be committed");

    const FeatureResult::Outcome feature_outcomes[] = {
        FeatureResult::Outcome::Succeeded, FeatureResult::Outcome::Failed,
        FeatureResult::Outcome::Interrupted,
    };
    for (FeatureResult::Outcome outcome : feature_outcomes) {
        FeatureResult feature_result;
        feature_result.job_id = offer.job_id;
        feature_result.attempt_id = offer.attempt_id;
        feature_result.outcome = outcome;
        feature_result.error = outcome == FeatureResult::Outcome::Succeeded
            ? ""
            : "worker did not complete the attempt";
        if (outcome == FeatureResult::Outcome::Succeeded)
            feature_result.outputs = {
                {"features/result.bin", 8192, std::string(64, 'e')},
            };
        Message feature_result_message;
        feature_result_message.payload = feature_result;
        Message decoded_feature_result;
        const bool roundtrips =
            Encode(feature_result_message, now_ms, json) == Error::None &&
            Decode(json, now_ms, decoded_feature_result) == Error::None;
        const auto* roundtrip_result =
            std::get_if<FeatureResult>(&decoded_feature_result.payload);
        check(roundtrips && roundtrip_result &&
                  roundtrip_result->job_id == feature_result.job_id &&
                  roundtrip_result->attempt_id == feature_result.attempt_id &&
                  roundtrip_result->outcome == feature_result.outcome &&
                  roundtrip_result->error == feature_result.error &&
                  roundtrip_result->outputs.size() == feature_result.outputs.size() &&
                  (feature_result.outputs.empty() ||
                   (roundtrip_result->outputs[0].path ==
                        feature_result.outputs[0].path &&
                    roundtrip_result->outputs[0].size ==
                        feature_result.outputs[0].size &&
                    roundtrip_result->outputs[0].sha256 ==
                        feature_result.outputs[0].sha256)),
              "feature results roundtrip outcomes and output manifests");
    }
    FeatureResult success_without_outputs;
    success_without_outputs.job_id = offer.job_id;
    success_without_outputs.attempt_id = offer.attempt_id;
    Message success_without_outputs_message;
    success_without_outputs_message.payload = success_without_outputs;
    check(Encode(success_without_outputs_message, now_ms, json) ==
              Error::InvalidCombination,
          "successful feature results require outputs");
    FeatureResult empty_failure;
    empty_failure.job_id = offer.job_id;
    empty_failure.attempt_id = offer.attempt_id;
    empty_failure.outcome = FeatureResult::Outcome::Failed;
    empty_failure.error = "worker failed";
    Message empty_failure_message;
    empty_failure_message.payload = empty_failure;
    check(Encode(empty_failure_message, now_ms, json) == Error::None,
          "failed results without outputs are valid");
    std::string forged_success = json;
    check(replace_once(forged_success, "\"outcome\": \"failed\"",
                       "\"outcome\": \"succeeded\"") &&
              rejects(forged_success, now_ms) == Error::InvalidCombination,
          "decoded successful results require outputs");

    FeatureResult success_with_outputs;
    success_with_outputs.job_id = offer.job_id;
    success_with_outputs.attempt_id = offer.attempt_id;
    success_with_outputs.outputs = {
        {"features/result.bin", 8192, std::string(64, 'e')},
    };
    Message success_with_outputs_message;
    success_with_outputs_message.payload = success_with_outputs;
    check(Encode(success_with_outputs_message, now_ms, json) == Error::None,
          "successful result with outputs is valid");
    const FeatureResult::Outcome unsuccessful_outcomes[] = {
        FeatureResult::Outcome::Failed, FeatureResult::Outcome::Interrupted,
    };
    for (FeatureResult::Outcome outcome : unsuccessful_outcomes) {
        FeatureResult invalid_result = success_with_outputs;
        invalid_result.outcome = outcome;
        Message invalid_result_message;
        invalid_result_message.payload = invalid_result;
        check(Encode(invalid_result_message, now_ms, json) ==
                  Error::InvalidCombination,
              "failed and interrupted results cannot carry outputs");
    }
    std::string forged_failure = json;
    check(replace_once(forged_failure, "\"outcome\": \"succeeded\"",
                       "\"outcome\": \"failed\"") &&
              rejects(forged_failure, now_ms) == Error::InvalidCombination,
          "decoded failed results cannot carry outputs");
    std::string forged_interruption = json;
    check(replace_once(forged_interruption, "\"outcome\": \"succeeded\"",
                       "\"outcome\": \"interrupted\"") &&
              rejects(forged_interruption, now_ms) == Error::InvalidCombination,
          "decoded interrupted results cannot carry outputs");

    PortableOffer training_offer;
    std::string training_offer_json;
    std::string training_decision_json;
    std::string training_result_json;
    PortableResult training_result;
    const PortableWorkload workloads[] = {
        PortableWorkload::Reconstruction, PortableWorkload::Training,
    };
    for (PortableWorkload workload : workloads) {
        PortableOffer portable_offer;
        portable_offer.workload = workload;
        portable_offer.job_id = "portable-job";
        portable_offer.attempt_id = "portable-attempt";
        portable_offer.input_identity_sha256 = std::string(64, 'f');
        portable_offer.required_build = "spirula-1.2.3";
        portable_offer.expires_at_ms = now_ms + 60'000;
        portable_offer.inputs = {
            {"input/data.bin", 128, std::string(64, 'a')},
        };
        Message portable_offer_message;
        portable_offer_message.payload = portable_offer;
        std::string portable_offer_json;
        Message decoded_portable_offer;
        const bool offer_roundtrips =
            Encode(portable_offer_message, now_ms, portable_offer_json) ==
                Error::None &&
            Decode(portable_offer_json, now_ms, decoded_portable_offer) ==
                Error::None;
        const auto* roundtrip_portable_offer =
            std::get_if<PortableOffer>(&decoded_portable_offer.payload);
        check(offer_roundtrips && roundtrip_portable_offer &&
                  roundtrip_portable_offer->workload == workload &&
                  roundtrip_portable_offer->job_id == portable_offer.job_id &&
                  roundtrip_portable_offer->attempt_id == portable_offer.attempt_id &&
                  roundtrip_portable_offer->input_identity_sha256 ==
                      portable_offer.input_identity_sha256 &&
                  roundtrip_portable_offer->required_build ==
                      portable_offer.required_build &&
                  roundtrip_portable_offer->expires_at_ms ==
                      portable_offer.expires_at_ms &&
                  roundtrip_portable_offer->inputs.size() == 1 &&
                  roundtrip_portable_offer->inputs[0].path ==
                      portable_offer.inputs[0].path &&
                  roundtrip_portable_offer->inputs[0].size ==
                      portable_offer.inputs[0].size &&
                  roundtrip_portable_offer->inputs[0].sha256 ==
                      portable_offer.inputs[0].sha256,
              workload == PortableWorkload::Training
                  ? "training offers roundtrip with bound input identity"
                  : "reconstruction offers roundtrip with bound input identity");
        if (workload == PortableWorkload::Training) {
            training_offer = portable_offer;
            training_offer_json = portable_offer_json;
        }

        PortableDecision portable_decision;
        portable_decision.workload = workload;
        portable_decision.job_id = portable_offer.job_id;
        portable_decision.attempt_id = portable_offer.attempt_id;
        portable_decision.step = workload == PortableWorkload::Training
            ? PortableDecision::Step::Output
            : PortableDecision::Step::Inputs;
        portable_decision.decision = workload == PortableWorkload::Training
            ? PortableDecision::Decision::Committed
            : PortableDecision::Decision::Accepted;
        Message portable_decision_message;
        portable_decision_message.payload = portable_decision;
        Message decoded_portable_decision;
        const bool decision_roundtrips =
            Encode(portable_decision_message, now_ms, portable_offer_json) ==
                Error::None &&
            Decode(portable_offer_json, now_ms, decoded_portable_decision) ==
                Error::None;
        const auto* roundtrip_portable_decision =
            std::get_if<PortableDecision>(&decoded_portable_decision.payload);
        check(decision_roundtrips && roundtrip_portable_decision &&
                  roundtrip_portable_decision->workload == workload &&
                  roundtrip_portable_decision->job_id == portable_decision.job_id &&
                  roundtrip_portable_decision->attempt_id ==
                      portable_decision.attempt_id &&
                  roundtrip_portable_decision->step == portable_decision.step &&
                  roundtrip_portable_decision->decision ==
                      portable_decision.decision,
              workload == PortableWorkload::Training
                  ? "training output decision roundtrips"
                  : "reconstruction input decision roundtrips");
        if (workload == PortableWorkload::Training)
            training_decision_json = portable_offer_json;

        PortableResult portable_result;
        portable_result.workload = workload;
        portable_result.job_id = portable_offer.job_id;
        portable_result.attempt_id = portable_offer.attempt_id;
        portable_result.outputs = {
            {"output/result.bin", 256, std::string(64, 'b')},
        };
        if (workload == PortableWorkload::Training)
            portable_result.output_metadata = "step-000000042.ckpt";
        Message portable_result_message;
        portable_result_message.payload = portable_result;
        std::string portable_result_json;
        Message decoded_portable_result;
        const bool result_roundtrips =
            Encode(portable_result_message, now_ms, portable_result_json) ==
                Error::None &&
            Decode(portable_result_json, now_ms, decoded_portable_result) ==
                Error::None;
        const auto* roundtrip_portable_result =
            std::get_if<PortableResult>(&decoded_portable_result.payload);
        check(result_roundtrips && roundtrip_portable_result &&
                  roundtrip_portable_result->workload == workload &&
                  roundtrip_portable_result->job_id == portable_result.job_id &&
                  roundtrip_portable_result->attempt_id ==
                      portable_result.attempt_id &&
                  roundtrip_portable_result->outputs.size() == 1 &&
                  roundtrip_portable_result->outputs[0].path ==
                      portable_result.outputs[0].path &&
                  roundtrip_portable_result->output_metadata ==
                      portable_result.output_metadata,
              workload == PortableWorkload::Training
                  ? "training results roundtrip exact checkpoint basename"
                  : "reconstruction results roundtrip without metadata");
        if (workload == PortableWorkload::Training) {
            training_result = portable_result;
            training_result_json = portable_result_json;
        }
    }

    std::string malformed_identity = training_offer_json;
    check(replace_once(malformed_identity, std::string(64, 'f'),
                       std::string(64, 'F')) &&
              rejects(malformed_identity, now_ms) == Error::InvalidField,
          "portable input identity requires lowercase SHA-256");
    std::string malformed_portable_id = training_offer_json;
    check(replace_once(malformed_portable_id, "portable-job", "../job") &&
              rejects(malformed_portable_id, now_ms) == Error::InvalidField,
          "portable job identifiers reject path syntax");
    std::string portable_unknown_field = training_offer_json;
    const std::size_t portable_offer_end = portable_unknown_field.rfind('}');
    if (portable_offer_end != std::string::npos)
        portable_unknown_field.insert(portable_offer_end, ",\"argv\":[]");
    check(portable_offer_end != std::string::npos &&
              rejects(portable_unknown_field, now_ms) == Error::UnknownField,
          "portable offers reject arbitrary command fields");
    std::string forged_portable_commit = training_decision_json;
    check(replace_once(forged_portable_commit, "\"output\"", "\"inputs\"") &&
              rejects(forged_portable_commit, now_ms) ==
                  Error::InvalidCombination,
          "decoded portable decisions reject commits before the output step");
    std::string malformed_portable_enum = training_decision_json;
    check(replace_once(malformed_portable_enum, "\"training\"", "\"unknown\"") &&
              rejects(malformed_portable_enum, now_ms) == Error::InvalidField,
          "decoded portable decisions reject unknown workload enums");

    PortableOffer far_expiry = training_offer;
    far_expiry.expires_at_ms =
        now_ms + app::agent::wire::kMaxPortableOfferLifetimeMs + 1;
    Message far_expiry_message;
    far_expiry_message.payload = far_expiry;
    check(Encode(far_expiry_message, now_ms, json) == Error::InvalidTime,
          "portable offer expiry is limited to a finite window");
    PortableOffer expired_portable_offer = training_offer;
    expired_portable_offer.expires_at_ms = now_ms;
    Message expired_portable_offer_message;
    expired_portable_offer_message.payload = expired_portable_offer;
    check(Encode(expired_portable_offer_message, now_ms, json) ==
              Error::ExpiredPortableOffer,
          "expired portable offers are rejected");

    PortableDecision invalid_portable_commit;
    invalid_portable_commit.workload = PortableWorkload::Training;
    invalid_portable_commit.job_id = "portable-job";
    invalid_portable_commit.attempt_id = "portable-attempt";
    invalid_portable_commit.step = PortableDecision::Step::Inputs;
    invalid_portable_commit.decision = PortableDecision::Decision::Committed;
    Message invalid_portable_commit_message;
    invalid_portable_commit_message.payload = invalid_portable_commit;
    check(Encode(invalid_portable_commit_message, now_ms, json) ==
              Error::InvalidCombination,
          "portable commits are only valid for the output step");

    PortableResult invalid_checkpoint = training_result;
    invalid_checkpoint.output_metadata = "../step-000000042.ckpt";
    Message invalid_checkpoint_message;
    invalid_checkpoint_message.payload = invalid_checkpoint;
    check(Encode(invalid_checkpoint_message, now_ms, json) == Error::InvalidField,
          "training metadata rejects checkpoint paths");
    std::string forged_checkpoint = training_result_json;
    check(replace_once(forged_checkpoint, "step-000000042.ckpt",
                       "step-000000042.ckpt/") &&
              rejects(forged_checkpoint, now_ms) == Error::InvalidField,
          "decoded training metadata rejects non-basename values");
    PortableResult reconstruction_metadata = invalid_checkpoint;
    reconstruction_metadata.workload = PortableWorkload::Reconstruction;
    reconstruction_metadata.output_metadata = "step-000000042.ckpt";
    Message reconstruction_metadata_message;
    reconstruction_metadata_message.payload = reconstruction_metadata;
    check(Encode(reconstruction_metadata_message, now_ms, json) ==
              Error::InvalidCombination,
          "reconstruction results reject output metadata");
    PortableResult failed_training;
    failed_training.workload = PortableWorkload::Training;
    failed_training.job_id = "portable-job";
    failed_training.attempt_id = "portable-attempt";
    failed_training.outcome = PortableResult::Outcome::Failed;
    failed_training.outputs = {{"output/result.bin", 256, std::string(64, 'b')}};
    Message failed_training_message;
    failed_training_message.payload = failed_training;
    check(Encode(failed_training_message, now_ms, json) ==
              Error::InvalidCombination,
          "failed portable results cannot carry outputs");
    failed_training.outputs.clear();
    failed_training.error = std::string(
        app::agent::wire::kMaxPortableTextBytes + 1, 'e');
    failed_training_message.payload = failed_training;
    check(Encode(failed_training_message, now_ms, json) == Error::InvalidField,
          "portable result error text is bounded");

    check(rejects("{", now_ms) == Error::MalformedJson,
          "malformed JSON is rejected");
    check(rejects("{\"schema_version\":01,\"type\":\"status\"}", now_ms) ==
              Error::MalformedJson,
          "non-JSON number spelling is rejected");
    check(rejects("{\"schema_version\":4,\"type\":\"status\"}", now_ms) ==
              Error::UnsupportedVersion,
          "previous wire schema version is rejected");
    check(rejects("{\"schema_version\":6,\"type\":\"status\"}", now_ms) ==
              Error::UnsupportedVersion,
          "unsupported version is rejected");
    check(rejects("{\"schema_version\":2,\"schema_version\":2}", now_ms) ==
              Error::DuplicateField,
          "duplicate fields are rejected");

    const std::string command_prefix =
        "{\"schema_version\":5,\"type\":\"command\",\"command_id\":\"command-1\","
        "\"leader_id\":\"leader-1\",\"leader_epoch\":1,\"sequence\":1,"
        "\"action\":\"stop\",\"target_job_id\":\"\",\"confirmed\":true,"
        "\"issued_at_ms\":";
    check(rejects(command_prefix + "1699999999000,\"expires_at_ms\":1700000000000}",
                  now_ms) == Error::ExpiredCommand,
          "expired commands are rejected");
    check(rejects(command_prefix + "1700000000001,\"expires_at_ms\":1700000001000}",
                  now_ms) == Error::InvalidTime,
          "future-issued commands are rejected");
    check(rejects(command_prefix + "1699999999000,\"expires_at_ms\":9007199254740992}",
                  now_ms) == Error::NumericOverflow,
          "unsafe timestamp overflow is rejected");
    check(rejects(command_prefix + "1699999999000,\"expires_at_ms\":1700000001000,"
                  "\"path\":\"C:\\\\worker.exe\"}", now_ms) == Error::UnknownField,
          "path injection fields are rejected");
    check(rejects("{\"schema_version\":5,\"type\":\"command\","
                  "\"command_id\":\"command-1\",\"leader_id\":\"leader-1\","
                  "\"leader_epoch\":1,\"sequence\":1,\"action\":\"exec\","
                  "\"target_job_id\":\"\",\"confirmed\":false,"
                  "\"issued_at_ms\":1699999999000,\"expires_at_ms\":1700000001000}",
                  now_ms) == Error::InvalidField,
          "unlisted execution actions are rejected");
    check(rejects(command_prefix + "1699999999000,\"expires_at_ms\":1700000001000,"
                  "\"command\":\"pause; rm -rf /\"}", now_ms) == Error::UnknownField,
          "free-form command injection is rejected");
    check(rejects("{\"schema_version\":3,\"type\":\"command\","
                  "\"command_id\":\"command-1\",\"leader_id\":\"leader-1\","
                  "\"leader_epoch\":1,\"sequence\":1,\"action\":\"stop\","
                  "\"issued_at_ms\":1699999999000,\"expires_at_ms\":1700000001000}",
                  now_ms) == Error::UnsupportedVersion,
          "old stop commands cannot be interpreted without confirmation");
    Command unsafe_stop = command;
    unsafe_stop.action = CommandAction::Stop;
    unsafe_stop.confirmed = true;
    unsafe_stop.target_job_id = "../other";
    Message unsafe_stop_message;
    unsafe_stop_message.payload = unsafe_stop;
    check(Encode(unsafe_stop_message, now_ms, json) == Error::InvalidField,
          "target job IDs reject path syntax");
    Command unconfirmed_stop = unsafe_stop;
    unconfirmed_stop.target_job_id.clear();
    unconfirmed_stop.confirmed = false;
    Message unconfirmed_stop_message;
    unconfirmed_stop_message.payload = unconfirmed_stop;
    check(Encode(unconfirmed_stop_message, now_ms, json) ==
              Error::InvalidCombination,
          "Stop requires explicit confirmation");
    Command confirmed_pause = command;
    confirmed_pause.confirmed = true;
    Message confirmed_pause_message;
    confirmed_pause_message.payload = confirmed_pause;
    check(Encode(confirmed_pause_message, now_ms, json) ==
              Error::InvalidCombination,
          "Pause rejects unexpected confirmation");
    Command confirmed_stop = command;
    confirmed_stop.action = CommandAction::Stop;
    confirmed_stop.target_job_id = "job-42";
    confirmed_stop.confirmed = true;
    Message confirmed_stop_message;
    confirmed_stop_message.payload = confirmed_stop;
    check(Encode(confirmed_stop_message, now_ms, json) == Error::None &&
              Decode(json, now_ms, decoded_command) == Error::None &&
              std::get_if<Command>(&decoded_command.payload) &&
              std::get<Command>(decoded_command.payload).target_job_id == "job-42" &&
              std::get<Command>(decoded_command.payload).confirmed,
          "confirmed targeted Stop roundtrips its bound job");

    status.maintenance = true;
    status_message.payload = status;
    check(Encode(status_message, now_ms, json) == Error::InvalidCombination,
          "maintenance cannot advertise accepting scheduling");
    status.maintenance = false;
    status.capabilities.push_back(Capability::Training);
    status_message.payload = status;
    check(Encode(status_message, now_ms, json) == Error::InvalidField,
          "duplicate capabilities are rejected");

    check(rejects(std::string(app::agent::wire::kMaxMessageBytes + 1, ' '), now_ms) ==
              Error::MessageTooLarge,
          "messages above the frame limit are rejected");

    if (failures) {
        std::printf("%d wire checks failed\n", failures);
        return 1;
    }
    std::printf("All agent wire checks passed.\n");
    return 0;
}
