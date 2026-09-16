#include "app/Subprocess.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace proc = app::proc;

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

int main() {
    // 1. Basic command execution and line streaming
    {
        proc::ProcessOptions opts;
#ifdef _WIN32
        opts.argv = {"cmd.exe", "/c", "echo alpha&&echo beta"};
#else
        opts.argv = {"/bin/sh", "-c", "printf 'alpha\nbeta\n'"};
#endif
        std::vector<std::string> lines;
        opts.on_line = [&](const std::string& l) {
            lines.push_back(l);
        };
        proc::ProcessResult res = proc::run_process(opts);
        check(res.outcome == proc::ProcessOutcome::Success, "basic execution outcome is Success");
        check(res.exit_code == 0, "basic execution exit code is 0");
        check(lines.size() == 2, "captured exactly two lines");
        if (lines.size() >= 2) {
            check(lines[0] == "alpha", "line 1 matches");
            check(lines[1] == "beta", "line 2 matches");
        }
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

    if (failures) {
        std::printf("FAILED with %d error(s)\n", failures);
        return 1;
    }
    std::printf("All subprocess tests passed.\n");
    return 0;
}
