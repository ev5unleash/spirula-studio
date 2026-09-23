#pragma once

#include <string>

namespace app {

class DeviceLease {
public:
    DeviceLease() = default;
    ~DeviceLease();

    DeviceLease(const DeviceLease&) = delete;
    DeviceLease& operator=(const DeviceLease&) = delete;
    DeviceLease(DeviceLease&& other) noexcept;
    DeviceLease& operator=(DeviceLease&& other) noexcept;

    bool try_acquire(const std::string& device, std::string& error);
    void release() noexcept;
    bool owns() const noexcept;
    const std::string& device() const noexcept;
#ifdef _WIN32
    void* native_handle() const noexcept;
#else
    int native_fd() const noexcept;
#endif

private:
#ifdef _WIN32
    void* _handle = nullptr;
#else
    int _fd = -1;
#endif
    std::string _device;
};

}  // namespace app
