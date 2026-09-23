#include "app/AgentLeader.h"

#include "core/FilesystemPath.h"

#include "core/Sha256.h"
#include "data/Json.h"
#include "data/JsonWrite.h"
#ifdef SS_TOOL_SFM
#include "sfm/Pipeline.h"
#include "app/AgentFeatureJob.h"
#include "app/AgentReconstructionJob.h"
#endif
#ifdef SS_TOOL_TRAIN
#include "app/AgentTrainingJob.h"
#include "config/TrainConfigJson.h"
#endif

#include <array>
#include <cctype>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace app::agent {
using spirula::NativeFilesystemPath;
using spirula::LogicalFilesystemPath;
namespace {

using Clock = std::chrono::steady_clock;
using NativeSocket = TlsChannel::NativeSocket;
constexpr std::size_t kMaxConnections = 16;
constexpr std::uint64_t kMaxSafeInteger = 9'007'199'254'740'991ULL;
constexpr auto kHandshakeTimeout = std::chrono::seconds(10);
constexpr auto kIoTimeout = std::chrono::seconds(10);
constexpr auto kListenerPoll = std::chrono::milliseconds(250);
constexpr auto kMaximumCommandTtl = std::chrono::minutes(5);
constexpr std::size_t kMaxCommandHistory = 512;
constexpr std::uint64_t kAdminResolutionGraceMs = 5ULL * 60 * 1000;
constexpr std::size_t kMaxCommandStateBytes = 2 * 1024 * 1024;
constexpr char kSequenceFile[] = "leader-command-sequence";
constexpr char kCommandStateFile[] = "leader-command-state.json";

void SetError(std::string* error, const char* message) {
    if (error) *error = message;
}
void SetError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

bool RequiresExplicitCommandConfirmation(
    wire::CommandAction action) noexcept {
    return action == wire::CommandAction::Stop ||
           action == wire::CommandAction::ForceRestartService;
}
bool IsAdminCommand(wire::CommandAction action) noexcept {
    return action == wire::CommandAction::RebootMachine ||
           action == wire::CommandAction::ActivateUpdate;
}
bool AdvertisedPlatformMatches(const std::string& platform,
                               const std::string& package_os) {
    std::string normalized = platform;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char ch) {
            return static_cast<char>(ch >= 'A' && ch <= 'Z'
                ? ch + ('a' - 'A') : ch);
        });
    return normalized == package_os &&
           (package_os == "windows" || package_os == "linux" ||
            package_os == "macos");
}


std::uint64_t UnixNowMs() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return ms > 0 ? static_cast<std::uint64_t>(ms) : 0;
}

#ifdef _WIN32
using RawSocket = SOCKET;
constexpr RawSocket kInvalidSocket = INVALID_SOCKET;
using SocketLength = int;
void CloseSocket(RawSocket socket) noexcept {
    if (socket != kInvalidSocket) closesocket(socket);
}
void ShutdownSocket(RawSocket socket) noexcept {
    if (socket != kInvalidSocket) shutdown(socket, SD_BOTH);
}
bool IsTransientSocketError(int error) noexcept {
    return error == WSAEINTR || error == WSAEWOULDBLOCK ||
           error == WSAEINPROGRESS || error == WSAEALREADY;
}
#else
using RawSocket = int;
constexpr RawSocket kInvalidSocket = -1;
using SocketLength = socklen_t;
void CloseSocket(RawSocket socket) noexcept {
    if (socket != kInvalidSocket) ::close(socket);
}
void ShutdownSocket(RawSocket socket) noexcept {
    if (socket != kInvalidSocket) ::shutdown(socket, SHUT_RDWR);
}
bool IsTransientSocketError(int error) noexcept {
    return error == EINTR || error == EAGAIN || error == EWOULDBLOCK ||
           error == EINPROGRESS || error == EALREADY;
}
#endif
// A separate handle prevents cancellation from racing TLS close and descriptor reuse.
RawSocket DuplicateSocket(RawSocket socket) noexcept {
#ifdef _WIN32
    WSAPROTOCOL_INFOW protocol{};
    if (WSADuplicateSocketW(socket, GetCurrentProcessId(), &protocol) != 0)
        return kInvalidSocket;
    return WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
                      &protocol, 0, WSA_FLAG_OVERLAPPED);
#else
    return ::fcntl(socket, F_DUPFD_CLOEXEC, 3);
#endif
}


NativeSocket ToNative(RawSocket socket) noexcept {
    return socket == kInvalidSocket ? TlsChannel::kInvalidSocket
                                    : static_cast<NativeSocket>(socket);
}
RawSocket FromNative(NativeSocket socket) noexcept {
    return socket == TlsChannel::kInvalidSocket
        ? kInvalidSocket : static_cast<RawSocket>(socket);
}

struct SocketRuntime final {
#ifdef _WIN32
    bool started = false;
    bool Start() noexcept {
        WSADATA data{};
        const int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0 || LOBYTE(data.wVersion) != 2 || HIBYTE(data.wVersion) != 2) {
            if (result == 0) WSACleanup();
            return false;
        }
        started = true;
        return true;
    }
    ~SocketRuntime() { if (started) WSACleanup(); }
#else
    bool Start() noexcept { return true; }
#endif
};

bool IsPrivateBindAddress(const std::string& address) noexcept {
    in_addr ipv4{};
    if (inet_pton(AF_INET, address.c_str(), &ipv4) == 1) {
        const std::uint32_t host = ntohl(ipv4.s_addr);
        const std::uint32_t first = host >> 24;
        const std::uint32_t second = (host >> 16) & 0xff;
        return first == 10 || first == 127 ||
               (first == 172 && second >= 16 && second <= 31) ||
               (first == 192 && second == 168) ||
               (first == 169 && second == 254);
    }
    in6_addr ipv6{};
    if (inet_pton(AF_INET6, address.c_str(), &ipv6) != 1 ||
        IN6_IS_ADDR_V4MAPPED(&ipv6))
        return false;
    if (IN6_IS_ADDR_LOOPBACK(&ipv6) ||
        (ipv6.s6_addr[0] & 0xfe) == 0xfc)
        return true;
    return ipv6.s6_addr[0] == 0xfe && (ipv6.s6_addr[1] & 0xc0) == 0x80;
}

bool WaitReadable(RawSocket socket, std::chrono::milliseconds timeout) noexcept {
    if (socket == kInvalidSocket) return false;
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket, &read_set);
    timeval wait{};
    wait.tv_sec = static_cast<long>(timeout.count() / 1000);
    wait.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
#ifdef _WIN32
    return select(0, &read_set, nullptr, nullptr, &wait) > 0;
#else
    return select(socket + 1, &read_set, nullptr, nullptr, &wait) > 0;
#endif
}

RawSocket AcceptReady(RawSocket& listener, std::mutex& listener_mutex,
                      std::atomic<bool>& stopping) noexcept {
    std::lock_guard<std::mutex> lock(listener_mutex);
    if (stopping.load(std::memory_order_acquire) || listener == kInvalidSocket ||
        !WaitReadable(listener, kListenerPoll))
        return kInvalidSocket;
    RawSocket accepted = accept(listener, nullptr, nullptr);
    if (accepted == kInvalidSocket &&
        !IsTransientSocketError(
#ifdef _WIN32
            WSAGetLastError()
#else
            errno
#endif
        ))
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return accepted;
}

bool CreateListener(const std::string& address, std::uint16_t requested_port,
                    RawSocket& socket, std::uint16_t& bound_port,
                    std::string* error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICHOST;
    const std::string service = std::to_string(requested_port);
    addrinfo* result = nullptr;
    if (getaddrinfo(address.c_str(), service.c_str(), &hints, &result) != 0 || !result) {
        SetError(error, "bind address must be a private numeric IPv4 or IPv6 address");
        if (result) freeaddrinfo(result);
        return false;
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(result, freeaddrinfo);
    for (const addrinfo* item = addresses.get(); item; item = item->ai_next) {
        RawSocket candidate = ::socket(item->ai_family, item->ai_socktype,
                                       item->ai_protocol);
        if (candidate == kInvalidSocket) continue;
#ifdef _WIN32
        int exclusive = 1;
        bool configured = setsockopt(candidate, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == 0;
#else
        int reuse = 1;
        bool configured = setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR,
                                     &reuse, sizeof(reuse)) == 0;
#endif
        if (configured && item->ai_family == AF_INET6) {
            int only_v6 = 1;
#ifdef _WIN32
            configured = setsockopt(candidate, IPPROTO_IPV6, IPV6_V6ONLY,
                reinterpret_cast<const char*>(&only_v6), sizeof(only_v6)) == 0;
#else
            configured = setsockopt(candidate, IPPROTO_IPV6, IPV6_V6ONLY,
                                    &only_v6, sizeof(only_v6)) == 0;
#endif
        }
        if (!configured || bind(candidate, item->ai_addr,
#ifdef _WIN32
                                static_cast<int>(item->ai_addrlen)
#else
                                static_cast<socklen_t>(item->ai_addrlen)
#endif
                                ) != 0 || listen(candidate, 16) != 0) {
            CloseSocket(candidate);
            continue;
        }
        sockaddr_storage local{};
        SocketLength local_size = sizeof(local);
        if (getsockname(candidate, reinterpret_cast<sockaddr*>(&local), &local_size) != 0) {
            CloseSocket(candidate);
            continue;
        }
        if (local.ss_family == AF_INET)
            bound_port = ntohs(reinterpret_cast<sockaddr_in*>(&local)->sin_port);
        else if (local.ss_family == AF_INET6)
            bound_port = ntohs(reinterpret_cast<sockaddr_in6*>(&local)->sin6_port);
        else {
            CloseSocket(candidate);
            continue;
        }
        socket = candidate;
        return true;
    }
    SetError(error, "could not bind a private control-plane listener");
    return false;
}

std::string SequenceText(std::uint64_t sequence) {
    return std::to_string(sequence) + "\n";
}

bool ParseSequence(const std::string& bytes, std::uint64_t& sequence) noexcept {
    std::string_view text(bytes);
    if (!text.empty() && text.back() == '\n') text.remove_suffix(1);
    if (text.empty() || text.size() > 20) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), sequence);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
           sequence <= kMaxSafeInteger;
}

bool LoadDurableText(const std::filesystem::path& root,
                     const std::string& filename, std::size_t maximum_bytes,
                     std::string& bytes, bool& exists, std::string* error,
                     const char* description) {
    const auto path = NativeFilesystemPath(root / "agent-pairing" / filename);
    exists = false;
#ifdef _WIN32
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return true;
        SetError(error, std::string("could not read ") + description);
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const bool queried = GetFileInformationByHandle(file, &info) != 0;
    const std::uint64_t size = queried
        ? (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) |
              info.nFileSizeLow
        : std::numeric_limits<std::uint64_t>::max();
    const bool safe = queried &&
        !(info.dwFileAttributes &
          (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        size <= maximum_bytes;
    if (!safe) {
        CloseHandle(file);
        SetError(error, std::string(description) + " is unsafe");
        return false;
    }
    const DWORD length = info.nFileSizeLow;
    bytes.assign(length, '\0');
    DWORD read = 0;
    const bool ok = length == 0 ||
        (ReadFile(file, bytes.data(), length, &read, nullptr) && read == length);
    CloseHandle(file);
    if (!ok) {
        SetError(error, std::string("could not read ") + description);
        return false;
    }
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) return true;
        SetError(error, std::string("could not read ") + description);
        return false;
    }
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != ::geteuid() || (info.st_mode & 0077) != 0 ||
        info.st_size < 0 ||
        static_cast<std::uint64_t>(info.st_size) > maximum_bytes) {
        ::close(fd);
        SetError(error, std::string(description) + " is unsafe");
        return false;
    }
    bytes.assign(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t got = ::read(fd, bytes.data() + offset, bytes.size() - offset);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        offset += static_cast<std::size_t>(got);
    }
    ::close(fd);
    if (offset != bytes.size()) {
        SetError(error, std::string("could not read ") + description);
        return false;
    }
#endif
    exists = true;
    return true;
}

bool PersistDurableText(const std::filesystem::path& root,
                        const std::string& filename, const std::string& bytes,
                        std::size_t maximum_bytes, std::string* error,
                        const char* description) {
    static std::atomic<std::uint64_t> temporary_id{0};
    const auto directory = NativeFilesystemPath(root / "agent-pairing");
    if (bytes.size() > maximum_bytes) {
        SetError(error, std::string(description) + " exceeds its storage limit");
        return false;
    }
#ifdef _WIN32
    const std::wstring name(filename.begin(), filename.end());
    const auto target = directory / name;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::filesystem::path temporary;
    for (;;) {
        temporary = directory /
            (name + L".tmp-" + std::to_wstring(temporary_id.fetch_add(1)));
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) continue;
        SetError(error, std::string("could not create ") + description);
        return false;
    }
    DWORD written = 0;
    bool ok = bytes.size() <= std::numeric_limits<DWORD>::max() &&
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                  &written, nullptr) && written == bytes.size() &&
        FlushFileBuffers(file);
    CloseHandle(file);
    if (ok) ok = MoveFileExW(temporary.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(temporary.c_str());
    if (!ok) SetError(error, std::string("could not persist ") + description);
    return ok;
#else
    const int dirfd = ::open(directory.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dirfd < 0) {
        SetError(error, std::string("could not open ") + description + " directory");
        return false;
    }
    std::string temporary;
    int fd = -1;
    for (;;) {
        temporary = filename + ".tmp-" +
            std::to_string(temporary_id.fetch_add(1));
        fd = ::openat(dirfd, temporary.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0) break;
        if (errno == EEXIST || errno == EINTR) continue;
        ::close(dirfd);
        SetError(error, std::string("could not create ") + description);
        return false;
    }
    bool ok = ::fchmod(fd, 0600) == 0;
    std::size_t offset = 0;
    while (ok && offset < bytes.size()) {
        const ssize_t count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { ok = false; break; }
        offset += static_cast<std::size_t>(count);
    }
    if (ok) ok = ::fsync(fd) == 0;
    ::close(fd);
    if (ok) ok = ::renameat(dirfd, temporary.c_str(), dirfd, filename.c_str()) == 0;
    if (ok) ok = ::fsync(dirfd) == 0;
    if (!ok) ::unlinkat(dirfd, temporary.c_str(), 0);
    ::close(dirfd);
    if (!ok) SetError(error, std::string("could not persist ") + description);
    return ok;
#endif
}

bool LoadSequence(const std::filesystem::path& root, std::uint64_t& sequence,
                  std::string* error) {
    std::string bytes;
    bool exists = false;
    if (!LoadDurableText(root, kSequenceFile, 32, bytes, exists, error,
                         "durable command sequence file"))
        return false;
    if (!exists) {
        sequence = 0;
        return true;
    }
    if (!ParseSequence(bytes, sequence)) {
        SetError(error, "durable command sequence file is malformed");
        return false;
    }
    return true;
}

bool PersistSequence(const std::filesystem::path& root, std::uint64_t sequence,
                     std::string* error) {
    return PersistDurableText(root, kSequenceFile, SequenceText(sequence), 32,
                              error, "durable command sequence");
}

wire::Status LeaderStatus() {
    wire::Status status;
    status.connection = wire::ConnectionState::Connected;
    status.compatibility = wire::CompatibilityState::Unknown;
    status.scheduling = wire::SchedulingState::Stopped;
    status.activity = wire::ActivityState::Idle;
    status.health = wire::HealthState::Healthy;
    status.capabilities.clear();
#ifdef SS_VERSION
    status.build = SS_VERSION;
#else
    status.build = "unknown";
#endif
#ifdef _WIN32
    status.platform = "Windows";
#elif defined(__APPLE__)
    status.platform = "macOS";
#else
    status.platform = "Linux";
#endif
    status.gpu.clear();
    status.maintenance = false;
    status.online = true;
    return status;
}

wire::Status DisconnectedStatus() {
    wire::Status status;
    status.connection = wire::ConnectionState::Disconnected;
    status.compatibility = wire::CompatibilityState::Unknown;
    status.scheduling = wire::SchedulingState::Stopped;
    status.activity = wire::ActivityState::Idle;
    status.health = wire::HealthState::Healthy;
    status.capabilities.clear();
    status.online = false;
    return status;
}

wire::Status SnapshotStatus(const wire::Status& worker_status) {
    wire::Status status = worker_status;
    status.connection = wire::ConnectionState::Connected;
    return status;
}

std::optional<std::string> EncodeMessage(const wire::Message& message,
                                         std::uint64_t now_ms) {
    std::string json;
    if (wire::Encode(message, now_ms, json) != wire::Error::None)
        return std::nullopt;
    return json;
}

bool SendMessage(TlsChannel& channel, const wire::Message& message,
                 std::string* error = nullptr) {
    const auto encoded = EncodeMessage(message, UnixNowMs());
    if (!encoded) {
        SetError(error, "could not encode control-plane message");
        return false;
    }
    return channel.SendFrame(reinterpret_cast<const std::uint8_t*>(encoded->data()),
                             encoded->size(), error);
}

constexpr char kLeaderUpdateStore[] = "leader-update-packages";
std::atomic<std::uint64_t> g_package_temp_id{0};

bool HashPackageFile(const std::filesystem::path& path,
                     std::uint64_t& size, std::string& digest) {
    std::ifstream input(NativeFilesystemPath(path), std::ios::binary);
    if (!input) return false;
    spirula::Sha256 hash;
    std::array<char, 64 * 1024> buffer{};
    size = 0;
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            const auto bytes = static_cast<std::uint64_t>(count);
            if (size > update::kMaxPackageBytes ||
                bytes > update::kMaxPackageBytes - size)
                return false;
            hash.update(reinterpret_cast<const std::uint8_t*>(buffer.data()),
                        static_cast<std::size_t>(count));
            size += bytes;
        }
    }
    if (!input.eof()) return false;
    digest = hash.hex();
    return true;
}

bool EnsurePackageDirectory(const std::filesystem::path& path,
                            std::string* error) {
    const auto native = NativeFilesystemPath(path);
    std::error_code ec;
    auto status = std::filesystem::symlink_status(native, ec);
    bool created = false;
    if ((ec == std::errc::no_such_file_or_directory) ||
        (!ec && status.type() == std::filesystem::file_type::not_found)) {
        ec.clear();
        created = std::filesystem::create_directory(native, ec);
        if (ec && ec != std::errc::file_exists) {
            SetError(error, "could not create leader update staging directory");
            return false;
        }
        ec.clear();
        status = std::filesystem::symlink_status(native, ec);
    }
    if (ec || !std::filesystem::is_directory(status) ||
        std::filesystem::is_symlink(status)) {
        SetError(error, "leader update staging directory is unsafe");
        return false;
    }
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(native.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        SetError(error, "leader update staging directory is unsafe");
        return false;
    }
#endif
#ifndef _WIN32
    if (created) {
        std::filesystem::permissions(native, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, ec);
        if (ec) {
            SetError(error, "could not protect leader update staging directory");
            return false;
        }
        status = std::filesystem::symlink_status(native, ec);
    }
    const auto permissions = status.permissions();
    if (ec ||
        (permissions & std::filesystem::perms::owner_all) !=
            std::filesystem::perms::owner_all ||
        (permissions & (std::filesystem::perms::group_all |
                       std::filesystem::perms::others_all)) !=
            std::filesystem::perms::none) {
        SetError(error, "leader update staging directory is not private");
        return false;
    }
#endif
    return true;
}

bool IsPackageFile(const std::filesystem::path& path, bool& exists,
                   std::string* error) {
    const auto native = NativeFilesystemPath(path);
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(native, ec);
    if (ec == std::errc::no_such_file_or_directory ||
        (!ec && status.type() == std::filesystem::file_type::not_found)) {
        exists = false;
        return true;
    }
    if (ec || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status)) {
        SetError(error, "leader update package path is unsafe");
        return false;
    }
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(native.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        SetError(error, "leader update package path is unsafe");
        return false;
    }
#endif
    exists = true;
    return true;
}

bool VerifyStagedPackage(const std::filesystem::path& path,
                         std::uint64_t expected_size,
                         const std::string& expected_digest,
                         std::string* error) {
    bool exists = false;
    if (!IsPackageFile(path, exists, error) || !exists) {
        if (!error || error->empty())
            SetError(error, "staged leader update package is missing");
        return false;
    }
    std::error_code ec;
    const auto native = NativeFilesystemPath(path);
    const auto size = std::filesystem::file_size(native, ec);
    const auto links = std::filesystem::hard_link_count(native, ec);
    if (ec || size != expected_size || links != 1) {
        SetError(error, "staged leader update package conflicts with its digest");
        return false;
    }
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(native.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        !(attributes & FILE_ATTRIBUTE_READONLY)) {
        SetError(error, "staged leader update package is not immutable");
        return false;
    }
#else
    const auto status = std::filesystem::status(native, ec);
    if (ec ||
        (status.permissions() & (std::filesystem::perms::group_all |
                                 std::filesystem::perms::others_all |
                                 std::filesystem::perms::owner_write)) !=
            std::filesystem::perms::none) {
        SetError(error, "staged leader update package is not immutable");
        return false;
    }
#endif
    std::uint64_t actual_size = 0;
    std::string actual_digest;
    if (!HashPackageFile(path, actual_size, actual_digest) ||
        actual_size != expected_size || actual_digest != expected_digest) {
        SetError(error, "staged leader update package failed digest verification");
        return false;
    }
    return true;
}

bool FlushAndProtectPackage(const std::filesystem::path& path,
                            std::string* error) {
    const auto native = NativeFilesystemPath(path);
#ifdef _WIN32
    HANDLE file = CreateFileW(native.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        SetError(error, "could not flush staged leader update package");
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const bool safe = GetFileInformationByHandle(file, &info) &&
        !(info.dwFileAttributes &
          (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
    const bool flushed = safe && FlushFileBuffers(file);
    CloseHandle(file);
    if (!flushed) {
        SetError(error, "could not flush staged leader update package");
        return false;
    }
    const DWORD attributes = GetFileAttributesW(native.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        !SetFileAttributesW(native.c_str(), attributes | FILE_ATTRIBUTE_READONLY)) {
        SetError(error, "could not make staged leader update package immutable");
        return false;
    }
#else
    const int file = ::open(native.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (file < 0) {
        SetError(error, "could not flush staged leader update package");
        return false;
    }
    struct stat info{};
    const bool safe = ::fstat(file, &info) == 0 && S_ISREG(info.st_mode) &&
                      info.st_nlink == 1;
    const bool flushed = safe && ::fsync(file) == 0 &&
                         ::fchmod(file, S_IRUSR) == 0 && ::fsync(file) == 0;
    ::close(file);
    if (!flushed) {
        SetError(error, "could not protect staged leader update package");
        return false;
    }
#endif
    return true;
}

bool FlushPackageDirectory(const std::filesystem::path& path) {
#ifdef _WIN32
    (void)path;
    return true;
#else
    const auto native = NativeFilesystemPath(path);
    const int directory = ::open(native.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) return false;
    const bool flushed = ::fsync(directory) == 0;
    ::close(directory);
    return flushed;
#endif
}

std::optional<std::filesystem::path> StageLeaderPackage(
    const std::filesystem::path& source,
    const std::filesystem::path& state_root,
    update::PackageManifest& manifest, std::string* error) {
    if (source.empty() ||
        (manifest.size && manifest.size > update::kMaxPackageBytes) ||
        (!manifest.sha256.empty() &&
         (manifest.sha256.size() != 64 ||
          !std::all_of(manifest.sha256.begin(), manifest.sha256.end(),
              [](char ch) { return (ch >= '0' && ch <= '9') ||
                                    (ch >= 'a' && ch <= 'f'); })))) {
        SetError(error, "update package source or supplied manifest is invalid");
        return std::nullopt;
    }
    bool source_exists = false;
    if (!IsPackageFile(source, source_exists, error) || !source_exists) {
        if (!error || error->empty())
            SetError(error, "update package source is missing");
        return std::nullopt;
    }
    std::error_code ec;
    const auto source_size = std::filesystem::file_size(
        NativeFilesystemPath(source), ec);
    if (ec || !source_size || source_size > update::kMaxPackageBytes) {
        SetError(error, "update package source size is invalid");
        return std::nullopt;
    }

    const auto pairing_root = state_root / "agent-pairing";
    const auto store_root = pairing_root / kLeaderUpdateStore;
    const auto temporary_root = store_root / ".staging";
    if (!EnsurePackageDirectory(pairing_root, error) ||
        !EnsurePackageDirectory(store_root, error) ||
        !EnsurePackageDirectory(temporary_root, error))
        return std::nullopt;
    if (!FlushPackageDirectory(state_root) ||
        !FlushPackageDirectory(pairing_root) ||
        !FlushPackageDirectory(store_root)) {
        SetError(error, "could not persist leader update staging directories");
        return std::nullopt;
    }

    std::filesystem::path temporary;
    bool copied = false;
    for (unsigned attempt = 0; attempt != 128; ++attempt) {
        temporary = NativeFilesystemPath(temporary_root /
            ("package-" + std::to_string(
                g_package_temp_id.fetch_add(1, std::memory_order_relaxed)) +
             ".tmp"));
        ec.clear();
        copied = std::filesystem::copy_file(
            NativeFilesystemPath(source), temporary,
            std::filesystem::copy_options::none, ec);
        if (copied) break;
        if (ec != std::errc::file_exists) {
            SetError(error, "could not copy update package into leader staging");
            return std::nullopt;
        }
    }
    if (!copied) {
        SetError(error, "could not reserve leader update staging file");
        return std::nullopt;
    }
    struct Cleanup final {
        std::filesystem::path path;
        bool keep = false;
        ~Cleanup() {
            if (!keep) {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        }
    } cleanup{temporary};

    bool temporary_exists = false;
    if (!IsPackageFile(temporary, temporary_exists, error) || !temporary_exists) {
        if (!error || error->empty())
            SetError(error, "leader update staging copy is unsafe");
        return std::nullopt;
    }
    std::uint64_t actual_size = 0;
    std::string actual_digest;
    if (!HashPackageFile(temporary, actual_size, actual_digest) ||
        !actual_size ||
        (manifest.size && manifest.size != actual_size) ||
        (!manifest.sha256.empty() && manifest.sha256 != actual_digest)) {
        SetError(error, "copied update package does not match its supplied manifest");
        return std::nullopt;
    }
    if (!FlushAndProtectPackage(temporary, error)) return std::nullopt;

    const auto digest_root = store_root / actual_digest;
    if (!EnsurePackageDirectory(digest_root, error)) return std::nullopt;
    if (!FlushPackageDirectory(store_root)) {
        SetError(error, "could not persist leader update package directory");
        return std::nullopt;
    }
    const auto destination = digest_root / "package.bin";
    bool destination_exists = false;
    if (!IsPackageFile(destination, destination_exists, error))
        return std::nullopt;
    if (destination_exists) {
        if (!VerifyStagedPackage(destination, actual_size, actual_digest, error))
            return std::nullopt;
    } else {
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), NativeFilesystemPath(destination).c_str(),
                         MOVEFILE_WRITE_THROUGH)) {
            const DWORD code = GetLastError();
            if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) {
                SetError(error, "could not publish staged leader update package");
                return std::nullopt;
            }
            if (!VerifyStagedPackage(destination, actual_size, actual_digest, error))
                return std::nullopt;
        } else {
            cleanup.keep = true;
        }
#else
        ec.clear();
        std::filesystem::rename(temporary, NativeFilesystemPath(destination), ec);
        if (ec) {
            SetError(error, "could not publish staged leader update package");
            return std::nullopt;
        }
        cleanup.keep = true;
#endif
        if (!FlushPackageDirectory(digest_root)) {
            SetError(error, "could not persist staged leader update package");
            return std::nullopt;
        }
    }
    if (!VerifyStagedPackage(destination, actual_size, actual_digest, error))
        return std::nullopt;
    manifest.size = actual_size;
    manifest.sha256 = actual_digest;
    return digest_root;
}


bool IsReparsePoint(const std::filesystem::path& path) {
#ifdef _WIN32
    const DWORD attributes =
        GetFileAttributesW(NativeFilesystemPath(path).c_str());
    return attributes == INVALID_FILE_ATTRIBUTES ||
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    (void)path;
    return false;
#endif
}

bool EnsureTransferRoot(const std::filesystem::path& path,
                        std::string* error) {
    if (!path.is_absolute()) {
        SetError(error, "artifact staging root is not absolute");
        return false;
    }
    const auto native = NativeFilesystemPath(path);
    std::error_code ec;
    const auto parent = std::filesystem::symlink_status(
        native.parent_path(), ec);
    if (ec || !std::filesystem::is_directory(parent) ||
        std::filesystem::is_symlink(parent) ||
        IsReparsePoint(native.parent_path())) {
        SetError(error, "artifact staging parent is unsafe");
        return false;
    }
    ec.clear();
    auto status = std::filesystem::symlink_status(native, ec);
    const bool missing = ec == std::errc::no_such_file_or_directory ||
        (!ec && status.type() == std::filesystem::file_type::not_found);
    if (missing) {
        ec.clear();
        std::filesystem::create_directory(native, ec);
        if (ec) {
            SetError(error, "could not create artifact staging root");
            return false;
        }
        ec.clear();
        status = std::filesystem::symlink_status(native, ec);
    }
    if (ec || !std::filesystem::is_directory(status) ||
        std::filesystem::is_symlink(status) || IsReparsePoint(native)) {
        SetError(error, "artifact staging root is unsafe");
        return false;
    }
    return true;
}


#ifdef SS_TOOL_SFM

constexpr char kFeatureStoreDirectory[] = "agent-feature-jobs";
constexpr char kFeatureStoreFile[] = "jobs.json";
constexpr std::size_t kMaxFeatureAttempts = 64;
constexpr std::size_t kMaxFeatureStoreBytes = 64 * 1024 * 1024;
constexpr std::size_t kMaxFeatureManifestJsonBytes = 256 * 1024;
constexpr std::uint64_t kMaxFeatureDiskBudgetBytes = 1ULL << 40;
constexpr std::size_t kMaxFeatureStagingTasks = 2;
constexpr std::uint64_t kFeatureOfferLifetimeMs = 60'000;

struct FeatureAttempt final {
    LeaderServer::FeatureJobSnapshot snapshot;
    std::filesystem::path attempt_root;
    std::vector<TransferFile> inputs;
    std::vector<TransferFile> outputs;
    std::string plan_digest;
    std::string request_digest;
    std::string required_build;
    std::string result_error;
    std::uint64_t expires_at_ms = 0;
    std::uint64_t disk_budget_bytes = 0;
    wire::FeatureResult::Outcome outcome = wire::FeatureResult::Outcome::Succeeded;
    bool has_result = false;
    bool current = true;
};
struct FeatureStageTask final {
    std::string job_id;
    std::string attempt_id;
    std::string worker_id;
    std::filesystem::path plan_path;
    std::filesystem::path request_path;
    std::filesystem::path image_root;
    std::filesystem::path mask_root;
    std::string required_build;
    std::uint64_t disk_budget_bytes = 0;
};

constexpr char kReconstructionStoreDirectory[] = "agent-reconstruction-jobs";
constexpr char kReconstructionStoreFile[] = "jobs.json";
constexpr std::size_t kMaxReconstructionAttempts = 64;
constexpr std::size_t kMaxReconstructionStoreBytes = 64 * 1024 * 1024;
constexpr std::size_t kMaxReconstructionManifestJsonBytes = 256 * 1024;
constexpr std::uint64_t kMaxReconstructionDiskBudgetBytes = 1ULL << 40;
constexpr std::size_t kMaxReconstructionStagingTasks = 2;
constexpr std::uint64_t kReconstructionOfferLifetimeMs = 60'000;

struct ReconstructionAttempt final {
    LeaderServer::ReconstructionJobSnapshot snapshot;
    std::filesystem::path attempt_root;
    std::vector<TransferFile> inputs;
    std::vector<TransferFile> outputs;
    std::string result_error_sha256;
    std::uint64_t expires_at_ms = 0;
    std::uint64_t disk_budget_bytes = 0;
    wire::PortableResult::Outcome outcome = wire::PortableResult::Outcome::Failed;
    bool has_result = false;
    bool current = true;
};

struct ReconstructionStageTask final {
    std::string job_id;
    std::string attempt_id;
    std::string worker_id;
    sfm::AutoRequest request;
    std::filesystem::path source_manifest;
    std::string required_build;
    std::uint64_t disk_budget_bytes = 0;
};

const char* FeatureStateName(LeaderServer::FeatureJobState state) noexcept {
    using State = LeaderServer::FeatureJobState;
    switch (state) {
        case State::Staging: return "staging";
        case State::Queued: return "queued";
        case State::Offered: return "offered";
        case State::TransferringInputs: return "transferring-inputs";
        case State::Running: return "running";
        case State::Unknown: return "unknown";
        case State::ReceivingOutput: return "receiving-output";
        case State::Succeeded: return "succeeded";
        case State::Failed: return "failed";
        case State::Interrupted: return "interrupted";
        case State::Rejected: return "rejected";
        case State::Superseded: return "superseded";
    }
    return "failed";
}

LeaderServer::FeatureJobState ParseFeatureState(const std::string& name) {
    using State = LeaderServer::FeatureJobState;
    if (name == "staging") return State::Staging;
    if (name == "queued") return State::Queued;
    if (name == "offered") return State::Offered;
    if (name == "transferring-inputs") return State::TransferringInputs;
    if (name == "running") return State::Running;
    if (name == "unknown") return State::Unknown;
    if (name == "receiving-output") return State::ReceivingOutput;
    if (name == "succeeded") return State::Succeeded;
    if (name == "failed") return State::Failed;
    if (name == "interrupted") return State::Interrupted;
    if (name == "rejected") return State::Rejected;
    if (name == "superseded") return State::Superseded;
    throw std::runtime_error("invalid persisted feature state");
}

const char* FeatureOutcomeName(wire::FeatureResult::Outcome outcome) noexcept {
    switch (outcome) {
        case wire::FeatureResult::Outcome::Succeeded: return "succeeded";
        case wire::FeatureResult::Outcome::Failed: return "failed";
        case wire::FeatureResult::Outcome::Interrupted: return "interrupted";
    }
    return "failed";
}

wire::FeatureResult::Outcome ParseFeatureOutcome(const std::string& name) {
    if (name == "succeeded") return wire::FeatureResult::Outcome::Succeeded;
    if (name == "failed") return wire::FeatureResult::Outcome::Failed;
    if (name == "interrupted") return wire::FeatureResult::Outcome::Interrupted;
    throw std::runtime_error("invalid persisted feature outcome");
}

bool TerminalFeatureState(LeaderServer::FeatureJobState state) noexcept {
    using State = LeaderServer::FeatureJobState;
    return state == State::Succeeded || state == State::Failed ||
           state == State::Interrupted || state == State::Rejected ||
           state == State::Superseded;
}

bool ValidFeatureId(const std::string& id) noexcept {
    if (id.empty() || id.size() > wire::kMaxFeatureIdBytes ||
        id == "." || id == "..")
        return false;
    for (const unsigned char ch : id)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.'))
            return false;
    return true;
}
std::string FeatureAttemptKey(const std::string& job_id,
                              const std::string& attempt_id) {
    std::string key;
    key.reserve(job_id.size() + 1 + attempt_id.size());
    key.append(job_id);
    key.push_back('\0');
    key.append(attempt_id);
    return key;
}

std::string FeatureManifestJson(const std::vector<TransferFile>& files) {
    JsonWriter writer;
    writer.array();
    for (const TransferFile& file : files) {
        writer.object()
            .field("path", file.path)
            .field("size", std::to_string(file.size))
            .field("sha256", file.sha256)
        .end();
    }
    writer.end();
    return writer.str();
}

std::string FeatureAttemptsJson(const std::map<std::string, FeatureAttempt>& attempts) {
    JsonWriter writer;
    writer.object().field("schema_version", 1).key("attempts").array();
    for (const auto& entry : attempts) {
        const FeatureAttempt& attempt = entry.second;
        writer.object()
            .field("job_id", attempt.snapshot.job_id)
            .field("attempt_id", attempt.snapshot.attempt_id)
            .field("worker_id", attempt.snapshot.worker_id)
            .field("state", FeatureStateName(attempt.snapshot.state))
            .field("error", attempt.snapshot.error)
            .field_raw("progress", attempt.snapshot.progress
                ? json_number(*attempt.snapshot.progress) : "null")
            .field("plan_digest", attempt.plan_digest)
            .field("request_digest", attempt.request_digest)
            .field("required_build", attempt.required_build)
            .field("expires_at_ms", std::to_string(attempt.expires_at_ms))
            .field("disk_budget_bytes", std::to_string(attempt.disk_budget_bytes))
            .field("has_result", attempt.has_result)
            .field("outcome", FeatureOutcomeName(attempt.outcome))
            .field("result_error", attempt.result_error)
            .field("current", attempt.current)
            .field_raw("inputs", FeatureManifestJson(attempt.inputs))
            .field_raw("outputs", FeatureManifestJson(attempt.outputs))
        .end();
    }
    writer.end().end();
    return writer.str();
}

const JsonValue& FeatureField(const JsonValue& object, const char* name,
                              JsonValue::Type type) {
    const JsonValue* value = object.find(name);
    if (!value || value->type != type)
        throw std::runtime_error(std::string("invalid feature store field: ") + name);
    return *value;
}

std::uint64_t FeatureUint(const JsonValue& object, const char* name) {
    const std::string& text =
        FeatureField(object, name, JsonValue::Type::String).str;
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size())
        throw std::runtime_error(std::string("invalid feature store integer: ") + name);
    return value;
}

std::vector<TransferFile> ParseFeatureManifest(const JsonValue& value) {
    if (value.type != JsonValue::Type::Array ||
        value.arr.size() > wire::kMaxManifestEntries)
        throw std::runtime_error("invalid feature store manifest");
    std::vector<TransferFile> files;
    files.reserve(value.arr.size());
    for (const JsonValue& item : value.arr) {
        if (!item.is_object())
            throw std::runtime_error("invalid feature store manifest entry");
        TransferFile file;
        file.path = FeatureField(item, "path", JsonValue::Type::String).str;
        const std::string& size =
            FeatureField(item, "size", JsonValue::Type::String).str;
        const auto parsed = std::from_chars(size.data(), size.data() + size.size(),
                                            file.size);
        if (size.empty() || parsed.ec != std::errc{} ||
            parsed.ptr != size.data() + size.size())
            throw std::runtime_error("invalid feature store manifest size");
        file.sha256 = FeatureField(item, "sha256", JsonValue::Type::String).str;
        files.push_back(std::move(file));
    }
    return files;
}

const char* ReconstructionStateName(
    LeaderServer::ReconstructionJobState state) noexcept {
    using State = LeaderServer::ReconstructionJobState;
    switch (state) {
        case State::Staging: return "staging";
        case State::Queued: return "queued";
        case State::Offered: return "offered";
        case State::TransferringInputs: return "transferring-inputs";
        case State::Running: return "running";
        case State::Unknown: return "unknown";
        case State::ReceivingOutput: return "receiving-output";
        case State::Succeeded: return "succeeded";
        case State::Failed: return "failed";
        case State::Interrupted: return "interrupted";
        case State::Rejected: return "rejected";
        case State::Superseded: return "superseded";
    }
    return "failed";
}

LeaderServer::ReconstructionJobState ParseReconstructionState(
    const std::string& name) {
    using State = LeaderServer::ReconstructionJobState;
    if (name == "staging") return State::Staging;
    if (name == "queued") return State::Queued;
    if (name == "offered") return State::Offered;
    if (name == "transferring-inputs") return State::TransferringInputs;
    if (name == "running") return State::Running;
    if (name == "unknown") return State::Unknown;
    if (name == "receiving-output") return State::ReceivingOutput;
    if (name == "succeeded") return State::Succeeded;
    if (name == "failed") return State::Failed;
    if (name == "interrupted") return State::Interrupted;
    if (name == "rejected") return State::Rejected;
    if (name == "superseded") return State::Superseded;
    throw std::runtime_error("invalid persisted reconstruction state");
}

const char* ReconstructionOutcomeName(wire::PortableResult::Outcome outcome) noexcept {
    switch (outcome) {
        case wire::PortableResult::Outcome::Succeeded: return "succeeded";
        case wire::PortableResult::Outcome::Failed: return "failed";
        case wire::PortableResult::Outcome::Interrupted: return "interrupted";
    }
    return "failed";
}

wire::PortableResult::Outcome ParseReconstructionOutcome(const std::string& name) {
    if (name == "succeeded") return wire::PortableResult::Outcome::Succeeded;
    if (name == "failed") return wire::PortableResult::Outcome::Failed;
    if (name == "interrupted") return wire::PortableResult::Outcome::Interrupted;
    throw std::runtime_error("invalid persisted reconstruction outcome");
}

bool TerminalReconstructionState(
    LeaderServer::ReconstructionJobState state) noexcept {
    using State = LeaderServer::ReconstructionJobState;
    return state == State::Succeeded || state == State::Failed ||
           state == State::Interrupted || state == State::Rejected ||
           state == State::Superseded;
}

bool ValidDigest(const std::string& digest) noexcept {
    if (digest.size() != 64) return false;
    for (char c : digest)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

std::string ReconstructionAttemptsJson(
    const std::map<std::string, ReconstructionAttempt>& attempts) {
    JsonWriter writer;
    writer.object().field("schema_version", 1).key("attempts").array();
    for (const auto& entry : attempts) {
        const ReconstructionAttempt& attempt = entry.second;
        writer.object()
            .field("job_id", attempt.snapshot.job_id)
            .field("attempt_id", attempt.snapshot.attempt_id)
            .field("worker_id", attempt.snapshot.worker_id)
            .field("paired_leader_id", attempt.snapshot.paired_leader_id)
            .field("paired_leader_epoch",
                   std::to_string(attempt.snapshot.paired_leader_epoch))
            .field("state", ReconstructionStateName(attempt.snapshot.state))
            .field("error", attempt.snapshot.error)
            .field("required_build", attempt.snapshot.required_build)
            .field("source_manifest_sha256",
                   attempt.snapshot.source_manifest_sha256)
            .field("input_identity_sha256", attempt.snapshot.input_identity_sha256)
            .field("expires_at_ms", std::to_string(attempt.expires_at_ms))
            .field("disk_budget_bytes", std::to_string(attempt.disk_budget_bytes))
            .field("has_result", attempt.has_result)
            .field("outcome", ReconstructionOutcomeName(attempt.outcome))
            .field("result_error_sha256", attempt.result_error_sha256)
            .field("current", attempt.current)
            .field_raw("inputs", FeatureManifestJson(attempt.inputs))
            .field_raw("outputs", FeatureManifestJson(attempt.outputs))
        .end();
    }
    writer.end().end();
    return writer.str();
}

bool ReadReconstructionStore(
    const std::filesystem::path& path,
    std::map<std::string, ReconstructionAttempt>& attempts,
    std::map<std::string, std::string>& current,
    const std::string& leader_id, std::uint64_t leader_epoch,
    std::string* error) {
    const auto io_path = NativeFilesystemPath(path);
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(io_path, ec);
    if (ec == std::errc::no_such_file_or_directory) return true;
    if (ec || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status)) {
        SetError(error, "reconstruction job state file is unsafe");
        return false;
    }
    const auto size = std::filesystem::file_size(io_path, ec);
    if (ec || size > kMaxReconstructionStoreBytes) {
        SetError(error, "reconstruction job state file exceeds its limit");
        return false;
    }
    std::ifstream input(io_path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof()) {
        SetError(error, "could not read reconstruction job state file");
        return false;
    }
    try {
        const JsonValue root = json_parse(bytes);
        if (!root.is_object() ||
            FeatureField(root, "schema_version", JsonValue::Type::Number).num != 1.0)
            throw std::runtime_error("unsupported reconstruction job state schema");
        const JsonValue& list = FeatureField(root, "attempts", JsonValue::Type::Array);
        if (list.arr.size() > kMaxReconstructionAttempts)
            throw std::runtime_error("reconstruction attempt capacity exceeded");
        for (const JsonValue& item : list.arr) {
            if (!item.is_object())
                throw std::runtime_error("invalid reconstruction attempt");
            ReconstructionAttempt attempt;
            attempt.snapshot.job_id =
                FeatureField(item, "job_id", JsonValue::Type::String).str;
            attempt.snapshot.attempt_id =
                FeatureField(item, "attempt_id", JsonValue::Type::String).str;
            attempt.snapshot.worker_id =
                FeatureField(item, "worker_id", JsonValue::Type::String).str;
            attempt.snapshot.paired_leader_id =
                FeatureField(item, "paired_leader_id", JsonValue::Type::String).str;
            attempt.snapshot.paired_leader_epoch =
                FeatureUint(item, "paired_leader_epoch");
            attempt.snapshot.state = ParseReconstructionState(
                FeatureField(item, "state", JsonValue::Type::String).str);
            attempt.snapshot.error =
                FeatureField(item, "error", JsonValue::Type::String).str;
            attempt.snapshot.required_build =
                FeatureField(item, "required_build", JsonValue::Type::String).str;
            attempt.snapshot.source_manifest_sha256 =
                FeatureField(item, "source_manifest_sha256",
                             JsonValue::Type::String).str;
            attempt.snapshot.input_identity_sha256 =
                FeatureField(item, "input_identity_sha256",
                             JsonValue::Type::String).str;
            attempt.expires_at_ms = FeatureUint(item, "expires_at_ms");
            attempt.disk_budget_bytes = FeatureUint(item, "disk_budget_bytes");
            attempt.has_result =
                FeatureField(item, "has_result", JsonValue::Type::Bool).b;
            attempt.outcome = ParseReconstructionOutcome(
                FeatureField(item, "outcome", JsonValue::Type::String).str);
            attempt.result_error_sha256 =
                FeatureField(item, "result_error_sha256",
                             JsonValue::Type::String).str;
            attempt.current = FeatureField(item, "current", JsonValue::Type::Bool).b;
            attempt.snapshot.current = attempt.current;
            attempt.inputs = ParseFeatureManifest(
                FeatureField(item, "inputs", JsonValue::Type::Array));
            attempt.outputs = ParseFeatureManifest(
                FeatureField(item, "outputs", JsonValue::Type::Array));
            const std::string& job_id = attempt.snapshot.job_id;
            const std::string& attempt_id = attempt.snapshot.attempt_id;
            const std::string attempt_key = FeatureAttemptKey(job_id, attempt_id);
            if (!ValidFeatureId(job_id) || !ValidFeatureId(attempt_id) ||
                !ValidFeatureId(attempt.snapshot.worker_id) ||
                !ValidFeatureId(attempt.snapshot.paired_leader_id) ||
                !attempt.snapshot.paired_leader_epoch ||
                attempt.snapshot.paired_leader_id != leader_id ||
                attempt.snapshot.paired_leader_epoch != leader_epoch ||
                attempt.snapshot.required_build.empty() ||
                attempt.snapshot.required_build.size() > wire::kMaxBuildBytes ||
                attempt.disk_budget_bytes == 0 ||
                attempt.disk_budget_bytes > kMaxReconstructionDiskBudgetBytes ||
                attempt.snapshot.error.size() > 4096 ||
                (!attempt.snapshot.source_manifest_sha256.empty() &&
                 !ValidDigest(attempt.snapshot.source_manifest_sha256)))
                throw std::runtime_error("invalid persisted reconstruction identity");
            const bool no_bundle = attempt.inputs.empty() &&
                attempt.snapshot.source_manifest_sha256.empty() &&
                attempt.snapshot.input_identity_sha256.empty();
            if (no_bundle && attempt.has_result)
                throw std::runtime_error(
                    "reconstruction result has no frozen input bundle");
            if (!no_bundle &&
                (attempt.inputs.empty() ||
                 !ValidDigest(attempt.snapshot.input_identity_sha256) ||
                 FeatureManifestJson(attempt.inputs).size() >
                     kMaxReconstructionManifestJsonBytes))
                throw std::runtime_error("invalid staged reconstruction metadata");
            const bool staging =
                attempt.snapshot.state == LeaderServer::ReconstructionJobState::Staging;
            if (staging && (!no_bundle || attempt.has_result))
                throw std::runtime_error("invalid staging reconstruction attempt");
            if ((!attempt.has_result &&
                 (!attempt.outputs.empty() || !attempt.result_error_sha256.empty())) ||
                (attempt.has_result && !ValidDigest(attempt.result_error_sha256)) ||
                (!attempt.current &&
                 attempt.snapshot.state != LeaderServer::ReconstructionJobState::Superseded) ||
                (attempt.current &&
                 attempt.snapshot.state == LeaderServer::ReconstructionJobState::Superseded) ||
                (attempt.snapshot.state == LeaderServer::ReconstructionJobState::Succeeded &&
                 (!attempt.has_result ||
                  attempt.outcome != wire::PortableResult::Outcome::Succeeded ||
                  attempt.outputs.empty())) ||
                (attempt.has_result &&
                 attempt.outcome == wire::PortableResult::Outcome::Succeeded &&
                 attempt.outputs.empty()) ||
                (attempt.has_result &&
                 attempt.outcome != wire::PortableResult::Outcome::Succeeded &&
                 !attempt.outputs.empty()))
                throw std::runtime_error("inconsistent persisted reconstruction state");
            auto within_budget = [&](const std::vector<TransferFile>& files) {
                std::uint64_t total = 0;
                for (const TransferFile& file : files) {
                    if (total > attempt.disk_budget_bytes ||
                        file.size > attempt.disk_budget_bytes - total)
                        return false;
                    total += file.size;
                }
                return true;
            };
            if (!no_bundle &&
                (!within_budget(attempt.inputs) || !within_budget(attempt.outputs) ||
                 FeatureManifestJson(attempt.outputs).size() >
                     kMaxReconstructionManifestJsonBytes))
                throw std::runtime_error("persisted reconstruction manifest exceeds its limits");
            if (!no_bundle) {
                wire::PortableOffer offer;
                offer.workload = wire::PortableWorkload::Reconstruction;
                offer.job_id = job_id;
                offer.attempt_id = attempt_id;
                offer.input_identity_sha256 = attempt.snapshot.input_identity_sha256;
                offer.required_build = attempt.snapshot.required_build;
                offer.expires_at_ms = 2;
                offer.inputs = attempt.inputs;
                wire::Message message;
                message.payload = std::move(offer);
                std::string encoded;
                if (wire::Encode(message, 1, encoded) != wire::Error::None)
                    throw std::runtime_error("invalid persisted reconstruction offer");
            }
            if (attempt.has_result) {
                wire::PortableResult result;
                result.workload = wire::PortableWorkload::Reconstruction;
                result.job_id = job_id;
                result.attempt_id = attempt_id;
                result.outcome = attempt.outcome;
                result.outputs = attempt.outputs;
                wire::Message message;
                message.payload = std::move(result);
                std::string encoded;
                if (wire::Encode(message, 1, encoded) != wire::Error::None)
                    throw std::runtime_error("invalid persisted reconstruction result");
            }
            attempt.snapshot.result_root =
                path.parent_path() / std::filesystem::u8path(job_id) /
                std::filesystem::u8path(attempt_id) / "result";
            attempt.attempt_root = attempt.snapshot.result_root.parent_path();
            if (!attempts.emplace(attempt_key, std::move(attempt)).second)
                throw std::runtime_error("duplicate persisted reconstruction attempt");
            const auto& loaded = attempts.at(attempt_key);
            if (loaded.current &&
                !current.emplace(loaded.snapshot.job_id, attempt_key).second)
                throw std::runtime_error("multiple authoritative attempts for a reconstruction job");
        }
    } catch (const std::exception& exception) {
        SetError(error, std::string("invalid reconstruction job state: ") +
                            exception.what());
        return false;
    }
    return true;
}

bool ReadFeatureStore(const std::filesystem::path& path,
                      std::map<std::string, FeatureAttempt>& attempts,
                      std::map<std::string, std::string>& current,
                      std::string* error) {
    const auto io_path = NativeFilesystemPath(path);
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(io_path, ec);
    if (ec == std::errc::no_such_file_or_directory) return true;
    if (ec || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status)) {
        SetError(error, "feature job state file is unsafe");
        return false;
    }
    const auto size = std::filesystem::file_size(io_path, ec);
    if (ec || size > kMaxFeatureStoreBytes) {
        SetError(error, "feature job state file exceeds its limit");
        return false;
    }
    std::ifstream input(io_path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof()) {
        SetError(error, "could not read feature job state file");
        return false;
    }
    try {
        const JsonValue root = json_parse(bytes);
        if (!root.is_object() ||
            FeatureField(root, "schema_version", JsonValue::Type::Number).num != 1.0)
            throw std::runtime_error("unsupported feature job state schema");
        const JsonValue& list = FeatureField(root, "attempts", JsonValue::Type::Array);
        if (list.arr.size() > kMaxFeatureAttempts)
            throw std::runtime_error("feature attempt capacity exceeded");
        for (const JsonValue& item : list.arr) {
            if (!item.is_object()) throw std::runtime_error("invalid feature attempt");
            FeatureAttempt attempt;
            attempt.snapshot.job_id =
                FeatureField(item, "job_id", JsonValue::Type::String).str;
            const std::string attempt_id =
                FeatureField(item, "attempt_id", JsonValue::Type::String).str;
            attempt.snapshot.attempt_id = attempt_id;
            const std::string attempt_key =
                FeatureAttemptKey(attempt.snapshot.job_id, attempt_id);
            attempt.snapshot.worker_id =
                FeatureField(item, "worker_id", JsonValue::Type::String).str;
            if (!ValidFeatureId(attempt.snapshot.job_id) ||
                !ValidFeatureId(attempt_id) ||
                !ValidFeatureId(attempt.snapshot.worker_id))
                throw std::runtime_error("invalid feature attempt identity");
            attempt.snapshot.state = ParseFeatureState(
                FeatureField(item, "state", JsonValue::Type::String).str);
            attempt.snapshot.error =
                FeatureField(item, "error", JsonValue::Type::String).str;
            const JsonValue* progress = item.find("progress");
            if (!progress) throw std::runtime_error("missing feature progress");
            if (progress->type == JsonValue::Type::Number) {
                if (!std::isfinite(progress->num) || progress->num < 0.0 ||
                    progress->num > 1.0)
                    throw std::runtime_error("invalid feature progress");
                attempt.snapshot.progress = progress->num;
            } else if (progress->type != JsonValue::Type::Null) {
                throw std::runtime_error("invalid feature progress");
            }
            attempt.plan_digest =
                FeatureField(item, "plan_digest", JsonValue::Type::String).str;
            attempt.request_digest =
                FeatureField(item, "request_digest", JsonValue::Type::String).str;
            attempt.required_build =
                FeatureField(item, "required_build", JsonValue::Type::String).str;
            attempt.expires_at_ms = FeatureUint(item, "expires_at_ms");
            attempt.disk_budget_bytes = FeatureUint(item, "disk_budget_bytes");
            attempt.has_result =
                FeatureField(item, "has_result", JsonValue::Type::Bool).b;
            attempt.outcome = ParseFeatureOutcome(
                FeatureField(item, "outcome", JsonValue::Type::String).str);
            attempt.result_error =
                FeatureField(item, "result_error", JsonValue::Type::String).str;
            attempt.current = FeatureField(item, "current", JsonValue::Type::Bool).b;
            attempt.inputs = ParseFeatureManifest(
                FeatureField(item, "inputs", JsonValue::Type::Array));
            attempt.outputs = ParseFeatureManifest(
                FeatureField(item, "outputs", JsonValue::Type::Array));
            if (!attempt.disk_budget_bytes ||
                attempt.disk_budget_bytes > kMaxFeatureDiskBudgetBytes ||
                attempt.required_build.empty() ||
                attempt.snapshot.error.size() > 4096)
                throw std::runtime_error("invalid persisted feature attempt fields");
            const bool staging =
                attempt.snapshot.state == LeaderServer::FeatureJobState::Staging;
            const bool no_bundle = !attempt.has_result && attempt.inputs.empty() &&
                attempt.outputs.empty() && attempt.plan_digest.empty() &&
                attempt.request_digest.empty() && attempt.result_error.empty();
            const bool unstaged = no_bundle &&
                (staging || attempt.snapshot.state == LeaderServer::FeatureJobState::Failed ||
                 attempt.snapshot.state == LeaderServer::FeatureJobState::Superseded);
            if (staging && !unstaged)
                throw std::runtime_error("invalid staging feature attempt");
            if (!unstaged && (attempt.plan_digest.size() != 64 ||
                attempt.request_digest.size() != 64 ||
                FeatureManifestJson(attempt.inputs).size() >
                    kMaxFeatureManifestJsonBytes)) {
                throw std::runtime_error("invalid staged feature metadata");
            }
            if ((!attempt.has_result &&
                 (!attempt.outputs.empty() || !attempt.result_error.empty())) ||
                (attempt.has_result && attempt.result_error.size() != 64) ||
                (!attempt.current &&
                 attempt.snapshot.state != LeaderServer::FeatureJobState::Superseded) ||
                (attempt.current &&
                 attempt.snapshot.state == LeaderServer::FeatureJobState::Superseded) ||
                (attempt.snapshot.state == LeaderServer::FeatureJobState::Succeeded &&
                 (!attempt.has_result ||
                  attempt.outcome != wire::FeatureResult::Outcome::Succeeded ||
                  attempt.snapshot.progress != std::optional<double>(1.0)))
                )
                throw std::runtime_error("inconsistent persisted feature attempt state");
            auto within_budget = [&](const std::vector<TransferFile>& files) {
                std::uint64_t total = 0;
                for (const auto& file : files) {
                    if (total > attempt.disk_budget_bytes ||
                        file.size > attempt.disk_budget_bytes - total)
                        return false;
                    total += file.size;
                }
                return true;
            };
            if (!unstaged && (!within_budget(attempt.inputs) ||
                             FeatureManifestJson(attempt.outputs).size() >
                                 kMaxFeatureManifestJsonBytes ||
                             !within_budget(attempt.outputs)))
                throw std::runtime_error("persisted feature manifest exceeds its limits");
            if (!unstaged) {
                wire::FeatureOffer offer;
                offer.job_id = attempt.snapshot.job_id;
                offer.attempt_id = attempt_id;
                offer.plan_digest = attempt.plan_digest;
                offer.request_digest = attempt.request_digest;
                offer.required_build = attempt.required_build;
                offer.expires_at_ms = 2;
                offer.inputs = attempt.inputs;
                wire::Message message;
                message.payload = std::move(offer);
                std::string encoded;
                if (wire::Encode(message, 1, encoded) != wire::Error::None)
                    throw std::runtime_error("invalid persisted input offer");
            }
            if (attempt.has_result) {
                wire::FeatureResult result;
                result.job_id = attempt.snapshot.job_id;
                result.attempt_id = attempt_id;
                result.outcome = attempt.outcome;
                result.outputs = attempt.outputs;
                wire::Message message;
                message.payload = std::move(result);
                std::string encoded;
                if (wire::Encode(message, 1, encoded) != wire::Error::None)
                    throw std::runtime_error("invalid persisted worker result");
            }
            attempt.snapshot.result_root = path.parent_path() /
                std::filesystem::u8path(attempt.snapshot.job_id) /
                std::filesystem::u8path(attempt_id) / "output";
            attempt.attempt_root = attempt.snapshot.result_root.parent_path();
            if (!attempts.emplace(attempt_key, std::move(attempt)).second)
                throw std::runtime_error("duplicate persisted feature attempt");
            const auto& loaded = attempts.at(attempt_key);
            if (loaded.current &&
                !current.emplace(loaded.snapshot.job_id, attempt_key).second)
                throw std::runtime_error("multiple current attempts for a feature job");
        }
    } catch (const std::exception& exception) {
        SetError(error, std::string("invalid feature job state: ") + exception.what());
        return false;
    }
    return true;
}

#endif  // SS_TOOL_SFM
bool AtomicWriteAgentJobStore(const std::filesystem::path& directory,
                              const char* file_name,
                              const std::string& bytes,
                              std::string* error) {
    static std::atomic<std::uint64_t> serial{0};
    const std::string temporary_name =
        std::string(file_name) + ".tmp-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
        "-" + std::to_string(serial.fetch_add(1, std::memory_order_relaxed));
    const auto io_directory = NativeFilesystemPath(directory);
#ifdef _WIN32
    const auto temporary = io_directory / std::wstring(
        temporary_name.begin(), temporary_name.end());
    const auto destination = io_directory / std::wstring(
        file_name, file_name + std::strlen(file_name));
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        SetError(error, "could not create agent job state temporary file");
        return false;
    }
    bool ok = true;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD count = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, count, &written, nullptr) ||
            written != count) {
            ok = false;
            break;
        }
        offset += written;
    }
    if (ok) ok = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    if (ok) ok = MoveFileExW(temporary.c_str(), destination.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(temporary.c_str());
#else
    const int dirfd = ::open(io_directory.c_str(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dirfd < 0) {
        SetError(error, "could not open agent job state directory");
        return false;
    }
    const std::string target = file_name;
    const std::string temporary = temporary_name;
    const int fd = ::openat(dirfd, temporary.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        ::close(dirfd);
        SetError(error, "could not create agent job state temporary file");
        return false;
    }
    bool ok = ::fchmod(fd, 0600) == 0;
    std::size_t offset = 0;
    while (ok && offset < bytes.size()) {
        const ssize_t written = ::write(fd, bytes.data() + offset,
                                        bytes.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { ok = false; break; }
        offset += static_cast<std::size_t>(written);
    }
    if (ok) ok = ::fsync(fd) == 0;
    ::close(fd);
    if (ok) ok = ::renameat(dirfd, temporary.c_str(), dirfd, target.c_str()) == 0;
    if (ok) ok = ::fsync(dirfd) == 0;
    if (!ok) ::unlinkat(dirfd, temporary.c_str(), 0);
    ::close(dirfd);
#endif
    if (!ok) SetError(error, "could not atomically persist agent job state");
    return ok;
}
#ifdef SS_TOOL_TRAIN
constexpr char kTrainingStoreDirectory[] = "agent-training-jobs";
constexpr char kTrainingStoreFile[] = "jobs.json";
constexpr std::size_t kMaxTrainingAttempts = 64;
constexpr std::size_t kMaxTrainingStoreBytes = 64 * 1024 * 1024;
constexpr std::size_t kMaxTrainingManifestJsonBytes = 256 * 1024;
constexpr std::uint64_t kMaxTrainingDiskBudgetBytes = 1ULL << 40;
constexpr std::size_t kMaxTrainingStagingTasks = 2;
constexpr std::uint64_t kTrainingOfferLifetimeMs = 60'000;

struct TrainingAttempt final {
    LeaderServer::TrainingJobSnapshot snapshot;
    std::filesystem::path attempt_root;
    std::vector<TransferFile> inputs;
    std::vector<TransferFile> outputs;
    std::string result_checkpoint;
    std::string result_error_sha256;
    std::uint64_t expires_at_ms = 0;
    std::uint64_t disk_budget_bytes = 0;
    wire::PortableResult::Outcome outcome = wire::PortableResult::Outcome::Failed;
    bool has_result = false;
    bool current = true;
};

struct TrainingStageTask final {
    std::string job_id;
    std::string attempt_id;
    std::string worker_id;
    TrainConfig config;
    std::string preset;
    std::filesystem::path resume_checkpoint;
    std::string required_build;
    std::uint64_t disk_budget_bytes = 0;
};

const char* TrainingStateName(LeaderServer::TrainingJobState state) noexcept {
    using State = LeaderServer::TrainingJobState;
    switch (state) {
        case State::Staging: return "staging";
        case State::Queued: return "queued";
        case State::Offered: return "offered";
        case State::TransferringInputs: return "transferring-inputs";
        case State::Running: return "running";
        case State::Unknown: return "unknown";
        case State::ReceivingOutput: return "receiving-output";
        case State::Succeeded: return "succeeded";
        case State::Failed: return "failed";
        case State::Interrupted: return "interrupted";
        case State::Rejected: return "rejected";
        case State::Superseded: return "superseded";
    }
    return "failed";
}

LeaderServer::TrainingJobState ParseTrainingState(const std::string& name) {
    using State = LeaderServer::TrainingJobState;
    if (name == "staging") return State::Staging;
    if (name == "queued") return State::Queued;
    if (name == "offered") return State::Offered;
    if (name == "transferring-inputs") return State::TransferringInputs;
    if (name == "running") return State::Running;
    if (name == "unknown") return State::Unknown;
    if (name == "receiving-output") return State::ReceivingOutput;
    if (name == "succeeded") return State::Succeeded;
    if (name == "failed") return State::Failed;
    if (name == "interrupted") return State::Interrupted;
    if (name == "rejected") return State::Rejected;
    if (name == "superseded") return State::Superseded;
    throw std::runtime_error("invalid persisted training state");
}

const char* TrainingOutcomeName(wire::PortableResult::Outcome outcome) noexcept {
    switch (outcome) {
        case wire::PortableResult::Outcome::Succeeded: return "succeeded";
        case wire::PortableResult::Outcome::Failed: return "failed";
        case wire::PortableResult::Outcome::Interrupted: return "interrupted";
    }
    return "failed";
}

wire::PortableResult::Outcome ParseTrainingOutcome(const std::string& name) {
    if (name == "succeeded") return wire::PortableResult::Outcome::Succeeded;
    if (name == "failed") return wire::PortableResult::Outcome::Failed;
    if (name == "interrupted") return wire::PortableResult::Outcome::Interrupted;
    throw std::runtime_error("invalid persisted training outcome");
}

bool TerminalTrainingState(LeaderServer::TrainingJobState state) noexcept {
    using State = LeaderServer::TrainingJobState;
    return state == State::Succeeded || state == State::Failed ||
           state == State::Interrupted || state == State::Rejected ||
           state == State::Superseded;
}

bool ValidTrainingId(const std::string& id) noexcept {
    if (id.empty() || id.size() > wire::kMaxFeatureIdBytes ||
        id == "." || id == "..")
        return false;
    for (const unsigned char ch : id)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.'))
            return false;
    return true;
}

std::string TrainingAttemptKey(const std::string& job_id,
                               const std::string& attempt_id) {
    std::string key;
    key.reserve(job_id.size() + 1 + attempt_id.size());
    key.append(job_id);
    key.push_back('\0');
    key.append(attempt_id);
    return key;
}

bool ValidTrainingDigest(const std::string& digest) noexcept {
    if (digest.size() != 64) return false;
    for (char c : digest)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

std::string TrainingErrorDigest(const std::string& text) {
    spirula::Sha256 hash;
    hash.update(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    return hash.hex();
}

std::string TrainingManifestJson(const std::vector<TransferFile>& files) {
    JsonWriter writer;
    writer.array();
    for (const TransferFile& file : files) {
        writer.object()
            .field("path", file.path)
            .field("size", std::to_string(file.size))
            .field("sha256", file.sha256)
        .end();
    }
    writer.end();
    return writer.str();
}

std::string TrainingConfigJson(const TrainConfig& config,
                               const std::string& preset) {
    JsonWriter writer;
    writer.object().field("preset", preset);
    for (const auto& [key, value] : train_config_json_pairs(config))
        writer.field_raw(key, value);
    return writer.end().str();
}

void WriteTrainingConfig(const std::filesystem::path& path,
                         const std::string& bytes) {
    std::ofstream output(NativeFilesystemPath(path),
                         std::ios::binary | std::ios::trunc);
    if (!output ||
        !output.write(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("could not restore staged training config");
    output.flush();
    if (!output)
        throw std::runtime_error("could not flush staged training config");
}


const JsonValue& TrainingField(const JsonValue& object, const char* name,
                               JsonValue::Type type) {
    const JsonValue* value = object.find(name);
    if (!value || value->type != type)
        throw std::runtime_error(std::string("invalid training store field: ") + name);
    return *value;
}

std::uint64_t TrainingUint(const JsonValue& object, const char* name) {
    const std::string& text =
        TrainingField(object, name, JsonValue::Type::String).str;
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size())
        throw std::runtime_error(std::string("invalid training store integer: ") + name);
    return value;
}

std::vector<TransferFile> ParseTrainingManifest(const JsonValue& value) {
    if (value.type != JsonValue::Type::Array ||
        value.arr.size() > wire::kMaxManifestEntries)
        throw std::runtime_error("invalid training store manifest");
    std::vector<TransferFile> files;
    files.reserve(value.arr.size());
    for (const JsonValue& item : value.arr) {
        if (!item.is_object())
            throw std::runtime_error("invalid training store manifest entry");
        TransferFile file;
        file.path = TrainingField(item, "path", JsonValue::Type::String).str;
        const std::string& size =
            TrainingField(item, "size", JsonValue::Type::String).str;
        const auto parsed = std::from_chars(size.data(), size.data() + size.size(),
                                            file.size);
        if (size.empty() || parsed.ec != std::errc{} ||
            parsed.ptr != size.data() + size.size())
            throw std::runtime_error("invalid training store manifest size");
        file.sha256 = TrainingField(item, "sha256", JsonValue::Type::String).str;
        files.push_back(std::move(file));
    }
    return files;
}

std::string TrainingAttemptsJson(
    const std::map<std::string, TrainingAttempt>& attempts) {
    JsonWriter writer;
    writer.object().field("schema_version", 1).key("attempts").array();
    for (const auto& entry : attempts) {
        const TrainingAttempt& attempt = entry.second;
        writer.object()
            .field("job_id", attempt.snapshot.job_id)
            .field("attempt_id", attempt.snapshot.attempt_id)
            .field("worker_id", attempt.snapshot.worker_id)
            .field("paired_leader_id", attempt.snapshot.paired_leader_id)
            .field("paired_leader_epoch",
                   std::to_string(attempt.snapshot.paired_leader_epoch))
            .field("state", TrainingStateName(attempt.snapshot.state))
            .field("error", attempt.snapshot.error)
            .field("required_build", attempt.snapshot.required_build)
            .field("input_identity_sha256",
                   attempt.snapshot.input_identity_sha256)
            .field("expires_at_ms", std::to_string(attempt.expires_at_ms))
            .field("disk_budget_bytes", std::to_string(attempt.disk_budget_bytes))
            .field("has_result", attempt.has_result)
            .field("outcome", TrainingOutcomeName(attempt.outcome))
            .field("result_error_sha256", attempt.result_error_sha256)
            .field("returned_checkpoint", attempt.result_checkpoint)
            .field("current", attempt.current)
            .field_raw("inputs", TrainingManifestJson(attempt.inputs))
            .field_raw("outputs", TrainingManifestJson(attempt.outputs))
        .end();
    }
    writer.end().end();
    return writer.str();
}

bool ReadTrainingStore(
    const std::filesystem::path& path,
    std::map<std::string, TrainingAttempt>& attempts,
    std::map<std::string, std::string>& current,
    const std::string& leader_id, std::uint64_t leader_epoch,
    std::string* error) {
    const auto io_path = NativeFilesystemPath(path);
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(io_path, ec);
    if (ec == std::errc::no_such_file_or_directory) return true;
    if (ec || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status)) {
        SetError(error, "training job state file is unsafe");
        return false;
    }
    const auto size = std::filesystem::file_size(io_path, ec);
    if (ec || size > kMaxTrainingStoreBytes) {
        SetError(error, "training job state file exceeds its limit");
        return false;
    }
    std::ifstream input(io_path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof()) {
        SetError(error, "could not read training job state file");
        return false;
    }
    try {
        const JsonValue root = json_parse(bytes);
        if (!root.is_object() ||
            TrainingField(root, "schema_version", JsonValue::Type::Number).num != 1.0)
            throw std::runtime_error("unsupported training job state schema");
        const JsonValue& list =
            TrainingField(root, "attempts", JsonValue::Type::Array);
        if (list.arr.size() > kMaxTrainingAttempts)
            throw std::runtime_error("training attempt capacity exceeded");
        for (const JsonValue& item : list.arr) {
            if (!item.is_object())
                throw std::runtime_error("invalid training attempt");
            TrainingAttempt attempt;
            attempt.snapshot.job_id =
                TrainingField(item, "job_id", JsonValue::Type::String).str;
            attempt.snapshot.attempt_id =
                TrainingField(item, "attempt_id", JsonValue::Type::String).str;
            attempt.snapshot.worker_id =
                TrainingField(item, "worker_id", JsonValue::Type::String).str;
            attempt.snapshot.paired_leader_id =
                TrainingField(item, "paired_leader_id", JsonValue::Type::String).str;
            attempt.snapshot.paired_leader_epoch =
                TrainingUint(item, "paired_leader_epoch");
            attempt.snapshot.state = ParseTrainingState(
                TrainingField(item, "state", JsonValue::Type::String).str);
            attempt.snapshot.error =
                TrainingField(item, "error", JsonValue::Type::String).str;
            attempt.snapshot.required_build =
                TrainingField(item, "required_build", JsonValue::Type::String).str;
            attempt.snapshot.input_identity_sha256 =
                TrainingField(item, "input_identity_sha256",
                              JsonValue::Type::String).str;
            attempt.expires_at_ms = TrainingUint(item, "expires_at_ms");
            attempt.disk_budget_bytes = TrainingUint(item, "disk_budget_bytes");
            attempt.has_result =
                TrainingField(item, "has_result", JsonValue::Type::Bool).b;
            attempt.outcome = ParseTrainingOutcome(
                TrainingField(item, "outcome", JsonValue::Type::String).str);
            attempt.result_error_sha256 =
                TrainingField(item, "result_error_sha256",
                              JsonValue::Type::String).str;
            attempt.result_checkpoint =
                TrainingField(item, "returned_checkpoint",
                              JsonValue::Type::String).str;
            attempt.current =
                TrainingField(item, "current", JsonValue::Type::Bool).b;
            attempt.snapshot.current = attempt.current;
            attempt.inputs = ParseTrainingManifest(
                TrainingField(item, "inputs", JsonValue::Type::Array));
            attempt.outputs = ParseTrainingManifest(
                TrainingField(item, "outputs", JsonValue::Type::Array));

            const std::string& job_id = attempt.snapshot.job_id;
            const std::string& attempt_id = attempt.snapshot.attempt_id;
            const std::string attempt_key = TrainingAttemptKey(job_id, attempt_id);
            if (!ValidTrainingId(job_id) || !ValidTrainingId(attempt_id) ||
                !ValidTrainingId(attempt.snapshot.worker_id) ||
                !ValidTrainingId(attempt.snapshot.paired_leader_id) ||
                !attempt.snapshot.paired_leader_epoch ||
                attempt.snapshot.paired_leader_id != leader_id ||
                attempt.snapshot.paired_leader_epoch != leader_epoch ||
                attempt.snapshot.required_build.empty() ||
                attempt.snapshot.required_build.size() > wire::kMaxBuildBytes ||
                !attempt.disk_budget_bytes ||
                attempt.disk_budget_bytes > kMaxTrainingDiskBudgetBytes ||
                attempt.snapshot.error.size() > 4096)
                throw std::runtime_error("invalid persisted training identity");

            const bool no_bundle = attempt.inputs.empty() &&
                attempt.snapshot.input_identity_sha256.empty();
            const bool staging =
                attempt.snapshot.state == LeaderServer::TrainingJobState::Staging;
            if ((staging && (!no_bundle || attempt.has_result)) ||
                (!no_bundle &&
                 (attempt.inputs.empty() ||
                  !ValidTrainingDigest(attempt.snapshot.input_identity_sha256) ||
                  TrainingManifestJson(attempt.inputs).size() >
                      kMaxTrainingManifestJsonBytes)) ||
                (no_bundle && attempt.has_result))
                throw std::runtime_error("invalid staged training metadata");
            if ((!attempt.has_result &&
                 (!attempt.outputs.empty() ||
                  !attempt.result_error_sha256.empty() ||
                  !attempt.result_checkpoint.empty())) ||
                (attempt.has_result &&
                 (!ValidTrainingDigest(attempt.result_error_sha256) ||
                  (attempt.outcome ==
                       wire::PortableResult::Outcome::Succeeded &&
                   attempt.result_checkpoint.empty()))) ||
                (!attempt.current &&
                 attempt.snapshot.state != LeaderServer::TrainingJobState::Superseded) ||
                (attempt.current &&
                 attempt.snapshot.state == LeaderServer::TrainingJobState::Superseded) ||
                (attempt.snapshot.state == LeaderServer::TrainingJobState::Succeeded &&
                 (!attempt.has_result ||
                  attempt.outcome != wire::PortableResult::Outcome::Succeeded ||
                  attempt.outputs.empty())) ||
                (attempt.has_result &&
                 attempt.outcome == wire::PortableResult::Outcome::Succeeded &&
                 attempt.outputs.empty()) ||
                (attempt.has_result &&
                 attempt.outcome != wire::PortableResult::Outcome::Succeeded &&
                 (!attempt.outputs.empty() ||
                  !attempt.result_checkpoint.empty())))
                throw std::runtime_error("inconsistent persisted training result");

            auto within_budget = [&](const std::vector<TransferFile>& files) {
                std::uint64_t total = 0;
                for (const TransferFile& file : files) {
                    if (total > attempt.disk_budget_bytes ||
                        file.size > attempt.disk_budget_bytes - total)
                        return false;
                    total += file.size;
                }
                return true;
            };
            if (!no_bundle &&
                (!within_budget(attempt.inputs) ||
                 !within_budget(attempt.outputs) ||
                 TrainingManifestJson(attempt.outputs).size() >
                     kMaxTrainingManifestJsonBytes))
                throw std::runtime_error("persisted training manifest exceeds its limits");
            if (!no_bundle) {
                wire::PortableOffer offer;
                offer.workload = wire::PortableWorkload::Training;
                offer.job_id = job_id;
                offer.attempt_id = attempt_id;
                offer.input_identity_sha256 =
                    attempt.snapshot.input_identity_sha256;
                offer.required_build = attempt.snapshot.required_build;
                offer.expires_at_ms = 2;
                offer.inputs = attempt.inputs;
                wire::Message message;
                message.payload = std::move(offer);
                std::string encoded;
                if (wire::Encode(message, 1, encoded) != wire::Error::None)
                    throw std::runtime_error("invalid persisted training offer");
            }
            if (attempt.has_result) {
                wire::PortableResult result;
                result.workload = wire::PortableWorkload::Training;
                result.job_id = job_id;
                result.attempt_id = attempt_id;
                result.outcome = attempt.outcome;
                result.outputs = attempt.outputs;
                result.output_metadata = attempt.result_checkpoint;
                wire::Message message;
                message.payload = std::move(result);
                std::string encoded;
                if (wire::Encode(message, 1, encoded) != wire::Error::None)
                    throw std::runtime_error("invalid persisted training result");
            }
            const auto store_root = path.parent_path();
            attempt.snapshot.result_root = store_root /
                std::filesystem::u8path(job_id) /
                std::filesystem::u8path(attempt_id) / "result";
            attempt.attempt_root = attempt.snapshot.result_root.parent_path();
            if (!attempts.emplace(attempt_key, std::move(attempt)).second)
                throw std::runtime_error("duplicate persisted training attempt");
            const TrainingAttempt& loaded = attempts.at(attempt_key);
            if (loaded.current &&
                !current.emplace(loaded.snapshot.job_id, attempt_key).second)
                throw std::runtime_error(
                    "multiple authoritative attempts for a training job");
        }
    } catch (const std::exception& exception) {
        SetError(error, std::string("invalid training job state: ") +
                            exception.what());
        return false;
    }
    return true;
}
#endif


bool JsonUnsigned(const JsonValue* value, std::uint64_t maximum,
                  std::uint64_t& result) noexcept {
    if (!value || value->type != JsonValue::Type::Number ||
        !std::isfinite(value->num) || value->num < 0 ||
        value->num > static_cast<double>(maximum) ||
        std::floor(value->num) != value->num)
        return false;
    result = static_cast<std::uint64_t>(value->num);
    return true;
}

bool JsonString(const JsonValue* value, std::size_t maximum,
                std::string& result) {
    if (!value || value->type != JsonValue::Type::String ||
        value->str.size() > maximum)
        return false;
    result = value->str;
    return true;
}

bool JsonBool(const JsonValue* value, bool& result) noexcept {
    if (!value || value->type != JsonValue::Type::Bool) return false;
    result = value->b;
    return true;
}

}  // namespace

struct LeaderServer::Impl final {
    struct Session;

    struct PendingCommand {
        wire::Command command;
        CommandResult result;
        bool accepted = false;
        bool ever_sent = false;
        bool terminal = false;
        bool waiter_notified = false;
        std::weak_ptr<Session> sent_session;
    };

    struct Session final {
        NativeSocket socket = TlsChannel::kInvalidSocket;
        NativeSocket cancel_socket = TlsChannel::kInvalidSocket;
        std::mutex cancel_mutex;
        std::mutex write_mutex;
        std::atomic<bool> stopping{false};
        std::atomic<bool> active{false};
        std::atomic<bool> done{false};
        std::thread thread;
        std::string worker_id;
        TlsChannel::PeerFingerprint peer_fingerprint{};
        mutable std::mutex mutex;
        std::string last_admin_transfer_command_id;
        std::string admin_transfer_in_progress;
        bool has_status = false;
        wire::Status status = DisconnectedStatus();
        std::string error;
        std::uint64_t last_seen_unix_ms = 0;
        std::optional<wire::FeatureDecision::Step> waiting_feature_step;
        std::string waiting_feature_attempt;
        std::optional<wire::PortableDecision::Step> waiting_portable_step;
        std::optional<wire::PortableWorkload> waiting_portable_workload;
        std::string waiting_portable_attempt;
        std::string waiting_portable_job;


        void Abort() noexcept {
            stopping.store(true, std::memory_order_release);
            std::lock_guard<std::mutex> lock(cancel_mutex);
            ShutdownSocket(FromNative(cancel_socket));
        }
    };

    Options options;
    pairing::Leader pairing_leader;
    const std::string update_signer_fingerprint;
    SocketRuntime socket_runtime;
    RawSocket enrollment_listener = kInvalidSocket;
    RawSocket operational_listener = kInvalidSocket;
    std::uint16_t enrollment_port = 0;
    std::uint16_t operational_port = 0;
    mutable std::mutex enrollment_listener_mutex;
    mutable std::mutex enrollment_cancel_mutex;
    RawSocket enrollment_cancel_socket = kInvalidSocket;
    mutable std::mutex operational_listener_mutex;
    std::atomic<bool> stopping{false};
    std::atomic<bool> running{false};
    std::thread enrollment_thread;
    std::thread operational_thread;
    mutable std::mutex stop_mutex;
    bool stopped = false;
    mutable std::mutex pairing_mutex;
    mutable std::mutex sessions_mutex;
    std::map<std::string, std::shared_ptr<Session>> worker_sessions;
    std::vector<std::shared_ptr<Session>> sessions;
    mutable std::mutex command_mutex;
    std::uint64_t command_sequence = 0;
    std::vector<std::shared_ptr<PendingCommand>> command_history;
    std::map<std::string, std::shared_ptr<PendingCommand>> active_commands;
#if defined(SS_TOOL_SFM) || defined(SS_TOOL_TRAIN)
    mutable std::mutex assignment_mutex;
#endif
#ifdef SS_TOOL_SFM
    mutable std::mutex feature_mutex;
    std::filesystem::path feature_store_directory;
    std::map<std::string, FeatureAttempt> feature_attempts;
    std::map<std::string, std::string> current_feature_attempts;
    std::set<std::string> reserved_feature_attempts;
    std::set<std::string> reserved_feature_jobs;
    std::set<std::string> reserved_feature_workers;
    std::mutex feature_staging_mutex;
    std::condition_variable feature_staging_condition;
    std::deque<FeatureStageTask> feature_staging_queue;
    bool feature_staging_active = false;
    bool feature_staging_stopping = false;
    std::size_t feature_staging_reserved = 0;

    mutable std::mutex reconstruction_mutex;
    std::filesystem::path reconstruction_store_directory;
    std::map<std::string, ReconstructionAttempt> reconstruction_attempts;
    std::map<std::string, std::string> current_reconstruction_attempts;
    std::set<std::string> reserved_reconstruction_attempts;
    std::set<std::string> reserved_reconstruction_jobs;
    std::set<std::string> reserved_reconstruction_workers;
    std::mutex reconstruction_staging_mutex;
    std::condition_variable reconstruction_staging_condition;
    std::deque<ReconstructionStageTask> reconstruction_staging_queue;
    bool reconstruction_staging_active = false;
    bool reconstruction_staging_stopping = false;
    std::size_t reconstruction_staging_reserved = 0;
#endif
#ifdef SS_TOOL_TRAIN
    mutable std::mutex training_mutex;
    std::filesystem::path training_store_directory;
    std::map<std::string, TrainingAttempt> training_attempts;
    std::map<std::string, std::string> current_training_attempts;
    std::set<std::string> reserved_training_attempts;
    std::set<std::string> reserved_training_jobs;
    std::set<std::string> reserved_training_workers;
    std::mutex training_staging_mutex;
    std::condition_variable training_staging_condition;
    std::deque<TrainingStageTask> training_staging_queue;
    bool training_staging_active = false;
    bool training_staging_stopping = false;
    std::size_t training_staging_reserved = 0;
#endif

    Impl(Options opts, pairing::Leader leader,
         std::string signer_fingerprint) noexcept
        : options(std::move(opts)), pairing_leader(std::move(leader)),
          update_signer_fingerprint(std::move(signer_fingerprint)) {}

    std::shared_ptr<Session> CurrentWorkerSession(
        const std::string& worker_id, wire::Status* status,
        std::string* error) const {
        std::shared_ptr<Session> session;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex);
            const auto found = worker_sessions.find(worker_id);
            if (found != worker_sessions.end()) session = found->second;
        }
        if (!session || !session->active.load(std::memory_order_acquire)) {
            SetError(error, "worker has no active operational session");
            return {};
        }
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (!session->has_status ||
                session->stopping.load(std::memory_order_acquire)) {
                SetError(error, "worker has not sent an operational status");
                return {};
            }
            if (status) *status = session->status;
        }
        return session;
    }

    std::optional<CommandResult> QueueAdminCommand(
        const std::string& worker_id, const std::shared_ptr<Session>& session,
        admin::Operation operation,
        std::optional<update::PackageManifest> manifest,
        std::uint64_t security_version, bool confirmed, std::string* error) {
        if (!confirmed) {
            SetError(error, "remote admin request requires explicit confirmation");
            return std::nullopt;
        }
        const bool activating = operation == admin::Operation::Activate;
        if ((activating && (!manifest || !security_version)) ||
            (!activating && (manifest || security_version))) {
            SetError(error, "remote admin request fields are inconsistent");
            return std::nullopt;
        }

        std::unique_lock<std::mutex> lock(command_mutex);
        if (!RefreshExpiredCommandsLocked(UnixNowMs(), error))
            return std::nullopt;
        if (active_commands.count(worker_id)) {
            SetError(error, "worker already has a live command");
            return std::nullopt;
        }
        if (command_sequence >= kMaxSafeInteger) {
            SetError(error, "leader command sequence is exhausted");
            return std::nullopt;
        }
        if (command_history.size() >= kMaxCommandHistory &&
            std::none_of(command_history.begin(), command_history.end(),
                         [](const auto& item) { return item->terminal; })) {
            SetError(error, "durable command history is full");
            return std::nullopt;
        }

        const std::uint64_t sequence = command_sequence + 1;
        const std::string command_id = "cmd-" + std::to_string(sequence);
        const std::uint64_t issued = UnixNowMs();
        const std::uint64_t issued_unix = issued / 1000;
        if (!issued || issued > kMaxSafeInteger || !issued_unix ||
            issued_unix > kMaxSafeInteger / 1000 - 300) {
            SetError(error, "system clock cannot represent admin expiration");
            return std::nullopt;
        }
        const std::uint64_t expires_unix = issued_unix + 300;
        const std::uint64_t expires = expires_unix * 1000;
        if (expires <= issued || expires > kMaxSafeInteger) {
            SetError(error, "system clock cannot represent admin expiration");
            return std::nullopt;
        }
        const auto deadline = Clock::now() +
            std::chrono::milliseconds(expires - issued);

        auto pending = std::make_shared<PendingCommand>();
        pending->command.command_id = command_id;
        pending->command.sequence = sequence;
        pending->command.action = activating
            ? wire::CommandAction::ActivateUpdate
            : wire::CommandAction::RebootMachine;
        pending->command.confirmed = true;
        pending->command.issued_at_ms = issued;
        pending->command.expires_at_ms = expires;
        pending->result.worker_id = worker_id;
        pending->result.command_id = command_id;
        pending->result.sequence = sequence;
        pending->result.action = pending->command.action;
        pending->result.confirmed = true;
        pending->result.issued_at_ms = issued;
        pending->result.expires_at_ms = expires;
        pending->result.state = CommandState::Pending;

        {
            std::lock_guard<std::mutex> pairing_lock(pairing_mutex);
            {
                std::lock_guard<std::mutex> sessions_lock(sessions_mutex);
                const auto current = worker_sessions.find(worker_id);
                if (current == worker_sessions.end() ||
                    current->second != session ||
                    !session->active.load(std::memory_order_acquire) ||
                    session->stopping.load(std::memory_order_acquire)) {
                    SetError(error, "worker session changed before admin request");
                    return std::nullopt;
                }
            }
            const auto paired = pairing_leader.WorkerForPeer(
                session->peer_fingerprint);
            if (!paired || paired->id != worker_id ||
                paired->status != pairing::WorkerStatus::Paired ||
                paired->revoked) {
                SetError(error, "admin request target is not currently paired");
                return std::nullopt;
            }

            wire::Status status;
            {
                std::lock_guard<std::mutex> session_lock(session->mutex);
                if (!session->has_status ||
                    session->stopping.load(std::memory_order_acquire)) {
                    SetError(error, "worker has not sent an operational status");
                    return std::nullopt;
                }
                status = session->status;
            }
            if (activating &&
                !AdvertisedPlatformMatches(status.platform, manifest->os)) {
                SetError(error, "update package OS does not match worker status");
                return std::nullopt;
            }

            pending->command.leader_id = pairing_leader.LeaderId();
            pending->command.leader_epoch = pairing_leader.LeaderEpoch();
            admin::Intent intent;
            intent.operation = operation;
            intent.worker_id = worker_id;
            intent.leader_id = pending->command.leader_id;
            intent.leader_epoch = pending->command.leader_epoch;
            intent.intent_id = command_id;
            intent.issued_at_unix = issued_unix;
            intent.expires_at_unix = expires_unix;
            if (activating) {
                auto offer = update::SignPackageOffer(pairing_leader, worker_id,
                    expires_unix, command_id, *manifest, error);
                if (!offer) return std::nullopt;
                intent.update_id = command_id;
                intent.package_sha256 = offer->manifest.sha256;
                intent.security_version = security_version;
                pending->command.package_offer = std::move(*offer);
            }
            auto signed_intent = admin::SignIntent(
                pairing_leader, std::move(intent), error);
            if (!signed_intent) return std::nullopt;
            pending->command.admin_intent = std::move(*signed_intent);
        }

        wire::Message validation;
        validation.payload = pending->command;
        if (wire::Validate(validation, issued) != wire::Error::None) {
            SetError(error, "signed admin request failed wire validation");
            return std::nullopt;
        }
        if (!PersistSequence(options.state_root, sequence, error))
            return std::nullopt;
        command_sequence = sequence;
        const auto previous_history = command_history;
        command_history.push_back(pending);
        if (!PruneCommandHistoryLocked()) {
            command_history = previous_history;
            SetError(error, "durable command history is full");
            return std::nullopt;
        }
        if (!PersistCommandStateLocked(error)) {
            command_history = previous_history;
            return std::nullopt;
        }
        active_commands.emplace(worker_id, pending);

        while (!pending->waiter_notified && !pending->accepted &&
               !pending->terminal) {
            if (condition.wait_until(lock, deadline) == std::cv_status::timeout &&
                !pending->waiter_notified && !pending->accepted &&
                !pending->terminal) {
                const CommandResult previous = pending->result;
                const bool previous_terminal = pending->terminal;
                pending->result.state = pending->ever_sent
                    ? CommandState::Uncertain : CommandState::Expired;
                pending->terminal = !pending->ever_sent;
                pending->waiter_notified = true;
                if (pending->terminal) active_commands.erase(worker_id);
                if (!PersistCommandStateLocked(error)) {
                    pending->result = previous;
                    pending->terminal = previous_terminal;
                    pending->waiter_notified = false;
                    if (!previous_terminal)
                        active_commands[worker_id] = pending;
                    return std::nullopt;
                }
            }
        }
        return pending->result;
    }

    static bool TerminalCommandState(CommandState state) noexcept {
        return state == CommandState::Applied ||
               state == CommandState::Rejected ||
               state == CommandState::Failed ||
               state == CommandState::Expired;
    }

    bool PersistCommandStateLocked(std::string* error) const {
        JsonWriter writer;
        writer.object()
            .field("version", 2)
            .field("sequence", static_cast<long long>(command_sequence))
            .key("commands").array();
        for (const auto& item : command_history) {
            const CommandResult& result = item->result;
            const wire::Command& command = item->command;
            std::string admin_payload;
            if (IsAdminCommand(command.action)) {
                wire::Message message;
                message.payload = command;
                if (wire::Encode(message, command.issued_at_ms, admin_payload) !=
                    wire::Error::None) {
                    SetError(error, "could not encode signed admin command history");
                    return false;
                }
            }
            writer.object()
                .field("worker_id", result.worker_id)
                .field("command_id", command.command_id)
                .field("leader_id", command.leader_id)
                .field("leader_epoch", static_cast<long long>(command.leader_epoch))
                .field("sequence", static_cast<long long>(command.sequence))
                .field("action", static_cast<int>(command.action))
                .field("target_job_id", command.target_job_id)
                .field("confirmed", command.confirmed)
                .field("issued_at_ms", static_cast<long long>(command.issued_at_ms))
                .field("expires_at_ms", static_cast<long long>(command.expires_at_ms))
                .field("state", static_cast<int>(result.state))
                .field("acknowledgment", static_cast<int>(result.acknowledgment))
                .field("acknowledgment_received", result.acknowledgment_received)
                .field("accepted", item->accepted)
                .field("ever_sent", item->ever_sent)
                .field("terminal", item->terminal)
                .field("admin_payload", admin_payload)
                .end();
        }
        writer.end().end();
        return PersistDurableText(options.state_root, kCommandStateFile,
            writer.str(), kMaxCommandStateBytes, error,
            "durable command history");
    }

    bool PruneCommandHistoryLocked() {
        while (command_history.size() > kMaxCommandHistory) {
            const auto oldest_terminal = std::find_if(
                command_history.begin(), command_history.end(),
                [](const auto& item) { return item->terminal; });
            if (oldest_terminal == command_history.end()) return false;
            command_history.erase(oldest_terminal);
        }
        return true;
    }
    bool RefreshExpiredCommandsLocked(std::uint64_t now,
                                      std::string* error) {
        struct Previous final {
            std::string worker_id;
            std::shared_ptr<PendingCommand> command;
            CommandResult result;
            bool terminal = false;
            bool waiter_notified = false;
        };
        std::vector<Previous> changed;
        for (auto it = active_commands.begin(); it != active_commands.end();) {
            const auto& command = it->second;
            const bool admin_resolution_timed_out =
                IsAdminCommand(command->command.action) &&
                command->ever_sent &&
                now >= command->command.expires_at_ms &&
                now - command->command.expires_at_ms >=
                    kAdminResolutionGraceMs;
            if ((command->accepted && !admin_resolution_timed_out) ||
                command->command.expires_at_ms > now) {
                ++it;
                continue;
            }
            const CommandState state = command->ever_sent
                ? CommandState::Uncertain : CommandState::Expired;
            const bool terminal = admin_resolution_timed_out ||
                                  !command->ever_sent;
            if (command->result.state == state &&
                command->terminal == terminal && command->waiter_notified) {
                ++it;
                continue;
            }
            changed.push_back({it->first, command, command->result,
                               command->terminal, command->waiter_notified});
            command->result.state = state;
            command->terminal = terminal;
            command->waiter_notified = true;
            if (terminal)
                it = active_commands.erase(it);
            else
                ++it;
        }
        if (changed.empty()) return true;
        if (!PersistCommandStateLocked(error)) {
            for (auto& previous : changed) {
                previous.command->result = std::move(previous.result);
                previous.command->terminal = previous.terminal;
                previous.command->waiter_notified = previous.waiter_notified;
                active_commands[previous.worker_id] = previous.command;
            }
            return false;
        }
        condition.notify_all();
        return true;
    }

    bool InitializeCommandState(std::string* error) {
        std::string bytes;
        bool exists = false;
        if (!LoadDurableText(options.state_root, kCommandStateFile,
                kMaxCommandStateBytes, bytes, exists, error,
                "durable command history file"))
            return false;
        if (!exists) return true;

        JsonValue root;
        try {
            root = json_parse(bytes);
        } catch (const std::exception&) {
            SetError(error, "durable command history file is malformed");
            return false;
        }
        std::uint64_t version = 0;
        std::uint64_t saved_sequence = 0;
        const JsonValue* version_value = root.find("version");
        const JsonValue* sequence_value = root.find("sequence");
        const JsonValue* commands_value = root.find("commands");
        if (!root.is_object() || root.obj.size() != 3 ||
            !JsonUnsigned(version_value, 2, version) ||
            (version != 1 && version != 2) ||
            !JsonUnsigned(sequence_value, kMaxSafeInteger, saved_sequence) ||
            saved_sequence > command_sequence || !commands_value ||
            !commands_value->is_array() ||
            commands_value->arr.size() > kMaxCommandHistory) {
            SetError(error, "durable command history file is malformed");
            return false;
        }
        const bool legacy_version = version == 1;

        const std::string current_leader_id = pairing_leader.LeaderId();
        const std::uint64_t current_leader_epoch = pairing_leader.LeaderEpoch();
        std::set<std::string> command_ids;
        std::set<std::uint64_t> sequences;
        std::uint64_t greatest_sequence = 0;
        bool normalized = false;
        const std::uint64_t now = UnixNowMs();
        for (const JsonValue& value : commands_value->arr) {
            if (!value.is_object() ||
                value.obj.size() != (legacy_version ? 16u : 17u)) {
                SetError(error, "durable command history entry is malformed");
                return false;
            }
            std::string worker_id, command_id, leader_id, target_job_id;
            std::string admin_payload;
            std::uint64_t leader_epoch = 0, sequence = 0, action_number = 0;
            std::uint64_t issued_at_ms = 0, expires_at_ms = 0;
            std::uint64_t state_number = 0, acknowledgment_number = 0;
            bool confirmed = false, acknowledgment_received = false;
            bool accepted = false, ever_sent = false, terminal = false;
            if (!JsonString(value.find("worker_id"), 128, worker_id) ||
                worker_id.empty() ||
                !JsonString(value.find("command_id"), 64, command_id) ||
                !JsonString(value.find("leader_id"), 64, leader_id) ||
                !JsonString(value.find("target_job_id"), 128, target_job_id) ||
                !JsonUnsigned(value.find("leader_epoch"), kMaxSafeInteger,
                              leader_epoch) || !leader_epoch ||
                !JsonUnsigned(value.find("sequence"), kMaxSafeInteger, sequence) ||
                !sequence ||
                !JsonUnsigned(value.find("action"), 255, action_number) ||
                !JsonUnsigned(value.find("issued_at_ms"), kMaxSafeInteger,
                              issued_at_ms) || !issued_at_ms ||
                !JsonUnsigned(value.find("expires_at_ms"), kMaxSafeInteger,
                              expires_at_ms) || expires_at_ms <= issued_at_ms ||
                !JsonUnsigned(value.find("state"), 255, state_number) ||
                !JsonUnsigned(value.find("acknowledgment"), 255,
                              acknowledgment_number) ||
                !JsonBool(value.find("confirmed"), confirmed) ||
                !JsonBool(value.find("acknowledgment_received"),
                          acknowledgment_received) ||
                !JsonBool(value.find("accepted"), accepted) ||
                !JsonBool(value.find("ever_sent"), ever_sent) ||
                !JsonBool(value.find("terminal"), terminal) ||
                (!legacy_version &&
                 !JsonString(value.find("admin_payload"),
                             wire::kMaxMessageBytes, admin_payload))) {
                SetError(error, "durable command history entry is malformed");
                return false;
            }

            auto item = std::make_shared<PendingCommand>();
            item->command.command_id = std::move(command_id);
            item->command.leader_id = std::move(leader_id);
            item->command.leader_epoch = leader_epoch;
            item->command.sequence = sequence;
            item->command.action =
                static_cast<wire::CommandAction>(action_number);
            item->command.target_job_id = std::move(target_job_id);
            item->command.confirmed = confirmed;
            item->command.issued_at_ms = issued_at_ms;
            item->command.expires_at_ms = expires_at_ms;
            if (!admin_payload.empty()) {
                wire::Message persisted;
                if (wire::Decode(admin_payload, issued_at_ms, persisted) !=
                        wire::Error::None ||
                    !std::holds_alternative<wire::Command>(persisted.payload)) {
                    SetError(error, "durable signed admin command payload is malformed");
                    return false;
                }
                const wire::Command& command =
                    std::get<wire::Command>(persisted.payload);
                if (command.command_id != item->command.command_id ||
                    command.leader_id != item->command.leader_id ||
                    command.leader_epoch != item->command.leader_epoch ||
                    command.sequence != item->command.sequence ||
                    command.action != item->command.action ||
                    command.target_job_id != item->command.target_job_id ||
                    command.confirmed != item->command.confirmed ||
                    command.issued_at_ms != item->command.issued_at_ms ||
                    command.expires_at_ms != item->command.expires_at_ms) {
                    SetError(error, "durable signed admin command snapshot conflicts");
                    return false;
                }
                item->command = command;
            } else if (IsAdminCommand(item->command.action)) {
                SetError(error, "durable admin command has no signed payload");
                return false;
            }
            item->result.worker_id = std::move(worker_id);
            item->result.command_id = item->command.command_id;
            item->result.sequence = sequence;
            item->result.action = item->command.action;
            item->result.target_job_id = item->command.target_job_id;
            item->result.confirmed = confirmed;
            item->result.issued_at_ms = issued_at_ms;
            item->result.expires_at_ms = item->command.expires_at_ms;
            item->result.state = static_cast<CommandState>(state_number);
            item->result.acknowledgment =
                static_cast<wire::AcknowledgmentOutcome>(acknowledgment_number);
            item->result.acknowledgment_received = acknowledgment_received;
            item->accepted = accepted;
            item->ever_sent = ever_sent;
            item->terminal = terminal;


            wire::Message validation;
            validation.payload = item->command;
            if (!wire::IsValid(item->command.action) ||
                !wire::IsValid(item->result.acknowledgment) ||
                state_number > static_cast<std::uint64_t>(
                    CommandState::Disconnected) ||
                wire::Validate(validation, issued_at_ms) != wire::Error::None ||
                item->command.command_id !=
                    "cmd-" + std::to_string(item->command.sequence) ||
                item->command.leader_id != current_leader_id ||
                item->command.leader_epoch != current_leader_epoch ||
                !command_ids.insert(item->command.command_id).second ||
                !sequences.insert(item->command.sequence).second ||
                item->command.sequence <= greatest_sequence ||
                terminal != (TerminalCommandState(item->result.state) ||
                    (IsAdminCommand(item->command.action) &&
                     item->result.state == CommandState::Uncertain &&
                     item->ever_sent &&
                     ((item->accepted &&
                       item->result.acknowledgment_received &&
                       item->result.acknowledgment ==
                           wire::AcknowledgmentOutcome::Accepted) ||
                      (!item->accepted &&
                       !item->result.acknowledgment_received &&
                       item->result.acknowledgment ==
                           wire::AcknowledgmentOutcome::Failed))))) {
                SetError(error, "durable command history entry is inconsistent");
                return false;
            }
            if (item->command.action == wire::CommandAction::ActivateUpdate &&
                !terminal &&
                (expires_at_ms > now ||
                 (item->ever_sent && !item->accepted &&
                  now >= expires_at_ms &&
                  now - expires_at_ms < kAdminResolutionGraceMs))) {
                const auto& offer = *item->command.package_offer;
                const auto staged = options.state_root / "agent-pairing" /
                    kLeaderUpdateStore / offer.manifest.sha256 / "package.bin";
                if (!VerifyStagedPackage(staged, offer.manifest.size,
                                         offer.manifest.sha256, error))
                    return false;
            }
            greatest_sequence = item->command.sequence;
            if (!acknowledgment_received &&
                item->result.acknowledgment !=
                    wire::AcknowledgmentOutcome::Failed) {
                SetError(error, "durable command history entry is inconsistent");
                return false;
            }
            if (accepted && !acknowledgment_received) {
                SetError(error, "durable command history entry is inconsistent");
                return false;
            }
            if (acknowledgment_received &&
                item->result.acknowledgment ==
                    wire::AcknowledgmentOutcome::Accepted && !accepted) {
                SetError(error, "durable command history entry is inconsistent");
                return false;
            }
            if (item->terminal) {
                const bool matching_terminal = item->ever_sent &&
                    item->result.acknowledgment_received &&
                    ((item->result.state == CommandState::Applied &&
                      item->result.acknowledgment ==
                          wire::AcknowledgmentOutcome::Completed) ||
                     (item->result.state == CommandState::Rejected &&
                      item->result.acknowledgment ==
                          wire::AcknowledgmentOutcome::Rejected) ||
                     (item->result.state == CommandState::Failed &&
                      item->result.acknowledgment ==
                          wire::AcknowledgmentOutcome::Failed));
                const bool expired_unsent =
                    item->result.state == CommandState::Expired &&
                    !item->ever_sent && !item->accepted &&
                    !item->result.acknowledgment_received;
                const bool timed_out_admin =
                    IsAdminCommand(item->command.action) &&
                    item->result.state == CommandState::Uncertain &&
                    item->ever_sent &&
                    ((item->accepted &&
                      item->result.acknowledgment_received &&
                      item->result.acknowledgment ==
                          wire::AcknowledgmentOutcome::Accepted) ||
                     (!item->accepted &&
                      !item->result.acknowledgment_received &&
                      item->result.acknowledgment ==
                          wire::AcknowledgmentOutcome::Failed));
                if ((!matching_terminal && !timed_out_admin &&
                     item->result.state != CommandState::Expired) ||
                    (item->result.state == CommandState::Expired &&
                     !expired_unsent)) {
                    SetError(error, "durable command history entry is inconsistent");
                    return false;
                }
            } else if ((item->result.state == CommandState::Uncertain &&
                        (!item->ever_sent || item->accepted)) ||
                       (item->accepted && !acknowledgment_received) ||
                       (acknowledgment_received &&
                        item->result.acknowledgment !=
                            wire::AcknowledgmentOutcome::Accepted) ||
                       (item->accepted &&
                        item->result.state != CommandState::Pending)) {
                SetError(error, "durable command history entry is inconsistent");
                return false;
            }

            const bool admin_resolution_timed_out =
                IsAdminCommand(item->command.action) && item->ever_sent &&
                now >= item->command.expires_at_ms &&
                now - item->command.expires_at_ms >=
                    kAdminResolutionGraceMs;
            if (!item->terminal && admin_resolution_timed_out) {
                item->result.state = CommandState::Uncertain;
                item->terminal = true;
                normalized = true;
            } else if (!item->terminal && !item->accepted &&
                       item->command.expires_at_ms <= now) {
                if (item->ever_sent) {
                    item->result.state = CommandState::Uncertain;
                } else {
                    item->result.state = CommandState::Expired;
                    item->terminal = true;
                }
                normalized = true;
            } else if (!item->terminal && item->accepted &&
                       item->result.state != CommandState::Pending) {
                item->result.state = CommandState::Pending;
                normalized = true;
            }
            command_history.push_back(item);
            if (!item->terminal &&
                !active_commands.emplace(item->result.worker_id, item).second) {
                SetError(error, "multiple live commands exist for one worker");
                return false;
            }
        }
        if (greatest_sequence > saved_sequence) {
            SetError(error, "durable command history sequence is inconsistent");
            return false;
        }
        if (normalized && !PersistCommandStateLocked(error)) return false;
        return true;
    }

#ifdef SS_TOOL_SFM
    bool PersistFeatureJobsLocked(std::string* error) {
        const std::string bytes = FeatureAttemptsJson(feature_attempts);
        if (bytes.size() > kMaxFeatureStoreBytes) {
            SetError(error, "feature job state exceeds its storage limit");
            return false;
        }
        return AtomicWriteAgentJobStore(feature_store_directory,
                                        kFeatureStoreFile, bytes, error);
    }

    bool InitializeFeatureJobs(std::string* error) {
        feature_store_directory = options.state_root / kFeatureStoreDirectory;
        const auto io_directory = NativeFilesystemPath(feature_store_directory);
        std::error_code ec;
        std::filesystem::create_directories(io_directory, ec);
        const auto status = std::filesystem::symlink_status(io_directory, ec);
        if (ec || !std::filesystem::is_directory(status) ||
            std::filesystem::is_symlink(status)) {
            SetError(error, "feature job state directory is unsafe");
            return false;
        }
#ifndef _WIN32
        std::filesystem::permissions(io_directory,
            std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace, ec);
        if (ec) {
            SetError(error, "could not secure feature job state directory");
            return false;
        }
#endif
        if (!ReadFeatureStore(feature_store_directory / kFeatureStoreFile,
                              feature_attempts, current_feature_attempts, error))
            return false;
        std::set<std::string> active_workers;
        bool changed = false;
        for (auto& [attempt_key, attempt] : feature_attempts) {
            if (attempt_key != FeatureAttemptKey(
                    attempt.snapshot.job_id, attempt.snapshot.attempt_id)) {
                SetError(error, "inconsistent feature attempt index");
                return false;
            }
            if (!attempt.current) {
                if (!TerminalFeatureState(attempt.snapshot.state)) {
                    SetError(error, "superseded feature attempt is not terminal");
                    return false;
                }
                continue;
            }
            if (TerminalFeatureState(attempt.snapshot.state)) continue;
            if (!active_workers.insert(attempt.snapshot.worker_id).second) {
                SetError(error, "multiple active feature attempts target one worker");
                return false;
            }
            if (attempt.snapshot.state == LeaderServer::FeatureJobState::Staging) {
                attempt.snapshot.state = LeaderServer::FeatureJobState::Failed;
                attempt.snapshot.error = "leader restarted during input staging";
                std::error_code cleanup_error;
                std::filesystem::remove_all(
                    NativeFilesystemPath(attempt.attempt_root), cleanup_error);
                changed = true;
                continue;
            }
            if (attempt.snapshot.state != LeaderServer::FeatureJobState::Queued) {
                attempt.snapshot.state = LeaderServer::FeatureJobState::Unknown;
                attempt.snapshot.error = "leader restarted; worker outcome is unknown";
                changed = true;
            }
            try {
                const auto request = sfm::feature_work::readRequestFile(
                    NativeFilesystemPath(attempt.attempt_root / "input" /
                        "request.json").u8string());
                const auto plan_relative =
                    std::filesystem::u8path(request.plan_path);
                if (plan_relative.empty() || plan_relative.is_absolute() ||
                    plan_relative.has_root_path())
                    throw std::runtime_error("staged plan path is not relative");
                for (const auto& part : plan_relative)
                    if (part == "..")
                        throw std::runtime_error("staged plan path escapes its bundle");
                const auto plan = sfm::feature_work::readPlanFile(
                    NativeFilesystemPath(
                        attempt.attempt_root / "input" / plan_relative).u8string());
                if (request.attempt_id != attempt.snapshot.attempt_id ||
                    sfm::feature_work::planDigest(plan) != attempt.plan_digest ||
                    sfm::feature_work::requestDigest(request) != attempt.request_digest)
                    throw std::runtime_error("staged feature identity changed");
                VerifyFeatureInputs(attempt.attempt_root / "input", plan, request,
                                    attempt.inputs, attempt.disk_budget_bytes);
            } catch (const std::exception& exception) {
                attempt.snapshot.state = LeaderServer::FeatureJobState::Failed;
                attempt.snapshot.error = std::string("staged inputs unavailable: ") +
                                         exception.what();
                attempt.snapshot.error.resize(std::min<std::size_t>(
                    attempt.snapshot.error.size(), 4096));
                changed = true;
            }
        }
        return !changed || PersistFeatureJobsLocked(error);
    }
    bool PersistReconstructionJobsLocked(std::string* error) {
        const std::string bytes = ReconstructionAttemptsJson(reconstruction_attempts);
        if (bytes.size() > kMaxReconstructionStoreBytes) {
            SetError(error, "reconstruction job state exceeds its storage limit");
            return false;
        }
        return AtomicWriteAgentJobStore(reconstruction_store_directory,
                                        kReconstructionStoreFile, bytes, error);
    }

    bool InitializeReconstructionJobs(std::string* error) {
        reconstruction_store_directory =
            options.state_root / kReconstructionStoreDirectory;
        const auto io_directory =
            NativeFilesystemPath(reconstruction_store_directory);
        std::error_code ec;
        std::filesystem::create_directories(io_directory, ec);
        const auto status = std::filesystem::symlink_status(io_directory, ec);
        if (ec || !std::filesystem::is_directory(status) ||
            std::filesystem::is_symlink(status)) {
            SetError(error, "reconstruction job state directory is unsafe");
            return false;
        }
#ifndef _WIN32
        std::filesystem::permissions(io_directory,
            std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace, ec);
        if (ec) {
            SetError(error, "could not secure reconstruction job state directory");
            return false;
        }
#endif
        if (!ReadReconstructionStore(
                reconstruction_store_directory / kReconstructionStoreFile,
                reconstruction_attempts, current_reconstruction_attempts,
                pairing_leader.LeaderId(), pairing_leader.LeaderEpoch(), error))
            return false;
        std::set<std::string> active_workers;
        bool changed = false;
        for (auto& [attempt_key, attempt] : reconstruction_attempts) {
            if (attempt_key != FeatureAttemptKey(
                    attempt.snapshot.job_id, attempt.snapshot.attempt_id)) {
                SetError(error, "inconsistent reconstruction attempt index");
                return false;
            }
            if (!attempt.current) {
                if (attempt.snapshot.state !=
                    LeaderServer::ReconstructionJobState::Superseded) {
                    SetError(error, "superseded reconstruction attempt is not terminal");
                    return false;
                }
                std::filesystem::remove_all(
                    NativeFilesystemPath(attempt.snapshot.result_root), ec);
                ec.clear();
                std::filesystem::remove_all(
                    NativeFilesystemPath(attempt.attempt_root / "output-staging"), ec);
                ec.clear();
                continue;
            }
            const bool previously_succeeded = attempt.snapshot.state ==
                LeaderServer::ReconstructionJobState::Succeeded;
            if (TerminalReconstructionState(attempt.snapshot.state) &&
                !previously_succeeded)
                continue;
            if (!TerminalReconstructionState(attempt.snapshot.state) &&
                !active_workers.insert(attempt.snapshot.worker_id).second) {
                SetError(error, "multiple active reconstruction attempts target one worker");
                return false;
            }
            if (attempt.snapshot.state ==
                LeaderServer::ReconstructionJobState::Staging) {
                attempt.snapshot.state = LeaderServer::ReconstructionJobState::Failed;
                attempt.snapshot.error = "leader restarted during input staging";
                std::filesystem::remove_all(
                    NativeFilesystemPath(attempt.attempt_root), ec);
                ec.clear();
                changed = true;
                continue;
            }
            if (!TerminalReconstructionState(attempt.snapshot.state)) {
                attempt.snapshot.state = LeaderServer::ReconstructionJobState::Unknown;
                attempt.snapshot.error = "leader restarted; worker outcome is unknown";
                changed = true;
            }
            try {
                const ReconstructionInputBundle inputs =
                    VerifyReconstructionInputs(
                        attempt.attempt_root / "input", attempt.inputs,
                        attempt.snapshot.input_identity_sha256,
                        attempt.disk_budget_bytes);
                if (inputs.build_id != attempt.snapshot.required_build ||
                    inputs.source_manifest_sha256 !=
                        attempt.snapshot.source_manifest_sha256)
                    throw std::runtime_error(
                        "staged reconstruction provenance changed");
                const auto result_status = std::filesystem::symlink_status(
                    NativeFilesystemPath(attempt.snapshot.result_root), ec);
                const bool has_published_result = !ec &&
                    std::filesystem::is_directory(result_status) &&
                    !std::filesystem::is_symlink(result_status);
                ec.clear();
                if (previously_succeeded || has_published_result) {
                    if (!attempt.has_result ||
                        attempt.outcome != wire::PortableResult::Outcome::Succeeded)
                        throw std::runtime_error(
                            "published reconstruction result has no successful result record");
                    VerifyReconstructionOutputs(inputs,
                        attempt.snapshot.result_root,
                        attempt.outputs, attempt.disk_budget_bytes);
                    attempt.snapshot.state =
                        LeaderServer::ReconstructionJobState::Succeeded;
                    attempt.snapshot.error.clear();
                    changed = changed || !previously_succeeded;
                }
            } catch (const std::exception& exception) {
                attempt.snapshot.state = LeaderServer::ReconstructionJobState::Failed;
                attempt.snapshot.error =
                    std::string("staged reconstruction unavailable: ") +
                    exception.what();
                attempt.snapshot.error.resize(std::min<std::size_t>(
                    attempt.snapshot.error.size(), 4096));
                std::filesystem::remove_all(
                    NativeFilesystemPath(attempt.snapshot.result_root), ec);
                ec.clear();
                changed = true;
            }
        }
        return !changed || PersistReconstructionJobsLocked(error);
    }

    bool StartReconstructionStaging(const std::shared_ptr<Impl>& self,
                                    std::string* error) {
        std::thread worker;
        try {
            worker = std::thread([self] { self->ReconstructionStagingLoop(); });
            worker.detach();
            return true;
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(reconstruction_staging_mutex);
                reconstruction_staging_stopping = true;
            }
            reconstruction_staging_condition.notify_all();
            if (worker.joinable()) worker.join();
            SetError(error, "could not start reconstruction input staging worker");
            return false;
        }
    }

    bool StageReconstructionTask(const ReconstructionStageTask& task,
                                 ReconstructionInputBundle& bundle, bool& owned,
                                 std::string& failure) {
        const auto job_directory = reconstruction_store_directory /
                                   std::filesystem::u8path(task.job_id);
        const auto attempt_root = job_directory /
                                  std::filesystem::u8path(task.attempt_id);
        try {
            const auto io_job_directory = NativeFilesystemPath(job_directory);
            const auto io_attempt_root = NativeFilesystemPath(attempt_root);
            const auto io_input_root = io_attempt_root / "input";
            std::error_code ec;
            std::filesystem::create_directories(io_job_directory, ec);
            if (ec) throw std::runtime_error("could not create reconstruction job directory");
            const auto job_status =
                std::filesystem::symlink_status(io_job_directory, ec);
            if (ec || !std::filesystem::is_directory(job_status) ||
                std::filesystem::is_symlink(job_status))
                throw std::runtime_error("reconstruction job directory is unsafe");
            static_cast<void>(std::filesystem::symlink_status(io_attempt_root, ec));
            if (!ec || ec != std::errc::no_such_file_or_directory)
                throw std::runtime_error(
                    "reconstruction attempt directory already exists or is unsafe");
            ec.clear();
            if (!std::filesystem::create_directory(io_attempt_root, ec) || ec)
                throw std::runtime_error("could not create reconstruction attempt directory");
            owned = true;
            const auto input_root = attempt_root / "input";
            if (!std::filesystem::create_directory(io_input_root, ec) || ec)
                throw std::runtime_error("could not create reconstruction input directory");
            if (!std::filesystem::create_directory(
                    io_attempt_root / "output-staging", ec) || ec)
                throw std::runtime_error("could not create reconstruction output staging directory");
            bundle = StageReconstructionInputs(task.request, task.source_manifest,
                                               input_root, task.disk_budget_bytes);
            return true;
        } catch (const std::exception& exception) {
            failure = std::string("could not stage reconstruction inputs: ") +
                      exception.what();
            return false;
        } catch (...) {
            failure = "could not stage reconstruction inputs";
            return false;
        }
    }

    void FinishReconstructionStaging(const ReconstructionStageTask& task,
                                     ReconstructionInputBundle* bundle,
                                     std::string failure, bool owned) noexcept {
        bool keep_root = false;
        try {
            if (stopping.load(std::memory_order_acquire))
                failure = "leader stopped during reconstruction input staging";
            if (bundle && failure.empty()) {
                if (bundle->files.empty() ||
                    !ValidDigest(bundle->identity_sha256) ||
                    (!bundle->source_manifest_sha256.empty() &&
                     !ValidDigest(bundle->source_manifest_sha256)) ||
                    bundle->build_id != task.required_build ||
                    FeatureManifestJson(bundle->files).size() >
                        kMaxReconstructionManifestJsonBytes)
                    throw std::runtime_error(
                        "staged reconstruction bundle metadata is invalid");
                const std::uint64_t now = UnixNowMs();
                if (!now || now > kMaxSafeInteger -
                                     kReconstructionOfferLifetimeMs)
                    throw std::runtime_error(
                        "system clock cannot represent reconstruction offer expiration");
                wire::PortableOffer offer;
                offer.workload = wire::PortableWorkload::Reconstruction;
                offer.job_id = task.job_id;
                offer.attempt_id = task.attempt_id;
                offer.input_identity_sha256 = bundle->identity_sha256;
                offer.required_build = task.required_build;
                offer.expires_at_ms = now + kReconstructionOfferLifetimeMs;
                offer.inputs = bundle->files;
                wire::Message check;
                check.payload = std::move(offer);
                std::string encoded;
                if (wire::Encode(check, now, encoded) != wire::Error::None)
                    throw std::runtime_error(
                        "reconstruction input offer exceeds wire limits");
            }
        } catch (const std::exception& exception) {
            failure = exception.what();
            bundle = nullptr;
        } catch (...) {
            failure = "reconstruction input validation failed";
            bundle = nullptr;
        }
        try {
            if (failure.empty() &&
                !IsWorkerPaired(task.worker_id))
                failure = "reconstruction worker was revoked during input staging";
            std::lock_guard<std::mutex> lock(reconstruction_mutex);
            const auto found = reconstruction_attempts.find(
                FeatureAttemptKey(task.job_id, task.attempt_id));
            if (found != reconstruction_attempts.end() &&
                found->second.snapshot.worker_id == task.worker_id &&
                found->second.snapshot.state ==
                    LeaderServer::ReconstructionJobState::Staging &&
                IsCurrentReconstructionAttemptLocked(found->second)) {
                ReconstructionAttempt& attempt = found->second;
                if (failure.empty() && bundle &&
                    !stopping.load(std::memory_order_acquire)) {
                    ReconstructionAttempt previous = attempt;
                    attempt.inputs = std::move(bundle->files);
                    attempt.snapshot.source_manifest_sha256 =
                        std::move(bundle->source_manifest_sha256);
                    attempt.snapshot.input_identity_sha256 =
                        std::move(bundle->identity_sha256);
                    attempt.snapshot.state =
                        LeaderServer::ReconstructionJobState::Queued;
                    attempt.snapshot.error.clear();
                    std::string persist_error;
                    if (PersistReconstructionJobsLocked(&persist_error)) {
                        keep_root = true;
                    } else {
                        attempt = std::move(previous);
                        attempt.snapshot.state =
                            LeaderServer::ReconstructionJobState::Failed;
                        attempt.snapshot.error =
                            "could not persist staged reconstruction inputs: " +
                            persist_error;
                        attempt.snapshot.error.resize(std::min<std::size_t>(
                            attempt.snapshot.error.size(), 4096));
                        std::string ignored;
                        keep_root = !PersistReconstructionJobsLocked(&ignored);
                    }
                } else {
                    attempt.snapshot.state =
                        LeaderServer::ReconstructionJobState::Failed;
                    attempt.snapshot.error = failure.empty()
                        ? "reconstruction input staging did not complete"
                        : std::move(failure);
                    attempt.snapshot.error.resize(std::min<std::size_t>(
                        attempt.snapshot.error.size(), 4096));
                    std::string ignored;
                    PersistReconstructionJobsLocked(&ignored);
                }
            }
        } catch (...) {
            keep_root = true;
        }
        if (owned && !keep_root) {
            std::error_code ec;
            std::filesystem::remove_all(NativeFilesystemPath(
                reconstruction_store_directory /
                std::filesystem::u8path(task.job_id) /
                std::filesystem::u8path(task.attempt_id)), ec);
        }
    }

    void ReconstructionStagingLoop() noexcept {
        for (;;) {
            ReconstructionStageTask task;
            bool stop_before_stage = false;
            {
                std::unique_lock<std::mutex> lock(reconstruction_staging_mutex);
                reconstruction_staging_condition.wait(lock, [&] {
                    return reconstruction_staging_stopping ||
                           !reconstruction_staging_queue.empty();
                });
                if (reconstruction_staging_queue.empty() &&
                    reconstruction_staging_stopping)
                    return;
                task = std::move(reconstruction_staging_queue.front());
                reconstruction_staging_queue.pop_front();
                reconstruction_staging_active = true;
                stop_before_stage = reconstruction_staging_stopping;
            }
            bool owned = false;
            ReconstructionInputBundle bundle;
            std::string failure;
            bool current_attempt = false;
            {
                std::lock_guard<std::mutex> lock(reconstruction_mutex);
                const auto found = reconstruction_attempts.find(
                    FeatureAttemptKey(task.job_id, task.attempt_id));
                current_attempt = found != reconstruction_attempts.end() &&
                    found->second.snapshot.worker_id == task.worker_id &&
                    found->second.snapshot.state ==
                        LeaderServer::ReconstructionJobState::Staging &&
                    IsCurrentReconstructionAttemptLocked(found->second);
            }
            if (stop_before_stage) {
                failure = "leader stopped before reconstruction staging";
            } else if (!current_attempt) {
                failure = "reconstruction attempt was superseded before staging";
            } else {
                StageReconstructionTask(task, bundle, owned, failure);
            }
            FinishReconstructionStaging(task,
                failure.empty() ? &bundle : nullptr, std::move(failure), owned);
            {
                std::lock_guard<std::mutex> lock(reconstruction_staging_mutex);
                reconstruction_staging_active = false;
            }
        }
    }

    void StopReconstructionStaging() noexcept {
        std::deque<ReconstructionStageTask> abandoned;
        {
            std::lock_guard<std::mutex> lock(reconstruction_staging_mutex);
            reconstruction_staging_stopping = true;
            abandoned.swap(reconstruction_staging_queue);
        }
        reconstruction_staging_condition.notify_all();
        for (const auto& task : abandoned)
            FinishReconstructionStaging(task, nullptr,
                "leader stopped before reconstruction staging", false);
    }

    bool StartFeatureStaging(const std::shared_ptr<Impl>& self,
                             std::string* error) {
        std::thread worker;
        try {
            worker = std::thread([self] { self->FeatureStagingLoop(); });
            worker.detach();
            return true;
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(feature_staging_mutex);
                feature_staging_stopping = true;
            }
            feature_staging_condition.notify_all();
            if (worker.joinable()) worker.join();
            SetError(error, "could not start feature input staging worker");
            return false;
        }
    }

    bool StageFeatureTask(const FeatureStageTask& task,
                          FeatureInputBundle& bundle, bool& owned,
                          std::string& error) {
        const auto job_directory = feature_store_directory /
                                   std::filesystem::u8path(task.job_id);
        const auto attempt_root = job_directory /
                                  std::filesystem::u8path(task.attempt_id);
        try {
            const auto io_job_directory = NativeFilesystemPath(job_directory);
            const auto io_attempt_root = NativeFilesystemPath(attempt_root);
            const auto io_input_root = io_attempt_root / "input";
            std::error_code ec;
            std::filesystem::create_directories(io_job_directory, ec);
            if (ec) throw std::runtime_error("could not create feature job directory");
            const auto job_status =
                std::filesystem::symlink_status(io_job_directory, ec);
            if (ec || !std::filesystem::is_directory(job_status) ||
                std::filesystem::is_symlink(job_status))
                throw std::runtime_error("feature job directory is unsafe");
            static_cast<void>(std::filesystem::symlink_status(io_attempt_root, ec));
            if (!ec || ec != std::errc::no_such_file_or_directory)
                throw std::runtime_error("feature attempt directory already exists or is unsafe");
            ec.clear();
            if (!std::filesystem::create_directory(io_attempt_root, ec) || ec)
                throw std::runtime_error("could not create feature attempt directory");
            owned = true;
            const auto input_root = attempt_root / "input";
            if (!std::filesystem::create_directory(io_input_root, ec) || ec)
                throw std::runtime_error("could not create feature input directory");
            if (!std::filesystem::create_directory(io_attempt_root / "output", ec) || ec)
                throw std::runtime_error("could not create feature output directory");
            bundle = StageFeatureInputs(task.plan_path, task.request_path,
                task.image_root, task.mask_root, input_root,
                task.disk_budget_bytes);
            return true;
        } catch (const std::exception& exception) {
            error = std::string("could not stage feature inputs: ") + exception.what();
            return false;
        } catch (...) {
            error = "could not stage feature inputs";
            return false;
        }
    }

    void FinishFeatureStaging(const FeatureStageTask& task,
                              FeatureInputBundle* bundle,
                              std::string failure, bool owned) noexcept {
        bool keep_root = false;
        try {
            if (stopping.load(std::memory_order_acquire))
                failure = "leader stopped during input staging";
            if (bundle && failure.empty()) {
                if (bundle->request.attempt_id != task.attempt_id ||
                    bundle->manifest.empty() ||
                    FeatureManifestJson(bundle->manifest).size() >
                        kMaxFeatureManifestJsonBytes)
                    throw std::runtime_error("staged feature bundle identity or manifest is invalid");
                const std::uint64_t now = UnixNowMs();
                if (!now || now > kMaxSafeInteger - kFeatureOfferLifetimeMs)
                    throw std::runtime_error("system clock cannot represent feature offer expiration");
                wire::FeatureOffer offer;
                offer.job_id = task.job_id;
                offer.attempt_id = task.attempt_id;
                offer.plan_digest = sfm::feature_work::planDigest(bundle->plan);
                offer.request_digest = sfm::feature_work::requestDigest(bundle->request);
                offer.required_build = task.required_build;
                offer.expires_at_ms = now + kFeatureOfferLifetimeMs;
                offer.inputs = bundle->manifest;
                wire::Message message;
                message.payload = std::move(offer);
                std::string encoded;
                if (wire::Encode(message, now, encoded) != wire::Error::None)
                    throw std::runtime_error("feature input offer exceeds wire limits");
            }
        } catch (const std::exception& exception) {
            failure = exception.what();
            bundle = nullptr;
        } catch (...) {
            failure = "feature input validation failed";
            bundle = nullptr;
        }
        try {
            std::lock_guard<std::mutex> pairing_lock(pairing_mutex);
            bool paired = false;
            for (const auto& worker : pairing_leader.Workers())
                if (worker.id == task.worker_id &&
                    worker.status == pairing::WorkerStatus::Paired && !worker.revoked) {
                    paired = true;
                    break;
                }
            if (!paired && failure.empty())
                failure = "feature worker was revoked during input staging";
            std::lock_guard<std::mutex> feature_lock(feature_mutex);
            const auto found = feature_attempts.find(
                FeatureAttemptKey(task.job_id, task.attempt_id));
            if (found != feature_attempts.end() &&
                found->second.snapshot.job_id == task.job_id &&
                found->second.snapshot.worker_id == task.worker_id &&
                found->second.snapshot.state == LeaderServer::FeatureJobState::Staging &&
                IsCurrentFeatureAttemptLocked(found->second)) {
                FeatureAttempt& attempt = found->second;
                if (failure.empty() && bundle &&
                    !stopping.load(std::memory_order_acquire)) {
                    FeatureAttempt previous = attempt;
                    attempt.inputs = std::move(bundle->manifest);
                    attempt.plan_digest = sfm::feature_work::planDigest(bundle->plan);
                    attempt.request_digest = sfm::feature_work::requestDigest(bundle->request);
                    attempt.snapshot.state = LeaderServer::FeatureJobState::Queued;
                    attempt.snapshot.error.clear();
                    std::string persist_error;
                    if (PersistFeatureJobsLocked(&persist_error)) {
                        keep_root = true;
                    } else {
                        attempt = std::move(previous);
                        attempt.snapshot.state = LeaderServer::FeatureJobState::Failed;
                        attempt.snapshot.error =
                            "could not persist staged feature inputs: " + persist_error;
                        attempt.snapshot.error.resize(std::min<std::size_t>(
                            attempt.snapshot.error.size(), 4096));
                        keep_root = true;
                    }
                } else {
                    attempt.snapshot.state = LeaderServer::FeatureJobState::Failed;
                    attempt.snapshot.error = failure.empty()
                        ? "feature input staging did not complete" : std::move(failure);
                    attempt.snapshot.error.resize(std::min<std::size_t>(
                        attempt.snapshot.error.size(), 4096));
                    std::string ignored;
                    PersistFeatureJobsLocked(&ignored);
                }
            }
        } catch (...) {
            keep_root = true;
        }
        if (owned && !keep_root) {
            std::error_code ec;
            const auto root = NativeFilesystemPath(feature_store_directory /
                std::filesystem::u8path(task.job_id) /
                std::filesystem::u8path(task.attempt_id));
            std::filesystem::remove_all(root, ec);
        }
    }

    void FeatureStagingLoop() noexcept {
        for (;;) {
            FeatureStageTask task;
            bool stop_before_stage = false;
            {
                std::unique_lock<std::mutex> lock(feature_staging_mutex);
                feature_staging_condition.wait(lock, [&] {
                    return feature_staging_stopping || !feature_staging_queue.empty();
                });
                if (feature_staging_queue.empty() && feature_staging_stopping) return;
                task = std::move(feature_staging_queue.front());
                feature_staging_queue.pop_front();
                feature_staging_active = true;
                stop_before_stage = feature_staging_stopping;
            }
            bool owned = false;
            FeatureInputBundle bundle;
            std::string failure;
            bool current_attempt = false;
            {
                std::lock_guard<std::mutex> lock(feature_mutex);
                const auto found = feature_attempts.find(
                    FeatureAttemptKey(task.job_id, task.attempt_id));
                current_attempt = found != feature_attempts.end() &&
                    found->second.snapshot.job_id == task.job_id &&
                    found->second.snapshot.worker_id == task.worker_id &&
                    found->second.snapshot.state ==
                        LeaderServer::FeatureJobState::Staging &&
                    IsCurrentFeatureAttemptLocked(found->second);
            }
            if (stop_before_stage) {
                failure = "leader stopped before input staging";
            } else if (!current_attempt) {
                failure = "feature attempt was superseded before input staging";
            } else if (!StageFeatureTask(task, bundle, owned, failure)) {
                // StageFeatureTask already set a bounded diagnostic.
            }
            FeatureInputBundle* staged = failure.empty() ? &bundle : nullptr;
            FinishFeatureStaging(task, staged, std::move(failure), owned);
            {
                std::lock_guard<std::mutex> lock(feature_staging_mutex);
                feature_staging_active = false;
            }
        }
    }

    void StopFeatureStaging() noexcept {
        std::deque<FeatureStageTask> abandoned;
        {
            std::lock_guard<std::mutex> lock(feature_staging_mutex);
            feature_staging_stopping = true;
            abandoned.swap(feature_staging_queue);
        }
        feature_staging_condition.notify_all();
        for (const auto& task : abandoned)
            FinishFeatureStaging(task, nullptr,
                "leader stopped before input staging", false);
    }


    bool IsWorkerPaired(const std::string& worker_id) const {
        std::lock_guard<std::mutex> lock(pairing_mutex);
        for (const auto& worker : pairing_leader.Workers())
            if (worker.id == worker_id)
                return worker.status == pairing::WorkerStatus::Paired &&
                       !worker.revoked;
        return false;
    }

    bool IsSessionTrusted(const std::shared_ptr<Session>& session) const {
        if (!session || !session->active.load(std::memory_order_acquire) ||
            session->stopping.load(std::memory_order_acquire))
            return false;
        std::lock_guard<std::mutex> lock(pairing_mutex);
        const auto worker = pairing_leader.WorkerForPeer(session->peer_fingerprint);
        return worker && worker->id == session->worker_id &&
               worker->status == pairing::WorkerStatus::Paired &&
               !worker->revoked;
    }

    bool IsCurrentFeatureAttemptLocked(const FeatureAttempt& attempt) const {
        const auto current = current_feature_attempts.find(attempt.snapshot.job_id);
        return attempt.current && current != current_feature_attempts.end() &&
               current->second == FeatureAttemptKey(
                   attempt.snapshot.job_id, attempt.snapshot.attempt_id);
    }
    bool WorkerHasActiveFeature(const std::string& worker_id) const {
        std::lock_guard<std::mutex> lock(feature_mutex);
        for (const auto& [job_id, attempt_id] : current_feature_attempts) {
            const auto found = feature_attempts.find(attempt_id);
            if (found != feature_attempts.end() &&
                found->second.snapshot.worker_id == worker_id &&
                !TerminalFeatureState(found->second.snapshot.state))
                return true;
        }
        return false;
    }

    bool SetFeatureStateLocked(FeatureAttempt& attempt,
                               LeaderServer::FeatureJobState state,
                               std::string error,
                               std::string* persist_error) {
        FeatureAttempt previous = attempt;
        attempt.snapshot.state = state;
        attempt.snapshot.error = std::move(error);
        if (attempt.snapshot.error.size() > 4096)
            attempt.snapshot.error.resize(4096);
        if (state == LeaderServer::FeatureJobState::Succeeded)
            attempt.snapshot.progress = 1.0;
        if (PersistFeatureJobsLocked(persist_error)) return true;
        attempt = std::move(previous);
        return false;
    }
    bool IsCurrentReconstructionAttemptLocked(
        const ReconstructionAttempt& attempt) const {
        const auto current =
            current_reconstruction_attempts.find(attempt.snapshot.job_id);
        return attempt.current &&
            current != current_reconstruction_attempts.end() &&
            current->second == FeatureAttemptKey(
                attempt.snapshot.job_id, attempt.snapshot.attempt_id);
    }

    ReconstructionAttempt* CurrentReconstructionForWorkerLocked(
        const std::string& worker_id) {
        for (const auto& [job_id, attempt_key] : current_reconstruction_attempts) {
            const auto found = reconstruction_attempts.find(attempt_key);
            if (found != reconstruction_attempts.end() &&
                found->second.snapshot.worker_id == worker_id &&
                !TerminalReconstructionState(found->second.snapshot.state))
                return &found->second;
        }
        return nullptr;
    }

    bool WorkerHasActiveReconstruction(const std::string& worker_id) const {
        std::lock_guard<std::mutex> lock(reconstruction_mutex);
        for (const auto& [job_id, attempt_key] : current_reconstruction_attempts) {
            const auto found = reconstruction_attempts.find(attempt_key);
            if (found != reconstruction_attempts.end() &&
                found->second.snapshot.worker_id == worker_id &&
                !TerminalReconstructionState(found->second.snapshot.state))
                return true;
        }
        return false;
    }

    bool SetReconstructionStateLocked(
        ReconstructionAttempt& attempt, ReconstructionJobState state,
        std::string error, std::string* persist_error) {
        ReconstructionAttempt previous = attempt;
        attempt.snapshot.state = state;
        attempt.snapshot.error = std::move(error);
        if (attempt.snapshot.error.size() > 4096)
            attempt.snapshot.error.resize(4096);
        if (PersistReconstructionJobsLocked(persist_error)) return true;
        attempt = std::move(previous);
        return false;
    }

    bool PersistReconstructionChangeLocked(
        ReconstructionAttempt& attempt,
        const std::function<void(ReconstructionAttempt&)>& change,
        std::string* error) {
        ReconstructionAttempt previous = attempt;
        change(attempt);
        if (PersistReconstructionJobsLocked(error)) return true;
        attempt = std::move(previous);
        return false;
    }

    bool SupportsReconstructionBuild(const wire::Status& status,
                                     const std::string& required_build) const {
        if (status.compatibility != wire::CompatibilityState::Compatible ||
            status.build != required_build || required_build.empty() ||
            required_build == "unknown" ||
            std::find(status.capabilities.begin(), status.capabilities.end(),
                      wire::Capability::Reconstruction) == status.capabilities.end())
            return false;
        std::string gpu = status.gpu;
        std::transform(gpu.begin(), gpu.end(), gpu.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return gpu.find("nvidia") == std::string::npos &&
               gpu.find("vulkan") != std::string::npos;
    }

    bool ReconstructionSessionStatus(const std::shared_ptr<Session>& session,
                                     wire::Status& status) const {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!session->has_status ||
            session->stopping.load(std::memory_order_acquire))
            return false;
        status = session->status;
        return true;
    }

    void MarkReconstructionUnknown(
        const std::shared_ptr<Session>& session) noexcept {
        try {
            if (session->worker_id.empty()) return;
            std::lock_guard<std::mutex> lock(reconstruction_mutex);
            ReconstructionAttempt* attempt =
                CurrentReconstructionForWorkerLocked(session->worker_id);
            if (attempt && !TerminalReconstructionState(attempt->snapshot.state) &&
                attempt->snapshot.state != ReconstructionJobState::Queued &&
                attempt->snapshot.state != ReconstructionJobState::Staging) {
                std::string ignored;
                SetReconstructionStateLocked(*attempt, ReconstructionJobState::Unknown,
                    "worker disconnected; outcome is unknown", &ignored);
            }
        } catch (...) {
        }
    }
#endif  // SS_TOOL_SFM
#ifdef SS_TOOL_TRAIN
    bool PersistTrainingJobsLocked(std::string* error) {
        const std::string bytes = TrainingAttemptsJson(training_attempts);
        if (bytes.size() > kMaxTrainingStoreBytes) {
            SetError(error, "training job state exceeds its storage limit");
            return false;
        }
        return AtomicWriteAgentJobStore(training_store_directory,
                                        kTrainingStoreFile, bytes, error);
    }

    bool IsCurrentTrainingAttemptLocked(const TrainingAttempt& attempt) const {
        const auto current = current_training_attempts.find(
            attempt.snapshot.job_id);
        return attempt.current && current != current_training_attempts.end() &&
            current->second == TrainingAttemptKey(
                attempt.snapshot.job_id, attempt.snapshot.attempt_id);
    }

    TrainingAttempt* CurrentTrainingForWorkerLocked(const std::string& worker_id) {
        for (const auto& [job_id, attempt_key] : current_training_attempts) {
            const auto found = training_attempts.find(attempt_key);
            if (found != training_attempts.end() &&
                found->second.snapshot.worker_id == worker_id &&
                !TerminalTrainingState(found->second.snapshot.state))
                return &found->second;
        }
        return nullptr;
    }

    bool WorkerHasActiveTraining(const std::string& worker_id) const {
        std::lock_guard<std::mutex> lock(training_mutex);
        for (const auto& [job_id, attempt_key] : current_training_attempts) {
            const auto found = training_attempts.find(attempt_key);
            if (found != training_attempts.end() &&
                found->second.snapshot.worker_id == worker_id &&
                !TerminalTrainingState(found->second.snapshot.state))
                return true;
        }
        return false;
    }

    bool SetTrainingStateLocked(TrainingAttempt& attempt,
                                TrainingJobState state, std::string error,
                                std::string* persist_error) {
        TrainingAttempt previous = attempt;
        attempt.snapshot.state = state;
        attempt.snapshot.error = std::move(error);
        if (attempt.snapshot.error.size() > 4096)
            attempt.snapshot.error.resize(4096);
        if (PersistTrainingJobsLocked(persist_error)) return true;
        attempt = std::move(previous);
        return false;
    }

    bool PersistTrainingChangeLocked(
        TrainingAttempt& attempt,
        const std::function<void(TrainingAttempt&)>& change,
        std::string* error) {
        TrainingAttempt previous = attempt;
        change(attempt);
        if (PersistTrainingJobsLocked(error)) return true;
        attempt = std::move(previous);
        return false;
    }

    bool SupportsTrainingBuild(const wire::Status& status,
                               const std::string& required_build) const {
        if (status.compatibility != wire::CompatibilityState::Compatible ||
            status.build != required_build || required_build.empty() ||
            required_build == "unknown" ||
            std::find(status.capabilities.begin(), status.capabilities.end(),
                      wire::Capability::Training) == status.capabilities.end())
            return false;
        std::string gpu = status.gpu;
        std::transform(gpu.begin(), gpu.end(), gpu.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return gpu.find("nvidia") == std::string::npos &&
               gpu.find("vulkan") != std::string::npos;
    }

    bool TrainingSessionStatus(const std::shared_ptr<Session>& session,
                               wire::Status& status) const {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!session->has_status ||
            session->stopping.load(std::memory_order_acquire))
            return false;
        status = session->status;
        return true;
    }

    void SetTrainingResumeSnapshot(TrainingAttempt& attempt,
                                   const TrainingOutputBundle& output) {
        attempt.result_checkpoint = output.returned_checkpoint;
        attempt.snapshot.returned_checkpoint = output.returned_checkpoint;
        attempt.snapshot.checkpoint_dir = LogicalFilesystemPath(
            attempt.snapshot.result_root /
            std::filesystem::u8path(output.returned_checkpoint));
        attempt.snapshot.resume_config = output.resume_config;
        attempt.snapshot.resume_config.resume =
            LogicalFilesystemPath(attempt.snapshot.checkpoint_dir).u8string();
        attempt.snapshot.resume_config.output_dir_prefix =
            LogicalFilesystemPath(
                attempt.snapshot.result_root.parent_path()).u8string();
        attempt.snapshot.resume_config.output_dir_name =
            attempt.snapshot.result_root.filename().u8string();
    }

    std::filesystem::path TrainingWorkspace(
        const TrainingAttempt& attempt) const {
        return attempt.attempt_root / "workspace";
    }

    TrainingOutputBundle VerifyPublishedTrainingResult(
        const TrainingAttempt& attempt, const TrainingInputBundle& inputs) {
        const auto result_root =
            NativeFilesystemPath(attempt.snapshot.result_root);
        const auto result_status = std::filesystem::symlink_status(result_root);
        if (!std::filesystem::is_directory(result_status) ||
            std::filesystem::is_symlink(result_status) ||
            IsReparsePoint(attempt.snapshot.result_root))
            throw std::runtime_error("published training result root is unsafe");
        const auto output_root = inputs.output_root;
        const auto io_output_root = NativeFilesystemPath(output_root);
        std::error_code ec;
        const auto output_parent = output_root.parent_path();
        const auto parent_status =
            std::filesystem::symlink_status(
                NativeFilesystemPath(output_parent), ec);
        if (ec != std::errc::no_such_file_or_directory &&
            (ec || !std::filesystem::is_directory(parent_status) ||
             std::filesystem::is_symlink(parent_status) ||
             IsReparsePoint(output_parent)))
            throw std::runtime_error("training output parent is unsafe");
        ec.clear();
        const auto output_status =
            std::filesystem::symlink_status(io_output_root, ec);
        if (!ec || ec != std::errc::no_such_file_or_directory)
            throw std::runtime_error("training verification output path already exists");
        ec.clear();
        std::filesystem::create_directories(
            NativeFilesystemPath(output_root.parent_path()), ec);
        if (ec) throw std::runtime_error("could not create training output parent");
        std::filesystem::rename(result_root, io_output_root, ec);
        if (ec) throw std::runtime_error("could not open published training output");
        std::vector<std::filesystem::path> config_paths;
        std::string local_config_bytes;
        try {
            const auto frozen_config = std::find_if(
                attempt.inputs.begin(), attempt.inputs.end(),
                [](const TransferFile& file) { return file.path == "config.json"; });
            if (frozen_config == attempt.inputs.end() ||
                frozen_config->size > static_cast<std::uint64_t>(
                    std::numeric_limits<std::streamsize>::max()) ||
                frozen_config->size > std::numeric_limits<std::size_t>::max())
                throw std::runtime_error("frozen training config manifest is invalid");
            std::ifstream frozen_input(
                NativeFilesystemPath(inputs.config_path), std::ios::binary);
            if (!frozen_input)
                throw std::runtime_error("could not read frozen training config");
            std::string worker_config(
                static_cast<std::size_t>(frozen_config->size), '\0');
            frozen_input.read(worker_config.data(),
                static_cast<std::streamsize>(worker_config.size()));
            if (!frozen_input ||
                frozen_input.peek() != std::char_traits<char>::eof() ||
                TrainingErrorDigest(worker_config) != frozen_config->sha256)
                throw std::runtime_error("frozen training config digest changed");
            TrainConfig local_config = inputs.config;
            local_config.data = inputs.dataset_root.u8string();
            local_config_bytes = TrainingConfigJson(local_config, inputs.preset);
            for (const TransferFile& file : attempt.outputs) {
                if (file.path != "config.json" &&
                    (file.path.size() < 12 ||
                     file.path.compare(file.path.size() - 12, 12,
                                       "/config.json") != 0))
                    continue;
                const auto relative = std::filesystem::u8path(file.path);
                if (relative.has_root_path() ||
                    file.path.find_first_of("\\:") != std::string::npos ||
                    file.path.find('\0') != std::string::npos)
                    throw std::runtime_error(
                        "persisted training config path is not relative");
                for (const auto& component : relative)
                    if (component.empty() || component == "." ||
                        component == ".." || component.native().back() == '.' ||
                        component.native().back() == ' ')
                        throw std::runtime_error(
                            "persisted training config path escapes output");
                if (file.size != worker_config.size() ||
                    file.sha256 != frozen_config->sha256)
                    throw std::runtime_error(
                        "persisted worker config manifest does not match frozen inputs");
                const auto path = output_root / std::filesystem::u8path(file.path);
                std::error_code path_error;
                const auto parent = std::filesystem::symlink_status(
                    NativeFilesystemPath(path.parent_path()), path_error);
                if (path_error || !std::filesystem::is_directory(parent) ||
                    std::filesystem::is_symlink(parent) ||
                    IsReparsePoint(path.parent_path()))
                    throw std::runtime_error("published training config parent is unsafe");
                const auto status = std::filesystem::symlink_status(
                    NativeFilesystemPath(path), path_error);
                if (path_error || !std::filesystem::is_regular_file(status) ||
                    std::filesystem::is_symlink(status) || IsReparsePoint(path))
                    throw std::runtime_error("published training config is unsafe");
                const auto size = std::filesystem::file_size(
                    NativeFilesystemPath(path), path_error);
                if (path_error || size != local_config_bytes.size())
                    throw std::runtime_error(
                        "published training config differs from leader binding");
                std::ifstream input(NativeFilesystemPath(path), std::ios::binary);
                std::string bytes(static_cast<std::size_t>(size), '\0');
                input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                if (!input || input.peek() != std::char_traits<char>::eof() ||
                    bytes != local_config_bytes)
                    throw std::runtime_error(
                        "published training config differs from leader binding");
                config_paths.push_back(path);
            }
            if (config_paths.empty())
                throw std::runtime_error(
                    "published training result has no config files");

            // Revalidate the original transferred manifest after proving the only
            // rewritten bytes are the deterministic leader-local config binding.
            for (const auto& path : config_paths)
                WriteTrainingConfig(path, worker_config);
            TrainingOutputPackage package{
                attempt.outputs, attempt.snapshot.input_identity_sha256,
                attempt.result_checkpoint};
            TrainingOutputBundle output = VerifyTrainingOutputs(
                inputs, output_root, package, attempt.disk_budget_bytes);
            std::filesystem::rename(io_output_root, result_root, ec);
            if (ec)
                throw std::runtime_error("could not restore published training output");
            return output;
        } catch (...) {
            for (const auto& path : config_paths) {
                try {
                    WriteTrainingConfig(path, local_config_bytes);
                } catch (...) {
                }
            }
            std::error_code restore_error;
            std::filesystem::rename(io_output_root, result_root, restore_error);
            throw;
        }
    }

    bool PublishTrainingOutput(TrainingAttempt& attempt,
                               const TrainingInputBundle& inputs,
                               TrainingOutputBundle* verified,
                               std::string* error) {
        TrainingOutputPackage package{
            attempt.outputs, attempt.snapshot.input_identity_sha256,
            attempt.result_checkpoint};
        TrainingOutputBundle output = VerifyTrainingOutputs(
            inputs, inputs.output_root, package, attempt.disk_budget_bytes);
        std::error_code ec;
        const auto result_root =
            NativeFilesystemPath(attempt.snapshot.result_root);
        const auto io_output_root = NativeFilesystemPath(inputs.output_root);
        const auto result_status = std::filesystem::symlink_status(result_root, ec);
        if (!ec || ec != std::errc::no_such_file_or_directory)
            throw std::runtime_error("training result path already exists");
        ec.clear();
        std::filesystem::rename(io_output_root, result_root, ec);
        if (ec) throw std::runtime_error("could not publish verified training output");
        SetTrainingResumeSnapshot(attempt, output);
        if (verified) *verified = std::move(output);
        return true;
    }

    bool InitializeTrainingJobs(std::string* error) {
        training_store_directory = options.state_root / kTrainingStoreDirectory;
        const auto io_directory = NativeFilesystemPath(training_store_directory);
        std::error_code ec;
        std::filesystem::create_directories(io_directory, ec);
        const auto status = std::filesystem::symlink_status(io_directory, ec);
        if (ec || !std::filesystem::is_directory(status) ||
            std::filesystem::is_symlink(status)) {
            SetError(error, "training job state directory is unsafe");
            return false;
        }
#ifndef _WIN32
        std::filesystem::permissions(io_directory,
            std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace, ec);
        if (ec) {
            SetError(error, "could not secure training job state directory");
            return false;
        }
#endif
        if (!ReadTrainingStore(training_store_directory / kTrainingStoreFile,
                training_attempts, current_training_attempts,
                pairing_leader.LeaderId(), pairing_leader.LeaderEpoch(), error))
            return false;

        std::set<std::string> active_workers;
        bool changed = false;
        for (auto& [attempt_key, attempt] : training_attempts) {
            if (attempt_key != TrainingAttemptKey(
                    attempt.snapshot.job_id, attempt.snapshot.attempt_id)) {
                SetError(error, "inconsistent training attempt index");
                return false;
            }
            if (!attempt.current) {
                if (attempt.snapshot.state != TrainingJobState::Superseded) {
                    SetError(error, "superseded training attempt is not terminal");
                    return false;
                }
                std::filesystem::remove_all(
                    NativeFilesystemPath(attempt.snapshot.result_root), ec);
                ec.clear();
                std::filesystem::remove_all(
                    NativeFilesystemPath(TrainingWorkspace(attempt) / "output"), ec);
                ec.clear();
                continue;
            }
            const bool previously_succeeded =
                attempt.snapshot.state == TrainingJobState::Succeeded;
            if (TerminalTrainingState(attempt.snapshot.state) &&
                !previously_succeeded)
                continue;
            if (!TerminalTrainingState(attempt.snapshot.state) &&
                !active_workers.insert(attempt.snapshot.worker_id).second) {
                SetError(error, "multiple active training attempts target one worker");
                return false;
            }
            if (attempt.snapshot.state == TrainingJobState::Staging) {
                attempt.snapshot.state = TrainingJobState::Failed;
                attempt.snapshot.error = "leader restarted during input staging";
                std::filesystem::remove_all(
                    NativeFilesystemPath(attempt.attempt_root), ec);
                ec.clear();
                changed = true;
                continue;
            }
            if (!TerminalTrainingState(attempt.snapshot.state)) {
                attempt.snapshot.state = TrainingJobState::Unknown;
                attempt.snapshot.error = "leader restarted; worker outcome is unknown";
                changed = true;
            }
            try {
                const auto workspace = TrainingWorkspace(attempt);
                const TrainingInputBundle inputs = VerifyTrainingInputs(
                    workspace / "input", attempt.inputs,
                    attempt.snapshot.input_identity_sha256,
                    attempt.disk_budget_bytes);
                std::error_code result_error;
                const auto output_parent_status =
                    std::filesystem::symlink_status(
                        NativeFilesystemPath(inputs.output_root.parent_path()),
                        result_error);
                const bool output_parent_missing =
                    result_error == std::errc::no_such_file_or_directory;
                const bool safe_output_parent = !result_error &&
                    std::filesystem::is_directory(output_parent_status) &&
                    !std::filesystem::is_symlink(output_parent_status);
                if (!output_parent_missing &&
                    (result_error || !safe_output_parent))
                    throw std::runtime_error(
                        "training output parent path is unsafe");
                result_error.clear();
                const auto result_status = std::filesystem::symlink_status(
                    NativeFilesystemPath(attempt.snapshot.result_root), result_error);
                const bool result_missing =
                    result_error == std::errc::no_such_file_or_directory;
                const bool has_result_root = !result_error &&
                    std::filesystem::is_directory(result_status) &&
                    !std::filesystem::is_symlink(result_status);
                if (!result_missing && (result_error || !has_result_root))
                    throw std::runtime_error("training result path is unsafe");
                const auto output_status =
                    std::filesystem::symlink_status(
                        NativeFilesystemPath(inputs.output_root), result_error);
                const bool output_missing =
                    result_error == std::errc::no_such_file_or_directory;
                const bool has_workspace_output = !result_error &&
                    std::filesystem::is_directory(output_status) &&
                    !std::filesystem::is_symlink(output_status);
                if (!output_missing && (result_error || !has_workspace_output))
                    throw std::runtime_error("training output staging path is unsafe");

                if (previously_succeeded || has_result_root) {
                    if (!attempt.has_result ||
                        attempt.outcome != wire::PortableResult::Outcome::Succeeded)
                        throw std::runtime_error(
                            "published training result has no successful result record");
                    if (has_result_root) {
                        const TrainingOutputBundle output =
                            VerifyPublishedTrainingResult(attempt, inputs);
                        SetTrainingResumeSnapshot(attempt, output);
                    } else if (has_workspace_output) {
                        PublishTrainingOutput(attempt, inputs, nullptr, error);
                    } else {
                        throw std::runtime_error(
                            "successful training output is missing");
                    }
                    attempt.snapshot.state = TrainingJobState::Succeeded;
                    attempt.snapshot.error.clear();
                    changed = true;
                } else if (attempt.has_result &&
                           attempt.outcome ==
                               wire::PortableResult::Outcome::Succeeded &&
                           has_workspace_output) {
                    try {
                        PublishTrainingOutput(attempt, inputs, nullptr, error);
                        attempt.snapshot.state = TrainingJobState::Succeeded;
                        attempt.snapshot.error.clear();
                        changed = true;
                    } catch (...) {
                        std::filesystem::remove_all(
                            NativeFilesystemPath(inputs.output_root), ec);
                        if (ec)
                            throw std::runtime_error(
                                "could not clear partial training output after restart");
                    }
                } else if (has_workspace_output) {
                    std::filesystem::remove_all(
                        NativeFilesystemPath(inputs.output_root), ec);
                    if (ec)
                        throw std::runtime_error(
                            "could not clear partial training output after restart");
                }
            } catch (const std::exception& exception) {
                attempt.snapshot.state = TrainingJobState::Failed;
                attempt.snapshot.error =
                    std::string("staged training unavailable: ") + exception.what();
                attempt.snapshot.error.resize(std::min<std::size_t>(
                    attempt.snapshot.error.size(), 4096));
                std::filesystem::remove_all(
                    NativeFilesystemPath(attempt.snapshot.result_root), ec);
                ec.clear();
                changed = true;
            }
        }
        return !changed || PersistTrainingJobsLocked(error);
    }

    bool StartTrainingStaging(const std::shared_ptr<Impl>& self,
                              std::string* error) {
        std::thread worker;
        try {
            worker = std::thread([self] { self->TrainingStagingLoop(); });
            worker.detach();
            return true;
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(training_staging_mutex);
                training_staging_stopping = true;
            }
            training_staging_condition.notify_all();
            if (worker.joinable()) worker.join();
            SetError(error, "could not start training input staging worker");
            return false;
        }
    }

    bool StageTrainingTask(const TrainingStageTask& task,
                           TrainingInputBundle& bundle, bool& owned,
                           std::string& failure) {
        const auto job_directory =
            training_store_directory / std::filesystem::u8path(task.job_id);
        const auto attempt_root =
            job_directory / std::filesystem::u8path(task.attempt_id);
        try {
            const auto io_job_directory = NativeFilesystemPath(job_directory);
            const auto io_attempt_root = NativeFilesystemPath(attempt_root);
            std::error_code ec;
            std::filesystem::create_directories(io_job_directory, ec);
            if (ec)
                throw std::runtime_error("could not create training job directory");
            const auto job_status =
                std::filesystem::symlink_status(io_job_directory, ec);
            if (ec || !std::filesystem::is_directory(job_status) ||
                std::filesystem::is_symlink(job_status))
                throw std::runtime_error("training job directory is unsafe");
            static_cast<void>(std::filesystem::symlink_status(io_attempt_root, ec));
            if (!ec || ec != std::errc::no_such_file_or_directory)
                throw std::runtime_error(
                    "training attempt directory already exists or is unsafe");
            ec.clear();
            if (!std::filesystem::create_directory(io_attempt_root, ec) || ec)
                throw std::runtime_error("could not create training attempt directory");
            owned = true;
            const auto workspace = attempt_root / "workspace";
            if (!std::filesystem::create_directory(
                    NativeFilesystemPath(workspace), ec) || ec)
                throw std::runtime_error("could not create training workspace");
            bundle = StageTrainingInputs(task.config, task.preset,
                task.resume_checkpoint, workspace, task.disk_budget_bytes);
            return true;
        } catch (const std::exception& exception) {
            failure = std::string("could not stage training inputs: ") +
                      exception.what();
            return false;
        } catch (...) {
            failure = "could not stage training inputs";
            return false;
        }
    }

    void FinishTrainingStaging(const TrainingStageTask& task,
                               TrainingInputBundle* bundle,
                               std::string failure, bool owned) noexcept {
        bool keep_root = false;
        try {
            if (stopping.load(std::memory_order_acquire))
                failure = "leader stopped during training input staging";
            if (bundle && failure.empty()) {
                if (bundle->files.empty() ||
                    !ValidTrainingDigest(bundle->identity_sha256) ||
                    TrainingManifestJson(bundle->files).size() >
                        kMaxTrainingManifestJsonBytes)
                    throw std::runtime_error(
                        "staged training bundle identity or manifest is invalid");
                const std::uint64_t now = UnixNowMs();
                if (!now || now > kMaxSafeInteger - kTrainingOfferLifetimeMs)
                    throw std::runtime_error(
                        "system clock cannot represent training offer expiration");
                wire::PortableOffer offer;
                offer.workload = wire::PortableWorkload::Training;
                offer.job_id = task.job_id;
                offer.attempt_id = task.attempt_id;
                offer.input_identity_sha256 = bundle->identity_sha256;
                offer.required_build = task.required_build;
                offer.expires_at_ms = now + kTrainingOfferLifetimeMs;
                offer.inputs = bundle->files;
                wire::Message message;
                message.payload = std::move(offer);
                std::string encoded;
                if (wire::Encode(message, now, encoded) != wire::Error::None)
                    throw std::runtime_error("training input offer exceeds wire limits");
            }
        } catch (const std::exception& exception) {
            failure = exception.what();
            bundle = nullptr;
        } catch (...) {
            failure = "training input validation failed";
            bundle = nullptr;
        }
        try {
            std::lock_guard<std::mutex> pairing_lock(pairing_mutex);
            bool paired = false;
            for (const auto& worker : pairing_leader.Workers())
                if (worker.id == task.worker_id &&
                    worker.status == pairing::WorkerStatus::Paired &&
                    !worker.revoked) {
                    paired = true;
                    break;
                }
            if (!paired && failure.empty())
                failure = "training worker was revoked during input staging";
            std::lock_guard<std::mutex> lock(training_mutex);
            const auto found = training_attempts.find(
                TrainingAttemptKey(task.job_id, task.attempt_id));
            if (found != training_attempts.end() &&
                found->second.snapshot.worker_id == task.worker_id &&
                found->second.snapshot.state == TrainingJobState::Staging &&
                IsCurrentTrainingAttemptLocked(found->second)) {
                TrainingAttempt& attempt = found->second;
                if (failure.empty() && bundle &&
                    !stopping.load(std::memory_order_acquire)) {
                    TrainingAttempt previous = attempt;
                    attempt.inputs = std::move(bundle->files);
                    attempt.snapshot.input_identity_sha256 =
                        std::move(bundle->identity_sha256);
                    attempt.snapshot.state = TrainingJobState::Queued;
                    attempt.snapshot.error.clear();
                    std::string persist_error;
                    if (PersistTrainingJobsLocked(&persist_error)) {
                        keep_root = true;
                    } else {
                        attempt = std::move(previous);
                        attempt.snapshot.state = TrainingJobState::Failed;
                        attempt.snapshot.error =
                            "could not persist staged training inputs: " +
                            persist_error;
                        attempt.snapshot.error.resize(std::min<std::size_t>(
                            attempt.snapshot.error.size(), 4096));
                        keep_root = !PersistTrainingJobsLocked(nullptr);
                    }
                } else {
                    attempt.snapshot.state = TrainingJobState::Failed;
                    attempt.snapshot.error = failure.empty()
                        ? "training input staging did not complete" : std::move(failure);
                    attempt.snapshot.error.resize(std::min<std::size_t>(
                        attempt.snapshot.error.size(), 4096));
                    keep_root = !PersistTrainingJobsLocked(nullptr);
                }
            }
        } catch (...) {
            keep_root = true;
        }
        if (owned && !keep_root) {
            std::error_code ec;
            std::filesystem::remove_all(NativeFilesystemPath(
                training_store_directory /
                std::filesystem::u8path(task.job_id) /
                std::filesystem::u8path(task.attempt_id)), ec);
        }
    }

    void TrainingStagingLoop() noexcept {
        for (;;) {
            TrainingStageTask task;
            bool stop_before_stage = false;
            {
                std::unique_lock<std::mutex> lock(training_staging_mutex);
                training_staging_condition.wait(lock, [&] {
                    return training_staging_stopping ||
                           !training_staging_queue.empty();
                });
                if (training_staging_queue.empty() && training_staging_stopping)
                    return;
                task = std::move(training_staging_queue.front());
                training_staging_queue.pop_front();
                training_staging_active = true;
                stop_before_stage = training_staging_stopping;
            }
            bool owned = false;
            TrainingInputBundle bundle;
            std::string failure;
            bool current_attempt = false;
            {
                std::lock_guard<std::mutex> lock(training_mutex);
                const auto found = training_attempts.find(
                    TrainingAttemptKey(task.job_id, task.attempt_id));
                current_attempt = found != training_attempts.end() &&
                    found->second.snapshot.worker_id == task.worker_id &&
                    found->second.snapshot.state == TrainingJobState::Staging &&
                    IsCurrentTrainingAttemptLocked(found->second);
            }
            if (stop_before_stage)
                failure = "leader stopped before training staging";
            else if (!current_attempt)
                failure = "training attempt was superseded before staging";
            else
                StageTrainingTask(task, bundle, owned, failure);
            FinishTrainingStaging(task,
                failure.empty() ? &bundle : nullptr, std::move(failure), owned);
            {
                std::lock_guard<std::mutex> lock(training_staging_mutex);
                training_staging_active = false;
            }
        }
    }

    void StopTrainingStaging() noexcept {
        std::deque<TrainingStageTask> abandoned;
        {
            std::lock_guard<std::mutex> lock(training_staging_mutex);
            training_staging_stopping = true;
            abandoned.swap(training_staging_queue);
        }
        training_staging_condition.notify_all();
        for (const auto& task : abandoned)
            FinishTrainingStaging(task, nullptr,
                "leader stopped before training staging", false);
    }

    void MarkTrainingUnknown(
        const std::shared_ptr<Session>& session) noexcept {
        try {
            if (session->worker_id.empty()) return;
            std::lock_guard<std::mutex> lock(training_mutex);
            TrainingAttempt* attempt =
                CurrentTrainingForWorkerLocked(session->worker_id);
            if (attempt && !TerminalTrainingState(attempt->snapshot.state) &&
                attempt->snapshot.state != TrainingJobState::Queued &&
                attempt->snapshot.state != TrainingJobState::Staging) {
                std::string ignored;
                SetTrainingStateLocked(*attempt, TrainingJobState::Unknown,
                    "worker disconnected; outcome is unknown", &ignored);
            }
        } catch (...) {
        }
    }
#endif

    void CloseListener(RawSocket& listener, std::mutex& mutex) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        if (listener == kInvalidSocket) return;
        ShutdownSocket(listener);
        CloseSocket(listener);
        listener = kInvalidSocket;
    }

    void ReapFinished() noexcept {
        std::vector<std::shared_ptr<Session>> finished;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex);
            auto it = sessions.begin();
            while (it != sessions.end()) {
                if ((*it)->done.load(std::memory_order_acquire)) {
                    finished.push_back(std::move(*it));
                    it = sessions.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (const auto& session : finished)
            if (session->thread.joinable()) session->thread.join();
    }

    void EnrollmentLoop() noexcept {
        while (!stopping.load(std::memory_order_acquire)) {
            RawSocket accepted = AcceptReady(enrollment_listener,
                                              enrollment_listener_mutex, stopping);
            if (accepted == kInvalidSocket) continue;
            if (stopping.load(std::memory_order_acquire)) {
                CloseSocket(accepted);
                break;
            }
            const RawSocket handler_socket = DuplicateSocket(accepted);
            if (handler_socket == kInvalidSocket) {
                CloseSocket(accepted);
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(enrollment_cancel_mutex);
                if (stopping.load(std::memory_order_acquire)) {
                    CloseSocket(handler_socket);
                    CloseSocket(accepted);
                    break;
                }
                enrollment_cancel_socket = accepted;
            }
            std::string ignored;
            try {
                pairing_leader.HandleEnrollmentConnection(
                    ToNative(handler_socket), &ignored, &stopping);
            } catch (...) {
                // The enrollment API owns the socket once called.
            }
            {
                std::lock_guard<std::mutex> lock(enrollment_cancel_mutex);
                enrollment_cancel_socket = kInvalidSocket;
                CloseSocket(accepted);
            }
            if (!stopping.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void SetSessionError(const std::shared_ptr<Session>& session,
                         const std::string& message) noexcept {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->error = message;
    }

    void MarkDisconnected(const std::shared_ptr<Session>& session) noexcept {
        session->active.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->has_status = false;
            session->status.connection = wire::ConnectionState::Disconnected;
            session->status.compatibility = wire::CompatibilityState::Unknown;
            session->status.scheduling = wire::SchedulingState::Stopped;
            session->status.activity = wire::ActivityState::Idle;
            session->status.capabilities.clear();
            session->status.online = false;
            session->last_admin_transfer_command_id.clear();
            session->admin_transfer_in_progress.clear();
            session->waiting_feature_step.reset();
            session->waiting_feature_attempt.clear();
            session->waiting_portable_step.reset();
            session->waiting_portable_attempt.clear();
            session->waiting_portable_workload.reset();
            session->waiting_portable_job.clear();
        }
        bool current_session = false;
        if (!session->worker_id.empty()) {
            std::lock_guard<std::mutex> lock(sessions_mutex);
            const auto current = worker_sessions.find(session->worker_id);
            current_session = current != worker_sessions.end() &&
                              current->second == session;
        }
        bool persistence_failed = false;
        if (current_session) {
            std::lock_guard<std::mutex> lock(command_mutex);
            const auto found = active_commands.find(session->worker_id);
            if (found != active_commands.end()) {
                PendingCommand& pending = *found->second;
                pending.sent_session.reset();
                pending.result.state = pending.accepted
                    ? CommandState::Pending : CommandState::Disconnected;
                pending.waiter_notified = true;
                persistence_failed = !PersistCommandStateLocked(nullptr);
            }
        }
        if (persistence_failed)
            SetSessionError(session, "could not persist command disconnect state");
#ifdef SS_TOOL_SFM
        MarkFeatureUnknown(session);
        MarkReconstructionUnknown(session);
#endif
#ifdef SS_TOOL_TRAIN
        MarkTrainingUnknown(session);
#endif
        condition.notify_all();
    }

    // The condition variable is stored separately to keep the snapshot's status lock simple.
    std::condition_variable condition;

#ifdef SS_TOOL_SFM
    FeatureAttempt* CurrentFeatureForWorkerLocked(const std::string& worker_id) {
        for (const auto& [job_id, attempt_id] : current_feature_attempts) {
            const auto found = feature_attempts.find(attempt_id);
            if (found != feature_attempts.end() &&
                found->second.snapshot.worker_id == worker_id &&
                !TerminalFeatureState(found->second.snapshot.state))
                return &found->second;
        }
        return nullptr;
    }

    bool PersistFeatureChangeLocked(FeatureAttempt& attempt,
                                    const std::function<void(FeatureAttempt&)>& change,
                                    std::string* error) {
        FeatureAttempt previous = attempt;
        change(attempt);
        if (PersistFeatureJobsLocked(error)) return true;
        attempt = std::move(previous);
        return false;
    }

    bool SupportsFeatureBuild(const wire::Status& status,
                              const std::string& required_build) const {
        if (status.compatibility != wire::CompatibilityState::Compatible ||
            status.build != required_build || required_build.empty() ||
            required_build == "unknown" ||
            std::find(status.capabilities.begin(), status.capabilities.end(),
                      wire::Capability::Feature) == status.capabilities.end())
            return false;
        std::string gpu = status.gpu;
        std::transform(gpu.begin(), gpu.end(), gpu.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return gpu.find("nvidia") == std::string::npos;
    }

    bool FeatureSessionStatus(const std::shared_ptr<Session>& session,
                              wire::Status& status) const {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!session->has_status || session->stopping.load(std::memory_order_acquire))
            return false;
        status = session->status;
        return true;
    }

    bool SendFeatureOffer(TlsChannel& channel,
                          const std::shared_ptr<Session>& session,
                          const wire::Status& status,
                          bool& offered) {
        offered = false;
        if (!IsSessionTrusted(session)) return false;
        if (WorkerHasActiveReconstruction(session->worker_id)) return true;
        wire::FeatureOffer offer;
        {
            std::lock_guard<std::mutex> lock(feature_mutex);
            FeatureAttempt* attempt = CurrentFeatureForWorkerLocked(session->worker_id);
            if (!attempt) return true;
            const bool queued = attempt->snapshot.state ==
                LeaderServer::FeatureJobState::Queued;
            const bool resuming = attempt->snapshot.state ==
                    LeaderServer::FeatureJobState::Unknown &&
                !attempt->has_result &&
                (status.activity == wire::ActivityState::Idle ||
                 status.activity == wire::ActivityState::Feature);
            if (!queued && !resuming) return true;
            if (!SupportsFeatureBuild(status, attempt->required_build)) {
                std::string reason;
                if (status.build != attempt->required_build)
                    reason = "worker build does not match the required build";
                else if (status.compatibility == wire::CompatibilityState::Incompatible)
                    reason = "worker is incompatible with the leader";
                else if (std::find(status.capabilities.begin(), status.capabilities.end(),
                                   wire::Capability::Feature) == status.capabilities.end())
                    reason = "worker does not advertise feature capability";
                else {
                    std::string gpu = status.gpu;
                    std::transform(gpu.begin(), gpu.end(), gpu.begin(),
                        [](unsigned char ch) {
                            return static_cast<char>(std::tolower(ch));
                        });
                    if (gpu.find("nvidia") != std::string::npos)
                        reason = "NVIDIA workers are not eligible for feature shards";
                }
                if (queued && !reason.empty()) {
                    std::string ignored;
                    if (!SetFeatureStateLocked(*attempt,
                            LeaderServer::FeatureJobState::Failed, reason, &ignored))
                        return false;
                }
                return true;
            }
            if (status.scheduling != wire::SchedulingState::Accepting ||
                !status.online || status.maintenance ||
                status.health != wire::HealthState::Healthy ||
                (queued && status.activity != wire::ActivityState::Idle))
                return true;
            const std::uint64_t now = UnixNowMs();
            if (!now || now > kMaxSafeInteger - kFeatureOfferLifetimeMs)
                return false;
            offer.job_id = attempt->snapshot.job_id;
            offer.attempt_id = attempt->snapshot.attempt_id;
            offer.plan_digest = attempt->plan_digest;
            offer.request_digest = attempt->request_digest;
            offer.required_build = attempt->required_build;
            offer.expires_at_ms = now + kFeatureOfferLifetimeMs;
            offer.inputs = attempt->inputs;
            wire::Message check;
            check.payload = offer;
            const auto encoded = EncodeMessage(check, now);
            if (!encoded) {
                std::string ignored;
                SetFeatureStateLocked(*attempt,
                    LeaderServer::FeatureJobState::Failed,
                    "feature input offer exceeds the wire limit", &ignored);
                return true;
            }
            std::string persist_error;
            if (!PersistFeatureChangeLocked(*attempt, [&](FeatureAttempt& current) {
                    current.snapshot.state = LeaderServer::FeatureJobState::Offered;
                    current.snapshot.error.clear();
                    current.expires_at_ms = offer.expires_at_ms;
                }, &persist_error)) {
                SetSessionError(session, persist_error);
                return false;
            }
        }
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->waiting_feature_step = wire::FeatureDecision::Step::Offer;
            session->waiting_feature_attempt = offer.attempt_id;
        }
        wire::Message message;
        message.payload = std::move(offer);
        {
            std::lock_guard<std::mutex> lock(session->write_mutex);
            if (stopping.load(std::memory_order_acquire) ||
                session->stopping.load(std::memory_order_acquire))
                return false;
            if (!SendMessage(channel, message)) return false;
        }
        offered = true;
        return true;
    }

    void MarkFeatureUnknown(const std::shared_ptr<Session>& session) noexcept {
        try {
            if (session->worker_id.empty()) return;
            std::lock_guard<std::mutex> lock(feature_mutex);
            FeatureAttempt* attempt = CurrentFeatureForWorkerLocked(session->worker_id);
            if (attempt && attempt->snapshot.state !=
                               LeaderServer::FeatureJobState::Queued &&
                attempt->snapshot.state != LeaderServer::FeatureJobState::Staging) {
                std::string ignored;
                SetFeatureStateLocked(*attempt, LeaderServer::FeatureJobState::Unknown,
                    "worker disconnected; outcome is unknown", &ignored);
            }
        } catch (...) {
        }
    }

    bool SendFeatureDecision(TlsChannel& channel,
                             const std::shared_ptr<Session>& session,
                             wire::FeatureDecision decision) {
        if (!IsSessionTrusted(session)) return false;
        wire::Message message;
        message.payload = std::move(decision);
        std::lock_guard<std::mutex> lock(session->write_mutex);
        if (stopping.load(std::memory_order_acquire) ||
            session->stopping.load(std::memory_order_acquire))
            return false;
        return SendMessage(channel, message);
    }

    bool ReplyFeatureDecision(TlsChannel& channel,
                              const std::shared_ptr<Session>& session,
                              const wire::FeatureDecision& incoming,
                              wire::FeatureDecision::Decision decision,
                              const char* reason) {
        wire::FeatureDecision response;
        response.job_id = incoming.job_id;
        response.attempt_id = incoming.attempt_id;
        response.step = incoming.step;
        response.decision = decision;
        if (reason) response.reason = reason;
        return SendFeatureDecision(channel, session, std::move(response));
    }

    bool FeatureDecisionMatchesSession(const std::shared_ptr<Session>& session,
                                       const wire::FeatureDecision& decision) {
        std::lock_guard<std::mutex> lock(session->mutex);
        return session->waiting_feature_step &&
               *session->waiting_feature_step == decision.step &&
               session->waiting_feature_attempt == decision.attempt_id;
    }

    void ClearFeatureWait(const std::shared_ptr<Session>& session) {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->waiting_feature_step.reset();
        session->waiting_feature_attempt.clear();
    }

    bool ProcessFeatureDecision(TlsChannel& channel,
                                const std::shared_ptr<Session>& session,
                                const wire::FeatureDecision& decision) {
        if (!IsSessionTrusted(session)) return false;
        if (decision.step != wire::FeatureDecision::Step::Offer &&
            decision.step != wire::FeatureDecision::Step::Inputs)
            return ReplyFeatureDecision(channel, session, decision,
                wire::FeatureDecision::Decision::Rejected,
                "leader does not accept this feature decision step");
        if (!FeatureDecisionMatchesSession(session, decision))
            return ReplyFeatureDecision(channel, session, decision,
                wire::FeatureDecision::Decision::Rejected,
                "feature decision does not match the outstanding attempt");

        if (decision.step == wire::FeatureDecision::Step::Offer) {
            if (decision.decision == wire::FeatureDecision::Decision::Rejected) {
                {
                    std::lock_guard<std::mutex> lock(feature_mutex);
                    const auto found = feature_attempts.find(
                        FeatureAttemptKey(decision.job_id, decision.attempt_id));
                    if (found != feature_attempts.end() &&
                        found->second.snapshot.job_id == decision.job_id &&
                        found->second.snapshot.worker_id == session->worker_id &&
                        IsCurrentFeatureAttemptLocked(found->second)) {
                        std::string ignored;
                        SetFeatureStateLocked(found->second,
                            LeaderServer::FeatureJobState::Rejected,
                            decision.reason.empty() ? "worker rejected the feature offer"
                                                    : decision.reason,
                            &ignored);
                    }
                }
                ClearFeatureWait(session);
                return SendLeaderStatus(channel, session);
            }

            wire::Status status;
            if (!FeatureSessionStatus(session, status)) return false;
            std::string job_id;
            std::string attempt_id;
            std::filesystem::path input_root;
            std::vector<TransferFile> inputs;
            std::uint64_t budget = 0;
            bool rejected = false;
            std::string rejection = "feature attempt is stale or assigned to another worker";
            {
                std::lock_guard<std::mutex> lock(feature_mutex);
                const auto found = feature_attempts.find(
                    FeatureAttemptKey(decision.job_id, decision.attempt_id));
                if (found != feature_attempts.end() &&
                    found->second.snapshot.job_id == decision.job_id &&
                    found->second.snapshot.worker_id == session->worker_id &&
                    IsCurrentFeatureAttemptLocked(found->second)) {
                    FeatureAttempt& attempt = found->second;
                    if (attempt.snapshot.state != LeaderServer::FeatureJobState::Offered) {
                        rejection = "feature offer is no longer active";
                        rejected = true;
                    } else if (UnixNowMs() >= attempt.expires_at_ms) {
                        std::string ignored;
                        SetFeatureStateLocked(attempt, LeaderServer::FeatureJobState::Unknown,
                            "feature offer expired before acceptance", &ignored);
                        rejection = "feature offer expired";
                        rejected = true;
                    } else if (!SupportsFeatureBuild(status, attempt.required_build) ||
                               status.scheduling != wire::SchedulingState::Accepting ||
                               !status.online || status.maintenance ||
                               status.health != wire::HealthState::Healthy ||
                               (status.activity != wire::ActivityState::Idle &&
                                status.activity != wire::ActivityState::Feature)) {
                        std::string ignored;
                        SetFeatureStateLocked(attempt, LeaderServer::FeatureJobState::Rejected,
                            "worker eligibility changed before input transfer", &ignored);
                        rejection = "worker is no longer eligible for this feature offer";
                        rejected = true;
                    } else {
                        std::string persist_error;
                        if (!SetFeatureStateLocked(attempt,
                                LeaderServer::FeatureJobState::TransferringInputs, {},
                                &persist_error)) {
                            SetSessionError(session, persist_error);
                            return false;
                        }
                        job_id = attempt.snapshot.job_id;
                        attempt_id = attempt.snapshot.attempt_id;
                        input_root = attempt.attempt_root / "input";
                        inputs = attempt.inputs;
                        budget = attempt.disk_budget_bytes;
                    }
                } else {
                    rejected = true;
                }
            }
            if (rejected || attempt_id.empty()) {
                ClearFeatureWait(session);
                return ReplyFeatureDecision(channel, session, decision,
                    wire::FeatureDecision::Decision::Rejected, rejection.c_str());
            }
            const TransferResult sent = SendArtifacts(channel, input_root, inputs, budget);
            if (!sent) {
                if (sent.error != TransferError::Interrupted) {
                    std::lock_guard<std::mutex> lock(feature_mutex);
                    const auto found = feature_attempts.find(
                        FeatureAttemptKey(job_id, attempt_id));
                    if (found != feature_attempts.end() &&
                        IsCurrentFeatureAttemptLocked(found->second)) {
                        std::string ignored;
                        SetFeatureStateLocked(found->second,
                            LeaderServer::FeatureJobState::Failed,
                            std::string("input transfer failed: ") + sent.message,
                            &ignored);
                    }
                }
                return false;
            }
            if (!IsSessionTrusted(session)) return false;
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                session->waiting_feature_step = wire::FeatureDecision::Step::Inputs;
                session->waiting_feature_attempt = attempt_id;
            }
            return true;
        }

        if (decision.decision == wire::FeatureDecision::Decision::Rejected) {
            {
                std::lock_guard<std::mutex> lock(feature_mutex);
                const auto found = feature_attempts.find(
                    FeatureAttemptKey(decision.job_id, decision.attempt_id));
                if (found != feature_attempts.end() &&
                    found->second.snapshot.job_id == decision.job_id &&
                    found->second.snapshot.worker_id == session->worker_id &&
                    IsCurrentFeatureAttemptLocked(found->second)) {
                    std::string ignored;
                    SetFeatureStateLocked(found->second,
                        LeaderServer::FeatureJobState::Rejected,
                        decision.reason.empty() ? "worker rejected feature inputs"
                                                : decision.reason,
                        &ignored);
                }
            }
            ClearFeatureWait(session);
            return SendLeaderStatus(channel, session);
        }

        bool current = false;
        {
            std::lock_guard<std::mutex> lock(feature_mutex);
            const auto found = feature_attempts.find(
                FeatureAttemptKey(decision.job_id, decision.attempt_id));
            if (found != feature_attempts.end() &&
                found->second.snapshot.job_id == decision.job_id &&
                found->second.snapshot.worker_id == session->worker_id &&
                IsCurrentFeatureAttemptLocked(found->second) &&
                found->second.snapshot.state ==
                    LeaderServer::FeatureJobState::TransferringInputs) {
                std::string persist_error;
                current = SetFeatureStateLocked(found->second,
                    LeaderServer::FeatureJobState::Running, {}, &persist_error);
                if (!current) SetSessionError(session, persist_error);
            }
        }
        ClearFeatureWait(session);
        if (!current)
            return ReplyFeatureDecision(channel, session, decision,
                wire::FeatureDecision::Decision::Rejected,
                "feature attempt is stale or no longer transferring inputs");
        return true;
    }

    bool SendFeatureOutputDecision(
        TlsChannel& channel, const std::shared_ptr<Session>& session,
        const wire::FeatureResult& result,
        wire::FeatureDecision::Decision decision, const std::string& reason = {}) {
        wire::FeatureDecision response;
        response.job_id = result.job_id;
        response.attempt_id = result.attempt_id;
        response.step = wire::FeatureDecision::Step::Output;
        response.decision = decision;
        response.reason = reason;
        return SendFeatureDecision(channel, session, std::move(response));
    }

    bool SameFeatureResult(const FeatureAttempt& attempt,
                           const wire::FeatureResult& result) const {
        if (!attempt.has_result || attempt.outcome != result.outcome ||
            attempt.result_error != sfm::feature_work::sha256Text(result.error) ||
            attempt.outputs.size() != result.outputs.size())
            return false;
        auto a = attempt.outputs;
        auto b = result.outputs;
        const auto by_path = [](const TransferFile& left, const TransferFile& right) {
            return left.path < right.path;
        };
        std::sort(a.begin(), a.end(), by_path);
        std::sort(b.begin(), b.end(), by_path);
        for (std::size_t i = 0; i < a.size(); ++i)
            if (a[i].path != b[i].path || a[i].size != b[i].size ||
                a[i].sha256 != b[i].sha256)
                return false;
        return true;
    }

    bool SetWorkerResult(const std::shared_ptr<Session>& session,
                         const wire::FeatureResult& result,
                         LeaderServer::FeatureJobState state,
                         std::string diagnostic) {
        std::lock_guard<std::mutex> pair_lock(pairing_mutex);
        const auto worker = pairing_leader.WorkerForPeer(session->peer_fingerprint);
        if (!worker || worker->id != session->worker_id ||
            worker->status != pairing::WorkerStatus::Paired || worker->revoked ||
            session->stopping.load(std::memory_order_acquire))
            return false;
        std::lock_guard<std::mutex> lock(feature_mutex);
        const auto found = feature_attempts.find(
            FeatureAttemptKey(result.job_id, result.attempt_id));
        if (found == feature_attempts.end() ||
            found->second.snapshot.job_id != result.job_id ||
            found->second.snapshot.worker_id != session->worker_id ||
            !IsCurrentFeatureAttemptLocked(found->second))
            return false;
        if ((!found->second.has_result &&
             found->second.snapshot.state != LeaderServer::FeatureJobState::Running &&
             found->second.snapshot.state != LeaderServer::FeatureJobState::Unknown) ||
            (found->second.has_result &&
             found->second.snapshot.state != LeaderServer::FeatureJobState::ReceivingOutput &&
             found->second.snapshot.state != LeaderServer::FeatureJobState::Unknown))
            return false;
        FeatureAttempt& attempt = found->second;
        FeatureAttempt previous = attempt;
        attempt.has_result = true;
        attempt.outcome = result.outcome;
        attempt.outputs = result.outputs;
        attempt.result_error = sfm::feature_work::sha256Text(result.error);
        attempt.snapshot.state = state;
        attempt.snapshot.error = std::move(diagnostic);
        if (attempt.snapshot.error.size() > 4096)
            attempt.snapshot.error.resize(4096);
        attempt.snapshot.progress.reset();
        std::string persist_error;
        if (PersistFeatureJobsLocked(&persist_error)) return true;
        attempt = std::move(previous);
        SetSessionError(session, persist_error);
        return false;
    }

    bool CommitVerifiedFeatureResult(const std::shared_ptr<Session>& session,
                                     const wire::FeatureResult& result) {
        std::lock_guard<std::mutex> pair_lock(pairing_mutex);
        const auto worker = pairing_leader.WorkerForPeer(session->peer_fingerprint);
        if (!worker || worker->id != session->worker_id ||
            worker->status != pairing::WorkerStatus::Paired || worker->revoked ||
            session->stopping.load(std::memory_order_acquire))
            return false;
        std::lock_guard<std::mutex> lock(feature_mutex);
        const auto found = feature_attempts.find(
            FeatureAttemptKey(result.job_id, result.attempt_id));
        if (found == feature_attempts.end() ||
            found->second.snapshot.job_id != result.job_id ||
            found->second.snapshot.worker_id != session->worker_id ||
            !IsCurrentFeatureAttemptLocked(found->second) ||
            found->second.snapshot.state !=
                LeaderServer::FeatureJobState::ReceivingOutput ||
            !SameFeatureResult(found->second, result))
            return false;
        std::string persist_error;
        if (SetFeatureStateLocked(found->second,
                LeaderServer::FeatureJobState::Succeeded, {}, &persist_error))
            return true;
        SetSessionError(session, persist_error);
        return false;
    }

    bool ProcessFeatureResult(TlsChannel& channel,
                              const std::shared_ptr<Session>& session,
                              const wire::FeatureResult& result) {
        if (!IsSessionTrusted(session)) return false;
        wire::Status status;
        if (!FeatureSessionStatus(session, status))
            return SendFeatureOutputDecision(channel, session, result,
                wire::FeatureDecision::Decision::Rejected,
                "worker status is unavailable for feature result publication");

        FeatureAttempt attempt;
        bool found_attempt = false;
        {
            std::lock_guard<std::mutex> lock(feature_mutex);
            const auto found = feature_attempts.find(
                FeatureAttemptKey(result.job_id, result.attempt_id));
            if (found != feature_attempts.end() &&
                found->second.snapshot.job_id == result.job_id &&
                found->second.snapshot.worker_id == session->worker_id &&
                IsCurrentFeatureAttemptLocked(found->second)) {
                attempt = found->second;
                found_attempt = true;
            }
        }
        if (!found_attempt)
            return SendFeatureOutputDecision(channel, session, result,
                wire::FeatureDecision::Decision::Rejected,
                "feature attempt is stale or assigned to another worker");
        if (!SupportsFeatureBuild(status, attempt.required_build))
            return SendFeatureOutputDecision(channel, session, result,
                wire::FeatureDecision::Decision::Rejected,
                "worker build or feature capability does not match this attempt");

        if (attempt.has_result) {
            if (!SameFeatureResult(attempt, result))
                return SendFeatureOutputDecision(channel, session, result,
                    wire::FeatureDecision::Decision::Rejected,
                    "duplicate feature result differs from the accepted result");
            if (attempt.snapshot.state == LeaderServer::FeatureJobState::Succeeded ||
                ((attempt.snapshot.state == LeaderServer::FeatureJobState::Failed ||
                  attempt.snapshot.state == LeaderServer::FeatureJobState::Interrupted) &&
                 result.outcome != wire::FeatureResult::Outcome::Succeeded))
                return SendFeatureOutputDecision(channel, session, result,
                    wire::FeatureDecision::Decision::Committed);
            if (attempt.snapshot.state == LeaderServer::FeatureJobState::Failed ||
                attempt.snapshot.state == LeaderServer::FeatureJobState::Rejected ||
                attempt.snapshot.state == LeaderServer::FeatureJobState::Superseded)
                return SendFeatureOutputDecision(channel, session, result,
                    wire::FeatureDecision::Decision::Rejected,
                    "feature attempt has already been rejected");
        } else if (TerminalFeatureState(attempt.snapshot.state)) {
            return SendFeatureOutputDecision(channel, session, result,
                wire::FeatureDecision::Decision::Rejected,
                "feature attempt is no longer active");
        }
        const bool result_state_allowed =
            attempt.snapshot.state == LeaderServer::FeatureJobState::Running ||
            attempt.snapshot.state == LeaderServer::FeatureJobState::Unknown ||
            (attempt.has_result &&
             attempt.snapshot.state == LeaderServer::FeatureJobState::ReceivingOutput);
        if (!result_state_allowed)
            return SendFeatureOutputDecision(channel, session, result,
                wire::FeatureDecision::Decision::Rejected,
                "feature attempt has not accepted its inputs");

        if (result.outcome != wire::FeatureResult::Outcome::Succeeded) {
            const auto state = result.outcome == wire::FeatureResult::Outcome::Failed
                ? LeaderServer::FeatureJobState::Failed
                : LeaderServer::FeatureJobState::Interrupted;
            const std::string diagnostic = result.error.empty()
                ? (state == LeaderServer::FeatureJobState::Failed
                    ? "worker reported feature failure"
                    : "worker interrupted feature extraction")
                : result.error;
            if (!SetWorkerResult(session, result, state, diagnostic))
                return SendFeatureOutputDecision(channel, session, result,
                    wire::FeatureDecision::Decision::Rejected,
                    "feature attempt became stale before result publication");
            return SendFeatureOutputDecision(channel, session, result,
                wire::FeatureDecision::Decision::Committed);
        }

        std::uint64_t output_bytes = 0;
        for (const TransferFile& file : result.outputs) {
            if (output_bytes > attempt.disk_budget_bytes ||
                file.size > attempt.disk_budget_bytes - output_bytes)
                return SendFeatureOutputDecision(channel, session, result,
                    wire::FeatureDecision::Decision::Rejected,
                    "feature output manifest exceeds the attempt quota");
            output_bytes += file.size;
        }
        const std::string manifest_json = FeatureManifestJson(result.outputs);
        if (manifest_json.size() > kMaxFeatureManifestJsonBytes)
            return SendFeatureOutputDecision(channel, session, result,
                wire::FeatureDecision::Decision::Rejected,
                "feature output manifest exceeds the metadata limit");

        if (!attempt.has_result) {
            if (attempt.snapshot.state != LeaderServer::FeatureJobState::Running &&
                attempt.snapshot.state != LeaderServer::FeatureJobState::Unknown)
                return SendFeatureOutputDecision(channel, session, result,
                    wire::FeatureDecision::Decision::Rejected,
                    "feature attempt has not accepted its inputs");
            if (!SetWorkerResult(session, result,
                    LeaderServer::FeatureJobState::ReceivingOutput, {}))
                return SendFeatureOutputDecision(channel, session, result,
                    wire::FeatureDecision::Decision::Rejected,
                    "feature attempt became stale before output transfer");
        } else if (attempt.snapshot.state !=
                       LeaderServer::FeatureJobState::ReceivingOutput &&
                   attempt.snapshot.state != LeaderServer::FeatureJobState::Unknown) {
            return SendFeatureOutputDecision(channel, session, result,
                wire::FeatureDecision::Decision::Rejected,
                "feature output is no longer awaiting verification");
        }

        const auto output_root = attempt.snapshot.result_root;
        std::string staging_error;
        if (!EnsureTransferRoot(output_root, &staging_error)) {
            const std::string diagnostic =
                std::string("feature output staging failed: ") + staging_error;
            if (!SetWorkerResult(session, result,
                    LeaderServer::FeatureJobState::Failed, diagnostic))
                return false;
            return SendFeatureOutputDecision(channel, session, result,
                wire::FeatureDecision::Decision::Rejected, diagnostic);
        }
        if (!IsSessionTrusted(session) ||
            !SendFeatureOutputDecision(channel, session, result,
                wire::FeatureDecision::Decision::Accepted))
            return false;

        const auto request_path = attempt.attempt_root / "input" / "request.json";
        const TransferResult received = ReceiveArtifacts(
            channel, output_root, result.outputs, attempt.disk_budget_bytes);
        if (!received) {
            if (received.error != TransferError::Interrupted)
                SetWorkerResult(session, result, LeaderServer::FeatureJobState::Failed,
                    std::string("feature output transfer failed: ") + received.message);
            return false;
        }
        if (!IsSessionTrusted(session)) return false;
        try {
            VerifyFeatureOutputs(request_path, output_root, result.outputs,
                                 attempt.disk_budget_bytes);
        } catch (const std::exception& exception) {
            const std::string diagnostic =
                std::string("feature output verification failed: ") + exception.what();
            if (!SetWorkerResult(session, result,
                    LeaderServer::FeatureJobState::Failed, diagnostic))
                return false;
            return SendFeatureOutputDecision(channel, session, result,
                wire::FeatureDecision::Decision::Rejected, diagnostic);
        }
        if (!CommitVerifiedFeatureResult(session, result))
            return SendFeatureOutputDecision(channel, session, result,
                wire::FeatureDecision::Decision::Rejected,
                "feature attempt became stale or revoked before commit");
        return SendFeatureOutputDecision(channel, session, result,
            wire::FeatureDecision::Decision::Committed);
    }
    bool SendReconstructionOffer(
        TlsChannel& channel, const std::shared_ptr<Session>& session,
        const wire::Status& status, bool& offered) {
        offered = false;
        if (!IsSessionTrusted(session)) return false;
        if (WorkerHasActiveFeature(session->worker_id)) return true;
        wire::PortableOffer offer;
        {
            std::lock_guard<std::mutex> lock(reconstruction_mutex);
            ReconstructionAttempt* attempt =
                CurrentReconstructionForWorkerLocked(session->worker_id);
            if (!attempt) return true;
            const bool queued = attempt->snapshot.state ==
                ReconstructionJobState::Queued;
            const bool resuming = attempt->snapshot.state ==
                    ReconstructionJobState::Unknown &&
                !attempt->has_result &&
                status.activity == wire::ActivityState::Idle;
            if (!queued && !resuming) return true;
            if (!SupportsReconstructionBuild(
                    status, attempt->snapshot.required_build) ||
                status.scheduling != wire::SchedulingState::Accepting ||
                status.activity != wire::ActivityState::Idle ||
                !status.online || status.maintenance ||
                status.health != wire::HealthState::Healthy)
                return true;
            const std::uint64_t now = UnixNowMs();
            if (!now || now > kMaxSafeInteger -
                                 kReconstructionOfferLifetimeMs)
                return false;
            offer.workload = wire::PortableWorkload::Reconstruction;
            offer.job_id = attempt->snapshot.job_id;
            offer.attempt_id = attempt->snapshot.attempt_id;
            offer.input_identity_sha256 =
                attempt->snapshot.input_identity_sha256;
            offer.required_build = attempt->snapshot.required_build;
            offer.expires_at_ms = now + kReconstructionOfferLifetimeMs;
            offer.inputs = attempt->inputs;
            wire::Message check;
            check.payload = offer;
            std::string encoded;
            if (wire::Encode(check, now, encoded) != wire::Error::None) {
                std::string ignored;
                SetReconstructionStateLocked(*attempt,
                    ReconstructionJobState::Failed,
                    "reconstruction input offer exceeds wire limits", &ignored);
                return true;
            }
            std::string persist_error;
            if (!PersistReconstructionChangeLocked(*attempt,
                    [&](ReconstructionAttempt& current) {
                        current.snapshot.state = ReconstructionJobState::Offered;
                        current.snapshot.error.clear();
                        current.expires_at_ms = offer.expires_at_ms;
                    }, &persist_error)) {
                SetSessionError(session, persist_error);
                return false;
            }
        }
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->waiting_portable_step = wire::PortableDecision::Step::Offer;
            session->waiting_portable_workload =
                wire::PortableWorkload::Reconstruction;
            session->waiting_portable_attempt = offer.attempt_id;
            session->waiting_portable_job = offer.job_id;
        }
        wire::Message message;
        message.payload = std::move(offer);
        {
            std::lock_guard<std::mutex> lock(session->write_mutex);
            if (stopping.load(std::memory_order_acquire) ||
                session->stopping.load(std::memory_order_acquire))
                return false;
            if (!SendMessage(channel, message)) return false;
        }
        offered = true;
        return true;
    }

    bool SendPortableDecision(TlsChannel& channel,
                              const std::shared_ptr<Session>& session,
                              wire::PortableDecision decision) {
        if (!IsSessionTrusted(session)) return false;
        wire::Message message;
        message.payload = std::move(decision);
        std::lock_guard<std::mutex> lock(session->write_mutex);
        if (stopping.load(std::memory_order_acquire) ||
            session->stopping.load(std::memory_order_acquire))
            return false;
        return SendMessage(channel, message);
    }

    bool ReplyPortableDecision(TlsChannel& channel,
                               const std::shared_ptr<Session>& session,
                               const wire::PortableDecision& incoming,
                               wire::PortableDecision::Decision decision,
                               const char* reason) {
        wire::PortableDecision response;
        response.workload = incoming.workload;
        response.job_id = incoming.job_id;
        response.attempt_id = incoming.attempt_id;
        response.step = incoming.step;
        response.decision = decision;
        if (reason) response.reason = reason;
        return SendPortableDecision(channel, session, std::move(response));
    }

    bool PortableDecisionMatchesSession(
        const std::shared_ptr<Session>& session,
        const wire::PortableDecision& decision) {
        std::lock_guard<std::mutex> lock(session->mutex);
        return session->waiting_portable_step &&
               session->waiting_portable_workload == decision.workload &&
               *session->waiting_portable_step == decision.step &&
               session->waiting_portable_attempt == decision.attempt_id &&
               session->waiting_portable_job == decision.job_id;
    }

    void ClearPortableWait(const std::shared_ptr<Session>& session) {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->waiting_portable_step.reset();
        session->waiting_portable_attempt.clear();
        session->waiting_portable_workload.reset();
        session->waiting_portable_job.clear();
    }

    bool ProcessPortableDecision(
        TlsChannel& channel, const std::shared_ptr<Session>& session,
        const wire::PortableDecision& decision) {
        if (!IsSessionTrusted(session) ||
            decision.workload != wire::PortableWorkload::Reconstruction)
            return false;
        if (decision.step != wire::PortableDecision::Step::Offer &&
            decision.step != wire::PortableDecision::Step::Inputs)
            return ReplyPortableDecision(channel, session, decision,
                wire::PortableDecision::Decision::Rejected,
                "leader does not accept this reconstruction decision step");
        if (!PortableDecisionMatchesSession(session, decision))
            return ReplyPortableDecision(channel, session, decision,
                wire::PortableDecision::Decision::Rejected,
                "reconstruction decision does not match the outstanding attempt");
        if (decision.decision == wire::PortableDecision::Decision::Committed)
            return false;

        if (decision.step == wire::PortableDecision::Step::Offer &&
            decision.decision == wire::PortableDecision::Decision::Rejected) {
            {
                std::lock_guard<std::mutex> lock(reconstruction_mutex);
                const auto found = reconstruction_attempts.find(
                    FeatureAttemptKey(decision.job_id, decision.attempt_id));
                if (found != reconstruction_attempts.end() &&
                    found->second.snapshot.worker_id == session->worker_id &&
                    IsCurrentReconstructionAttemptLocked(found->second)) {
                    std::string ignored;
                    SetReconstructionStateLocked(found->second,
                        ReconstructionJobState::Rejected,
                        decision.reason.empty()
                            ? "worker rejected the reconstruction offer"
                            : decision.reason, &ignored);
                }
            }
            ClearPortableWait(session);
            return SendLeaderStatus(channel, session);
        }

        if (decision.step == wire::PortableDecision::Step::Offer) {
            wire::Status status;
            if (!ReconstructionSessionStatus(session, status)) return false;
            std::filesystem::path input_root;
            std::vector<TransferFile> inputs;
            std::uint64_t budget = 0;
            bool rejected = false;
            const char* rejection =
                "reconstruction attempt is stale or assigned to another worker";
            {
                std::lock_guard<std::mutex> lock(reconstruction_mutex);
                const auto found = reconstruction_attempts.find(
                    FeatureAttemptKey(decision.job_id, decision.attempt_id));
                if (found != reconstruction_attempts.end() &&
                    found->second.snapshot.worker_id == session->worker_id &&
                    IsCurrentReconstructionAttemptLocked(found->second)) {
                    ReconstructionAttempt& attempt = found->second;
                    if (attempt.snapshot.state != ReconstructionJobState::Offered) {
                        rejection = "reconstruction offer is no longer active";
                        rejected = true;
                    } else if (UnixNowMs() >= attempt.expires_at_ms) {
                        std::string ignored;
                        SetReconstructionStateLocked(attempt,
                            ReconstructionJobState::Unknown,
                            "reconstruction offer expired before acceptance", &ignored);
                        rejection = "reconstruction offer expired";
                        rejected = true;
                    } else if (!SupportsReconstructionBuild(
                                   status, attempt.snapshot.required_build) ||
                               status.scheduling != wire::SchedulingState::Accepting ||
                               !status.online || status.maintenance ||
                               status.health != wire::HealthState::Healthy ||
                               status.activity != wire::ActivityState::Idle) {
                        std::string ignored;
                        SetReconstructionStateLocked(attempt,
                            ReconstructionJobState::Rejected,
                            "worker eligibility changed before input transfer",
                            &ignored);
                        rejection =
                            "worker is no longer eligible for this reconstruction offer";
                        rejected = true;
                    } else {
                        std::string persist_error;
                        if (!SetReconstructionStateLocked(attempt,
                                ReconstructionJobState::TransferringInputs, {},
                                &persist_error)) {
                            SetSessionError(session, persist_error);
                            return false;
                        }
                        input_root = attempt.attempt_root / "input";
                        inputs = attempt.inputs;
                        budget = attempt.disk_budget_bytes;
                    }
                } else {
                    rejected = true;
                }
            }
            if (rejected || inputs.empty()) {
                ClearPortableWait(session);
                return ReplyPortableDecision(channel, session, decision,
                    wire::PortableDecision::Decision::Rejected, rejection);
            }
            const TransferResult sent =
                SendArtifacts(channel, input_root, inputs, budget);
            if (!sent) {
                if (sent.error != TransferError::Interrupted) {
                    std::lock_guard<std::mutex> lock(reconstruction_mutex);
                    const auto found = reconstruction_attempts.find(
                        FeatureAttemptKey(decision.job_id, decision.attempt_id));
                    if (found != reconstruction_attempts.end() &&
                        IsCurrentReconstructionAttemptLocked(found->second)) {
                        std::string ignored;
                        SetReconstructionStateLocked(found->second,
                            ReconstructionJobState::Failed,
                            std::string("input transfer failed: ") + sent.message,
                            &ignored);
                    }
                }
                return false;
            }
            if (!IsSessionTrusted(session)) return false;
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                session->waiting_portable_step =
                    wire::PortableDecision::Step::Inputs;
            }
            return true;
        }

        if (decision.decision == wire::PortableDecision::Decision::Rejected) {
            {
                std::lock_guard<std::mutex> lock(reconstruction_mutex);
                const auto found = reconstruction_attempts.find(
                    FeatureAttemptKey(decision.job_id, decision.attempt_id));
                if (found != reconstruction_attempts.end() &&
                    found->second.snapshot.worker_id == session->worker_id &&
                    IsCurrentReconstructionAttemptLocked(found->second)) {
                    std::string ignored;
                    SetReconstructionStateLocked(found->second,
                        ReconstructionJobState::Rejected,
                        decision.reason.empty()
                            ? "worker rejected reconstruction inputs"
                            : decision.reason, &ignored);
                }
            }
            ClearPortableWait(session);
            return SendLeaderStatus(channel, session);
        }

        bool current = false;
        {
            std::lock_guard<std::mutex> lock(reconstruction_mutex);
            const auto found = reconstruction_attempts.find(
                FeatureAttemptKey(decision.job_id, decision.attempt_id));
            if (found != reconstruction_attempts.end() &&
                found->second.snapshot.worker_id == session->worker_id &&
                IsCurrentReconstructionAttemptLocked(found->second) &&
                found->second.snapshot.state ==
                    ReconstructionJobState::TransferringInputs) {
                std::string persist_error;
                current = SetReconstructionStateLocked(found->second,
                    ReconstructionJobState::Running, {}, &persist_error);
                if (!current) SetSessionError(session, persist_error);
            }
        }
        ClearPortableWait(session);
        if (!current)
            return ReplyPortableDecision(channel, session, decision,
                wire::PortableDecision::Decision::Rejected,
                "reconstruction attempt is stale or no longer transferring inputs");
        return true;
    }

    bool SendPortableOutputDecision(
        TlsChannel& channel, const std::shared_ptr<Session>& session,
        const wire::PortableResult& result,
        wire::PortableDecision::Decision decision,
        const std::string& reason = {}) {
        wire::PortableDecision response;
        response.workload = result.workload;
        response.job_id = result.job_id;
        response.attempt_id = result.attempt_id;
        response.step = wire::PortableDecision::Step::Output;
        response.decision = decision;
        response.reason = reason;
        return SendPortableDecision(channel, session, std::move(response));
    }

    bool SameReconstructionResult(const ReconstructionAttempt& attempt,
                                  const wire::PortableResult& result) const {
        if (!attempt.has_result || attempt.outcome != result.outcome ||
            attempt.result_error_sha256 !=
                sfm::feature_work::sha256Text(result.error) ||
            result.workload != wire::PortableWorkload::Reconstruction ||
            !result.output_metadata.empty() ||
            attempt.outputs.size() != result.outputs.size())
            return false;
        auto left = attempt.outputs;
        auto right = result.outputs;
        const auto by_path = [](const TransferFile& a, const TransferFile& b) {
            return a.path < b.path;
        };
        std::sort(left.begin(), left.end(), by_path);
        std::sort(right.begin(), right.end(), by_path);
        for (std::size_t i = 0; i < left.size(); ++i)
            if (left[i].path != right[i].path ||
                left[i].size != right[i].size ||
                left[i].sha256 != right[i].sha256)
                return false;
        return true;
    }

    bool FailUnacceptedReconstructionResult(
        const std::shared_ptr<Session>& session,
        const wire::PortableResult& result, std::string diagnostic) {
        if (!IsSessionTrusted(session)) return false;
        std::lock_guard<std::mutex> lock(reconstruction_mutex);
        const auto found = reconstruction_attempts.find(
            FeatureAttemptKey(result.job_id, result.attempt_id));
        if (found == reconstruction_attempts.end() ||
            found->second.snapshot.worker_id != session->worker_id ||
            !IsCurrentReconstructionAttemptLocked(found->second) ||
            found->second.has_result ||
            (found->second.snapshot.state != ReconstructionJobState::Running &&
             found->second.snapshot.state != ReconstructionJobState::Unknown))
            return false;
        std::string persist_error;
        return SetReconstructionStateLocked(found->second,
            ReconstructionJobState::Failed, std::move(diagnostic),
            &persist_error);
    }

    bool SetReconstructionResult(
        const std::shared_ptr<Session>& session,
        const wire::PortableResult& result, ReconstructionJobState state,
        std::string diagnostic) {
        if (!IsSessionTrusted(session)) return false;
        std::lock_guard<std::mutex> lock(reconstruction_mutex);
        const auto found = reconstruction_attempts.find(
            FeatureAttemptKey(result.job_id, result.attempt_id));
        if (found == reconstruction_attempts.end() ||
            found->second.snapshot.worker_id != session->worker_id ||
            !IsCurrentReconstructionAttemptLocked(found->second))
            return false;
        ReconstructionAttempt& attempt = found->second;
        if (attempt.has_result && !SameReconstructionResult(attempt, result))
            return false;
        if ((!attempt.has_result &&
             attempt.snapshot.state != ReconstructionJobState::Running &&
             attempt.snapshot.state != ReconstructionJobState::Unknown) ||
            (attempt.has_result &&
             attempt.snapshot.state != ReconstructionJobState::ReceivingOutput &&
             attempt.snapshot.state != ReconstructionJobState::Unknown))
            return false;
        ReconstructionAttempt previous = attempt;
        attempt.has_result = true;
        attempt.outcome = result.outcome;
        attempt.outputs = result.outputs;
        attempt.result_error_sha256 =
            sfm::feature_work::sha256Text(result.error);
        attempt.snapshot.state = state;
        attempt.snapshot.error = std::move(diagnostic);
        attempt.snapshot.error.resize(std::min<std::size_t>(
            attempt.snapshot.error.size(), 4096));
        std::string persist_error;
        if (PersistReconstructionJobsLocked(&persist_error)) return true;
        attempt = std::move(previous);
        SetSessionError(session, persist_error);
        return false;
    }

    bool CommitVerifiedReconstructionResult(
        const std::shared_ptr<Session>& session,
        const wire::PortableResult& result) {
        std::lock_guard<std::mutex> pair_lock(pairing_mutex);
        const auto worker = pairing_leader.WorkerForPeer(session->peer_fingerprint);
        if (!worker || worker->id != session->worker_id ||
            worker->status != pairing::WorkerStatus::Paired || worker->revoked ||
            session->stopping.load(std::memory_order_acquire))
            return false;
        std::lock_guard<std::mutex> lock(reconstruction_mutex);
        const auto found = reconstruction_attempts.find(
            FeatureAttemptKey(result.job_id, result.attempt_id));
        if (found == reconstruction_attempts.end() ||
            found->second.snapshot.worker_id != session->worker_id ||
            !IsCurrentReconstructionAttemptLocked(found->second) ||
            found->second.snapshot.state != ReconstructionJobState::ReceivingOutput ||
            !SameReconstructionResult(found->second, result))
            return false;
        ReconstructionAttempt& attempt = found->second;
        std::error_code ec;
        const auto result_root =
            NativeFilesystemPath(attempt.snapshot.result_root);
        const auto target_status =
            std::filesystem::symlink_status(result_root, ec);
        if (!ec || ec != std::errc::no_such_file_or_directory) {
            SetSessionError(session, "reconstruction result path already exists");
            return false;
        }
        ec.clear();
        const auto staging_root = attempt.attempt_root / "output-staging";
        const auto io_staging_root = NativeFilesystemPath(staging_root);
        const auto staging_status =
            std::filesystem::symlink_status(io_staging_root, ec);
        if (ec || !std::filesystem::is_directory(staging_status) ||
            std::filesystem::is_symlink(staging_status)) {
            SetSessionError(session, "reconstruction output staging path is unsafe");
            return false;
        }
        std::filesystem::rename(io_staging_root, result_root, ec);
        if (ec) {
            SetSessionError(session, "could not publish verified reconstruction output");
            return false;
        }
        ReconstructionAttempt previous = attempt;
        attempt.snapshot.state = ReconstructionJobState::Succeeded;
        attempt.snapshot.error.clear();
        std::string persist_error;
        if (PersistReconstructionJobsLocked(&persist_error)) return true;
        attempt = std::move(previous);
        std::error_code rollback_error;
        std::filesystem::rename(result_root, io_staging_root, rollback_error);

        SetSessionError(session, persist_error);
        return false;
    }

    bool ProcessPortableResult(TlsChannel& channel,
                               const std::shared_ptr<Session>& session,
                               const wire::PortableResult& result) {
        if (!IsSessionTrusted(session) ||
            result.workload != wire::PortableWorkload::Reconstruction)
            return false;
        wire::Status status;
        if (!ReconstructionSessionStatus(session, status))
            return SendPortableOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Rejected,
                "worker status is unavailable for reconstruction result publication");
        ReconstructionAttempt attempt;
        bool found_attempt = false;
        {
            std::lock_guard<std::mutex> lock(reconstruction_mutex);
            const auto found = reconstruction_attempts.find(
                FeatureAttemptKey(result.job_id, result.attempt_id));
            if (found != reconstruction_attempts.end() &&
                found->second.snapshot.worker_id == session->worker_id &&
                IsCurrentReconstructionAttemptLocked(found->second)) {
                attempt = found->second;
                found_attempt = true;
            }
        }
        if (!found_attempt)
            return SendPortableOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Rejected,
                "reconstruction attempt is stale or assigned to another worker");
        if (!SupportsReconstructionBuild(status, attempt.snapshot.required_build))
            return SendPortableOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Rejected,
                "worker build or reconstruction capability does not match this attempt");
        if (!result.output_metadata.empty()) {
            if (!attempt.has_result)
                FailUnacceptedReconstructionResult(session, result,
                    "reconstruction result metadata must be empty");
            return SendPortableOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Rejected,
                "reconstruction result metadata must be empty");
        }

        if (attempt.has_result) {
            if (!SameReconstructionResult(attempt, result))
                return SendPortableOutputDecision(channel, session, result,
                    wire::PortableDecision::Decision::Rejected,
                    "duplicate reconstruction result differs from the accepted result");
            if (attempt.snapshot.state == ReconstructionJobState::Succeeded)
                return SendPortableOutputDecision(channel, session, result,
                    wire::PortableDecision::Decision::Committed);
            if (attempt.snapshot.state == ReconstructionJobState::Failed ||
                attempt.snapshot.state == ReconstructionJobState::Interrupted) {
                if (result.outcome != wire::PortableResult::Outcome::Succeeded)
                    return SendPortableOutputDecision(channel, session, result,
                        wire::PortableDecision::Decision::Committed);
                return SendPortableOutputDecision(channel, session, result,
                    wire::PortableDecision::Decision::Rejected,
                    "leader rejected the reconstruction output");
            }
            if (attempt.snapshot.state == ReconstructionJobState::Rejected ||
                attempt.snapshot.state == ReconstructionJobState::Superseded)
                return SendPortableOutputDecision(channel, session, result,
                    wire::PortableDecision::Decision::Rejected,
                    "reconstruction attempt has already been rejected");
        } else if (TerminalReconstructionState(attempt.snapshot.state)) {
            return SendPortableOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Rejected,
                "reconstruction attempt is no longer active");
        }
        if (result.outcome != wire::PortableResult::Outcome::Succeeded) {
            if (!result.outputs.empty())
                return SendPortableOutputDecision(channel, session, result,
                    wire::PortableDecision::Decision::Rejected,
                    "failed reconstruction result cannot contain artifacts");
            const ReconstructionJobState state =
                result.outcome == wire::PortableResult::Outcome::Failed
                    ? ReconstructionJobState::Failed
                    : ReconstructionJobState::Interrupted;
            const std::string diagnostic = result.error.empty()
                ? (state == ReconstructionJobState::Failed
                    ? "worker reported reconstruction failure"
                    : "worker interrupted reconstruction")
                : result.error;
            if (!SetReconstructionResult(session, result, state, diagnostic))
                return SendPortableOutputDecision(channel, session, result,
                    wire::PortableDecision::Decision::Rejected,
                    "reconstruction attempt became stale before result commit");
            return SendPortableOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Committed);
        }

        if (result.outputs.empty()) {
            if (!attempt.has_result)
                FailUnacceptedReconstructionResult(session, result,
                    "successful reconstruction result has no sparse output");
            return SendPortableOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Rejected,
                "successful reconstruction result has no sparse output");
        }
        std::uint64_t output_bytes = 0;
        for (const TransferFile& file : result.outputs) {
            if (output_bytes > attempt.disk_budget_bytes ||
                file.size > attempt.disk_budget_bytes - output_bytes) {
                if (!attempt.has_result)
                    FailUnacceptedReconstructionResult(session, result,
                        "reconstruction output manifest exceeds the attempt quota");
                return SendPortableOutputDecision(channel, session, result,
                    wire::PortableDecision::Decision::Rejected,
                    "reconstruction output manifest exceeds the attempt quota");
            }
            output_bytes += file.size;
        }
        if (FeatureManifestJson(result.outputs).size() >
            kMaxReconstructionManifestJsonBytes) {
            if (!attempt.has_result)
                FailUnacceptedReconstructionResult(session, result,
                    "reconstruction output manifest exceeds the metadata limit");
            return SendPortableOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Rejected,
                "reconstruction output manifest exceeds the metadata limit");
        }
        if (!attempt.has_result) {
            if (attempt.snapshot.state != ReconstructionJobState::Running &&
                attempt.snapshot.state != ReconstructionJobState::Unknown)
                return SendPortableOutputDecision(channel, session, result,
                    wire::PortableDecision::Decision::Rejected,
                    "reconstruction attempt has not accepted its inputs");
            if (!SetReconstructionResult(session, result,
                    ReconstructionJobState::ReceivingOutput, {}))
                return SendPortableOutputDecision(channel, session, result,
                    wire::PortableDecision::Decision::Rejected,
                    "reconstruction attempt became stale before output transfer");
        } else if (attempt.snapshot.state !=
                       ReconstructionJobState::ReceivingOutput &&
                   attempt.snapshot.state != ReconstructionJobState::Unknown) {
            return SendPortableOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Rejected,
                "reconstruction output is no longer awaiting verification");
        }

        ReconstructionInputBundle inputs;
        try {
            inputs = VerifyReconstructionInputs(
                attempt.attempt_root / "input", attempt.inputs,
                attempt.snapshot.input_identity_sha256,
                attempt.disk_budget_bytes);
        } catch (const std::exception& exception) {
            const std::string diagnostic =
                std::string("reconstruction input verification failed: ") +
                exception.what();
            SetReconstructionResult(session, result,
                ReconstructionJobState::Failed, diagnostic);
            return SendPortableOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Rejected, diagnostic);
        }
        if (!IsSessionTrusted(session) ||
            !SendPortableOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Accepted))
            return false;
        const auto output_staging = attempt.attempt_root / "output-staging";
        const TransferResult received = ReceiveArtifacts(
            channel, output_staging, result.outputs, attempt.disk_budget_bytes);
        if (!received) {
            if (received.error != TransferError::Interrupted)
                SetReconstructionResult(session, result,
                    ReconstructionJobState::Failed,
                    std::string("reconstruction output transfer failed: ") +
                        received.message);
            return false;
        }
        if (!IsSessionTrusted(session)) return false;
        try {
            VerifyReconstructionOutputs(inputs, output_staging, result.outputs,
                                        attempt.disk_budget_bytes);
        } catch (const std::exception& exception) {
            const std::string diagnostic =
                std::string("reconstruction output verification failed: ") +
                exception.what();
            if (!SetReconstructionResult(session, result,
                    ReconstructionJobState::Failed, diagnostic))
                return false;
            return SendPortableOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Rejected, diagnostic);
        }
        if (!CommitVerifiedReconstructionResult(session, result))
            return SendPortableOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Rejected,
                "reconstruction attempt became stale or revoked before commit");
        return SendPortableOutputDecision(channel, session, result,
            wire::PortableDecision::Decision::Committed);
    }


#endif

#ifdef SS_TOOL_TRAIN
    bool SendTrainingOffer(
        TlsChannel& channel, const std::shared_ptr<Session>& session,
        const wire::Status& status, bool& offered) {
        offered = false;
        if (!IsSessionTrusted(session)) return false;
#ifdef SS_TOOL_SFM
        if (WorkerHasActiveFeature(session->worker_id) ||
            WorkerHasActiveReconstruction(session->worker_id))
            return true;
#endif
        wire::PortableOffer offer;
        {
            std::lock_guard<std::mutex> lock(training_mutex);
            TrainingAttempt* attempt =
                CurrentTrainingForWorkerLocked(session->worker_id);
            if (!attempt) return true;
            const bool queued = attempt->snapshot.state == TrainingJobState::Queued;
            const bool resuming = attempt->snapshot.state == TrainingJobState::Unknown &&
                !attempt->has_result && status.activity == wire::ActivityState::Idle;
            if (!queued && !resuming) return true;
            if (!SupportsTrainingBuild(status, attempt->snapshot.required_build) ||
                status.scheduling != wire::SchedulingState::Accepting ||
                status.activity != wire::ActivityState::Idle || !status.online ||
                status.maintenance || status.health != wire::HealthState::Healthy)
                return true;
            const std::uint64_t now = UnixNowMs();
            if (!now || now > kMaxSafeInteger - kTrainingOfferLifetimeMs)
                return false;
            offer.workload = wire::PortableWorkload::Training;
            offer.job_id = attempt->snapshot.job_id;
            offer.attempt_id = attempt->snapshot.attempt_id;
            offer.input_identity_sha256 =
                attempt->snapshot.input_identity_sha256;
            offer.required_build = attempt->snapshot.required_build;
            offer.expires_at_ms = now + kTrainingOfferLifetimeMs;
            offer.inputs = attempt->inputs;
            wire::Message check;
            check.payload = offer;
            std::string encoded;
            if (wire::Encode(check, now, encoded) != wire::Error::None) {
                std::string ignored;
                SetTrainingStateLocked(*attempt, TrainingJobState::Failed,
                    "training input offer exceeds wire limits", &ignored);
                return true;
            }
            std::string persist_error;
            if (!PersistTrainingChangeLocked(*attempt,
                    [&](TrainingAttempt& current) {
                        current.snapshot.state = TrainingJobState::Offered;
                        current.snapshot.error.clear();
                        current.expires_at_ms = offer.expires_at_ms;
                    }, &persist_error)) {
                SetSessionError(session, persist_error);
                return false;
            }
        }
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->waiting_portable_step = wire::PortableDecision::Step::Offer;
            session->waiting_portable_workload = wire::PortableWorkload::Training;
            session->waiting_portable_attempt = offer.attempt_id;
            session->waiting_portable_job = offer.job_id;
        }
        wire::Message message;
        message.payload = std::move(offer);
        std::lock_guard<std::mutex> lock(session->write_mutex);
        if (stopping.load(std::memory_order_acquire) ||
            session->stopping.load(std::memory_order_acquire))
            return false;
        if (!SendMessage(channel, message)) return false;
        offered = true;
        return true;
    }

    bool SendTrainingDecision(TlsChannel& channel,
                              const std::shared_ptr<Session>& session,
                              wire::PortableDecision decision) {
        if (!IsSessionTrusted(session)) return false;
        wire::Message message;
        message.payload = std::move(decision);
        std::lock_guard<std::mutex> lock(session->write_mutex);
        if (stopping.load(std::memory_order_acquire) ||
            session->stopping.load(std::memory_order_acquire))
            return false;
        return SendMessage(channel, message);
    }

    bool ReplyTrainingDecision(
        TlsChannel& channel, const std::shared_ptr<Session>& session,
        const wire::PortableDecision& incoming,
        wire::PortableDecision::Decision decision, const char* reason) {
        wire::PortableDecision response;
        response.workload = wire::PortableWorkload::Training;
        response.job_id = incoming.job_id;
        response.attempt_id = incoming.attempt_id;
        response.step = incoming.step;
        response.decision = decision;
        if (reason) response.reason = reason;
        return SendTrainingDecision(channel, session, std::move(response));
    }

    bool TrainingDecisionMatchesSession(
        const std::shared_ptr<Session>& session,
        const wire::PortableDecision& decision) {
        std::lock_guard<std::mutex> lock(session->mutex);
        return session->waiting_portable_step &&
               session->waiting_portable_workload ==
                   wire::PortableWorkload::Training &&
               *session->waiting_portable_step == decision.step &&
               session->waiting_portable_attempt == decision.attempt_id &&
               session->waiting_portable_job == decision.job_id;
    }

    void ClearTrainingWait(const std::shared_ptr<Session>& session) {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->waiting_portable_step.reset();
        session->waiting_portable_workload.reset();
        session->waiting_portable_attempt.clear();
        session->waiting_portable_job.clear();
    }

    bool ProcessTrainingDecision(
        TlsChannel& channel, const std::shared_ptr<Session>& session,
        const wire::PortableDecision& decision) {
        if (!IsSessionTrusted(session) ||
            decision.workload != wire::PortableWorkload::Training)
            return false;
        if (decision.step != wire::PortableDecision::Step::Offer &&
            decision.step != wire::PortableDecision::Step::Inputs)
            return ReplyTrainingDecision(channel, session, decision,
                wire::PortableDecision::Decision::Rejected,
                "leader does not accept this training decision step");
        if (!TrainingDecisionMatchesSession(session, decision))
            return ReplyTrainingDecision(channel, session, decision,
                wire::PortableDecision::Decision::Rejected,
                "training decision does not match the outstanding attempt");
        if (decision.decision == wire::PortableDecision::Decision::Committed)
            return false;

        if (decision.step == wire::PortableDecision::Step::Offer &&
            decision.decision == wire::PortableDecision::Decision::Rejected) {
            {
                std::lock_guard<std::mutex> lock(training_mutex);
                const auto found = training_attempts.find(
                    TrainingAttemptKey(decision.job_id, decision.attempt_id));
                if (found != training_attempts.end() &&
                    found->second.snapshot.worker_id == session->worker_id &&
                    IsCurrentTrainingAttemptLocked(found->second) &&
                    found->second.snapshot.state == TrainingJobState::Offered) {
                    std::string ignored;
                    SetTrainingStateLocked(found->second, TrainingJobState::Rejected,
                        decision.reason.empty()
                            ? "worker rejected the training offer" : decision.reason,
                        &ignored);
                }
            }
            ClearTrainingWait(session);
            return SendLeaderStatus(channel, session);
        }

        if (decision.step == wire::PortableDecision::Step::Offer) {
            wire::Status status;
            if (!TrainingSessionStatus(session, status)) return false;
            std::filesystem::path input_root;
            std::vector<TransferFile> inputs;
            std::uint64_t budget = 0;
            const char* rejection =
                "training attempt is stale or assigned to another worker";
            bool rejected = false;
            {
                std::lock_guard<std::mutex> lock(training_mutex);
                const auto found = training_attempts.find(
                    TrainingAttemptKey(decision.job_id, decision.attempt_id));
                if (found != training_attempts.end() &&
                    found->second.snapshot.worker_id == session->worker_id &&
                    IsCurrentTrainingAttemptLocked(found->second)) {
                    TrainingAttempt& attempt = found->second;
                    if (attempt.snapshot.state != TrainingJobState::Offered) {
                        rejection = "training offer is no longer active";
                        rejected = true;
                    } else if (UnixNowMs() >= attempt.expires_at_ms) {
                        std::string ignored;
                        SetTrainingStateLocked(attempt, TrainingJobState::Unknown,
                            "training offer expired before acceptance", &ignored);
                        rejection = "training offer expired";
                        rejected = true;
                    } else if (!SupportsTrainingBuild(
                                   status, attempt.snapshot.required_build) ||
                               status.scheduling != wire::SchedulingState::Accepting ||
                               !status.online || status.maintenance ||
                               status.health != wire::HealthState::Healthy ||
                               status.activity != wire::ActivityState::Idle) {
                        std::string ignored;
                        SetTrainingStateLocked(attempt, TrainingJobState::Rejected,
                            "worker eligibility changed before input transfer",
                            &ignored);
                        rejection =
                            "worker is no longer eligible for this training offer";
                        rejected = true;
                    } else {
                        std::string persist_error;
                        if (!SetTrainingStateLocked(attempt,
                                TrainingJobState::TransferringInputs, {},
                                &persist_error)) {
                            SetSessionError(session, persist_error);
                            return false;
                        }
                        input_root = TrainingWorkspace(attempt) / "input";
                        inputs = attempt.inputs;
                        budget = attempt.disk_budget_bytes;
                    }
                } else {
                    rejected = true;
                }
            }
            if (rejected || inputs.empty()) {
                ClearTrainingWait(session);
                return ReplyTrainingDecision(channel, session, decision,
                    wire::PortableDecision::Decision::Rejected, rejection);
            }
            const TransferResult sent =
                SendArtifacts(channel, input_root, inputs, budget);
            if (!sent) {
                if (sent.error != TransferError::Interrupted) {
                    std::lock_guard<std::mutex> lock(training_mutex);
                    const auto found = training_attempts.find(
                        TrainingAttemptKey(decision.job_id, decision.attempt_id));
                    if (found != training_attempts.end() &&
                        IsCurrentTrainingAttemptLocked(found->second)) {
                        std::string ignored;
                        SetTrainingStateLocked(found->second,
                            TrainingJobState::Failed,
                            std::string("input transfer failed: ") + sent.message,
                            &ignored);
                    }
                }
                return false;
            }
            if (!IsSessionTrusted(session)) return false;
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                session->waiting_portable_step =
                    wire::PortableDecision::Step::Inputs;
            }
            return true;
        }

        if (decision.decision == wire::PortableDecision::Decision::Rejected) {
            {
                std::lock_guard<std::mutex> lock(training_mutex);
                const auto found = training_attempts.find(
                    TrainingAttemptKey(decision.job_id, decision.attempt_id));
                if (found != training_attempts.end() &&
                    found->second.snapshot.worker_id == session->worker_id &&
                    IsCurrentTrainingAttemptLocked(found->second)) {
                    std::string ignored;
                    SetTrainingStateLocked(found->second, TrainingJobState::Rejected,
                        decision.reason.empty()
                            ? "worker rejected training inputs" : decision.reason,
                        &ignored);
                }
            }
            ClearTrainingWait(session);
            return SendLeaderStatus(channel, session);
        }

        bool current = false;
        {
            std::lock_guard<std::mutex> lock(training_mutex);
            const auto found = training_attempts.find(
                TrainingAttemptKey(decision.job_id, decision.attempt_id));
            if (found != training_attempts.end() &&
                found->second.snapshot.worker_id == session->worker_id &&
                IsCurrentTrainingAttemptLocked(found->second) &&
                found->second.snapshot.state ==
                    TrainingJobState::TransferringInputs) {
                std::string persist_error;
                current = SetTrainingStateLocked(found->second,
                    TrainingJobState::Running, {}, &persist_error);
                if (!current) SetSessionError(session, persist_error);
            }
        }
        ClearTrainingWait(session);
        if (!current)
            return ReplyTrainingDecision(channel, session, decision,
                wire::PortableDecision::Decision::Rejected,
                "training attempt is stale or no longer transferring inputs");
        return true;
    }

    bool SendTrainingOutputDecision(
        TlsChannel& channel, const std::shared_ptr<Session>& session,
        const wire::PortableResult& result,
        wire::PortableDecision::Decision decision,
        const std::string& reason = {}) {
        wire::PortableDecision response;
        response.workload = wire::PortableWorkload::Training;
        response.job_id = result.job_id;
        response.attempt_id = result.attempt_id;
        response.step = wire::PortableDecision::Step::Output;
        response.decision = decision;
        response.reason = reason;
        return SendTrainingDecision(channel, session, std::move(response));
    }

    bool SameTrainingResult(const TrainingAttempt& attempt,
                            const wire::PortableResult& result) const {
        if (!attempt.has_result || attempt.outcome != result.outcome ||
            attempt.result_error_sha256 != TrainingErrorDigest(result.error) ||
            attempt.result_checkpoint != result.output_metadata ||
            attempt.outputs.size() != result.outputs.size())
            return false;
        auto left = attempt.outputs;
        auto right = result.outputs;
        const auto by_path = [](const TransferFile& a, const TransferFile& b) {
            return a.path < b.path;
        };
        std::sort(left.begin(), left.end(), by_path);
        std::sort(right.begin(), right.end(), by_path);
        for (std::size_t i = 0; i < left.size(); ++i)
            if (left[i].path != right[i].path ||
                left[i].size != right[i].size ||
                left[i].sha256 != right[i].sha256)
                return false;
        return true;
    }

    bool FailUnacceptedTrainingResult(
        const std::shared_ptr<Session>& session,
        const wire::PortableResult& result, std::string diagnostic) {
        if (!IsSessionTrusted(session)) return false;
        std::lock_guard<std::mutex> lock(training_mutex);
        const auto found = training_attempts.find(
            TrainingAttemptKey(result.job_id, result.attempt_id));
        if (found == training_attempts.end() ||
            found->second.snapshot.worker_id != session->worker_id ||
            !IsCurrentTrainingAttemptLocked(found->second) ||
            found->second.has_result ||
            (found->second.snapshot.state != TrainingJobState::Running &&
             found->second.snapshot.state != TrainingJobState::Unknown))
            return false;
        std::string persist_error;
        return SetTrainingStateLocked(found->second, TrainingJobState::Failed,
                                      std::move(diagnostic), &persist_error);
    }

    bool SetTrainingResult(
        const std::shared_ptr<Session>& session,
        const wire::PortableResult& result, TrainingJobState state,
        std::string diagnostic) {
        if (!IsSessionTrusted(session)) return false;
        std::lock_guard<std::mutex> lock(training_mutex);
        const auto found = training_attempts.find(
            TrainingAttemptKey(result.job_id, result.attempt_id));
        if (found == training_attempts.end() ||
            found->second.snapshot.worker_id != session->worker_id ||
            !IsCurrentTrainingAttemptLocked(found->second))
            return false;
        TrainingAttempt& attempt = found->second;
        if (attempt.has_result && !SameTrainingResult(attempt, result))
            return false;
        if ((!attempt.has_result &&
             attempt.snapshot.state != TrainingJobState::Running &&
             attempt.snapshot.state != TrainingJobState::Unknown) ||
            (attempt.has_result &&
             attempt.snapshot.state != TrainingJobState::ReceivingOutput &&
             attempt.snapshot.state != TrainingJobState::Unknown))
            return false;
        TrainingAttempt previous = attempt;
        attempt.has_result = true;
        attempt.outcome = result.outcome;
        attempt.outputs = result.outputs;
        attempt.result_checkpoint = result.output_metadata;
        attempt.result_error_sha256 = TrainingErrorDigest(result.error);
        attempt.snapshot.state = state;
        attempt.snapshot.error = std::move(diagnostic);
        attempt.snapshot.error.resize(std::min<std::size_t>(
            attempt.snapshot.error.size(), 4096));
        std::string persist_error;
        if (PersistTrainingJobsLocked(&persist_error)) return true;
        attempt = std::move(previous);
        SetSessionError(session, persist_error);
        return false;
    }

    bool CommitVerifiedTrainingResult(
        const std::shared_ptr<Session>& session,
        const wire::PortableResult& result,
        const TrainingOutputBundle& verified) {
        std::lock_guard<std::mutex> pair_lock(pairing_mutex);
        const auto worker = pairing_leader.WorkerForPeer(session->peer_fingerprint);
        if (!worker || worker->id != session->worker_id ||
            worker->status != pairing::WorkerStatus::Paired || worker->revoked ||
            session->stopping.load(std::memory_order_acquire))
            return false;
        std::lock_guard<std::mutex> lock(training_mutex);
        const auto found = training_attempts.find(
            TrainingAttemptKey(result.job_id, result.attempt_id));
        if (found == training_attempts.end() ||
            found->second.snapshot.worker_id != session->worker_id ||
            !IsCurrentTrainingAttemptLocked(found->second) ||
            found->second.snapshot.state != TrainingJobState::ReceivingOutput ||
            !SameTrainingResult(found->second, result))
            return false;
        TrainingAttempt& attempt = found->second;
        const auto workspace = TrainingWorkspace(attempt);
        const auto output_root = workspace / "output" / "run";
        const auto io_output_root = NativeFilesystemPath(output_root);
        std::error_code ec;
        const auto output_parent_status =
            std::filesystem::symlink_status(
                NativeFilesystemPath(output_root.parent_path()), ec);
        if (ec || !std::filesystem::is_directory(output_parent_status) ||
            std::filesystem::is_symlink(output_parent_status)) {
            SetSessionError(session, "training output parent path is unsafe");
            return false;
        }
        ec.clear();
        const auto output_status =
            std::filesystem::symlink_status(io_output_root, ec);
        if (ec || !std::filesystem::is_directory(output_status) ||
            std::filesystem::is_symlink(output_status)) {
            SetSessionError(session, "training output staging path is unsafe");
            return false;
        }
        ec.clear();
        const auto result_root =
            NativeFilesystemPath(attempt.snapshot.result_root);
        const auto result_status = std::filesystem::symlink_status(result_root, ec);
        if (!ec || ec != std::errc::no_such_file_or_directory) {
            SetSessionError(session, "training result path already exists");
            return false;
        }
        ec.clear();
        std::filesystem::rename(io_output_root, result_root, ec);
        if (ec) {
            SetSessionError(session, "could not publish verified training output");
            return false;
        }
        TrainingAttempt previous = attempt;
        attempt.snapshot.state = TrainingJobState::Succeeded;
        attempt.snapshot.error.clear();
        attempt.result_checkpoint = result.output_metadata;
        attempt.snapshot.returned_checkpoint = result.output_metadata;
        attempt.snapshot.checkpoint_dir = LogicalFilesystemPath(
            attempt.snapshot.result_root /
            std::filesystem::u8path(result.output_metadata));
        attempt.snapshot.resume_config = verified.resume_config;
        attempt.snapshot.resume_config.resume =
            LogicalFilesystemPath(attempt.snapshot.checkpoint_dir).u8string();
        attempt.snapshot.resume_config.output_dir_prefix =
            LogicalFilesystemPath(
                attempt.snapshot.result_root.parent_path()).u8string();
        attempt.snapshot.resume_config.output_dir_name =
            attempt.snapshot.result_root.filename().u8string();
        std::string persist_error;
        if (PersistTrainingJobsLocked(&persist_error)) return true;
        attempt = std::move(previous);
        std::error_code rollback_error;
        std::filesystem::rename(result_root, io_output_root, rollback_error);
        SetSessionError(session, persist_error);
        return false;
    }

    bool ProcessTrainingResult(
        TlsChannel& channel, const std::shared_ptr<Session>& session,
        const wire::PortableResult& result) {
        if (!IsSessionTrusted(session) ||
            result.workload != wire::PortableWorkload::Training)
            return false;
        wire::Status status;
        if (!TrainingSessionStatus(session, status))
            return SendTrainingOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Rejected,
                "worker status is unavailable for training result publication");
        TrainingAttempt attempt;
        bool found_attempt = false;
        {
            std::lock_guard<std::mutex> lock(training_mutex);
            const auto found = training_attempts.find(
                TrainingAttemptKey(result.job_id, result.attempt_id));
            if (found != training_attempts.end() &&
                found->second.snapshot.worker_id == session->worker_id &&
                IsCurrentTrainingAttemptLocked(found->second)) {
                attempt = found->second;
                found_attempt = true;
            }
        }
        if (!found_attempt)
            return SendTrainingOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Rejected,
                "training attempt is stale or assigned to another worker");
        if (!SupportsTrainingBuild(status, attempt.snapshot.required_build))
            return SendTrainingOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Rejected,
                "worker build or training capability does not match this attempt");
        if (!status.online || status.connection != wire::ConnectionState::Connected ||
            status.maintenance || status.health != wire::HealthState::Healthy)
            return SendTrainingOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Rejected,
                "worker is offline or unhealthy for training result publication");

        if (attempt.has_result) {
            if (!SameTrainingResult(attempt, result))
                return SendTrainingOutputDecision(channel, session, result,
                    wire::PortableDecision::Decision::Rejected,
                    "duplicate training result differs from the accepted result");
            if (attempt.snapshot.state == TrainingJobState::Succeeded)
                return SendTrainingOutputDecision(channel, session, result,
                    wire::PortableDecision::Decision::Committed);
            if (attempt.snapshot.state == TrainingJobState::Failed ||
                attempt.snapshot.state == TrainingJobState::Interrupted) {
                if (result.outcome != wire::PortableResult::Outcome::Succeeded)
                    return SendTrainingOutputDecision(channel, session, result,
                        wire::PortableDecision::Decision::Committed);
                return SendTrainingOutputDecision(channel, session, result,
                    wire::PortableDecision::Decision::Rejected,
                    "leader rejected the training output");
            }
            if (attempt.snapshot.state == TrainingJobState::Rejected ||
                attempt.snapshot.state == TrainingJobState::Superseded)
                return SendTrainingOutputDecision(channel, session, result,
                    wire::PortableDecision::Decision::Rejected,
                    "training attempt has already been rejected");
        } else if (TerminalTrainingState(attempt.snapshot.state)) {
            return SendTrainingOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Rejected,
                "training attempt is no longer active");
        }

        if (result.outcome != wire::PortableResult::Outcome::Succeeded) {
            if (!result.outputs.empty() || !result.output_metadata.empty()) {
                if (!attempt.has_result)
                    FailUnacceptedTrainingResult(session, result,
                        "failed training result cannot contain artifacts or checkpoint metadata");
                return SendTrainingOutputDecision(channel, session, result,
                    wire::PortableDecision::Decision::Rejected,
                    "failed training result cannot contain artifacts or checkpoint metadata");
            }
            const TrainingJobState state =
                result.outcome == wire::PortableResult::Outcome::Failed
                    ? TrainingJobState::Failed : TrainingJobState::Interrupted;
            const std::string diagnostic = result.error.empty()
                ? (state == TrainingJobState::Failed
                    ? "worker reported training failure"
                    : "worker interrupted training")
                : result.error;
            if (!SetTrainingResult(session, result, state, diagnostic))
                return SendTrainingOutputDecision(channel, session, result,
                    wire::PortableDecision::Decision::Rejected,
                    "training attempt became stale before result commit");
            return SendTrainingOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Committed);
        }

        if (result.outputs.empty() || result.output_metadata.empty()) {
            if (!attempt.has_result)
                FailUnacceptedTrainingResult(session, result,
                    "successful training result needs output files and one checkpoint basename");
            return SendTrainingOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Rejected,
                "successful training result needs output files and one checkpoint basename");
        }
        std::uint64_t output_bytes = 0;
        for (const TransferFile& file : result.outputs) {
            if (output_bytes > attempt.disk_budget_bytes ||
                file.size > attempt.disk_budget_bytes - output_bytes) {
                if (!attempt.has_result)
                    FailUnacceptedTrainingResult(session, result,
                        "training output manifest exceeds the attempt quota");
                return SendTrainingOutputDecision(channel, session, result,
                    wire::PortableDecision::Decision::Rejected,
                    "training output manifest exceeds the attempt quota");
            }
            output_bytes += file.size;
        }
        if (TrainingManifestJson(result.outputs).size() >
            kMaxTrainingManifestJsonBytes) {
            if (!attempt.has_result)
                FailUnacceptedTrainingResult(session, result,
                    "training output manifest exceeds the metadata limit");
            return SendTrainingOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Rejected,
                "training output manifest exceeds the metadata limit");
        }
        if (!attempt.has_result) {
            if (attempt.snapshot.state != TrainingJobState::Running &&
                attempt.snapshot.state != TrainingJobState::Unknown)
                return SendTrainingOutputDecision(channel, session, result,
                    wire::PortableDecision::Decision::Rejected,
                    "training attempt has not accepted its inputs");
            if (!SetTrainingResult(session, result,
                    TrainingJobState::ReceivingOutput, {}))
                return SendTrainingOutputDecision(channel, session, result,
                    wire::PortableDecision::Decision::Rejected,
                    "training attempt became stale before output transfer");
        } else if (attempt.snapshot.state != TrainingJobState::ReceivingOutput &&
                   attempt.snapshot.state != TrainingJobState::Unknown) {
            return SendTrainingOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Rejected,
                "training output is no longer awaiting verification");
        }

        TrainingInputBundle inputs;
        try {
            const auto workspace = TrainingWorkspace(attempt);
            inputs = VerifyTrainingInputs(workspace / "input", attempt.inputs,
                attempt.snapshot.input_identity_sha256, attempt.disk_budget_bytes);
        } catch (const std::exception& exception) {
            const std::string diagnostic =
                std::string("training input verification failed: ") +
                exception.what();
            SetTrainingResult(session, result, TrainingJobState::Failed, diagnostic);
            return SendTrainingOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Rejected, diagnostic);
        }
        if (!IsSessionTrusted(session)) return false;
        const auto io_output_root = NativeFilesystemPath(inputs.output_root);
        std::error_code ec;
        std::filesystem::create_directories(
            NativeFilesystemPath(inputs.output_root.parent_path()), ec);
        if (ec) {
            const std::string diagnostic =
                "could not create training output staging directory";
            SetTrainingResult(session, result, TrainingJobState::Failed, diagnostic);
            return SendTrainingOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Rejected, diagnostic);
        }
        const auto parent_status = std::filesystem::symlink_status(
            NativeFilesystemPath(inputs.output_root.parent_path()), ec);
        if (ec || !std::filesystem::is_directory(parent_status) ||
            std::filesystem::is_symlink(parent_status)) {
            const std::string diagnostic =
                "training output staging directory is unsafe";
            SetTrainingResult(session, result, TrainingJobState::Failed, diagnostic);
            return SendTrainingOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Rejected, diagnostic);
        }
        std::string staging_error;
        if (!EnsureTransferRoot(inputs.output_root, &staging_error)) {
            const std::string diagnostic =
                std::string("training output staging failed: ") + staging_error;
            SetTrainingResult(session, result, TrainingJobState::Failed, diagnostic);
            return SendTrainingOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Rejected, diagnostic);
        }
        const auto remove_staged_output = [&] {
            std::error_code ignored;
            std::filesystem::remove_all(io_output_root, ignored);
        };
        if (!SendTrainingOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Accepted))
            return false;
        const TransferResult received = ReceiveArtifacts(
            channel, inputs.output_root, result.outputs, attempt.disk_budget_bytes);
        if (!received) {
            if (received.error != TransferError::Interrupted) {
                SetTrainingResult(session, result, TrainingJobState::Failed,
                    std::string("training output transfer failed: ") +
                        received.message);
                remove_staged_output();
            }
            return false;
        }
        if (!IsSessionTrusted(session)) return false;

        TrainingOutputBundle verified;
        try {
            const TrainingOutputPackage package{
                result.outputs, attempt.snapshot.input_identity_sha256,
                result.output_metadata};
            verified = VerifyTrainingOutputs(inputs, inputs.output_root,
                package, attempt.disk_budget_bytes);
        } catch (const std::exception& exception) {
            const std::string diagnostic =
                std::string("training output verification failed: ") +
                exception.what();
            const bool recorded = SetTrainingResult(session, result,
                TrainingJobState::Failed, diagnostic);
            remove_staged_output();
            if (!recorded) return false;
            return SendTrainingOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Rejected, diagnostic);
        }
        if (!CommitVerifiedTrainingResult(session, result, verified)) {
            bool superseded = false;
            {
                std::lock_guard<std::mutex> lock(training_mutex);
                const auto found = training_attempts.find(
                    TrainingAttemptKey(result.job_id, result.attempt_id));
                superseded = found != training_attempts.end() &&
                    found->second.snapshot.state == TrainingJobState::Superseded;
            }
            if (superseded) {
                std::error_code ignored;
                std::filesystem::remove_all(
                    NativeFilesystemPath(TrainingWorkspace(attempt) / "output"),
                    ignored);
            }
            return SendTrainingOutputDecision(channel, session, result,
                wire::PortableDecision::Decision::Rejected,
                "training attempt became stale or revoked before commit");
        }
        return SendTrainingOutputDecision(channel, session, result,
            wire::PortableDecision::Decision::Committed);
    }
#endif
    bool SendLeaderStatus(TlsChannel& channel,
                          const std::shared_ptr<Session>& session) {
        std::lock_guard<std::mutex> lock(session->write_mutex);
        if (stopping.load(std::memory_order_acquire) ||
            session->stopping.load(std::memory_order_acquire))
            return false;
        wire::Message response;
        response.payload = LeaderStatus();
        return SendMessage(channel, response);
    }

    bool ProcessStatus(TlsChannel& channel, const std::shared_ptr<Session>& session,
                       const wire::Status& received) {
        if (stopping.load(std::memory_order_acquire) ||
            session->stopping.load(std::memory_order_acquire))
            return false;
        if (received.connection != wire::ConnectionState::Connected) {
            SetSessionError(session, "worker status did not report a connected session");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (session->waiting_feature_step) {
                session->error = "worker heartbeat arrived during a feature decision";
                return false;
            }
            if (session->waiting_portable_step) {
                session->error =
                    "worker heartbeat arrived during a portable job decision";
                return false;
            }
            if (!session->admin_transfer_in_progress.empty()) {
                session->error = "worker status arrived during an admin package transfer";
                return false;
            }
            session->status = SnapshotStatus(received);
            session->last_seen_unix_ms = UnixNowMs();
            session->has_status = true;
            session->error.clear();
        }

        std::shared_ptr<PendingCommand> pending;
        std::optional<std::string> encoded;
        bool has_live_command = false;
        bool previously_sent = false;
        std::weak_ptr<Session> previous_sent_session;
        std::string persistence_error;
        {
            std::lock_guard<std::mutex> lock(command_mutex);
            if (RefreshExpiredCommandsLocked(UnixNowMs(),
                                             &persistence_error)) {
                const auto found = active_commands.find(session->worker_id);
                if (found != active_commands.end()) {
                    pending = found->second;
                    has_live_command = true;
                    const std::uint64_t now = UnixNowMs();
                    const bool expired_admin_replay =
                        IsAdminCommand(pending->command.action) &&
                        pending->ever_sent && !pending->accepted &&
                        now >= pending->command.expires_at_ms &&
                        now - pending->command.expires_at_ms <
                            kAdminResolutionGraceMs;
                    if ((pending->command.expires_at_ms > now ||
                         expired_admin_replay) &&
                        pending->sent_session.lock() != session &&
                        !(pending->accepted &&
                          IsAdminCommand(pending->command.action))) {
                        wire::Message message;
                        message.payload = pending->command;
                        const std::uint64_t validation_time =
                            expired_admin_replay
                                ? pending->command.issued_at_ms : now;
                        encoded = EncodeMessage(message, validation_time);
                        if (encoded) {
                            const CommandResult previous = pending->result;
                            const bool previous_ever_sent = pending->ever_sent;
                            const auto previous_session = pending->sent_session;
                            previously_sent = pending->ever_sent;
                            previous_sent_session = pending->sent_session;
                            pending->ever_sent = true;
                            pending->sent_session = session;
                            pending->result.state = CommandState::Pending;
                            if (!PersistCommandStateLocked(&persistence_error)) {
                                pending->result = previous;
                                pending->ever_sent = previous_ever_sent;
                                pending->sent_session = previous_session;
                                encoded.reset();
                            }
                        } else {
                            persistence_error = "could not encode pending command";
                        }
                    }
                }
            }
        }
        if (!persistence_error.empty()) {
            SetSessionError(session, persistence_error);
            return false;
        }

        if (encoded) {
            const auto record_unsent_expiration = [&]() {
                std::string persist_error;
                bool persisted = true;
                {
                    std::lock_guard<std::mutex> lock(command_mutex);
                    const auto current = active_commands.find(session->worker_id);
                    if (current != active_commands.end() &&
                        current->second == pending && !pending->terminal &&
                        !pending->accepted) {
                        const CommandResult previous = pending->result;
                        const bool previous_terminal = pending->terminal;
                        const bool previous_notified = pending->waiter_notified;
                        const bool previous_ever_sent = pending->ever_sent;
                        const auto previous_session = pending->sent_session;
                        if (!previously_sent) {
                            pending->ever_sent = false;
                            pending->sent_session = previous_sent_session;
                            pending->result.state = CommandState::Expired;
                            pending->terminal = true;
                            active_commands.erase(current);
                        } else {
                            pending->result.state = CommandState::Uncertain;
                        }
                        pending->waiter_notified = true;
                        if (!PersistCommandStateLocked(&persist_error)) {
                            pending->result = previous;
                            pending->terminal = previous_terminal;
                            pending->waiter_notified = previous_notified;
                            pending->ever_sent = previous_ever_sent;
                            pending->sent_session = previous_session;
                            active_commands[session->worker_id] = pending;
                            persisted = false;
                        }
                    }
                }
                if (!persisted)
                    SetSessionError(session, persist_error);
                condition.notify_all();
                return persisted;
            };
            const std::uint64_t now = UnixNowMs();
            const bool expired_admin_replay =
                IsAdminCommand(pending->command.action) &&
                pending->ever_sent && !pending->accepted &&
                now >= pending->command.expires_at_ms &&
                now - pending->command.expires_at_ms <
                    kAdminResolutionGraceMs;
            if (now >= pending->command.expires_at_ms &&
                !expired_admin_replay) {
                if (!record_unsent_expiration()) return false;
                return SendLeaderStatus(channel, session);
            }
            bool command_sent = false;
            {
                std::lock_guard<std::mutex> write_lock(session->write_mutex);
                if (stopping.load(std::memory_order_acquire) ||
                    session->stopping.load(std::memory_order_acquire))
                    return false;
                const std::uint64_t send_now = UnixNowMs();
                const bool replay_after_expiry =
                    IsAdminCommand(pending->command.action) &&
                    pending->ever_sent && !pending->accepted &&
                    send_now >= pending->command.expires_at_ms &&
                    send_now - pending->command.expires_at_ms <
                        kAdminResolutionGraceMs;
                if (send_now < pending->command.expires_at_ms ||
                    replay_after_expiry) {
                    const std::string& bytes = *encoded;
                    if (!channel.SendFrame(
                            reinterpret_cast<const std::uint8_t*>(bytes.data()),
                            bytes.size()))
                        return false;
                    command_sent = true;
                    if (pending->command.action ==
                        wire::CommandAction::ActivateUpdate) {
                        const auto& offer = *pending->command.package_offer;
                        const auto staging = options.state_root /
                            "agent-pairing" / kLeaderUpdateStore /
                            offer.manifest.sha256;
                        const TransferFile package{
                            "package.bin", offer.manifest.size,
                            offer.manifest.sha256};
                        {
                            std::lock_guard<std::mutex> session_lock(session->mutex);
                            session->last_admin_transfer_command_id.clear();
                            session->admin_transfer_in_progress =
                                pending->command.command_id;
                        }
                        const TransferResult transferred = SendArtifacts(
                            channel, staging, {package},
                            offer.manifest.size + 4096);
                        {
                            std::lock_guard<std::mutex> session_lock(session->mutex);
                            if (transferred)
                                session->last_admin_transfer_command_id =
                                    pending->command.command_id;
                            session->admin_transfer_in_progress.clear();
                        }
                        if (!transferred) {
                            SetSessionError(session,
                                std::string("update package transfer failed: ") +
                                    transferred.message);
                            return false;
                        }
                    }
                }
            }
            if (command_sent) return true;
            if (!record_unsent_expiration()) return false;
            return SendLeaderStatus(channel, session);
        }
        if (has_live_command) return SendLeaderStatus(channel, session);

        bool offered = false;
#ifdef SS_TOOL_SFM
        if (!SendFeatureOffer(channel, session, received, offered)) return false;
        if (offered) return true;
        if (!SendReconstructionOffer(channel, session, received, offered))
            return false;
        if (offered) return true;
#endif
#ifdef SS_TOOL_TRAIN
        if (!SendTrainingOffer(channel, session, received, offered))
            return false;
        if (offered) return true;
#endif
        return SendLeaderStatus(channel, session);
    }

    bool ProcessAcknowledgment(const std::shared_ptr<Session>& session,
                               const wire::Acknowledgment& acknowledgment) {
        bool accepted = false;
        bool package_transfer_finished = false;
        {
            std::lock_guard<std::mutex> session_lock(session->mutex);
            package_transfer_finished =
                session->admin_transfer_in_progress.empty() &&
                session->last_admin_transfer_command_id ==
                    acknowledgment.command_id;
        }
        bool persistence_failed = false;
        std::string persistence_error;
        {
            std::lock_guard<std::mutex> lock(command_mutex);
            const auto found = std::find_if(command_history.begin(),
                command_history.end(), [&](const auto& item) {
                    return item->command.command_id == acknowledgment.command_id;
                });
            if (found != command_history.end()) {
                const auto& item = *found;
                const wire::Command& command = item->command;
                if (item->result.worker_id == session->worker_id &&
                    acknowledgment.leader_id == command.leader_id &&
                    acknowledgment.leader_epoch == command.leader_epoch &&
                    acknowledgment.sequence == command.sequence &&
                    acknowledgment.action == command.action &&
                    (command.action != wire::CommandAction::ActivateUpdate ||
                     package_transfer_finished || item->accepted ||
                     (item->terminal &&
                      item->result.state == CommandState::Uncertain))) {
                    if (item->terminal) {
                        const bool late_admin_ack =
                            IsAdminCommand(command.action) &&
                            item->result.state == CommandState::Uncertain &&
                            item->ever_sent &&
                            (acknowledgment.outcome ==
                                 wire::AcknowledgmentOutcome::Accepted ||
                             acknowledgment.outcome ==
                                 wire::AcknowledgmentOutcome::Completed ||
                             acknowledgment.outcome ==
                                 wire::AcknowledgmentOutcome::Rejected ||
                             acknowledgment.outcome ==
                                 wire::AcknowledgmentOutcome::Failed);
                        if (!late_admin_ack) {
                            accepted = item->result.acknowledgment_received &&
                                acknowledgment.outcome ==
                                    item->result.acknowledgment &&
                                (acknowledgment.outcome ==
                                     wire::AcknowledgmentOutcome::Completed ||
                                 acknowledgment.outcome ==
                                     wire::AcknowledgmentOutcome::Rejected ||
                                 acknowledgment.outcome ==
                                     wire::AcknowledgmentOutcome::Failed);
                        } else if (acknowledgment.outcome ==
                                       wire::AcknowledgmentOutcome::Accepted &&
                                   item->accepted &&
                                   item->result.acknowledgment_received &&
                                   item->result.acknowledgment ==
                                       wire::AcknowledgmentOutcome::Accepted) {
                            accepted = true;
                        } else {
                            const CommandResult previous = item->result;
                            const bool previous_accepted = item->accepted;
                            const bool previous_waiter_notified =
                                item->waiter_notified;
                            item->result.acknowledgment =
                                acknowledgment.outcome;
                            item->result.acknowledgment_received = true;
                            if (acknowledgment.outcome ==
                                wire::AcknowledgmentOutcome::Accepted) {
                                item->accepted = true;
                            } else {
                                switch (acknowledgment.outcome) {
                                    case wire::AcknowledgmentOutcome::Completed:
                                        item->result.state = CommandState::Applied;
                                        break;
                                    case wire::AcknowledgmentOutcome::Rejected:
                                        item->result.state = CommandState::Rejected;
                                        break;
                                    case wire::AcknowledgmentOutcome::Failed:
                                        item->result.state = CommandState::Failed;
                                        break;
                                    case wire::AcknowledgmentOutcome::Accepted:
                                        break;
                                }
                            }
                            item->waiter_notified = true;
                            if (PersistCommandStateLocked(&persistence_error)) {
                                accepted = true;
                            } else {
                                item->result = previous;
                                item->accepted = previous_accepted;
                                item->waiter_notified =
                                    previous_waiter_notified;
                                persistence_failed = true;
                            }
                        }
                    } else if (item->ever_sent) {
                        const auto active = active_commands.find(session->worker_id);
                        if (active != active_commands.end() &&
                            active->second == item) {
                            if (acknowledgment.outcome ==
                                wire::AcknowledgmentOutcome::Accepted) {
                                if (item->accepted) {
                                    accepted =
                                        item->result.acknowledgment ==
                                            wire::AcknowledgmentOutcome::Accepted &&
                                        item->result.state == CommandState::Pending;
                                } else {
                                    const CommandResult previous = item->result;
                                    const bool previous_waiter_notified =
                                        item->waiter_notified;
                                    item->accepted = true;
                                    item->result.acknowledgment =
                                        wire::AcknowledgmentOutcome::Accepted;
                                    item->result.acknowledgment_received = true;
                                    item->result.state = CommandState::Pending;
                                    item->waiter_notified = true;
                                    if (PersistCommandStateLocked(&persistence_error))
                                        accepted = true;
                                    else {
                                        item->result = previous;
                                        item->accepted = false;
                                        item->waiter_notified =
                                            previous_waiter_notified;
                                        persistence_failed = true;
                                    }
                                }
                            } else {
                                const CommandResult previous = item->result;
                                const bool previous_terminal = item->terminal;
                                const bool previous_waiter_notified =
                                    item->waiter_notified;
                                item->result.acknowledgment = acknowledgment.outcome;
                                item->result.acknowledgment_received = true;
                                switch (acknowledgment.outcome) {
                                    case wire::AcknowledgmentOutcome::Completed:
                                        item->result.state = CommandState::Applied;
                                        break;
                                    case wire::AcknowledgmentOutcome::Rejected:
                                        item->result.state = CommandState::Rejected;
                                        break;
                                    case wire::AcknowledgmentOutcome::Failed:
                                        item->result.state = CommandState::Failed;
                                        break;
                                    case wire::AcknowledgmentOutcome::Accepted:
                                        break;
                                }
                                item->terminal = true;
                                item->waiter_notified = true;
                                if (PersistCommandStateLocked(&persistence_error)) {
                                    active_commands.erase(active);
                                    accepted = true;
                                } else {
                                    item->result = previous;
                                    item->terminal = previous_terminal;
                                    item->waiter_notified =
                                        previous_waiter_notified;
                                    persistence_failed = true;
                                }
                            }
                        }
                    }
                }
            }
        }
        if (persistence_failed) {
            SetSessionError(session, persistence_error);
            return false;
        }
        if (!accepted) {
            SetSessionError(session,
                "worker acknowledgment did not match command history");
            return false;
        }
        condition.notify_all();
        return true;
    }
    void HandleOperationalSession(const std::shared_ptr<Session>& session) noexcept {
        std::optional<TlsChannel> channel;
        struct Cleanup final {
            Impl& impl;
            const std::shared_ptr<Session>& session;
            std::optional<TlsChannel>& channel;
            ~Cleanup() {
                impl.MarkDisconnected(session);
                const NativeSocket socket = session->socket;
                session->socket = TlsChannel::kInvalidSocket;
                if (channel) channel->Close();
                else CloseSocket(FromNative(socket));
                {
                    std::lock_guard<std::mutex> lock(session->cancel_mutex);
                    const NativeSocket cancel_socket = session->cancel_socket;
                    session->cancel_socket = TlsChannel::kInvalidSocket;
                    CloseSocket(FromNative(cancel_socket));
                }
                if (!impl.stopping.load(std::memory_order_acquire))
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        } cleanup{*this, session, channel};
        try {
            if (stopping.load(std::memory_order_acquire) ||
                session->stopping.load(std::memory_order_acquire))
                return;
            std::optional<TlsChannel::Options> tls_options;
            {
                std::lock_guard<std::mutex> lock(pairing_mutex);
                tls_options = pairing_leader.OptionsForLeader();
            }
            if (!tls_options) return;
            tls_options->handshake_timeout_ms =
                static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    kHandshakeTimeout).count());
            tls_options->io_timeout_ms =
                static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    kIoTimeout).count());
            if (session->stopping.load(std::memory_order_acquire)) return;
            channel = TlsChannel::AcceptServer(session->socket, *tls_options);
            if (!channel) session->socket = TlsChannel::kInvalidSocket;
            if (!channel || stopping.load(std::memory_order_acquire) ||
                session->stopping.load(std::memory_order_acquire))
                return;
            const auto pin = channel->PeerSpkiSha256();
            if (!pin) return;
            {
                std::lock_guard<std::mutex> pair_lock(pairing_mutex);
                const auto worker = pairing_leader.WorkerForPeer(*pin);
                if (!worker || worker->status != pairing::WorkerStatus::Paired ||
                    worker->revoked)
                    return;
                session->worker_id = worker->id;
                session->peer_fingerprint = *pin;
                session->status = DisconnectedStatus();
                session->status.connection = wire::ConnectionState::Connected;
                session->active.store(true, std::memory_order_release);
                std::lock_guard<std::mutex> sessions_lock(sessions_mutex);
                const auto current = worker_sessions.find(worker->id);
                if (current != worker_sessions.end() &&
                    current->second->active.load(std::memory_order_acquire)) {
                    session->active.store(false, std::memory_order_release);
                    return;
                }
                worker_sessions[worker->id] = session;
            }

            std::vector<std::uint8_t> frame;
            std::string receive_error;
            bool first = true;
            while (!stopping.load(std::memory_order_acquire) &&
                   !session->stopping.load(std::memory_order_acquire)) {
                if (!channel->ReceiveFrame(frame, &receive_error)) break;
                wire::Message message;
                if (wire::Decode(std::string(frame.begin(), frame.end()), UnixNowMs(),
                                 message) != wire::Error::None) {
                    SetSessionError(session, "worker sent a malformed control-plane message");
                    break;
                }
                if (auto* status = std::get_if<wire::Status>(&message.payload)) {
                    first = false;
                    if (!ProcessStatus(*channel, session, *status)) break;
                    continue;
                }
                if (first) {
                    SetSessionError(session, "worker did not begin with status");
                    break;
                }
#ifdef SS_TOOL_SFM
                bool waiting_feature_decision = false;
#endif
#if defined(SS_TOOL_SFM) || defined(SS_TOOL_TRAIN)
                bool waiting_portable_decision = false;
                {
                    std::lock_guard<std::mutex> lock(session->mutex);
#ifdef SS_TOOL_SFM
                    waiting_feature_decision =
                        session->waiting_feature_step.has_value();
#endif
                    waiting_portable_decision =
                        session->waiting_portable_step.has_value();
                }
                if (
#ifdef SS_TOOL_SFM
                    (waiting_feature_decision &&
                     !std::get_if<wire::FeatureDecision>(&message.payload)) ||
#endif
                    (waiting_portable_decision &&
                     !std::get_if<wire::PortableDecision>(&message.payload))) {
                    SetSessionError(session,
                        "worker did not complete the outstanding job decision");
                    break;
                }
#ifdef SS_TOOL_SFM
                if (const auto* decision =
                        std::get_if<wire::FeatureDecision>(&message.payload)) {
                    if (!ProcessFeatureDecision(*channel, session, *decision)) break;
                    continue;
                }
                if (const auto* result =
                        std::get_if<wire::FeatureResult>(&message.payload)) {
                    if (!ProcessFeatureResult(*channel, session, *result)) break;
                    continue;
                }
#endif
                if (const auto* decision =
                        std::get_if<wire::PortableDecision>(&message.payload)) {
                    if (decision->workload == wire::PortableWorkload::Training) {
#ifdef SS_TOOL_TRAIN
                        if (!ProcessTrainingDecision(*channel, session, *decision))
                            break;
#else
                        break;
#endif
                    } else {
#ifdef SS_TOOL_SFM
                        if (!ProcessPortableDecision(*channel, session, *decision))
                            break;
#else
                        break;
#endif
                    }
                    continue;
                }
                if (const auto* result =
                        std::get_if<wire::PortableResult>(&message.payload)) {
                    if (result->workload == wire::PortableWorkload::Training) {
#ifdef SS_TOOL_TRAIN
                        if (!ProcessTrainingResult(*channel, session, *result))
                            break;
#else
                        break;
#endif
                    } else {
#ifdef SS_TOOL_SFM
                        if (!ProcessPortableResult(*channel, session, *result))
                            break;
#else
                        break;
#endif
                    }
                    continue;
                }
#endif
                if (const auto* acknowledgment =
                        std::get_if<wire::Acknowledgment>(&message.payload)) {
                    if (!ProcessAcknowledgment(session, *acknowledgment)) break;
                    continue;
                }
                SetSessionError(session, "worker sent a leader-only message");
                break;
            }
        } catch (...) {
            SetSessionError(session, "operational session ended unexpectedly");
        }
    }

    void OperationalLoop() noexcept {
        while (!stopping.load(std::memory_order_acquire)) {
            ReapFinished();
            RawSocket accepted = AcceptReady(operational_listener,
                                              operational_listener_mutex, stopping);
            if (accepted == kInvalidSocket) continue;
            if (stopping.load(std::memory_order_acquire)) {
                CloseSocket(accepted);
                break;
            }
            const RawSocket cancel_socket = DuplicateSocket(accepted);
            if (cancel_socket == kInvalidSocket) {
                CloseSocket(accepted);
                continue;
            }
            auto session = std::make_shared<Session>();
            session->socket = ToNative(accepted);
            session->cancel_socket = ToNative(cancel_socket);
            bool capacity = false;
            {
                std::lock_guard<std::mutex> lock(sessions_mutex);
                capacity = sessions.size() < kMaxConnections;
                if (capacity) sessions.push_back(session);
            }
            if (!capacity) {
                CloseSocket(accepted);
                CloseSocket(cancel_socket);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            try {
                session->thread = std::thread([this, session] {
                    HandleOperationalSession(session);
                    session->done.store(true, std::memory_order_release);
                });
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lock(sessions_mutex);
                    sessions.erase(std::remove(sessions.begin(), sessions.end(), session),
                                   sessions.end());
                }
                CloseSocket(FromNative(session->socket));
                session->socket = TlsChannel::kInvalidSocket;
                std::lock_guard<std::mutex> lock(session->cancel_mutex);
                CloseSocket(FromNative(session->cancel_socket));
                session->cancel_socket = TlsChannel::kInvalidSocket;
            }
        }
        ReapFinished();
    }

    bool StartThreads(std::string* error) {
        try {
            enrollment_thread = std::thread([this] { EnrollmentLoop(); });
            operational_thread = std::thread([this] { OperationalLoop(); });
        } catch (...) {
            SetError(error, "could not start leader listener threads");
            Stop();
            return false;
        }
        running.store(true, std::memory_order_release);
        return true;
    }

    void Stop() noexcept {
        std::lock_guard<std::mutex> stop_lock(stop_mutex);
        if (stopped) return;
        stopping.store(true, std::memory_order_release);
        running.store(false, std::memory_order_release);
        CloseListener(enrollment_listener, enrollment_listener_mutex);
        CloseListener(operational_listener, operational_listener_mutex);
        {
            std::lock_guard<std::mutex> lock(enrollment_cancel_mutex);
            ShutdownSocket(enrollment_cancel_socket);
        }
#ifdef SS_TOOL_SFM
        StopFeatureStaging();
        StopReconstructionStaging();
#endif
#ifdef SS_TOOL_TRAIN
        StopTrainingStaging();
#endif
        if (enrollment_thread.joinable() && enrollment_thread.get_id() != std::this_thread::get_id())
            enrollment_thread.join();
        if (operational_thread.joinable() && operational_thread.get_id() != std::this_thread::get_id())
            operational_thread.join();
        std::vector<std::shared_ptr<Session>> active_sessions;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex);
            active_sessions = sessions;
        }
        for (const auto& session : active_sessions) session->Abort();
        for (const auto& session : active_sessions) {
            if (session->thread.joinable() && session->thread.get_id() != std::this_thread::get_id())
                session->thread.join();
        }
        {
            std::lock_guard<std::mutex> lock(sessions_mutex);
            sessions.clear();
            worker_sessions.clear();
        }
        stopped = true;
    }
};

LeaderServer::LeaderServer(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
LeaderServer::~LeaderServer() { Stop(); }

std::unique_ptr<LeaderServer> LeaderServer::Start(Options options,
                                                  std::string* error) {
    options.state_root = LogicalFilesystemPath(options.state_root);
    if (!options.state_root.is_absolute() || options.bind_address.empty() ||
        options.bind_address.size() > 64 || options.server_name.empty() ||
        !IsPrivateBindAddress(options.bind_address) ||
        (options.operational_port && options.operational_port == options.enrollment_port)) {
        SetError(error, "leader options require an absolute state root, private numeric bind address, distinct ports, and server name");
        return nullptr;
    }
    std::string pairing_error;
    auto pairing_leader = pairing::Leader::Open(options.state_root, options.server_name,
                                                 &pairing_error);
    if (!pairing_leader) {
        SetError(error, pairing_error);
        return nullptr;
    }
    std::string signer_public_key_pem;
    std::string signer_fingerprint;
    if (!pairing_leader->InitializeUpdateSigner(
            signer_public_key_pem, signer_fingerprint, error))
        return nullptr;
    auto impl = std::make_shared<Impl>(
        std::move(options), std::move(*pairing_leader),
        std::move(signer_fingerprint));
    if (!impl->socket_runtime.Start()) {
        SetError(error, "could not initialize TCP socket runtime");
        return nullptr;
    }
    if (!LoadSequence(impl->options.state_root, impl->command_sequence, error))
        return nullptr;
    if (!impl->InitializeCommandState(error)) return nullptr;
#ifdef SS_TOOL_SFM
    if (!impl->InitializeFeatureJobs(error) ||
        !impl->InitializeReconstructionJobs(error))
        return nullptr;
#endif
#ifdef SS_TOOL_TRAIN
    if (!impl->InitializeTrainingJobs(error))
        return nullptr;
#endif
    if (!CreateListener(impl->options.bind_address, impl->options.enrollment_port,
                        impl->enrollment_listener, impl->enrollment_port, error))
        return nullptr;
    if (!CreateListener(impl->options.bind_address, impl->options.operational_port,
                        impl->operational_listener, impl->operational_port, error)) {
        CloseSocket(impl->enrollment_listener);
        impl->enrollment_listener = kInvalidSocket;
        return nullptr;
    }
    if (!impl->enrollment_port || !impl->operational_port ||
        impl->enrollment_port == impl->operational_port) {
        CloseSocket(impl->enrollment_listener);
        CloseSocket(impl->operational_listener);
        impl->enrollment_listener = impl->operational_listener = kInvalidSocket;
        SetError(error, "leader listeners did not receive distinct usable ports");
        return nullptr;
    }
    auto server = std::unique_ptr<LeaderServer>(new LeaderServer(impl));
    if (!server->impl_->StartThreads(error)) return nullptr;
#ifdef SS_TOOL_SFM
    if (!server->impl_->StartFeatureStaging(impl, error) ||
        !server->impl_->StartReconstructionStaging(impl, error))
        return nullptr;
#endif
#ifdef SS_TOOL_TRAIN
    if (!server->impl_->StartTrainingStaging(impl, error))
        return nullptr;
#endif
    return server;
}

void LeaderServer::Stop() noexcept {
    if (impl_) impl_->Stop();
}
bool LeaderServer::Running() const noexcept {
    return impl_ && impl_->running.load(std::memory_order_acquire) &&
           !impl_->stopping.load(std::memory_order_acquire);
}
std::uint16_t LeaderServer::OperationalPort() const noexcept {
    return impl_ ? impl_->operational_port : 0;
}
std::uint16_t LeaderServer::EnrollmentPort() const noexcept {
    return impl_ ? impl_->enrollment_port : 0;
}

std::optional<pairing::Invitation> LeaderServer::IssueInvitation(
    std::chrono::seconds lifetime, std::string* error) {
    if (!impl_ || !Running()) {
        SetError(error, "leader server is not running");
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(impl_->pairing_mutex);
    return impl_->pairing_leader.IssueInvitation(lifetime, error);
}
bool LeaderServer::Approve(const std::string& worker_id, std::string* error) {
    if (!impl_ || !Running()) {
        SetError(error, "leader server is not running");
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->pairing_mutex);
    return impl_->pairing_leader.Approve(worker_id, error);
}
bool LeaderServer::Reject(const std::string& worker_id, std::string* error) {
    if (!impl_ || !Running()) {
        SetError(error, "leader server is not running");
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->pairing_mutex);
    return impl_->pairing_leader.Reject(worker_id, error);
}
bool LeaderServer::Revoke(const std::string& worker_id, std::string* error) {
    if (!impl_ || !Running()) {
        SetError(error, "leader server is not running");
        return false;
    }
    std::shared_ptr<Impl::Session> session;
    {
        std::lock_guard<std::mutex> pairing_lock(impl_->pairing_mutex);
        {
            std::lock_guard<std::mutex> sessions_lock(impl_->sessions_mutex);
            const auto it = impl_->worker_sessions.find(worker_id);
            if (it != impl_->worker_sessions.end()) session = it->second;
        }
        if (!impl_->pairing_leader.Revoke(worker_id, error)) return false;
        if (session) session->stopping.store(true, std::memory_order_release);
    }
    if (session) session->Abort();
    return true;
}
TlsChannel::PeerFingerprint LeaderServer::EnrollmentPeerFingerprint() const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> lock(impl_->pairing_mutex);
    return impl_->pairing_leader.EnrollmentPeerFingerprint();
}
std::string LeaderServer::UpdateSignerFingerprint() const {
    return impl_ ? impl_->update_signer_fingerprint : std::string{};
}

std::vector<pairing::WorkerInfo> LeaderServer::Workers() const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> lock(impl_->pairing_mutex);
    return impl_->pairing_leader.Workers();
}
std::vector<LeaderServer::WorkerSnapshot> LeaderServer::Snapshot() const {
    if (!impl_) return {};
    std::vector<pairing::WorkerInfo> workers;
    std::map<std::string, std::shared_ptr<Impl::Session>> sessions;
    {
        std::lock_guard<std::mutex> pair_lock(impl_->pairing_mutex);
        workers = impl_->pairing_leader.Workers();
        std::lock_guard<std::mutex> sessions_lock(impl_->sessions_mutex);
        sessions = impl_->worker_sessions;
    }
    std::vector<WorkerSnapshot> snapshots;
    snapshots.reserve(workers.size());
    for (const auto& worker : workers) {
        WorkerSnapshot snapshot;
        snapshot.id = worker.id;
        snapshot.label = worker.label;
        snapshot.pairing_status = worker.status;
        snapshot.revoked = worker.revoked;
        snapshot.status = DisconnectedStatus();
        const auto it = sessions.find(worker.id);
        if (it != sessions.end()) {
            const auto& session = it->second;
            bool has_status = false;
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                snapshot.connected = session->active.load(std::memory_order_acquire) &&
                    !session->stopping.load(std::memory_order_acquire);
                snapshot.status = session->status;
                if (!snapshot.connected)
                    snapshot.status.connection = wire::ConnectionState::Disconnected;
                has_status = session->has_status;
                snapshot.last_seen_unix_ms = session->last_seen_unix_ms;
                snapshot.error = session->error;
            }
            snapshot.ready = snapshot.connected && has_status &&
                snapshot.status.compatibility == wire::CompatibilityState::Compatible &&
                snapshot.status.scheduling == wire::SchedulingState::Accepting &&
                snapshot.status.activity == wire::ActivityState::Idle &&
                snapshot.status.online && !snapshot.status.maintenance &&
                !snapshot.status.capabilities.empty() && !snapshot.status.gpu.empty() &&
                snapshot.status.health == wire::HealthState::Healthy &&
                snapshot.error.empty();
#if defined(SS_TOOL_SFM) || defined(SS_TOOL_TRAIN)
            bool supports_workload = false;
#ifdef SS_TOOL_SFM
            supports_workload =
                impl_->SupportsFeatureBuild(snapshot.status, LeaderStatus().build) ||
                impl_->SupportsReconstructionBuild(
                    snapshot.status, ReconstructionBuildIdentity());
#endif
#ifdef SS_TOOL_TRAIN
            supports_workload = supports_workload ||
                impl_->SupportsTrainingBuild(snapshot.status, LeaderStatus().build);
#endif
            snapshot.ready = snapshot.ready && supports_workload;
#ifdef SS_TOOL_SFM
            snapshot.ready = snapshot.ready &&
                !impl_->WorkerHasActiveFeature(worker.id) &&
                !impl_->WorkerHasActiveReconstruction(worker.id);
#endif
#ifdef SS_TOOL_TRAIN
            snapshot.ready = snapshot.ready &&
                !impl_->WorkerHasActiveTraining(worker.id);
#endif
#endif
        }
        snapshots.push_back(std::move(snapshot));
    }
    return snapshots;
}

std::vector<LeaderServer::CommandResult> LeaderServer::CommandSnapshots() const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> lock(impl_->command_mutex);
    std::string ignored_error;
    impl_->RefreshExpiredCommandsLocked(UnixNowMs(), &ignored_error);
    std::vector<CommandResult> snapshots;
    snapshots.reserve(impl_->command_history.size());
    for (const auto& item : impl_->command_history)
        snapshots.push_back(item->result);
    return snapshots;
}

std::optional<LeaderServer::CommandResult> LeaderServer::RequestReboot(
    const std::string& worker_id, bool confirmed, std::string* error) {
    if (!impl_ || !Running()) {
        SetError(error, "leader server is not running");
        return std::nullopt;
    }
    if (!confirmed) {
        SetError(error, "remote admin request requires explicit confirmation");
        return std::nullopt;
    }
    const auto session =
        impl_->CurrentWorkerSession(worker_id, nullptr, error);
    if (!session) return std::nullopt;
    return impl_->QueueAdminCommand(worker_id, session,
        admin::Operation::Reboot, std::nullopt, 0, confirmed, error);
}

std::optional<LeaderServer::CommandResult> LeaderServer::RequestUpdate(
    const std::string& worker_id, const std::filesystem::path& package_file,
    update::PackageManifest manifest, std::uint64_t security_version,
    bool confirmed, std::string* error) {
    if (!impl_ || !Running()) {
        SetError(error, "leader server is not running");
        return std::nullopt;
    }
    if (!confirmed) {
        SetError(error, "remote admin request requires explicit confirmation");
        return std::nullopt;
    }
    if (!security_version ||
        (manifest.architecture != "x86_64" &&
         manifest.architecture != "aarch64")) {
        SetError(error, "update security version or package architecture is invalid");
        return std::nullopt;
    }
    wire::Status status;
    const auto session =
        impl_->CurrentWorkerSession(worker_id, &status, error);
    if (!session) return std::nullopt;
    if (!AdvertisedPlatformMatches(status.platform, manifest.os)) {
        SetError(error, "update package OS does not match worker status");
        return std::nullopt;
    }
    const auto staged_root = StageLeaderPackage(
        package_file, impl_->options.state_root, manifest, error);
    if (!staged_root) return std::nullopt;
    return impl_->QueueAdminCommand(worker_id, session,
        admin::Operation::Activate, std::move(manifest),
        security_version, confirmed, error);
}


std::optional<LeaderServer::CommandResult> LeaderServer::SendCommand(
    const std::string& worker_id, wire::CommandAction action,
    std::chrono::milliseconds ttl, std::string* error) {
    if (IsAdminCommand(action)) {
        SetError(error, "admin actions require a signed admin request");
        return std::nullopt;
    }
    if (RequiresExplicitCommandConfirmation(action)) {
        SetError(error, "command requires explicit confirmation");
        return std::nullopt;
    }
    return SendCommand(worker_id, action, std::string{}, false, ttl, error);
}
std::optional<LeaderServer::CommandResult> LeaderServer::SendCommand(
    const std::string& worker_id, wire::CommandAction action,
    const std::string& target_job_id, bool confirmed,
    std::chrono::milliseconds ttl, std::string* error) {
    if (!impl_ || !Running()) {
        SetError(error, "leader server is not running");
        return std::nullopt;
    }
    if (IsAdminCommand(action)) {
        SetError(error, "admin actions require a signed admin request");
        return std::nullopt;
    }
    if (!wire::IsValid(action) ||
        RequiresExplicitCommandConfirmation(action) != confirmed ||
        (!target_job_id.empty() && action != wire::CommandAction::Stop) ||
        ttl.count() < 1 || ttl > kMaximumCommandTtl) {
        SetError(error, "command action, confirmation, target, or TTL is invalid");
        return std::nullopt;
    }
    std::shared_ptr<Impl::Session> session;
    {
        std::lock_guard<std::mutex> lock(impl_->sessions_mutex);
        const auto it = impl_->worker_sessions.find(worker_id);
        if (it != impl_->worker_sessions.end()) session = it->second;
    }
    if (!session || !session->active.load(std::memory_order_acquire)) {
        SetError(error, "worker has no active operational session");
        return std::nullopt;
    }
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!session->has_status ||
            session->stopping.load(std::memory_order_acquire)) {
            SetError(error, "worker has not sent an operational status");
            return std::nullopt;
        }
    }
    const std::uint64_t issued = UnixNowMs();
    const auto ttl_ms = static_cast<std::uint64_t>(ttl.count());
    if (!issued || issued > kMaxSafeInteger ||
        ttl_ms > kMaxSafeInteger - issued) {
        SetError(error, "system clock cannot represent command expiration");
        return std::nullopt;
    }
    const auto deadline = Clock::now() + ttl;

    auto pending = std::make_shared<Impl::PendingCommand>();
    std::unique_lock<std::mutex> lock(impl_->command_mutex);
    if (!impl_->RefreshExpiredCommandsLocked(UnixNowMs(), error))
        return std::nullopt;
    if (impl_->active_commands.count(worker_id)) {
        SetError(error, "worker already has a live command");
        return std::nullopt;
    }
    if (impl_->command_sequence >= kMaxSafeInteger) {
        SetError(error, "leader command sequence is exhausted");
        return std::nullopt;
    }
    const std::uint64_t sequence = impl_->command_sequence + 1;
    pending->command.command_id = "cmd-" + std::to_string(sequence);
    pending->command.leader_id = impl_->pairing_leader.LeaderId();
    pending->command.leader_epoch = impl_->pairing_leader.LeaderEpoch();
    pending->command.sequence = sequence;
    pending->command.action = action;
    pending->command.target_job_id = target_job_id;
    pending->command.confirmed = confirmed;
    pending->command.issued_at_ms = issued;
    pending->command.expires_at_ms = issued + ttl_ms;
    pending->result.worker_id = worker_id;
    pending->result.command_id = pending->command.command_id;
    pending->result.sequence = sequence;
    pending->result.action = action;
    pending->result.target_job_id = target_job_id;
    pending->result.confirmed = confirmed;
    pending->result.issued_at_ms = issued;
    pending->result.expires_at_ms = pending->command.expires_at_ms;
    pending->result.state = CommandState::Pending;

    wire::Message validation;
    validation.payload = pending->command;
    if (wire::Validate(validation, issued) != wire::Error::None) {
        SetError(error, "command target or identity is invalid");
        return std::nullopt;
    }
    if (impl_->command_history.size() >= kMaxCommandHistory &&
        std::none_of(impl_->command_history.begin(),
                     impl_->command_history.end(),
                     [](const auto& item) { return item->terminal; })) {
        SetError(error, "durable command history is full");
        return std::nullopt;
    }
    if (!PersistSequence(impl_->options.state_root, sequence, error))
        return std::nullopt;
    impl_->command_sequence = sequence;
    const auto previous_history = impl_->command_history;
    impl_->command_history.push_back(pending);
    if (!impl_->PruneCommandHistoryLocked()) {
        impl_->command_history = previous_history;
        SetError(error, "durable command history is full");
        return std::nullopt;
    }
    if (!impl_->PersistCommandStateLocked(error)) {
        impl_->command_history = previous_history;
        return std::nullopt;
    }
    impl_->active_commands.emplace(worker_id, pending);

    while (!pending->waiter_notified && !pending->accepted &&
           !pending->terminal) {
        if (impl_->condition.wait_until(lock, deadline) == std::cv_status::timeout &&
            !pending->waiter_notified && !pending->accepted &&
            !pending->terminal) {
            const CommandResult previous = pending->result;
            const bool previous_terminal = pending->terminal;
            pending->result.state = pending->ever_sent
                ? CommandState::Uncertain : CommandState::Expired;
            pending->terminal = !pending->ever_sent;
            pending->waiter_notified = true;
            if (pending->terminal)
                impl_->active_commands.erase(worker_id);
            if (!impl_->PersistCommandStateLocked(error)) {
                pending->result = previous;
                pending->terminal = previous_terminal;
                pending->waiter_notified = false;
                if (!previous_terminal)
                    impl_->active_commands[worker_id] = pending;
                return std::nullopt;
            }
        }
    }
    return pending->result;
}

bool LeaderServer::SubmitFeatureShard(
    const std::string& worker_id, const std::string& job_id,
    const std::filesystem::path& plan_path,
    const std::filesystem::path& request_path,
    const std::filesystem::path& image_root,
    const std::filesystem::path& mask_root,
    std::uint64_t disk_budget_bytes, std::string* error) {
#ifndef SS_TOOL_SFM
    (void)worker_id;
    (void)job_id;
    (void)plan_path;
    (void)request_path;
    (void)image_root;
    (void)mask_root;
    (void)disk_budget_bytes;
    SetError(error, "feature sharding is unavailable in this build");
    return false;
#else
    if (!impl_ || !Running()) {
        SetError(error, "leader server is not running");
        return false;
    }
    if (!ValidFeatureId(worker_id) || !ValidFeatureId(job_id) ||
        !disk_budget_bytes || disk_budget_bytes > kMaxFeatureDiskBudgetBytes) {
        SetError(error, "feature shard identity or disk budget is invalid");
        return false;
    }
    const std::string required_build = LeaderStatus().build;
    if (required_build.empty() || required_build == "unknown" ||
        required_build.size() > wire::kMaxBuildBytes) {
        SetError(error, "leader build identity is unavailable");
        return false;
    }
    if (!impl_->IsWorkerPaired(worker_id)) {
        SetError(error, "feature worker is not paired");
        return false;
    }
    std::shared_ptr<Impl::Session> session;
    {
        std::lock_guard<std::mutex> lock(impl_->sessions_mutex);
        const auto found = impl_->worker_sessions.find(worker_id);
        if (found != impl_->worker_sessions.end()) session = found->second;
    }
    if (session && session->active.load(std::memory_order_acquire) &&
        !session->stopping.load(std::memory_order_acquire)) {
        wire::Status status;
        if (impl_->FeatureSessionStatus(session, status)) {
            if (!impl_->SupportsFeatureBuild(status, required_build)) {
                SetError(error, "connected worker lacks the required build or feature capability");
                return false;
            }
            if (status.scheduling != wire::SchedulingState::Accepting ||
                status.activity != wire::ActivityState::Idle || !status.online ||
                status.maintenance || status.health != wire::HealthState::Healthy) {
                SetError(error, "connected worker is not ready for a feature shard");
                return false;
            }
        }
    }
    sfm::feature_work::FeatureRequest request;
    try {
        request = sfm::feature_work::readRequestFile(request_path.u8string());
    } catch (const std::exception& exception) {
        SetError(error, std::string("could not read feature request: ") + exception.what());
        return false;
    }
    const std::string attempt_id = request.attempt_id;
    if (!ValidFeatureId(attempt_id)) {
        SetError(error, "feature request attempt identity is invalid");
        return false;
    }
    const std::string attempt_key = FeatureAttemptKey(job_id, attempt_id);
    {
        std::lock_guard<std::mutex> lock(impl_->feature_staging_mutex);
        if (impl_->feature_staging_stopping ||
            impl_->feature_staging_queue.size() +
                static_cast<std::size_t>(impl_->feature_staging_active) +
                impl_->feature_staging_reserved >= kMaxFeatureStagingTasks) {
            SetError(error, "feature input staging queue is full or stopping");
            return false;
        }
        ++impl_->feature_staging_reserved;
    }
    struct StagingCapacity final {
        Impl& impl;
        bool queued = false;
        ~StagingCapacity() {
            if (queued) return;
            std::lock_guard<std::mutex> lock(impl.feature_staging_mutex);
            if (impl.feature_staging_reserved) --impl.feature_staging_reserved;
        }
    } capacity{*impl_};
    {
        std::lock_guard<std::mutex> assignment_lock(impl_->assignment_mutex);
        {
            std::lock_guard<std::mutex> reconstruction_lock(
                impl_->reconstruction_mutex);
            if (impl_->CurrentReconstructionForWorkerLocked(worker_id) ||
                impl_->reserved_reconstruction_workers.count(worker_id)) {
                SetError(error, "worker already has an active reconstruction attempt");
                return false;
            }
        }
#ifdef SS_TOOL_TRAIN
        {
            std::lock_guard<std::mutex> training_lock(impl_->training_mutex);
            if (impl_->CurrentTrainingForWorkerLocked(worker_id) ||
                impl_->reserved_training_workers.count(worker_id)) {
                SetError(error, "worker already has an active training attempt");
                return false;
            }
        }
#endif
        std::lock_guard<std::mutex> lock(impl_->feature_mutex);
        if (impl_->feature_attempts.size() +
                impl_->reserved_feature_attempts.size() >= kMaxFeatureAttempts ||
            impl_->current_feature_attempts.count(job_id) ||
            impl_->reserved_feature_jobs.count(job_id) ||
            impl_->CurrentFeatureForWorkerLocked(worker_id) ||
            impl_->reserved_feature_workers.count(worker_id) ||
            impl_->feature_attempts.count(attempt_key) ||
            impl_->reserved_feature_attempts.count(attempt_key)) {
            SetError(error, "feature job, worker, or attempt is already in use");
            return false;
        }
        impl_->reserved_feature_jobs.insert(job_id);
        impl_->reserved_feature_workers.insert(worker_id);
        impl_->reserved_feature_attempts.insert(attempt_key);
    }
    struct Reservation final {
        Impl& impl;
        const std::string& job;
        const std::string& worker;
        const std::string& attempt_key;
        ~Reservation() {
            std::lock_guard<std::mutex> lock(impl.feature_mutex);
            impl.reserved_feature_jobs.erase(job);
            impl.reserved_feature_workers.erase(worker);
            impl.reserved_feature_attempts.erase(attempt_key);
        }
    } reservation{*impl_, job_id, worker_id, attempt_key};
    const auto attempt_root = impl_->feature_store_directory /
        std::filesystem::u8path(job_id) / std::filesystem::u8path(attempt_id);
    FeatureStageTask task;
    task.job_id = job_id;
    task.attempt_id = attempt_id;
    task.worker_id = worker_id;
    task.plan_path = plan_path;
    task.request_path = request_path;
    task.image_root = image_root;
    task.mask_root = mask_root;
    task.required_build = required_build;
    task.disk_budget_bytes = disk_budget_bytes;
    {
        std::lock_guard<std::mutex> pairing_lock(impl_->pairing_mutex);
        bool paired = false;
        for (const auto& worker : impl_->pairing_leader.Workers())
            if (worker.id == worker_id &&
                worker.status == pairing::WorkerStatus::Paired && !worker.revoked) {
                paired = true;
                break;
            }
        if (!paired) {
            SetError(error, "feature worker was revoked before submission");
            return false;
        }
        std::lock_guard<std::mutex> feature_lock(impl_->feature_mutex);
        if (impl_->current_feature_attempts.count(job_id) ||
            impl_->feature_attempts.count(attempt_key)) {
            SetError(error, "feature job or attempt became occupied before submission");
            return false;
        }
        FeatureAttempt attempt;
        attempt.snapshot.job_id = job_id;
        attempt.snapshot.attempt_id = attempt_id;
        attempt.snapshot.worker_id = worker_id;
        attempt.snapshot.result_root = attempt_root / "output";
        attempt.snapshot.state = FeatureJobState::Staging;
        attempt.attempt_root = attempt_root;
        attempt.required_build = required_build;
        attempt.disk_budget_bytes = disk_budget_bytes;
        attempt.current = true;
        if (!impl_->feature_attempts.emplace(attempt_key, std::move(attempt)).second) {
            SetError(error, "feature attempt identity is already stored");
            return false;
        }
        impl_->current_feature_attempts.emplace(job_id, attempt_key);
        if (!impl_->PersistFeatureJobsLocked(error)) {
            impl_->current_feature_attempts.erase(job_id);
            impl_->feature_attempts.erase(attempt_key);
            return false;
        }
    }
    bool queued = false;
    std::string queue_failure = "leader stopped before input staging";
    try {
        std::lock_guard<std::mutex> lock(impl_->feature_staging_mutex);
        if (!impl_->feature_staging_stopping &&
            !impl_->stopping.load(std::memory_order_acquire)) {
            impl_->feature_staging_queue.push_back(task);
            --impl_->feature_staging_reserved;
            capacity.queued = true;
            queued = true;
        }
    } catch (...) {
        queue_failure = "could not queue feature input staging";
    }
    if (queued) {
        impl_->feature_staging_condition.notify_one();
        return true;
    }
    impl_->FinishFeatureStaging(task, nullptr, std::move(queue_failure), false);
    return true;
#endif
}

bool LeaderServer::SupersedeFeatureShard(const std::string& job_id,
                                         std::string* error) {
#ifndef SS_TOOL_SFM
    (void)job_id;
    SetError(error, "feature sharding is unavailable in this build");
    return false;
#else
    if (!impl_ || !Running()) {
        SetError(error, "leader server is not running");
        return false;
    }
    if (!ValidFeatureId(job_id)) {
        SetError(error, "feature job identity is invalid");
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->feature_mutex);
    const auto current = impl_->current_feature_attempts.find(job_id);
    if (current == impl_->current_feature_attempts.end()) {
        SetError(error, "feature job has no current attempt");
        return false;
    }
    auto found = impl_->feature_attempts.find(current->second);
    if (found == impl_->feature_attempts.end()) {
        SetError(error, "current feature attempt is missing");
        return false;
    }
    const FeatureAttempt previous = found->second;
    const std::string attempt_key = current->second;
    found->second.current = false;
    found->second.snapshot.state = FeatureJobState::Superseded;
    found->second.snapshot.error = "attempt explicitly superseded";
    impl_->current_feature_attempts.erase(current);
    if (!impl_->PersistFeatureJobsLocked(error)) {
        found->second = previous;
        impl_->current_feature_attempts.emplace(job_id, attempt_key);
        return false;
    }
    return true;
#endif
}

std::vector<LeaderServer::FeatureJobSnapshot> LeaderServer::FeatureJobs() const {
#ifndef SS_TOOL_SFM
    return {};
#else
    std::vector<FeatureJobSnapshot> jobs;
    if (!impl_) return jobs;
    std::lock_guard<std::mutex> lock(impl_->feature_mutex);
    jobs.reserve(impl_->feature_attempts.size());
    for (const auto& [attempt_id, attempt] : impl_->feature_attempts)
        jobs.push_back(attempt.snapshot);
    return jobs;
#endif
}

bool LeaderServer::SubmitReconstruction(
    const std::string& worker_id, const std::string& job_id,
    sfm::AutoRequest request, const std::filesystem::path& source_manifest,
    std::uint64_t disk_budget_bytes, std::string* error) {
#ifndef SS_TOOL_SFM
    (void)worker_id;
    (void)job_id;
    (void)request;
    (void)source_manifest;
    (void)disk_budget_bytes;
    SetError(error, "whole reconstruction is unavailable in this build");
    return false;
#else
    if (!impl_ || !Running()) {
        SetError(error, "leader server is not running");
        return false;
    }
    if (!ValidFeatureId(worker_id) || !ValidFeatureId(job_id) ||
        !disk_budget_bytes ||
        disk_budget_bytes > kMaxReconstructionDiskBudgetBytes) {
        SetError(error, "reconstruction identity or disk budget is invalid");
        return false;
    }
    if (!request.in.feature_plan.empty()) {
        SetError(error, "feature-plan jobs are not portable whole reconstructions");
        return false;
    }
    const std::string required_build = ReconstructionBuildIdentity();
    if (required_build.empty() || required_build == "unknown" ||
        required_build.size() > wire::kMaxBuildBytes ||
        LeaderStatus().build != required_build) {
        SetError(error, "leader reconstruction build identity is unavailable");
        return false;
    }
    std::string paired_leader_id;
    std::uint64_t paired_leader_epoch = 0;
    bool paired = false;
    {
        std::lock_guard<std::mutex> lock(impl_->pairing_mutex);
        for (const auto& worker : impl_->pairing_leader.Workers())
            if (worker.id == worker_id &&
                worker.status == pairing::WorkerStatus::Paired &&
                !worker.revoked) {
                paired = true;
                break;
            }
        if (paired) {
            paired_leader_id = impl_->pairing_leader.LeaderId();
            paired_leader_epoch = impl_->pairing_leader.LeaderEpoch();
        }
    }
    if (!paired || !ValidFeatureId(paired_leader_id) || !paired_leader_epoch) {
        SetError(error, "reconstruction worker is not paired to a valid leader identity");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->reconstruction_staging_mutex);
        if (impl_->reconstruction_staging_stopping ||
            impl_->reconstruction_staging_queue.size() +
                static_cast<std::size_t>(impl_->reconstruction_staging_active) +
                impl_->reconstruction_staging_reserved >=
                    kMaxReconstructionStagingTasks) {
            SetError(error, "reconstruction input staging queue is full or stopping");
            return false;
        }
        ++impl_->reconstruction_staging_reserved;
    }
    struct StagingCapacity final {
        Impl& impl;
        bool queued = false;
        ~StagingCapacity() {
            if (queued) return;
            std::lock_guard<std::mutex> lock(impl.reconstruction_staging_mutex);
            if (impl.reconstruction_staging_reserved)
                --impl.reconstruction_staging_reserved;
        }
    } capacity{*impl_};

    static std::atomic<std::uint64_t> serial{0};
    const std::string attempt_id =
        "rec-" + std::to_string(UnixNowMs()) + "-" +
        std::to_string(Clock::now().time_since_epoch().count()) + "-" +
        std::to_string(serial.fetch_add(1, std::memory_order_relaxed));
    const std::string attempt_key = FeatureAttemptKey(job_id, attempt_id);
    ReconstructionStageTask task;
    task.job_id = job_id;
    task.attempt_id = attempt_id;
    task.worker_id = worker_id;
    task.request = std::move(request);
    task.source_manifest = source_manifest;
    task.required_build = required_build;
    task.disk_budget_bytes = disk_budget_bytes;

    std::unique_lock<std::mutex> assignment_lock(impl_->assignment_mutex);
    {
        std::lock_guard<std::mutex> feature_lock(impl_->feature_mutex);
        if (impl_->CurrentFeatureForWorkerLocked(worker_id) ||
            impl_->reserved_feature_workers.count(worker_id)) {
            SetError(error, "worker already has an active feature attempt");
            return false;
        }
    }
#ifdef SS_TOOL_TRAIN
    {
        std::lock_guard<std::mutex> training_lock(impl_->training_mutex);
        if (impl_->CurrentTrainingForWorkerLocked(worker_id) ||
            impl_->reserved_training_workers.count(worker_id)) {
            SetError(error, "worker already has an active training attempt");
            return false;
        }
    }
#endif
    {
        std::lock_guard<std::mutex> pairing_lock(impl_->pairing_mutex);
        bool worker_paired = false;
        for (const auto& worker : impl_->pairing_leader.Workers())
            if (worker.id == worker_id &&
                worker.status == pairing::WorkerStatus::Paired &&
                !worker.revoked) {
                worker_paired = true;
                break;
            }
        if (!worker_paired ||
            impl_->pairing_leader.LeaderId() != paired_leader_id ||
            impl_->pairing_leader.LeaderEpoch() != paired_leader_epoch) {
            SetError(error, "reconstruction worker pairing changed before submission");
            return false;
        }
        std::lock_guard<std::mutex> lock(impl_->reconstruction_mutex);
        if (impl_->reconstruction_attempts.size() +
                impl_->reserved_reconstruction_attempts.size() >=
                    kMaxReconstructionAttempts ||
            impl_->current_reconstruction_attempts.count(job_id) ||
            impl_->reserved_reconstruction_jobs.count(job_id) ||
            impl_->CurrentReconstructionForWorkerLocked(worker_id) ||
            impl_->reserved_reconstruction_workers.count(worker_id) ||
            impl_->reconstruction_attempts.count(attempt_key) ||
            impl_->reserved_reconstruction_attempts.count(attempt_key)) {
            SetError(error, "reconstruction job, worker, or attempt is already in use");
            return false;
        }
        impl_->reserved_reconstruction_jobs.insert(job_id);
        impl_->reserved_reconstruction_workers.insert(worker_id);
        impl_->reserved_reconstruction_attempts.insert(attempt_key);
        ReconstructionAttempt attempt;
        attempt.snapshot.job_id = job_id;
        attempt.snapshot.attempt_id = attempt_id;
        attempt.snapshot.worker_id = worker_id;
        attempt.snapshot.paired_leader_id = paired_leader_id;
        attempt.snapshot.paired_leader_epoch = paired_leader_epoch;
        attempt.snapshot.required_build = required_build;
        attempt.snapshot.result_root = impl_->reconstruction_store_directory /
            std::filesystem::u8path(job_id) /
            std::filesystem::u8path(attempt_id) / "result";
        attempt.snapshot.state = ReconstructionJobState::Staging;
        attempt.snapshot.current = true;
        attempt.attempt_root = attempt.snapshot.result_root.parent_path();
        attempt.disk_budget_bytes = disk_budget_bytes;
        attempt.current = true;
        if (!impl_->reconstruction_attempts.emplace(
                attempt_key, std::move(attempt)).second) {
            SetError(error, "reconstruction attempt identity is already stored");
            impl_->reserved_reconstruction_jobs.erase(job_id);
            impl_->reserved_reconstruction_workers.erase(worker_id);
            impl_->reserved_reconstruction_attempts.erase(attempt_key);
            return false;
        }
        impl_->current_reconstruction_attempts.emplace(job_id, attempt_key);
        if (!impl_->PersistReconstructionJobsLocked(error)) {
            impl_->current_reconstruction_attempts.erase(job_id);
            impl_->reconstruction_attempts.erase(attempt_key);
            impl_->reserved_reconstruction_jobs.erase(job_id);
            impl_->reserved_reconstruction_workers.erase(worker_id);
            impl_->reserved_reconstruction_attempts.erase(attempt_key);
            return false;
        }
    }
    struct Reservation final {
        Impl& impl;
        const std::string& job;
        const std::string& worker;
        const std::string& attempt;
        ~Reservation() {
            std::lock_guard<std::mutex> lock(impl.reconstruction_mutex);
            impl.reserved_reconstruction_jobs.erase(job);
            impl.reserved_reconstruction_workers.erase(worker);
            impl.reserved_reconstruction_attempts.erase(attempt);
        }
    } reservation{*impl_, job_id, worker_id, attempt_key};

    bool queued = false;
    std::string queue_failure = "leader stopped before reconstruction staging";
    try {
        std::lock_guard<std::mutex> lock(impl_->reconstruction_staging_mutex);
        if (!impl_->reconstruction_staging_stopping &&
            !impl_->stopping.load(std::memory_order_acquire)) {
            impl_->reconstruction_staging_queue.push_back(task);
            --impl_->reconstruction_staging_reserved;
            capacity.queued = true;
            queued = true;
        }
    } catch (...) {
        queue_failure = "could not queue reconstruction input staging";
    }
    if (queued) {
        impl_->reconstruction_staging_condition.notify_one();
        return true;
    }
    impl_->FinishReconstructionStaging(task, nullptr,
        std::move(queue_failure), false);
    return true;
#endif
}

bool LeaderServer::SupersedeReconstruction(const std::string& job_id,
                                           std::string* error) {
#ifndef SS_TOOL_SFM
    (void)job_id;
    SetError(error, "whole reconstruction is unavailable in this build");
    return false;
#else
    if (!impl_ || !Running()) {
        SetError(error, "leader server is not running");
        return false;
    }
    if (!ValidFeatureId(job_id)) {
        SetError(error, "reconstruction job identity is invalid");
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->reconstruction_mutex);
    const auto current = impl_->current_reconstruction_attempts.find(job_id);
    if (current == impl_->current_reconstruction_attempts.end()) {
        SetError(error, "reconstruction job has no current attempt");
        return false;
    }
    const auto found = impl_->reconstruction_attempts.find(current->second);
    if (found == impl_->reconstruction_attempts.end()) {
        SetError(error, "current reconstruction attempt is missing");
        return false;
    }
    const ReconstructionAttempt previous = found->second;
    const std::string attempt_key = current->second;
    found->second.current = false;
    found->second.snapshot.current = false;
    found->second.snapshot.state = ReconstructionJobState::Superseded;
    found->second.snapshot.error = "attempt explicitly superseded";
    impl_->current_reconstruction_attempts.erase(current);
    if (!impl_->PersistReconstructionJobsLocked(error)) {
        found->second = previous;
        impl_->current_reconstruction_attempts.emplace(job_id, attempt_key);
        return false;
    }
    return true;
#endif
}

std::vector<LeaderServer::ReconstructionJobSnapshot>
LeaderServer::ReconstructionJobs() const {
#ifndef SS_TOOL_SFM
    return {};
#else
    std::vector<ReconstructionJobSnapshot> jobs;
    if (!impl_) return jobs;
    std::lock_guard<std::mutex> lock(impl_->reconstruction_mutex);
    jobs.reserve(impl_->reconstruction_attempts.size());
    for (const auto& [attempt_key, attempt] : impl_->reconstruction_attempts)
        jobs.push_back(attempt.snapshot);
    return jobs;
#endif
}

bool LeaderServer::SubmitTraining(
    const std::string& worker_id, const std::string& job_id,
    TrainConfig config, const std::string& preset,
    const std::filesystem::path& resume_checkpoint,
    std::uint64_t disk_budget_bytes, std::string* error) {
#ifndef SS_TOOL_TRAIN
    (void)worker_id;
    (void)job_id;
    (void)config;
    (void)preset;
    (void)resume_checkpoint;
    (void)disk_budget_bytes;
    SetError(error, "portable training is unavailable in this build");
    return false;
#else
    if (!impl_ || !Running()) {
        SetError(error, "leader server is not running");
        return false;
    }
    if (!ValidTrainingId(worker_id) || !ValidTrainingId(job_id) ||
        preset.empty() || preset.size() > wire::kMaxPortableTextBytes ||
        !disk_budget_bytes || disk_budget_bytes > kMaxTrainingDiskBudgetBytes) {
        SetError(error, "training identity, preset, or disk budget is invalid");
        return false;
    }
    const std::string required_build = LeaderStatus().build;
    if (required_build.empty() || required_build == "unknown" ||
        required_build.size() > wire::kMaxBuildBytes) {
        SetError(error, "leader training build identity is unavailable");
        return false;
    }

    std::string paired_leader_id;
    std::uint64_t paired_leader_epoch = 0;
    bool paired = false;
    {
        std::lock_guard<std::mutex> lock(impl_->pairing_mutex);
        for (const auto& worker : impl_->pairing_leader.Workers())
            if (worker.id == worker_id &&
                worker.status == pairing::WorkerStatus::Paired &&
                !worker.revoked) {
                paired = true;
                break;
            }
        if (paired) {
            paired_leader_id = impl_->pairing_leader.LeaderId();
            paired_leader_epoch = impl_->pairing_leader.LeaderEpoch();
        }
    }
    if (!paired || !ValidTrainingId(paired_leader_id) ||
        !paired_leader_epoch) {
        SetError(error, "training worker is not paired to a valid leader identity");
        return false;
    }

    std::shared_ptr<Impl::Session> session;
    {
        std::lock_guard<std::mutex> lock(impl_->sessions_mutex);
        const auto found = impl_->worker_sessions.find(worker_id);
        if (found != impl_->worker_sessions.end()) session = found->second;
    }
    if (session && session->active.load(std::memory_order_acquire) &&
        !session->stopping.load(std::memory_order_acquire)) {
        wire::Status status;
        if (impl_->TrainingSessionStatus(session, status)) {
            if (!impl_->SupportsTrainingBuild(status, required_build)) {
                SetError(error,
                    "connected worker lacks the required build or training capability");
                return false;
            }
            if (status.scheduling != wire::SchedulingState::Accepting ||
                status.connection != wire::ConnectionState::Connected ||
                status.activity != wire::ActivityState::Idle || !status.online ||
                status.maintenance ||
                status.health != wire::HealthState::Healthy) {
                SetError(error, "connected worker is not ready for training");
                return false;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(impl_->training_staging_mutex);
        if (impl_->training_staging_stopping ||
            impl_->training_staging_queue.size() +
                static_cast<std::size_t>(impl_->training_staging_active) +
                impl_->training_staging_reserved >= kMaxTrainingStagingTasks) {
            SetError(error, "training input staging queue is full or stopping");
            return false;
        }
        ++impl_->training_staging_reserved;
    }
    struct StagingCapacity final {
        Impl& impl;
        bool queued = false;
        ~StagingCapacity() {
            if (queued) return;
            std::lock_guard<std::mutex> lock(impl.training_staging_mutex);
            if (impl.training_staging_reserved)
                --impl.training_staging_reserved;
        }
    } capacity{*impl_};

    static std::atomic<std::uint64_t> serial{0};
    const std::string attempt_id =
        "trn-" + std::to_string(UnixNowMs()) + "-" +
        std::to_string(Clock::now().time_since_epoch().count()) + "-" +
        std::to_string(serial.fetch_add(1, std::memory_order_relaxed));
    const std::string attempt_key = TrainingAttemptKey(job_id, attempt_id);
    TrainingStageTask task;
    task.job_id = job_id;
    task.attempt_id = attempt_id;
    task.worker_id = worker_id;
    task.config = std::move(config);
    task.preset = preset;
    task.resume_checkpoint = resume_checkpoint;
    task.required_build = required_build;
    task.disk_budget_bytes = disk_budget_bytes;

    std::unique_lock<std::mutex> assignment_lock(impl_->assignment_mutex);
#ifdef SS_TOOL_SFM
    {
        std::lock_guard<std::mutex> lock(impl_->feature_mutex);
        if (impl_->CurrentFeatureForWorkerLocked(worker_id) ||
            impl_->reserved_feature_workers.count(worker_id)) {
            SetError(error, "worker already has an active feature attempt");
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(impl_->reconstruction_mutex);
        if (impl_->CurrentReconstructionForWorkerLocked(worker_id) ||
            impl_->reserved_reconstruction_workers.count(worker_id)) {
            SetError(error, "worker already has an active reconstruction attempt");
            return false;
        }
    }
#endif
    {
        std::lock_guard<std::mutex> pairing_lock(impl_->pairing_mutex);
        bool still_paired = false;
        for (const auto& worker : impl_->pairing_leader.Workers())
            if (worker.id == worker_id &&
                worker.status == pairing::WorkerStatus::Paired &&
                !worker.revoked) {
                still_paired = true;
                break;
            }
        if (!still_paired ||
            impl_->pairing_leader.LeaderId() != paired_leader_id ||
            impl_->pairing_leader.LeaderEpoch() != paired_leader_epoch) {
            SetError(error, "training worker pairing changed before submission");
            return false;
        }
        std::lock_guard<std::mutex> lock(impl_->training_mutex);
        if (impl_->training_attempts.size() +
                impl_->reserved_training_attempts.size() >= kMaxTrainingAttempts ||
            impl_->current_training_attempts.count(job_id) ||
            impl_->reserved_training_jobs.count(job_id) ||
            impl_->CurrentTrainingForWorkerLocked(worker_id) ||
            impl_->reserved_training_workers.count(worker_id) ||
            impl_->training_attempts.count(attempt_key) ||
            impl_->reserved_training_attempts.count(attempt_key)) {
            SetError(error, "training job, worker, or attempt is already in use");
            return false;
        }
        impl_->reserved_training_jobs.insert(job_id);
        impl_->reserved_training_workers.insert(worker_id);
        impl_->reserved_training_attempts.insert(attempt_key);
        TrainingAttempt attempt;
        attempt.snapshot.job_id = job_id;
        attempt.snapshot.attempt_id = attempt_id;
        attempt.snapshot.worker_id = worker_id;
        attempt.snapshot.paired_leader_id = paired_leader_id;
        attempt.snapshot.paired_leader_epoch = paired_leader_epoch;
        attempt.snapshot.required_build = required_build;
        attempt.snapshot.result_root = impl_->training_store_directory /
            std::filesystem::u8path(job_id) /
            std::filesystem::u8path(attempt_id) / "result";
        attempt.snapshot.state = TrainingJobState::Staging;
        attempt.snapshot.current = true;
        attempt.attempt_root = attempt.snapshot.result_root.parent_path();
        attempt.disk_budget_bytes = disk_budget_bytes;
        attempt.current = true;
        if (!impl_->training_attempts.emplace(
                attempt_key, std::move(attempt)).second) {
            SetError(error, "training attempt identity is already stored");
            impl_->reserved_training_jobs.erase(job_id);
            impl_->reserved_training_workers.erase(worker_id);
            impl_->reserved_training_attempts.erase(attempt_key);
            return false;
        }
        impl_->current_training_attempts.emplace(job_id, attempt_key);
        if (!impl_->PersistTrainingJobsLocked(error)) {
            impl_->current_training_attempts.erase(job_id);
            impl_->training_attempts.erase(attempt_key);
            impl_->reserved_training_jobs.erase(job_id);
            impl_->reserved_training_workers.erase(worker_id);
            impl_->reserved_training_attempts.erase(attempt_key);
            return false;
        }
    }
    struct Reservation final {
        Impl& impl;
        const std::string& job;
        const std::string& worker;
        const std::string& attempt;
        ~Reservation() {
            std::lock_guard<std::mutex> lock(impl.training_mutex);
            impl.reserved_training_jobs.erase(job);
            impl.reserved_training_workers.erase(worker);
            impl.reserved_training_attempts.erase(attempt);
        }
    } reservation{*impl_, job_id, worker_id, attempt_key};

    bool queued = false;
    std::string queue_failure = "leader stopped before training staging";
    try {
        std::lock_guard<std::mutex> lock(impl_->training_staging_mutex);
        if (!impl_->training_staging_stopping &&
            !impl_->stopping.load(std::memory_order_acquire)) {
            impl_->training_staging_queue.push_back(task);
            --impl_->training_staging_reserved;
            capacity.queued = true;
            queued = true;
        }
    } catch (...) {
        queue_failure = "could not queue training input staging";
    }
    if (queued) {
        impl_->training_staging_condition.notify_one();
        return true;
    }
    impl_->FinishTrainingStaging(task, nullptr,
        std::move(queue_failure), false);
    return true;
#endif
}

bool LeaderServer::SupersedeTraining(const std::string& job_id,
                                     std::string* error) {
#ifndef SS_TOOL_TRAIN
    (void)job_id;
    SetError(error, "portable training is unavailable in this build");
    return false;
#else
    if (!impl_ || !Running()) {
        SetError(error, "leader server is not running");
        return false;
    }
    if (!ValidTrainingId(job_id)) {
        SetError(error, "training job identity is invalid");
        return false;
    }
    std::filesystem::path result_root;
    {
        std::lock_guard<std::mutex> lock(impl_->training_mutex);
        const auto current = impl_->current_training_attempts.find(job_id);
        if (current == impl_->current_training_attempts.end()) {
            SetError(error, "training job has no current attempt");
            return false;
        }
        const auto found = impl_->training_attempts.find(current->second);
        if (found == impl_->training_attempts.end()) {
            SetError(error, "current training attempt is missing");
            return false;
        }
        if (TerminalTrainingState(found->second.snapshot.state)) {
            SetError(error, "training job is already terminal");
            return false;
        }
        const TrainingAttempt previous = found->second;
        const std::string attempt_key = current->second;
        found->second.current = false;
        found->second.snapshot.current = false;
        found->second.snapshot.state = TrainingJobState::Superseded;
        found->second.snapshot.error = "attempt explicitly superseded";
        result_root = found->second.snapshot.result_root;
        impl_->current_training_attempts.erase(current);
        if (!impl_->PersistTrainingJobsLocked(error)) {
            found->second = previous;
            impl_->current_training_attempts.emplace(job_id, attempt_key);
            return false;
        }
    }
    std::error_code ignored;
    std::filesystem::remove_all(NativeFilesystemPath(result_root), ignored);
    return true;
#endif
}

std::vector<LeaderServer::TrainingJobSnapshot>
LeaderServer::TrainingJobs() const {
#ifndef SS_TOOL_TRAIN
    return {};
#else
    std::vector<TrainingJobSnapshot> jobs;
    if (!impl_) return jobs;
    std::lock_guard<std::mutex> lock(impl_->training_mutex);
    jobs.reserve(impl_->training_attempts.size());
    for (const auto& [attempt_key, attempt] : impl_->training_attempts)
        jobs.push_back(attempt.snapshot);
    return jobs;
#endif
}

}  // namespace app::agent
