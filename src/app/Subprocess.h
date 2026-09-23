#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace app::proc {

enum class WorkerCommand { Pause, Resume, Stop };

// Bounded parser for the scheduler's newline-delimited child control channel.
// Unknown input is ignored; only exact commands have meaning.
class WorkerCommandParser {
public:
    std::optional<WorkerCommand> push(char byte) noexcept {
        if (byte != '\n') {
            if (!_overflow) {
                if (_size < _line.size()) _line[_size++] = byte;
                else _overflow = true;
            }
            return std::nullopt;
        }

        std::string_view line(_line.data(), _size);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        std::optional<WorkerCommand> command;
        if (!_overflow) {
            if (line == "PAUSE") command = WorkerCommand::Pause;
            else if (line == "RESUME") command = WorkerCommand::Resume;
            else if (line == "STOP" || line == "CANCEL")
                command = WorkerCommand::Stop;
        }
        _size = 0;
        _overflow = false;
        return command;
    }

private:
    std::array<char, 7> _line{};
    std::size_t _size = 0;
    bool _overflow = false;
};

inline constexpr char kWorkerPauseAcknowledgment[] = "WORKER_PAUSED";
inline constexpr char kWorkerRunningAcknowledgment[] = "WORKER_RUNNING";

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
    std::function<void(uint64_t)> on_started;
    std::function<void(const std::string&)> on_line;
    const std::atomic<bool>* cancel = nullptr;
    const std::atomic<bool>* stop = nullptr;
    const std::atomic<bool>* pause_requested = nullptr;
    std::atomic<bool>* acknowledged_paused = nullptr;
    std::string stop_token = "STOP\n";
    std::string cancel_token = "CANCEL\n";
    std::string pause_token = "PAUSE\n";
    std::string resume_token = "RESUME\n";
    int grace_period_ms = 3000;
#ifdef _WIN32
    std::vector<void*> inherit_handles;
#else
    std::vector<int> inherit_fds;
#endif
};

ProcessResult run_process(const ProcessOptions& options);

bool command_exists(const std::string& exe);

std::vector<std::string> split_args(const std::string& s);

bool open_url(const std::string& url);

}  // namespace app::proc
