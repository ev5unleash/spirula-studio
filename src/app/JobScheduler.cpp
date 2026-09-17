// JobScheduler -- one local queue, FIFO per device, one process tree per GPU.

#include "app/JobScheduler.h"

#include "app/AppPaths.h"
#include "app/Subprocess.h"
#include "checkpoint/Resume.h"
#include "data/Json.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <unordered_set>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace app::sched {

namespace {

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", (unsigned)(unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

JobState state_from_string(const std::string& s) {
    if (s == "Queued")     return JobState::Queued;
    if (s == "Starting")   return JobState::Starting;
    if (s == "Running")    return JobState::Running;
    if (s == "Stopping")   return JobState::Stopping;
    if (s == "Succeeded")  return JobState::Succeeded;
    if (s == "Stopped")    return JobState::Stopped;
    if (s == "Failed")     return JobState::Failed;
    if (s == "Interrupted")return JobState::Interrupted;
    if (s == "Blocked")    return JobState::Blocked;
    return JobState::Failed;
}

uint64_t nonce() {
    static std::mt19937_64 rng{std::random_device{}()};
    return rng();
}

std::string format_hex(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)v);
    return buf;
}

std::string arg_value(const std::vector<std::string>& args, const char* key) {
    for (size_t i = args.size(); i-- > 1;)
        if (args[i - 1] == key) return args[i];
    return {};
}

fs::path training_output_dir(const Job& j) {
    const std::string prefix = arg_value(j.args, "--output-dir-prefix");
    const std::string name = arg_value(j.args, "--output-dir-name");
    if (prefix.empty() || name.empty()) return {};
    return fs::u8path(prefix) / fs::u8path(name);
}

bool training_output_published(const Job& j, std::string& error) {
    if (j.output_dir.empty()) {
        error = "training worker produced no output directory";
        return false;
    }
    const fs::path config = fs::u8path(j.output_dir) / "config.json";
    std::error_code ec;
    if (!fs::is_regular_file(config, ec) || fs::file_size(config, ec) == 0 || ec) {
        error = "training worker produced no published config.json";
        return false;
    }
    try {
        if (json_parse(read_file(config.u8string())).type != JsonValue::Type::Object)
            throw std::runtime_error("not an object");
    } catch (...) {
        error = "training worker produced an invalid config.json";
        return false;
    }
    return true;
}

void set_arg_value(std::vector<std::string>& args, const char* key,
                   const std::string& value) {
    for (size_t i = args.size(); i-- > 1;) {
        if (args[i - 1] == key) {
            args[i] = value;
            return;
        }
    }
    args.push_back(key);
    args.push_back(value);
}

}  // namespace

const char* to_string(JobState s) {
    switch (s) {
        case JobState::Queued:      return "Queued";
        case JobState::Starting:    return "Starting";
        case JobState::Running:     return "Running";
        case JobState::Stopping:    return "Stopping";
        case JobState::Succeeded:   return "Succeeded";
        case JobState::Stopped:     return "Stopped";
        case JobState::Failed:      return "Failed";
        case JobState::Interrupted: return "Interrupted";
        case JobState::Blocked:     return "Blocked";
    }
    return "Unknown";
}

JobScheduler::JobScheduler(std::string config_dir, std::string exe_path)
    : _config_dir(std::move(config_dir)), _exe_path(std::move(exe_path)) {
    _state_path = (fs::u8path(_config_dir) / "job-state.json").u8string();
    _lock_path = (fs::u8path(_config_dir) / "job-state.lock").u8string();
    fs::create_directories(fs::u8path(_config_dir));
    acquire_state_lock();
    _dispatcher = std::thread([this] { dispatcher_main(); });
}

JobScheduler::~JobScheduler() {
    shutdown();
    release_state_lock();
}

bool JobScheduler::acquire_state_lock() {
#ifdef _WIN32
    HANDLE h = CreateFileW(fs::u8path(_lock_path).wstring().c_str(),
                           GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        _state_error = "scheduler state is owned by another application";
        _paused.store(true);
        return false;
    }
    _state_lock = h;
    return true;
#else
    _state_lock = ::open(_lock_path.c_str(), O_CREAT | O_RDWR, 0666);
    if (_state_lock < 0 || flock(_state_lock, LOCK_EX | LOCK_NB) != 0) {
        if (_state_lock >= 0) { ::close(_state_lock); _state_lock = -1; }
        _state_error = "scheduler state is owned by another application";
        _paused.store(true);
        return false;
    }
    return true;
#endif
}

void JobScheduler::release_state_lock() {
#ifdef _WIN32
    if (_state_lock) {
        CloseHandle(static_cast<HANDLE>(_state_lock));
        _state_lock = nullptr;
    }
#else
    if (_state_lock >= 0) {
        flock(_state_lock, LOCK_UN);
        ::close(_state_lock);
        _state_lock = -1;
    }
#endif
}

void JobScheduler::dispatcher_main() {
    std::unique_lock<std::mutex> lk(_mu);
    for (;;) {
        _cv.wait(lk, [&] {
            if (_shutdown.load()) return true;
            if (has_eligible_job_locked()) return true;
            for (const auto& [_, a] : _active)
                if (a->finished.load()) return true;
            return false;
        });
        if (_shutdown.load()) return;
        lk.unlock();
        reap_finished();
        dispatch_one();
        lk.lock();
    }
}

std::string JobScheduler::new_job_id() {
    return "job-" + format_hex(nonce());
}

std::string JobScheduler::new_attempt_id() {
    return "att-" + format_hex(nonce());
}

std::string JobScheduler::now_iso() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

void JobScheduler::set_event_callback(std::function<void(const Event&)> cb) {
    std::lock_guard<std::mutex> lk(_mu);
    _on_event = std::move(cb);
}

void JobScheduler::drain_events() {
    std::deque<Event> events;
    std::function<void(const Event&)> cb;
    {
        std::lock_guard<std::mutex> lk(_mu);
        cb = _on_event;
        events.swap(_events);
    }
    if (!cb) return;
    for (const Event& e : events) cb(e);
}

void JobScheduler::queue_event_locked(const Job& j, const std::string& line,
                                      const std::string& err) {
    Event e;
    e.job_id = j.job_id;
    e.state = j.state;
    e.line = line;
    e.error = err;
    _events.push_back(std::move(e));
    while (_events.size() > 4096) _events.pop_front();
}

void JobScheduler::transition_locked(Job& j, JobState s, std::string err) {
    j.state = s;
    j.error = std::move(err);
    queue_event_locked(j);
}

std::string JobScheduler::submit(const SubmitOpts& o) {
    if (o.device.empty() || o.device == "auto") return {};
    auto j = std::make_shared<Job>();
    j->job_id = new_job_id();
    j->phase = o.phase;
    j->device = o.device;
    j->device_name = o.device_name;
    j->work_dir = o.work_dir;
    j->output_dir = o.output_dir;
    j->args = o.args;
    j->created_at = now_iso();
    j->state = JobState::Queued;
    if (j->phase == "train")
        set_arg_value(j->args, "--output-dir-name", j->job_id);
    if (j->output_dir.empty()) {
        if (const fs::path output = training_output_dir(*j); !output.empty())
            j->output_dir = output.u8string();
    }

    // Per-job run dir under config/jobs/<id>; holds request.json, result.json, log.txt.
    const fs::path dir = fs::u8path(_config_dir) / "jobs" / j->job_id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return {};
    j->run_dir = dir.u8string();

    {
        std::lock_guard<std::mutex> lk(_mu);
        j->order = _next_order++;
        _jobs[j->job_id] = j;
        _queue.push_back(j->job_id);
        queue_event_locked(*j);
        if (!save_locked()) {
            _jobs.erase(j->job_id);
            _queue.pop_back();
            _events.pop_back();
            return {};
        }
    }
    _cv.notify_all();
    return j->job_id;
}

std::vector<Job> JobScheduler::list() const {
    std::lock_guard<std::mutex> lk(_mu);
    std::vector<Job> out;
    out.reserve(_jobs.size());
    for (const auto& [_, j] : _jobs) out.push_back(*j);
    return out;
}

std::string JobScheduler::state_error() const {
    std::lock_guard<std::mutex> lk(_mu);
    return _state_error;
}

void JobScheduler::stop_and_save(const std::string& job_id) {
    std::lock_guard<std::mutex> lk(_mu);
    auto it = _active.find(job_id);
    if (it == _active.end()) return;
    auto jit = _jobs.find(job_id);
    if (jit == _jobs.end()) return;
    if (jit->second->state == JobState::Starting ||
        jit->second->state == JobState::Running) {
        jit->second->state = JobState::Stopping;
        queue_event_locked(*jit->second);
        save_locked();
    }
    it->second->stop.store(true);
}

void JobScheduler::force_stop(const std::string& job_id) {
    std::lock_guard<std::mutex> lk(_mu);
    auto it = _active.find(job_id);
    if (it == _active.end()) return;
    auto jit = _jobs.find(job_id);
    if (jit == _jobs.end()) return;
    if (jit->second->state == JobState::Starting ||
        jit->second->state == JobState::Running) {
        jit->second->state = JobState::Stopping;
        queue_event_locked(*jit->second);
        save_locked();
    }
    it->second->cancel.store(true);
}

void JobScheduler::cancel(const std::string& job_id) {
    std::lock_guard<std::mutex> lk(_mu);
    auto it = _jobs.find(job_id);
    if (it == _jobs.end()) return;
    Job& j = *it->second;
    if (j.state != JobState::Queued) return;
    _queue.erase(std::remove(_queue.begin(), _queue.end(), job_id), _queue.end());
    j.state = JobState::Stopped;
    j.error = "cancelled";
    queue_event_locked(j);
    save_locked();
}

void JobScheduler::set_device(const std::string& job_id, const std::string& device,
                              const std::string& device_name) {
    if (device.empty() || device == "auto") return;
    std::lock_guard<std::mutex> lk(_mu);
    auto it = _jobs.find(job_id);
    if (it == _jobs.end() || it->second->state != JobState::Queued) return;
    Job& j = *it->second;
    j.device = device;
    if (!device_name.empty()) j.device_name = device_name;
    queue_event_locked(j);
    save_locked();
    _cv.notify_all();
}

void JobScheduler::set_device_validator(DeviceValidator validator) {
    {
        std::lock_guard<std::mutex> lk(_mu);
        _device_validator = std::move(validator);
    }
    _cv.notify_all();
}

bool JobScheduler::try_reserve_foreground_device(const std::string& device) {
    if (device.empty()) return false;
    std::lock_guard<std::mutex> lk(_mu);
    if (_leases.count(device) != 0) return false;
    if (!_foreground_device.empty() && _foreground_device != device) return false;
    _foreground_device = device;
    return true;
}

void JobScheduler::set_foreground_device(const std::string& device) {
    {
        std::lock_guard<std::mutex> lk(_mu);
        if (_foreground_device == device) return;
        if (!device.empty() && _leases.count(device) != 0) return;
        _foreground_device = device;
    }
    _cv.notify_all();
}

void JobScheduler::retry(const std::string& job_id) {
    {
        std::lock_guard<std::mutex> lk(_mu);
        auto jit = _jobs.find(job_id);
        if (jit == _jobs.end()) return;
        Job& j = *jit->second;
        if (j.state != JobState::Interrupted && j.state != JobState::Failed &&
            j.state != JobState::Stopped && j.state != JobState::Blocked)
            return;
        j.attempt_id.clear();
        j.error.clear();
        j.pending_resume = false;
        if (j.phase == "train") {
            const fs::path output = training_output_dir(j);
            try {
                const ckpt::ResolvedCheckpoint resolved =
                    ckpt::resolve_checkpoint(output);
                ckpt::check_resumable(resolved.ckpt_dir);
                set_arg_value(j.args, "--resume", output.u8string());
            } catch (...) {
            }
        }
        j.state = JobState::Queued;
        _queue.push_back(job_id);
        queue_event_locked(j);
        save_locked();
    }
    _cv.notify_all();
}

void JobScheduler::remove(const std::string& job_id) {
    std::lock_guard<std::mutex> lk(_mu);
    auto jit = _jobs.find(job_id);
    if (jit == _jobs.end()) return;
    const Job& j = *jit->second;
    if (j.state == JobState::Running || j.state == JobState::Starting ||
        j.state == JobState::Stopping)
        return;
    _jobs.erase(jit);
    for (auto it = _queue.begin(); it != _queue.end(); ++it) {
        if (*it == job_id) { _queue.erase(it); break; }
    }
    save_locked();
}

void JobScheduler::pause_dispatch(bool on) {
    _paused.store(on);
    if (!on) _cv.notify_all();
}

bool JobScheduler::dispatch_paused() const { return _paused.load(); }

bool JobScheduler::save() {
    std::lock_guard<std::mutex> lk(_mu);
    return save_locked();
}

// Caller must hold _mu; supervisor_main and remove() reach it already locked.
bool JobScheduler::save_locked() {
    if (!_state_error.empty()) return false;
    auto fail = [this](const char* reason) {
        _state_error = reason;
        _paused.store(true);
        return false;
    };
    const fs::path tmp = fs::u8path(_state_path + ".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return fail("cannot write scheduler state");
        f << "{\n  \"schema_version\": 1,\n  \"jobs\": [\n";
        bool first = true;
        for (const auto& [_, jp] : _jobs) {
            const Job& j = *jp;
            if (!first) f << ",\n";
            first = false;
            f << "    {\"order\": " << j.order
               << ", \"job_id\": \"" << json_escape(j.job_id) << "\""
               << ", \"phase\": \"" << json_escape(j.phase) << "\""
               << ", \"device\": \"" << json_escape(j.device) << "\""
               << ", \"device_name\": \"" << json_escape(j.device_name) << "\""
               << ", \"work_dir\": \"" << json_escape(j.work_dir) << "\""
               << ", \"run_dir\": \"" << json_escape(j.run_dir) << "\""
               << ", \"output_dir\": \"" << json_escape(j.output_dir) << "\""
              << ", \"created_at\": \"" << json_escape(j.created_at) << "\""
              << ", \"state\": \"" << to_string(j.state) << "\""
              << ", \"attempt_id\": \"" << json_escape(j.attempt_id) << "\""
              << ", \"error\": \"" << json_escape(j.error) << "\""
              << ", \"pending_resume\": " << (j.pending_resume ? "true" : "false")
              << ", \"last_exit_code\": " << j.last_exit_code
              << ", \"args\": [";
            for (size_t i = 0; i < j.args.size(); ++i) {
                if (i) f << ", ";
                f << "\"" << json_escape(j.args[i]) << "\"";
            }
            f << "]}";
        }
        f << "\n  ]\n}\n";
        f.flush();
        if (!f) {
            std::error_code ec;
            fs::remove(tmp, ec);
            return fail("cannot flush scheduler state");
        }
    }
    const fs::path target = fs::u8path(_state_path);
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (!ec) return true;

    const fs::path backup = fs::u8path(_state_path + ".bak");
    std::error_code backup_ec;
    fs::remove(backup, backup_ec);
    backup_ec.clear();
    fs::rename(target, backup, backup_ec);
    if (backup_ec) {
        fs::remove(tmp, ec);
        return fail("cannot replace scheduler state");
    }
    std::error_code replace_ec;
    fs::rename(tmp, target, replace_ec);
    if (replace_ec) {
        std::error_code restore_ec;
        fs::rename(backup, target, restore_ec);
        fs::remove(tmp, ec);
        return fail("cannot replace scheduler state");
    }
    fs::remove(backup, backup_ec);
    return true;
}

void JobScheduler::load() {
    const fs::path state_path = fs::u8path(_state_path);
    const fs::path backup_path = fs::u8path(_state_path + ".bak");
    JsonValue root;
    std::string load_error;
    bool found_state = false;
    bool found_file = false;
    auto read_state = [&](const fs::path& path, std::string& error) {
        const std::string text = read_file(path.u8string());
        if (text.empty()) {
            error = "scheduler state is empty";
            return false;
        }
        try {
            root = json_parse(text);
        } catch (...) {
            error = "cannot parse scheduler state";
            return false;
        }
        if (root.type != JsonValue::Type::Object) {
            error = "scheduler state root is not an object";
            return false;
        }
        const JsonValue* schema = root.find("schema_version");
        if (!schema || schema->type != JsonValue::Type::Number ||
            schema->num != 1) {
            error = "unsupported scheduler state schema";
            return false;
        }
        const JsonValue* jobs = root.find("jobs");
        if (!jobs || jobs->type != JsonValue::Type::Array) {
            error = "scheduler state has no jobs array";
            return false;
        }
        const auto has_type = [](const JsonValue& object, const char* name,
                                 JsonValue::Type type) {
            const JsonValue* value = object.find(name);
            return value && value->type == type;
        };
        std::unordered_set<std::string> ids;
        std::unordered_set<uint64_t> orders;
        const auto finite_integer = [](double value) {
            return std::isfinite(value) && std::trunc(value) == value;
        };
        for (const JsonValue& el : jobs->arr) {
            if (el.type != JsonValue::Type::Object) {
                error = "scheduler state has an invalid job";
                return false;
            }
            if (!has_type(el, "order", JsonValue::Type::Number) ||
                !has_type(el, "job_id", JsonValue::Type::String) ||
                !has_type(el, "phase", JsonValue::Type::String) ||
                !has_type(el, "device", JsonValue::Type::String) ||
                !has_type(el, "device_name", JsonValue::Type::String) ||
                !has_type(el, "work_dir", JsonValue::Type::String) ||
                !has_type(el, "run_dir", JsonValue::Type::String) ||
                !has_type(el, "output_dir", JsonValue::Type::String) ||
                !has_type(el, "created_at", JsonValue::Type::String) ||
                !has_type(el, "state", JsonValue::Type::String) ||
                !has_type(el, "attempt_id", JsonValue::Type::String) ||
                !has_type(el, "error", JsonValue::Type::String) ||
                !has_type(el, "pending_resume", JsonValue::Type::Bool) ||
                !has_type(el, "last_exit_code", JsonValue::Type::Number) ||
                !has_type(el, "args", JsonValue::Type::Array)) {
                error = "scheduler state has an invalid job";
                return false;
            }
            const JsonValue* order = el.find("order");
            const JsonValue* phase = el.find("phase");
            const JsonValue* device = el.find("device");
            const JsonValue* run_dir = el.find("run_dir");
            const JsonValue* created_at = el.find("created_at");
            const JsonValue* state = el.find("state");
            const JsonValue* attempt_id = el.find("attempt_id");
            const JsonValue* last_exit_code = el.find("last_exit_code");
            if (!finite_integer(order->num) || order->num < 0.0 ||
                order->num >= std::ldexp(1.0, std::numeric_limits<uint64_t>::digits) ||
                !orders.insert(static_cast<uint64_t>(order->num)).second ||
                (phase->str != "train" && phase->str != "sfm" &&
                 phase->str != "geometry") ||
                device->str.empty() || device->str == "auto" ||
                run_dir->str.empty() || created_at->str.empty() ||
                !finite_integer(last_exit_code->num) ||
                last_exit_code->num < std::numeric_limits<int>::min() ||
                last_exit_code->num > std::numeric_limits<int>::max() ||
                ((state->str == "Starting" || state->str == "Running" ||
                  state->str == "Stopping") && attempt_id->str.empty())) {
                error = "scheduler state has an invalid job";
                return false;
            }
            const JsonValue* id = el.find("job_id");
            if (id->str.empty()) {
                error = "scheduler state has an invalid job";
                return false;
            }
            if (!ids.insert(id->str).second) {
                error = "scheduler state has duplicate job id";
                return false;
            }
            if (state->str != to_string(state_from_string(state->str))) {
                error = "scheduler state has an invalid job";
                return false;
            }
            const JsonValue* args = el.find("args");
            for (const JsonValue& arg : args->arr) {
                if (arg.type != JsonValue::Type::String) {
                    error = "scheduler state has an invalid job";
                    return false;
                }
            }
        }
        return true;
    };
    for (const fs::path& candidate : {state_path, backup_path}) {
        std::error_code ec;
        if (!fs::exists(candidate, ec) || ec) continue;
        found_file = true;
        if (read_state(candidate, load_error)) {
            found_state = true;
            break;
        }
    }
    if (!found_file) return;
    if (!found_state) {
        std::lock_guard<std::mutex> lk(_mu);
        _state_error = load_error.empty() ? "cannot load scheduler state"
                                          : load_error;
        _paused.store(true);
        return;
    }

    const JsonValue* jobs = root.find("jobs");
    std::lock_guard<std::mutex> lk(_mu);
    uint64_t fallback_order = 0;
    for (const JsonValue& el : jobs->arr) {
        auto j = std::make_shared<Job>();
        if (const JsonValue* v = el.find("order"))
            j->order = static_cast<uint64_t>(v->num);
        else
            j->order = fallback_order;
        fallback_order++;
        _next_order = std::max(_next_order, j->order + 1);
        j->job_id     = el.find("job_id")     ? el.find("job_id")->str     : "";
        j->phase      = el.find("phase")      ? el.find("phase")->str      : "";
        j->device     = el.find("device")     ? el.find("device")->str     : "auto";
        j->device_name = el.find("device_name") ? el.find("device_name")->str : "";
        j->work_dir   = el.find("work_dir")   ? el.find("work_dir")->str   : "";
        j->run_dir    = el.find("run_dir")    ? el.find("run_dir")->str    : "";
        j->output_dir = el.find("output_dir") ? el.find("output_dir")->str : "";
        j->created_at = el.find("created_at") ? el.find("created_at")->str : "";
        j->attempt_id = el.find("attempt_id") ? el.find("attempt_id")->str : "";
        j->error      = el.find("error")      ? el.find("error")->str      : "";
        j->pending_resume = el.find("pending_resume")
                            ? el.find("pending_resume")->as_bool(false) : false;
        j->last_exit_code = el.find("last_exit_code")
                            ? static_cast<int>(el.find("last_exit_code")->num) : -1;
        JobState s = el.find("state") ? state_from_string(el.find("state")->str)
                                      : JobState::Failed;
        // Anything that was mid-flight at crash is Interrupted and needs an
        // explicit recover. Queued rows stay queued.
        if (s == JobState::Starting || s == JobState::Running || s == JobState::Stopping) {
            s = JobState::Interrupted;
            j->pending_resume = true;
        }
        j->state = s;
        if (const JsonValue* a = el.find("args"))
            for (const JsonValue& v : a->arr)
                if (v.type == JsonValue::Type::String)
                    j->args.push_back(v.str);
        if (j->job_id.empty()) continue;
        _jobs[j->job_id] = j;
        if (j->state == JobState::Queued) _queue.push_back(j->job_id);
    }
    std::sort(_queue.begin(), _queue.end(), [this](const std::string& a,
                                                   const std::string& b) {
        return _jobs.at(a)->order < _jobs.at(b)->order;
    });
    _cv.notify_all();
}

bool JobScheduler::has_eligible_job_locked() const {
    if (_paused.load() || _shutdown.load() || !_state_error.empty()) return false;
    for (const std::string& id : _queue) {
        auto it = _jobs.find(id);
        if (it != _jobs.end() && it->second->state == JobState::Queued &&
            !_leases.count(it->second->device) &&
            it->second->device != _foreground_device) return true;
    }
    return false;
}

void JobScheduler::reap_finished() {
    std::vector<std::shared_ptr<Attempt>> done;
    {
        std::lock_guard<std::mutex> lk(_mu);
        for (auto it = _active.begin(); it != _active.end();) {
            if (!it->second->finished.load()) {
                ++it;
                continue;
            }
            done.push_back(it->second);
            it = _active.erase(it);
        }
    }
    for (const auto& att : done)
        if (att->thread.joinable()) att->thread.join();
}

void JobScheduler::dispatch_one() {
    std::shared_ptr<Job> j;
    std::shared_ptr<Attempt> att;
    {
        std::lock_guard<std::mutex> lk(_mu);
        for (;;) {
            if (!has_eligible_job_locked()) return;
            size_t idx = _queue.size();
            bool discarded = false;
            for (size_t i = 0; i < _queue.size(); ++i) {
                auto it = _jobs.find(_queue[i]);
                if (it == _jobs.end() || it->second->state != JobState::Queued)
                    continue;
                const std::string& dev = it->second->device;
                if (_leases.count(dev) != 0 || dev == _foreground_device)
                    continue;
                if (dev.empty() || dev == "auto") {
                    Job& blocked = *it->second;
                    _queue.erase(_queue.begin() + (ptrdiff_t)i);
                    transition_locked(blocked, JobState::Blocked,
                                      "device must be resolved before dispatch");
                    if (!save_locked()) _paused.store(true);
                    discarded = true;
                    break;
                }
                if (_device_validator) {
                    std::string error;
                    bool valid = false;
                    try {
                        valid = _device_validator(dev, error);
                    } catch (const std::exception& e) {
                        error = e.what();
                    }
                    if (!valid) {
                        Job& blocked = *it->second;
                        _queue.erase(_queue.begin() + (ptrdiff_t)i);
                        transition_locked(
                            blocked, JobState::Blocked,
                            error.empty() ? "device is unavailable" : error);
                        if (!save_locked()) _paused.store(true);
                        discarded = true;
                        break;
                    }
                }
                idx = i;
                break;
            }
            if (discarded) continue;
            if (idx == _queue.size()) {
                return;
            }
            j = _jobs[_queue[idx]];
            auto candidate = std::make_shared<Attempt>();
            if (!j->output_dir.empty()) {
                std::string error;
                if (!candidate->output_lease.acquire(fs::u8path(j->output_dir), error)) {
                    _queue.erase(_queue.begin() + (ptrdiff_t)idx);
                    transition_locked(
                        *j, JobState::Blocked,
                        error.empty() ? "output directory is unavailable" : error);
                    if (!save_locked()) _paused.store(true);
                    continue;
                }
            }
            const JobState previous_state = j->state;
            const std::string previous_attempt = j->attempt_id;
            const std::string previous_error = j->error;
            j->attempt_id = new_attempt_id();
            j->state = JobState::Starting;
            j->error.clear();
            if (!save_locked()) {
                j->state = previous_state;
                j->attempt_id = previous_attempt;
                j->error = previous_error;
                _paused.store(true);
                return;
            }

            _queue.erase(_queue.begin() + (ptrdiff_t)idx);
            _leases[j->device] = j->job_id;
            queue_event_locked(*j);
            att = std::move(candidate);
            att->id = j->attempt_id;
            att->job_id = j->job_id;
            _active[j->job_id] = att;
            break;
        }
    }
    try {
        att->thread = std::thread([this, att] { supervisor_main(att); });
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lk(_mu);
        _active.erase(j->job_id);
        _leases.erase(j->device);
        transition_locked(*j, JobState::Failed, e.what());
        save_locked();
    }
}

void JobScheduler::supervisor_main(std::shared_ptr<Attempt> att) {
    std::shared_ptr<Job> j;
    {
        std::lock_guard<std::mutex> lk(_mu);
        auto it = _jobs.find(att->job_id);
        if (it == _jobs.end()) {
            att->finished.store(true);
            _cv.notify_all();
            return;
        }
        j = it->second;
    }

    const fs::path run_dir = fs::u8path(j->run_dir);
    const fs::path req_path = run_dir / ("request-" + att->id + ".json");
    const fs::path res_path = run_dir / ("result-"  + att->id + ".json");
    const fs::path log_path = run_dir / ("log-"     + att->id + ".txt");
    {
        std::ofstream f(req_path, std::ios::binary | std::ios::trunc);
        if (!f) {
            std::lock_guard<std::mutex> lk(_mu);
            transition_locked(*j, JobState::Failed, "cannot write request file");
            _leases.erase(j->device);
            att->finished.store(true);
            save_locked();
            _cv.notify_all();
            return;
        }
        f << "{\n"
          << "  \"schema_version\": 1,\n"
          << "  \"job_id\": \"" << json_escape(j->job_id) << "\",\n"
          << "  \"attempt_id\": \"" << json_escape(att->id) << "\",\n"
          << "  \"phase\": \"" << json_escape(j->phase) << "\",\n"
          << "  \"device\": \"" << json_escape(j->device) << "\",\n"
          << "  \"work_dir\": \"" << json_escape(j->work_dir) << "\",\n"
          << "  \"result_path\": \"" << json_escape(res_path.u8string()) << "\",\n"
          << "  \"args\": [";
        for (size_t i = 0; i < j->args.size(); ++i) {
            if (i) f << ", ";
            f << "\"" << json_escape(j->args[i]) << "\"";
        }
        f << "]\n}\n";
        f.flush();
        if (!f) {
            std::lock_guard<std::mutex> lk(_mu);
            transition_locked(*j, JobState::Failed, "cannot write request file");
            _leases.erase(j->device);
            att->finished.store(true);
            save_locked();
            _cv.notify_all();
            return;
        }
    }

    {
        std::lock_guard<std::mutex> lk(_mu);
        if (j->state == JobState::Starting) {
            j->state = JobState::Running;
            if (save_locked())
                queue_event_locked(*j);
            else
                j->state = JobState::Starting;
        }
    }

    std::ofstream log_file(log_path, std::ios::binary | std::ios::trunc);
    proc::ProcessOptions opts;
    opts.argv = {_exe_path, "worker", "--request", req_path.u8string()};
    opts.cwd = j->work_dir;
    opts.cancel = &att->cancel;
    opts.stop = &att->stop;
    opts.stop_token = "STOP\n";
    opts.grace_period_ms = j->phase == "train" ? 300000 : 30000;
    opts.env_overrides = {{"SS_WORKER_CONTROL", "1"}};
    if (att->output_lease.valid()) {
        opts.env_overrides.push_back({"SS_OUTPUT_LEASE_HELD", "1"});
#ifdef _WIN32
        opts.inherit_handles.push_back(att->output_lease.native_handle());
        opts.env_overrides.push_back({
            "SS_OUTPUT_LEASE_HANDLE",
            std::to_string(reinterpret_cast<uintptr_t>(
                att->output_lease.native_handle()))});
#else
        opts.inherit_fds.push_back(att->output_lease.native_fd());
        opts.env_overrides.push_back({
            "SS_OUTPUT_LEASE_FD",
            std::to_string(att->output_lease.native_fd())});
#endif
    }
    opts.on_line = [this, &log_file, job_id = att->job_id](const std::string& line) {
        if (log_file) {
            log_file << line << "\n";
            log_file.flush();
        }
        std::lock_guard<std::mutex> lk(_mu);
        auto it = _jobs.find(job_id);
        if (it == _jobs.end()) return;
        queue_event_locked(*it->second, line);
    };
    const proc::ProcessResult res = proc::run_process(opts);
    if (log_file) log_file.flush();

    std::string outcome;
    int exit_code = res.exit_code;
    std::string message = res.error_message;
    bool result_ok = false;
    try {
        const std::string text = read_file(res_path);
        if (!text.empty()) {
            const JsonValue r = json_parse(text);
            const JsonValue* schema = r.find("schema_version");
            const JsonValue* result_job = r.find("job_id");
            const JsonValue* result_attempt = r.find("attempt_id");
            const JsonValue* result_phase = r.find("phase");
            const JsonValue* o = r.find("outcome");
            result_ok = schema && schema->as_int(0) == 1 &&
                        result_job && result_job->str == att->job_id &&
                        result_attempt && result_attempt->str == att->id &&
                        result_phase && result_phase->str == j->phase &&
                        o && (o->str == "success" || o->str == "stopped" ||
                              o->str == "failed");
            if (o) outcome = o->str;
            const JsonValue* m = r.find("message");
            if (m && !m->str.empty()) message = m->str;
            const JsonValue* e = r.find("exit_code");
            if (e) exit_code = (int)e->as_int(exit_code);
        }
    } catch (...) {}
    if (result_ok && outcome == "success" && j->phase == "train") {
        std::string artifact_error;
        if (!training_output_published(*j, artifact_error)) {
            result_ok = false;
            if (message.empty()) message = std::move(artifact_error);
        }
    }

    {
        std::lock_guard<std::mutex> lk(_mu);
        auto it = _jobs.find(att->job_id);
        if (it != _jobs.end()) {
            Job& job = *it->second;
            _leases.erase(job.device);
            const bool was_cancelled = res.outcome == proc::ProcessOutcome::Cancelled;
            const bool was_stopped  = res.outcome == proc::ProcessOutcome::Stopped;
            const bool was_spawn    = res.outcome == proc::ProcessOutcome::SpawnFailed;
            if (was_cancelled) {
                transition_locked(job, JobState::Interrupted, "force-stopped");
                job.pending_resume = true;
            } else if (res.outcome == proc::ProcessOutcome::Success &&
                       exit_code == 0 && result_ok && outcome == "success") {
                transition_locked(job, JobState::Succeeded);
            } else if (result_ok && outcome == "stopped" &&
                       (res.outcome == proc::ProcessOutcome::Success || was_stopped)) {
                transition_locked(job, JobState::Stopped);
                job.pending_resume = true;
            } else if (was_spawn) {
                transition_locked(job, JobState::Failed,
                                  message.empty() ? "worker could not launch phase" : message);
            } else {
                transition_locked(job, JobState::Failed,
                                  message.empty() ? "worker exited " +
                                      std::to_string(exit_code) : message);
            }
            job.last_exit_code = exit_code;
        }
        att->finished.store(true);
        save_locked();
    }
    _cv.notify_all();
}

void JobScheduler::shutdown() {
    bool expected = false;
    if (!_shutdown.compare_exchange_strong(expected, true)) return;
    _cv.notify_all();
    if (_dispatcher.joinable()) _dispatcher.join();

    std::vector<std::shared_ptr<Attempt>> live;
    {
        std::lock_guard<std::mutex> lk(_mu);
        for (auto& [_, a] : _active) live.push_back(a);
    }
    for (auto& a : live) a->stop.store(true);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(8);
    for (;;) {
        bool done = true;
        for (const auto& a : live)
            done = done && a->finished.load();
        if (done || std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (auto& a : live)
        if (!a->finished.load()) a->cancel.store(true);
    for (auto& a : live)
        if (a->thread.joinable()) a->thread.join();
    std::lock_guard<std::mutex> lk(_mu);
    save_locked();
}

}  // namespace app::sched
