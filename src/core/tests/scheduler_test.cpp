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
        sched::JobScheduler s(root.u8string(), exe);
        s.set_event_callback([&](const sched::Event& e) {
            std::lock_guard<std::mutex> lk(events_mu);
            events.push_back(e);
        });

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
