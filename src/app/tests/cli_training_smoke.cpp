#include "app/tests/SceneFixture.h"
#include "app/JobScheduler.h"
#include "app/Subprocess.h"
#include "app/WorkerRequest.h"
#include "backend/api/BackendRuntime.h"
#include "checkpoint/Resume.h"
#include "core/Env.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
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
        app::proc::ProcessOptions process;
        process.argv = {exe, "train"};
        process.argv.insert(process.argv.end(), direct_args.begin(), direct_args.end());
        process.cwd = root.u8string();
        std::atomic<bool> cancel{false};
        process.cancel = &cancel;
        process.on_line = [](const std::string& line) { std::printf("%s\n", line.c_str()); };
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

        sched::JobScheduler scheduler((root / "queue").u8string(), exe);
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
        std::puts("PASS real CLI and scheduled training lifecycle");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL training lifecycle: %s\n", e.what());
        return 1;
    }
}
