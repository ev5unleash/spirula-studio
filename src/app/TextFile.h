#pragma once

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#ifndef _WIN32
#include <cerrno>
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace app {

// Preserves attributes when replacing a hidden Windows bookkeeping file.
inline std::string write_text_file(const std::filesystem::path& path,
                                   std::string_view text) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    const DWORD flags = attributes == INVALID_FILE_ATTRIBUTES
                            ? FILE_ATTRIBUTE_NORMAL
                            : attributes;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, flags, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return path.u8string() + ": " +
               std::system_category().message(GetLastError());

    size_t offset = 0;
    while (offset < text.size()) {
        const DWORD count = static_cast<DWORD>(
            std::min<size_t>(text.size() - offset, MAXDWORD));
        DWORD written = 0;
        if (!WriteFile(file, text.data() + offset, count, &written, nullptr) ||
            written != count) {
            const DWORD error = GetLastError();
            CloseHandle(file);
            return path.u8string() + ": " +
                   std::system_category().message(error);
        }
        offset += written;
    }
    if (!CloseHandle(file))
        return path.u8string() + ": " +
               std::system_category().message(GetLastError());
    return {};
#else
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file)
        return path.u8string() + ": " +
               std::system_category().message(errno);
    const size_t written = std::fwrite(text.data(), 1, text.size(), file);
    bool ok = written == text.size() && std::ferror(file) == 0;
    if (std::fclose(file) != 0) ok = false;
    return ok ? std::string() : path.u8string() + ": write failed";
#endif
}

}  // namespace app
