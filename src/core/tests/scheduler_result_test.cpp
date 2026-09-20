#include "app/JobScheduler.h"
#include "app/WorkerRequest.h"
#include "data/ProjectManifest.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
namespace fs = std::filesystem;
namespace sched = app::sched;

namespace {

void write_marker(const fs::path& path, const char* value) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary | std::ios::trunc) << value;
}

bool marker_matches(const fs::path& path, const char* expected) {
    std::ifstream in(path, std::ios::binary);
    const std::string actual((std::istreambuf_iterator<char>(in)), {});
    return actual == expected;
}

void write_feature_artifacts(const fs::path& root, const std::string& mode) {
    if (mode != "feature-missing-binding")
        write_marker(root / "binding.json",
                     mode == "feature-malformed-binding" ? "bad" : "valid-binding");
    if (mode != "feature-missing-result")
        write_marker(root / "result.json",
                     mode == "feature-malformed-result" ? "bad" : "valid-result");
    if (mode != "feature-missing-receipt")
        write_marker(root / "receipts" / "00000000.json",
                     mode == "feature-malformed-receipt" ? "bad" : "valid-receipt");
    if (mode != "feature-missing-payload")
        write_marker(root / "payload" / "frame.bin",
                     mode == "feature-malformed-payload" ? "bad" : "valid-payload");
}

bool find_request(const fs::path& run_dir, const std::string& phase,
                  app::worker::Request& out) {
    std::error_code ec;
    for (fs::directory_iterator it(run_dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec) ||
            it->path().filename().u8string().rfind("request-", 0) != 0)
            continue;
        try {
            app::worker::Request request =
                app::worker::parse_request(it->path().u8string());
            if (request.phase == phase) {
                out = std::move(request);
                return true;
            }
        } catch (...) {
        }
    }
    return false;
}

}  // namespace


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
        result.exit_code = 0;
        const fs::path run_dir =
            fs::u8path(request.result_path).parent_path();
        if (mode == "rebind-first") {
            const fs::path project = fs::u8path(request.project_root);
            std::error_code remove_ec;
            fs::remove(project / "original.bin", remove_ec);
            write_marker(project / "replacement.bin", "replacement");
            result.outputs = {(project / "replacement.bin").u8string()};
        } else if (mode == "rebind-second") {
            const fs::path output = fs::u8path(request.project_root) /
                                    "second-output";
            write_marker(output / "config.json", "{}");
            result.outputs = {output.u8string()};
        } else if (request.phase == "sfm-extract") {
            fs::path output_root = run_dir;
            if (mode == "feature-unclaimed-output")
                output_root = fs::u8path(request.work_dir) / "unclaimed";
            write_feature_artifacts(output_root, mode);
            result.outputs = {output_root.u8string()};
            if (mode == "feature-extra-output")
                result.outputs.push_back((output_root / "extra").u8string());
        } else {
            result.outputs = {
                mode == "missing-output" ? (run_dir / "absent").u8string() :
                mode == "outside-claim" ? request.work_dir : run_dir.u8string()};
        }
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
        for (const char* mode : {"valid", "wrong-job", "wrong-attempt",
                                 "wrong-phase", "truncated", "missing-output",
                                 "outside-claim"}) {
            const fs::path work = root / mode;
            fs::create_directories(work);
            sched::JobScheduler scheduler((work / "queue").u8string(), exe);
            sched::SubmitOpts opts;
            opts.phase = "geometry";
            opts.device = "protocol-device";
            opts.work_dir = work.u8string();
            opts.args = {mode};
            const std::string id = scheduler.submit(opts);
            if (id.empty()) throw std::runtime_error("protocol submission rejected");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            bool terminal = false;
            while (std::chrono::steady_clock::now() < deadline) {
                const auto jobs = scheduler.list();
                if (!jobs.empty() && (jobs[0].state == sched::JobState::Succeeded ||
                                      jobs[0].state == sched::JobState::Failed)) {
                    const bool valid = std::string(mode) == "valid";
                    check(jobs[0].state == (valid ? sched::JobState::Succeeded : sched::JobState::Failed), mode);
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
        for (const char* mode : {"feature-valid", "feature-no-validator",
                                 "feature-missing-binding",
                                 "feature-malformed-result",
                                 "feature-missing-receipt",
                                 "feature-malformed-payload",
                                 "feature-extra-output",
                                 "feature-unclaimed-output"}) {
            const fs::path work = root / mode;
            fs::create_directories(work);
            sched::JobScheduler scheduler((work / "queue").u8string(), exe);
            int validator_calls = 0;
            if (std::string(mode) != "feature-no-validator") {
                scheduler.set_feature_result_validator(
                    [&](const sched::Job&, const sched::Phase& phase,
                        const std::string& output, std::string& error) {
                        ++validator_calls;
                        const fs::path root = fs::u8path(output);
                        const bool valid =
                            phase.phase == "sfm-extract" &&
                            marker_matches(root / "binding.json", "valid-binding") &&
                            marker_matches(root / "result.json", "valid-result") &&
                            marker_matches(root / "receipts" / "00000000.json",
                                           "valid-receipt") &&
                            marker_matches(root / "payload" / "frame.bin",
                                           "valid-payload");
                        if (!valid) error = "feature artifacts are invalid";
                        return valid;
                    });
            }
            sched::SubmitOpts opts;
            opts.phase = "sfm-extract";
            opts.device = "feature-device";
            opts.work_dir = work.u8string();
            opts.args = {mode};
            const std::string id = scheduler.submit(opts);
            if (id.empty()) throw std::runtime_error("feature submission rejected");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            bool terminal = false;
            while (std::chrono::steady_clock::now() < deadline) {
                const auto jobs = scheduler.list();
                if (!jobs.empty() && (jobs[0].state == sched::JobState::Succeeded ||
                                      jobs[0].state == sched::JobState::Failed)) {
                    const bool valid = std::string(mode) == "feature-valid";
                    check(jobs[0].state == (valid ? sched::JobState::Succeeded :
                                                   sched::JobState::Failed),
                          mode);
                    check(validator_calls ==
                              ((std::string(mode) == "feature-no-validator" ||
                                std::string(mode) == "feature-extra-output" ||
                                std::string(mode) == "feature-unclaimed-output")
                                   ? 0
                                   : 1),
                          "sfm-extract result validation is invoked at the result boundary");
                    if (!valid) check(jobs[0].completed_prefix == 0,
                                      "invalid sfm-extract result never advances workflow");
                    terminal = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!terminal) scheduler.force_stop(id);
            check(terminal, "sfm-extract attempt finishes before deadline");
            scheduler.shutdown();
        }

        {
            const fs::path rebinding = root / "rebind";
            const fs::path project = rebinding / "project";
            const fs::path queue = rebinding / "queue";
            const fs::path original = project / "original.bin";
            fs::create_directories(project);
            write_marker(original, "original");
            spirula::project::ProjectRevision initial;
            initial.name = "initial";
            initial.sources = {spirula::project::make_source_record(original)};
            spirula::project::publish_revision(project, initial);
            const app::worker::ProjectReference reference =
                app::worker::make_project_reference(project.u8string(),
                                                    initial.name);

            sched::JobScheduler scheduler(queue.u8string(), exe);
            sched::WorkflowSubmitOpts workflow;
            static_cast<app::worker::ProjectReference&>(workflow) = reference;
            workflow.work_dir = queue.u8string();
            workflow.workspace = project.u8string();
            workflow.path_claims.push_back({project.u8string(), true});

            sched::Phase first;
            first.phase = "geometry";
            first.planned_device = "rebind-device";
            first.args = {"rebind-first"};
            workflow.phases.push_back(std::move(first));

            sched::Phase second;
            second.phase = "train";
            second.planned_device = "rebind-device";
            second.output = (project / "second-output").u8string();
            second.args = {"rebind-second"};
            workflow.phases.push_back(std::move(second));

            const std::string id = scheduler.submit(workflow);
            check(!id.empty(), "project rebind workflow submits");
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(10);
            bool terminal = false;
            while (std::chrono::steady_clock::now() < deadline) {
                const auto jobs = scheduler.list();
                if (!jobs.empty() &&
                    (jobs[0].state == sched::JobState::Succeeded ||
                     jobs[0].state == sched::JobState::Failed)) {
                    terminal = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            check(terminal, "project rebind workflow reaches a terminal state");
            if (!terminal) scheduler.force_stop(id);

            const auto jobs = scheduler.list();
            if (!jobs.empty()) {
                check(jobs[0].state == sched::JobState::Succeeded,
                      "project rebind survives an input move");
                app::worker::Request second_request;
                const bool have_second =
                    find_request(fs::u8path(jobs[0].run_dir), "train",
                                 second_request);
                check(have_second,
                      "project rebind dispatches the second worker");
                if (have_second) {
                    app::worker::ProjectReference exact =
                        app::worker::make_project_reference(
                            project.u8string(), second_request.project_revision);
                    check(second_request.project_revision != initial.name &&
                              second_request.project_revision_digest ==
                                  exact.project_revision_digest,
                          "second request carries the rebound revision digest");
                    const spirula::project::ProjectRevision rebound =
                        spirula::project::read_revision(
                            project, second_request.project_revision);
                    const std::string replacement_id =
                        spirula::project::make_source_record(
                            project / "replacement.bin").source_id;
                    bool has_replacement = false;
                    for (const auto& source : rebound.sources)
                        has_replacement =
                            has_replacement ||
                            source.source_id == replacement_id;
                    check(has_replacement,
                          "rebound revision records the replacement source");
                }
            }
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
