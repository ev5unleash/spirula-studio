// scheduler_test -- JobScheduler admission, persistence and recovery.
//
// Uses `--help` as the phase workload: real GPU execution belongs in the
// Phase-5 matrix, not in a unit test. The point exercised here is the queue:
// admission, leases, state transitions, atomic persistence, recovery
// semantics.

#include "app/JobScheduler.h"
#include "app/OutputLease.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace sched = app::sched;

namespace {

int failures = 0;

void check(bool c, const char* m) {
    if (c) std::printf("ok   %s\n", m);
    else { std::printf("FAIL %s\n", m); ++failures; }
}

bool wait_for(const std::function<bool()>& pred, int timeout_ms = 8000) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return pred();
}

}  // namespace

int main(int argc, char** argv) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("spirula_scheduler_test_" + std::to_string(nonce));
    std::error_code ec;
    fs::create_directories(root, ec);

    std::vector<sched::Event> events;
    std::mutex events_mu;

    {
        app::OutputLease first;
        app::OutputLease second;
        const fs::path output = root / "lease";
        std::string error;
        check(first.acquire(output, error), "first output lease acquired");
        check(fs::exists(output / ".spirula-output.lock"),
              "output lease creates lock file");
        check(!second.acquire(output, error),
              "second output lease rejects active owner");
        first.release();
        check(second.acquire(output, error),
              "output lease becomes available after release");
    }

    // The real binary: `spirula train --help` exits quickly, so jobs reach a
    // terminal failure without a GPU. The queue and reaping paths stay real.
#ifdef _WIN32
    const char* binary_name = "spirula.exe";
#else
    const char* binary_name = "spirula";
#endif
    fs::path exe_path = argc > 0 ? fs::path(argv[0]).parent_path() / binary_name
                                 : fs::path(binary_name);
    if (!fs::exists(exe_path)) exe_path = fs::path("build") / binary_name;
    const std::string exe = fs::exists(exe_path)
                                ? fs::absolute(exe_path).u8string()
                                : binary_name;
    {
        const fs::path migration = root / "migration";
        fs::create_directories(migration);
        const fs::path state = migration / "job-state.json";
        std::ofstream(state, std::ios::binary | std::ios::trunc)
            << R"({"schema_version":1,"jobs":[{"order":0,"job_id":"legacy-job","phase":"train","device":"gpu-legacy","device_name":"Legacy GPU","work_dir":")"
            << migration.generic_u8string()
            << R"(","run_dir":")" << (migration / "run").generic_u8string()
            << R"(","output_dir":"","created_at":"created","state":"Queued","attempt_id":"","error":"","pending_resume":false,"last_exit_code":-1,"args":["--help"]}]})";
        sched::JobScheduler migrated(migration.u8string(), exe);
        migrated.pause_dispatch(true);
        migrated.load();
        const auto rows = migrated.list();
        check(rows.size() == 1 && rows[0].phases.size() == 1 &&
                  rows[0].completed_prefix == 0,
              "schema-1 row migrates to one queued phase");
        migrated.save();
        std::ifstream in(state, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        check(text.str().find("\"schema_version\": 2") != std::string::npos,
              "migration writes schema 2 only");
    }
    check(!sched::path_claims_conflict({(root / "dataset").u8string(), false},
                                       {(root / "dataset-copy").u8string(), true}),
          "non-overlapping path claims stay independent");
    check(!sched::path_claims_conflict({(root / "dataset").u8string(), false},
                                       {(root / "dataset").u8string(), false}),
          "read-only path claims share");
    check(sched::path_claims_conflict({(root / "dataset").u8string(), false},
                                      {(root / "dataset" / "child").u8string(), true}),
          "write descendant conflicts with read ancestor");

    {
        const fs::path failure_root = root / "submit_failure";
        const fs::path state_tmp = failure_root / "job-state.json.tmp";
        const fs::path jobs_root = failure_root / "jobs";
        auto job_dir_count = [](const fs::path& path) {
            size_t count = 0;
            std::error_code iter_ec;
            for (const auto& entry : fs::directory_iterator(path, iter_ec))
                if (entry.is_directory(iter_ec)) ++count;
            return count;
        };
        sched::SubmitOpts failed_submit;
        failed_submit.phase = "train";
        failed_submit.device = "gpu-failure";
        failed_submit.work_dir = failure_root.u8string();
        failed_submit.args = {"--help"};

        {
            sched::JobScheduler blocked(failure_root.u8string(), exe);
            blocked.pause_dispatch(true);
            std::error_code mkdir_ec;
            check(fs::create_directory(state_tmp, mkdir_ec) && !mkdir_ec,
                  "scheduler state temp path becomes unusable");
            check(blocked.submit(failed_submit).empty(),
                  "submit rejects unwritable scheduler state");
            check(blocked.list().empty(),
                  "failed submit leaves no in-memory job record");
            check(job_dir_count(jobs_root) == 0,
                  "failed submit removes its run directory");
            check(!fs::exists(failure_root / "job-state.json"),
                  "failed submit leaves no state record");
        }

        std::error_code restore_ec;
        fs::remove(state_tmp, restore_ec);
        check(!restore_ec && !fs::exists(state_tmp),
              "scheduler state temp path becomes writable again");
        sched::JobScheduler restored(failure_root.u8string(), exe);
        restored.pause_dispatch(true);
        const std::string restored_id = restored.submit(failed_submit);
        const auto restored_jobs = restored.list();
        check(!restored_id.empty(), "next submit succeeds after state recovery");
        check(restored_jobs.size() == 1 && restored_jobs[0].order == 0,
              "next submit does not consume an order");
    }

    {
        const fs::path ownership_root = root / "ownership";
        auto read_text = [](const fs::path& path) {
            std::ifstream in(path, std::ios::binary);
            std::ostringstream text;
            text << in.rdbuf();
            return text.str();
        };
        auto job_dir_count = [](const fs::path& path) {
            size_t count = 0;
            std::error_code iter_ec;
            for (const auto& entry : fs::directory_iterator(path, iter_ec))
                if (entry.is_directory(iter_ec)) ++count;
            return count;
        };

        sched::JobScheduler owner(ownership_root.u8string(), exe);
        owner.pause_dispatch(true);
        sched::SubmitOpts existing;
        existing.phase = "train";
        existing.device = "gpu-owner";
        existing.work_dir = ownership_root.u8string();
        existing.args = {"--help"};
        const std::string owner_id = owner.submit(existing);
        check(!owner_id.empty(), "state owner persists an initial job");

        const fs::path state = ownership_root / "job-state.json";
        const std::string before = read_text(state);
        const size_t job_dirs_before = job_dir_count(ownership_root / "jobs");
        sched::JobScheduler contender(ownership_root.u8string(), exe);
        check(contender.state_error() ==
                  "scheduler state is owned by another application",
              "second scheduler reports state ownership failure");
        check(contender.submit(existing).empty(),
              "second scheduler rejects submit");
        check(contender.list().empty(),
              "second scheduler does not add an in-memory job");
        check(!contender.save(), "second scheduler rejects save");
        check(read_text(state) == before,
              "second scheduler leaves persisted collection intact");
        check(job_dir_count(ownership_root / "jobs") == job_dirs_before,
              "second scheduler creates no job directory");
        const auto owner_jobs = owner.list();
        check(owner_jobs.size() == 1 && owner_jobs[0].job_id == owner_id,
              "first scheduler retains its job");
    }

    {
        sched::JobScheduler s(root.u8string(), exe);
        s.set_event_callback([&](const sched::Event& e) {
            std::lock_guard<std::mutex> lk(events_mu);
            events.push_back(e);
        });
        app::OutputLease workspace_hold;
        const fs::path workflow_workspace = root / "workflow-workspace";
        std::string workspace_error;
        check(workspace_hold.acquire(workflow_workspace, workspace_error),
              "test holds workflow workspace");
        sched::WorkflowSubmitOpts workflow;
        workflow.work_dir = root.u8string();
        workflow.workspace = workflow_workspace.u8string();
        workflow.path_claims.push_back({workflow.workspace, true});
        workflow.add_scheduler_publish = false;
        sched::Phase prep;
        prep.phase = "prep";
        prep.planned_device = "gpu-workflow";
        prep.output = (root / "source-images").u8string();
        workflow.phases.push_back(std::move(prep));
        const std::string workflow_id = s.submit(workflow);
        check(wait_for([&] {
            for (const auto& job : s.list())
                if (job.job_id == workflow_id &&
                    job.state == sched::JobState::Blocked)
                    return true;
            return false;
        }), "dataset phase leases its workspace");
        for (const auto& job : s.list())
            if (job.job_id == workflow_id)
                check(job.path_claims.size() == 1,
                      "phase artifacts are not implicit write claims");
        check(!fs::exists(root / "source-images" / ".spirula-output.lock"),
              "dataset phase does not lock its read-only artifact");
        workspace_hold.release();
        s.remove(workflow_id);


        sched::SubmitOpts a;
        a.phase = "train";
        a.device = "gpu-a";
        a.work_dir = root.u8string();
        a.args = {"--help"};
        const std::string a_id = s.submit(a);

        sched::SubmitOpts b;
        b.phase = "train";
        b.device = "gpu-b";
        b.work_dir = root.u8string();
        b.args = {"--help"};
        const std::string b_id = s.submit(b);

        sched::SubmitOpts a2;    // same GPU as A; must wait behind A
        a2.phase = "train";
        a2.device = "gpu-a";
        a2.work_dir = root.u8string();
        a2.args = {"--help"};
        const std::string a2_id = s.submit(a2);

        check(!a_id.empty() && a_id != b_id && a_id != a2_id,
              "submitted three jobs with distinct ids");
        sched::SubmitOpts unresolved;
        unresolved.phase = "train";
        unresolved.device = "auto";
        unresolved.work_dir = root.u8string();
        unresolved.args = {"--help"};
        check(s.submit(unresolved).empty(),
              "unresolved auto device is rejected at submission");

        // A and B run concurrently; A2 queues behind A.
        check(wait_for([&] {
            auto lst = s.list();
            bool a_done = false, b_done = false;
            bool a2_queued_or_later = false;
            for (const auto& j : lst) {
                if (j.job_id == a_id && j.state == sched::JobState::Failed) a_done = true;
                if (j.job_id == b_id && j.state == sched::JobState::Failed) b_done = true;
                if (j.job_id == a2_id) a2_queued_or_later = true;
            }
            return a_done && b_done && a2_queued_or_later;
        }), "A and B complete while A2 waits its turn");

        check(wait_for([&] {
            auto lst = s.list();
            for (const auto& j : lst)
                if (j.job_id == a2_id && j.state == sched::JobState::Failed)
                    return true;
            return false;
        }), "A2 released and completed after A freed gpu-a");

        check(s.try_reserve_foreground_device("gpu-f"),
              "foreground reservation succeeds on idle device");
        check(s.try_reserve_foreground_device("gpu-f"),
              "same foreground reservation is idempotent");
        check(!s.try_reserve_foreground_device("gpu-other"),
              "foreground reservation rejects device changes");
        sched::SubmitOpts foreground;
        foreground.phase = "train";
        foreground.device = "gpu-f";
        foreground.work_dir = root.u8string();
        foreground.args = {"--help"};
        const std::string foreground_id = s.submit(foreground);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        bool foreground_queued = false;
        for (const auto& j : s.list())
            if (j.job_id == foreground_id && j.state == sched::JobState::Queued)
                foreground_queued = true;
        check(foreground_queued, "scheduled job waits behind foreground device");
        s.set_foreground_device("");
        check(wait_for([&] {
            for (const auto& j : s.list())
                if (j.job_id == foreground_id && j.state == sched::JobState::Failed)
                    return true;
            return false;
        }), "scheduled job runs after foreground release");

        s.set_device_validator([](const std::string& device, std::string& error) {
            if (device == "gpu-bad") {
                error = "test device unavailable";
                return false;
            }
            return true;
        });
        sched::SubmitOpts invalid;
        invalid.phase = "train";
        invalid.device = "gpu-bad";
        invalid.work_dir = root.u8string();
        invalid.args = {"--help"};
        const std::string invalid_id = s.submit(invalid);
        check(wait_for([&] {
            for (const auto& j : s.list())
                if (j.job_id == invalid_id && j.state == sched::JobState::Blocked)
                    return true;
            return false;
        }), "dispatch-time device validation blocks unavailable target");

        const fs::path conflict_output = root / "conflict-output";
        app::OutputLease held_output;
        std::string output_error;
        check(held_output.acquire(conflict_output, output_error),
              "test holds scheduler output target");
        sched::SubmitOpts conflict;
        conflict.phase = "train";
        conflict.device = "gpu-c";
        conflict.work_dir = root.u8string();
        conflict.output_dir = conflict_output.u8string();
        conflict.args = {"--help"};
        const std::string conflict_id = s.submit(conflict);
        check(wait_for([&] {
            for (const auto& j : s.list())
                if (j.job_id == conflict_id && j.state == sched::JobState::Blocked)
                    return true;
            return false;
        }), "output conflict blocks before worker launch");

        s.drain_events();
        {
            std::lock_guard<std::mutex> lk(events_mu);
            check(!events.empty(), "events drain on owning thread");
        }
        s.save();
    }   // destructor: shutdown

    // Recovery: a fresh scheduler reopens state.
    {
        sched::JobScheduler s2(root.u8string(), exe);
        s2.load();
        auto lst = s2.list();
        check(lst.size() == 6, "state reloaded with all 6 jobs");
        int failed = 0;
        bool saw_unpublished = false;
        for (const auto& j : lst)
            if (j.state == sched::JobState::Failed) {
                ++failed;
                saw_unpublished = saw_unpublished ||
                    j.error == "training worker produced no output directory";
            }
        check(failed == 4, "terminal states persisted across restart");
        check(saw_unpublished, "success without published training output is rejected");
        bool saw_blocked = false;
        for (const auto& j : lst)
            if (j.state == sched::JobState::Blocked &&
                j.error == "test device unavailable")
                saw_blocked = true;
        check(saw_blocked, "blocked device reason persists across restart");
        bool saw_output_blocked = false;
        for (const auto& j : lst)
            if (j.state == sched::JobState::Blocked &&
                j.error.find("output directory is already in use") != std::string::npos)
                saw_output_blocked = true;
        check(saw_output_blocked, "output conflict reason persists across restart");

        // Fabricate an Interrupted row, write it by hand, and reload.
    }
    {
        const fs::path state = root / "job-state.json";
        const fs::path backup = root / "job-state.json.bak";
        std::ostringstream saved;
        {
            std::ifstream in(state, std::ios::binary);
            saved << in.rdbuf();
        }
        std::ofstream(backup, std::ios::binary | std::ios::trunc) << saved.str();
        std::ofstream(state, std::ios::binary | std::ios::trunc)
            << "{not valid scheduler state";
        sched::JobScheduler recovered(root.u8string(), exe);
        recovered.load();
        check(recovered.state_error().empty() && recovered.list().size() == 6,
              "valid scheduler backup replaces corrupt primary on load");
    }
    {
        const fs::path state = root / "job-state.json";
        const fs::path backup = root / "job-state.json.bak";
        std::string saved;
        {
            std::ifstream in(state, std::ios::binary);
            std::ostringstream ss;
            ss << in.rdbuf();
            saved = ss.str();
        }
        std::ofstream(backup, std::ios::binary | std::ios::trunc) << saved;
        std::ofstream(state, std::ios::binary | std::ios::trunc)
            << R"({"schema_version":1,"jobs":[{"order":0,"job_id":"bad","phase":"invalid","device":"auto","device_name":"","work_dir":"","run_dir":"run","output_dir":"","created_at":"created","state":"Queued","attempt_id":"","error":"","pending_resume":false,"last_exit_code":-1,"args":[]}]})";
        sched::JobScheduler recovered(root.u8string(), exe);
        recovered.load();
        check(recovered.state_error().empty() && recovered.list().size() == 6,
              "parseable invalid job row falls back to valid scheduler backup");
    }
    // Synthetic recovery: Running persisted, must come back Interrupted.
    {
        const fs::path p = root / "job-state.json";
        std::string text;
        {
            std::ifstream in(p, std::ios::binary);
            std::ostringstream ss; ss << in.rdbuf(); text = ss.str();
        }
        // Flip one row to Running, save-as, then reload.
        const size_t pos = text.find("\"Failed\"");
        check(pos != std::string::npos, "state file contains a Failed row");
        if (pos != std::string::npos) {
            text.replace(pos, 8, "\"Running\"");
            std::ofstream out(p, std::ios::binary | std::ios::trunc);
            out << text;
        }
        sched::JobScheduler s3(root.u8string(), exe);
        s3.load();
        auto lst = s3.list();
        bool saw_interrupted = false;
        for (const auto& j : lst)
            if (j.state == sched::JobState::Interrupted) {
                saw_interrupted = true;
                check(j.pending_resume, "interrupted row has pending_resume set");
            }
        check(saw_interrupted, "persisted Running reloads as Interrupted");
    }

    // Blocked-retry: submit on a busy device with dispatch paused, retry.
    {
        sched::JobScheduler s4(root.u8string(), exe);
        s4.pause_dispatch(true);
        sched::SubmitOpts x;
        x.phase = "train";
        x.device = "gpu-x";
        x.work_dir = root.u8string();
        x.args = {"--help"};
        const std::string xid = s4.submit(x);
        check(!xid.empty(), "submit under paused dispatch");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        auto lst = s4.list();
        bool still_queued = false;
        for (const auto& j : lst)
            if (j.job_id == xid && j.state == sched::JobState::Queued)
                still_queued = true;
        check(still_queued, "paused dispatch leaves job queued");
        s4.pause_dispatch(false);
        check(wait_for([&] {
            auto l2 = s4.list();
            for (const auto& j : l2)
                if (j.job_id == xid && j.state == sched::JobState::Failed)
                    return true;
            return false;
        }), "unpausing dispatch runs the queued job");
    }

    if (failures) {
        std::printf("FAILED with %d error(s)\n", failures);
        return 1;
    }
    std::printf("All scheduler tests passed.\n");
    return 0;
}
