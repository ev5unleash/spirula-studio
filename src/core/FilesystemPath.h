#pragma once

#include <filesystem>
#include <system_error>
#include <string>

namespace spirula {

inline std::filesystem::path NativeFilesystemPath(const std::filesystem::path& path) {
#ifdef _WIN32
    if (!path.is_absolute()) return path;
    auto normalized = path;
    normalized.make_preferred();
    const std::wstring& native = normalized.native();
    if (native.rfind(L"\\\\?\\", 0) == 0) return normalized;
    if (native.rfind(L"\\\\", 0) == 0)
        return std::filesystem::path(L"\\\\?\\UNC\\" + native.substr(2));
    return std::filesystem::path(L"\\\\?\\" + native);
#else
    return path;
#endif
}

inline std::filesystem::path LogicalFilesystemPath(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::wstring& native = path.native();
    if (native.rfind(L"\\\\?\\UNC\\", 0) == 0)
        return std::filesystem::path(L"\\\\" + native.substr(8));
    if (native.rfind(L"\\\\?\\", 0) == 0)
        return std::filesystem::path(native.substr(4));
#endif
    return path;
}

inline std::filesystem::path LogicalAbsoluteFilesystemPath(
    const std::filesystem::path& path, std::error_code& ec) {
    return LogicalFilesystemPath(
        std::filesystem::absolute(NativeFilesystemPath(path), ec));
}

inline std::filesystem::path LogicalAbsoluteFilesystemPath(
    const std::filesystem::path& path) {
    return LogicalFilesystemPath(
        std::filesystem::absolute(NativeFilesystemPath(path)));
}

}  // namespace spirula
