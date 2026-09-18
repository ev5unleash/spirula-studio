#include "app/tests/SceneFixture.h"
#include "app/JobScheduler.h"
#include "app/Subprocess.h"
#include "app/WorkerRequest.h"
#include "backend/api/BackendRuntime.h"
#include "checkpoint/Resume.h"
#include "core/Env.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <future>
#include <map>
#include <thread>

namespace fs = std::filesystem;
namespace sched = app::sched;
using Clock = std::chrono::steady_clock;

int checkpoint_step(const fs::path& run) {
    const auto checkpoint = ckpt::resolve_checkpoint(run);
    ckpt::check_resumable(checkpoint.ckpt_dir);
    const auto state = ckpt::read_state_json(checkpoint.ckpt_dir);
    const auto* step = state.find("step");
    if (!step) throw std::runtime_error("checkpoint has no step");
    return static_cast<int>(step->as_int(-1));
}

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
    std::printf("ok   %s\n", message);
}

bool has_nonfinite_number(const std::string& line) {
    size_t pos = 0;
    while ((pos = line.find('=', pos)) != std::string::npos) {
        const char* start = line.c_str() + ++pos;
        char* end = nullptr;
        const float value = std::strtof(start, &end);
        if (end != start && !std::isfinite(value)) return true;
    }
    return false;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        require(argc == 2 && fs::is_regular_file(fs::u8path(argv[1])), "exact built executable exists");
        const char* selector = spirula::env("TEST_DEVICE");
        require(selector && *selector, "SS_TEST_DEVICE explicitly selects the test GPU");
        std::string device = selector;
#ifdef SS_BACKEND_VULKAN
        require(backend::device_select_identity(device.c_str()), "Vulkan test device resolves");
        device = backend::device_current_selector();
#else
        size_t used = 0;
        const int ordinal = std::stoi(device, &used);
        require(used == device.size() && ordinal >= 0 && backend::device_select(ordinal), "CUDA test ordinal resolves");
#endif
        const auto info = backend::device_info(backend::device_current());
        require(info.usable, "selected hardware supports the workload");
        std::printf("GPU: %s; selector: %s\n", info.name, device.c_str());
        std::string second_device, second_name;
#ifdef SS_BACKEND_VULKAN
        if (const char* selector_b = spirula::env("TEST_DEVICE_B")) {
            second_device = backend::device_resolve_identity(selector_b);
            require(!second_device.empty() && second_device != device,
                    "second Vulkan test device resolves distinctly");
            for (int i = 0; i < backend::device_count(); ++i) {
                if (backend::device_selector(i) != second_device) continue;
                const auto candidate = backend::device_info(i);
                if (candidate.usable) second_name = candidate.name;
            }
            require(!second_name.empty(), "second Vulkan test device is usable");
        }
#endif
        const fs::path root = fs::current_path() / "training-fixture";
        const fs::path dataset = root / "dataset";
        const fs::path outputs = root / "outputs";
        test::write_scene(dataset);
        const std::string exe = fs::absolute(fs::u8path(argv[1])).u8string();
        auto args = [&](int steps) {
            return std::vector<std::string>{"--data", dataset.u8string(), "--data-format", "nerfstudio",
                "--output-dir-prefix", outputs.u8string(), "--num-iterations", std::to_string(steps),
                "--steps-per-save", "5", "--save-full-checkpoint", "1", "--save-only-latest-checkpoint", "0",
                "--disable-viewer", "1", "--keep-viewer-alive", "0", "--sh-degree", "0",
                "--cap-max", "256", "--eval-mode", "all", "--refine-start-iter", "100000"};
        };
        auto direct_args = args(12);
        direct_args.insert(direct_args.end(), {"--output-dir-name", "direct", "--device", device});
        int metric_lines = 0;
        bool nonfinite = false;
        auto observe = [&](const std::string& line) {
            std::printf("%s\n", line.c_str());
            const size_t metric = line.find("rgb_loss=");
            if (metric != std::string::npos) {
                const char* start = line.c_str() + metric + 9;
                char* end = nullptr;
                const float value = std::strtof(start, &end);
                if (end == start || !std::isfinite(value))
                    nonfinite = true;
                else
                    metric_lines++;
            }
            nonfinite = nonfinite || has_nonfinite_number(line);
        };
        app::proc::ProcessOptions process;
        process.argv = {exe, "train"};
        process.argv.insert(process.argv.end(), direct_args.begin(), direct_args.end());
        process.cwd = root.u8string();
        std::atomic<bool> cancel{false};
        process.cancel = &cancel;
        process.on_line = observe;
        auto running = std::async(std::launch::async, [&] { return app::proc::run_process(process); });
        if (running.wait_for(std::chrono::seconds(120)) != std::future_status::ready) {
            cancel.store(true);
            running.get();
            throw std::runtime_error("direct training exceeded deadline");
        }
        const auto completed = running.get();
        require(completed.outcome == app::proc::ProcessOutcome::Success && completed.exit_code == 0,
                "real CLI training completes");
        const int direct_step = checkpoint_step(outputs / "direct");
        require(direct_step == 12, "full checkpoint records completed direct training step");
        require(metric_lines > 0 && !nonfinite,
                "direct training reports finite metrics");
        metric_lines = 0;
        nonfinite = false;

        sched::JobScheduler scheduler((root / "queue").u8string(), exe);
        std::map<std::string, bool> selected_row;
        std::map<std::string, std::string> actual_devices;
        scheduler.set_event_callback([&](const sched::Event& event) {
            if (event.line.empty()) return;
            observe(event.line);
            if (event.line.find("* [") != std::string::npos) {
                selected_row[event.job_id] = true;
            } else if (selected_row[event.job_id]) {
                const size_t pos = event.line.find("uuid:");
                if (pos != std::string::npos) {
                    actual_devices[event.job_id] = event.line.substr(pos, 37);
                    selected_row[event.job_id] = false;
                }
            }
        });
        auto submit = [&](int steps, const std::string& resume) {
            sched::SubmitOpts opts;
            opts.phase = "train"; opts.device = device; opts.device_name = info.name;
            opts.work_dir = root.u8string(); opts.args = args(steps);
            if (!resume.empty()) opts.args.insert(opts.args.end(), {"--resume", resume});
            const auto id = scheduler.submit(opts);
            require(!id.empty(), "scheduled training accepted");
            return id;
        };
        auto job = [&](const std::string& id) {
            for (const auto& j : scheduler.list()) if (j.job_id == id) return j;
            throw std::runtime_error("scheduled job disappeared");
        };
        auto wait = [&](const std::string& id, auto predicate) {
            const auto deadline = Clock::now() + std::chrono::seconds(90);
            while (Clock::now() < deadline) {
                scheduler.drain_events();
                const auto current = job(id);
                if (predicate(current)) return current;
                if (current.state == sched::JobState::Failed || current.state == sched::JobState::Blocked)
                    throw std::runtime_error("scheduled attempt failed: " + current.error);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            scheduler.force_stop(id);
            throw std::runtime_error("scheduled attempt exceeded deadline");
        };
        try {
            const auto resumed_id = submit(24, (outputs / "direct").u8string());
            const auto resumed = wait(resumed_id, [](const auto& j) { return j.state == sched::JobState::Succeeded; });
            require(checkpoint_step(fs::u8path(resumed.output_dir)) == 24, "scheduled resume advances past saved step");
            bool matched_result = false;
            for (const auto& entry : fs::directory_iterator(fs::u8path(resumed.run_dir))) {
                if (entry.path().filename().u8string().rfind("result-", 0) != 0) continue;
                const auto result = app::worker::parse_result(entry.path().u8string());
                const auto request = app::worker::parse_request((entry.path().parent_path() /
                    ("request-" + result.attempt_id + ".json")).u8string());
                matched_result = result.job_id == resumed_id && result.attempt_id == request.attempt_id &&
                    result.phase == "train" && result.outcome == "success" && result.exit_code == 0;
                for (const auto& output : result.outputs) require(fs::exists(fs::u8path(output)), "worker output exists");
            }
            require(matched_result, "scheduled result matches its published attempt");
            if (!second_device.empty()) {
                scheduler.pause_dispatch(true);
                const auto first = submit(100000, "");
                const auto retargeted = submit(100000, "");
                scheduler.pause_dispatch(false);
                const auto first_active = wait(first, [&](const auto& j) {
                    if (j.state != sched::JobState::Running ||
                        actual_devices[first] != device)
                        return false;
                    try { return checkpoint_step(fs::u8path(j.output_dir)) >= 5; }
                    catch (const std::exception&) { return false; }
                });
                require(job(retargeted).state == sched::JobState::Queued,
                        "same-device work waits behind the active lease");
                scheduler.set_device(retargeted, second_device, second_name);
                const auto second_active = wait(retargeted, [&](const auto& j) {
                    if (j.state != sched::JobState::Running ||
                        actual_devices[retargeted] != second_device)
                        return false;
                    try { return checkpoint_step(fs::u8path(j.output_dir)) >= 5; }
                    catch (const std::exception&) { return false; }
                });
                require(job(first).state == sched::JobState::Running,
                        "first device keeps running after queued retarget");
                require(actual_devices[first] == device,
                        "first worker reports its actual device");
                require(actual_devices[retargeted] == second_device,
                        "retargeted worker reports its actual device");
                scheduler.force_stop(first);
                scheduler.force_stop(retargeted);
                const auto first_stopped = wait(first, [](const auto& j) {
                    return j.state == sched::JobState::Interrupted;
                });
                const auto second_stopped = wait(retargeted, [](const auto& j) {
                    return j.state == sched::JobState::Interrupted;
                });
                require(first_stopped.pending_resume &&
                            checkpoint_step(fs::u8path(first_active.output_dir)) >= 5 &&
                            second_stopped.pending_resume &&
                            checkpoint_step(fs::u8path(second_active.output_dir)) >= 5,
                        "both physical devices retain resumable checkpoints");
            }
            for (bool force : {false, true}) {
                const auto id = submit(100000, "");
                const auto active = wait(id, [&](const auto& j) {
                    if (j.state != sched::JobState::Running) return false;
                    try { return checkpoint_step(fs::u8path(j.output_dir)) >= 5; }
                    catch (const std::exception&) { return false; }
                });
                if (force) scheduler.force_stop(id); else scheduler.stop_and_save(id);
                const auto stopped = wait(id, [&](const auto& j) {
                    return j.state == (force ? sched::JobState::Interrupted : sched::JobState::Stopped);
                });
                require(stopped.pending_resume && checkpoint_step(fs::u8path(active.output_dir)) >= 5,
                        force ? "forced interruption retains a valid resume point" : "cooperative stop saves a resumable checkpoint");
            }
        } catch (...) {
            for (const auto& j : scheduler.list()) scheduler.force_stop(j.job_id);
            scheduler.shutdown();
            throw;
        }
        scheduler.shutdown();
        scheduler.drain_events();
        require(metric_lines > 0 && !nonfinite,
                "scheduled training reports finite metrics");
        std::puts("PASS real CLI and scheduled training lifecycle");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL training lifecycle: %s\n", e.what());
        return 1;
    }
}
