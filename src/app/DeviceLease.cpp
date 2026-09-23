#include "app/DeviceLease.h"

#include "core/Sha256.h"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#ifdef __linux__
#include <sys/socket.h>
#include <sys/un.h>
#endif
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

constexpr char kLockPrefix[] = "spirula-device-";

std::string device_hash(const std::string& device) {
    spirula::Sha256 hash;
    hash.update(reinterpret_cast<const uint8_t*>(device.data()), device.size());
    return hash.hex();
}

std::string busy_error(const std::string& device) {
    return "device lease busy: selector '" + device +
           "' is already owned by another process";
}

#if !defined(_WIN32) && !defined(__linux__)
std::string system_error_text(int code) {
    return std::strerror(code);
}

std::filesystem::path lock_path(const std::string& hash) {
    return std::filesystem::path("/var/tmp") /
           (std::string(kLockPrefix) + hash + ".lock");
}
#endif

}  // namespace

namespace app {

DeviceLease::~DeviceLease() { release(); }

#ifdef _WIN32
DeviceLease::DeviceLease(DeviceLease&& other) noexcept
    : _handle(other._handle), _device(std::move(other._device)) {
    other._handle = nullptr;
    other._device.clear();
}
#else
DeviceLease::DeviceLease(DeviceLease&& other) noexcept
    : _fd(other._fd), _device(std::move(other._device)) {
    other._fd = -1;
    other._device.clear();
}
#endif

DeviceLease& DeviceLease::operator=(DeviceLease&& other) noexcept {
    if (this == &other) return *this;
    release();
#ifdef _WIN32
    _handle = other._handle;
    other._handle = nullptr;
#else
    _fd = other._fd;
    other._fd = -1;
#endif
    _device = std::move(other._device);
    other._device.clear();
    return *this;
}

bool DeviceLease::try_acquire(const std::string& device, std::string& error) {
    error.clear();
    if (device.empty()) {
        error = "device lease error: device selector is empty";
        return false;
    }

    release();
    const std::string hash = device_hash(device);

#ifdef _WIN32
    const std::string suffix = std::string(kLockPrefix) + hash;
    std::wstring name = L"Global\\";
    for (const char c : suffix) name.push_back(static_cast<wchar_t>(c));

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    constexpr wchar_t kMutexSddl[] =
        L"D:P(A;;0x00100000;;;AU)(A;;GA;;;SY)(A;;GA;;;BA)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            kMutexSddl, SDDL_REVISION_1, &descriptor, nullptr)) {
        const DWORD code = GetLastError();
        error = "device lease error: cannot create mutex security descriptor: " +
                std::system_category().message((int)code);
        return false;
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    HANDLE handle = CreateMutexExW(&attributes, name.c_str(), 0, SYNCHRONIZE);
    const DWORD code = GetLastError();
    LocalFree(descriptor);
    if (!handle) {
        error = "device lease error: cannot create Global mutex for selector '" +
                device + "': " + std::system_category().message((int)code);
        return false;
    }
    if (code == ERROR_ALREADY_EXISTS) {
        CloseHandle(handle);
        error = busy_error(device);
        return false;
    }
    _handle = handle;
#elif defined(__linux__)
    int socket_type = SOCK_DGRAM;
#ifdef SOCK_CLOEXEC
    socket_type |= SOCK_CLOEXEC;
#endif
    const int fd = ::socket(AF_UNIX, socket_type, 0);
    if (fd < 0) {
        const int code = errno;
        error = "device lease error: cannot create local socket: " +
                std::string(std::strerror(code));
        return false;
    }
    struct sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string socket_name = std::string(kLockPrefix) + hash;
    if (socket_name.size() + 1 > sizeof(address.sun_path)) {
        ::close(fd);
        error = "device lease error: local socket name is too long";
        return false;
    }
    std::memcpy(address.sun_path + 1, socket_name.data(), socket_name.size());
    const socklen_t address_size = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + 1 + socket_name.size());
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), address_size) != 0) {
        const int code = errno;
        ::close(fd);
        if (code == EADDRINUSE) {
            error = busy_error(device);
        } else {
            error = "device lease error: cannot bind local socket: " +
                    std::string(std::strerror(code));
        }
        return false;
    }
    const int descriptor_flags = ::fcntl(fd, F_GETFD);
    if (descriptor_flags < 0 ||
        ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
        const int code = errno;
        ::close(fd);
        error = "device lease error: cannot protect local socket: " +
                std::string(std::strerror(code));
        return false;
    }
    if (fd < 3) {
        const int safe_fd = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
        if (safe_fd < 0) {
            const int code = errno;
            ::close(fd);
            error = "device lease error: cannot duplicate local socket: " +
                    std::string(std::strerror(code));
            return false;
        }
        ::close(fd);
        fd = safe_fd;
    }
    _fd = fd;
#else
    namespace fs = std::filesystem;
    const fs::path path = lock_path(hash);

    struct stat existing{};
    if (::lstat(path.c_str(), &existing) == 0) {
        if (S_ISLNK(existing.st_mode)) {
            error = "device lease error: lock path is a symlink: " +
                    path.string();
            return false;
        }
        if (!S_ISREG(existing.st_mode)) {
            error = "device lease error: lock path is not a regular file: " +
                    path.string();
            return false;
        }
    } else if (errno != ENOENT) {
        const int code = errno;
        error = "device lease error: cannot inspect lock path " + path.string() +
                ": " + system_error_text(code);
        return false;
    }

    int flags = O_CREAT | O_EXCL | O_RDWR;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = ::open(path.c_str(), flags, 0666);
    bool created = fd >= 0;
    if (fd < 0 && errno == EEXIST) {
        flags &= ~(O_CREAT | O_EXCL);
        fd = ::open(path.c_str(), flags);
    }
    if (fd < 0) {
        const int code = errno;
        error = "device lease error: cannot open lock path " + path.string() +
                ": " + system_error_text(code);
        return false;
    }
    if (created && ::fchmod(fd, 0666) != 0) {
        const int code = errno;
        ::close(fd);
        error = "device lease error: cannot share lock file " + path.string() +
                ": " + system_error_text(code);
        return false;
    }

    struct stat status{};
    if (::fstat(fd, &status) != 0) {
        const int code = errno;
        ::close(fd);
        error = "device lease error: cannot inspect lock file " + path.string() +
                ": " + system_error_text(code);
        return false;
    }
    if (!S_ISREG(status.st_mode)) {
        ::close(fd);
        error = "device lease error: lock path is not a regular file: " +
                path.string();
        return false;
    }
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        const int code = errno;
        ::close(fd);
        error = "device lease error: cannot protect lock file " + path.string() +
                ": " + system_error_text(code);
        return false;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int code = errno;
        ::close(fd);
        if (code == EWOULDBLOCK || code == EAGAIN) {
            error = busy_error(device);
        } else {
            error = "device lease error: cannot lock " + path.string() + ": " +
                    system_error_text(code);
        }
        return false;
    }
    if (fd < 3) {
        const int safe_fd = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
        if (safe_fd < 0) {
            const int code = errno;
            ::close(fd);
            error = "device lease error: cannot duplicate lock file " +
                    path.string() + ": " + system_error_text(code);
            return false;
        }
        ::close(fd);
        fd = safe_fd;
    }
    _fd = fd;
#endif
    _device = device;
    return true;
}

void DeviceLease::release() noexcept {
#ifdef _WIN32
    if (_handle) {
        HANDLE handle = static_cast<HANDLE>(_handle);
        CloseHandle(handle);
        _handle = nullptr;
    }
#else
    if (_fd >= 0) {
        ::close(_fd);
        _fd = -1;
    }
#endif
    _device.clear();
}

bool DeviceLease::owns() const noexcept {
#ifdef _WIN32
    return _handle != nullptr;
#else
    return _fd >= 0;
#endif
}

const std::string& DeviceLease::device() const noexcept { return _device; }

#ifdef _WIN32
void* DeviceLease::native_handle() const noexcept { return _handle; }
#else
int DeviceLease::native_fd() const noexcept { return _fd; }
#endif

}  // namespace app
