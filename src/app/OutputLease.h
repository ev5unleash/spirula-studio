#pragma once

#include <filesystem>
#include <string>

namespace app {

class OutputLease {
public:
    OutputLease() = default;
    ~OutputLease();

    OutputLease(const OutputLease&) = delete;
    OutputLease& operator=(const OutputLease&) = delete;

    bool acquire(const std::filesystem::path& output_dir, std::string& error);
    void release();
    bool valid() const;
#ifdef _WIN32
    void* native_handle() const;
#else
    int native_fd() const;
#endif

private:
#ifdef _WIN32
    void* _handle = nullptr;
#else
    int _fd = -1;
#endif
};

}  // namespace app
