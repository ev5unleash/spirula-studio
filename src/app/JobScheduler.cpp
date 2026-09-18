// JobScheduler -- one local queue, FIFO per device, one process tree per GPU.

#include "app/JobScheduler.h"

#include "app/Subprocess.h"
#include "checkpoint/Resume.h"
#include "data/Json.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

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
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", (unsigned)c);
                    out += buf;
                } else out += (char)c;
        }
    }
    return out;
}

std::string quote(const std::string& s) { return "\"" + json_escape(s) + "\""; }

uint64_t nonce() {
    static std::mt19937_64 rng{std::random_device{}()};
    return rng();
}

std::string format_hex(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)v);
    return buf;
}

JobState state_from_string(const std::string& s) {
    if (s == "Queued") return JobState::Queued;
    if (s == "Starting") return JobState::Starting;
    if (s == "Running") return JobState::Running;
    if (s == "Stopping") return JobState::Stopping;
    if (s == "Succeeded") return JobState::Succeeded;
    if (s == "Stopped") return JobState::Stopped;
    if (s == "Failed") return JobState::Failed;
    if (s == "Interrupted") return JobState::Interrupted;
    if (s == "Blocked") return JobState::Blocked;
    throw std::runtime_error("invalid job state");
}

bool finite_integer(double n) {
    return std::isfinite(n) && std::trunc(n) == n;
}

const JsonValue* field(const JsonValue& v, const char* key) {
    const JsonValue* p = v.find(key);
    if (!p) throw std::runtime_error(std::string("scheduler state: missing \"") + key + "\"");
    return p;
}

std::string raw_string_field(const JsonValue& v, const char* key,
                             bool required = true) {
    const JsonValue* p = v.find(key);
    if (!p) {
        if (!required) return {};
        throw std::runtime_error(std::string("scheduler state: missing \"") + key + "\"");
    }
    if (p->type != JsonValue::Type::String)
        throw std::runtime_error(std::string("scheduler state: \"") + key + "\" must be a string");
    return p->str;
}

std::string string_field(const JsonValue& v, const char* key, bool required = true) {
    std::string value = raw_string_field(v, key, required);
    if (!required && value.empty()) return value;
    if (value.find('\0') != std::string::npos)
        throw std::runtime_error(std::string("scheduler state: \"") + key + "\" contains NUL");
    return value;
}

bool bool_field(const JsonValue& v, const char* key, bool def = false) {
    const JsonValue* p = v.find(key);
    if (!p) return def;
    if (p->type != JsonValue::Type::Bool)
        throw std::runtime_error(std::string("scheduler state: \"") + key + "\" must be bool");
    return p->b;
}

double number_field(const JsonValue& v, const char* key, bool required = true, double def = 0) {
    const JsonValue* p = v.find(key);
    if (!p) {
        if (!required) return def;
        throw std::runtime_error(std::string("scheduler state: missing \"") + key + "\"");
    }
    if (p->type != JsonValue::Type::Number || !std::isfinite(p->num))
        throw std::runtime_error(std::string("scheduler state: \"") + key + "\" must be finite number");
    return p->num;
}

int int_field(const JsonValue& v, const char* key, int def = 0, bool required = true) {
    const double n = number_field(v, key, required, def);
    if (!finite_integer(n) || n < std::numeric_limits<int>::min() || n > std::numeric_limits<int>::max())
        throw std::runtime_error(std::string("scheduler state: \"") + key + " out of range");
    return (int)n;
}

std::vector<std::string> string_array(const JsonValue& v, const char* key, bool required = true) {
    const JsonValue* p = v.find(key);
    if (!p) {
        if (!required) return {};
        throw std::runtime_error(std::string("scheduler state: missing \"") + key + "\"");
    }
    if (p->type != JsonValue::Type::Array)
        throw std::runtime_error(std::string("scheduler state: \"") + key + "\" must be array");
    std::vector<std::string> out;
    out.reserve(p->arr.size());
    for (const JsonValue& item : p->arr) {
        if (item.type != JsonValue::Type::String || item.str.find('\0') != std::string::npos)
            throw std::runtime_error(std::string("scheduler state: invalid \"") + key + " item");
        out.push_back(item.str);
    }
    return out;
}

std::vector<std::string> path_array(const JsonValue& v, const char* key,
                                    bool required = true) {
    const JsonValue* p = v.find(key);
    if (!p) {
        if (!required) return {};
        throw std::runtime_error(std::string("scheduler state: missing \"") +
                                 key + "\"");
    }
    if (p->type != JsonValue::Type::Array)
        throw std::runtime_error(std::string("scheduler state: \"") + key +
                                 "\" must be array");
    std::vector<std::string> out;
    out.reserve(p->arr.size());
    for (const JsonValue& item : p->arr) {
        if (item.type != JsonValue::Type::String)
            throw std::runtime_error(std::string("scheduler state: invalid \"") +
                                     key + " item");
        out.push_back(item.str);
    }
    return out;
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
        if (json_parse(read_file(config)).type != JsonValue::Type::Object)
            throw std::runtime_error("not an object");
    } catch (...) {
        error = "training worker produced an invalid config.json";
        return false;
    }
    return true;
}

void set_arg_value(std::vector<std::string>& args, const char* key, const std::string& value) {
    for (size_t i = args.size(); i-- > 1;)
        if (args[i - 1] == key) { args[i] = value; return; }
    args.push_back(key); args.push_back(value);
}

void bind_prep_masks(std::vector<std::string>& args,
                     const std::vector<std::string>& prep_outputs) {
    const std::string mask_dir =
        prep_outputs.size() > 1 ? prep_outputs[1] : std::string();
    const bool have_masks = !mask_dir.empty();
    bool explicit_no_masks = false;
    bool flip_mask = false;
    std::vector<std::string> bound;
    bound.reserve(args.size() + (have_masks ? 2 : 1));
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--masks" || arg == "--mask-dir") {
            if (i + 1 >= args.size() || args[i + 1].empty() ||
                args[i + 1][0] == '-') {
                throw std::runtime_error(arg + ": missing value");
            }
            ++i;
            continue;
        }
        if (arg == "--no-masks") {
            explicit_no_masks = true;
            continue;
        }
        if (arg == "--flip-mask") {
            flip_mask = have_masks;
            continue;
        }
        bound.push_back(arg);
    }
    if (explicit_no_masks) {
        bound.push_back("--no-masks");
    } else if (have_masks) {
        bound.push_back("--masks");
        bound.push_back(mask_dir);
        if (flip_mask) bound.push_back("--flip-mask");
    } else {
        bound.push_back("--no-masks");
    }
    args = std::move(bound);
}

bool output_exists(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    return fs::exists(fs::u8path(path), ec) && !ec;
}

bool validate_phase_result(const Job& job, const Phase& phase, const app::worker::Result& result,
                          std::string& error) {
    const bool usable = result.outcome == "success" || result.outcome == "partial" ||
                        result.outcome == "nonmetric";
    if (!usable) { error = result.message.empty() ? "worker reported " + result.outcome : result.message; return false; }
    if (phase.phase == "train") {
        if (!training_output_published(job, error)) return false;
    }
    if (phase.phase == "prep") {
        if (result.outputs.empty() && phase.output.empty()) {
            error = "prep worker produced no validated output";
            return false;
        }
    }
    if (phase.phase == "sfm") {
        const std::string sparse = result.sparse_path.empty() ? phase.output : result.sparse_path;
        if (sparse.empty() || !output_exists(sparse)) {
            error = "SfM worker produced no validated sparse model";
            return false;
        }
    }
    if (phase.phase == "geometry" && result.outputs.empty()) {
        error = "geometry worker produced no validated output";
        return false;
    }
    for (const std::string& output : result.outputs) {
        if (!output_exists(output)) { error = "worker output does not exist: " + output; return false; }
    }
    return true;
}

std::string phase_json(const Phase& p) {
    std::ostringstream out;
    out << "{\"phase\":" << quote(p.phase)
        << ",\"optional\":" << (p.optional ? "true" : "false")
        << ",\"planned_device\":" << quote(p.planned_device)
        << ",\"planned_device_name\":" << quote(p.planned_device_name)
        << ",\"actual_device\":" << quote(p.actual_device)
        << ",\"actual_device_name\":" << quote(p.actual_device_name)
        << ",\"output\":" << quote(p.output) << ",\"outputs\":[";
    for (size_t i = 0; i < p.outputs.size(); ++i) out << (i ? "," : "") << quote(p.outputs[i]);
    out << "],\"args\":[";
    for (size_t i = 0; i < p.args.size(); ++i) out << (i ? "," : "") << quote(p.args[i]);
    out << "],\"payload\":" << quote(p.payload)
        << ",\"outcome\":" << quote(p.outcome)
        << ",\"completed\":" << (p.completed ? "true" : "false")
        << ",\"exit_code\":" << p.exit_code
        << ",\"error\":" << quote(p.error) << "}";
    return out.str();
}

Phase parse_phase(const JsonValue& v) {
    if (v.type != JsonValue::Type::Object) throw std::runtime_error("scheduler state: invalid phase");
    Phase p;
    p.phase = string_field(v, "phase");
    p.optional = bool_field(v, "optional");
    p.planned_device = string_field(v, "planned_device");
    p.planned_device_name = string_field(v, "planned_device_name");
    p.actual_device = string_field(v, "actual_device");
    p.actual_device_name = string_field(v, "actual_device_name");
    p.output = raw_string_field(v, "output");
    p.outputs = path_array(v, "outputs");
    p.args = string_array(v, "args");
    p.payload = string_field(v, "payload");
    p.outcome = string_field(v, "outcome");
    p.completed = bool_field(v, "completed");
    p.exit_code = int_field(v, "exit_code");
    p.error = string_field(v, "error");
    return p;
}

void write_request_file(const app::worker::Request& r, const fs::path& path) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write request file");
    f << "{\n"
      << "  \"schema_version\": 2,\n"
      << "  \"job_id\": " << quote(r.job_id) << ",\n"
      << "  \"attempt_id\": " << quote(r.attempt_id) << ",\n"
      << "  \"phase\": " << quote(r.phase) << ",\n"
      << "  \"device\": " << quote(r.device) << ",\n"
      << "  \"device_name\": " << quote(r.device_name) << ",\n"
      << "  \"work_dir\": " << quote(r.work_dir) << ",\n"
      << "  \"workspace\": " << quote(r.workspace) << ",\n"
      << "  \"result_path\": " << quote(r.result_path) << ",\n"
      << "  \"args\": [";
    for (size_t i = 0; i < r.args.size(); ++i) f << (i ? ", " : "") << quote(r.args[i]);
    f << "],\n  \"payload\": " << quote(r.payload) << "\n}\n";
    f.flush();
    if (!f) throw std::runtime_error("cannot flush request file");
}

bool owns_claims(JobState state) {
    return state == JobState::Queued || state == JobState::Starting ||
           state == JobState::Running || state == JobState::Stopping;
}

bool normalize_path(const std::string& value, const fs::path& base,
                    std::string& out, std::string& error,
                    bool require_directory = false) {
    if (value.empty() || value.find('\0') != std::string::npos) {
        error = "path is empty or contains NUL";
        return false;
    }
    try {
        std::error_code ec;
        fs::path path = fs::u8path(value);
        if (path.is_relative()) path = base / path;
        path = fs::absolute(path, ec).lexically_normal();
        if (ec) {
            error = "cannot make path absolute: " + ec.message();
            return false;
        }
        const fs::path canonical = fs::weakly_canonical(path, ec);
        if (!ec) path = canonical;
        if (require_directory && !fs::is_directory(path, ec)) {
            error = "work directory is unavailable: " + path.u8string();
            return false;
        }
        out = path.u8string();
        return true;
    } catch (const std::exception& e) {
        error = std::string("invalid path: ") + e.what();
        return false;
    }
}

}  // namespace

const char* to_string(JobState s) {
    switch (s) {
        case JobState::Queued: return "Queued";
        case JobState::Starting: return "Starting";
        case JobState::Running: return "Running";
        case JobState::Stopping: return "Stopping";
        case JobState::Succeeded: return "Succeeded";
        case JobState::Stopped: return "Stopped";
        case JobState::Failed: return "Failed";
        case JobState::Interrupted: return "Interrupted";
        case JobState::Blocked: return "Blocked";
    }
    return "Unknown";
}

std::string canonical_claim_path(const std::string& value) {
    if (value.empty()) return {};
    std::string out, error;
    std::error_code ec;
    const fs::path base = fs::current_path(ec);
    if (ec || !normalize_path(value, base, out, error)) return {};
#ifdef _WIN32
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
#endif
    return out;
}

bool path_claims_conflict(const PathClaim& aa, const PathClaim& bb) {
    if (aa.path.empty() || bb.path.empty()) return false;
    if (!aa.write && !bb.write) return false;
    const fs::path a = fs::u8path(canonical_claim_path(aa.path));
    const fs::path b = fs::u8path(canonical_claim_path(bb.path));
    auto ancestor = [](const fs::path& parent, const fs::path& child) {
        auto i = parent.begin(), j = child.begin();
        for (; i != parent.end() && j != child.end(); ++i, ++j)
            if (*i != *j) return false;
        return i == parent.end();
    };
    return ancestor(a, b) || ancestor(b, a);
}

JobScheduler::JobScheduler(std::string config_dir, std::string exe_path)
    : _config_dir(std::move(config_dir)), _exe_path(std::move(exe_path)) {
    _state_path = (fs::u8path(_config_dir) / "job-state.json").u8string();
    _lock_path = (fs::u8path(_config_dir) / "job-state.lock").u8string();
    std::error_code ec;
    fs::create_directories(fs::u8path(_config_dir), ec);
    acquire_state_lock();
    _dispatcher = std::thread([this] { dispatcher_main(); });
}

JobScheduler::~JobScheduler() {
    shutdown();
    release_state_lock();
}

bool JobScheduler::acquire_state_lock() {
#ifdef _WIN32
    HANDLE h = CreateFileW(fs::u8path(_lock_path).wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { _state_error = "scheduler state is owned by another application"; _paused.store(true); return false; }
    _state_lock = h;
    return true;
#else
    _state_lock = ::open(_lock_path.c_str(), O_CREAT | O_RDWR, 0666);
    if (_state_lock < 0 || flock(_state_lock, LOCK_EX | LOCK_NB) != 0) {
        if (_state_lock >= 0) { ::close(_state_lock); _state_lock = -1; }
        _state_error = "scheduler state is owned by another application"; _paused.store(true); return false;
    }
    return true;
#endif
}

void JobScheduler::release_state_lock() {
#ifdef _WIN32
    if (_state_lock) { CloseHandle(static_cast<HANDLE>(_state_lock)); _state_lock = nullptr; }
#else
    if (_state_lock >= 0) { flock(_state_lock, LOCK_UN); ::close(_state_lock); _state_lock = -1; }
#endif
}

void JobScheduler::dispatcher_main() {
    std::unique_lock<std::mutex> lk(_mu);
    for (;;) {
        _cv.wait(lk, [&] {
            if (_shutdown.load()) return true;
            if (has_eligible_job_locked()) return true;
            for (const auto& [_, a] : _active) if (a->finished.load()) return true;
            return false;
        });
        if (_shutdown.load()) return;
        lk.unlock();
        reap_finished();
        dispatch_one();
        lk.lock();
    }
}

std::string JobScheduler::new_job_id() { return "job-" + format_hex(nonce()); }
std::string JobScheduler::new_attempt_id() { return "att-" + format_hex(nonce()); }

std::string JobScheduler::now_iso() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

void JobScheduler::set_event_callback(std::function<void(const Event&)> cb) {
    std::lock_guard<std::mutex> lk(_mu); _on_event = std::move(cb);
}

void JobScheduler::drain_events() {
    std::deque<Event> events; std::function<void(const Event&)> cb;
    { std::lock_guard<std::mutex> lk(_mu); cb = _on_event; events.swap(_events); }
    if (cb) for (const Event& e : events) cb(e);
}

void JobScheduler::queue_event_locked(const Job& j, const std::string& line, const std::string& err) {
    Event e; e.job_id = j.job_id; e.state = j.state; e.line = line; e.error = err;
    _events.push_back(std::move(e));
    while (_events.size() > 4096) _events.pop_front();
}

void JobScheduler::transition_locked(Job& j, JobState s, std::string err) {
    j.state = s; j.error = std::move(err); queue_event_locked(j);
}

bool JobScheduler::valid_phase_order(const std::vector<Phase>& phases, std::string& error) {
    if (phases.empty()) { error = "workflow has no phases"; return false; }
    int previous = -1;
    for (const Phase& p : phases) {
        int rank = p.phase == "prep" ? 0 : p.phase == "sfm" ? 1 :
                   p.phase == "geometry" ? 2 : p.phase == "train" ? 3 :
                   p.phase == "publish" ? 4 : -1;
        if (rank < 0) { error = "unsupported workflow phase: " + p.phase; return false; }
        if (rank <= previous) { error = "workflow phases are not ordered"; return false; }
        previous = rank;
        if (p.phase == "publish" && p.optional) { error = "publish phase cannot be optional"; return false; }
    }
    return true;
}

static bool migrate_legacy_publish_order(std::vector<Phase>& phases) {
    const auto publish = std::find_if(phases.begin(), phases.end(),
        [](const Phase& phase) { return phase.phase == "publish"; });
    const auto train = std::find_if(phases.begin(), phases.end(),
        [](const Phase& phase) { return phase.phase == "train"; });
    if (publish == phases.end() || train == phases.end() || publish > train)
        return false;
    Phase moved = std::move(*publish);
    phases.erase(publish);
    phases.push_back(std::move(moved));
    int previous = -1;
    for (const Phase& phase : phases) {
        const int rank = phase.phase == "prep" ? 0 :
                         phase.phase == "sfm" ? 1 :
                         phase.phase == "geometry" ? 2 :
                         phase.phase == "train" ? 3 :
                         phase.phase == "publish" ? 4 : -1;
        if (rank <= previous || (phase.phase == "publish" && phase.optional))
            return false;
        previous = rank;
    }
    return true;
}

bool JobScheduler::claim_paths_locked(const Job& job, std::string& error) const {
    if (job.path_claims.empty()) {
        error = "workflow has no path claims";
        return false;
    }
    bool has_write = false;
    for (size_t i = 0; i < job.path_claims.size(); ++i) {
        const PathClaim& claim = job.path_claims[i];
        if (claim.path.empty()) { error = "empty path claim"; return false; }
        has_write = has_write || claim.write;
        for (size_t k = i + 1; k < job.path_claims.size(); ++k) {
            if (claim.write && job.path_claims[k].write &&
                path_claims_conflict(claim, job.path_claims[k])) {
                error = "workflow write claims overlap";
                return false;
            }
        }
        for (const auto& [id, other] : _jobs) {
            if (id == job.job_id || !owns_claims(other->state)) continue;
            for (const PathClaim& theirs : other->path_claims) {
                if (path_claims_conflict(claim, theirs)) {
                    error = "path claim overlaps job " + id;
                    return false;
                }
            }
        }
    }
    if (!has_write) { error = "workflow has no write claim"; return false; }
    return true;
}

bool JobScheduler::acquire_claim_leases_locked(const Job& job,
                                               std::string& error) {
    if (_claim_leases.count(job.job_id)) return true;
    std::vector<std::shared_ptr<OutputLease>> leases;
    for (const PathClaim& claim : job.path_claims) {
        if (!claim.write) continue;
        auto lease = std::make_shared<OutputLease>();
        if (!lease->acquire(fs::u8path(claim.path), error)) return false;
        leases.push_back(std::move(lease));
    }
    if (leases.empty()) {
        error = "workflow has no write claim";
        return false;
    }
    _claim_leases[job.job_id] = std::move(leases);
    return true;
}

void JobScheduler::release_claim_leases_locked(const std::string& job_id) {
    _claim_leases.erase(job_id);
}

std::string JobScheduler::submit(const SubmitOpts& o) {
    if (o.device.empty() || o.device == "auto") return {};
    WorkflowSubmitOpts w;
    w.work_dir = o.work_dir;
    w.path_claims = o.path_claims;
    if (!o.output_dir.empty()) w.path_claims.push_back({o.output_dir, true});
    for (const char* key : {"--data", "--resume"}) {
        const std::string source = arg_value(o.args, key);
        if (!source.empty()) {
            w.source_paths.push_back(source);
            w.path_claims.push_back({source, false});
        }
    }
    Phase p;
    p.phase = o.phase; p.planned_device = o.device; p.planned_device_name = o.device_name;
    p.output = o.output_dir; p.args = o.args; p.payload = o.payload;
    w.phases.push_back(std::move(p));
    w.add_scheduler_publish = false;
    return submit(w);
}

std::string JobScheduler::submit(const WorkflowSubmitOpts& o) {
    if (o.phases.empty()) return {};
    std::vector<Phase> phases = o.phases;
    bool publish_seen = false;
    for (const Phase& phase : phases)
        publish_seen = publish_seen || phase.phase == "publish";
    if (o.add_scheduler_publish && !publish_seen) {
        Phase publish;
        publish.phase = "publish";
        phases.push_back(std::move(publish));
    }
    std::string phase_error;
    if (!valid_phase_order(phases, phase_error)) return {};
    auto j = std::make_shared<Job>();
    j->job_id = new_job_id();
    std::error_code cwd_ec;
    const fs::path cwd = fs::current_path(cwd_ec);
    if (cwd_ec) return {};
    std::string path_error;
    if (!normalize_path(o.work_dir.empty() ? _config_dir : o.work_dir, cwd,
                        j->work_dir, path_error, true))
        return {};
    if (o.workspace.empty()) j->workspace = j->work_dir;
    else if (!normalize_path(o.workspace, fs::u8path(j->work_dir),
                             j->workspace, path_error))
        return {};
    j->source_paths = o.source_paths;
    if (!o.source.empty()) j->source_paths.push_back(o.source);
    for (std::string& source : j->source_paths) {
        std::string normalized;
        if (!normalize_path(source, fs::u8path(j->work_dir), normalized,
                            path_error))
            return {};
        source = std::move(normalized);
    }
    j->options_payload = o.options_payload;
    j->phases = std::move(phases);
    j->path_claims = o.path_claims;
    for (Phase& phase : j->phases) {
        if (phase.phase == "train") {
            set_arg_value(phase.args, "--output-dir-name", j->job_id);
            if (phase.output.empty()) {
                const std::string prefix = arg_value(phase.args, "--output-dir-prefix");
                const std::string name = arg_value(phase.args, "--output-dir-name");
                if (!prefix.empty() && !name.empty()) {
                    fs::path output = fs::u8path(prefix) / fs::u8path(name);
                    if (output.is_relative()) output = fs::u8path(j->work_dir) / output;
                    phase.output = canonical_claim_path(output.u8string());
                }
            }
        }
        if (!phase.output.empty()) {
            std::string normalized;
            if (!normalize_path(phase.output, fs::u8path(j->work_dir),
                                normalized, path_error))
                return {};
            phase.output = std::move(normalized);
        }
        if (phase.phase == "train" && !phase.output.empty()) {
            bool claimed =
                !o.workspace.empty() &&
                path_claims_conflict({j->workspace, true},
                                     {phase.output, true});
            for (const PathClaim& claim : j->path_claims)
                if (claim.write &&
                    path_claims_conflict(claim, {phase.output, true}))
                    claimed = true;
            if (!claimed) j->path_claims.push_back({phase.output, true});
        }
    }
    j->created_at = now_iso();
    j->state = JobState::Queued;
    j->current_phase = 0;
    j->completed_prefix = 0;
    j->phase = j->phases.front().phase;
    j->device = j->phases.front().planned_device;
    j->device_name = j->phases.front().planned_device_name;
    j->args = j->phases.front().args;
    j->output_dir = j->phases.front().output;
    if (j->phase == "train" && j->output_dir.empty()) {
        if (const fs::path output = training_output_dir(*j); !output.empty()) {
            fs::path resolved = output;
            if (resolved.is_relative()) resolved = fs::u8path(j->work_dir) / resolved;
            j->output_dir = canonical_claim_path(resolved.u8string());
        }
    }
    j->phases.front().output = j->output_dir;
    if (!o.workspace.empty()) {
        const PathClaim workspace_claim{j->workspace, true};
        bool present = false;
        for (const PathClaim& claim : j->path_claims)
            present = present || (claim.write &&
                canonical_claim_path(claim.path) ==
                    canonical_claim_path(workspace_claim.path));
        if (!present) j->path_claims.push_back(workspace_claim);
    }
    for (const std::string& source : j->source_paths) {
        PathClaim source_claim{source, false};
        bool present = false;
        for (const PathClaim& claim : j->path_claims)
            present = present || (!claim.write &&
                canonical_claim_path(claim.path) ==
                    canonical_claim_path(source_claim.path));
        if (!present) j->path_claims.push_back(std::move(source_claim));
    }
    for (PathClaim& claim : j->path_claims) {
        std::string normalized;
        if (!normalize_path(claim.path, fs::u8path(j->work_dir), normalized,
                            path_error))
            return {};
        claim.path = std::move(normalized);
    }
    const fs::path dir = fs::u8path(_config_dir) / "jobs" / j->job_id;
    std::lock_guard<std::mutex> lk(_mu);
    if (!_state_error.empty()) return {};
    std::error_code ec;
    fs::create_directories(dir.parent_path(), ec);
    if (ec) return {};
    const bool created = fs::create_directory(dir, ec);
    if (ec || !created) return {};
    j->run_dir = dir.u8string();
    j->path_claims.push_back({j->run_dir, true});
    std::string claim_error;
    bool runnable = claim_paths_locked(*j, claim_error);
    if (runnable) runnable = acquire_claim_leases_locked(*j, claim_error);
    if (!runnable) {
        j->state = JobState::Blocked;
        j->error = claim_error.empty() ? "workflow paths are unavailable"
                                       : claim_error;
        j->phases.front().outcome = "failed";
        j->phases.front().error = j->error;
    }
    const uint64_t previous_order = _next_order;
    j->order = _next_order++;
    _jobs[j->job_id] = j;
    if (runnable) _queue.push_back(j->job_id);
    queue_event_locked(*j);
    if (!save_locked()) {
        _jobs.erase(j->job_id);
        if (runnable) _queue.pop_back();
        _next_order = previous_order;
        release_claim_leases_locked(j->job_id);
        if (created) fs::remove_all(dir, ec);
        return {};
    }
    _cv.notify_all();
    return j->job_id;
}

std::vector<Job> JobScheduler::list() const {
    std::lock_guard<std::mutex> lk(_mu);
    std::vector<Job> out; out.reserve(_jobs.size());
    for (const auto& [_, j] : _jobs) out.push_back(*j);
    std::sort(out.begin(), out.end(), [](const Job& a, const Job& b) { return a.order < b.order; });
    return out;
}

std::string JobScheduler::state_error() const { std::lock_guard<std::mutex> lk(_mu); return _state_error; }

void JobScheduler::stop_and_save(const std::string& job_id) {
    std::lock_guard<std::mutex> lk(_mu);
    auto it = _active.find(job_id); auto jit = _jobs.find(job_id);
    if (it == _active.end() || jit == _jobs.end()) return;
    if (jit->second->state == JobState::Starting || jit->second->state == JobState::Running) {
        jit->second->state = JobState::Stopping; queue_event_locked(*jit->second); save_locked();
    }
    it->second->stop.store(true);
}

void JobScheduler::force_stop(const std::string& job_id) {
    std::lock_guard<std::mutex> lk(_mu);
    auto it = _active.find(job_id); auto jit = _jobs.find(job_id);
    if (it == _active.end() || jit == _jobs.end()) return;
    if (jit->second->state == JobState::Starting || jit->second->state == JobState::Running) {
        jit->second->state = JobState::Stopping; queue_event_locked(*jit->second); save_locked();
    }
    it->second->cancel.store(true);
}

void JobScheduler::cancel(const std::string& job_id) {
    std::lock_guard<std::mutex> lk(_mu);
    auto it = _jobs.find(job_id); if (it == _jobs.end() || it->second->state != JobState::Queued) return;
    _queue.erase(std::remove(_queue.begin(), _queue.end(), job_id), _queue.end());
    transition_locked(*it->second, JobState::Stopped, "cancelled");
    it->second->pending_resume = false;
    release_claim_leases_locked(job_id);
    save_locked();
}

void JobScheduler::set_device(const std::string& job_id, const std::string& device, const std::string& device_name) {
    if (device.empty() || device == "auto") return;
    std::lock_guard<std::mutex> lk(_mu);
    auto it = _jobs.find(job_id); if (it == _jobs.end() || it->second->state != JobState::Queued) return;
    Job& j = *it->second;
    j.device = device;
    if (!device_name.empty()) j.device_name = device_name;
    for (size_t i = j.current_phase; i < j.phases.size(); ++i) {
        j.phases[i].planned_device = device;
        if (!device_name.empty()) j.phases[i].planned_device_name = device_name;
    }
    queue_event_locked(j); save_locked(); _cv.notify_all();
}

void JobScheduler::set_device_validator(DeviceValidator validator) { { std::lock_guard<std::mutex> lk(_mu); _device_validator = std::move(validator); } _cv.notify_all(); }

bool JobScheduler::try_reserve_foreground_device(const std::string& device) {
    if (device.empty()) return false;
    std::lock_guard<std::mutex> lk(_mu);
    if (_leases.count(device) || (!_foreground_device.empty() && _foreground_device != device)) return false;
    _foreground_device = device; return true;
}

void JobScheduler::set_foreground_device(const std::string& device) {
    { std::lock_guard<std::mutex> lk(_mu); if (_foreground_device == device) return; if (!device.empty() && _leases.count(device)) return; _foreground_device = device; }
    _cv.notify_all();
}

void JobScheduler::retry(const std::string& job_id) {
    {
        std::lock_guard<std::mutex> lk(_mu);
        auto jit = _jobs.find(job_id); if (jit == _jobs.end()) return;
        Job& j = *jit->second;
        if (j.state != JobState::Interrupted && j.state != JobState::Failed && j.state != JobState::Stopped && j.state != JobState::Blocked) return;
        std::string claim_error;
        if (!claim_paths_locked(j, claim_error) ||
            !acquire_claim_leases_locked(j, claim_error)) {
            transition_locked(j, JobState::Blocked, claim_error);
            save_locked();
            return;
        }
        j.attempt_id.clear(); j.error.clear(); j.pending_resume = false;
        if (j.current_phase < j.phases.size()) {

            Phase& p = j.phases[j.current_phase]; p.completed = false; p.outcome = "pending"; p.error.clear(); p.exit_code = -1;
            j.phase = p.phase; j.device = p.planned_device; j.device_name = p.planned_device_name; j.args = p.args; j.output_dir = p.output;
            if (p.phase == "train") {
                const fs::path output = training_output_dir(j);
                try { const ckpt::ResolvedCheckpoint resolved = ckpt::resolve_checkpoint(output); ckpt::check_resumable(resolved.ckpt_dir); set_arg_value(j.args, "--resume", output.u8string()); p.args = j.args; } catch (...) {}
            }
        }
        j.state = JobState::Queued; _queue.push_back(job_id); queue_event_locked(j); save_locked();
    }
    _cv.notify_all();
}

void JobScheduler::remove(const std::string& job_id) {
    std::lock_guard<std::mutex> lk(_mu);
    auto jit = _jobs.find(job_id); if (jit == _jobs.end()) return;
    const Job& j = *jit->second;
    if (j.state == JobState::Running || j.state == JobState::Starting || j.state == JobState::Stopping) return;
    release_claim_leases_locked(job_id);
    _jobs.erase(jit); _queue.erase(std::remove(_queue.begin(), _queue.end(), job_id), _queue.end()); save_locked();
}

void JobScheduler::pause_dispatch(bool on) {
    {
        std::lock_guard<std::mutex> lk(_mu);
        _paused.store(on);
    }
    if (!on) _cv.notify_all();
}
bool JobScheduler::dispatch_paused() const { return _paused.load(); }

bool JobScheduler::save() { std::lock_guard<std::mutex> lk(_mu); return save_locked(); }

bool JobScheduler::save_locked() {
    if (!_state_error.empty()) return false;
    auto fail = [this](const char* reason) { _state_error = reason; _paused.store(true); return false; };
    const fs::path tmp = fs::u8path(_state_path + ".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return fail("cannot write scheduler state");
        f << "{\n  \"schema_version\": 2,\n  \"jobs\": [\n";
        bool first_job = true;
        for (const auto& [_, jp] : _jobs) {
            const Job& j = *jp;
            if (!first_job) f << ",\n"; first_job = false;
            f << "    {\"order\":" << j.order << ",\"job_id\":" << quote(j.job_id)
              << ",\"phase\":" << quote(j.phase) << ",\"device\":" << quote(j.device)
              << ",\"device_name\":" << quote(j.device_name) << ",\"work_dir\":" << quote(j.work_dir)
              << ",\"run_dir\":" << quote(j.run_dir) << ",\"output_dir\":" << quote(j.output_dir)
              << ",\"workspace\":" << quote(j.workspace) << ",\"source_paths\":[";
            for (size_t i = 0; i < j.source_paths.size(); ++i) f << (i ? "," : "") << quote(j.source_paths[i]);
            f << "],\"options_payload\":" << quote(j.options_payload)
              << ",\"created_at\":" << quote(j.created_at) << ",\"state\":" << quote(to_string(j.state))
              << ",\"attempt_id\":" << quote(j.attempt_id) << ",\"error\":" << quote(j.error)
              << ",\"pending_resume\":" << (j.pending_resume ? "true" : "false")
              << ",\"last_exit_code\":" << j.last_exit_code << ",\"completed_prefix\":" << j.completed_prefix
              << ",\"current_phase\":" << j.current_phase << ",\"args\":[";
            for (size_t i = 0; i < j.args.size(); ++i) f << (i ? "," : "") << quote(j.args[i]);
            f << "],\"path_claims\":[";
            for (size_t i = 0; i < j.path_claims.size(); ++i) {
                if (i) f << ',';
                f << "{\"path\":" << quote(j.path_claims[i].path) << ",\"write\":" << (j.path_claims[i].write ? "true" : "false") << '}';
            }
            f << "],\"phases\":[";
            for (size_t i = 0; i < j.phases.size(); ++i) f << (i ? "," : "") << phase_json(j.phases[i]);
            f << "]}";
        }
        f << "\n  ]\n}\n"; f.flush();
        if (!f) { std::error_code ec; fs::remove(tmp, ec); return fail("cannot flush scheduler state"); }
    }
    const fs::path target = fs::u8path(_state_path); std::error_code ec;
    fs::rename(tmp, target, ec);
    if (!ec) return true;
    const fs::path backup = fs::u8path(_state_path + ".bak"); std::error_code backup_ec;
    fs::remove(backup, backup_ec); backup_ec.clear(); fs::rename(target, backup, backup_ec);
    if (backup_ec) { fs::remove(tmp, ec); return fail("cannot replace scheduler state"); }
    std::error_code replace_ec; fs::rename(tmp, target, replace_ec);
    if (replace_ec) { std::error_code restore_ec; fs::rename(backup, target, restore_ec); fs::remove(tmp, ec); return fail("cannot replace scheduler state"); }
    fs::remove(backup, backup_ec); return true;
}

void JobScheduler::load() {
    {
        std::lock_guard<std::mutex> lk(_mu);
        if (!_state_error.empty()) return;
    }
    const fs::path state_path = fs::u8path(_state_path), backup_path = fs::u8path(_state_path + ".bak");
    JsonValue root; std::string load_error; bool found_state = false, found_file = false; int loaded_schema = 0;
    auto read_state = [&](const fs::path& path, std::string& error) {
        const std::string text = read_file(path);
        if (text.empty()) { error = "scheduler state is empty"; return false; }
        try { root = json_parse(text); } catch (...) { error = "cannot parse scheduler state"; return false; }
        if (root.type != JsonValue::Type::Object) { error = "scheduler state root is not an object"; return false; }
        try {
        const JsonValue* schema = root.find("schema_version");
        if (!schema || schema->type != JsonValue::Type::Number || !finite_integer(schema->num) || (schema->num != 1 && schema->num != 2)) { error = "unsupported scheduler state schema"; return false; }
        loaded_schema = (int)schema->num;
        const JsonValue* jobs = root.find("jobs");
        if (!jobs || jobs->type != JsonValue::Type::Array) { error = "scheduler state has no jobs array"; return false; }
        std::unordered_set<std::string> ids; std::unordered_set<uint64_t> orders;
        for (const JsonValue& el : jobs->arr) {
            if (el.type != JsonValue::Type::Object) { error = "scheduler state has an invalid job"; return false; }
            const double order = number_field(el, "order");
            const std::string id = string_field(el, "job_id");
            if (!finite_integer(order) || order < 0 || order >= std::ldexp(1.0, 64) || !ids.insert(id).second || !orders.insert((uint64_t)order).second) { error = "scheduler state has an invalid job"; return false; }
            const std::string phase = string_field(el, "phase");
            if (phase != "prep" && phase != "sfm" && phase != "geometry" && phase != "publish" && phase != "train") { error = "scheduler state has an invalid job"; return false; }
            if (string_field(el, "device").empty() || raw_string_field(el, "run_dir").empty() || string_field(el, "created_at").empty()) { error = "scheduler state has an invalid job"; return false; }
            const std::string state = string_field(el, "state"); try { state_from_string(state); } catch (...) { error = "scheduler state has an invalid job"; return false; }
            if (!finite_integer(number_field(el, "last_exit_code"))) { error = "scheduler state has an invalid job"; return false; }
            (void)string_array(el, "args");
            if (loaded_schema == 2) {
                const JsonValue* phases = el.find("phases");
                if (!phases || phases->type != JsonValue::Type::Array || phases->arr.empty()) { error = "scheduler state has no phases"; return false; }
                const double completed = number_field(el, "completed_prefix");
                const double current = number_field(el, "current_phase");
                if (!finite_integer(completed) || completed < 0 || completed > phases->arr.size() ||
                    !finite_integer(current) || current < 0 || current > phases->arr.size() ||
                    completed > current) {
                    error = "scheduler state has invalid phase cursor"; return false;
                }
                std::vector<Phase> persisted_phases;
                persisted_phases.reserve(phases->arr.size());
                for (const JsonValue& p : phases->arr)
                    persisted_phases.push_back(parse_phase(p));
                std::string order_error;
                if (!valid_phase_order(persisted_phases, order_error) &&
                    !migrate_legacy_publish_order(persisted_phases)) {
                    error = "scheduler state has invalid phase order";
                    return false;
                }
                (void)path_array(el, "source_paths", false);
                const JsonValue* claims = el.find("path_claims");
                if (!claims || claims->type != JsonValue::Type::Array) { error = "scheduler state has invalid path claims"; return false; }
                for (const JsonValue& c : claims->arr) { if (c.type != JsonValue::Type::Object || raw_string_field(c, "path").empty() || c.find("write") == nullptr) { error = "scheduler state has invalid path claim"; return false; } (void)bool_field(c, "write"); }
            }
        }
        return true;
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
    };
    for (const fs::path& candidate : {state_path, backup_path}) {
        std::error_code ec; if (!fs::exists(candidate, ec) || ec) continue; found_file = true;
        if (read_state(candidate, load_error)) { found_state = true; break; }
    }
    if (!found_file) return;
    if (!found_state) { std::lock_guard<std::mutex> lk(_mu); _state_error = load_error.empty() ? "cannot load scheduler state" : load_error; _paused.store(true); return; }
    const JsonValue* jobs = root.find("jobs");
    std::lock_guard<std::mutex> lk(_mu);
    bool load_changed = loaded_schema == 1;
    for (const JsonValue& el : jobs->arr) {
        auto j = std::make_shared<Job>();
        j->order = (uint64_t)number_field(el, "order"); _next_order = std::max(_next_order, j->order + 1);
        j->job_id = string_field(el, "job_id"); j->phase = string_field(el, "phase"); j->device = string_field(el, "device");
        j->device_name = string_field(el, "device_name", false); j->work_dir = raw_string_field(el, "work_dir"); j->run_dir = raw_string_field(el, "run_dir");
        j->output_dir = raw_string_field(el, "output_dir"); j->workspace = raw_string_field(el, "workspace", false); j->source_paths = path_array(el, "source_paths", false);
        j->options_payload = string_field(el, "options_payload", false); j->created_at = string_field(el, "created_at"); j->attempt_id = string_field(el, "attempt_id"); j->error = string_field(el, "error");
        j->args = string_array(el, "args");
        j->last_exit_code = int_field(el, "last_exit_code");
        j->pending_resume = loaded_schema == 2 ? bool_field(el, "pending_resume") : false;
        const JobState loaded_job_state = state_from_string(string_field(el, "state"));
        if (loaded_schema == 1) {
            Phase p; p.phase = j->phase; p.planned_device = j->device; p.planned_device_name = j->device_name; p.actual_device = j->device; p.actual_device_name = j->device_name; p.output = j->output_dir; p.args = j->args;
            p.outcome = loaded_job_state == JobState::Succeeded ? "success" : "pending"; p.completed = loaded_job_state == JobState::Succeeded; p.exit_code = j->last_exit_code;
            j->phases.push_back(std::move(p)); j->completed_prefix = j->phases.front().completed ? 1 : 0; j->current_phase = j->completed_prefix;
            if (!j->output_dir.empty()) j->path_claims.push_back({canonical_claim_path(j->output_dir), true});
            if (j->current_phase >= j->phases.size()) j->current_phase = j->phases.size() - 1;
        } else {
            const JsonValue& phases = *field(el, "phases");
            for (const JsonValue& p : phases.arr)
                j->phases.push_back(parse_phase(p));
            j->completed_prefix = (size_t)number_field(el, "completed_prefix");
            j->current_phase = (size_t)number_field(el, "current_phase");
            if (migrate_legacy_publish_order(j->phases)) {
                Phase& publish = j->phases.back();
                const Phase& train = j->phases[j->phases.size() - 2];
                if (train.completed) {
                    publish.output = train.output;
                    publish.outputs = train.outputs;
                    if (publish.outputs.empty() && !publish.output.empty())
                        publish.outputs.push_back(publish.output);
                } else {
                    publish.completed = false;
                    publish.outcome = "pending";
                    publish.output.clear();
                    publish.outputs.clear();
                    publish.error.clear();
                    publish.exit_code = -1;
                }
                j->completed_prefix = 0;
                while (j->completed_prefix < j->phases.size() &&
                       j->phases[j->completed_prefix].completed)
                    ++j->completed_prefix;
                j->current_phase = j->completed_prefix;
                load_changed = true;
            }
            for (const JsonValue& c : field(el, "path_claims")->arr)
                j->path_claims.push_back(
                    {raw_string_field(c, "path"), bool_field(c, "write")});
        }
        JobState s = loaded_job_state;
        if (s == JobState::Starting || s == JobState::Running || s == JobState::Stopping) {
            s = JobState::Interrupted;
            j->pending_resume = true;
            load_changed = true;
        }
        std::string path_error, normalized;
        bool paths_ok = normalize_path(j->work_dir, fs::u8path(_config_dir),
                                       normalized, path_error, true);
        if (paths_ok) j->work_dir = normalized;
        if (paths_ok) {
            paths_ok = normalize_path(j->run_dir, fs::u8path(_config_dir),
                                      normalized, path_error);
            if (paths_ok) j->run_dir = normalized;
        }
        if (paths_ok && !j->workspace.empty()) {
            paths_ok = normalize_path(j->workspace, fs::u8path(j->work_dir),
                                      normalized, path_error);
            if (paths_ok) j->workspace = normalized;
        }
        if (paths_ok && !j->output_dir.empty()) {
            paths_ok = normalize_path(j->output_dir, fs::u8path(j->work_dir),
                                      normalized, path_error);
            if (paths_ok) j->output_dir = normalized;
        }
        for (std::string& source : j->source_paths) {
            if (!paths_ok) break;
            paths_ok = normalize_path(source, fs::u8path(j->work_dir),
                                      normalized, path_error);
            if (paths_ok) source = normalized;
        }
        for (PathClaim& claim : j->path_claims) {
            if (!paths_ok) break;
            paths_ok = normalize_path(claim.path, fs::u8path(j->work_dir),
                                      normalized, path_error);
            if (paths_ok) claim.path = normalized;
        }
        for (Phase& phase : j->phases) {
            if (!paths_ok) break;
            if (!phase.output.empty()) {
                paths_ok = normalize_path(phase.output, fs::u8path(j->work_dir),
                                          normalized, path_error);
                if (paths_ok) phase.output = normalized;
            }
            for (std::string& output : phase.outputs) {
                if (!paths_ok) break;
                paths_ok = normalize_path(output, fs::u8path(j->work_dir),
                                          normalized, path_error);
                if (paths_ok) output = normalized;
            }
        }
        if (!paths_ok) {
            load_changed = true;
            s = JobState::Failed;
            j->pending_resume = false;
            j->error = "invalid persisted path: " + path_error;
            if (j->current_phase < j->phases.size()) {
                j->phases[j->current_phase].outcome = "failed";
                j->phases[j->current_phase].error = j->error;
            }
        }
        j->state = s;
        if (j->current_phase < j->phases.size()) {
            j->phase = j->phases[j->current_phase].phase;
            j->device = j->phases[j->current_phase].planned_device;
            j->device_name = j->phases[j->current_phase].planned_device_name;
            j->args = j->phases[j->current_phase].args;
            j->output_dir = j->phases[j->current_phase].output;
        }
        bool run_claim = false;
        for (const PathClaim& claim : j->path_claims)
            run_claim = run_claim || (claim.write &&
                canonical_claim_path(claim.path) ==
                    canonical_claim_path(j->run_dir));
        if (paths_ok && !run_claim && !j->run_dir.empty()) {
            j->path_claims.push_back({canonical_claim_path(j->run_dir), true});
            load_changed = true;
        }
        _jobs[j->job_id] = j;
        if (j->state == JobState::Queued) {
            std::string lease_error;
            if (!claim_paths_locked(*j, lease_error) ||
                !acquire_claim_leases_locked(*j, lease_error)) {
                j->state = JobState::Blocked;
                j->error = lease_error;
                load_changed = true;
                if (j->current_phase < j->phases.size()) {
                    j->phases[j->current_phase].outcome = "failed";
                    j->phases[j->current_phase].error = lease_error;
                }
            } else {
                _queue.push_back(j->job_id);
            }
        }
    }
    for (auto i = _jobs.begin(); i != _jobs.end(); ++i)
        for (auto j = std::next(i); j != _jobs.end(); ++j)
            for (const PathClaim& a : i->second->path_claims)
                for (const PathClaim& b : j->second->path_claims)
                    if (owns_claims(i->second->state) &&
                        owns_claims(j->second->state) &&
                        path_claims_conflict(a, b)) {
                        _state_error = "scheduler state has overlapping path claims";
                        _paused.store(true);
                        return;
                    }
    std::sort(_queue.begin(), _queue.end(), [this](const std::string& a, const std::string& b) { return _jobs.at(a)->order < _jobs.at(b)->order; });
    if (load_changed) save_locked();
    _cv.notify_all();
}

bool JobScheduler::has_eligible_job_locked() const {
    if (_paused.load() || _shutdown.load() || !_state_error.empty()) return false;
    for (const std::string& id : _queue) {
        auto it = _jobs.find(id); if (it == _jobs.end() || it->second->state != JobState::Queued) continue;
        const Job& j = *it->second;
        if (j.current_phase >= j.phases.size()) return true;
        const Phase& p = j.phases[j.current_phase];
        if (p.phase == "publish") return true;
        if (p.planned_device.empty() || p.planned_device == "auto" ||
            (!_leases.count(p.planned_device) &&
             p.planned_device != _foreground_device))
            return true;
    }
    return false;
}

void JobScheduler::reap_finished() {
    std::vector<std::shared_ptr<Attempt>> done;
    { std::lock_guard<std::mutex> lk(_mu); for (auto it = _active.begin(); it != _active.end();) { if (!it->second->finished.load()) { ++it; continue; } done.push_back(it->second); it = _active.erase(it); } }
    for (const auto& att : done) if (att->thread.joinable()) att->thread.join();
}

bool JobScheduler::advance_local_phase_locked(Job& j) {
    while (j.current_phase < j.phases.size() &&
           j.phases[j.current_phase].phase == "publish") {
        Phase& p = j.phases[j.current_phase];
        if (j.completed_prefix != j.current_phase) return false;
        if (j.current_phase > 0 &&
            j.phases[j.current_phase - 1].outputs.empty() &&
            j.phases[j.current_phase - 1].output.empty()) {
            return false;
        }
        p.outcome = "success";
        p.completed = true;
        p.error.clear();
        if (j.current_phase) {
            const Phase& previous = j.phases[j.current_phase - 1];
            p.output = previous.output;
            p.outputs = previous.outputs;
            if (p.outputs.empty() && !p.output.empty()) p.outputs.push_back(p.output);
        }
        j.completed_prefix++;
        j.current_phase++;
    }
    if (j.current_phase >= j.phases.size()) {
        j.state = JobState::Succeeded;
        j.phase = "publish";
        return true;
    }
    j.phase = j.phases[j.current_phase].phase;
    j.device = j.phases[j.current_phase].planned_device;
    j.device_name = j.phases[j.current_phase].planned_device_name;
    j.args = j.phases[j.current_phase].args;
    if (j.phase == "train") {
        set_arg_value(j.args, "--output-dir-name", j.job_id);
        j.phases[j.current_phase].args = j.args;
    }
    j.output_dir = j.phases[j.current_phase].output;
    if (j.phase == "train" && j.output_dir.empty()) {
        if (const fs::path output = training_output_dir(j); !output.empty()) {
            fs::path resolved = output;
            if (resolved.is_relative()) resolved = fs::u8path(j.work_dir) / resolved;
            j.output_dir = canonical_claim_path(resolved.u8string());
            j.phases[j.current_phase].output = j.output_dir;
            bool present = false;
            for (const PathClaim& claim : j.path_claims)
                if (claim.write && canonical_claim_path(claim.path) == canonical_claim_path(j.output_dir))
                    present = true;
            if (!present) j.path_claims.push_back({canonical_claim_path(j.output_dir), true});
        }
    }
    return true;
}

void JobScheduler::dispatch_one() {
    std::shared_ptr<Job> j; std::shared_ptr<Attempt> att;
    {
        std::lock_guard<std::mutex> lk(_mu);
        for (;;) {
            if (!has_eligible_job_locked()) return;
            size_t idx = _queue.size(); bool discarded = false;
            for (size_t i = 0; i < _queue.size(); ++i) {
                auto it = _jobs.find(_queue[i]); if (it == _jobs.end() || it->second->state != JobState::Queued) continue;
                Job& candidate_job = *it->second;
                if (candidate_job.current_phase >= candidate_job.phases.size()) { _queue.erase(_queue.begin() + (ptrdiff_t)i); candidate_job.state = JobState::Succeeded; save_locked(); discarded = true; break; }
                Phase& p = candidate_job.phases[candidate_job.current_phase];
                if (p.phase != "publish" && (_leases.count(p.planned_device) || p.planned_device == _foreground_device)) continue;
                std::string claim_error;
                if (!claim_paths_locked(candidate_job, claim_error)) { _queue.erase(_queue.begin() + (ptrdiff_t)i); p.error = claim_error; transition_locked(candidate_job, JobState::Blocked, claim_error); release_claim_leases_locked(candidate_job.job_id); save_locked(); discarded = true; break; }
                if (p.phase != "publish" && (p.planned_device.empty() || p.planned_device == "auto")) { _queue.erase(_queue.begin() + (ptrdiff_t)i); p.error = "device must be resolved before dispatch"; transition_locked(candidate_job, JobState::Blocked, p.error); release_claim_leases_locked(candidate_job.job_id); save_locked(); discarded = true; break; }
                if (p.phase != "publish" && _device_validator) {
                    std::string error; bool valid = false; try { valid = _device_validator(p.planned_device, error); } catch (const std::exception& e) { error = e.what(); }
                    if (!valid) { _queue.erase(_queue.begin() + (ptrdiff_t)i); p.error = error.empty() ? "device is unavailable" : error; transition_locked(candidate_job, JobState::Blocked, p.error); release_claim_leases_locked(candidate_job.job_id); save_locked(); discarded = true; break; }
                }
                idx = i; break;
            }
            if (discarded) continue;
            if (idx == _queue.size()) return;
            j = _jobs[_queue[idx]];
            Phase& p = j->phases[j->current_phase];
            if (p.phase == "publish") {
                _queue.erase(_queue.begin() + (ptrdiff_t)idx);
                if (advance_local_phase_locked(*j)) {
                    if (j->state != JobState::Succeeded) _queue.push_back(j->job_id);
                    else release_claim_leases_locked(j->job_id);
                    queue_event_locked(*j); save_locked();
                } else {
                    transition_locked(*j, JobState::Failed,
                                      "publish phase could not validate prior artifact");
                    if (j->current_phase < j->phases.size())
                        j->phases[j->current_phase].error = j->error;
                    release_claim_leases_locked(j->job_id);
                    save_locked();
                }
                continue;
            }
            auto candidate = std::make_shared<Attempt>();
            auto leases = _claim_leases.find(j->job_id);
            if (leases == _claim_leases.end()) {
                std::string error;
                if (!acquire_claim_leases_locked(*j, error)) {
                    _queue.erase(_queue.begin() + (ptrdiff_t)idx);
                    transition_locked(*j, JobState::Blocked,
                                      error.empty() ? "workflow paths are unavailable" : error);
                    save_locked();
                    continue;
                }
                leases = _claim_leases.find(j->job_id);
            }
            candidate->path_leases = leases->second;
            candidate->shutdown_grace_ms =
                p.phase == "train" ? 300000 : 30000;
            const JobState previous_state = j->state; const std::string previous_attempt = j->attempt_id; const std::string previous_error = j->error;
            j->attempt_id = new_attempt_id(); p.actual_device = p.planned_device; p.actual_device_name = p.planned_device_name; p.outcome = "running"; j->state = JobState::Starting; j->error.clear();
            if (!save_locked()) { j->state = previous_state; j->attempt_id = previous_attempt; j->error = previous_error; _paused.store(true); return; }
            _queue.erase(_queue.begin() + (ptrdiff_t)idx); _leases[p.planned_device] = j->job_id;
            queue_event_locked(*j); att = std::move(candidate); att->id = j->attempt_id; att->job_id = j->job_id; att->phase_index = j->current_phase; _active[j->job_id] = att; break;
        }
    }
    try { att->thread = std::thread([this, att] { supervisor_main(att); }); }
    catch (const std::exception& e) { std::lock_guard<std::mutex> lk(_mu); _active.erase(j->job_id); _leases.erase(j->device); if (j->current_phase < j->phases.size()) j->phases[j->current_phase].error = e.what(); transition_locked(*j, JobState::Failed, e.what()); release_claim_leases_locked(j->job_id); save_locked(); }
}

void JobScheduler::supervisor_main(std::shared_ptr<Attempt> att) {
    std::shared_ptr<Job> j;
    Phase phase;
    std::vector<std::string> prep_outputs;
    bool completed_prep = false;
    {
        std::lock_guard<std::mutex> lk(_mu);
        auto it = _jobs.find(att->job_id);
        if (it == _jobs.end() || att->phase_index >= it->second->phases.size()) {
            att->finished.store(true);
            _cv.notify_all();
            return;
        }
        j = it->second;
        phase = j->phases[att->phase_index];
        if (phase.phase == "sfm" && att->phase_index > 0) {
            const Phase& prep = j->phases[att->phase_index - 1];
            if (prep.phase == "prep" && prep.completed) {
                completed_prep = true;
                prep_outputs = prep.outputs;
            }
        }
        if (j->state == JobState::Starting) {
            j->state = JobState::Running;
            save_locked();
            queue_event_locked(*j);
        }
    }
    const fs::path run_dir = fs::u8path(j->run_dir); const fs::path req_path = run_dir / ("request-" + att->id + ".json"); const fs::path res_path = run_dir / ("result-" + att->id + ".json"); const fs::path log_path = run_dir / ("log-" + att->id + ".txt");
    app::worker::Request request; request.schema_version = 2; request.request_path = req_path.u8string(); request.job_id = j->job_id; request.attempt_id = att->id; request.phase = phase.phase; request.device = phase.planned_device; request.device_name = phase.planned_device_name; request.work_dir = j->work_dir; request.workspace = j->workspace; request.result_path = res_path.u8string(); request.args = phase.args; request.payload = phase.payload;
    try { if (completed_prep) bind_prep_masks(request.args, prep_outputs); write_request_file(request, req_path); app::worker::validate_request(request); }
    catch (const std::exception& e) { std::lock_guard<std::mutex> lk(_mu); if (att->phase_index < j->phases.size()) { j->phases[att->phase_index].outcome = "failed"; j->phases[att->phase_index].error = e.what(); } transition_locked(*j, JobState::Failed, e.what()); _leases.erase(phase.planned_device); release_claim_leases_locked(j->job_id); att->finished.store(true); save_locked(); _cv.notify_all(); return; }
    std::ofstream log_file(log_path, std::ios::binary | std::ios::trunc);
    proc::ProcessOptions opts; opts.argv = {_exe_path, "worker", "--request", req_path.u8string()}; opts.cwd = j->work_dir; opts.cancel = &att->cancel; opts.stop = &att->stop; opts.stop_token = "STOP\n"; opts.grace_period_ms = phase.phase == "train" ? 300000 : 30000; opts.env_overrides = {{"SS_WORKER_CONTROL", "1"}, {"SS_CRASH_DIR", j->run_dir}, {"SS_STATE_LOCK_HELD", "1"}};
    bool first_lease = true;
    for (const auto& lease : att->path_leases) {
        if (!lease || !lease->valid()) continue;
#ifdef _WIN32
        opts.inherit_handles.push_back(lease->native_handle());
        if (first_lease) opts.env_overrides.push_back({"SS_OUTPUT_LEASE_HANDLE", std::to_string(reinterpret_cast<uintptr_t>(lease->native_handle()))});
#else
        opts.inherit_fds.push_back(lease->native_fd());
        if (first_lease) opts.env_overrides.push_back({"SS_OUTPUT_LEASE_FD", std::to_string(lease->native_fd())});
#endif
        first_lease = false;
    }
    if (!first_lease) opts.env_overrides.push_back({"SS_OUTPUT_LEASE_HELD", "1"});
#ifdef _WIN32
    opts.inherit_handles.push_back(_state_lock);
    opts.env_overrides.push_back({"SS_STATE_LOCK_HANDLE", std::to_string(reinterpret_cast<uintptr_t>(_state_lock))});
#else
    opts.inherit_fds.push_back(_state_lock);
    opts.env_overrides.push_back({"SS_STATE_LOCK_FD", std::to_string(_state_lock)});
#endif
    opts.on_line = [this, &log_file, job_id = att->job_id](const std::string& line) { if (log_file) { log_file << line << '\n'; log_file.flush(); } std::lock_guard<std::mutex> lk(_mu); auto it = _jobs.find(job_id); if (it != _jobs.end()) queue_event_locked(*it->second, line); };
    const proc::ProcessResult process = proc::run_process(opts); if (log_file) log_file.flush();
    app::worker::Result result; bool result_ok = false; std::string message = process.error_message; int exit_code = process.exit_code;
    try { result = app::worker::parse_result(res_path.u8string()); result_ok = result.job_id == att->job_id && result.attempt_id == att->id && result.phase == phase.phase; if (result_ok) { message = result.message.empty() ? message : result.message; exit_code = result.exit_code; } } catch (...) {}
    std::string artifact_error;
    if (result_ok && (result.outcome == "success" || result.outcome == "partial" || result.outcome == "nonmetric")) {
        if (!validate_phase_result(*j, phase, result, artifact_error)) { result_ok = false; message = artifact_error; }
    }
    {
        std::lock_guard<std::mutex> lk(_mu); auto it = _jobs.find(att->job_id);
        if (it != _jobs.end()) {
            Job& job = *it->second; _leases.erase(phase.planned_device); Phase& saved = job.phases[att->phase_index]; saved.actual_device = phase.planned_device; saved.actual_device_name = phase.planned_device_name; saved.exit_code = exit_code;
            const bool cancelled = process.outcome == proc::ProcessOutcome::Cancelled; const bool stopped = process.outcome == proc::ProcessOutcome::Stopped; const bool spawn = process.outcome == proc::ProcessOutcome::SpawnFailed;
            if (cancelled) { saved.outcome = "interrupted"; saved.error = "force-stopped"; transition_locked(job, JobState::Interrupted, saved.error); job.pending_resume = true; release_claim_leases_locked(job.job_id); }
            else if (result_ok && (result.outcome == "success" || result.outcome == "partial" || result.outcome == "nonmetric") && process.outcome == proc::ProcessOutcome::Success && (exit_code == 0 || (phase.phase == "sfm" && (exit_code == 3 || exit_code == 4)))) {
                saved.outcome = result.outcome; saved.completed = true; saved.error.clear(); saved.outputs = result.outputs; if (!result.sparse_path.empty()) { saved.output = result.sparse_path; saved.outputs.push_back(result.sparse_path); } job.completed_prefix = std::max(job.completed_prefix, att->phase_index + 1); job.current_phase = att->phase_index + 1; job.attempt_id.clear(); job.pending_resume = false; if (advance_local_phase_locked(job)) { if (job.state != JobState::Succeeded) { job.state = JobState::Queued; _queue.push_back(job.job_id); } else release_claim_leases_locked(job.job_id); queue_event_locked(job); } else { saved.error = "workflow could not advance"; transition_locked(job, JobState::Failed, saved.error); release_claim_leases_locked(job.job_id); }
            } else if (result_ok && result.outcome == "stopped" && (process.outcome == proc::ProcessOutcome::Success || stopped)) { saved.outcome = "stopped"; saved.error = result.message; transition_locked(job, JobState::Stopped, saved.error); job.pending_resume = true; release_claim_leases_locked(job.job_id); }
            else { saved.outcome = spawn ? "spawn_failed" : "failed"; saved.error = message.empty() ? "worker exited " + std::to_string(exit_code) : message; transition_locked(job, JobState::Failed, saved.error); release_claim_leases_locked(job.job_id); }
            job.last_exit_code = exit_code; if (job.current_phase < job.phases.size()) { job.phase = job.phases[job.current_phase].phase; job.device = job.phases[job.current_phase].planned_device; job.device_name = job.phases[job.current_phase].planned_device_name; job.args = job.phases[job.current_phase].args; job.output_dir = job.phases[job.current_phase].output; }
        }
        att->finished.store(true); save_locked();
    }
    _cv.notify_all();
}

void JobScheduler::shutdown() {
    {
        std::lock_guard<std::mutex> lk(_mu);
        if (_shutdown.exchange(true)) return;
    }
    _cv.notify_all(); if (_dispatcher.joinable()) _dispatcher.join();
    std::vector<std::shared_ptr<Attempt>> live; { std::lock_guard<std::mutex> lk(_mu); for (auto& [_, a] : _active) live.push_back(a); }
    for (auto& a : live) a->stop.store(true);
    int grace_ms = 0;
    for (const auto& a : live)
        grace_ms = std::max(grace_ms, a->shutdown_grace_ms);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(grace_ms);
    for (;;) { bool done = true; for (const auto& a : live) done = done && a->finished.load(); if (done || std::chrono::steady_clock::now() >= deadline) break; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    for (auto& a : live) if (!a->finished.load()) a->cancel.store(true);
    for (auto& a : live) if (a->thread.joinable()) a->thread.join();
    std::lock_guard<std::mutex> lk(_mu); save_locked();
}

}  // namespace app::sched
