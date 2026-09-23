#include "app/AgentState.h"

#include "data/Json.h"
#include "data/JsonWrite.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace app::agent {
namespace {

namespace fs = std::filesystem;

constexpr int kSchemaVersion = 5;
constexpr std::size_t kMaxStateBytes = 256 * 1024;
constexpr std::uint64_t kMaxJsonInteger = 9007199254740991ULL;
constexpr char kStateFilename[] = "agent-state.json";
std::atomic<std::uint64_t> g_temp_sequence{0};

[[noreturn]] void fail(const char* message) {
    throw std::runtime_error(std::string("agent state: ") + message);
}

bool valid_identifier(const std::string& id) {
    if (id.empty() || id.size() > kMaxStateIdentifierBytes) return false;
    for (unsigned char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            return false;
    }
    return true;
}

void require_identifier(const std::string& id, const char* name) {
    if (!valid_identifier(id)) fail(name);
}

void require_fingerprint(const std::string& value) {
    if (value.size() != 64 ||
        !std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }))
        fail("invalid command fingerprint");
}

bool terminal(AttemptState state) {
    return state == AttemptState::Succeeded || state == AttemptState::Failed ||
           state == AttemptState::Interrupted;
}

const char* state_name(AttemptState state) {
    switch (state) {
        case AttemptState::Accepted: return "accepted";
        case AttemptState::InProgress: return "in_progress";
        case AttemptState::Succeeded: return "succeeded";
        case AttemptState::Failed: return "failed";
        case AttemptState::Interrupted: return "interrupted";
    }
    fail("invalid assignment state");
}

AttemptState parse_state(const std::string& value) {
    if (value == "accepted") return AttemptState::Accepted;
    if (value == "in_progress") return AttemptState::InProgress;
    if (value == "succeeded") return AttemptState::Succeeded;
    if (value == "failed") return AttemptState::Failed;
    if (value == "interrupted") return AttemptState::Interrupted;
    fail("invalid assignment state");
}

const char* outcome_name(CommandOutcome outcome) {
    switch (outcome) {
        case CommandOutcome::Pending: return "pending";
        case CommandOutcome::Accepted: return "accepted";
        case CommandOutcome::Applied: return "applied";
        case CommandOutcome::Rejected: return "rejected";
        case CommandOutcome::Failed: return "failed";
    }
    fail("invalid command outcome");
}

CommandOutcome parse_outcome(const std::string& value) {
    if (value == "pending") return CommandOutcome::Pending;
    if (value == "accepted") return CommandOutcome::Accepted;
    if (value == "applied") return CommandOutcome::Applied;
    if (value == "rejected") return CommandOutcome::Rejected;
    if (value == "failed") return CommandOutcome::Failed;
    fail("invalid command outcome");
}

bool administrative_action(const std::string& action) {
    return action == "reboot_machine" || action == "activate_update";
}

bool valid_command_action(const std::string& action) {
    return action == "pause" || action == "resume" || action == "stop" ||
           action == "maintenance" || action == "online" ||
           action == "restart_service" ||
           action == "force_restart_service" ||
           administrative_action(action);
}

void validate_state(const State& state) {
    if (state.accepted.size() > kMaxAcceptedAttempts)
        fail("too many accepted assignments");
    if (state.recent_commands.size() > kMaxRecentCommands)
        fail("too many recent commands");
    if (state.command_leader_id.empty()) {
        if (state.command_leader_epoch || state.last_command_sequence ||
            !state.recent_commands.empty())
            fail("command leader is missing");
    } else {
        require_identifier(state.command_leader_id, "invalid command leader");
        if (!state.command_leader_epoch ||
            state.command_leader_epoch > kMaxJsonInteger ||
            state.last_command_sequence > kMaxJsonInteger ||
            (!state.last_command_sequence && !state.recent_commands.empty()))
            fail("invalid command watermark");
    }
    for (std::size_t i = 0; i < state.accepted.size(); ++i) {
        const AcceptedAssignment& item = state.accepted[i];
        require_identifier(item.job_id, "invalid job_id");
        require_identifier(item.attempt_id, "invalid attempt_id");
        (void)state_name(item.state);
        for (std::size_t j = 0; j < i; ++j)
            if (state.accepted[j].attempt_id == item.attempt_id)
                fail("duplicate attempt_id");
    }
    for (std::size_t i = 0; i < state.recent_commands.size(); ++i) {
        const CommandRecord& item = state.recent_commands[i];
        require_identifier(item.command_id, "invalid command_id");
        require_fingerprint(item.command_fingerprint);
        (void)outcome_name(item.outcome);
        if (!item.sequence || item.sequence > state.last_command_sequence ||
            (i && item.sequence <= state.recent_commands[i - 1].sequence))
            fail("invalid command sequence");
        if (item.command_action.empty()) {
            if (!item.target_job_id.empty() || item.confirmed ||
                item.outcome == CommandOutcome::Accepted)
                fail("command scope is missing");
        } else {
            if (!valid_command_action(item.command_action))
                fail("invalid command action");
            if (!item.target_job_id.empty())
                require_identifier(item.target_job_id, "invalid target job_id");
            const bool force_restart =
                item.command_action == "force_restart_service";
            const bool confirmation_required =
                item.command_action == "stop" || force_restart ||
                administrative_action(item.command_action);
            if (item.confirmed != confirmation_required ||
                (!item.target_job_id.empty() &&
                 item.command_action != "stop"))
                fail("invalid command scope");
        }
        for (std::size_t j = 0; j < i; ++j)
            if (state.recent_commands[j].command_id == item.command_id)
                fail("duplicate command_id");
    }
    if (state.restart_intent) {
        const RestartIntent& intent = *state.restart_intent;
        require_identifier(intent.command_id, "invalid restart command_id");
        const CommandRecord* command = find_command(state, intent.sequence);
        if (!command || command->command_id != intent.command_id ||
            command->outcome != CommandOutcome::Accepted ||
            command->command_action !=
                (intent.force ? "force_restart_service" : "restart_service") ||
            command->confirmed != intent.force)
            fail("restart intent does not match an accepted command");
    }
}

const JsonValue& required(const JsonValue& object, const char* name) {
    const JsonValue* value = object.find(name);
    if (!value) fail("missing field");
    return *value;
}

void require_fields(const JsonValue& object,
                    std::initializer_list<const char*> allowed) {
    if (!object.is_object()) fail("expected object");
    for (std::size_t i = 0; i < object.obj.size(); ++i) {
        const std::string& key = object.obj[i].first;
        bool known = false;
        for (const char* name : allowed)
            if (key == name) known = true;
        if (!known) fail("unknown field");
        for (std::size_t j = 0; j < i; ++j)
            if (object.obj[j].first == key) fail("duplicate field");
    }
    if (object.obj.size() != allowed.size()) fail("missing field");
}

std::string string_value(const JsonValue& value, const char* name) {
    if (value.type != JsonValue::Type::String) fail(name);
    return value.str;
}
std::uint64_t safe_integer(const JsonValue& value, const char* name) {
    if (value.type != JsonValue::Type::Number ||
        !std::isfinite(value.num) || value.num < 0 ||
        value.num > static_cast<double>(kMaxJsonInteger) ||
        std::floor(value.num) != value.num)
        fail(name);
    return static_cast<std::uint64_t>(value.num);
}


State parse_document(const std::string& text) {
    const JsonValue root = json_parse(text);
    const JsonValue& schema = required(root, "schema_version");
    if (schema.type != JsonValue::Type::Number ||
        !std::isfinite(schema.num) ||
        (schema.num != 3 && schema.num != 4 && schema.num != kSchemaVersion))
        fail("unsupported schema version");
    const bool has_command_scope = schema.num >= 4;
    if (schema.num == kSchemaVersion) {
        require_fields(root, {"schema_version", "command_leader_id",
                              "command_leader_epoch", "last_command_sequence",
                              "maintenance", "paused", "accepted",
                              "recent_commands", "restart_intent"});
    } else {
        require_fields(root, {"schema_version", "command_leader_id",
                              "command_leader_epoch", "last_command_sequence",
                              "maintenance", "paused", "accepted",
                              "recent_commands"});
    }
    const JsonValue& maintenance = required(root, "maintenance");
    const JsonValue& paused = required(root, "paused");
    if (maintenance.type != JsonValue::Type::Bool ||
        paused.type != JsonValue::Type::Bool)
        fail("maintenance and paused must be booleans");

    State state;
    state.command_leader_id = string_value(
        required(root, "command_leader_id"), "invalid command leader");
    state.command_leader_epoch = safe_integer(
        required(root, "command_leader_epoch"), "invalid command leader epoch");
    state.last_command_sequence = safe_integer(
        required(root, "last_command_sequence"), "invalid command watermark");
    state.maintenance = maintenance.b;
    state.paused = paused.b;

    const JsonValue& accepted = required(root, "accepted");
    if (!accepted.is_array() || accepted.arr.size() > kMaxAcceptedAttempts)
        fail("invalid accepted assignments");
    state.accepted.reserve(accepted.arr.size());
    for (const JsonValue& value : accepted.arr) {
        require_fields(value, {"job_id", "attempt_id", "state"});
        const std::string job_id = string_value(required(value, "job_id"), "invalid job_id");
        const std::string attempt_id = string_value(required(value, "attempt_id"), "invalid attempt_id");
        const AttemptState status = parse_state(
            string_value(required(value, "state"), "invalid assignment state"));
        state.accepted.emplace_back(job_id, attempt_id, status);
    }

    const JsonValue& recent = required(root, "recent_commands");
    if (!recent.is_array() || recent.arr.size() > kMaxRecentCommands)
        fail("invalid recent commands");
    for (const JsonValue& value : recent.arr) {
        if (has_command_scope) {
            require_fields(value, {"command_id", "sequence", "command_fingerprint",
                                   "outcome", "command_action", "target_job_id",
                                   "confirmed"});
        } else {
            require_fields(value, {"command_id", "sequence", "command_fingerprint",
                                   "outcome"});
        }
        const std::string command_id = string_value(
            required(value, "command_id"), "invalid command_id");
        const std::uint64_t sequence = safe_integer(
            required(value, "sequence"), "invalid command sequence");
        const std::string fingerprint = string_value(
            required(value, "command_fingerprint"), "invalid command fingerprint");
        const CommandOutcome outcome = parse_outcome(
            string_value(required(value, "outcome"), "invalid command outcome"));
        std::string action;
        std::string target_job_id;
        bool confirmed = false;
        if (has_command_scope) {
            action = string_value(required(value, "command_action"),
                                  "invalid command action");
            target_job_id = string_value(required(value, "target_job_id"),
                                         "invalid target job_id");
            const JsonValue& confirmed_value = required(value, "confirmed");
            if (confirmed_value.type != JsonValue::Type::Bool)
                fail("invalid command confirmation");
            confirmed = confirmed_value.b;
        }
        state.recent_commands.emplace_back(command_id, sequence, fingerprint,
                                           outcome, std::move(action),
                                           std::move(target_job_id), confirmed);
    }
    if (schema.num == kSchemaVersion) {
        const JsonValue& intent = required(root, "restart_intent");
        if (!intent.is_null()) {
            require_fields(intent, {"command_id", "sequence", "force"});
            const std::string command_id = string_value(
                required(intent, "command_id"), "invalid restart command_id");
            const std::uint64_t sequence = safe_integer(
                required(intent, "sequence"), "invalid restart sequence");
            const JsonValue& force = required(intent, "force");
            if (force.type != JsonValue::Type::Bool)
                fail("invalid restart force flag");
            state.restart_intent = RestartIntent{command_id, sequence, force.b};
        }
    }
    validate_state(state);
    return state;
}

std::string serialize(const State& state) {
    validate_state(state);
    JsonWriter writer;
    writer.object()
        .field("schema_version", kSchemaVersion)
        .field("command_leader_id", state.command_leader_id)
        .field("command_leader_epoch",
               static_cast<long long>(state.command_leader_epoch))
        .field("last_command_sequence",
               static_cast<long long>(state.last_command_sequence))
        .field("maintenance", state.maintenance)
        .field("paused", state.paused)
        .key("restart_intent");
    if (state.restart_intent) {
        writer.object()
            .field("command_id", state.restart_intent->command_id)
            .field("sequence",
                   static_cast<long long>(state.restart_intent->sequence))
            .field("force", state.restart_intent->force)
            .end();
    } else {
        writer.raw("null");
    }
    writer.key("accepted").array();
    for (const AcceptedAssignment& item : state.accepted)
        writer.object().field("job_id", item.job_id)
            .field("attempt_id", item.attempt_id)
            .field("state", state_name(item.state)).end();
    writer.end().key("recent_commands").array();
    for (const CommandRecord& item : state.recent_commands)
        writer.object().field("command_id", item.command_id)
            .field("sequence", static_cast<long long>(item.sequence))
            .field("command_fingerprint", item.command_fingerprint)
            .field("outcome", outcome_name(item.outcome))
            .field("command_action", item.command_action)
            .field("target_job_id", item.target_job_id)
            .field("confirmed", item.confirmed).end();
    const std::string text = writer.end().end().str();
    if (text.size() > kMaxStateBytes) fail("state file is too large");
    return text;
}

void require_root(const fs::path& root) {
    if (!root.is_absolute()) fail("state root must be absolute");
    const fs::path normalized = root.lexically_normal();
    if (normalized == normalized.root_path()) fail("state root cannot be a filesystem root");
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(normalized, ec);
    if (ec) throw std::system_error(ec, "cannot inspect agent state root");
    if (fs::is_symlink(status)) fail("state root cannot be a symlink");
    if (!fs::is_directory(status)) fail("state root is not an existing directory");
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(normalized.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(), "cannot inspect agent state root");
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
        fail("state root cannot be a reparse point");
#endif
}

fs::path state_path(const fs::path& root) {
    return root.lexically_normal() / kStateFilename;
}

std::optional<std::string> read_file(const fs::path& path) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory) return std::nullopt;
    if (ec) throw std::system_error(ec, "cannot inspect agent state file");
    if (status.type() == fs::file_type::not_found) return std::nullopt;
    if (!fs::is_regular_file(status)) fail("state file is not a regular file");
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(), "cannot inspect agent state file");
    if (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
        fail("state file is not a regular file");
#endif

    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec) throw std::system_error(ec, "cannot size agent state file");
    if (size > kMaxStateBytes) fail("state file is too large");
    std::ifstream input(path, std::ios::binary);
    if (!input) fail("cannot open state file");
    std::string text(static_cast<std::size_t>(size), '\0');
    if (size != 0) {
        input.read(text.data(), static_cast<std::streamsize>(size));
        if (input.gcount() != static_cast<std::streamsize>(size))
            fail("truncated state file");
    }
    if (input.peek() != std::char_traits<char>::eof())
        fail("state file changed while reading");
    if (input.bad()) fail("I/O error reading state file");
    return text;
}


std::string temporary_suffix() {
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
    return ".tmp-" + std::to_string(pid) + "-" +
           std::to_string(g_temp_sequence.fetch_add(1, std::memory_order_relaxed));
}

void require_publish_target(const fs::path& target) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(target, ec);
    if (ec == std::errc::no_such_file_or_directory) return;
    if (ec) throw std::system_error(ec, "cannot inspect agent state file");
    if (status.type() == fs::file_type::not_found) return;
    if (!fs::is_regular_file(status)) fail("state file is not a regular file");
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(target.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return;
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "cannot inspect agent state file");
    }
    if (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
        fail("state file is not a regular file");
#endif
}

#ifdef _WIN32
[[noreturn]] void fail_windows(const char* operation) {
    throw std::system_error(static_cast<int>(GetLastError()),
                            std::system_category(), operation);
}

void publish(const fs::path& root, const fs::path& target,
             const std::string& text, bool machine = false) {
    fs::path temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    bool published = false;
    try {
        for (int attempt = 0; attempt < 128; ++attempt) {
            temporary = root / (std::string(kStateFilename) + temporary_suffix());
            file = CreateFileW(temporary.c_str(),
                               GENERIC_WRITE | (machine ? WRITE_OWNER : 0), 0,
                               nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) break;
            if (GetLastError() != ERROR_FILE_EXISTS &&
                GetLastError() != ERROR_ALREADY_EXISTS)
                fail_windows("cannot create temporary agent state file");
        }
        if (file == INVALID_HANDLE_VALUE) fail("cannot allocate temporary state file");

        std::size_t offset = 0;
        while (offset < text.size()) {
            const DWORD amount = static_cast<DWORD>(text.size() - offset);
            DWORD written = 0;
            if (!WriteFile(file, text.data() + offset, amount, &written, nullptr))
                fail_windows("cannot write temporary agent state file");
            if (written == 0) fail("short write of agent state file");
            offset += written;
        }
        if (!FlushFileBuffers(file)) fail_windows("cannot flush agent state file");
        if (machine) {
            std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> admins{};
            DWORD size = static_cast<DWORD>(admins.size());
            BOOL member = FALSE;
            if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
                                    admins.data(), &size) ||
                !CheckTokenMembership(nullptr, admins.data(), &member) || !member)
                fail("machine state requires an elevated local administrator");
            const DWORD result = SetSecurityInfo(
                file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                admins.data(), nullptr, nullptr, nullptr);
            if (result != ERROR_SUCCESS)
                throw std::system_error(static_cast<int>(result),
                                        std::system_category(),
                                        "cannot assign machine state ownership");
        }
        if (!CloseHandle(file)) {
            file = INVALID_HANDLE_VALUE;
            fail_windows("cannot close agent state file");
        }
        file = INVALID_HANDLE_VALUE;
        if (!MoveFileExW(temporary.c_str(), target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            fail_windows("cannot replace agent state file");
        published = true;
    } catch (...) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        if (!published && !temporary.empty()) DeleteFileW(temporary.c_str());
        throw;
    }
}
#else
[[noreturn]] void fail_errno(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

void publish(const fs::path& root, const fs::path& target,
             const std::string& text) {
    fs::path temporary;
    int file = -1;
    bool published = false;
    try {
        for (int attempt = 0; attempt < 128; ++attempt) {
            temporary = root / (std::string(kStateFilename) + temporary_suffix());
            file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                          0600);
            if (file >= 0) break;
            if (errno != EEXIST) fail_errno("cannot create temporary agent state file");
        }
        if (file < 0) fail("cannot allocate temporary state file");

        std::size_t offset = 0;
        while (offset < text.size()) {
            const ssize_t written = ::write(file, text.data() + offset,
                                            text.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written < 0) fail_errno("cannot write temporary agent state file");
            if (written == 0) fail("short write of agent state file");
            offset += static_cast<std::size_t>(written);
        }
        if (::fsync(file) != 0) fail_errno("cannot flush agent state file");
        if (::close(file) != 0) {
            file = -1;
            fail_errno("cannot close agent state file");
        }
        file = -1;
        if (::rename(temporary.c_str(), target.c_str()) != 0)
            fail_errno("cannot replace agent state file");
        published = true;

        const int directory = ::open(root.c_str(), O_RDONLY | O_CLOEXEC);
        if (directory < 0) fail_errno("cannot open agent state directory");
        const int sync_result = ::fsync(directory);
        const int sync_error = errno;
        const int close_result = ::close(directory);
        if (sync_result != 0) {
            errno = sync_error;
            fail_errno("cannot flush agent state directory");
        }
        if (close_result != 0) fail_errno("cannot close agent state directory");
    } catch (...) {
        if (file >= 0) ::close(file);
        if (!published && !temporary.empty()) ::unlink(temporary.c_str());
        throw;
    }
}
#endif

}  // namespace

namespace {
State load_impl(const fs::path& state_root, bool machine) {
    require_root(state_root);
    const std::optional<std::string> text = read_file(state_path(state_root));
    if (!text) return {};

    State state = parse_document(*text);
    bool recovered = false;
    for (AcceptedAssignment& item : state.accepted) {
        if (item.state == AttemptState::InProgress) {
            item.state = AttemptState::Interrupted;
            recovered = true;
        }
    }
    if (recovered) {
        if (machine) save_machine_state(state_root, state);
        else save_state(state_root, state);
    }
    return state;
}
}  // namespace

State load_state(const fs::path& state_root) {
    return load_impl(state_root, false);
}

State load_machine_state(const fs::path& state_root) {
#ifdef _WIN32
    return load_impl(state_root, true);
#else
    (void)state_root;
    fail("machine state requires Windows service permissions");
#endif
}


void save_state(const fs::path& state_root, const State& state) {
    require_root(state_root);
    const std::string text = serialize(state);
    const fs::path target = state_path(state_root);
    require_publish_target(target);
    publish(state_root, target, text);
}
void save_machine_state(const fs::path& state_root, const State& state) {
#ifdef _WIN32
    require_root(state_root);
    const std::string text = serialize(state);
    const fs::path target = state_path(state_root);
    require_publish_target(target);
    publish(state_root, target, text, true);
#else
    (void)state_root;
    (void)state;
    fail("machine state requires Windows service permissions");
#endif
}
void accept_assignment(State& state, std::string job_id, std::string attempt_id) {
    validate_state(state);
    require_identifier(job_id, "invalid job_id");
    require_identifier(attempt_id, "invalid attempt_id");
    for (const AcceptedAssignment& item : state.accepted)
        if (item.attempt_id == attempt_id) fail("duplicate attempt_id");

    if (state.accepted.size() == kMaxAcceptedAttempts) {
        const auto oldest_terminal = std::find_if(
            state.accepted.begin(), state.accepted.end(),
            [](const AcceptedAssignment& item) { return terminal(item.state); });
        if (oldest_terminal == state.accepted.end())
            fail("too many unfinished assignments");
        std::vector<AcceptedAssignment> retained;
        retained.reserve(state.accepted.size() - 1);
        for (auto it = state.accepted.begin(); it != state.accepted.end(); ++it)
            if (it != oldest_terminal)
                retained.emplace_back(it->job_id, it->attempt_id, it->state);
        state.accepted.swap(retained);
    }
    state.accepted.emplace_back(std::move(job_id), std::move(attempt_id));
}

void transition_assignment(State& state, const std::string& attempt_id,
                           AttemptState next) {
    validate_state(state);
    for (AcceptedAssignment& item : state.accepted) {
        if (item.attempt_id != attempt_id) continue;
        if (item.state == next && next != AttemptState::Interrupted) return;
        const bool allowed =
            (item.state == AttemptState::Accepted && next == AttemptState::InProgress) ||
            (item.state == AttemptState::InProgress &&
             (next == AttemptState::Succeeded || next == AttemptState::Failed ||
              next == AttemptState::Interrupted));
        if (!allowed) fail("invalid assignment transition");
        item.state = next;
        return;
    }
    fail("unknown attempt_id");
}

void rebind_command_leader(State& state, std::string leader_id,
                           std::uint64_t leader_epoch) {
    validate_state(state);
    if (state.restart_intent ||
        std::any_of(state.recent_commands.begin(),
                    state.recent_commands.end(),
                    [](const CommandRecord& item) {
                        return administrative_action(item.command_action) &&
                               (item.outcome == CommandOutcome::Pending ||
                                item.outcome == CommandOutcome::Accepted);
                    }))
        fail("cannot rebind a leader while an operation is pending");
    require_identifier(leader_id, "invalid command leader");
    if (!leader_epoch || leader_epoch > kMaxJsonInteger)
        fail("invalid command leader epoch");
    state.command_leader_id = std::move(leader_id);
    state.command_leader_epoch = leader_epoch;
    state.last_command_sequence = 0;
    state.recent_commands.clear();
}

const CommandRecord* find_command(const State& state,
                                  std::uint64_t sequence) noexcept {
    for (const CommandRecord& item : state.recent_commands)
        if (item.sequence == sequence) return &item;
    return nullptr;
}

void begin_command(State& state, std::string leader_id,
                   std::uint64_t leader_epoch, std::uint64_t sequence,
                   std::string command_id, std::string command_fingerprint,
                   std::string action, std::string target_job_id,
                   bool confirmed) {
    validate_state(state);
    require_identifier(leader_id, "invalid command leader");
    require_identifier(command_id, "invalid command_id");
    if (!leader_epoch || leader_epoch > kMaxJsonInteger ||
        !sequence || sequence > kMaxJsonInteger)
        fail("invalid command sequence");
    require_fingerprint(command_fingerprint);
    if (action.empty()) {
        if (!target_job_id.empty() || confirmed)
            fail("command scope is missing");
    } else {
        if (!valid_command_action(action))
            fail("invalid command action");
        if (!target_job_id.empty())
            require_identifier(target_job_id, "invalid target job_id");
        const bool confirmation_required =
            action == "stop" || action == "force_restart_service" ||
            administrative_action(action);
        if (confirmed != confirmation_required ||
            (!target_job_id.empty() && action != "stop"))
            fail("invalid command scope");
    }
    if (state.command_leader_id.empty())
        rebind_command_leader(state, std::move(leader_id), leader_epoch);
    else if (state.command_leader_id != leader_id ||
             state.command_leader_epoch != leader_epoch)
        fail("command leader does not match paired identity");
    if (sequence <= state.last_command_sequence)
        fail("replayed command sequence");
    for (const CommandRecord& item : state.recent_commands)
        if (item.command_id == command_id) fail("duplicate command_id");
    if (state.recent_commands.size() == kMaxRecentCommands) {
        const auto evictable = std::find_if(
            state.recent_commands.begin(), state.recent_commands.end(),
            [](const CommandRecord& item) {
                return item.outcome != CommandOutcome::Pending &&
                       item.outcome != CommandOutcome::Accepted;
            });
        if (evictable == state.recent_commands.end())
            fail("command history is full with outstanding outcomes");
        state.recent_commands.erase(evictable);
    }
    state.recent_commands.emplace_back(
        std::move(command_id), sequence, std::move(command_fingerprint),
        CommandOutcome::Pending, std::move(action), std::move(target_job_id),
        confirmed);
    state.last_command_sequence = sequence;
    validate_state(state);
}

bool set_restart_intent(State& state, std::uint64_t sequence, bool force) {
    validate_state(state);
    const CommandRecord* command = find_command(state, sequence);
    if (!command || command->outcome != CommandOutcome::Accepted ||
        command->command_action !=
            (force ? "force_restart_service" : "restart_service") ||
        command->confirmed != force)
        fail("restart intent requires its accepted restart command");
    if (state.restart_intent) {
        if (state.restart_intent->sequence == sequence &&
            state.restart_intent->command_id == command->command_id &&
            state.restart_intent->force == force)
            return false;
        fail("another restart intent is already pending");
    }
    state.restart_intent = RestartIntent{command->command_id, sequence, force};
    validate_state(state);
    return true;
}

void complete_restart_intent(State& state, std::uint64_t sequence) {
    validate_state(state);
    if (!state.restart_intent ||
        state.restart_intent->sequence != sequence)
        fail("restart intent is not pending");
    const CommandRecord* command = find_command(state, sequence);
    if (!command ||
        command->command_id != state.restart_intent->command_id ||
        command->outcome != CommandOutcome::Accepted)
        fail("restart intent does not match its accepted command");
    state.restart_intent.reset();
    finish_command(state, sequence, CommandOutcome::Applied);
    validate_state(state);
}

void finish_command(State& state, std::uint64_t sequence,
                    CommandOutcome outcome) {
    validate_state(state);
    if (outcome == CommandOutcome::Pending) fail("command is still pending");
    (void)outcome_name(outcome);
    for (CommandRecord& item : state.recent_commands) {
        if (item.sequence != sequence) continue;
        if (item.outcome != CommandOutcome::Pending &&
            item.outcome != CommandOutcome::Accepted) {
            if (item.outcome == CommandOutcome::Applied &&
                outcome == CommandOutcome::Failed &&
                administrative_action(item.command_action)) {
                item.outcome = outcome;
                validate_state(state);
                return;
            }
            fail("command already completed");
        }
        if (item.outcome == CommandOutcome::Accepted &&
            outcome != CommandOutcome::Applied &&
            outcome != CommandOutcome::Failed)
            fail("invalid accepted command transition");
        item.outcome = outcome;
        validate_state(state);
        return;
    }
    fail("unknown command sequence");
}

}  // namespace app::agent
