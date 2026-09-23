#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace app::agent::service {

struct Configuration {
    std::filesystem::path executable;
    std::filesystem::path config_root;
    std::filesystem::path state_root;
    std::filesystem::path storage_root;
};

enum class State {
    NotInstalled,
    Stopped,
    StartPending,
    Running,
    StopPending,
    Paused,
    Failed,
    Unknown,
};

struct Result {
    bool success = false;
    std::string error;
};

struct Status {
    State state = State::Unknown;
    bool startup_enabled = false;
    std::string error;
    std::optional<Configuration> configuration;
};

// Mutations are local-administrator operations. install() enables startup but
// never starts the service. Executable and roots must already exist and pass
// platform security checks.
Result install(const Configuration& configuration);
Result uninstall();
Result start();
Result stop();
Result enable_startup();
Result disable_startup();
Status query_status();
// Read-only check for service startup, including an existing agent-state.json.
// Succeeds when that file does not exist yet.
Result validate_state_security(const std::filesystem::path& state_root);
// Read-only verification that the current POSIX process is the installed,
// active systemd/LaunchDaemon service process.
bool is_managed_service_process();

// Broker-only checks against the fixed worker service. The first validates
// its image/root configuration; the second restricts its SCM DACL; the third
// verifies that DACL before the LocalSystem broker performs a mutation.
Result broker_worker_configuration(Configuration& configuration);
Result protect_worker_service_control();
Result verify_broker_worker_control();
Result validate_broker_worker_candidate(const Configuration& configuration);
}  // namespace app::agent::service
