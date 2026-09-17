#include "app/OutputLease.h"

#include <cerrno>
#include <cstring>
#include <system_error>

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

namespace app {

OutputLease::~OutputLease() { release(); }

bool OutputLease::acquire(const fs::path& output_dir, std::string& error) {
    release();
    std::error_code ec;
    fs::create_directories(output_dir, ec);
    if (ec) {
        error = "cannot create output directory " + output_dir.u8string() +
                ": " + ec.message();
        return false;
    }

    const fs::path lock_path = output_dir / ".spirula-output.lock";
#ifdef _WIN32
    HANDLE handle = CreateFileW(lock_path.wstring().c_str(),
                                GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = "output directory is already in use: " + lock_path.u8string();
        return false;
    }
    _handle = handle;
#else
    int fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        error = "cannot open output lock " + lock_path.u8string() + ": " +
                std::strerror(errno);
        return false;
    }
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        const int saved_errno = errno;
        ::close(fd);
        error = "cannot protect output lock " + lock_path.u8string() + ": " +
                std::strerror(saved_errno);
        return false;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        error = "output directory is already in use: " + lock_path.u8string();
        return false;
    }
    if (fd < 3) {
        const int safe_fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        if (safe_fd < 0) {
            const int saved_errno = errno;
            ::close(fd);
            error = "cannot duplicate output lock " + lock_path.u8string() + ": " +
                    std::strerror(saved_errno);
            return false;
        }
        ::close(fd);
        fd = safe_fd;
    }
    _fd = fd;
#endif
    return true;
}

void OutputLease::release() {
#ifdef _WIN32
    if (_handle) {
        CloseHandle(static_cast<HANDLE>(_handle));
        _handle = nullptr;
    }
#else
    if (_fd >= 0) {
        ::close(_fd);
        _fd = -1;
    }
#endif
}

bool OutputLease::valid() const {
#ifdef _WIN32
    return _handle != nullptr;
#else
    return _fd >= 0;
#endif
}

#ifdef _WIN32
void* OutputLease::native_handle() const { return _handle; }
#else
int OutputLease::native_fd() const { return _fd; }
#endif

}  // namespace app
