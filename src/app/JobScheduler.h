#pragma once

// JobScheduler -- one local queue, FIFO per device, one process tree per GPU.
//
// Owns the list of submitted jobs, a lease per physical GPU it is using, and
// one supervising thread per active attempt. Jobs are immutable once their
// attempt starts; stop, crash and success end the attempt, never mutate it.
// Requeued attempts get a fresh attempt_id.
//
// The state file is the only authoritative record. In-memory state is a
// replica of the file; the file is the recovery record.
//
// ponytail: no survivor-PID start-token verification or host-RAM preflight.
// Those belong in a phase-3b pass.

#include "app/OutputLease.h"
#include "app/WorkerRequest.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace app::sched {

enum class JobState {
    Queued,
    Starting,
    Running,
    Stopping,
    Succeeded,
    Stopped,
    Failed,
    Interrupted,
    Blocked,
};

struct Job {
    uint64_t order = 0;
    std::string job_id;
    std::string phase;        // train | sfm | geometry
    std::string device;       // resolved device identity
    std::string device_name;  // display name captured at submission
    std::string work_dir;
    std::string run_dir;      // log/result root for this job
    std::string output_dir;   // phase output, when known
    std::vector<std::string> args;
    std::string created_at;   // ISO-ish; display only

    JobState state = JobState::Queued;
    std::string attempt_id;   // empty until Starting
    std::string error;        // failure/block reason
    bool pending_resume = false;
    int last_exit_code = -1;
};

struct SubmitOpts {
    std::string phase;
    std::string device;
    std::string device_name;
    std::string work_dir;
    std::string output_dir;
    std::vector<std::string> args;
};

// Events are queued by worker threads and drained by the owning UI thread.
struct Event {
    std::string job_id;
    JobState state;
    std::string line;         // one log line, if any
    std::string error;
};

class JobScheduler {
public:
    // exe_path is this executable's own path; the worker is spawned as
    // `<exe_path> worker --request <file>` so a PATH lookup cannot reach a
    // different build.
    explicit JobScheduler(std::string config_dir, std::string exe_path);
    ~JobScheduler();

    JobScheduler(const JobScheduler&) = delete;
    JobScheduler& operator=(const JobScheduler&) = delete;

    // Load job-state.json. Mid-flight rows become Interrupted with resume set.
    void load();

    // Force-persist now. Atomic rename in the config dir.
    bool save();
    std::string state_error() const;

private:
    // save() with _mu already held (supervisor threads reach it locked).
    bool save_locked();

public:

    std::string submit(const SubmitOpts& o);

    // UI-facing snapshot. Never blocks on a running attempt.
    std::vector<Job> list() const;

    void set_event_callback(std::function<void(const Event&)> cb);
    void drain_events();

    // Queue control. All are no-ops for unknown job_id.
    void stop_and_save(const std::string& job_id);
    void force_stop(const std::string& job_id);
    void cancel(const std::string& job_id);
    void set_device(const std::string& job_id, const std::string& device,
                    const std::string& device_name = {});
    using DeviceValidator =
        std::function<bool(const std::string& device, std::string& error)>;
    void set_device_validator(DeviceValidator validator);
    bool try_reserve_foreground_device(const std::string& device);
    void set_foreground_device(const std::string& device);
    void retry(const std::string& job_id);          // Interrupted/Failed/Stopped → Queued
    void remove(const std::string& job_id);         // pending/finished rows only
    void pause_dispatch(bool on);
    bool dispatch_paused() const;

    // Scheduler-level shutdown: stop-and-save every active job, join threads,
    // persist state. Used when the app exits.
    void shutdown();

private:
    struct Attempt {
        std::string id;
        std::string job_id;
        std::thread thread;
        OutputLease output_lease;
        std::atomic<bool> cancel{false};
        std::atomic<bool> stop{false};
        std::atomic<bool> finished{false};
    };

    void supervisor_main(std::shared_ptr<Attempt> att);
    void dispatch_one();      // UI/dispatch thread: claim device, spawn attempt
    void transition_locked(Job& j, JobState s, std::string err = "");
    void queue_event_locked(const Job& j, const std::string& line = "",
                            const std::string& err = "");
    void reap_finished();
    bool has_eligible_job_locked() const;
    std::string new_attempt_id();
    static std::string new_job_id();
    static std::string now_iso();
    bool acquire_state_lock();
    void release_state_lock();

    std::string _config_dir;
    std::string _state_path;
    std::string _lock_path;
    std::string _exe_path;

    mutable std::mutex _mu;
    std::condition_variable _cv;
    std::deque<std::string> _queue;                     // job_ids, FIFO
    std::unordered_map<std::string, std::shared_ptr<Job>>     _jobs;
    std::unordered_map<std::string, std::shared_ptr<Attempt>> _active;   // job_id → attempt
    std::deque<Event> _events;
    std::string _state_error;

    // device lease: normalized device string → job_id
    std::unordered_map<std::string, std::string> _leases;
    std::string _foreground_device;
    DeviceValidator _device_validator;

    std::function<void(const Event&)> _on_event;
    std::atomic<bool> _paused{false};
    std::atomic<bool> _shutdown{false};
    uint64_t _next_order = 0;
    std::thread _dispatcher;
#ifdef _WIN32
    void* _state_lock = nullptr;
#else
    int _state_lock = -1;
#endif

    void dispatcher_main();

};

const char* to_string(JobState s);

}  // namespace app::sched
