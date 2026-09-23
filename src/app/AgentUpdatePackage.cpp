#include "app/AgentUpdatePackage.h"

#include "core/Sha256.h"
#include "app/AgentConfig.h"
#include "app/AgentPairing.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <ctime>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef SS_VERSION
#define SS_VERSION "unknown"
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace app::agent::update {
namespace {
namespace fs = std::filesystem;

constexpr std::size_t kCopyBytes = 64 * 1024;
std::atomic<std::uint64_t> g_temp_counter{0};

StageResult Failure(StageError error, const char* message) {
    return {error, message, {}};
}

bool IsAsciiAlnum(unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9');
}

bool ValidBuild(const std::string& value) {
    if (value.empty() || value.size() > 128 ||
        value.front() == ' ' || value.back() == ' ')
        return false;
    bool has_alnum = false;
    for (unsigned char ch : value) {
        if (IsAsciiAlnum(ch)) {
            has_alnum = true;
            continue;
        }
        if (ch != '.' && ch != '_' && ch != '+' && ch != '-' &&
            ch != '(' && ch != ')' && ch != ' ')
            return false;
    }
    return has_alnum;
}


bool ValidRelease(const std::string& value) {
    if (value.empty() || value.size() > 64 || !IsAsciiAlnum(value.front()))
        return false;
    for (unsigned char ch : value)
        if (!IsAsciiAlnum(ch) && ch != '.' && ch != '_' && ch != '+' && ch != '-')
            return false;
    return value.back() != '.';
}

bool ValidDigest(const std::string& value) {
    if (value.size() != 64) return false;
    for (char ch : value)
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
    return true;
}

bool ValidManifest(const PackageManifest& manifest) {
    const bool known_os = manifest.os == "windows" || manifest.os == "linux" ||
                          manifest.os == "macos";
    const bool known_arch = manifest.architecture == "x86_64" ||
                            manifest.architecture == "aarch64";
    return known_os && known_arch && ValidBuild(manifest.build) &&
           ValidRelease(manifest.release) && manifest.size != 0 &&
           manifest.size <= kMaxPackageBytes && ValidDigest(manifest.sha256);
}

bool ValidIdentity(std::string_view value) {
    if (value.size() != 64) return false;
    for (char ch : value)
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
    return true;
}

bool ValidUpdateId(std::string_view value) {
    if (value.empty() || value.size() > 64 || !IsAsciiAlnum(value.front()))
        return false;
    for (unsigned char ch : value)
        if (!IsAsciiAlnum(ch) && ch != '-' && ch != '_' && ch != '.')
            return false;
    return true;
}

bool AppendString(std::vector<std::uint8_t>& bytes, std::string_view value,
                  std::size_t maximum) {
    if (value.size() > maximum ||
        value.size() > std::numeric_limits<std::uint16_t>::max())
        return false;
    const auto length = static_cast<std::uint16_t>(value.size());
    bytes.push_back(static_cast<std::uint8_t>(length >> 8));
    bytes.push_back(static_cast<std::uint8_t>(length));
    for (unsigned char ch : value) bytes.push_back(ch);
    return true;
}

void AppendU64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

bool CanonicalOffer(const PackageOffer& offer,
                    std::vector<std::uint8_t>& bytes) {
    static constexpr char domain[] = "spirula-agent-update-offer-v1";
    bytes.clear();
    bytes.reserve(512);
    for (unsigned char ch : std::string_view(domain, sizeof(domain) - 1))
        bytes.push_back(ch);
    bytes.push_back(1);
    if (!AppendString(bytes, offer.worker_id, 64) ||
        !AppendString(bytes, offer.leader_id, 64))
        return false;
    AppendU64(bytes, offer.expires_at_unix);
    if (!AppendString(bytes, offer.update_id, 64) ||
        !AppendString(bytes, offer.manifest.os, 16) ||
        !AppendString(bytes, offer.manifest.architecture, 16) ||
        !AppendString(bytes, offer.manifest.build, 128) ||
        !AppendString(bytes, offer.manifest.release, 64))
        return false;
    AppendU64(bytes, offer.manifest.size);
    if (!AppendString(bytes, offer.manifest.sha256, 64)) return false;
    return bytes.size() <= 4096;
}

bool DecodeFingerprint(std::string_view hex,
                       app::agent::TlsChannel::PeerFingerprint& bytes) {
    if (hex.size() != 64) return false;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const int hi = digit(hex[i * 2]);
        const int lo = digit(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

std::string EncodeFingerprint(
    const app::agent::TlsChannel::PeerFingerprint& bytes) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string value(bytes.size() * 2, '0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        value[i * 2] = hex[bytes[i] >> 4];
        value[i * 2 + 1] = hex[bytes[i] & 15];
    }
    return value;
}
bool EqualFingerprint(
    const app::agent::TlsChannel::PeerFingerprint& left,
    const app::agent::TlsChannel::PeerFingerprint& right) {
    std::uint8_t difference = 0;
    for (std::size_t i = 0; i < left.size(); ++i)
        difference |= left[i] ^ right[i];
    return difference == 0;
}

bool IsSafePathComponent(const fs::path& component) {
    const auto& name = component.native();
    if (name.empty() || component == "." || component == ".." ||
        name.size() > 255 || name.back() == static_cast<fs::path::value_type>('.') ||
        name.back() == static_cast<fs::path::value_type>(' '))
        return false;
    std::string ascii;
    ascii.reserve(name.size());
    for (auto ch : name) {
        if (ch < 0 || ch > 0x7f) return false;
        const auto c = static_cast<unsigned char>(ch);
        if (!IsAsciiAlnum(c) && c != '.' && c != '_' && c != '-') return false;
        ascii.push_back(static_cast<char>(c));
    }
    std::string stem = ascii.substr(0, ascii.find('.'));
    for (char& ch : stem)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul")
        return false;
    return !(stem.size() == 4 &&
             (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) &&
             stem[3] >= '1' && stem[3] <= '9');
}

bool ValidRelativeFile(const fs::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() ||
        path.has_root_directory() || path.lexically_normal() != path ||
        path.native().size() > 4096)
        return false;
    bool has_component = false;
    for (const fs::path& component : path) {
        if (!IsSafePathComponent(component)) return false;
        has_component = true;
    }
    return has_component;
}

bool IsCanonicalAbsolute(const fs::path& path) {
    if (path.empty() || !path.is_absolute() || path == path.root_path() ||
        path.lexically_normal() != path)
        return false;
    for (const fs::path& component : path.relative_path())
        if (component.empty() || component == "." || component == "..")
            return false;
    return true;
}

#ifdef _WIN32

class NativeHandle final {
public:
    NativeHandle() = default;
    explicit NativeHandle(HANDLE value) : value_(value) {}
    ~NativeHandle() { Close(); }
    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;
    NativeHandle(NativeHandle&& other) noexcept : value_(other.release()) {}
    NativeHandle& operator=(NativeHandle&& other) noexcept {
        if (this != &other) {
            Close();
            value_ = other.release();
        }
        return *this;
    }
    HANDLE get() const { return value_; }
    bool valid() const { return value_ != INVALID_HANDLE_VALUE && value_ != nullptr; }
    HANDLE release() {
        HANDLE value = value_;
        value_ = INVALID_HANDLE_VALUE;
        return value;
    }
    void Close() {
        if (valid()) CloseHandle(value_);
        value_ = INVALID_HANDLE_VALUE;
    }
private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};


bool SameComponent(const fs::path& a, const fs::path& b) {
    const auto& left = a.native();
    const auto& right = b.native();
    return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
                                right.c_str(), static_cast<int>(right.size()),
                                TRUE) == CSTR_EQUAL;
}

bool IsPrefixPath(const fs::path& prefix, const fs::path& path) {
    auto left = prefix.begin();
    auto right = path.begin();
    for (; left != prefix.end(); ++left, ++right)
        if (right == path.end() || !SameComponent(*left, *right)) return false;
    return true;
}
// Staging appends a digest directory to storage, which may exceed legacy MAX_PATH.
std::wstring NativePath(const fs::path& path) {
    const std::wstring& native = path.native();
    if (native.rfind(L"\\\\?\\", 0) == 0) return native;
    if (native.rfind(L"\\\\", 0) == 0)
        return L"\\\\?\\UNC\\" + native.substr(2);
    return L"\\\\?\\" + native;
}

fs::path UsablePath(const fs::path& path) {
    return path.native().size() < MAX_PATH ? path : fs::path(NativePath(path));
}


bool PlainDirectoryPath(const fs::path& path) {
    if (!IsCanonicalAbsolute(path)) return false;
    fs::path current = path.root_path();
    std::wstring native = NativePath(current);
    DWORD attributes = GetFileAttributesW(native.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return false;
    for (const fs::path& part : path.relative_path()) {
        current /= part;
        native = NativePath(current);
        attributes = GetFileAttributesW(native.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
    }
    return true;
}


bool OpenDirectory(const fs::path& path, NativeHandle& handle) {
    if (!PlainDirectoryPath(path)) return false;
    const std::wstring native = NativePath(path);
    handle = NativeHandle(CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!handle.valid()) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    return GetFileInformationByHandle(handle.get(), &info) &&
           (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
           !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
}


bool FinalPath(HANDLE handle, std::wstring& result) {
    std::vector<wchar_t> buffer(32768);
    const DWORD size = GetFinalPathNameByHandleW(
        handle, buffer.data(), static_cast<DWORD>(buffer.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (size == 0 || size >= buffer.size()) return false;
    result.assign(buffer.data(), size);
    return true;
}

bool HandleWithin(HANDLE file, HANDLE root) {
    std::wstring file_path, root_path;
    if (!FinalPath(file, file_path) || !FinalPath(root, root_path)) return false;
    while (root_path.size() > 4 &&
           (root_path.back() == L'\\' || root_path.back() == L'/'))
        root_path.pop_back();
    if (file_path.size() <= root_path.size() ||
        file_path[root_path.size()] != L'\\')
        return false;
    return CompareStringOrdinal(file_path.data(), static_cast<int>(root_path.size()),
                                root_path.data(), static_cast<int>(root_path.size()),
                                TRUE) == CSTR_EQUAL;
}

bool OpenRegularFile(const fs::path& path, DWORD access, DWORD share,
                     NativeHandle& handle, BY_HANDLE_FILE_INFORMATION& info) {
    const std::wstring native = NativePath(path);
    handle = NativeHandle(CreateFileW(native.c_str(), access, share, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!handle.valid() || GetFileType(handle.get()) != FILE_TYPE_DISK ||
        !GetFileInformationByHandle(handle.get(), &info))
        return false;
    return !(info.dwFileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
           info.nNumberOfLinks == 1;
}


std::uint64_t FileSize(const BY_HANDLE_FILE_INFORMATION& info) {
    return (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) |
           info.nFileSizeLow;
}

bool HashHandle(HANDLE handle, std::uint64_t size, std::string& digest) {
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN)) return false;
    spirula::Sha256 hash;
    std::array<std::uint8_t, kCopyBytes> buffer{};
    std::uint64_t total = 0;
    while (total < size) {
        const DWORD want = static_cast<DWORD>(std::min<std::uint64_t>(
            buffer.size(), size - total));
        DWORD got = 0;
        if (!ReadFile(handle, buffer.data(), want, &got, nullptr) || got == 0)
            return false;
        hash.update(buffer.data(), got);
        total += got;
    }
    std::uint8_t extra = 0;
    DWORD got = 0;
    if (!ReadFile(handle, &extra, 1, &got, nullptr) || got != 0) return false;
    digest = hash.hex();
    return true;
}

bool CopyAndHash(HANDLE source, HANDLE output, std::uint64_t size,
                 std::string& digest, StageError& error) {
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(source, zero, nullptr, FILE_BEGIN) ||
        !SetFilePointerEx(output, zero, nullptr, FILE_BEGIN)) {
        error = StageError::Filesystem;
        return false;
    }
    spirula::Sha256 hash;
    std::array<std::uint8_t, kCopyBytes> buffer{};
    std::uint64_t total = 0;
    while (total < size) {
        const DWORD want = static_cast<DWORD>(std::min<std::uint64_t>(
            buffer.size(), size - total));
        DWORD got = 0;
        if (!ReadFile(source, buffer.data(), want, &got, nullptr) || got == 0) {
            error = StageError::Filesystem;
            return false;
        }
        DWORD written = 0;
        std::size_t offset = 0;
        while (offset < got) {
            if (!WriteFile(output, buffer.data() + offset,
                           static_cast<DWORD>(got - offset), &written, nullptr) ||
                written == 0) {
                const DWORD code = GetLastError();
                error = code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL
                    ? StageError::DiskSpace : StageError::Filesystem;
                return false;
            }
            offset += written;
        }
        hash.update(buffer.data(), got);
        total += got;
    }
    std::uint8_t extra = 0;
    DWORD got = 0;
    if (!ReadFile(source, &extra, 1, &got, nullptr) || got != 0) {
        error = StageError::Integrity;
        return false;
    }
    digest = hash.hex();
    return true;
}

bool EnsureDirectory(const fs::path& path) {
    if (!PlainDirectoryPath(path.parent_path())) return false;
    const std::wstring native = NativePath(path);
    if (!CreateDirectoryW(native.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    NativeHandle handle;
    return OpenDirectory(path, handle);
}


bool HasEnoughSpace(const fs::path& path, std::uint64_t bytes) {
    ULARGE_INTEGER available{}, total{}, free{};
    const std::wstring native = NativePath(path);
    return GetDiskFreeSpaceExW(native.c_str(), &available, &total, &free) &&
           available.QuadPart >= bytes;
}


StageResult CheckPublished(HANDLE directory_handle, const fs::path& target,
                           const PackageManifest& manifest, bool& exists) {
    exists = false;
    const std::wstring native = NativePath(target);
    const DWORD attributes = GetFileAttributesW(native.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return {};
        return Failure(StageError::Filesystem, "cannot inspect staged package path");
    }
    exists = true;
    NativeHandle file;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!OpenRegularFile(target, GENERIC_READ, FILE_SHARE_READ, file, info) ||
        !(info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ||
        FileSize(info) != manifest.size ||
        !HandleWithin(file.get(), directory_handle))
        return Failure(StageError::Conflict, "staged package path is unsafe or conflicts");
    std::string digest;
    if (!HashHandle(file.get(), manifest.size, digest) || digest != manifest.sha256)
        return Failure(StageError::Conflict, "staged package path contains different bytes");
    return {};
}



StageResult StageNative(const service::Configuration& configuration,
                        const fs::path& package_root,
                        const fs::path& relative,
                        const PackageManifest& manifest) {
    const fs::path& storage = configuration.storage_root;
    const fs::path& executable = configuration.executable;
    if (!IsCanonicalAbsolute(storage) || !IsCanonicalAbsolute(executable) ||
        !PlainDirectoryPath(storage) ||
        !PlainDirectoryPath(executable.parent_path()))
        return Failure(StageError::InvalidArgument, "service paths are not safe canonical paths");
    if (IsPrefixPath(storage, executable.parent_path()) ||
        IsPrefixPath(executable.parent_path(), storage))
        return Failure(StageError::InvalidArgument, "storage and executable paths overlap");
    if (GetDriveTypeW(storage.root_path().c_str()) == DRIVE_REMOTE)
        return Failure(StageError::InvalidArgument, "service storage must be on a local volume");

    NativeHandle known_good;
    BY_HANDLE_FILE_INFORMATION known_good_info{};
    if (!OpenRegularFile(executable, GENERIC_READ, FILE_SHARE_READ, known_good,
                         known_good_info))
        return Failure(StageError::InvalidArgument, "configured executable is unsafe");
    NativeHandle executable_dir;
    if (!OpenDirectory(executable.parent_path(), executable_dir) ||
        !HandleWithin(known_good.get(), executable_dir.get()))
        return Failure(StageError::InvalidArgument, "configured executable path escapes its directory");

    NativeHandle source_dir;
    if (!IsCanonicalAbsolute(package_root) || !OpenDirectory(package_root, source_dir))
        return Failure(StageError::InvalidArgument, "package root is not a safe directory");
    const fs::path source_path = package_root / relative;
    if (!PlainDirectoryPath(source_path.parent_path()))
        return Failure(StageError::Filesystem, "package path contains an unsafe directory");
    NativeHandle source;
    BY_HANDLE_FILE_INFORMATION source_info{};
    if (!OpenRegularFile(source_path, GENERIC_READ, FILE_SHARE_READ, source,
                         source_info))
        return Failure(StageError::Filesystem, "package is not a single-linked regular file");
    if (FileSize(source_info) != manifest.size)
        return Failure(StageError::Integrity, "package size does not match manifest");
    if (!HandleWithin(source.get(), source_dir.get()))
        return Failure(StageError::Filesystem, "package path escapes its root");
    std::string source_digest;
    if (!HashHandle(source.get(), manifest.size, source_digest) ||
        source_digest != manifest.sha256)
        return Failure(StageError::Integrity, "package digest does not match manifest");

    const fs::path update_dir = storage / L"agent-updates";
    if (!EnsureDirectory(update_dir))
        return Failure(StageError::Filesystem, "cannot create safe update directory");
    const fs::path stage_dir = update_dir / std::wstring(
        manifest.sha256.begin(), manifest.sha256.end());
    if (!EnsureDirectory(stage_dir))
        return Failure(StageError::Filesystem, "cannot create safe package directory");
    NativeHandle stage_handle;
    if (!OpenDirectory(stage_dir, stage_handle))
        return Failure(StageError::Filesystem, "cannot open package directory");
    const fs::path target = stage_dir / L"spirula.exe";
    bool exists = false;
    StageResult published = CheckPublished(stage_handle.get(), target,
                                           manifest, exists);

    if (exists || !published) {
        if (!published) return published;
        published.staged_executable = UsablePath(target);
        return published;
    }
    if (!HasEnoughSpace(stage_dir, manifest.size))
        return Failure(StageError::DiskSpace, "insufficient space for staged package");

    fs::path temporary;
    NativeHandle output;
    for (unsigned attempt = 0; attempt != 128; ++attempt) {
        temporary = stage_dir / (L".update-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(g_temp_counter.fetch_add(1)) + L".tmp");
        output = NativeHandle(CreateFileW(NativePath(temporary).c_str(),
            GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                FILE_FLAG_WRITE_THROUGH,
            nullptr));
        if (output.valid()) break;
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
            return Failure(GetLastError() == ERROR_DISK_FULL ||
                           GetLastError() == ERROR_HANDLE_DISK_FULL
                               ? StageError::DiskSpace : StageError::Filesystem,
                           "cannot create staged temporary file");
    }
    if (!output.valid())
        return Failure(StageError::Filesystem, "cannot reserve staged temporary file");

    struct TempCleanup {
        const fs::path& path;
        NativeHandle& file;
        bool keep = false;
        ~TempCleanup() {
            file.Close();
            if (!keep) {
                const std::wstring native = NativePath(path);
                SetFileAttributesW(native.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(native.c_str());
            }
        }
    } cleanup{temporary, output};

    std::string copied_digest;
    StageError copy_error = StageError::Filesystem;
    if (!CopyAndHash(source.get(), output.get(), manifest.size, copied_digest,
                     copy_error))
        return Failure(copy_error, "cannot safely copy package");
    if (copied_digest != manifest.sha256)
        return Failure(StageError::Integrity, "package changed while staging");
    BY_HANDLE_FILE_INFORMATION source_after{};
    if (!GetFileInformationByHandle(source.get(), &source_after) ||
        source_after.nNumberOfLinks != 1 ||
        FileSize(source_after) != manifest.size ||
        source_after.dwVolumeSerialNumber != source_info.dwVolumeSerialNumber ||
        source_after.nFileIndexHigh != source_info.nFileIndexHigh ||
        source_after.nFileIndexLow != source_info.nFileIndexLow ||
        !HandleWithin(source.get(), source_dir.get()))
        return Failure(StageError::Integrity, "package changed while staging");


    if (!FlushFileBuffers(output.get()))
        return Failure(GetLastError() == ERROR_DISK_FULL ||
                       GetLastError() == ERROR_HANDLE_DISK_FULL
                           ? StageError::DiskSpace : StageError::Filesystem,
                       "cannot flush staged package");
    std::string staged_digest;
    if (!HashHandle(output.get(), manifest.size, staged_digest) ||
        staged_digest != manifest.sha256)
        return Failure(StageError::Integrity, "staged package rehash failed");
    const std::wstring native_temporary = NativePath(temporary);
    const std::wstring native_target = NativePath(target);
    if (!SetFileAttributesW(native_temporary.c_str(), FILE_ATTRIBUTE_READONLY))
        return Failure(StageError::Filesystem, "cannot protect staged package");
    output.Close();
    if (!MoveFileExW(native_temporary.c_str(), native_target.c_str(),
                     MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        SetFileAttributesW(native_temporary.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(native_temporary.c_str());
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
            published = CheckPublished(stage_handle.get(), target, manifest, exists);

            if (published && exists) {
                published.staged_executable = UsablePath(target);
                return published;
            }
            if (!published) return published;
            return Failure(StageError::Conflict, "staged package path conflicts");
        }
        return Failure(error == ERROR_DISK_FULL || error == ERROR_HANDLE_DISK_FULL
                           ? StageError::DiskSpace : StageError::Filesystem,
                       "cannot atomically publish staged package");
    }
    cleanup.keep = true;
    published = CheckPublished(stage_handle.get(), target, manifest, exists);

    if (!published || !exists) {
        SetFileAttributesW(native_target.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(native_target.c_str());
        return !published ? published
                          : Failure(StageError::Filesystem, "published package disappeared");
    }
    published.staged_executable = UsablePath(target);
    return published;
}

#else

class NativeFd final {
public:
    NativeFd() = default;
    explicit NativeFd(int value) : value_(value) {}
    ~NativeFd() { Close(); }
    NativeFd(const NativeFd&) = delete;
    NativeFd& operator=(const NativeFd&) = delete;
    NativeFd(NativeFd&& other) noexcept : value_(other.release()) {}
    NativeFd& operator=(NativeFd&& other) noexcept {
        if (this != &other) {
            Close();
            value_ = other.release();
        }
        return *this;
    }
    int get() const { return value_; }
    bool valid() const { return value_ >= 0; }
    int release() {
        const int value = value_;
        value_ = -1;
        return value;
    }
    void Close() {
        if (value_ >= 0) close(value_);
        value_ = -1;
    }
private:
    int value_ = -1;
};

bool SameComponent(const fs::path& a, const fs::path& b) {
    return a == b;
}

bool IsPrefixPath(const fs::path& prefix, const fs::path& path) {
    auto left = prefix.begin();
    auto right = path.begin();
    for (; left != prefix.end(); ++left, ++right)
        if (right == path.end() || !SameComponent(*left, *right)) return false;
    return true;
}

bool OpenDirectoryPath(const fs::path& path, NativeFd& result) {
    if (!IsCanonicalAbsolute(path)) return false;
    NativeFd current(open(path.root_path().c_str(), O_RDONLY | O_DIRECTORY |
                          O_CLOEXEC | O_NOFOLLOW));
    if (!current.valid()) return false;
    for (const fs::path& part : path.relative_path()) {
        if (part.empty() || part == "." || part == "..") return false;

        NativeFd next(openat(current.get(), part.c_str(), O_RDONLY | O_DIRECTORY |
                             O_CLOEXEC | O_NOFOLLOW));
        if (!next.valid()) return false;
        current = std::move(next);
    }
    struct stat info{};
    if (fstat(current.get(), &info) != 0 || !S_ISDIR(info.st_mode)) return false;
    result = std::move(current);
    return true;
}

bool OpenRelativeDirectory(int root, const fs::path& relative, NativeFd& result) {
    if (relative.is_absolute() || relative.has_root_name() ||
        relative.has_root_directory() || relative.lexically_normal() != relative)
        return false;
    NativeFd current(openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                            O_NOFOLLOW));
    if (!current.valid()) return false;
    for (const fs::path& part : relative) {
        if (!IsSafePathComponent(part)) return false;
        NativeFd next(openat(current.get(), part.c_str(), O_RDONLY | O_DIRECTORY |
                             O_CLOEXEC | O_NOFOLLOW));
        if (!next.valid()) return false;
        current = std::move(next);
    }
    struct stat info{};
    if (fstat(current.get(), &info) != 0 || !S_ISDIR(info.st_mode)) return false;
    result = std::move(current);
    return true;
}

bool OpenRegularAt(int directory, const fs::path& name, int flags,
                   NativeFd& result, struct stat& info) {
    result = NativeFd(openat(directory, name.c_str(), flags | O_CLOEXEC | O_NOFOLLOW));
    return result.valid() && fstat(result.get(), &info) == 0 &&
           S_ISREG(info.st_mode) && info.st_nlink == 1;
}


bool OwnedPrivateDirectory(int fd, bool require_write) {
    struct stat info{};
    if (fstat(fd, &info) != 0 || !S_ISDIR(info.st_mode) ||
        info.st_uid != geteuid() || (info.st_mode & 0077) != 0)
        return false;
    const mode_t required = require_write ? 0700 : 0500;
    return (info.st_mode & required) == required;
}

bool EnsureDirectoryAt(int parent, const char* name, NativeFd& result) {
    if (mkdirat(parent, name, 0700) != 0 && errno != EEXIST) return false;
    result = NativeFd(openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                             O_NOFOLLOW));
    return result.valid() && OwnedPrivateDirectory(result.get(), true);
}

bool HashFd(int fd, std::uint64_t size, std::string& digest) {
    spirula::Sha256 hash;
    std::array<std::uint8_t, kCopyBytes> buffer{};
    std::uint64_t total = 0;
    while (total < size) {
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(
            buffer.size(), size - total));
        ssize_t got;
        do {
            got = pread(fd, buffer.data(), want, static_cast<off_t>(total));
        } while (got < 0 && errno == EINTR);
        if (got <= 0) return false;
        hash.update(buffer.data(), static_cast<std::size_t>(got));
        total += static_cast<std::uint64_t>(got);
    }
    std::uint8_t extra = 0;
    ssize_t got;
    do {
        got = pread(fd, &extra, 1, static_cast<off_t>(size));
    } while (got < 0 && errno == EINTR);
    if (got != 0) return false;
    digest = hash.hex();
    return true;
}

bool AvailableBytes(int directory, std::uint64_t bytes) {
    struct statvfs info{};
    if (fstatvfs(directory, &info) != 0) return false;
    const std::uint64_t available = info.f_frsize != 0 &&
        static_cast<std::uint64_t>(info.f_bavail) >
            std::numeric_limits<std::uint64_t>::max() /
                static_cast<std::uint64_t>(info.f_frsize)
        ? std::numeric_limits<std::uint64_t>::max()
        : static_cast<std::uint64_t>(info.f_bavail) *
              static_cast<std::uint64_t>(info.f_frsize);
    return available >= bytes;
}

StageResult CheckPublished(int directory, const char* target,
                           const PackageManifest& manifest, bool& exists) {
    exists = false;
    struct stat info{};
    if (fstatat(directory, target, &info, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return {};
        return Failure(StageError::Filesystem, "cannot inspect staged package path");
    }
    exists = true;
    if (!S_ISREG(info.st_mode) || info.st_nlink != 1 ||
        info.st_uid != geteuid() || (info.st_mode & 0077) != 0 ||
        (info.st_mode & 0500) != 0500 ||
        static_cast<std::uint64_t>(info.st_size) != manifest.size)
        return Failure(StageError::Conflict, "staged package path is unsafe or conflicts");
    NativeFd file;
    if (!OpenRegularAt(directory, target, O_RDONLY, file, info) ||
        info.st_uid != geteuid() || (info.st_mode & 0077) != 0 ||
        (info.st_mode & 0500) != 0500 ||
        static_cast<std::uint64_t>(info.st_size) != manifest.size)
        return Failure(StageError::Conflict, "staged package path is unsafe or conflicts");

    std::string digest;
    if (!HashFd(file.get(), manifest.size, digest) || digest != manifest.sha256)
        return Failure(StageError::Conflict, "staged package path contains different bytes");
    return {};
}

StageResult StageNative(const service::Configuration& configuration,
                        const fs::path& package_root,
                        const fs::path& relative,
                        const PackageManifest& manifest) {
    const fs::path& storage = configuration.storage_root;
    const fs::path& executable = configuration.executable;
    if (!IsCanonicalAbsolute(storage) || !IsCanonicalAbsolute(executable))
        return Failure(StageError::InvalidArgument, "service paths are not canonical absolute paths");
    NativeFd storage_fd, executable_dir, known_good;
    struct stat known_good_info{};
    if (!OpenDirectoryPath(storage, storage_fd) ||
        !OpenDirectoryPath(executable.parent_path(), executable_dir) ||
        !OpenRegularAt(executable_dir.get(), executable.filename(), O_RDONLY,
                       known_good, known_good_info))
        return Failure(StageError::InvalidArgument, "configured service paths are unsafe");
    if (IsPrefixPath(storage, executable.parent_path()) ||
        IsPrefixPath(executable.parent_path(), storage))
        return Failure(StageError::InvalidArgument, "storage and executable paths overlap");

    NativeFd source_root_fd;
    if (!OpenDirectoryPath(package_root, source_root_fd))
        return Failure(StageError::InvalidArgument, "package root is not a safe directory");
    NativeFd source_parent;
    if (!OpenRelativeDirectory(source_root_fd.get(), relative.parent_path(),
                               source_parent))
        return Failure(StageError::Filesystem, "package path contains an unsafe directory");
    NativeFd source;
    struct stat source_info{};
    if (!OpenRegularAt(source_parent.get(), relative.filename(), O_RDONLY,
                       source, source_info))
        return Failure(StageError::Filesystem, "package is not a single-linked regular file");

    if (source_info.st_size < 0 ||
        static_cast<std::uint64_t>(source_info.st_size) != manifest.size)
        return Failure(StageError::Integrity, "package size does not match manifest");
    std::string source_digest;
    if (!HashFd(source.get(), manifest.size, source_digest) ||
        source_digest != manifest.sha256)
        return Failure(StageError::Integrity, "package digest does not match manifest");

    NativeFd update_dir;
    if (!EnsureDirectoryAt(storage_fd.get(), "agent-updates", update_dir))
        return Failure(StageError::Filesystem, "cannot create safe update directory");
    NativeFd stage_dir;
    if (!EnsureDirectoryAt(update_dir.get(), manifest.sha256.c_str(), stage_dir))
        return Failure(StageError::Filesystem, "cannot create safe package directory");
    const char* target = "spirula";
    bool exists = false;
    StageResult published = CheckPublished(stage_dir.get(), target, manifest, exists);
    if (exists || !published) {
        if (!published) return published;
        published.staged_executable = storage / "agent-updates" /
                                      manifest.sha256 / target;
        return published;
    }
    if (!AvailableBytes(stage_dir.get(), manifest.size))
        return Failure(StageError::DiskSpace, "insufficient space for staged package");

    NativeFd temporary;
    std::string temporary_name;
    for (unsigned attempt = 0; attempt != 128; ++attempt) {
        temporary_name = ".update-" + std::to_string(static_cast<unsigned long long>(getpid())) +
            "-" + std::to_string(g_temp_counter.fetch_add(1)) + ".tmp";
        temporary = NativeFd(openat(stage_dir.get(), temporary_name.c_str(),
            O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
        if (temporary.valid()) break;
        if (errno != EEXIST)
            return Failure(errno == ENOSPC || errno == EDQUOT
                               ? StageError::DiskSpace : StageError::Filesystem,
                           "cannot create staged temporary file");
    }
    if (!temporary.valid())
        return Failure(StageError::Filesystem, "cannot reserve staged temporary file");
    struct TempCleanup {
        int directory;
        const std::string& name;
        bool keep = false;
        ~TempCleanup() { if (!keep) unlinkat(directory, name.c_str(), 0); }
    } cleanup{stage_dir.get(), temporary_name};

    spirula::Sha256 copy_hash;
    std::array<std::uint8_t, kCopyBytes> buffer{};
    std::uint64_t total = 0;
    while (total < manifest.size) {
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(
            buffer.size(), manifest.size - total));
        ssize_t got;
        do {
            got = pread(source.get(), buffer.data(), want, static_cast<off_t>(total));
        } while (got < 0 && errno == EINTR);
        if (got <= 0)
            return Failure(StageError::Filesystem, "cannot read package while staging");
        copy_hash.update(buffer.data(), static_cast<std::size_t>(got));
        std::size_t written_total = 0;
        while (written_total < static_cast<std::size_t>(got)) {
            ssize_t written;
            do {
                written = write(temporary.get(), buffer.data() + written_total,
                                static_cast<std::size_t>(got) - written_total);
            } while (written < 0 && errno == EINTR);
            if (written <= 0)
                return Failure(errno == ENOSPC || errno == EDQUOT
                                   ? StageError::DiskSpace : StageError::Filesystem,
                               "cannot write staged package");
            written_total += static_cast<std::size_t>(written);
        }
        total += static_cast<std::uint64_t>(got);
    }
    std::uint8_t extra = 0;
    ssize_t extra_count;
    do {
        extra_count = pread(source.get(), &extra, 1,
                            static_cast<off_t>(manifest.size));
    } while (extra_count < 0 && errno == EINTR);
    if (extra_count != 0 || copy_hash.hex() != manifest.sha256)
        return Failure(StageError::Integrity, "package changed while staging");

    struct stat after_copy{};
    if (fstat(source.get(), &after_copy) != 0 ||
        after_copy.st_dev != source_info.st_dev ||
        after_copy.st_ino != source_info.st_ino || after_copy.st_nlink != 1 ||
        after_copy.st_size < 0 ||
        static_cast<std::uint64_t>(after_copy.st_size) != manifest.size)
        return Failure(StageError::Integrity, "package changed while staging");
    if (fchmod(temporary.get(), 0500) != 0 || fsync(temporary.get()) != 0)
        return Failure(errno == ENOSPC || errno == EDQUOT
                           ? StageError::DiskSpace : StageError::Filesystem,
                       "cannot flush staged package");
    struct stat staged_info{};
    if (fstat(temporary.get(), &staged_info) != 0 ||
        !S_ISREG(staged_info.st_mode) || staged_info.st_nlink != 1 ||
        staged_info.st_uid != geteuid() || staged_info.st_size < 0 ||
        static_cast<std::uint64_t>(staged_info.st_size) != manifest.size)
        return Failure(StageError::Filesystem, "staged temporary file is unsafe");
    std::string staged_digest;
    if (!HashFd(temporary.get(), manifest.size, staged_digest) ||
        staged_digest != manifest.sha256)
        return Failure(StageError::Integrity, "staged package rehash failed");

    if (linkat(stage_dir.get(), temporary_name.c_str(), stage_dir.get(),
               target, 0) != 0) {
        if (errno != EEXIST)
            return Failure(errno == ENOSPC || errno == EDQUOT
                               ? StageError::DiskSpace : StageError::Filesystem,
                           "cannot atomically publish staged package");
        published = CheckPublished(stage_dir.get(), target, manifest, exists);
        if (!published || !exists)
            return !published ? published
                              : Failure(StageError::Conflict, "staged package path conflicts");
        cleanup.keep = false;
        published.staged_executable = storage / "agent-updates" /
                                      manifest.sha256 / target;
        return published;
    }
    if (unlinkat(stage_dir.get(), temporary_name.c_str(), 0) != 0)
        return Failure(StageError::Filesystem, "cannot finalize staged package");
    cleanup.keep = true;
    fsync(stage_dir.get());
    published = CheckPublished(stage_dir.get(), target, manifest, exists);
    if (!published || !exists) {
        unlinkat(stage_dir.get(), target, 0);
        return !published ? published
                          : Failure(StageError::Filesystem, "published package disappeared");
    }
    published.staged_executable = storage / "agent-updates" /
                                  manifest.sha256 / target;
    return published;
}

#endif

}  // namespace

PlatformIdentity CurrentPlatformIdentity() {
    PlatformIdentity identity;
#ifdef _WIN32
    identity.os = "windows";
#elif defined(__APPLE__)
    identity.os = "macos";
#elif defined(__linux__)
    identity.os = "linux";
#else
    identity.os = "unknown";
#endif
#if defined(_M_X64) || defined(__x86_64__)
    identity.architecture = "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__) || defined(__arm64__)
    identity.architecture = "aarch64";
#else
    identity.architecture = "unknown";
#endif
    identity.build = SS_VERSION;
    return identity;
}

namespace detail {
struct OfferAuthority {
    static AuthorizedPackageOffer Create(const PackageOffer& offer,
                                         std::string signer_sha256) {
        return AuthorizedPackageOffer(offer.manifest, offer.update_id,
                                     offer.worker_id, offer.leader_id,
                                     std::move(signer_sha256),
                                     offer.expires_at_unix);
    }
};
}

std::optional<PackageOffer> SignPackageOffer(
    pairing::Leader& leader, const std::string& worker_id,
    std::uint64_t expires_at_unix, std::string update_id,
    PackageManifest manifest, std::string* error) {
    auto reject = [&](const char* message) -> std::optional<PackageOffer> {
        if (error) *error = message;
        return std::nullopt;
    };
    if (!ValidIdentity(worker_id) || !ValidUpdateId(update_id) ||
        !expires_at_unix || !ValidManifest(manifest))
        return reject("update offer fields are invalid");

    PackageOffer offer;
    offer.worker_id = worker_id;
    offer.leader_id = leader.LeaderId();
    offer.expires_at_unix = expires_at_unix;
    offer.update_id = std::move(update_id);
    offer.manifest = std::move(manifest);
    if (!ValidIdentity(offer.leader_id))
        return reject("leader identity is invalid");

    std::vector<std::uint8_t> canonical;
    if (!CanonicalOffer(offer, canonical))
        return reject("update offer cannot be canonically encoded");
    if (!leader.SignUpdateOffer(offer.worker_id, canonical, offer.signature,
                                offer.signer_public_key_pem, error))
        return std::nullopt;
    return offer;
}

std::optional<AuthorizedPackageOffer> AuthorizePackageOffer(
    const Config& local_policy, const pairing::Worker& paired_worker,
    const PackageOffer& offer,
    const std::vector<std::string>& consumed_update_ids,
    std::string* error) {
    auto reject = [&](const char* message)
        -> std::optional<AuthorizedPackageOffer> {
        if (error) *error = message;
        return std::nullopt;
    };
    if (!local_policy.allow_remote_update)
        return reject("remote update policy is disabled");
    app::agent::TlsChannel::PeerFingerprint approved_fingerprint{};
    if (!DecodeFingerprint(local_policy.update_signer_sha256,
                           approved_fingerprint))
        return reject("admin update signer pin is invalid");
    if (local_policy.update_signer_sha256 == local_policy.leader_id)
        return reject("update signer pin must differ from the paired leader identity");
    if (!ValidIdentity(local_policy.leader_id) ||
        !ValidIdentity(offer.worker_id) || !ValidIdentity(offer.leader_id) ||
        offer.leader_id != local_policy.leader_id ||
        !paired_worker.IsPairedTo(offer.worker_id, offer.leader_id))
        return reject("update offer is not for the current approved pairing");
    if (!ValidUpdateId(offer.update_id) || !ValidManifest(offer.manifest) ||
        !offer.expires_at_unix)
        return reject("update offer fields are invalid");
    const std::time_t now = std::time(nullptr);
    if (now <= 0 || offer.expires_at_unix <= static_cast<std::uint64_t>(now))
        return reject("update offer has expired");
    if (consumed_update_ids.size() > 65536)
        return reject("durable update replay journal is too large");
    for (const std::string& consumed : consumed_update_ids) {
        if (!ValidUpdateId(consumed))
            return reject("durable update replay journal is invalid");
        if (consumed == offer.update_id)
            return reject("update offer ID was already consumed");
    }

    const PlatformIdentity current = CurrentPlatformIdentity();
    if (offer.manifest.os != current.os ||
        offer.manifest.architecture != current.architecture ||
        current.os == "unknown" || current.architecture == "unknown")
        return reject("update offer platform is incompatible");

    std::vector<std::uint8_t> canonical;
    app::agent::TlsChannel::PeerFingerprint signer_fingerprint{};
    if (!CanonicalOffer(offer, canonical) ||
        !pairing::VerifyDetachedUpdateSignature(
            offer.signer_public_key_pem, canonical, offer.signature,
            signer_fingerprint, error) ||
        !EqualFingerprint(signer_fingerprint, approved_fingerprint))
        return reject("update signer does not match the admin-pinned key");

    return detail::OfferAuthority::Create(
        offer, EncodeFingerprint(signer_fingerprint));
}


StageResult StageUpdatePackage(
    const service::Configuration& configuration,
    const Config& local_policy,
    const pairing::Worker& paired_worker,
    const std::filesystem::path& package_root,
    const std::filesystem::path& package_relative_path,
    const AuthorizedPackageOffer& authorized_offer) {
    const std::time_t now = std::time(nullptr);
    if (!local_policy.allow_remote_update ||
        local_policy.leader_id != authorized_offer.leader_id() ||
        local_policy.update_signer_sha256 != authorized_offer.signer_sha256() ||
        !paired_worker.IsPairedTo(authorized_offer.worker_id(),
                                  authorized_offer.leader_id()) ||
        now <= 0 ||
        authorized_offer.expires_at_unix() <= static_cast<std::uint64_t>(now))
        return Failure(StageError::InvalidArgument,
                       "package authorization is disabled, expired, or no longer paired");
    const PackageManifest& manifest = authorized_offer.manifest();
    if (!ValidManifest(manifest))
        return Failure(StageError::InvalidManifest, "package manifest is invalid");
    if (!ValidRelativeFile(package_relative_path))
        return Failure(StageError::InvalidArgument, "package path must be a safe relative file path");
    const PlatformIdentity current = CurrentPlatformIdentity();
    if (manifest.os != current.os ||
        manifest.architecture != current.architecture ||
        current.os == "unknown" || current.architecture == "unknown")
        return Failure(StageError::Incompatible, "package platform is incompatible");
    return StageNative(configuration, package_root, package_relative_path, manifest);
}

}  // namespace app::agent::update
