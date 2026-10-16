#include "app/JobScheduler.h"
#include "app/WorkerRequest.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
namespace sched = app::sched;

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 4 && std::string(argv[1]) == "worker") {
        auto request = app::worker::parse_request(argv[3]);
        const std::string mode = request.args.at(0);
        if (mode == "truncated") {
            std::ofstream(fs::u8path(request.result_path)) << "{\"outcome\":";
            return 0;
        }
        if (mode == "wrong-job") request.job_id = "job-ffffffffffffffff";
        if (mode == "wrong-attempt") request.attempt_id = "att-ffffffffffffffff";
        if (mode == "wrong-phase") request.phase = "prep";
        app::worker::Result result;
        result.outcome = "success";
        if (mode == "train-route") {
            auto value = [&](const std::string& key) {
                for (size_t i = 1; i < request.args.size(); ++i)
                    if (request.args[i - 1] == key) return request.args[i];
                throw std::runtime_error("training output argument missing");
            };
            const fs::path output = fs::u8path(value("--output-dir-prefix")) /
                                    fs::u8path(value("--output-dir-name"));
            app::OutputLease competing_writer;
            std::string lease_error;
            if (competing_writer.acquire(output, lease_error)) {
                result.outcome = "failed"; result.exit_code = 2;
                result.message = "actual training destination was not leased";
                app::worker::publish_result(request, result);
                return 2;
            }
            std::ofstream(output / "config.json") << "{}";
            std::ofstream(output / "native-output") << "trained";
            result.exit_code = 0; result.outputs = {output.u8string()};
            return app::worker::publish_result(request, result) ? 0 : 1;
        }
        if (mode == "stopped") {
            result.outcome = "stopped"; result.exit_code = 42;
            return app::worker::publish_result(request, result) ? 42 : 1;
        }
        if (mode == "failed") {
            result.outcome = "failed"; result.exit_code = 2;
            return app::worker::publish_result(request, result) ? 2 : 1;
        }
        result.exit_code = 0;
        result.outputs = {mode == "missing-output"
            ? (fs::u8path(request.work_dir) / "absent").u8string()
            : request.work_dir};
        return app::worker::publish_result(request, result) ? 0 : 1;
    }
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", message);
        failures += !ok;
    };
    try {
        const fs::path root = fs::current_path() / "protocol-fixture";
        fs::create_directories(root);
        const std::string exe = fs::absolute(fs::u8path(argv[0])).u8string();
        fs::create_directories(root / "shared-admission");
        const auto existing_queue = root / "shared-admission" / "job-state.json";
        std::ofstream(existing_queue, std::ios::binary) << "retained-unrelated-queue\n";
        {
            sched::JobScheduler owner((root / "admission-owner").u8string(), exe,
                                      (root / "shared-admission").u8string());
            owner.pause_dispatch(true);
            sched::JobScheduler conflicting((root / "admission-other").u8string(), exe,
                                            (root / "shared-admission").u8string());
            check(!conflicting.state_error().empty(),
                  "separate group attempt cannot bypass shared scheduler admission");
            sched::SubmitOpts opts;
            opts.phase = "geometry"; opts.device = "protocol-device";
            opts.work_dir = root.u8string(); opts.args = {"valid"};
            check(conflicting.submit(opts).empty(),
                  "conflicting global admission never spawns owner work");
        }
        std::string retained_queue;
        std::getline(std::ifstream(existing_queue, std::ios::binary), retained_queue);
        check(retained_queue == "retained-unrelated-queue",
              "private group admission never overwrites unrelated persisted queue");
        for (const char* mode : {"valid", "wrong-job", "wrong-attempt", "wrong-phase",
                                 "truncated", "missing-output", "stopped", "failed", "train-route"}) {
            const fs::path work = root / mode;
            fs::create_directories(work);
            sched::JobScheduler scheduler((work / "queue").u8string(), exe);
            sched::SubmitOpts opts;
            opts.phase = "geometry";
            opts.device = "protocol-device";
            opts.work_dir = work.u8string();
            opts.args = {mode};
            if (std::string(mode) == "train-route") {
                opts.phase = "train";
                opts.output_dir = (work / "owner-0").u8string();
                opts.args = {mode, "--output-dir-prefix", (work / "wrong").u8string(),
                             "--output-dir-name", "wrong"};
            }
            const std::string id = scheduler.submit(opts);
            if (id.empty()) throw std::runtime_error("protocol submission rejected");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            bool terminal = false;
            while (std::chrono::steady_clock::now() < deadline) {
                const auto jobs = scheduler.list();
                if (!jobs.empty() && (jobs[0].state == sched::JobState::Succeeded ||
                                      jobs[0].state == sched::JobState::Failed ||
                                      jobs[0].state == sched::JobState::Stopped)) {
                    const bool valid = std::string(mode) == "valid" || std::string(mode) == "train-route";
                    const auto expected = std::string(mode) == "stopped" ? sched::JobState::Stopped :
                        valid ? sched::JobState::Succeeded : sched::JobState::Failed;
                    check(jobs[0].state == expected, mode);
                    if (std::string(mode) == "train-route")
                        check(fs::is_regular_file(work / "owner-0" / "native-output") &&
                              !fs::exists(work / "wrong"),
                              "explicit training destination is both leased and actually published");
                    if (!valid) check(jobs[0].completed_prefix == 0,
                                      "untrusted result never advances dependent publication");
                    terminal = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!terminal) scheduler.force_stop(id);
            check(terminal, "attempt finishes before deadline");
            scheduler.shutdown();
        }
        for (int i = 0; i < 200; ++i) {
            sched::JobScheduler scheduler((root / "shutdown").u8string(), exe);
            scheduler.pause_dispatch(true);
            scheduler.pause_dispatch(false);
            scheduler.shutdown();
        }
        check(true, "idle dispatcher survives immediate wake and shutdown repeatedly");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL scheduler protocol: %s\n", e.what());
        ++failures;
    }
    return failures ? 1 : 0;
}
