#include "app/Subprocess.h"
#include "app/OutputLease.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace proc = app::proc;
namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition) {
        std::printf("ok   %s\n", message);
    } else {
        std::printf("FAIL %s\n", message);
        ++failures;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--closed-output-delay") {
#ifdef _WIN32
        HANDLE stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
        HANDLE stderr_handle = GetStdHandle(STD_ERROR_HANDLE);
        if (stdout_handle && stdout_handle != INVALID_HANDLE_VALUE)
            CloseHandle(stdout_handle);
        if (stderr_handle && stderr_handle != INVALID_HANDLE_VALUE &&
            stderr_handle != stdout_handle)
            CloseHandle(stderr_handle);
#else
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
#endif
        char byte = 0;
#ifdef _WIN32
        DWORD got = 0;
        while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), &byte, 1, &got, nullptr) &&
               got) {}
#else
        while (read(STDIN_FILENO, &byte, 1) > 0) {}
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        return 0;
    }

    if (argc == 2 && std::string(argv[1]) == "--spawn-survivor") {
#ifdef _WIN32
        const std::wstring exe = fs::absolute(argv[0]).wstring();
        std::wstring command = L"\"" + exe + L"\" --closed-output-delay";
        std::vector<wchar_t> command_line(command.begin(), command.end());
        command_line.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(
            nullptr, command_line.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
        if (!created) return 1;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 0;
#else
        const pid_t child = fork();
        if (child < 0) return 1;
        if (child == 0) {
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
            _exit(0);
        }
        return 0;
#endif

    }
    if (argc == 2 && std::string(argv[1]) == "--emit-pause-acks") {
        std::puts("WORKER_PAUSED");
        std::puts("visible output");
        std::puts("WORKER_RUNNING");
        return 0;
    }

    if (argc == 2 && std::string(argv[1]) == "--close-output-hold") {
#ifdef _WIN32
        HANDLE stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
        HANDLE stderr_handle = GetStdHandle(STD_ERROR_HANDLE);
        if (stdout_handle && stdout_handle != INVALID_HANDLE_VALUE)
            CloseHandle(stdout_handle);
        if (stderr_handle && stderr_handle != INVALID_HANDLE_VALUE &&
            stderr_handle != stdout_handle)
            CloseHandle(stderr_handle);
#else
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
#endif
        std::this_thread::sleep_for(std::chrono::seconds(10));
        return 0;
    }

    {
        proc::WorkerCommandParser parser;
        auto parse = [&](std::string_view text) {
            std::optional<proc::WorkerCommand> result;
            for (char byte : text)
                if (auto command = parser.push(byte)) result = command;
            return result;
        };
        check(parse("PAUSE\r\n") == proc::WorkerCommand::Pause,
              "control parser accepts exact PAUSE");
        check(parse("PAUSE extra\n") == std::nullopt,
              "control parser ignores command prefixes with extra text");
        check(parse("RESUME\n") == proc::WorkerCommand::Resume,
              "control parser accepts exact RESUME");
        check(parse("CANCEL\n") == proc::WorkerCommand::Stop,
              "control parser maps CANCEL to Stop");
        check(parse("STOP\n") == proc::WorkerCommand::Stop,
              "control parser accepts exact STOP");
        check(parse("12345678\nPAUSE\n") == proc::WorkerCommand::Pause,
              "control parser resets after an oversized line");
    }

    {
        std::atomic<bool> acknowledged_paused{false};
        bool output_saw_paused = false;
        std::vector<std::string> lines;
        proc::ProcessOptions opts;
        opts.argv = {fs::absolute(argv[0]).u8string(), "--emit-pause-acks"};
        opts.acknowledged_paused = &acknowledged_paused;
        opts.on_line = [&](const std::string& line) {
            lines.push_back(line);
            output_saw_paused =
                output_saw_paused ||
                (line == "visible output" && acknowledged_paused.load());
        };
        const proc::ProcessResult result = proc::run_process(opts);
        check(result.outcome == proc::ProcessOutcome::Success,
              "worker acknowledgment child exits successfully");
        check(output_saw_paused,
              "pause acknowledgment updates status before normal output");
        check(!acknowledged_paused.load(),
              "running acknowledgment clears paused status");
        check(lines == std::vector<std::string>{"visible output"},
              "reserved worker acknowledgments stay out of normal output");
    }

    // 1. Basic command execution and line streaming
    {
        proc::ProcessOptions opts;
#ifdef _WIN32
        opts.argv = {"cmd.exe", "/c", "echo alpha&&echo beta"};
#else
        opts.argv = {"/bin/sh", "-c", "printf 'alpha\nbeta\n'"};
#endif
        std::vector<std::string> lines;
        uint64_t child_pid = 0;
        opts.on_started = [&](uint64_t pid) { child_pid = pid; };
        opts.on_line = [&](const std::string& l) {
            lines.push_back(l);
        };
        proc::ProcessResult res = proc::run_process(opts);
        check(res.outcome == proc::ProcessOutcome::Success, "basic execution outcome is Success");
        check(res.exit_code == 0, "basic execution exit code is 0");
        check(child_pid != 0, "started callback reports the child process ID");
        check(lines.size() == 2, "captured exactly two lines");
        if (lines.size() >= 2) {
            check(lines[0] == "alpha", "line 1 matches");
            check(lines[1] == "beta", "line 2 matches");
        }
    }

    {
        std::atomic<bool> cancel_flag{false};
        proc::ProcessOptions opts;
        opts.argv = {fs::absolute(argv[0]).u8string(), "--close-output-hold"};
        opts.cancel = &cancel_flag;
        std::thread canceller([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            cancel_flag.store(true);
        });
        const auto start = std::chrono::steady_clock::now();
        const proc::ProcessResult res = proc::run_process(opts);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        canceller.join();
        check(res.outcome == proc::ProcessOutcome::Cancelled,
              "closed output does not disable cancellation");
        check(elapsed < 4000,
              "closed-output process terminates promptly on cancellation");
    }

    {
        std::atomic<bool> cancel_flag{false};
        proc::ProcessOptions opts;
        opts.argv = {fs::absolute(argv[0]).u8string(), "--spawn-survivor"};
        opts.cancel = &cancel_flag;
        std::thread canceller([&]() {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            cancel_flag.store(true);
        });
        const auto start = std::chrono::steady_clock::now();
        const proc::ProcessResult res = proc::run_process(opts);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        canceller.join();
        check(res.outcome == proc::ProcessOutcome::Success,
              "surviving descendant preserves successful outcome");
        check(elapsed >= 500,
              "process ownership waits for descendants after root exit");
    }

    // 2. Child-local environment overrides
    {
        const char* var_name = "SPIRULA_TEST_ENV_KEY";
        const char* var_val = "test_value_xyz_789";
        check(std::getenv(var_name) == nullptr, "parent environment clean before launch");

        proc::ProcessOptions opts;
        opts.env_overrides = {{var_name, var_val}};
#ifdef _WIN32
        opts.argv = {"cmd.exe", "/c", "echo %SPIRULA_TEST_ENV_KEY%"};
#else
        opts.argv = {"/bin/sh", "-c", "echo \"$SPIRULA_TEST_ENV_KEY\""};
#endif
        std::vector<std::string> lines;
        opts.on_line = [&](const std::string& l) {
            lines.push_back(l);
        };
        proc::ProcessResult res = proc::run_process(opts);
        check(res.outcome == proc::ProcessOutcome::Success, "child executed successfully with env override");
        check(res.exit_code == 0, "child exit code is 0");
        bool saw_val = false;
        for (const auto& l : lines) {
            if (l.find(var_val) != std::string::npos) saw_val = true;
        }
        check(saw_val, "child output contains overridden variable value");
        check(std::getenv(var_name) == nullptr, "parent environment untouched after child launch");
    }

    // 3. Exit code propagation
    {
        proc::ProcessOptions opts;
#ifdef _WIN32
        opts.argv = {"cmd.exe", "/c", "exit 42"};
#else
        opts.argv = {"/bin/sh", "-c", "exit 42"};
#endif
        proc::ProcessResult res = proc::run_process(opts);
        check(res.outcome == proc::ProcessOutcome::Success, "non-zero exit is Success outcome");
        check(res.exit_code == 42, "exit code 42 propagated accurately");
    }

    // 4. Cancellation and process tree termination
    {
        std::atomic<bool> cancel_flag{false};
        proc::ProcessOptions opts;
#ifdef _WIN32
        opts.argv = {"cmd.exe", "/c", "powershell -NoProfile -Command Start-Sleep -Seconds 10"};
#else
        opts.argv = {"/bin/sh", "-c", "sleep 10"};
#endif
        opts.cancel = &cancel_flag;

        auto start = std::chrono::steady_clock::now();
        std::thread canceller([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            cancel_flag.store(true);
        });

        proc::ProcessResult res = proc::run_process(opts);
        canceller.join();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();

        check(res.outcome == proc::ProcessOutcome::Cancelled, "cancelled outcome is Cancelled");
        check(elapsed < 4000, "process terminated promptly on cancellation");
    }

    // 5. Cooperative stop
    {
        std::atomic<bool> stop_flag{false};
        proc::ProcessOptions opts;
#ifdef _WIN32
        opts.argv = {"powershell", "-NoProfile", "-Command",
                     "$in = [Console]::In.ReadLine(); Write-Output \"STOP_RECVD_$in\""};
#else
        opts.argv = {"/bin/sh", "-c", "read line && echo STOP_RECVD_$line"};
#endif
        opts.stop = &stop_flag;
        opts.stop_token = "COOP_OK\n";

        std::vector<std::string> lines;
        opts.on_line = [&](const std::string& l) {
            lines.push_back(l);
        };

        std::thread stopper([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            stop_flag.store(true);
        });

        proc::ProcessResult res = proc::run_process(opts);
        stopper.join();

        check(res.outcome == proc::ProcessOutcome::Stopped, "cooperative stop outcome is Stopped");
        check(res.exit_code == 0, "child exited cleanly on stop");
        bool heard_stop = false;
        for (const auto& l : lines) {
            if (l.find("STOP_RECVD_COOP_OK") != std::string::npos) heard_stop = true;
        }
        check(heard_stop, "child received cooperative stop token via stdin");
    }

    // 6. A stop that exceeds grace is a forced cancellation, not a clean stop.
    {
        std::atomic<bool> stop_flag{false};
        proc::ProcessOptions opts;
#ifdef _WIN32
        opts.argv = {"powershell", "-NoProfile", "-Command", "Start-Sleep -Seconds 10"};
#else
        opts.argv = {"/bin/sh", "-c", "sleep 10"};
#endif
        opts.stop = &stop_flag;
        opts.grace_period_ms = 100;
        std::thread stopper([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            stop_flag.store(true);
        });
        proc::ProcessResult res = proc::run_process(opts);
        stopper.join();
        check(res.outcome == proc::ProcessOutcome::Cancelled,
              "forced stop is reported as Cancelled");
    }

    // 7. Spawn failure on non-existent binary
    {
        proc::ProcessOptions opts;
        opts.argv = {"__non_existent_binary_xyz_12345__"};
        proc::ProcessResult res = proc::run_process(opts);
        check(res.outcome == proc::ProcessOutcome::SpawnFailed, "missing executable reports SpawnFailed");
    }

    // 8. A worker-held output lease survives parent release.
    {
        const fs::path output = fs::temp_directory_path() /
            ("spirula_subprocess_lease_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        app::OutputLease owner;
        app::OutputLease contender;
        std::string error;
        check(owner.acquire(output, error), "handoff test acquires output lease");

        std::atomic<bool> ready{false};
        proc::ProcessResult worker_result;
        proc::ProcessOptions opts;
#ifdef _WIN32
        DWORD original_flags = 0;
        const bool got_original_flags =
            GetHandleInformation(owner.native_handle(), &original_flags) != 0;
        opts.argv = {"powershell", "-NoProfile", "-Command",
                     "Write-Output READY; Start-Sleep -Seconds 1"};
        opts.inherit_handles.push_back(owner.native_handle());
#else
        opts.argv = {"/bin/sh", "-c", "printf 'READY\\n'; sleep 1"};
        opts.inherit_fds.push_back(owner.native_fd());
#endif
        opts.on_line = [&](const std::string& line) { ready = line == "READY"; };
        std::thread worker([&] { worker_result = proc::run_process(opts); });
        for (int i = 0; i < 100 && !ready; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        check(ready, "handoff worker started");
#ifdef _WIN32
        DWORD current_flags = 0;
        check(got_original_flags &&
                  GetHandleInformation(owner.native_handle(), &current_flags) &&
                  ((current_flags ^ original_flags) & HANDLE_FLAG_INHERIT) == 0,
              "inherited handle flags restored after spawn");
#else
        const int flags = fcntl(owner.native_fd(), F_GETFD);
        check(flags >= 0 && (flags & FD_CLOEXEC),
              "output lease remains close-on-exec");
#endif
        owner.release();
        check(!contender.acquire(output, error),
              "worker keeps output lease after parent release");
        worker.join();
        check(contender.acquire(output, error),
              "output lease releases after worker exit");
        check(worker_result.outcome == proc::ProcessOutcome::Success,
              "handoff worker exits successfully");
        check(worker_result.exit_code == 0,
              "handoff worker exit code is zero");
    }

#ifdef _WIN32
    {
        const char* previous = std::getenv("SS_WORKER_CONTROL");
        const bool had_previous = previous != nullptr;
        const std::string previous_value = previous ? previous : "";
        _putenv_s("SS_WORKER_CONTROL", "1");

        std::atomic<bool> cancel_flag{false};
        proc::ProcessOptions opts;
        opts.argv = {"powershell", "-NoProfile", "-Command",
                     "Write-Output READY; Start-Sleep -Seconds 10"};
        opts.cancel = &cancel_flag;
        std::atomic<bool> ready{false};
        std::vector<std::string> lines;
        opts.on_line = [&](const std::string& line) {
            lines.push_back(line);
            ready.store(line == "READY");
        };
        std::thread canceller([&]() {
            for (int i = 0; i < 200 && !ready.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            cancel_flag.store(true);
        });
        const proc::ProcessResult res = proc::run_process(opts);
        canceller.join();

        check(res.outcome == proc::ProcessOutcome::Cancelled,
              "nested worker helper cancellation is reported as Cancelled");
        check(!lines.empty() && lines.front() == "READY",
              "nested worker helper starts without a second job object");

        _putenv_s("SS_WORKER_CONTROL",
                  had_previous ? previous_value.c_str() : "");
    }
#endif

#ifndef _WIN32
    // 9. An inherited lease never occupies a standard descriptor.
    {
        const int saved_stdin = dup(STDIN_FILENO);
        if (saved_stdin >= 0) close(STDIN_FILENO);
        const fs::path output = fs::temp_directory_path() /
            ("spirula_subprocess_standard_fd_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        app::OutputLease lease;
        std::string error;
        check(lease.acquire(output, error),
              "output lease acquires with closed standard input");
        check(lease.valid() && lease.native_fd() >= 3,
              "output lease avoids standard descriptors");
        lease.release();
        if (saved_stdin >= 0) {
            dup2(saved_stdin, STDIN_FILENO);
            close(saved_stdin);
        }
    }
#endif

#ifndef _WIN32
    // 10. Cancellation requested before launch still kills process group.
    {
        std::atomic<bool> cancel_flag{true};
        proc::ProcessOptions opts;
        opts.argv = {"/bin/sh", "-c", "sleep 2"};
        opts.cancel = &cancel_flag;
        const auto start = std::chrono::steady_clock::now();
        const proc::ProcessResult res = proc::run_process(opts);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        check(res.outcome == proc::ProcessOutcome::Cancelled,
              "pre-launch cancellation is reported as Cancelled");
        check(elapsed < 1000, "pre-launch cancellation terminates promptly");
    }
#endif

#ifdef __linux__
    // 11. Worker-control helpers stay in the worker group and cancel locally.
    {
        const char* previous = std::getenv("SS_WORKER_CONTROL");
        const bool had_previous = previous != nullptr;
        const std::string previous_value = previous ? previous : "";
        setenv("SS_WORKER_CONTROL", "1", 1);

        std::atomic<bool> cancel_flag{false};
        std::vector<std::string> lines;
        proc::ProcessOptions opts;
        opts.argv = {"/bin/sh", "-c",
                     "printf '%s\\n' \"$(ps -o pgid= -p $$)\"; sleep 2"};
        opts.cancel = &cancel_flag;
        opts.on_line = [&](const std::string& line) { lines.push_back(line); };
        std::thread canceller([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            cancel_flag.store(true);
        });
        const proc::ProcessResult res = proc::run_process(opts);
        canceller.join();

        long reported_group = -1;
        if (!lines.empty()) {
            char* end = nullptr;
            reported_group = std::strtol(lines.front().c_str(), &end, 10);
            if (!end || *end != '\0') reported_group = -1;
        }
        check(res.outcome == proc::ProcessOutcome::Cancelled,
              "nested worker helper cancellation is reported as Cancelled");
        check(reported_group == static_cast<long>(getpgrp()),
              "nested worker helper remains in the caller process group");

        if (had_previous)
            setenv("SS_WORKER_CONTROL", previous_value.c_str(), 1);
        else
            unsetenv("SS_WORKER_CONTROL");
    }

    // 12. A worker parent death terminates its nested helper.
    {
        const fs::path root = fs::temp_directory_path() /
            ("spirula_subprocess_parent_death_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        const fs::path started = root.string() + ".started";
        const fs::path alive = root.string() + ".alive";
        const pid_t worker = fork();
        if (worker < 0) {
            check(false, "parent-death worker fork succeeds");
        } else if (worker == 0) {
            setenv("SS_WORKER_CONTROL", "1", 1);
            proc::ProcessOptions opts;
            opts.argv = {"/bin/sh", "-c",
                         "printf started > \"$SPIRULA_TEST_STARTED\"; "
                         "sleep 1; printf alive > \"$SPIRULA_TEST_ALIVE\""};
            opts.env_overrides = {
                {"SPIRULA_TEST_STARTED", started.string()},
                {"SPIRULA_TEST_ALIVE", alive.string()}};
            (void)proc::run_process(opts);
            _exit(0);
        } else {
            bool helper_started = false;
            for (int i = 0; i < 100 && !helper_started; ++i) {
                helper_started = fs::exists(started);
                if (!helper_started)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            check(helper_started, "parent-death helper reaches exec");
            (void)kill(worker, SIGKILL);
            int status = 0;
            while (waitpid(worker, &status, 0) < 0 && errno == EINTR) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(1300));
            check(!fs::exists(alive),
                  "nested helper does not outlive its worker parent");
        }
        std::error_code ec;
        fs::remove(started, ec);
        fs::remove(alive, ec);
    }
#endif

    if (failures) {
        std::printf("FAILED with %d error(s)\n", failures);
        return 1;
    }
    std::printf("All subprocess tests passed.\n");
    return 0;
}
