#include "app/AgentWire.h"

#include "data/Json.h"
#include "data/JsonWrite.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace app::agent::wire {
namespace {

constexpr std::uint64_t kMaxSafeInteger = 9'007'199'254'740'991ULL;
constexpr std::uint64_t kMaxWireUnixSeconds = kMaxSafeInteger / 1000;
constexpr std::uint64_t kMaxAdminIntentLifetimeSeconds = 5 * 60;

template <typename E>
struct Token {
    const char* text;
    E value;
};

constexpr Token<ConnectionState> kConnectionTokens[] = {
    {"connected", ConnectionState::Connected},
    {"reconnecting", ConnectionState::Reconnecting},
    {"disconnected", ConnectionState::Disconnected},
};
constexpr Token<CompatibilityState> kCompatibilityTokens[] = {
    {"unknown", CompatibilityState::Unknown},
    {"compatible", CompatibilityState::Compatible},
    {"incompatible", CompatibilityState::Incompatible},
};
constexpr Token<SchedulingState> kSchedulingTokens[] = {
    {"accepting", SchedulingState::Accepting},
    {"pausing", SchedulingState::Pausing},
    {"paused", SchedulingState::Paused},
    {"stopped", SchedulingState::Stopped},
};
constexpr Token<ActivityState> kActivityTokens[] = {
    {"idle", ActivityState::Idle},
    {"feature", ActivityState::Feature},
    {"reconstruction", ActivityState::Reconstruction},
    {"training", ActivityState::Training},
};
constexpr Token<HealthState> kHealthTokens[] = {
    {"healthy", HealthState::Healthy},
    {"degraded", HealthState::Degraded},
    {"unhealthy", HealthState::Unhealthy},
};
constexpr Token<Capability> kCapabilityTokens[] = {
    {"feature", Capability::Feature},
    {"reconstruction", Capability::Reconstruction},
    {"training", Capability::Training},
};
constexpr Token<CommandAction> kActionTokens[] = {
    {"pause", CommandAction::Pause},
    {"resume", CommandAction::Resume},
    {"stop", CommandAction::Stop},
    {"maintenance", CommandAction::Maintenance},
    {"online", CommandAction::Online},
    {"restart_service", CommandAction::RestartService},
    {"force_restart_service", CommandAction::ForceRestartService},
    {"reboot_machine", CommandAction::RebootMachine},
    {"activate_update", CommandAction::ActivateUpdate},
};
constexpr Token<admin::Operation> kAdminOperationTokens[] = {
    {"reboot", admin::Operation::Reboot},
    {"activate", admin::Operation::Activate},
};
constexpr Token<AcknowledgmentOutcome> kOutcomeTokens[] = {
    {"accepted", AcknowledgmentOutcome::Accepted},
    {"rejected", AcknowledgmentOutcome::Rejected},
    {"completed", AcknowledgmentOutcome::Completed},
    {"failed", AcknowledgmentOutcome::Failed},
};

constexpr Token<FeatureDecision::Step> kFeatureStepTokens[] = {
    {"offer", FeatureDecision::Step::Offer},
    {"inputs", FeatureDecision::Step::Inputs},
    {"output", FeatureDecision::Step::Output},
};
constexpr Token<FeatureDecision::Decision> kFeatureDecisionTokens[] = {
    {"accepted", FeatureDecision::Decision::Accepted},
    {"rejected", FeatureDecision::Decision::Rejected},
    {"committed", FeatureDecision::Decision::Committed},
};
constexpr Token<FeatureResult::Outcome> kFeatureOutcomeTokens[] = {
    {"succeeded", FeatureResult::Outcome::Succeeded},
    {"failed", FeatureResult::Outcome::Failed},
    {"interrupted", FeatureResult::Outcome::Interrupted},
};
constexpr Token<PortableWorkload> kPortableWorkloadTokens[] = {
    {"reconstruction", PortableWorkload::Reconstruction},
    {"training", PortableWorkload::Training},
};
constexpr Token<PortableDecision::Step> kPortableStepTokens[] = {
    {"offer", PortableDecision::Step::Offer},
    {"inputs", PortableDecision::Step::Inputs},
    {"output", PortableDecision::Step::Output},
};
constexpr Token<PortableDecision::Decision> kPortableDecisionTokens[] = {
    {"accepted", PortableDecision::Decision::Accepted},
    {"rejected", PortableDecision::Decision::Rejected},
    {"committed", PortableDecision::Decision::Committed},
};
constexpr Token<PortableResult::Outcome> kPortableOutcomeTokens[] = {
    {"succeeded", PortableResult::Outcome::Succeeded},
    {"failed", PortableResult::Outcome::Failed},
    {"interrupted", PortableResult::Outcome::Interrupted},
};


struct Failure {
    Error error;
};

[[noreturn]] void fail(Error error) { throw Failure{error}; }

bool next_codepoint(const std::string& text, std::size_t& at,
                    std::uint32_t& codepoint) noexcept {
    if (at >= text.size()) return false;
    const auto first = static_cast<unsigned char>(text[at++]);
    if (first < 0x80) {
        codepoint = first;
        return true;
    }

    unsigned continuation_count = 0;
    std::uint32_t minimum = 0;
    if (first >= 0xC2 && first <= 0xDF) {
        codepoint = first & 0x1F;
        continuation_count = 1;
        minimum = 0x80;
    } else if (first >= 0xE0 && first <= 0xEF) {
        codepoint = first & 0x0F;
        continuation_count = 2;
        minimum = 0x800;
    } else if (first >= 0xF0 && first <= 0xF4) {
        codepoint = first & 0x07;
        continuation_count = 3;
        minimum = 0x10000;
    } else {
        return false;
    }

    if (text.size() - at < continuation_count) return false;
    for (unsigned i = 0; i < continuation_count; ++i) {
        const auto next = static_cast<unsigned char>(text[at++]);
        if ((next & 0xC0) != 0x80) return false;
        codepoint = (codepoint << 6) | (next & 0x3F);
    }
    return codepoint >= minimum && codepoint <= 0x10FFFF &&
           !(codepoint >= 0xD800 && codepoint <= 0xDFFF);
}

bool valid_utf8(const std::string& text) noexcept {
    std::size_t at = 0;
    while (at < text.size()) {
        std::uint32_t codepoint = 0;
        if (!next_codepoint(text, at, codepoint)) return false;
    }
    return true;
}

Error text_error(const std::string& text) noexcept {
    if (!valid_utf8(text)) return Error::InvalidUtf8;
    std::size_t at = 0;
    while (at < text.size()) {
        std::uint32_t codepoint = 0;
        if (!next_codepoint(text, at, codepoint)) return Error::InvalidUtf8;
        if (codepoint < 0x20 || (codepoint >= 0x7F && codepoint <= 0x9F))
            return Error::InvalidField;
    }
    return Error::None;
}

bool valid_json_strings(const JsonValue& value) noexcept {
    if (value.type == JsonValue::Type::String) return valid_utf8(value.str);
    if (value.type == JsonValue::Type::Array) {
        for (const JsonValue& item : value.arr)
            if (!valid_json_strings(item)) return false;
    } else if (value.type == JsonValue::Type::Object) {
        for (const auto& field : value.obj)
            if (!valid_utf8(field.first) || !valid_json_strings(field.second))
                return false;
    }
    return true;
}

bool json_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool number_delimiter(char c) noexcept {
    return json_space(c) || c == ',' || c == ']' || c == '}';
}

Error check_json_lexical(const std::string& text) noexcept {
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        return Error::MalformedJson;

    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '"') {
            ++i;
            bool closed = false;
            while (i < text.size()) {
                const unsigned char ch = static_cast<unsigned char>(text[i++]);
                if (ch == '"') {
                    closed = true;
                    break;
                }
                if (ch < 0x20) return Error::MalformedJson;
                if (ch != '\\') continue;
                if (i == text.size()) return Error::MalformedJson;
                const char escape = text[i++];
                if (escape == 'u') {
                    if (text.size() - i < 4) return Error::MalformedJson;
                    for (unsigned digit = 0; digit < 4; ++digit) {
                        const char h = text[i++];
                        if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') ||
                              (h >= 'A' && h <= 'F')))
                            return Error::MalformedJson;
                    }
                } else if (escape != '"' && escape != '\\' && escape != '/' &&
                           escape != 'b' && escape != 'f' && escape != 'n' &&
                           escape != 'r' && escape != 't') {
                    return Error::MalformedJson;
                }
            }
            if (!closed) return Error::MalformedJson;
            continue;
        }

        if (c == '-' || (c >= '0' && c <= '9')) {
            std::size_t at = i;
            if (text[at] == '-') ++at;
            if (at == text.size()) return Error::MalformedJson;
            if (text[at] == '0') {
                ++at;
                if (at < text.size() && text[at] >= '0' && text[at] <= '9')
                    return Error::MalformedJson;
            } else if (text[at] >= '1' && text[at] <= '9') {
                do { ++at; } while (at < text.size() && text[at] >= '0' && text[at] <= '9');
            } else {
                return Error::MalformedJson;
            }
            if (at < text.size() && text[at] == '.') {
                ++at;
                const std::size_t first_digit = at;
                while (at < text.size() && text[at] >= '0' && text[at] <= '9') ++at;
                if (at == first_digit) return Error::MalformedJson;
            }
            if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
                ++at;
                if (at < text.size() && (text[at] == '+' || text[at] == '-')) ++at;
                const std::size_t first_digit = at;
                while (at < text.size() && text[at] >= '0' && text[at] <= '9') ++at;
                if (at == first_digit) return Error::MalformedJson;
            }
            if (at < text.size() && !number_delimiter(text[at]))
                return Error::MalformedJson;
            i = at;
            continue;
        }

        if (c == 'I' || c == 'N') return Error::MalformedJson;
        ++i;
    }
    return Error::None;
}

template <typename E, std::size_t N>
const char* token_for(E value, const Token<E> (&tokens)[N]) noexcept {
    for (const auto& token : tokens)
        if (token.value == value) return token.text;
    return nullptr;
}

template <typename E, std::size_t N>
E parse_token(const JsonValue& value, const Token<E> (&tokens)[N]) {
    if (value.type != JsonValue::Type::String) fail(Error::InvalidField);
    for (const auto& token : tokens)
        if (value.str == token.text) return token.value;
    fail(Error::InvalidField);
}

Error check_duplicate_fields(const JsonValue& value) {
    if (value.type == JsonValue::Type::Object) {
        std::unordered_set<std::string> keys;
        keys.reserve(value.obj.size());
        for (const auto& field : value.obj)
            if (!keys.insert(field.first).second) return Error::DuplicateField;
        for (const auto& field : value.obj) {
            const Error nested = check_duplicate_fields(field.second);
            if (nested != Error::None) return nested;
        }
    } else if (value.type == JsonValue::Type::Array) {
        for (const JsonValue& item : value.arr) {
            const Error nested = check_duplicate_fields(item);
            if (nested != Error::None) return nested;
        }
    }
    return Error::None;
}

bool allowed_key(const std::string& key, const char* const* allowed,
                 std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i)
        if (key == allowed[i]) return true;
    return false;
}

Error check_fields(const JsonValue& object, const char* const* allowed,
                   std::size_t count) noexcept {
    for (const auto& field : object.obj)
        if (!allowed_key(field.first, allowed, count)) return Error::UnknownField;
    return object.obj.size() == count ? Error::None : Error::InvalidField;
}

const JsonValue& required(const JsonValue& object, const char* key) {
    const JsonValue* value = object.find(key);
    if (!value) fail(Error::InvalidField);
    return *value;
}

Error read_uint(const JsonValue& value, std::uint64_t maximum,
                std::uint64_t& output) noexcept {
    if (value.type != JsonValue::Type::Number) return Error::InvalidField;
    if (!std::isfinite(value.num)) return Error::NumericOverflow;
    if (value.num < 0.0 || std::floor(value.num) != value.num)
        return Error::InvalidField;
    if (value.num > static_cast<double>(maximum)) return Error::NumericOverflow;
    output = static_cast<std::uint64_t>(value.num);
    return static_cast<double>(output) == value.num ? Error::None : Error::NumericOverflow;
}

std::uint64_t uint_field(const JsonValue& object, const char* key,
                         std::uint64_t maximum) {
    std::uint64_t result = 0;
    const Error error = read_uint(required(object, key), maximum, result);
    if (error != Error::None) fail(error);
    return result;
}

const std::string& string_field(const JsonValue& object, const char* key,
                                std::size_t maximum, bool allow_empty) {
    const JsonValue& value = required(object, key);
    if (value.type != JsonValue::Type::String) fail(Error::InvalidField);
    if (value.str.size() > maximum || (!allow_empty && value.str.empty()))
        fail(Error::InvalidField);
    const Error text_error_result = text_error(value.str);
    if (text_error_result != Error::None) fail(text_error_result);
    return value.str;
}
std::string pem_field(const JsonValue& object, const char* key) {
    const JsonValue& value = required(object, key);
    if (value.type != JsonValue::Type::String ||
        value.str.empty() || value.str.size() > kMaxAdminPublicKeyPemBytes)
        fail(Error::InvalidField);
    if (!valid_utf8(value.str)) fail(Error::InvalidUtf8);
    for (unsigned char ch : value.str)
        if ((ch < 0x20 && ch != '\r' && ch != '\n') || ch > 0x7E)
            fail(Error::InvalidField);
    return value.str;
}

std::vector<std::uint8_t> signature_field(const JsonValue& object,
                                          const char* key) {
    const std::string& encoded = string_field(
        object, key, kMaxAdminSignatureBytes * 2, false);
    if (encoded.empty() || encoded.size() % 2 != 0) fail(Error::InvalidField);
    auto nibble = [](char ch) {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        return -1;
    };
    std::vector<std::uint8_t> signature;
    signature.reserve(encoded.size() / 2);
    for (std::size_t i = 0; i < encoded.size(); i += 2) {
        const int high = nibble(encoded[i]);
        const int low = nibble(encoded[i + 1]);
        if (high < 0 || low < 0) fail(Error::InvalidField);
        signature.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return signature;
}

std::string token_field(const JsonValue& object, const char* key,
                        std::size_t maximum) {
    const std::string& value = string_field(object, key, maximum, false);
    for (unsigned char ch : value)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
            fail(Error::InvalidField);
    return value;
}

bool bool_field(const JsonValue& object, const char* key) {
    const JsonValue& value = required(object, key);
    if (value.type != JsonValue::Type::Bool) fail(Error::InvalidField);
    return value.b;
}

admin::Intent parse_admin_intent(const JsonValue& value) {
    if (!value.is_object()) fail(Error::InvalidField);
    admin::Intent intent;
    intent.operation = parse_token(
        required(value, "operation"), kAdminOperationTokens);
    static constexpr const char* reboot_fields[] = {
        "operation", "worker_id", "leader_id", "leader_epoch", "intent_id",
        "issued_at_unix", "expires_at_unix", "signer_public_key_pem",
        "signature",
    };
    static constexpr const char* activate_fields[] = {
        "operation", "worker_id", "leader_id", "leader_epoch", "intent_id",
        "issued_at_unix", "expires_at_unix", "update_id", "package_sha256",
        "security_version", "signer_public_key_pem", "signature",
    };
    const Error fields_error = intent.operation == admin::Operation::Activate
        ? check_fields(value, activate_fields, sizeof activate_fields / sizeof *activate_fields)
        : check_fields(value, reboot_fields, sizeof reboot_fields / sizeof *reboot_fields);
    if (fields_error != Error::None) fail(fields_error);
    intent.worker_id = string_field(value, "worker_id", 64, false);
    intent.leader_id = string_field(value, "leader_id", 64, false);
    intent.leader_epoch = uint_field(value, "leader_epoch", kMaxSafeInteger);
    intent.intent_id = string_field(value, "intent_id", 64, false);
    intent.issued_at_unix =
        uint_field(value, "issued_at_unix", kMaxWireUnixSeconds);
    intent.expires_at_unix =
        uint_field(value, "expires_at_unix", kMaxWireUnixSeconds);
    if (intent.operation == admin::Operation::Activate) {
        intent.update_id = string_field(value, "update_id", 64, false);
        intent.package_sha256 = string_field(value, "package_sha256", 64, false);
        intent.security_version = uint_field(
            value, "security_version", kMaxSafeInteger);
    }
    intent.signer_public_key_pem = pem_field(value, "signer_public_key_pem");
    intent.signature = signature_field(value, "signature");
    return intent;
}

update::PackageManifest parse_package_manifest(const JsonValue& value) {
    if (!value.is_object()) fail(Error::InvalidField);
    static constexpr const char* fields[] = {
        "os", "architecture", "build", "release", "size", "sha256",
    };
    const Error fields_error = check_fields(value, fields, sizeof fields / sizeof *fields);
    if (fields_error != Error::None) fail(fields_error);
    update::PackageManifest manifest;
    manifest.os = string_field(value, "os", 16, false);
    manifest.architecture = string_field(value, "architecture", 16, false);
    manifest.build = string_field(value, "build", kMaxBuildBytes, false);
    manifest.release = string_field(value, "release", 64, false);
    manifest.size = uint_field(value, "size", update::kMaxPackageBytes);
    manifest.sha256 = string_field(value, "sha256", 64, false);
    return manifest;
}

update::PackageOffer parse_package_offer(const JsonValue& value) {
    if (!value.is_object()) fail(Error::InvalidField);
    static constexpr const char* fields[] = {
        "worker_id", "leader_id", "expires_at_unix", "update_id", "manifest",
        "signer_public_key_pem", "signature",
    };
    const Error fields_error = check_fields(value, fields, sizeof fields / sizeof *fields);
    if (fields_error != Error::None) fail(fields_error);
    update::PackageOffer offer;
    offer.worker_id = string_field(value, "worker_id", 64, false);
    offer.leader_id = string_field(value, "leader_id", 64, false);
    offer.expires_at_unix =
        uint_field(value, "expires_at_unix", kMaxWireUnixSeconds);
    offer.update_id = string_field(value, "update_id", 64, false);
    offer.manifest = parse_package_manifest(required(value, "manifest"));
    offer.signer_public_key_pem = pem_field(value, "signer_public_key_pem");
    offer.signature = signature_field(value, "signature");
    return offer;
}

Status parse_status(const JsonValue& root) {
    static constexpr const char* fields[] = {
        "schema_version", "type", "connection", "compatibility", "scheduling",
        "activity", "health", "capabilities", "build", "platform", "gpu",
        "maintenance", "online",
    };
    const Error fields_error = check_fields(root, fields, sizeof fields / sizeof *fields);
    if (fields_error != Error::None) fail(fields_error);

    Status status;
    status.connection = parse_token(required(root, "connection"), kConnectionTokens);
    status.compatibility = parse_token(required(root, "compatibility"), kCompatibilityTokens);
    status.scheduling = parse_token(required(root, "scheduling"), kSchedulingTokens);
    status.activity = parse_token(required(root, "activity"), kActivityTokens);
    status.health = parse_token(required(root, "health"), kHealthTokens);
    const JsonValue& capabilities = required(root, "capabilities");
    if (capabilities.type != JsonValue::Type::Array || capabilities.arr.size() > 3)
        fail(Error::InvalidField);
    for (const JsonValue& value : capabilities.arr) {
        const Capability capability = parse_token(value, kCapabilityTokens);
        for (Capability present : status.capabilities)
            if (present == capability) fail(Error::InvalidField);
        status.capabilities.push_back(capability);
    }
    status.build = string_field(root, "build", kMaxBuildBytes, false);
    status.platform = string_field(root, "platform", kMaxPlatformBytes, false);
    status.gpu = string_field(root, "gpu", kMaxGpuBytes, true);
    status.maintenance = bool_field(root, "maintenance");
    status.online = bool_field(root, "online");
    return status;
}

Command parse_command(const JsonValue& root) {
    static constexpr const char* common_fields[] = {
        "schema_version", "type", "command_id", "leader_id", "leader_epoch",
        "sequence", "action", "target_job_id", "confirmed",
        "issued_at_ms", "expires_at_ms",
    };
    static constexpr const char* reboot_fields[] = {
        "schema_version", "type", "command_id", "leader_id", "leader_epoch",
        "sequence", "action", "target_job_id", "confirmed", "issued_at_ms",
        "expires_at_ms", "admin_intent",
    };
    static constexpr const char* activate_fields[] = {
        "schema_version", "type", "command_id", "leader_id", "leader_epoch",
        "sequence", "action", "target_job_id", "confirmed", "issued_at_ms",
        "expires_at_ms", "admin_intent", "package_offer",
    };

    Command command;
    command.action = parse_token(required(root, "action"), kActionTokens);
    const char* const* fields = common_fields;
    std::size_t field_count = sizeof common_fields / sizeof *common_fields;
    if (command.action == CommandAction::RebootMachine) {
        fields = reboot_fields;
        field_count = sizeof reboot_fields / sizeof *reboot_fields;
    } else if (command.action == CommandAction::ActivateUpdate) {
        fields = activate_fields;
        field_count = sizeof activate_fields / sizeof *activate_fields;
    }
    const Error fields_error = check_fields(root, fields, field_count);
    if (fields_error != Error::None) fail(fields_error);
    command.command_id = token_field(root, "command_id", kMaxCommandIdBytes);
    command.leader_id = token_field(root, "leader_id", kMaxLeaderIdBytes);
    command.leader_epoch = uint_field(root, "leader_epoch", kMaxSafeInteger);
    command.sequence = uint_field(root, "sequence", kMaxSafeInteger);
    command.target_job_id = string_field(
        root, "target_job_id", kMaxTargetJobIdBytes, true);
    command.confirmed = bool_field(root, "confirmed");
    command.issued_at_ms = uint_field(root, "issued_at_ms", kMaxSafeInteger);
    command.expires_at_ms = uint_field(root, "expires_at_ms", kMaxSafeInteger);
    if (command.action == CommandAction::RebootMachine ||
        command.action == CommandAction::ActivateUpdate)
        command.admin_intent = parse_admin_intent(required(root, "admin_intent"));
    if (command.action == CommandAction::ActivateUpdate)
        command.package_offer = parse_package_offer(required(root, "package_offer"));
    return command;
}

Acknowledgment parse_acknowledgment(const JsonValue& root) {
    static constexpr const char* fields[] = {
        "schema_version", "type", "command_id", "leader_id", "leader_epoch",
        "sequence", "action", "outcome",
    };
    const Error fields_error = check_fields(root, fields, sizeof fields / sizeof *fields);
    if (fields_error != Error::None) fail(fields_error);

    Acknowledgment acknowledgment;
    acknowledgment.command_id = token_field(root, "command_id", kMaxCommandIdBytes);
    acknowledgment.leader_id = token_field(root, "leader_id", kMaxLeaderIdBytes);
    acknowledgment.leader_epoch = uint_field(root, "leader_epoch", kMaxSafeInteger);
    acknowledgment.sequence = uint_field(root, "sequence", kMaxSafeInteger);
    acknowledgment.action = parse_token(required(root, "action"), kActionTokens);
    acknowledgment.outcome = parse_token(required(root, "outcome"), kOutcomeTokens);
    return acknowledgment;
}

std::string feature_id_field(const JsonValue& object, const char* key) {
    const std::string& value =
        string_field(object, key, kMaxFeatureIdBytes, false);
    for (unsigned char ch : value)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.'))
            fail(Error::InvalidField);
    return value;
}

std::vector<app::agent::TransferFile> parse_manifest(
    const JsonValue& object, const char* key) {
    const JsonValue& value = required(object, key);
    if (value.type != JsonValue::Type::Array ||
        value.arr.size() > kMaxManifestEntries)
        fail(Error::InvalidField);

    std::vector<app::agent::TransferFile> files;
    files.reserve(value.arr.size());
    for (const JsonValue& entry : value.arr) {
        if (!entry.is_object()) fail(Error::InvalidField);
        static constexpr const char* fields[] = {"path", "size", "sha256"};
        const Error fields_error = check_fields(entry, fields, 3);
        if (fields_error != Error::None) fail(fields_error);
        app::agent::TransferFile file;
        file.path = string_field(entry, "path", kMaxManifestPathBytes, false);
        file.size = uint_field(entry, "size", kMaxSafeInteger);
        file.sha256 = string_field(entry, "sha256", 64, false);
        files.push_back(std::move(file));
    }
    return files;
}

FeatureOffer parse_feature_offer(const JsonValue& root) {
    static constexpr const char* fields[] = {
        "schema_version", "type", "job_id", "attempt_id", "plan_digest",
        "request_digest", "required_build", "expires_at_ms", "inputs",
    };
    const Error fields_error =
        check_fields(root, fields, sizeof fields / sizeof *fields);
    if (fields_error != Error::None) fail(fields_error);

    FeatureOffer offer;
    offer.job_id = feature_id_field(root, "job_id");
    offer.attempt_id = feature_id_field(root, "attempt_id");
    offer.plan_digest = string_field(root, "plan_digest", 64, false);
    offer.request_digest = string_field(root, "request_digest", 64, false);
    offer.required_build = string_field(root, "required_build", kMaxBuildBytes, false);
    offer.expires_at_ms = uint_field(root, "expires_at_ms", kMaxSafeInteger);
    offer.inputs = parse_manifest(root, "inputs");
    return offer;
}

FeatureDecision parse_feature_decision(const JsonValue& root) {
    static constexpr const char* fields[] = {
        "schema_version", "type", "job_id", "attempt_id", "step",
        "decision", "reason",
    };
    const Error fields_error =
        check_fields(root, fields, sizeof fields / sizeof *fields);
    if (fields_error != Error::None) fail(fields_error);

    FeatureDecision decision;
    decision.job_id = feature_id_field(root, "job_id");
    decision.attempt_id = feature_id_field(root, "attempt_id");
    decision.step = parse_token(required(root, "step"), kFeatureStepTokens);
    decision.decision =
        parse_token(required(root, "decision"), kFeatureDecisionTokens);
    decision.reason = string_field(root, "reason", kMaxMessageBytes, true);
    return decision;
}

PortableOffer parse_portable_offer(const JsonValue& root) {
    static constexpr const char* fields[] = {
        "schema_version", "type", "workload", "job_id", "attempt_id",
        "input_identity_sha256", "required_build", "expires_at_ms", "inputs",
    };
    const Error fields_error = check_fields(root, fields, sizeof fields / sizeof *fields);
    if (fields_error != Error::None) fail(fields_error);

    PortableOffer offer;
    offer.workload = parse_token(required(root, "workload"), kPortableWorkloadTokens);
    offer.job_id = feature_id_field(root, "job_id");
    offer.attempt_id = feature_id_field(root, "attempt_id");
    offer.input_identity_sha256 =
        string_field(root, "input_identity_sha256", 64, false);
    offer.required_build = string_field(root, "required_build", kMaxBuildBytes, false);
    offer.expires_at_ms = uint_field(root, "expires_at_ms", kMaxSafeInteger);
    offer.inputs = parse_manifest(root, "inputs");
    return offer;
}

PortableDecision parse_portable_decision(const JsonValue& root) {
    static constexpr const char* fields[] = {
        "schema_version", "type", "workload", "job_id", "attempt_id",
        "step", "decision", "reason",
    };
    const Error fields_error = check_fields(root, fields, sizeof fields / sizeof *fields);
    if (fields_error != Error::None) fail(fields_error);

    PortableDecision decision;
    decision.workload = parse_token(required(root, "workload"), kPortableWorkloadTokens);
    decision.job_id = feature_id_field(root, "job_id");
    decision.attempt_id = feature_id_field(root, "attempt_id");
    decision.step = parse_token(required(root, "step"), kPortableStepTokens);
    decision.decision = parse_token(required(root, "decision"), kPortableDecisionTokens);
    decision.reason = string_field(root, "reason", kMaxPortableTextBytes, true);
    return decision;
}

PortableResult parse_portable_result(const JsonValue& root) {
    static constexpr const char* fields[] = {
        "schema_version", "type", "workload", "job_id", "attempt_id",
        "outcome", "outputs", "output_metadata", "error",
    };
    const Error fields_error = check_fields(root, fields, sizeof fields / sizeof *fields);
    if (fields_error != Error::None) fail(fields_error);

    PortableResult result;
    result.workload = parse_token(required(root, "workload"), kPortableWorkloadTokens);
    result.job_id = feature_id_field(root, "job_id");
    result.attempt_id = feature_id_field(root, "attempt_id");
    result.outcome = parse_token(required(root, "outcome"), kPortableOutcomeTokens);
    result.outputs = parse_manifest(root, "outputs");
    result.output_metadata =
        string_field(root, "output_metadata", kMaxCheckpointBasenameBytes, true);
    result.error = string_field(root, "error", kMaxPortableTextBytes, true);
    return result;
}

FeatureResult parse_feature_result(const JsonValue& root) {
    static constexpr const char* fields[] = {
        "schema_version", "type", "job_id", "attempt_id", "outcome",
        "outputs", "error",
    };
    const Error fields_error =
        check_fields(root, fields, sizeof fields / sizeof *fields);
    if (fields_error != Error::None) fail(fields_error);

    FeatureResult result;
    result.job_id = feature_id_field(root, "job_id");
    result.attempt_id = feature_id_field(root, "attempt_id");
    result.outcome = parse_token(required(root, "outcome"), kFeatureOutcomeTokens);
    result.outputs = parse_manifest(root, "outputs");
    result.error = string_field(root, "error", kMaxMessageBytes, true);
    return result;
}

Error validate_text(const std::string& value, std::size_t maximum,
                    bool allow_empty) noexcept {
    if (value.size() > maximum || (!allow_empty && value.empty()))
        return Error::InvalidField;
    return text_error(value);
}

Error validate_token(const std::string& value, std::size_t maximum) noexcept {
    const Error text_error = validate_text(value, maximum, false);
    if (text_error != Error::None) return text_error;
    for (unsigned char ch : value)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
            return Error::InvalidField;
    return Error::None;
}

char fold_ascii(unsigned char ch) noexcept {
    return ch >= 'A' && ch <= 'Z'
        ? static_cast<char>(ch - 'A' + 'a')
        : static_cast<char>(ch);
}

int folded_compare(std::string_view left, std::string_view right) noexcept {
    const std::size_t common = std::min(left.size(), right.size());
    for (std::size_t i = 0; i < common; ++i) {
        const char a = fold_ascii(static_cast<unsigned char>(left[i]));
        const char b = fold_ascii(static_cast<unsigned char>(right[i]));
        if (a != b) return a < b ? -1 : 1;
    }
    return left.size() == right.size() ? 0 : (left.size() < right.size() ? -1 : 1);
}

bool ascii_equal(std::string_view value, std::string_view expected) noexcept {
    return folded_compare(value, expected) == 0;
}

bool windows_reserved_name(std::string_view component) noexcept {
    std::string_view stem = component.substr(0, component.find('.'));
    while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.'))
        stem.remove_suffix(1);
    if (ascii_equal(stem, "con") || ascii_equal(stem, "prn") ||
        ascii_equal(stem, "aux") || ascii_equal(stem, "nul") ||
        ascii_equal(stem, "conin$") || ascii_equal(stem, "conout$"))
        return true;
    if (stem.size() == 4 &&
        (ascii_equal(stem.substr(0, 3), "com") ||
         ascii_equal(stem.substr(0, 3), "lpt")) &&
        stem[3] >= '1' && stem[3] <= '9')
        return true;
    if (stem.size() == 5 &&
        (ascii_equal(stem.substr(0, 3), "com") ||
         ascii_equal(stem.substr(0, 3), "lpt"))) {
        const std::string_view suffix = stem.substr(3);
        return suffix == "\xC2\xB9" || suffix == "\xC2\xB2" ||
               suffix == "\xC2\xB3";
    }
    return false;
}

Error validate_feature_id(const std::string& value) noexcept {
    const Error error = validate_text(value, kMaxFeatureIdBytes, false);
    if (error != Error::None) return error;
    for (unsigned char ch : value)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.'))
            return Error::InvalidField;
    return Error::None;
}
Error validate_target_job_id(const std::string& value) noexcept {
    const Error error = validate_text(value, kMaxTargetJobIdBytes, true);
    if (error != Error::None) return error;
    for (unsigned char ch : value)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.'))
            return Error::InvalidField;
    return Error::None;
}
Error validate_portable_expiry(std::uint64_t now_unix_ms,
                               std::uint64_t expires_at_ms) noexcept {
    if (now_unix_ms > kMaxSafeInteger || expires_at_ms > kMaxSafeInteger)
        return Error::NumericOverflow;
    if (expires_at_ms <= now_unix_ms) return Error::ExpiredPortableOffer;
    return expires_at_ms - now_unix_ms > kMaxPortableOfferLifetimeMs
        ? Error::InvalidTime
        : Error::None;
}

bool valid_checkpoint_basename(const std::string& value) noexcept {
    if (value.size() < 19 || value.size() > kMaxCheckpointBasenameBytes ||
        value.compare(0, 5, "step-") != 0 ||
        value.compare(value.size() - 5, 5, ".ckpt") != 0)
        return false;

    const std::size_t digits = value.size() - 10;
    if (digits < 9 || digits > 20) return false;
    const char* first = value.data() + 5;
    const char* last = first + digits;
    for (const char* at = first; at != last; ++at)
        if (*at < '0' || *at > '9') return false;

    std::uint64_t step = 0;
    const auto parsed = std::from_chars(first, last, step);
    if (parsed.ec != std::errc{} || parsed.ptr != last) return false;
    char canonical[20];
    const auto formatted = std::to_chars(canonical, canonical + sizeof canonical, step);
    if (formatted.ec != std::errc{}) return false;
    const std::size_t canonical_size =
        static_cast<std::size_t>(formatted.ptr - canonical);
    const std::size_t expected_digits = std::max<std::size_t>(9, canonical_size);
    if (digits != expected_digits) return false;
    const std::size_t padding = expected_digits - canonical_size;
    for (std::size_t i = 0; i < expected_digits; ++i) {
        const char expected = i < padding ? '0' : canonical[i - padding];
        if (first[i] != expected) return false;
    }
    return true;
}


bool valid_digest(const std::string& digest) noexcept {
    if (digest.size() != 64) return false;
    for (unsigned char ch : digest)
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
    return true;
}

bool ascii_alnum(unsigned char ch) noexcept {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9');
}

Error validate_update_id(const std::string& value) noexcept {
    const Error text = validate_text(value, 64, false);
    if (text != Error::None) return text;
    if (!ascii_alnum(static_cast<unsigned char>(value.front())))
        return Error::InvalidField;
    for (unsigned char ch : value)
        if (!ascii_alnum(ch) && ch != '-' && ch != '_' && ch != '.')
            return Error::InvalidField;
    return Error::None;
}

Error validate_update_build(const std::string& value) noexcept {
    const Error text = validate_text(value, kMaxBuildBytes, false);
    if (text != Error::None) return text;
    if (value.front() == ' ' || value.back() == ' ') return Error::InvalidField;
    bool has_alnum = false;
    for (unsigned char ch : value) {
        if (ascii_alnum(ch)) {
            has_alnum = true;
        } else if (ch != '.' && ch != '_' && ch != '+' && ch != '-' &&
                   ch != '(' && ch != ')' && ch != ' ') {
            return Error::InvalidField;
        }
    }
    return has_alnum ? Error::None : Error::InvalidField;
}

Error validate_update_release(const std::string& value) noexcept {
    const Error text = validate_text(value, 64, false);
    if (text != Error::None) return text;
    if (!ascii_alnum(static_cast<unsigned char>(value.front())) ||
        value.back() == '.')
        return Error::InvalidField;
    for (unsigned char ch : value)
        if (!ascii_alnum(ch) && ch != '.' && ch != '_' && ch != '+' && ch != '-')
            return Error::InvalidField;
    return Error::None;
}

Error validate_admin_pem(const std::string& pem) noexcept {
    if (pem.empty() || pem.size() > kMaxAdminPublicKeyPemBytes)
        return Error::InvalidField;
    if (!valid_utf8(pem)) return Error::InvalidUtf8;
    for (unsigned char ch : pem)
        if ((ch < 0x20 && ch != '\r' && ch != '\n') || ch > 0x7E)
            return Error::InvalidField;
    return Error::None;
}

Error validate_signature(const std::vector<std::uint8_t>& signature) noexcept {
    return signature.empty() || signature.size() > kMaxAdminSignatureBytes
        ? Error::InvalidField
        : Error::None;
}

Error validate_admin_intent(const admin::Intent& intent) noexcept {
    if (intent.operation != admin::Operation::Reboot &&
        intent.operation != admin::Operation::Activate)
        return Error::InvalidField;
    if (!valid_digest(intent.worker_id) || !valid_digest(intent.leader_id))
        return Error::InvalidField;
    if (intent.leader_epoch > kMaxSafeInteger) return Error::NumericOverflow;
    if (!intent.leader_epoch) return Error::InvalidField;
    Error error = validate_update_id(intent.intent_id);
    if (error != Error::None) return error;
    if (intent.issued_at_unix > kMaxWireUnixSeconds ||
        intent.expires_at_unix > kMaxWireUnixSeconds)
        return Error::NumericOverflow;
    if (!intent.issued_at_unix || !intent.expires_at_unix ||
        intent.expires_at_unix <= intent.issued_at_unix ||
        intent.expires_at_unix - intent.issued_at_unix >
            kMaxAdminIntentLifetimeSeconds)
        return Error::InvalidTime;
    if (intent.operation == admin::Operation::Reboot) {
        if (!intent.update_id.empty() || !intent.package_sha256.empty() ||
            intent.security_version)
            return Error::InvalidCombination;
    } else {
        error = validate_update_id(intent.update_id);
        if (error != Error::None) return error;
        if (!valid_digest(intent.package_sha256)) return Error::InvalidField;
        if (intent.security_version > kMaxSafeInteger)
            return Error::NumericOverflow;
        if (!intent.security_version) return Error::InvalidField;
    }
    error = validate_admin_pem(intent.signer_public_key_pem);
    if (error != Error::None) return error;
    return validate_signature(intent.signature);
}

Error validate_package_manifest(
    const update::PackageManifest& manifest) noexcept {
    if ((manifest.os != "windows" && manifest.os != "linux" &&
         manifest.os != "macos") ||
        (manifest.architecture != "x86_64" &&
         manifest.architecture != "aarch64"))
        return Error::InvalidField;
    Error error = validate_update_build(manifest.build);
    if (error != Error::None) return error;
    error = validate_update_release(manifest.release);
    if (error != Error::None) return error;
    if (!manifest.size || manifest.size > update::kMaxPackageBytes ||
        !valid_digest(manifest.sha256))
        return Error::InvalidField;
    return Error::None;
}

Error validate_package_offer(const update::PackageOffer& offer) noexcept {
    if (!valid_digest(offer.worker_id) || !valid_digest(offer.leader_id))
        return Error::InvalidField;
    if (offer.expires_at_unix > kMaxWireUnixSeconds)
        return Error::NumericOverflow;
    if (!offer.expires_at_unix) return Error::InvalidTime;
    Error error = validate_update_id(offer.update_id);
    if (error != Error::None) return error;
    error = validate_package_manifest(offer.manifest);
    if (error != Error::None) return error;
    error = validate_admin_pem(offer.signer_public_key_pem);
    if (error != Error::None) return error;
    return validate_signature(offer.signature);
}

Error validate_manifest_path(const std::string& path) noexcept {
    const Error text = validate_text(path, kMaxManifestPathBytes, false);
    if (text != Error::None) return text;
    if (path.front() == '/' || path.back() == '/' ||
        path.find('\\') != std::string::npos)
        return Error::InvalidField;

    std::size_t start = 0;
    while (start < path.size()) {
        const std::size_t end = path.find('/', start);
        const std::size_t count =
            (end == std::string::npos ? path.size() : end) - start;
        const std::string_view component(path.data() + start, count);
        if (component.empty() || component == "." || component == ".." ||
            component.size() > 255 || component.back() == '.' ||
            component.back() == ' ' || windows_reserved_name(component) ||
            ascii_equal(component, ".agent-transfer-v1"))
            return Error::InvalidField;
        for (unsigned char ch : component)
            if (ch == ':' || ch == '<' || ch == '>' || ch == '"' ||
                ch == '|' || ch == '?' || ch == '*')
                return Error::InvalidField;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return Error::None;
}

Error validate_manifest(
    const std::vector<app::agent::TransferFile>& files) noexcept {
    if (files.size() > kMaxManifestEntries) return Error::InvalidField;
    std::array<std::string_view, kMaxManifestEntries> paths{};
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& file = files[i];
        Error error = validate_manifest_path(file.path);
        if (error != Error::None) return error;
        if (!valid_digest(file.sha256)) return Error::InvalidField;
        if (file.size > kMaxSafeInteger ||
            file.size > kMaxSafeInteger - total)
            return Error::NumericOverflow;
        total += file.size;
        paths[i] = file.path;
    }
    std::sort(paths.begin(),
              paths.begin() + static_cast<std::ptrdiff_t>(files.size()),
              [](std::string_view a, std::string_view b) {
                  return folded_compare(a, b) < 0;
              });
    for (std::size_t i = 1; i < files.size(); ++i)
        if (folded_compare(paths[i - 1], paths[i]) == 0)
            return Error::InvalidField;
    return Error::None;
}

void write_status(JsonWriter& writer, const Status& status) {
    writer.object()
        .field("schema_version", static_cast<int>(kSchemaVersion))
        .field("type", "status")
        .field("connection", token_for(status.connection, kConnectionTokens))
        .field("compatibility", token_for(status.compatibility, kCompatibilityTokens))
        .field("scheduling", token_for(status.scheduling, kSchedulingTokens))
        .field("activity", token_for(status.activity, kActivityTokens))
        .field("health", token_for(status.health, kHealthTokens))
        .key("capabilities").array();
    for (Capability capability : status.capabilities)
        writer.value(token_for(capability, kCapabilityTokens));
    writer.end()
        .field("build", status.build)
        .field("platform", status.platform)
        .field("gpu", status.gpu)
        .field("maintenance", status.maintenance)
        .field("online", status.online)
        .end();
}

std::string signature_hex(const std::vector<std::uint8_t>& signature) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(signature.size() * 2);
    for (std::uint8_t byte : signature) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0F]);
    }
    return result;
}

void write_admin_intent(JsonWriter& writer, const admin::Intent& intent) {
    writer.key("admin_intent").object()
        .field("operation", token_for(intent.operation, kAdminOperationTokens))
        .field("worker_id", intent.worker_id)
        .field("leader_id", intent.leader_id)
        .field("leader_epoch", static_cast<long long>(intent.leader_epoch))
        .field("intent_id", intent.intent_id)
        .field("issued_at_unix", static_cast<long long>(intent.issued_at_unix))
        .field("expires_at_unix", static_cast<long long>(intent.expires_at_unix));
    if (intent.operation == admin::Operation::Activate)
        writer.field("update_id", intent.update_id)
            .field("package_sha256", intent.package_sha256)
            .field("security_version", static_cast<long long>(intent.security_version));
    writer.field("signer_public_key_pem", intent.signer_public_key_pem)
        .field("signature", signature_hex(intent.signature))
        .end();
}

void write_package_offer(JsonWriter& writer,
                         const update::PackageOffer& offer) {
    writer.key("package_offer").object()
        .field("worker_id", offer.worker_id)
        .field("leader_id", offer.leader_id)
        .field("expires_at_unix", static_cast<long long>(offer.expires_at_unix))
        .field("update_id", offer.update_id)
        .key("manifest").object()
        .field("os", offer.manifest.os)
        .field("architecture", offer.manifest.architecture)
        .field("build", offer.manifest.build)
        .field("release", offer.manifest.release)
        .field("size", static_cast<long long>(offer.manifest.size))
        .field("sha256", offer.manifest.sha256)
        .end()
        .field("signer_public_key_pem", offer.signer_public_key_pem)
        .field("signature", signature_hex(offer.signature))
        .end();
}
void write_command(JsonWriter& writer, const Command& command) {
    writer.object()
        .field("schema_version", static_cast<int>(kSchemaVersion))
        .field("type", "command")
        .field("command_id", command.command_id)
        .field("leader_id", command.leader_id)
        .field("leader_epoch", static_cast<long long>(command.leader_epoch))
        .field("sequence", static_cast<long long>(command.sequence))
        .field("action", token_for(command.action, kActionTokens))
        .field("target_job_id", command.target_job_id)
        .field("confirmed", command.confirmed)
        .field("issued_at_ms", static_cast<long long>(command.issued_at_ms))
        .field("expires_at_ms", static_cast<long long>(command.expires_at_ms));
    if (command.admin_intent) write_admin_intent(writer, *command.admin_intent);
    if (command.package_offer) write_package_offer(writer, *command.package_offer);
    writer.end();
}

void write_acknowledgment(JsonWriter& writer,
                          const Acknowledgment& acknowledgment) {
    writer.object()
        .field("schema_version", static_cast<int>(kSchemaVersion))
        .field("type", "acknowledgment")
        .field("command_id", acknowledgment.command_id)
        .field("leader_id", acknowledgment.leader_id)
        .field("leader_epoch", static_cast<long long>(acknowledgment.leader_epoch))
        .field("sequence", static_cast<long long>(acknowledgment.sequence))
        .field("action", token_for(acknowledgment.action, kActionTokens))
        .field("outcome", token_for(acknowledgment.outcome, kOutcomeTokens))
        .end();
}

void write_manifest(JsonWriter& writer, const char* key,
                    const std::vector<app::agent::TransferFile>& files) {
    writer.key(key).array();
    for (const auto& file : files) {
        writer.object()
            .field("path", file.path)
            .field("size", static_cast<long long>(file.size))
            .field("sha256", file.sha256)
            .end();
    }
    writer.end();
}

void write_feature_offer(JsonWriter& writer, const FeatureOffer& offer) {
    writer.object()
        .field("schema_version", static_cast<int>(kSchemaVersion))
        .field("type", "feature_offer")
        .field("job_id", offer.job_id)
        .field("attempt_id", offer.attempt_id)
        .field("plan_digest", offer.plan_digest)
        .field("request_digest", offer.request_digest)
        .field("required_build", offer.required_build)
        .field("expires_at_ms", static_cast<long long>(offer.expires_at_ms));
    write_manifest(writer, "inputs", offer.inputs);
    writer.end();
}

void write_feature_decision(JsonWriter& writer,
                            const FeatureDecision& decision) {
    writer.object()
        .field("schema_version", static_cast<int>(kSchemaVersion))
        .field("type", "feature_decision")
        .field("job_id", decision.job_id)
        .field("attempt_id", decision.attempt_id)
        .field("step", token_for(decision.step, kFeatureStepTokens))
        .field("decision", token_for(decision.decision, kFeatureDecisionTokens))
        .field("reason", decision.reason)
        .end();
}

void write_feature_result(JsonWriter& writer, const FeatureResult& result) {
    writer.object()
        .field("schema_version", static_cast<int>(kSchemaVersion))
        .field("type", "feature_result")
        .field("job_id", result.job_id)
        .field("attempt_id", result.attempt_id)
        .field("outcome", token_for(result.outcome, kFeatureOutcomeTokens));
    write_manifest(writer, "outputs", result.outputs);
    writer.field("error", result.error).end();
}

void write_portable_offer(JsonWriter& writer, const PortableOffer& offer) {
    writer.object()
        .field("schema_version", static_cast<int>(kSchemaVersion))
        .field("type", "portable_offer")
        .field("workload", token_for(offer.workload, kPortableWorkloadTokens))
        .field("job_id", offer.job_id)
        .field("attempt_id", offer.attempt_id)
        .field("input_identity_sha256", offer.input_identity_sha256)
        .field("required_build", offer.required_build)
        .field("expires_at_ms", static_cast<long long>(offer.expires_at_ms));
    write_manifest(writer, "inputs", offer.inputs);
    writer.end();
}

void write_portable_decision(JsonWriter& writer,
                             const PortableDecision& decision) {
    writer.object()
        .field("schema_version", static_cast<int>(kSchemaVersion))
        .field("type", "portable_decision")
        .field("workload", token_for(decision.workload, kPortableWorkloadTokens))
        .field("job_id", decision.job_id)
        .field("attempt_id", decision.attempt_id)
        .field("step", token_for(decision.step, kPortableStepTokens))
        .field("decision", token_for(decision.decision, kPortableDecisionTokens))
        .field("reason", decision.reason)
        .end();
}

void write_portable_result(JsonWriter& writer, const PortableResult& result) {
    writer.object()
        .field("schema_version", static_cast<int>(kSchemaVersion))
        .field("type", "portable_result")
        .field("workload", token_for(result.workload, kPortableWorkloadTokens))
        .field("job_id", result.job_id)
        .field("attempt_id", result.attempt_id)
        .field("outcome", token_for(result.outcome, kPortableOutcomeTokens));
    write_manifest(writer, "outputs", result.outputs);
    writer.field("output_metadata", result.output_metadata)
        .field("error", result.error)
        .end();
}

}  // namespace

bool IsValid(ConnectionState value) noexcept {
    return token_for(value, kConnectionTokens) != nullptr;
}

bool IsValid(CompatibilityState value) noexcept {
    return token_for(value, kCompatibilityTokens) != nullptr;
}

bool IsValid(SchedulingState value) noexcept {
    return token_for(value, kSchedulingTokens) != nullptr;
}

bool IsValid(ActivityState value) noexcept {
    return token_for(value, kActivityTokens) != nullptr;
}

bool IsValid(HealthState value) noexcept {
    return token_for(value, kHealthTokens) != nullptr;
}

bool IsValid(Capability value) noexcept {
    return token_for(value, kCapabilityTokens) != nullptr;
}

bool IsValid(CommandAction value) noexcept {
    return token_for(value, kActionTokens) != nullptr;
}

bool IsValid(AcknowledgmentOutcome value) noexcept {
    return token_for(value, kOutcomeTokens) != nullptr;
}

bool IsValid(PortableWorkload value) noexcept {
    return token_for(value, kPortableWorkloadTokens) != nullptr;
}

Error Validate(const Message& message, std::uint64_t now_unix_ms) noexcept {
    if (message.schema_version != kSchemaVersion) return Error::UnsupportedVersion;
    if (message.payload.valueless_by_exception()) return Error::InvalidField;

    if (const auto* status = std::get_if<Status>(&message.payload)) {
        if (!IsValid(status->connection) || !IsValid(status->compatibility) ||
            !IsValid(status->scheduling) || !IsValid(status->activity) ||
            !IsValid(status->health))
            return Error::InvalidField;
        for (std::size_t i = 0; i < status->capabilities.size(); ++i) {
            if (!IsValid(status->capabilities[i])) return Error::InvalidField;
            for (std::size_t j = 0; j < i; ++j)
                if (status->capabilities[i] == status->capabilities[j])
                    return Error::InvalidField;
        }
        Error error = validate_text(status->build, kMaxBuildBytes, false);
        if (error != Error::None) return error;
        error = validate_text(status->platform, kMaxPlatformBytes, false);
        if (error != Error::None) return error;
        error = validate_text(status->gpu, kMaxGpuBytes, true);
        if (error != Error::None) return error;
        if ((status->maintenance || !status->online ||
             status->compatibility == CompatibilityState::Incompatible ||
             status->health == HealthState::Unhealthy) &&
            status->scheduling == SchedulingState::Accepting)
            return Error::InvalidCombination;
        return Error::None;
    }

    if (const auto* command = std::get_if<Command>(&message.payload)) {
        Error error = validate_token(command->command_id, kMaxCommandIdBytes);
        if (error != Error::None) return error;
        error = validate_token(command->leader_id, kMaxLeaderIdBytes);
        if (error != Error::None) return error;
        error = validate_target_job_id(command->target_job_id);
        if (error != Error::None) return error;
        if (command->leader_epoch > kMaxSafeInteger ||
            command->sequence > kMaxSafeInteger)
            return Error::NumericOverflow;
        if (command->leader_epoch == 0 || command->sequence == 0)
            return Error::InvalidField;
        if (!IsValid(command->action)) return Error::InvalidField;
        const bool admin_action =
            command->action == CommandAction::RebootMachine ||
            command->action == CommandAction::ActivateUpdate;
        const bool confirmation_required =
            command->action == CommandAction::Stop ||
            command->action == CommandAction::ForceRestartService ||
            admin_action;
        if (command->confirmed != confirmation_required ||
            (!command->target_job_id.empty() &&
             command->action != CommandAction::Stop) ||
            (!admin_action &&
             (command->admin_intent || command->package_offer)))
            return Error::InvalidCombination;
        if (now_unix_ms > kMaxSafeInteger ||
            command->issued_at_ms > kMaxSafeInteger ||
            command->expires_at_ms > kMaxSafeInteger)
            return Error::NumericOverflow;
        if (command->issued_at_ms > now_unix_ms) return Error::InvalidTime;
        const bool restart_action =
            command->action == CommandAction::RestartService ||
            command->action == CommandAction::ForceRestartService;
        if (command->expires_at_ms <= command->issued_at_ms ||
            (restart_action &&
             command->expires_at_ms - command->issued_at_ms >
                 kMaxRestartCommandLifetimeMs) ||
            (admin_action &&
             command->expires_at_ms - command->issued_at_ms >
                 kMaxRestartCommandLifetimeMs))
            return Error::InvalidCombination;
        if (command->expires_at_ms <= now_unix_ms) return Error::ExpiredCommand;
        if (admin_action) {
            if (!command->admin_intent) return Error::InvalidCombination;
            const admin::Intent& intent = *command->admin_intent;
            Error error = validate_admin_intent(intent);
            if (error != Error::None) return error;
            if (intent.leader_id != command->leader_id ||
                intent.leader_epoch != command->leader_epoch ||
                command->expires_at_ms > intent.expires_at_unix * 1000)
                return Error::InvalidCombination;
            if (command->action == CommandAction::RebootMachine) {
                if (intent.operation != admin::Operation::Reboot ||
                    command->package_offer)
                    return Error::InvalidCombination;
            } else {
                if (intent.operation != admin::Operation::Activate ||
                    !command->package_offer)
                    return Error::InvalidCombination;
                const update::PackageOffer& offer = *command->package_offer;
                error = validate_package_offer(offer);
                if (error != Error::None) return error;
                if (offer.worker_id != intent.worker_id ||
                    offer.leader_id != intent.leader_id ||
                    offer.leader_id != command->leader_id ||
                    offer.update_id != intent.update_id ||
                    offer.manifest.sha256 != intent.package_sha256 ||
                    command->expires_at_ms > offer.expires_at_unix * 1000)
                    return Error::InvalidCombination;
            }
        }
        return Error::None;
    }

    if (const auto* acknowledgment =
            std::get_if<Acknowledgment>(&message.payload)) {
        Error error = validate_token(acknowledgment->command_id, kMaxCommandIdBytes);
        if (error != Error::None) return error;
        error = validate_token(acknowledgment->leader_id, kMaxLeaderIdBytes);
        if (error != Error::None) return error;
        if (acknowledgment->leader_epoch > kMaxSafeInteger ||
            acknowledgment->sequence > kMaxSafeInteger)
            return Error::NumericOverflow;
        if (acknowledgment->leader_epoch == 0 || acknowledgment->sequence == 0)
            return Error::InvalidField;
        if (!IsValid(acknowledgment->action) ||
            !IsValid(acknowledgment->outcome))
            return Error::InvalidField;
        return Error::None;
    }

    if (const auto* offer = std::get_if<FeatureOffer>(&message.payload)) {
        Error error = validate_feature_id(offer->job_id);
        if (error != Error::None) return error;
        error = validate_feature_id(offer->attempt_id);
        if (error != Error::None) return error;
        if (!valid_digest(offer->plan_digest) ||
            !valid_digest(offer->request_digest))
            return Error::InvalidField;
        error = validate_text(offer->required_build, kMaxBuildBytes, false);
        if (error != Error::None) return error;
        if (now_unix_ms > kMaxSafeInteger ||
            offer->expires_at_ms > kMaxSafeInteger)
            return Error::NumericOverflow;
        error = validate_manifest(offer->inputs);
        if (error != Error::None) return error;
        return offer->expires_at_ms <= now_unix_ms
            ? Error::ExpiredFeatureOffer
            : Error::None;
    }

    if (const auto* decision = std::get_if<FeatureDecision>(&message.payload)) {
        Error error = validate_feature_id(decision->job_id);
        if (error != Error::None) return error;
        error = validate_feature_id(decision->attempt_id);
        if (error != Error::None) return error;
        if (!token_for(decision->step, kFeatureStepTokens) ||
            !token_for(decision->decision, kFeatureDecisionTokens))
            return Error::InvalidField;
        error = validate_text(decision->reason, kMaxMessageBytes, true);
        if (error != Error::None) return error;
        if (decision->decision == FeatureDecision::Decision::Committed &&
            decision->step != FeatureDecision::Step::Output)
            return Error::InvalidCombination;
        return Error::None;
    }

    if (const auto* result = std::get_if<FeatureResult>(&message.payload)) {
        Error error = validate_feature_id(result->job_id);
        if (error != Error::None) return error;
        error = validate_feature_id(result->attempt_id);
        if (error != Error::None) return error;
        if (!token_for(result->outcome, kFeatureOutcomeTokens))
            return Error::InvalidField;
        error = validate_text(result->error, kMaxMessageBytes, true);
        if (error != Error::None) return error;
        error = validate_manifest(result->outputs);
        if (error != Error::None) return error;
        if ((result->outcome == FeatureResult::Outcome::Succeeded &&
             result->outputs.empty()) ||
            (result->outcome != FeatureResult::Outcome::Succeeded &&
             !result->outputs.empty()))
            return Error::InvalidCombination;
        return Error::None;
    }
    if (const auto* offer = std::get_if<PortableOffer>(&message.payload)) {
        if (!IsValid(offer->workload)) return Error::InvalidField;
        Error error = validate_feature_id(offer->job_id);
        if (error != Error::None) return error;
        error = validate_feature_id(offer->attempt_id);
        if (error != Error::None) return error;
        if (!valid_digest(offer->input_identity_sha256))
            return Error::InvalidField;
        error = validate_text(offer->required_build, kMaxBuildBytes, false);
        if (error != Error::None) return error;
        error = validate_portable_expiry(now_unix_ms, offer->expires_at_ms);
        if (error != Error::None) return error;
        return validate_manifest(offer->inputs);
    }

    if (const auto* decision =
            std::get_if<PortableDecision>(&message.payload)) {
        if (!IsValid(decision->workload)) return Error::InvalidField;
        Error error = validate_feature_id(decision->job_id);
        if (error != Error::None) return error;
        error = validate_feature_id(decision->attempt_id);
        if (error != Error::None) return error;
        if (!token_for(decision->step, kPortableStepTokens) ||
            !token_for(decision->decision, kPortableDecisionTokens))
            return Error::InvalidField;
        error = validate_text(decision->reason, kMaxPortableTextBytes, true);
        if (error != Error::None) return error;
        if (decision->decision == PortableDecision::Decision::Committed &&
            decision->step != PortableDecision::Step::Output)
            return Error::InvalidCombination;
        return Error::None;
    }

    if (const auto* result = std::get_if<PortableResult>(&message.payload)) {
        if (!IsValid(result->workload)) return Error::InvalidField;
        Error error = validate_feature_id(result->job_id);
        if (error != Error::None) return error;
        error = validate_feature_id(result->attempt_id);
        if (error != Error::None) return error;
        if (!token_for(result->outcome, kPortableOutcomeTokens))
            return Error::InvalidField;
        error = validate_text(result->error, kMaxPortableTextBytes, true);
        if (error != Error::None) return error;
        error = validate_manifest(result->outputs);
        if (error != Error::None) return error;
        error = validate_text(result->output_metadata,
                              kMaxCheckpointBasenameBytes, true);
        if (error != Error::None) return error;
        if (result->outcome != PortableResult::Outcome::Succeeded) {
            return result->outputs.empty() && result->output_metadata.empty()
                ? Error::None
                : Error::InvalidCombination;
        }
        if (result->outputs.empty()) return Error::InvalidCombination;
        if (result->workload == PortableWorkload::Reconstruction)
            return result->output_metadata.empty()
                ? Error::None
                : Error::InvalidCombination;
        return valid_checkpoint_basename(result->output_metadata)
            ? Error::None
            : Error::InvalidField;
    }
    return Error::InvalidField;
}

Error Encode(const Message& message, std::uint64_t now_unix_ms,
             std::string& json) {
    const Error validation = Validate(message, now_unix_ms);
    if (validation != Error::None) return validation;

    JsonWriter writer;
    if (const auto* status = std::get_if<Status>(&message.payload)) {
        write_status(writer, *status);
    } else if (const auto* command = std::get_if<Command>(&message.payload)) {
        write_command(writer, *command);
    } else if (const auto* acknowledgment =
                   std::get_if<Acknowledgment>(&message.payload)) {
        write_acknowledgment(writer, *acknowledgment);
    } else if (const auto* offer = std::get_if<FeatureOffer>(&message.payload)) {
        write_feature_offer(writer, *offer);
    } else if (const auto* decision =
                   std::get_if<FeatureDecision>(&message.payload)) {
        write_feature_decision(writer, *decision);
    } else if (const auto* portable_offer =
                   std::get_if<PortableOffer>(&message.payload)) {
        write_portable_offer(writer, *portable_offer);
    } else if (const auto* portable_decision =
                   std::get_if<PortableDecision>(&message.payload)) {
        write_portable_decision(writer, *portable_decision);
    } else if (const auto* portable_result =
                   std::get_if<PortableResult>(&message.payload)) {
        write_portable_result(writer, *portable_result);
    } else {
        write_feature_result(writer, std::get<FeatureResult>(message.payload));
    }
    std::string encoded = writer.str();
    if (encoded.size() > kMaxMessageBytes) return Error::MessageTooLarge;
    json.swap(encoded);
    return Error::None;
}

Error Decode(const std::string& json, std::uint64_t now_unix_ms,
             Message& message) {
    if (json.size() > kMaxMessageBytes) return Error::MessageTooLarge;
    if (!valid_utf8(json)) return Error::InvalidUtf8;
    const Error lexical_error = check_json_lexical(json);
    if (lexical_error != Error::None) return lexical_error;

    try {
        JsonValue root = json_parse(json);
        if (!valid_json_strings(root)) return Error::InvalidUtf8;
        if (!root.is_object()) return Error::InvalidField;
        const Error duplicates = check_duplicate_fields(root);
        if (duplicates != Error::None) return duplicates;

        std::uint64_t version = 0;
        const Error version_error = read_uint(required(root, "schema_version"),
                                              std::numeric_limits<std::uint32_t>::max(),
                                              version);
        if (version_error != Error::None) return version_error;
        if (version != kSchemaVersion) return Error::UnsupportedVersion;
        const JsonValue& type = required(root, "type");
        if (type.type != JsonValue::Type::String) return Error::InvalidField;

        Message parsed;
        if (type.str == "status") {
            parsed.payload = parse_status(root);
        } else if (type.str == "command") {
            parsed.payload = parse_command(root);
        } else if (type.str == "acknowledgment") {
            parsed.payload = parse_acknowledgment(root);
        } else if (type.str == "feature_offer") {
            parsed.payload = parse_feature_offer(root);
        } else if (type.str == "feature_decision") {
            parsed.payload = parse_feature_decision(root);
        } else if (type.str == "feature_result") {
            parsed.payload = parse_feature_result(root);
        } else if (type.str == "portable_offer") {
            parsed.payload = parse_portable_offer(root);
        } else if (type.str == "portable_decision") {
            parsed.payload = parse_portable_decision(root);
        } else if (type.str == "portable_result") {
            parsed.payload = parse_portable_result(root);
        } else {
            return Error::InvalidField;
        }
        parsed.schema_version = static_cast<std::uint32_t>(version);
        const Error validation = Validate(parsed, now_unix_ms);
        if (validation != Error::None) return validation;
        message = std::move(parsed);
        return Error::None;
    } catch (const Failure& failure) {
        return failure.error;
    } catch (const std::runtime_error&) {
        return Error::MalformedJson;
    }
}

}  // namespace app::agent::wire
