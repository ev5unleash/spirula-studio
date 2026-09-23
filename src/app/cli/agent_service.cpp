#include "app/AgentConfig.h"
#include "app/AgentAdminBroker.h"
#include "app/AgentClient.h"
#include "app/AgentServiceManager.h"
#include "app/AgentState.h"
#include "app/AppPaths.h"
#include "app/CrashLog.h"
#include "app/Subprocess.h"
#include "core/Sha256.h"
#include "data/Json.h"
#include "data/JsonWrite.h"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

#ifdef _WIN32
namespace {

constexpr wchar_t kServiceName[] = L"SpirulaRemoteWorker";
constexpr wchar_t kServiceAccount[] = L"NT SERVICE\\SpirulaRemoteWorker";

std::string g_device;
std::string g_evidence;
std::atomic<bool> g_stop{false};
SERVICE_STATUS_HANDLE g_status_handle = nullptr;

std::string wide_to_utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()), nullptr,
                                         0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}

void set_status(DWORD state, DWORD error = NO_ERROR,
                DWORD service_error = 0, DWORD wait_hint = 0) {
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_RUNNING
        ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN
        : 0;
    status.dwWin32ExitCode = error;
    status.dwServiceSpecificExitCode = service_error;
    status.dwWaitHint = wait_hint;
    SetServiceStatus(g_status_handle, &status);
}

struct Identity {
    std::string account;
    std::string sid;
    std::string expected_sid;
    DWORD restricted_sid_count = 0;
    DWORD session_id = 0;
    bool virtual_account = false;
    bool service_sid_restricted = false;
    std::string error;
};

Identity current_identity() {
    Identity result;
    DWORD expected_size = 0;
    DWORD expected_domain_size = 0;
    SID_NAME_USE expected_use{};
    LookupAccountNameW(nullptr, kServiceAccount, nullptr, &expected_size,
                       nullptr, &expected_domain_size, &expected_use);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !expected_size) {
        result.error = "cannot resolve service account SID";
        return result;
    }
    std::vector<uint8_t> expected_buffer(expected_size);
    std::wstring expected_domain(expected_domain_size, L'\0');
    if (!LookupAccountNameW(nullptr, kServiceAccount, expected_buffer.data(),
                            &expected_size, expected_domain.data(),
                            &expected_domain_size, &expected_use)) {
        result.error = "cannot resolve service account SID";
        return result;
    }
    PSID expected_sid = expected_buffer.data();
    LPWSTR expected_text = nullptr;
    if (ConvertSidToStringSidW(expected_sid, &expected_text)) {
        result.expected_sid = wide_to_utf8(expected_text);
        LocalFree(expected_text);
    } else {
        result.error = "cannot format service account SID";
        return result;
    }

    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        result.error = "cannot query service process token";
        return result;
    }

    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<uint8_t> user_buffer(size);
    if (size && GetTokenInformation(token, TokenUser, user_buffer.data(), size,
                                    &size)) {
        const auto* user = reinterpret_cast<const TOKEN_USER*>(user_buffer.data());
        result.virtual_account = EqualSid(user->User.Sid, expected_sid) != 0;
        LPWSTR sid_text = nullptr;
        if (ConvertSidToStringSidW(user->User.Sid, &sid_text)) {
            result.sid = wide_to_utf8(sid_text);
            LocalFree(sid_text);
        }

        DWORD name_size = 0;
        DWORD domain_size = 0;
        SID_NAME_USE use{};
        LookupAccountSidW(nullptr, user->User.Sid, nullptr, &name_size, nullptr,
                          &domain_size, &use);
        std::wstring name(name_size, L'\0');
        std::wstring domain(domain_size, L'\0');
        if (name_size && LookupAccountSidW(nullptr, user->User.Sid, name.data(),
                                           &name_size, domain.data(),
                                           &domain_size, &use)) {
            name.resize(name_size);
            domain.resize(domain_size);
            result.account = wide_to_utf8(domain.empty() ? name
                                                          : domain + L"\\" + name);
        }

        DWORD restricted_size = 0;
        GetTokenInformation(token, TokenRestrictedSids, nullptr, 0,
                            &restricted_size);
        std::vector<uint8_t> restricted_buffer(restricted_size);
        if (restricted_size &&
            GetTokenInformation(token, TokenRestrictedSids,
                                restricted_buffer.data(), restricted_size,
                                &restricted_size)) {
            const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(
                restricted_buffer.data());
            result.restricted_sid_count = groups->GroupCount;
            for (DWORD i = 0; i < groups->GroupCount; ++i)
                if (EqualSid(expected_sid, groups->Groups[i].Sid))
                    result.service_sid_restricted = true;
        }
    } else {
        result.error = "cannot query service token identity";
    }
    CloseHandle(token);
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &result.session_id)) {
        result.error = "cannot query service session";
    } else if (!result.virtual_account) {
        result.error = "service is not running as its virtual account";
    } else if (!result.service_sid_restricted) {
        result.error = "service SID is not restricted";
    } else if (result.session_id != 0) {
        result.error = "service is not running in session zero";
    }
    return result;
}

const char* outcome_name(app::proc::ProcessOutcome outcome) {
    switch (outcome) {
        case app::proc::ProcessOutcome::Success: return "success";
        case app::proc::ProcessOutcome::SpawnFailed: return "spawn_failed";
        case app::proc::ProcessOutcome::Cancelled: return "cancelled";
        case app::proc::ProcessOutcome::Stopped: return "stopped";
        case app::proc::ProcessOutcome::Crashed: return "crashed";
    }
    return "unknown";
}

bool valid_probe(const std::string& text) {
    try {
        const JsonValue root = json_parse(text);
        const JsonValue* success = root.find("success");
        const JsonValue* lease = root.find("lease_acquired");
        const JsonValue* backend = root.find("backend");
        const JsonValue* device = root.find("device");
        return root.type == JsonValue::Type::Object && success &&
               success->type == JsonValue::Type::Bool && success->b && lease &&
               lease->type == JsonValue::Type::Bool && lease->b && backend &&
               backend->type == JsonValue::Type::String &&
               backend->str == "vulkan" && device &&
               device->type == JsonValue::Type::String &&
               device->str == g_device;
    } catch (...) {
        return false;
    }
}

bool safe_evidence_directory(const fs::path& directory,
                             bool allow_missing_tail) {
    if (directory.empty() || !directory.is_absolute() ||
        directory == directory.root_path())
        return false;

    fs::path current = directory.root_path();
    for (const fs::path& part : directory.relative_path()) {
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
        current /= part;
        const DWORD child = GetFileAttributesW(current.c_str());
        if (child == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            return allow_missing_tail &&
                   (error == ERROR_FILE_NOT_FOUND ||
                    error == ERROR_PATH_NOT_FOUND);
        }
        if (!(child & FILE_ATTRIBUTE_DIRECTORY) ||
            (child & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
    }
    return true;
}

bool safe_evidence_target(const fs::path& target) {
    const DWORD attributes = GetFileAttributesW(target.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    return !(attributes & (FILE_ATTRIBUTE_DIRECTORY |
                           FILE_ATTRIBUTE_REPARSE_POINT));
}

bool write_evidence(const std::string& text) {
    const fs::path target = fs::u8path(g_evidence);
    const fs::path parent = target.parent_path();
    if (!safe_evidence_directory(parent, true)) return false;
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec || !safe_evidence_directory(parent, false) ||
        !safe_evidence_target(target))
        return false;

    fs::path temporary = target;
    temporary += ".tmp." + std::to_string(GetCurrentProcessId());
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL |
                                  FILE_FLAG_OPEN_REPARSE_POINT,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    bool okay = text.size() <= static_cast<size_t>(MAXDWORD);
    DWORD written = 0;
    if (okay)
        okay = WriteFile(file, text.data(), static_cast<DWORD>(text.size()),
                         &written, nullptr) &&
               written == text.size() && FlushFileBuffers(file);
    if (!CloseHandle(file)) okay = false;
    if (!okay) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (!safe_evidence_target(target) ||
        !MoveFileExW(temporary.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

DWORD WINAPI service_handler(DWORD control, DWORD, void*, void*) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        g_stop.store(true);
        set_status(SERVICE_STOP_PENDING, NO_ERROR, 0, 10000);
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI service_main(DWORD, LPWSTR*) {
    g_status_handle = RegisterServiceCtrlHandlerExW(kServiceName,
                                                    service_handler, nullptr);
    if (!g_status_handle) return;
    set_status(SERVICE_START_PENDING, NO_ERROR, 0, 30000);
    set_status(SERVICE_RUNNING);

    const Identity identity = current_identity();
    const bool identity_ok = identity.error.empty();
    const std::string executable_sha256 = spirula::sha256_file(app::exe_path());
    const bool fingerprint_ok = !executable_sha256.empty();
    uint64_t child_pid = 0;
    std::string probe_output;

    app::proc::ProcessOptions options;
    options.argv = {app::exe_path(), "agent", "probe", "--device", g_device,
                    "--hold-ms", "1000"};
    options.cwd = app::exe_dir();
    options.cancel = &g_stop;
    options.on_started = [&](uint64_t pid) { child_pid = pid; };
    options.on_line = [&](const std::string& line) {
        probe_output += line;
        probe_output += '\n';
    };
    options.env_overrides.push_back(
        {"SS_CRASH_DIR", fs::u8path(g_evidence).parent_path().u8string()});
    const app::proc::ProcessResult process = app::proc::run_process(options);
    const bool probe_ok = process.outcome == app::proc::ProcessOutcome::Success &&
                           process.exit_code == 0 && valid_probe(probe_output);
    const bool success = identity_ok && fingerprint_ok && probe_ok;

    JsonWriter writer;
    writer.object()
        .field("success", success)
        .field("service", "SpirulaRemoteWorker")
        .field("service_pid", static_cast<long long>(GetCurrentProcessId()))
        .field("child_pid", static_cast<long long>(child_pid))
        .field("session_id", static_cast<long long>(identity.session_id))
        .field("account", identity.account)
        .field("user_sid", identity.sid)
        .field("expected_service_sid", identity.expected_sid)
        .field("virtual_account", identity.virtual_account)
        .field("restricted_sid_count",
               static_cast<long long>(identity.restricted_sid_count))
        .field("service_sid_restricted", identity.service_sid_restricted)
        .field("identity_error", identity.error)
        .field("executable_sha256", executable_sha256)
        .field("process_outcome", outcome_name(process.outcome))
        .field("probe_exit_code", process.exit_code)
        .field("process_error", process.error_message);
    if (probe_ok)
        writer.field_raw("probe", probe_output);
    else
        writer.field("probe_output", probe_output);
    writer.end();

    const bool evidence_ok = write_evidence(writer.str());
    const DWORD service_error = !evidence_ok ? 2 : success ? 0 : 1;
    set_status(SERVICE_STOPPED,
               service_error ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR,
               service_error);
}

bool parse_options(int argc, char** argv, std::string& error) {
    bool have_device = false;
    bool have_evidence = false;
    for (int i = 2; i < argc; ++i) {
        if (!argv[i]) break;
        if (std::strcmp(argv[i], "--device") == 0 && !have_device &&
            i + 1 < argc && argv[i + 1] && argv[i + 1][0]) {
            g_device = argv[++i];
            have_device = true;
        } else if (std::strcmp(argv[i], "--evidence") == 0 && !have_evidence &&
                   i + 1 < argc && argv[i + 1] && argv[i + 1][0]) {
            g_evidence = argv[++i];
            have_evidence = true;
        } else {
            error = "invalid service probe option";
            return false;
        }
    }
    if (!have_device || !have_evidence) {
        error = "service probe requires --device and --evidence";
        return false;
    }
    if (!fs::u8path(g_evidence).is_absolute()) {
        error = "service probe evidence path must be absolute";
        return false;
    }
    return true;
}

}  // namespace
#endif

int spirula_agent_service_probe_main(int argc, char** argv) {
#ifndef _WIN32
    (void)argc;
    (void)argv;
    std::fputs("service probe is supported only on Windows\n", stderr);
    return 1;
#else
    std::string error;
    if (!parse_options(argc, argv, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(kServiceName), service_main},
        {nullptr, nullptr},
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        std::fprintf(stderr, "StartServiceCtrlDispatcherW failed: %lu\n",
                     static_cast<unsigned long>(GetLastError()));
        return 1;
    }
    return 0;
#endif
}

namespace {

struct AgentRoots {
    fs::path config_root;
    fs::path state_root;
    fs::path storage_root;
};
std::string g_service_activation_id;

#ifdef _WIN32
std::atomic<bool> g_host_stop{false};
AgentRoots g_service_roots;
#else
volatile std::sig_atomic_t g_host_stop = 0;
#endif

bool parse_agent_roots(int argc, char** argv, AgentRoots& roots,
                       std::string& error) {
    g_service_activation_id.clear();
    bool have_activation = false;
    bool have_config = false;
    bool have_state = false;
    bool have_storage = false;
    for (int i = 2; i < argc; ++i) {
        if (!argv[i]) {
            error = "invalid agent host arguments";
            return false;
        }
        if (std::strcmp(argv[i], "--broker-activation") == 0) {
            if (have_activation || i + 1 >= argc || !argv[i + 1] ||
                std::strlen(argv[i + 1]) != 64) {
                error = "broker activation token must be supplied once as 64 lowercase hex characters";
                return false;
            }
            const char* token = argv[++i];
            for (int j = 0; j < 64; ++j)
                if (!((token[j] >= '0' && token[j] <= '9') ||
                      (token[j] >= 'a' && token[j] <= 'f'))) {
                    error = "broker activation token is malformed";
                    return false;
                }
            have_activation = true;
            g_service_activation_id = token;
            continue;
        }
        fs::path* root = nullptr;
        bool* seen = nullptr;
        if (std::strcmp(argv[i], "--config-root") == 0) {
            root = &roots.config_root;
            seen = &have_config;
        } else if (std::strcmp(argv[i], "--state-root") == 0) {
            root = &roots.state_root;
            seen = &have_state;
        } else if (std::strcmp(argv[i], "--storage-root") == 0) {
            root = &roots.storage_root;
            seen = &have_storage;
        } else {
            error = "unsupported agent host option";
            return false;
        }
        if (*seen || i + 1 >= argc || !argv[i + 1] || !argv[i + 1][0]) {
            error = "agent host roots must be specified once with a value";
            return false;
        }
        *seen = true;
        *root = fs::u8path(argv[++i]);
    }
    if (!have_config || !have_state || !have_storage) {
        error = "agent host requires --config-root, --state-root, and "
                "--storage-root";
        return false;
    }
    return true;
}

bool validate_root(const fs::path& root, const char* name, bool service_owned,
                   std::string& error) {
    if (root.empty() || !root.is_absolute()) {
        error = std::string(name) + " must be an absolute path";
        return false;
    }
    if (root == root.root_path()) {
        error = std::string(name) + " must not be a filesystem root";
        return false;
    }
    for (const fs::path& part : root.relative_path()) {
        if (part == "." || part == "..") {
            error = std::string(name) + " must not contain dot path components";
            return false;
        }
    }

    fs::path current = root.root_path();
#ifdef _WIN32
    if (GetDriveTypeW(current.c_str()) == DRIVE_REMOTE) {
        error = std::string(name) + " must be on a local filesystem";
        return false;
    }
#endif
    for (const fs::path& part : root.relative_path()) {
        current /= part;
#ifdef _WIN32
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            error = std::string(name) + " cannot be inspected";
            return false;
        }
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            error = std::string(name) + " must not use reparse points";
            return false;
        }
#else
        std::error_code ec;
        const fs::file_status component = fs::symlink_status(current, ec);
        if (ec || component.type() == fs::file_type::not_found) {
            error = std::string(name) + " does not exist or cannot be inspected";
            return false;
        }
        if (fs::is_symlink(component)) {
            error = std::string(name) + " must not use symbolic links";
            return false;
        }
#endif
    }

    std::error_code ec;
    const fs::file_status status = fs::status(root, ec);
    if (ec || !fs::is_directory(status)) {
        error = std::string(name) + " must be an existing directory";
        return false;
    }
#ifndef _WIN32
    constexpr fs::perms writable_by_others =
        fs::perms::group_write | fs::perms::others_write;
    if ((status.permissions() & writable_by_others) != fs::perms::none) {
        error = std::string(name) + " must not be group- or world-writable";
        return false;
    }
    constexpr fs::perms service_access =
        fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec;
    if (service_owned &&
        (status.permissions() & service_access) != service_access) {
        error = std::string(name) + " must be readable and writable by the "
                "service account";
        return false;
    }
    struct stat info {};
    if (::stat(root.c_str(), &info) != 0) {
        error = std::string(name) + " cannot be inspected";
        return false;
    }
    if (service_owned && info.st_uid != ::geteuid()) {
        error = std::string(name) + " must be owned by the service account";
        return false;
    }
#else
    if (service_owned) {
        HANDLE directory = CreateFileW(
            root.c_str(),
            FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (directory == INVALID_HANDLE_VALUE) {
            error = std::string(name) + " is not writable by the service account";
            return false;
        }
        CloseHandle(directory);
    }
#endif
    return true;
}

bool prepare_agent_host(const AgentRoots& roots, app::agent::Config& config,
                        app::agent::State& state, std::string& error) {
#ifndef _WIN32
    if (::geteuid() == 0) {
        error = "agent host must run as a dedicated non-root account";
        return false;
    }
#endif
    if (!validate_root(roots.config_root, "config root", false, error) ||
        !validate_root(roots.state_root, "state root", true, error) ||
        !validate_root(roots.storage_root, "storage root", true, error))
        return false;
    std::error_code ec;
    const fs::path* validated_roots[] = {
        &roots.config_root, &roots.state_root, &roots.storage_root};
    for (int i = 0; i < 3; ++i) {
        for (int j = i + 1; j < 3; ++j) {
            ec.clear();
            if (fs::equivalent(*validated_roots[i], *validated_roots[j], ec)) {
                error = "config, state, and storage roots must be distinct";
                return false;
            }
            if (ec) {
                error = "agent roots cannot be compared: " + ec.message();
                return false;
            }
        }
    }

    try {
        config = app::agent::load_config(roots.config_root);
    } catch (const std::exception& e) {
        error = std::string("agent policy validation failed: ") + e.what();
        return false;
    }
    const app::agent::service::Result state_security =
        app::agent::service::validate_state_security(roots.state_root);
    if (!state_security.success) {
        error = "agent state security validation failed: " + state_security.error;
        return false;
    }
    try {
        state = app::agent::load_state(roots.state_root);
    } catch (const std::exception& e) {
        error = std::string("agent state validation failed: ") + e.what();
        return false;
    } catch (...) {
        error = "agent state validation failed";
        return false;
    }
    return true;
}

std::string host_status(const app::agent::State& state) {
    return std::string("agent host process running (authenticated feature "
                       "execution depends on compatible Vulkan/SFM build and "
                       "local device policy; maintenance=") +
           (state.maintenance ? "true" : "false") + "; paused=" +
           (state.paused ? "true" : "false") + ")\n";
}

bool agent_stop_requested(void*) noexcept {
#ifdef _WIN32
    return g_host_stop.load(std::memory_order_relaxed);
#else
    return g_host_stop != 0;
#endif
}

#ifdef _WIN32
BOOL WINAPI agent_console_handler(DWORD event) {
    switch (event) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_host_stop.store(true);
            return TRUE;
        default:
            return FALSE;
    }
}

DWORD WINAPI agent_service_handler(DWORD control, DWORD, void*, void*) {
    if (control == SERVICE_CONTROL_STOP ||
        control == SERVICE_CONTROL_SHUTDOWN) {
        g_host_stop.store(true);
        set_status(SERVICE_STOP_PENDING, NO_ERROR, 0, 10000);
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

bool agent_broker_readiness(
    const app::agent::wire::Status& status, void* context) noexcept {
    const auto* activation_id = static_cast<const std::string*>(context);
    if (!activation_id || activation_id->empty()) return false;
    try {
        const app::agent::service::Result result =
            app::agent::admin::report_worker_ready(*activation_id, status);
        if (!result.success)
            OutputDebugStringA(("broker rejected worker readiness: " +
                                result.error + "\n").c_str());
        return result.success;
    } catch (...) {
        return false;
    }
}

void WINAPI agent_service_entry(DWORD, LPWSTR*) {
    g_status_handle = RegisterServiceCtrlHandlerExW(
        kServiceName, agent_service_handler, nullptr);
    if (!g_status_handle) return;
    set_status(SERVICE_START_PENDING, NO_ERROR, 0, 30000);

    try {
        const Identity identity = current_identity();
        if (!identity.error.empty()) {
            OutputDebugStringA(("agent service identity validation failed: " +
                                identity.error + "\n").c_str());
            set_status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 1);
            return;
        }

        app::agent::Config config;
        app::agent::State state;
        std::string error;
        if (!prepare_agent_host(g_service_roots, config, state, error)) {
            OutputDebugStringA(("agent service startup validation failed: " +
                                error + "\n").c_str());
            set_status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 2);
            return;
        }

        app::install_crash_log(g_service_roots.state_root.u8string());
        app::set_crash_note("agent host (M2 feature worker)");
        g_host_stop.store(false);
        set_status(SERVICE_RUNNING);
        const std::string status = host_status(state);
        OutputDebugStringA(status.c_str());
        const app::agent::HostExit exit = app::agent::RunAgentClient(
            config, g_service_roots.state_root, g_service_roots.storage_root,
            true, state, agent_stop_requested, nullptr,
            g_service_activation_id.empty() ? nullptr : agent_broker_readiness,
            g_service_activation_id.empty() ? nullptr : &g_service_activation_id);
        if (exit == app::agent::HostExit::RestartRequested)
            set_status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 4);
        else
            set_status(SERVICE_STOPPED);
    } catch (const std::exception& e) {
        OutputDebugStringA((std::string("agent service startup failed: ") +
                            e.what() + "\n").c_str());
        set_status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 3);
    } catch (...) {
        OutputDebugStringA("agent service startup failed\n");
        set_status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 3);
    }
}
#else
void agent_stop_signal(int) {
    g_host_stop = 1;
}
#endif

}  // namespace

int spirula_agent_run_main(int argc, char** argv) {
    AgentRoots roots;
    std::string error;
    try {
        if (!parse_agent_roots(argc, argv, roots, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 2;
        }
        app::agent::Config config;
        app::agent::State state;
        if (!prepare_agent_host(roots, config, state, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        app::install_crash_log(roots.state_root.u8string());
        app::set_crash_note("agent host (M2 feature worker)");

#ifdef _WIN32
        g_host_stop.store(false);
        if (!SetConsoleCtrlHandler(agent_console_handler, TRUE)) {
            std::fputs("cannot install agent stop handler\n", stderr);
            return 1;
        }
#else
        g_host_stop = 0;
        const auto old_int = std::signal(SIGINT, agent_stop_signal);
        const auto old_term = std::signal(SIGTERM, agent_stop_signal);
        if (old_int == SIG_ERR || old_term == SIG_ERR) {
            if (old_int != SIG_ERR) std::signal(SIGINT, old_int);
            if (old_term != SIG_ERR) std::signal(SIGTERM, old_term);
            std::fputs("cannot install agent stop handlers\n", stderr);
            return 1;
        }
#endif

        const std::string status = host_status(state);
        std::fputs(status.c_str(), stdout);
        const app::agent::HostExit exit = app::agent::RunAgentClient(
            config, roots.state_root, roots.storage_root, false, state,
            agent_stop_requested, nullptr);
#ifdef _WIN32
        SetConsoleCtrlHandler(agent_console_handler, FALSE);
#else
        std::signal(SIGINT, old_int);
        std::signal(SIGTERM, old_term);
#endif
        if (exit == app::agent::HostExit::RestartRequested) {
            std::fputs("agent host is exiting for service-manager restart\n",
                       stdout);
            return 75;
        }
        std::fputs("agent host stopped cleanly\n", stdout);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "agent host failed: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fputs("agent host failed\n", stderr);
        return 1;
    }
}

int spirula_agent_service_main(int argc, char** argv) {
#ifndef _WIN32
    (void)argc;
    (void)argv;
    std::fputs("agent service is supported only through Windows SCM; "
               "use agent run on POSIX\n",
               stderr);
    return 1;
#else
    std::string error;
    try {
        if (!parse_agent_roots(argc, argv, g_service_roots, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 2;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "invalid agent service roots: %s\n", e.what());
        return 2;
    }

    g_host_stop.store(false);
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(kServiceName), agent_service_entry},
        {nullptr, nullptr},
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        std::fprintf(stderr, "StartServiceCtrlDispatcherW failed: %lu\n",
                     static_cast<unsigned long>(GetLastError()));
        return 1;
    }
    return 0;
#endif
}
