#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace app::proc {

enum class ProcessOutcome {
    Success,
    SpawnFailed,
    Cancelled,
    Stopped,
    Crashed
};

struct ProcessResult {
    ProcessOutcome outcome = ProcessOutcome::SpawnFailed;
    int exit_code = -1;
    std::string error_message;
};

struct ProcessOptions {
    std::vector<std::string> argv;
    std::string cwd;
    std::vector<std::pair<std::string, std::string>> env_overrides;
    std::function<void(const std::string&)> on_line;
    const std::atomic<bool>* cancel = nullptr;
    const std::atomic<bool>* stop = nullptr;
    std::string stop_token = "STOP\n";
    std::string cancel_token = "CANCEL\n";
    int grace_period_ms = 3000;
};

ProcessResult run_process(const ProcessOptions& options);

bool command_exists(const std::string& exe);

std::vector<std::string> split_args(const std::string& s);

bool open_url(const std::string& url);

}  // namespace app::proc
