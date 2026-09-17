// worker_main.cpp -- `spirula worker --request <file>`, internal.
//
// One phase, one process: the scheduler's unit of execution. Reads the frozen
// request, splices --device into argv, enters the existing tool's main
// in-process. Publishes `result.json` by atomic rename and exits with the
// tool's exit code (0 = phase success; anything else is in the result file).
//
// No help text -- this is not for a human. The scheduler is the only
// producer of well-formed requests; anything else gets a loud error.
//
// Stop and cancel: the scheduler drops "STOP\n" onto our stdin. We forward
// it to whichever phase needs it; the train phase listens via the
// SS_WORKER_CONTROL=1 hook in cli/main.cpp.

#include "app/Tools.h"
#include "app/WorkerRequest.h"
#include "core/Env.h"

#include <cerrno>
#include <climits>
#include <chrono>
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
#endif

namespace fs = std::filesystem;

// Each tool's main. Declared in Tools.h behind its SS_TOOL_* macro; this file
// is only linked when the combined binary has at least TRAIN or SFM enabled.
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
    if (!text || !text[0]) {
        error = "output lease handle missing";
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long raw = std::strtoull(text, &end, 10);
    if (errno || end == text || *end || raw > UINTPTR_MAX) {
        error = "output lease handle invalid";
        return false;
    }
    HANDLE handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(raw));
    if (!SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0)) {
        error = "output lease handle is not owned by worker";
        return false;
    }
#else
    const char* text = spirula::env("OUTPUT_LEASE_FD");
    if (!text || !text[0]) {
        error = "output lease file descriptor missing";
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const long raw = std::strtol(text, &end, 10);
    if (errno || end == text || *end || raw < 0 || raw > INT_MAX) {
        error = "output lease file descriptor invalid";
        return false;
    }
    const int fd = static_cast<int>(raw);
    const int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
        error = "output lease file descriptor is not owned by worker";
        return false;
    }
#endif
    return true;
}

// --device is spliced in so the phase lands on the resolved GPU even when the
// request forgot.
std::vector<std::string> build_argv(const app::worker::Request& r) {
    std::vector<std::string> argv;
    // argv[0] is the program-name the tool prints in its usage lines.
    argv.push_back("spirula " + r.phase);
    argv.insert(argv.end(), r.args.begin(), r.args.end());
    if (!r.device.empty()) {
        argv.push_back("--device");
        argv.push_back(r.device);
    }
    return argv;
}

int run_phase(const app::worker::Request& r) {
    std::vector<std::string> storage = build_argv(r);
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& s : storage) argv.push_back(s.data());
    argv.push_back(nullptr);
    const int argc = (int)argv.size() - 1;

    if (r.phase == "train") {
#ifdef SS_TOOL_TRAIN
        // The stdin stop hook only wires itself when this is set.
#ifdef _WIN32
        _putenv_s("SS_WORKER_CONTROL", "1");
#else
        setenv("SS_WORKER_CONTROL", "1", 1);
#endif
        return spirula_train_main(argc, argv.data());
#else
        std::fprintf(stderr, "worker: train phase unavailable in this build\n");
        return 100;
#endif
    }
    if (r.phase == "sfm") {
#ifdef SS_TOOL_SFM
        return spirula_sfm_main(argc, argv.data());
#else
        std::fprintf(stderr, "worker: sfm phase unavailable in this build\n");
        return 100;
#endif
    }
    if (r.phase == "geometry") {
#ifdef SS_TOOL_GEOMETRY
        return spirula_geometry_main(argc, argv.data());
#else
        std::fprintf(stderr, "worker: geometry phase unavailable in this build\n");
        return 100;
#endif
    }
    std::fprintf(stderr, "worker: unknown phase %s\n", r.phase.c_str());
    return 101;
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
    } catch (const std::exception& e) {
        // Cannot publish into a request we could not read. Print where a
        // scheduler watching the process sees it.
        std::fprintf(stderr, "worker: bad request: %s\n", e.what());
        return 2;
    }
    std::string lease_error;
    if (!protect_output_lease(lease_error)) {
        std::fprintf(stderr, "worker: invalid output lease: %s\n",
                     lease_error.c_str());
        return 2;
    }

    // Change cwd after parse -- args in the request may be relative to it.
    if (!req.work_dir.empty()) {
        std::error_code ec;
        fs::current_path(fs::u8path(req.work_dir), ec);
        if (ec) {
            res.outcome = "spawn_failed";
            res.message = "cannot chdir to work_dir: " + ec.message();
            app::worker::publish_result(req, res);
            return 3;
        }
    }

    int rc = 100;
    try {
        rc = run_phase(req);
    } catch (const std::exception& e) {
        res.outcome = "failed";
        res.exit_code = 99;
        res.message = e.what();
        app::worker::publish_result(req, res);
        return 99;
    }

    // The underlying tool owns success/failure; the outcome spelling matches
    // what the scheduler understands.
    res.exit_code = rc;
    if (rc == 0) {
        res.outcome = "success";
    } else if (rc == 100) {
        res.outcome = "spawn_failed";  // build lacks the tool
        res.message = "phase unavailable in this build";
    } else if (rc == 42) {
        // Reserved: cooperative stop wrote its last checkpoint and exited.
        res.outcome = "stopped";
    } else {
        res.outcome = "failed";
    }
    if (!app::worker::publish_result(req, res)) {
        std::fprintf(stderr, "worker: could not publish result to %s\n",
                     req.result_path.c_str());
        // Still return the tool's code; scheduler reconciles missing files as
        // crashes once the process is seen to have exited.
    }
    return rc;
}
