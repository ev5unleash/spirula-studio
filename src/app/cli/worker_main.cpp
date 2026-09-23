// worker_main.cpp -- `spirula worker --request <file>`, internal.
// One phase, one process: the scheduler's unit of execution.

#include "app/AgentFeatureWorker.h"
#include "app/Subprocess.h"
#include "app/TextFile.h"
#include "app/Tools.h"
#include "app/WorkerRequest.h"
#include "core/Env.h"

#ifdef SS_TOOL_SFM
#include "app/AgentReconstructionJob.h"
#include "sfm/Pipeline.h"
#include "sfm/core/Progress.h"
#include "sfm/core/Cancel.h"
#endif
#ifdef SS_TOOL_TRAIN
#include "app/AgentTrainingJob.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

#ifndef SS_VERSION
#define SS_VERSION "unknown"
#endif

namespace fs = std::filesystem;

#ifdef SS_TOOL_SFM
int spirula_sfm_main(int argc, char** argv);
void spirula_sfm_set_cancel_token(const std::atomic<bool>* token);
#endif
#ifdef SS_TOOL_GEOMETRY
int spirula_geometry_main(int argc, char** argv);
#endif

namespace {

bool protect_output_lease(std::string& error) {
    if (!spirula::env_on("OUTPUT_LEASE_HELD")) return true;
#ifdef _WIN32
    const char* text = spirula::env("OUTPUT_LEASE_HANDLE");
    if (!text || !text[0]) { error = "output lease handle missing"; return false; }
    errno = 0; char* end = nullptr;
    const unsigned long long raw = std::strtoull(text, &end, 10);
    if (errno || end == text || *end || raw > UINTPTR_MAX) { error = "output lease handle invalid"; return false; }
    HANDLE handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(raw));
    if (!SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0)) { error = "output lease handle is not owned by worker"; return false; }
#else
    const char* text = spirula::env("OUTPUT_LEASE_FD");
    if (!text || !text[0]) { error = "output lease file descriptor missing"; return false; }
    errno = 0; char* end = nullptr; const long raw = std::strtol(text, &end, 10);
    if (errno || end == text || *end || raw < 0 || raw > INT_MAX) { error = "output lease file descriptor invalid"; return false; }
    const int fd = static_cast<int>(raw); const int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) { error = "output lease file descriptor is not owned by worker"; return false; }
#endif
    return true;
}

bool protect_state_lock(std::string& error) {
    if (!spirula::env_on("STATE_LOCK_HELD")) return true;
#ifdef _WIN32
    const char* text = spirula::env("STATE_LOCK_HANDLE");
    if (!text || !text[0]) { error = "scheduler state lock handle missing"; return false; }
    errno = 0; char* end = nullptr;
    const unsigned long long raw = std::strtoull(text, &end, 10);
    if (errno || end == text || *end || raw > UINTPTR_MAX) { error = "scheduler state lock handle invalid"; return false; }
    HANDLE handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(raw));
    if (!SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0)) { error = "scheduler state lock is not owned by worker"; return false; }
#else
    const char* text = spirula::env("STATE_LOCK_FD");
    if (!text || !text[0]) { error = "scheduler state lock file descriptor missing"; return false; }
    errno = 0; char* end = nullptr; const long raw = std::strtol(text, &end, 10);
    if (errno || end == text || *end || raw < 0 || raw > INT_MAX) { error = "scheduler state lock file descriptor invalid"; return false; }
    const int fd = static_cast<int>(raw); const int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) { error = "scheduler state lock is not owned by worker"; return false; }
#endif
    return true;
}

struct WorkerControl {
    std::atomic<bool> stop{false};
    std::atomic<bool> pause_requested{false};
    std::atomic<bool> acknowledged_paused{false};
    std::atomic<bool> done{false};
    std::thread listener;

    void command(app::proc::WorkerCommand value) {
        switch (value) {
            case app::proc::WorkerCommand::Pause:
                if (!stop.load(std::memory_order_acquire))
                    pause_requested.store(true, std::memory_order_release);
                break;
            case app::proc::WorkerCommand::Resume:
                if (!stop.load(std::memory_order_acquire))
                    pause_requested.store(false, std::memory_order_release);
                break;
            case app::proc::WorkerCommand::Stop:
                pause_requested.store(false, std::memory_order_release);
                stop.store(true, std::memory_order_release);
                break;
        }
    }

    static void acknowledge(void* context, bool paused) {
        auto& self = *static_cast<WorkerControl*>(context);
        if (self.stop.load(std::memory_order_acquire)) return;
        const bool previous =
            self.acknowledged_paused.exchange(paused,
                                              std::memory_order_acq_rel);
        if (previous == paused) return;
        std::fputs(paused ? "WORKER_PAUSED\n" : "WORKER_RUNNING\n",
                   stdout);
        std::fflush(stdout);
    }

    void install_sfm_pause_gate() {
#ifdef SS_TOOL_SFM
        sfm::cancel::set_pause_token(
            spirula::env_on("WORKER_CONTROL") ? &pause_requested : nullptr,
            &WorkerControl::acknowledge, this);
#endif
    }

    WorkerControl() {
        if (!spirula::env_on("WORKER_CONTROL")) return;
        listener = std::thread([this] {
            app::proc::WorkerCommandParser parser;
            auto consume = [&](const char* bytes, size_t count) {
                for (size_t i = 0; i < count; ++i)
                    if (auto command = parser.push(bytes[i]))
                        this->command(*command);
            };
#ifdef _WIN32
            HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
            for (;;) {
                if (done.load(std::memory_order_acquire)) return;
                DWORD available = 0;
                if (!in || !PeekNamedPipe(in, nullptr, 0, nullptr, &available,
                                          nullptr))
                    return;
                if (!available) { Sleep(25); continue; }
                char buf[32];
                DWORD n = 0;
                if (!ReadFile(in, buf, sizeof buf, &n, nullptr) || !n)
                    return;
                consume(buf, n);
            }
#else
            pollfd input{STDIN_FILENO, POLLIN | POLLHUP | POLLERR, 0};
            for (;;) {
                if (done.load(std::memory_order_acquire)) return;
                const int ready = ::poll(&input, 1, 50);
                if (ready < 0 && errno == EINTR) continue;
                if (ready <= 0) {
                    if (ready < 0) return;
                    continue;
                }
                char buf[32];
                const ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
                if (n <= 0) return;
                consume(buf, static_cast<size_t>(n));
            }
#endif
        });
    }
    ~WorkerControl() {
#ifdef SS_TOOL_SFM
        sfm::cancel::set_pause_token(nullptr);
#endif
        done.store(true, std::memory_order_release);
        if (listener.joinable()) listener.join();
    }
};

std::vector<std::string> build_argv(const app::worker::Request& r) {
    std::vector<std::string> argv;
    if (r.phase == "sfm-extract") argv.push_back("spirula sfm");
    else argv.push_back("spirula " + r.phase);
    argv.insert(argv.end(), r.args.begin(), r.args.end());
    if (!r.device.empty()) { argv.push_back("--device"); argv.push_back(r.device); }
    return argv;
}

int run_sfm_extract(const app::worker::Request& r, app::worker::Result& result) {
    std::string output;
    const auto reject_missing_output = [&] {
        result.outcome = "failed";
        result.message = "worker: SfM extract output path missing";
        return 2;
    };
    for (size_t i = 0; i < r.args.size(); ++i) {
        const std::string& arg = r.args[i];
        if (arg == "-o" || arg == "--output") {
            if (i + 1 >= r.args.size() || r.args[i + 1].empty())
                return reject_missing_output();
            output = r.args[++i];
        } else if (arg.compare(0, 9, "--output=") == 0) {
            output = arg.substr(9);
            if (output.empty()) return reject_missing_output();
        }
    }
    if (output.empty()) return reject_missing_output();
#ifdef SS_TOOL_SFM
    WorkerControl control;
    control.install_sfm_pause_gate();
    std::vector<std::string> storage = build_argv(r);
    std::vector<char*> argv; argv.reserve(storage.size() + 1);
    for (std::string& s : storage) argv.push_back(s.data());
    argv.push_back(nullptr);
    const int argc = (int)argv.size() - 1;
    spirula_sfm_set_cancel_token(&control.stop);
    int rc;
    try {
        rc = spirula_sfm_main(argc, argv.data());
    } catch (...) {
        spirula_sfm_set_cancel_token(nullptr);
        throw;
    }
    spirula_sfm_set_cancel_token(nullptr);
    if (control.stop.load() && rc != 0) rc = 42;
    if (rc == 0) {
        result.outputs.clear();
        result.outputs.push_back(output);
    }
    return rc;
#else
    (void)r;
    result.outcome = "spawn_failed";
    result.message = "worker: SfM phase unavailable in this build";
    return 100;
#endif
}

#ifdef SS_TOOL_SFM
int run_portable_reconstruction(const app::worker::Request& r,
                                app::worker::Result& result) {
    const auto fail = [&](const std::string& message) {
        result.outcome = "failed";
        result.message = message;
        result.exit_code = 2;
        return 2;
    };
    if (r.args.size() != 4 ||
        r.args[0] != "--agent-portable-reconstruction" || r.payload != "{}")
        return fail("worker: invalid portable reconstruction request");
    std::uint64_t disk_budget = 0;
    const std::string& budget_text = r.args[3];
    const auto parsed = std::from_chars(budget_text.data(),
                                        budget_text.data() + budget_text.size(),
                                        disk_budget);
    if (parsed.ec != std::errc() ||
        parsed.ptr != budget_text.data() + budget_text.size() ||
        !disk_budget)
        return fail("worker: invalid portable reconstruction disk budget");

    const fs::path portable_root =
        fs::u8path(r.work_dir) / "reconstruction-attempts";
    if (r.device.empty())
        return fail("worker: scheduler did not lease a Vulkan device");
    const fs::path attempt_root =
        app::agent::portable_worker_detail::PortableAttemptDirectory(
            portable_root, r.args[1], r.args[2]);
    app::agent::portable_worker_detail::PortableOfferRecord record;
    std::string error;
    if (!app::agent::portable_worker_detail::LoadPortableOfferRecordFromAttempt(
            attempt_root, record, error) ||
        record.offer.workload !=
            app::agent::wire::PortableWorkload::Reconstruction ||
        record.offer.job_id != r.args[1] ||
        record.offer.attempt_id != r.args[2])
        return fail(error.empty()
                        ? "worker: reconstruction offer does not match the scheduler request"
                        : error);
    bool authoritative = false;
    if (!app::agent::portable_worker_detail::IsPortableAttemptAuthoritative(
            portable_root, r.args[1], r.args[2], record.leader_id,
            record.leader_epoch, authoritative, error) ||
        !authoritative)
        return fail(error.empty()
                        ? "worker: reconstruction attempt was superseded"
                        : error);
    if (record.offer.required_build !=
        app::agent::ReconstructionBuildIdentity())
        return fail("worker: reconstruction build identity changed");

    try {
        const app::agent::ReconstructionInputBundle inputs =
            app::agent::VerifyReconstructionInputs(
                attempt_root / "inputs", record.offer.inputs,
                record.offer.input_identity_sha256, disk_budget);
        sfm::AutoRequest request = app::agent::DecodeReconstructionRequest(
            inputs, attempt_root / "output" / "workspace", disk_budget);
        request.cfg.device_request = r.device;
        request.cfg.device_request_set = true;
        if (const std::string device_error = request.cfg.resolveDevice();
            !device_error.empty())
            return fail("worker: cannot resolve the leased Vulkan device: " +
                        device_error);
        WorkerControl control;
        control.install_sfm_pause_gate();
        sfm::RunContext context;
        context.set_cancel(&control.stop);
        context.set_progress_dir(request.progress_dir);
        context.set_events(sfm::progress::status);
        sfm::AutoResult value;
        try {
            value = sfm::run_auto(request.cfg, request.in);
        } catch (const sfm::Cancelled&) {
            result.outcome = "stopped";
            result.message = "SfM stopped";
            result.exit_code = 42;
            return 42;
        }
        result.exit_code = value.exit_code;
        result.registered = value.registered;
        result.images = value.images;
        result.points = value.points;
        result.models = value.models;
        result.mean_reprojection = value.mean_reproj;
        result.median_reprojection = value.median_reproj;
        result.partial = value.partial;
        result.metric = value.metric;
        result.sparse_path = value.sparse_dir.u8string();
        if (!result.sparse_path.empty())
            result.outputs.push_back(result.sparse_path);
        if (value.exit_code == 0)
            result.outcome = "success";
        else if (value.exit_code == 3)
            result.outcome = "partial";
        else if (value.exit_code == 4)
            result.outcome = "nonmetric";
        else if (control.stop.load())
            result.outcome = "stopped";
        else
            result.outcome = "failed";
        if (control.stop.load() && value.exit_code != 0) return 42;
        return value.exit_code;
    } catch (const std::exception& e) {
        return fail(e.what());
    }
}
#endif
#ifdef SS_TOOL_TRAIN
std::string train_cli_value(bool value) { return value ? "1" : "0"; }
std::string train_cli_value(int value) { return std::to_string(value); }
std::string train_cli_value(float value) {
    if (std::isinf(value)) return value > 0 ? "inf" : "-inf";
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
    return text.str();
}
std::string train_cli_value(const std::string& value) {
    if (value.find('\0') != std::string::npos)
        throw std::runtime_error("training config contains an embedded NUL");
    return value.empty() ? "none" : value;
}
template <typename T>
std::string train_cli_value(const std::optional<T>& value) {
    return value ? train_cli_value(*value) : "none";
}
template <typename T>
void append_train_flag(std::vector<std::string>& args, const char* name,
                       const T& value) {
    std::string flag = "--";
    for (const char c : std::string(name))
        flag += c == '_' ? '-' : c;
    args.push_back(std::move(flag));
    args.push_back(train_cli_value(value));
}
template <typename T, std::size_t N>
void append_train_flag(std::vector<std::string>& args, const char* name,
                       const std::array<T, N>& values) {
    std::string flag = "--";
    for (const char c : std::string(name))
        flag += c == '_' ? '-' : c;
    args.push_back(std::move(flag));
    for (const T& value : values) args.push_back(train_cli_value(value));
}

template <typename T>
void append_train_config_field(std::vector<std::string>& args,
                               const char* name, const T& value,
                               bool preserve_none_densification) {
    if (preserve_none_densification &&
        std::string(name) == "distraction_robustness")
        append_train_flag(args, name, std::string("off"));
    else
        append_train_flag(args, name, value);
}
void append_train_config_field(std::vector<std::string>& args,
                               const char* name, const std::string& value,
                               bool preserve_none_densification) {
    if (preserve_none_densification &&
        std::string(name) == "densify_loss_map_mode" && value == "none")
        return;
    if (preserve_none_densification &&
        std::string(name) == "distraction_robustness")
        append_train_flag(args, name, std::string("off"));
    else
        append_train_flag(args, name, value);
}

std::vector<std::string> training_cli_args(
    const app::agent::TrainingInvocationOptions& invocation,
    const std::string& device) {
    std::vector<std::string> args;
    args.push_back("spirula train");
    const bool preserve_none_densification =
        invocation.config.densify_loss_map_mode == "none";
    // The CLI reserves the string token "none" for an empty string. Its
    // academic-baseline preset is the typed spelling for densify mode "none";
    // every other field is explicitly supplied below.
    if (preserve_none_densification)
        args.emplace_back("academic-baseline");
    else if (!invocation.preset.empty())
        args.push_back(invocation.preset);
#define SS_APPEND_TRAIN_FIELD(type, member, default_, section, tier, choices) \
    append_train_config_field(args, #member, invocation.config.member,       \
                              preserve_none_densification);
    SS_CONFIG_FIELDS(SS_APPEND_TRAIN_FIELD)
#undef SS_APPEND_TRAIN_FIELD
    args.emplace_back("--device");
    args.push_back(device);
    return args;
}

void restore_training_configs(const fs::path& frozen_config,
                              const fs::path& output_root) {
    std::vector<fs::path> destinations{output_root / "config.json"};
    std::error_code ec;
    for (fs::directory_iterator it(output_root, ec), end; !ec && it != end;
         it.increment(ec)) {
        const fs::file_status status = it->symlink_status(ec);
        if (ec || fs::is_symlink(status) || !fs::is_directory(status)) continue;
        const std::string name = it->path().filename().u8string();
        if (name.rfind("step-", 0) == 0 &&
            name.size() > 5 && name.compare(name.size() - 5, 5, ".ckpt") == 0)
            destinations.push_back(it->path() / "config.json");
    }
    if (ec) throw std::runtime_error("cannot inspect training checkpoints");
    for (const fs::path& destination : destinations) {
        const fs::file_status status = fs::symlink_status(destination, ec);
        if (ec || !fs::is_regular_file(status) || fs::is_symlink(status))
            throw std::runtime_error("training produced an unsafe config.json");
        fs::copy_file(frozen_config, destination,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) throw std::runtime_error("cannot restore frozen training config");
    }
}

int run_portable_training(const app::worker::Request& r,
                          app::worker::Result& result) {
    const auto fail = [&](const std::string& message) {
        result.outcome = "failed";
        result.message = message;
        result.exit_code = 2;
        return 2;
    };
    if (r.args.size() != 4 || r.args[0] != "--agent-portable-training" ||
        r.payload != "{}")
        return fail("worker: invalid portable training request");
    std::uint64_t disk_budget = 0;
    const std::string& budget_text = r.args[3];
    const auto parsed = std::from_chars(budget_text.data(),
                                        budget_text.data() + budget_text.size(),
                                        disk_budget);
    if (parsed.ec != std::errc() ||
        parsed.ptr != budget_text.data() + budget_text.size() ||
        !disk_budget)
        return fail("worker: invalid portable training disk budget");
    if (r.device.empty())
        return fail("worker: scheduler did not lease a Vulkan device");

    const fs::path portable_root =
        fs::u8path(r.work_dir) / "reconstruction-attempts";
    const fs::path attempt_root =
        app::agent::portable_worker_detail::PortableAttemptDirectory(
            portable_root, r.args[1], r.args[2]);
    app::agent::portable_worker_detail::PortableOfferRecord record;
    std::string error;
    if (!app::agent::portable_worker_detail::LoadPortableOfferRecordFromAttempt(
            attempt_root, record, error) ||
        record.offer.workload != app::agent::wire::PortableWorkload::Training ||
        record.offer.job_id != r.args[1] ||
        record.offer.attempt_id != r.args[2])
        return fail(error.empty()
                        ? "worker: training offer does not match the scheduler request"
                        : error);
    if (record.offer.required_build != SS_VERSION)
        return fail("worker: training build identity changed");
    bool authoritative = false;
    if (!app::agent::portable_worker_detail::IsPortableAttemptAuthoritative(
            portable_root, r.args[1], r.args[2], record.leader_id,
            record.leader_epoch, authoritative, error) ||
        !authoritative)
        return fail(error.empty() ? "worker: training attempt was superseded"
                                  : error);

    try {
        const fs::path workspace =
            attempt_root / "output" / "workspace";
        const app::agent::TrainingInputBundle inputs =
            app::agent::VerifyTrainingInputs(
                workspace / "input", record.offer.inputs,
                record.offer.input_identity_sha256, disk_budget);
        const app::agent::TrainingInvocationOptions invocation =
            app::agent::MakeTrainingInvocation(inputs);
        std::vector<std::string> storage =
            training_cli_args(invocation, r.device);
        std::vector<char*> argv;
        argv.reserve(storage.size() + 1);
        for (std::string& arg : storage) argv.push_back(arg.data());
        argv.push_back(nullptr);
        const fs::path old_directory = fs::current_path();
#ifdef _WIN32
        _putenv_s("SS_WORKER_CONTROL", "1");
#else
        setenv("SS_WORKER_CONTROL", "1", 1);
#endif
        fs::current_path(invocation.working_directory);
        int rc = 0;
        try {
            rc = spirula_train_main(static_cast<int>(argv.size() - 1),
                                    argv.data());
        } catch (...) {
            std::error_code restore_error;
            fs::current_path(old_directory, restore_error);
            throw;
        }
        std::error_code restore_error;
        fs::current_path(old_directory, restore_error);
        if (restore_error)
            return fail("worker: cannot restore training working directory");
        if (rc != 0) return rc;

        restore_training_configs(inputs.config_path, invocation.output_directory);
        if (!app::agent::portable_worker_detail::IsPortableAttemptAuthoritative(
                portable_root, r.args[1], r.args[2], record.leader_id,
                record.leader_epoch, authoritative, error) ||
            !authoritative)
            return fail(error.empty() ? "worker: training attempt was superseded"
                                      : error);
        result.outputs.push_back(invocation.output_directory.u8string());
        return 0;
    } catch (const std::exception& e) {
        return fail(e.what());
    }
}
#endif

int run_sfm(const app::worker::Request& r, app::worker::Result& result) {
#ifdef SS_TOOL_SFM
    if (!r.args.empty() &&
        r.args.front() == "--agent-portable-reconstruction")
        return run_portable_reconstruction(r, result);
    WorkerControl control;
    control.install_sfm_pause_gate();
    std::vector<std::string> args = r.args;
    if (!args.empty() && args.front() == "auto") args.erase(args.begin());
    if (!r.payload.empty() && r.payload != "{}") {
        auto manifest = std::find(args.begin(), args.end(), "--manifest");
        if (manifest == args.end() || ++manifest == args.end()) {
            result.outcome = "failed"; result.message = "worker: SfM manifest path missing"; return 2;
        }
        const fs::path path = fs::u8path(*manifest);
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        const std::string write_error =
            ec ? path.u8string() + ": " + ec.message()
               : app::write_text_file(path, r.payload);
        if (!write_error.empty()) {
            result.outcome = "failed";
            result.message = "worker: cannot write SfM manifest " + write_error;
            return 2;
        }
    }
    args.push_back("--device"); args.push_back(r.device);
    sfm::AutoRequest request;
    if (const std::string error = sfm::parse_auto_args(args, request); !error.empty()) {
        result.outcome = "failed"; result.message = error; return 2;
    }
    sfm::RunContext context;
    context.set_cancel(&control.stop);
    context.set_progress_dir(request.progress_dir);
    context.set_events(sfm::progress::status);
    sfm::AutoResult value;
    try {
        if (!request.progress_dir.empty()) {
            std::ofstream marker(fs::path(request.progress_dir) / "attempt",
                                 std::ios::binary | std::ios::trunc);
            marker << r.attempt_id;
            marker.close();
            if (!marker)
                throw std::runtime_error("worker: cannot publish SfM preview attempt");
        }
        value = sfm::run_auto(request.cfg, request.in);
    } catch (const sfm::Cancelled&) {
        result.outcome = "stopped";
        result.message = "SfM stopped";
        result.exit_code = 42;
        return 42;
    }
    result.exit_code = value.exit_code;
    result.registered = value.registered; result.images = value.images;
    result.points = value.points; result.models = value.models;
    result.mean_reprojection = value.mean_reproj; result.median_reprojection = value.median_reproj;
    result.partial = value.partial; result.metric = value.metric;
    result.sparse_path = value.sparse_dir.u8string();
    if (!result.sparse_path.empty()) result.outputs.push_back(result.sparse_path);
    if (value.exit_code == 0) result.outcome = "success";
    else if (value.exit_code == 3) result.outcome = "partial";
    else if (value.exit_code == 4) result.outcome = "nonmetric";
    else if (control.stop.load()) result.outcome = "stopped";
    else result.outcome = "failed";
    if (control.stop.load() && value.exit_code != 0) return 42;
    return value.exit_code;
#else
    (void)r; result.outcome = "spawn_failed"; result.message = "worker: SfM phase unavailable in this build"; return 100;
#endif
}

int run_prep(const app::worker::Request& r, app::worker::Result& result) {
    WorkerControl control;
    try {
        app::PrepJob job = app::worker::deserialize_prep_job(r.payload);
        job.device = r.device;
        std::string reject;
        if (app::worker::reject_external_masking(job, reject) ||
            (job.mask_enable && !app::backends().builtin_masking)) {
            if (reject.empty())
                reject = "scheduled prep rejects external Python masking; enable a native SAM build "
                         "(ffmpeg CPU decoding remains allowed)";
            result.outcome = "failed"; result.message = reject; result.exit_code = 2; return 2;
        }
        app::PrepResult prepared;
        std::string error;
        app::DatasetPrep prep(control.stop);
        if (!prep.run(job, prepared, error)) {
            result.exit_code = control.stop.load() ? 42 : 2;
            result.outcome = control.stop.load() ? "stopped" : "failed";
            result.message = error;
            return result.exit_code;
        }
        result.exit_code = 0; result.outcome = "success";
        if (!prepared.image_dir.empty()) result.outputs.push_back(prepared.image_dir);
        if (!prepared.mask_dir.empty()) result.outputs.push_back(prepared.mask_dir);
        if (!prepared.provenance_sidecar.empty())
            result.outputs.push_back(prepared.provenance_sidecar);
        result.images = prepared.n_images;
        return 0;
    } catch (const std::exception& e) {
        result.exit_code = 2; result.outcome = "failed"; result.message = e.what(); return 2;
    }
}

int run_tool_phase(const app::worker::Request& r, app::worker::Result& result) {
    if (r.phase == "train" && !r.args.empty() &&
        r.args.front() == "--agent-portable-training") {
#ifdef SS_TOOL_TRAIN
        return run_portable_training(r, result);
#else
        result.outcome = "spawn_failed";
        result.message = "worker: train phase unavailable in this build";
        return 100;
#endif
    }
    std::vector<std::string> storage = build_argv(r);
    std::vector<char*> argv; argv.reserve(storage.size() + 1);
    for (std::string& s : storage) argv.push_back(s.data());
    argv.push_back(nullptr);
    const int argc = (int)argv.size() - 1;
    if (r.phase == "train") {
#ifdef SS_TOOL_TRAIN
#ifdef _WIN32
        _putenv_s("SS_WORKER_CONTROL", "1");
#else
        setenv("SS_WORKER_CONTROL", "1", 1);
#endif
        const bool has_project_reference =
            !r.project_root.empty() || !r.project_revision.empty() ||
            !r.project_revision_digest.empty() || !r.metadata_paths.empty() ||
            !r.artifact_paths.empty();
        if (has_project_reference)
            return spirula_train_main_with_project(
                argc, argv.data(), r.project_root.c_str(),
                r.project_revision.c_str());
        return spirula_train_main(argc, argv.data());
#else
        result.outcome = "spawn_failed"; result.message = "worker: train phase unavailable in this build"; return 100;
#endif
    }
    if (r.phase == "geometry") {
#ifdef SS_TOOL_GEOMETRY
        WorkerControl control;
        const int rc = spirula_geometry_main(argc, argv.data());
        if (rc == 0) {
            if (std::find(r.args.begin(), r.args.end(), "--depth") != r.args.end())
                result.outputs.push_back((fs::u8path(r.workspace) / "depths").u8string());
            if (std::find(r.args.begin(), r.args.end(), "--no-normal") == r.args.end())
                result.outputs.push_back((fs::u8path(r.workspace) / "normals").u8string());
        }
        return rc;
#else
        result.outcome = "spawn_failed"; result.message = "worker: geometry phase unavailable in this build"; return 100;
#endif
    }
    result.outcome = "failed"; result.message = "worker: unknown phase " + r.phase; return 101;
}

}  // namespace

int spirula_worker_main(int argc, char** argv) {
    if (argc < 3 || std::strcmp(argv[1], "--request") != 0) {
        std::fprintf(stderr, "usage: spirula worker --request <file>\n");
        return 2;
    }
    const std::string req_path = argv[2];
    app::worker::Request req;
    app::worker::Result res;
    try {
        req = app::worker::parse_request(req_path);
        app::worker::validate_request(req);
        res.job_id = req.job_id; res.attempt_id = req.attempt_id; res.phase = req.phase;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "worker: bad request: %s\n", e.what());
        return 2;
    }
    std::string lease_error;
    if (!protect_output_lease(lease_error)) {
        std::fprintf(stderr, "worker: invalid output lease: %s\n", lease_error.c_str());
        return 2;
    }
    if (!protect_state_lock(lease_error)) {
        std::fprintf(stderr, "worker: invalid scheduler state lock: %s\n",
                     lease_error.c_str());
        return 2;
    }
    if (!req.work_dir.empty()) {
        std::error_code ec; fs::current_path(fs::u8path(req.work_dir), ec);
        if (ec) {
            res.outcome = "spawn_failed"; res.exit_code = 3;
            res.message = "cannot chdir to work_dir: " + ec.message();
            app::worker::publish_result(req, res); return 3;
        }
    }
    int rc = 100;
    try {
        if (req.phase == "prep") rc = run_prep(req, res);
        else if (req.phase == "sfm") rc = run_sfm(req, res);
        else if (req.phase == "sfm-extract") rc = run_sfm_extract(req, res);
        else rc = run_tool_phase(req, res);
    } catch (const std::exception& e) {
        res.outcome = "failed"; res.exit_code = 99; res.message = e.what();
        app::worker::publish_result(req, res); return 99;
    }
    res.exit_code = rc;
    if (res.outcome.empty()) {
        if (rc == 0) res.outcome = "success";
        else if (rc == 42) res.outcome = "stopped";
        else if (rc == 100) { res.outcome = "spawn_failed"; if (res.message.empty()) res.message = "phase unavailable in this build"; }
        else res.outcome = "failed";
    }
    if (!app::worker::publish_result(req, res))
        std::fprintf(stderr, "worker: could not publish result to %s\n", req.result_path.c_str());
    return rc;
}
