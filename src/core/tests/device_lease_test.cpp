#include "app/DeviceLease.h"
#include "app/Subprocess.h"
#include "core/Sha256.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition) {
        std::printf("ok   %s\n", message);
    } else {
        std::printf("FAIL %s\n", message);
        ++failures;
    }
}

std::string unique_selector(const char* suffix) {
    const std::string seed =
        std::string(suffix) + std::to_string(
                                  std::chrono::steady_clock::now().time_since_epoch().count());
    spirula::Sha256 hash;
    hash.update(reinterpret_cast<const uint8_t*>(seed.data()), seed.size());
    return "uuid:" + hash.hex().substr(0, 32);
}

#if !defined(_WIN32) && !defined(__linux__)
fs::path lock_path_for(const std::string& device) {
    spirula::Sha256 hash;
    hash.update(reinterpret_cast<const uint8_t*>(device.data()), device.size());
    return fs::path("/var/tmp") / ("spirula-device-" + hash.hex() + ".lock");
}
#endif

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--hold") {
        std::puts("ready");
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--try") {
        app::DeviceLease lease;
        std::string error;
        return lease.try_acquire(argv[2], error) ? 0 : 2;
    }

    const std::string device = unique_selector("contention");
    std::string error;

    app::DeviceLease empty;
    check(!empty.try_acquire(std::string{}, error), "empty selector is rejected");
    check(!empty.owns() && empty.device().empty() && error.find("empty") != std::string::npos,
          "empty selector leaves the lease unowned with an actionable error");

    app::DeviceLease owner;
    app::DeviceLease contender;
    check(owner.try_acquire(device, error), "first lease acquires the device");
    check(owner.owns() && owner.device() == device, "accessors report the owned device");
    check(!contender.try_acquire(device, error), "same-process contention is rejected");
    check(!contender.owns() && error.find("busy") != std::string::npos,
           "contention reports busy rather than a generic error");

    app::proc::ProcessOptions child;
    child.argv = {fs::absolute(argv[0]).u8string(), "--try", device};
    app::proc::ProcessResult child_result = app::proc::run_process(child);
    check(child_result.exit_code == 2,
          "another process cannot acquire an owned device");

    owner.release();
    check(!owner.owns() && owner.device().empty(), "release clears ownership");
    check(contender.try_acquire(device, error), "released device can be reacquired");
    contender.release();
    child_result = app::proc::run_process(child);
    check(child_result.exit_code == 0,
          "another process acquires a released device");

    check(owner.try_acquire(device, error), "inherited lease test acquires the device");
    app::proc::ProcessOptions inherited;
    inherited.argv = {fs::absolute(argv[0]).u8string(), "--hold"};
#ifdef _WIN32
    inherited.inherit_handles.push_back(owner.native_handle());
#else
    inherited.inherit_fds.push_back(owner.native_fd());
#endif
    std::atomic<bool> child_ready{false};
    inherited.on_line = [&](const std::string& line) {
        if (line == "ready") child_ready.store(true);
    };
    app::proc::ProcessResult inherited_result;
    std::thread inherited_thread([&] {
        inherited_result = app::proc::run_process(inherited);
    });
    for (int i = 0; i < 200 && !child_ready.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(child_ready.load(), "child inherits the device lease");
    owner.release();
    check(!contender.try_acquire(device, error),
          "inherited lease survives parent release");
    inherited_thread.join();
    check(inherited_result.exit_code == 0,
          "inherited lease holder exits cleanly");
    check(contender.try_acquire(device, error),
          "device is released after inherited holder exits");
    contender.release();

    app::DeviceLease source;
    check(source.try_acquire(device, error), "move test acquires the device");
    app::DeviceLease moved(std::move(source));
    check(!source.owns() && source.device().empty() && moved.owns() && moved.device() == device,
          "move construction transfers ownership");
    app::DeviceLease assigned;
    assigned = std::move(moved);
    check(!moved.owns() && moved.device().empty() && assigned.owns() &&
              assigned.device() == device,
          "move assignment transfers ownership");
    assigned.release();

#ifndef _WIN32
    const int saved_stdin = dup(STDIN_FILENO);
    if (saved_stdin >= 0) close(STDIN_FILENO);
    app::DeviceLease safe_descriptor;
    const std::string descriptor_device = unique_selector("descriptor");
    check(safe_descriptor.try_acquire(descriptor_device, error),
          "device lease acquires with closed standard input");
    check(safe_descriptor.owns() && safe_descriptor.native_fd() >= 3,
          "device lease avoids standard descriptors");
    safe_descriptor.release();
    if (saved_stdin >= 0) {
        dup2(saved_stdin, STDIN_FILENO);
        close(saved_stdin);
    }
#endif

#if !defined(_WIN32) && !defined(__linux__)
    const std::string symlink_device = unique_selector("symlink");
    const fs::path path = lock_path_for(symlink_device);
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (!ec && status.type() == fs::file_type::not_found) {
        fs::create_symlink("device-lease-target", path, ec);
        if (!ec) {
            app::DeviceLease symlink;
            check(!symlink.try_acquire(symlink_device, error),
                  "symlink lock path is rejected");
            check(!symlink.owns() && error.find("busy") == std::string::npos,
                  "symlink rejection is an error, not a busy result");
            fs::remove(path, ec);
        } else {
            std::printf("ok   symlink test skipped: %s\n", ec.message().c_str());
        }
    } else {
        std::printf("ok   symlink test skipped: predictable path already exists\n");
    }
#endif

    return failures == 0 ? 0 : 1;
}
