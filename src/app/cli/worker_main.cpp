// worker_main.cpp -- `spirula worker --request <file>`, internal.
// One phase, one process: the scheduler's unit of execution.

#include "app/Tools.h"
#include "app/WorkerRequest.h"
#include "core/Env.h"

#ifdef SS_TOOL_SFM
#include "sfm/Pipeline.h"
#endif

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
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

namespace fs = std::filesystem;

#ifdef SS_TOOL_TRAIN
int spirula_train_main(int argc, char** argv);
#endif
#ifdef SS_TOOL_SFM
int spirula_sfm_main(int argc, char** argv);
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

struct WorkerControl {
    std::atomic<bool> stop{false};
    std::atomic<bool> done{false};
    std::thread listener;
    WorkerControl() {
        if (!spirula::env_on("WORKER_CONTROL") && !spirula::env_on("SS_WORKER_CONTROL")) return;
        listener = std::thread([this] {
#ifdef _WIN32
            HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
            for (;;) {
                if (done.load()) return;
                DWORD available = 0;
                if (!in || !PeekNamedPipe(in, nullptr, 0, nullptr, &available, nullptr)) break;
                if (!available) { Sleep(25); continue; }
                char buf[32]; DWORD n = 0;
                if (!ReadFile(in, buf, sizeof buf, &n, nullptr) || !n) break;
                stop.store(true); return;
            }
#else
            pollfd input{STDIN_FILENO, POLLIN | POLLHUP | POLLERR, 0};
            for (;;) {
                if (done.load()) return;
                const int ready = ::poll(&input, 1, 50);
                if (ready < 0 && errno == EINTR) continue;
                if (ready <= 0) { if (ready < 0) break; continue; }
                char buf[32];
                if (::read(STDIN_FILENO, buf, sizeof buf) <= 0) break;
                stop.store(true); return;
            }
#endif
            stop.store(true);
        });
    }
    ~WorkerControl() {
        done.store(true);
        if (listener.joinable()) listener.join();
    }
};

std::vector<std::string> build_argv(const app::worker::Request& r) {
    std::vector<std::string> argv;
    argv.push_back("spirula " + r.phase);
    argv.insert(argv.end(), r.args.begin(), r.args.end());
    if (!r.device.empty()) { argv.push_back("--device"); argv.push_back(r.device); }
    return argv;
}

int run_sfm(const app::worker::Request& r, app::worker::Result& result) {
#ifdef SS_TOOL_SFM
    std::vector<std::string> args = r.args;
    if (!args.empty() && args.front() == "auto") args.erase(args.begin());
    args.push_back("--device"); args.push_back(r.device);
    sfm::AutoRequest request;
    if (const std::string error = sfm::parse_auto_args(args, request); !error.empty()) {
        result.outcome = "failed"; result.message = error; return 2;
    }
    const sfm::AutoResult value = sfm::run_auto(request.cfg, request.in);
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
    else result.outcome = "failed";
    return value.exit_code;
#else
    (void)r; result.outcome = "spawn_failed"; result.message = "worker: SfM phase unavailable in this build"; return 100;
#endif
}

int run_prep(const app::worker::Request& r, app::worker::Result& result) {
    WorkerControl control;
    try {
        app::PrepJob job = app::worker::deserialize_prep_job(r.payload);
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
        result.images = prepared.n_images;
        return 0;
    } catch (const std::exception& e) {
        result.exit_code = 2; result.outcome = "failed"; result.message = e.what(); return 2;
    }
}

int run_tool_phase(const app::worker::Request& r, app::worker::Result& result) {
    std::vector<std::string> storage = build_argv(r);
    std::vector<char*> argv; argv.reserve(storage.size() + 1);
    for (std::string& s : storage) argv.push_back(s.data());
    argv.push_back(nullptr);
    const int argc = (int)argv.size() - 1;
    if (r.phase == "train") {
#ifdef SS_TOOL_TRAIN
#ifdef _WIN32
        _putenv_s("SS_WORKER_CONTROL", "1"); _putenv_s("WORKER_CONTROL", "1");
#else
        setenv("SS_WORKER_CONTROL", "1", 1); setenv("WORKER_CONTROL", "1", 1);
#endif
        return spirula_train_main(argc, argv.data());
#else
        result.outcome = "spawn_failed"; result.message = "worker: train phase unavailable in this build"; return 100;
#endif
    }
    if (r.phase == "geometry") {
#ifdef SS_TOOL_GEOMETRY
        return spirula_geometry_main(argc, argv.data());
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
