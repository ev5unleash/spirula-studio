#include "app/AgentAdminBroker.h"
#include "app/AppPaths.h"

#include "app/AgentConfig.h"
#include "app/AgentPairing.h"
#include "app/AgentWire.h"
#include "core/Sha256.h"

#include <algorithm>
#include <array>
#include <cwchar>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#endif

namespace app::agent::admin {
namespace {

using service::Result;
using service::Configuration;
namespace fs = std::filesystem;

constexpr wchar_t kBrokerServiceNameW[] = L"SpirulaRemoteAdminBroker";
constexpr wchar_t kWorkerServiceNameW[] = L"SpirulaRemoteWorker";
constexpr wchar_t kWorkerAccountW[] = L"NT SERVICE\\SpirulaRemoteWorker";
constexpr wchar_t kPipeNameW[] = L"\\\\.\\pipe\\SpirulaRemoteAdminBroker";
constexpr wchar_t kHealthPipeNameW[] = L"\\\\.\\pipe\\SpirulaRemoteAdminBroker.Health";
constexpr wchar_t kBrokerExecutableW[] = L"spirula-admin-broker.exe";
constexpr std::uint64_t kMaxSafeInteger = 9007199254740991ULL;
constexpr std::size_t kMaxRequestBytes = 32768;
constexpr std::size_t kMaxJournalBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaxJournalEntries = 65536;
constexpr std::uint32_t kPipeTimeoutMs = 10000;
constexpr std::uint64_t kRebootHealthDeadlineSeconds = 3600;
#ifdef _WIN32
constexpr ACCESS_MASK kWorkerTraverseAccess =
    FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
constexpr ACCESS_MASK kWorkerImageAccess =
    FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
constexpr DWORD kActivationReadyTimeoutMs = 120000;
#endif

Result success() { return {true, {}}; }
Result failure(std::string message) { return {false, std::move(message)}; }

class Writer final {
public:
    std::vector<std::uint8_t> bytes;

    void u8(std::uint8_t value) { bytes.push_back(value); }
    void u32(std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8)
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void u64(std::uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8)
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    bool string(std::string_view value, std::size_t maximum) {
        if (value.size() > maximum || value.size() > 65535) return false;
        const auto size = static_cast<std::uint16_t>(value.size());
        bytes.push_back(static_cast<std::uint8_t>(size >> 8));
        bytes.push_back(static_cast<std::uint8_t>(size));
        bytes.insert(bytes.end(), value.begin(), value.end());
        return true;
    }
    bool blob(const std::vector<std::uint8_t>& value, std::size_t maximum) {
        if (value.size() > maximum || value.size() > 65535) return false;
        const auto size = static_cast<std::uint16_t>(value.size());
        bytes.push_back(static_cast<std::uint8_t>(size >> 8));
        bytes.push_back(static_cast<std::uint8_t>(size));
        bytes.insert(bytes.end(), value.begin(), value.end());
        return true;
    }
};

class Reader final {
public:
    Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
    explicit Reader(const std::vector<std::uint8_t>& data)
        : Reader(data.data(), data.size()) {}

    bool u8(std::uint8_t& value) {
        if (remaining() < 1) return false;
        value = data_[position_++];
        return true;
    }
    bool u32(std::uint32_t& value) {
        if (remaining() < 4) return false;
        value = 0;
        for (int i = 0; i < 4; ++i) value = (value << 8) | data_[position_++];
        return true;
    }
    bool u64(std::uint64_t& value) {
        if (remaining() < 8) return false;
        value = 0;
        for (int i = 0; i < 8; ++i) value = (value << 8) | data_[position_++];
        return true;
    }
    bool string(std::string& value, std::size_t maximum) {
        if (remaining() < 2) return false;
        const std::size_t size =
            (static_cast<std::size_t>(data_[position_]) << 8) |
            data_[position_ + 1];
        position_ += 2;
        if (size > maximum || remaining() < size) return false;
        value.assign(reinterpret_cast<const char*>(data_ + position_), size);
        position_ += size;
        return true;
    }
    bool blob(std::vector<std::uint8_t>& value, std::size_t maximum) {
        if (remaining() < 2) return false;
        const std::size_t size =
            (static_cast<std::size_t>(data_[position_]) << 8) |
            data_[position_ + 1];
        position_ += 2;
        if (size > maximum || remaining() < size) return false;
        value.assign(data_ + position_, data_ + position_ + size);
        position_ += size;
        return true;
    }
    bool raw(std::uint8_t* output, std::size_t count) {
        if (remaining() < count) return false;
        std::memcpy(output, data_ + position_, count);
        position_ += count;
        return true;
    }
    std::size_t remaining() const { return size_ - position_; }
    bool done() const { return position_ == size_; }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t position_ = 0;
};

bool encode_offer(Writer& writer, const update::PackageOffer& offer) {
    return writer.string(offer.worker_id, 64) &&
           writer.string(offer.leader_id, 64) &&
           (writer.u64(offer.expires_at_unix), true) &&
           writer.string(offer.update_id, 64) &&
           writer.string(offer.manifest.os, 16) &&
           writer.string(offer.manifest.architecture, 16) &&
           writer.string(offer.manifest.build, 128) &&
           writer.string(offer.manifest.release, 64) &&
           (writer.u64(offer.manifest.size), true) &&
           writer.string(offer.manifest.sha256, 64) &&
           writer.string(offer.signer_public_key_pem, 8192) &&
           writer.blob(offer.signature, 2048);
}

bool decode_offer(Reader& reader, update::PackageOffer& offer) {
    return reader.string(offer.worker_id, 64) &&
           reader.string(offer.leader_id, 64) &&
           reader.u64(offer.expires_at_unix) &&
           reader.string(offer.update_id, 64) &&
           reader.string(offer.manifest.os, 16) &&
           reader.string(offer.manifest.architecture, 16) &&
           reader.string(offer.manifest.build, 128) &&
           reader.string(offer.manifest.release, 64) &&
           reader.u64(offer.manifest.size) &&
           reader.string(offer.manifest.sha256, 64) &&
           reader.string(offer.signer_public_key_pem, 8192) &&
           reader.blob(offer.signature, 2048);
}

bool encode_request(const Intent& intent, const update::PackageOffer* offer,
                    std::vector<std::uint8_t>& output) {
    if (intent.operation != Operation::Reboot &&
        intent.operation != Operation::Activate)
        return false;
    Writer writer;
    writer.bytes.insert(writer.bytes.end(), {'S', 'A', 'B', '1'});
    writer.u8(static_cast<std::uint8_t>(intent.operation));
    if (!writer.string(intent.worker_id, 64) ||
        !writer.string(intent.leader_id, 64))
        return false;
    writer.u64(intent.leader_epoch);
    if (!writer.string(intent.intent_id, 64)) return false;
    writer.u64(intent.issued_at_unix);
    writer.u64(intent.expires_at_unix);
    if (!writer.string(intent.update_id, 64) ||
        !writer.string(intent.package_sha256, 64))
        return false;
    writer.u64(intent.security_version);
    if (!writer.string(intent.signer_public_key_pem, 8192) ||
        !writer.blob(intent.signature, 2048))
        return false;
    writer.u8(offer ? 1 : 0);
    if (offer && !encode_offer(writer, *offer)) return false;
    if (writer.bytes.size() > kMaxRequestBytes) return false;
    output = std::move(writer.bytes);
    return true;
}

bool decode_request(const std::vector<std::uint8_t>& input, Intent& intent,
                    std::optional<update::PackageOffer>& offer) {
    if (input.size() > kMaxRequestBytes || input.size() < 5 ||
        std::memcmp(input.data(), "SAB1", 4) != 0)
        return false;
    Reader reader(input.data() + 4, input.size() - 4);
    std::uint8_t operation = 0;
    if (!reader.u8(operation) ||
        (operation != static_cast<std::uint8_t>(Operation::Reboot) &&
         operation != static_cast<std::uint8_t>(Operation::Activate)))
        return false;
    intent.operation = static_cast<Operation>(operation);
    if (!reader.string(intent.worker_id, 64) ||
        !reader.string(intent.leader_id, 64) ||
        !reader.u64(intent.leader_epoch) ||
        !reader.string(intent.intent_id, 64) ||
        !reader.u64(intent.issued_at_unix) ||
        !reader.u64(intent.expires_at_unix) ||
        !reader.string(intent.update_id, 64) ||
        !reader.string(intent.package_sha256, 64) ||
        !reader.u64(intent.security_version) ||
        !reader.string(intent.signer_public_key_pem, 8192) ||
        !reader.blob(intent.signature, 2048))
        return false;
    std::uint8_t has_offer = 0;
    if (!reader.u8(has_offer) || has_offer > 1) return false;
    if (has_offer) {
        offer.emplace();
        if (!decode_offer(reader, *offer)) return false;
    }
    return reader.done();
}

bool encode_reply(const Result& result, std::vector<std::uint8_t>& output) {
    Writer writer;
    writer.u8(result.success ? 1 : 0);
    if (!writer.string(result.error, 2048)) return false;
    output = std::move(writer.bytes);
    return true;
}

bool decode_reply(const std::vector<std::uint8_t>& input, Result& result) {
    Reader reader(input);
    std::uint8_t okay = 0;
    if (!reader.u8(okay) || okay > 1 || !reader.string(result.error, 2048) ||
        !reader.done())
        return false;
    result.success = okay != 0;
    return true;
}
bool valid_hex64(const std::string& value) {
    if (value.size() != 64) return false;
    for (char ch : value)
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
    return true;
}

bool valid_id(const std::string& value) {
    if (value.empty() || value.size() > 64) return false;
    for (unsigned char ch : value)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.'))
            return false;
    return true;
}
bool encode_query_request(const std::string& intent_id,
                          std::vector<std::uint8_t>& output) {
    if (!valid_id(intent_id)) return false;
    Writer writer;
    writer.bytes.insert(writer.bytes.end(), {'S', 'A', 'B', 'Q'});
    if (!writer.string(intent_id, 64)) return false;
    output = std::move(writer.bytes);
    return true;
}

bool decode_query_request(const std::vector<std::uint8_t>& input,
                          std::string& intent_id) {
    if (input.size() < 4 || std::memcmp(input.data(), "SABQ", 4) != 0)
        return false;
    Reader reader(input.data() + 4, input.size() - 4);
    return reader.string(intent_id, 64) && reader.done() && valid_id(intent_id);
}

bool encode_outcome_reply(const Result& result, const IntentOutcome& outcome,
                          std::vector<std::uint8_t>& output) {
    Writer writer;
    writer.bytes.insert(writer.bytes.end(), {'S', 'A', 'B', 'O'});
    writer.u8(result.success ? 1 : 0);
    writer.u8(static_cast<std::uint8_t>(outcome.state));
    if (!writer.string(outcome.update_id, 64)) return false;
    writer.u64(outcome.security_version);
    if (!writer.string(outcome.detail, 2048) ||
        !writer.string(result.error, 2048))
        return false;
    output = std::move(writer.bytes);
    return true;
}

bool decode_outcome_reply(const std::vector<std::uint8_t>& input,
                          Result& result, IntentOutcome& outcome) {
    if (input.size() < 4 || std::memcmp(input.data(), "SABO", 4) != 0)
        return false;
    Reader reader(input.data() + 4, input.size() - 4);
    std::uint8_t okay = 0, state = 0;
    if (!reader.u8(okay) || okay > 1 || !reader.u8(state) ||
        state > static_cast<std::uint8_t>(IntentOutcomeState::Failed) ||
        !reader.string(outcome.update_id, 64) ||
        !reader.u64(outcome.security_version) ||
        !reader.string(outcome.detail, 2048) ||
        !reader.string(result.error, 2048) || !reader.done())
        return false;
    result.success = okay != 0;
    outcome.state = static_cast<IntentOutcomeState>(state);
    return true;
}
bool encode_health_request(const std::string& token,
                           const wire::Status& status,
                           std::vector<std::uint8_t>& output) {
    if (!valid_hex64(token)) return false;
    Writer writer;
    writer.bytes.insert(writer.bytes.end(), {'S', 'A', 'B', 'H'});
    if (!writer.string(token, 64)) return false;
    writer.u8(static_cast<std::uint8_t>(status.health));
    writer.u8(static_cast<std::uint8_t>(status.compatibility));
    if (!writer.string(status.build, 128)) return false;
    output = std::move(writer.bytes);
    return true;
}

bool decode_health_request(const std::vector<std::uint8_t>& input,
                           std::string& token, wire::Status& status) {
    if (input.size() < 4 || std::memcmp(input.data(), "SABH", 4) != 0)
        return false;
    Reader reader(input.data() + 4, input.size() - 4);
    std::uint8_t health = 0, compatibility = 0;
    if (!reader.string(token, 64) || !reader.u8(health) ||
        health > static_cast<std::uint8_t>(wire::HealthState::Unhealthy) ||
        !reader.u8(compatibility) ||
        compatibility >
            static_cast<std::uint8_t>(wire::CompatibilityState::Incompatible) ||
        !reader.string(status.build, 128) || !reader.done() ||
        !valid_hex64(token))
        return false;
    status.health = static_cast<wire::HealthState>(health);
    status.compatibility =
        static_cast<wire::CompatibilityState>(compatibility);
    return true;
}

#ifdef _WIN32

struct Handle final {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(other.value) {
        other.value = INVALID_HANDLE_VALUE;
    }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value);
            value = other.value;
            other.value = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    explicit operator bool() const { return value != INVALID_HANDLE_VALUE && value; }
};

struct ScHandle final {
    SC_HANDLE value = nullptr;
    ScHandle() = default;
    explicit ScHandle(SC_HANDLE handle) : value(handle) {}
    ~ScHandle() { if (value) CloseServiceHandle(value); }
    ScHandle(const ScHandle&) = delete;
    ScHandle& operator=(const ScHandle&) = delete;
    ScHandle(ScHandle&& other) noexcept : value(other.value) { other.value = nullptr; }
    ScHandle& operator=(ScHandle&& other) noexcept {
        if (this != &other) {
            if (value) CloseServiceHandle(value);
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }
    explicit operator bool() const { return value != nullptr; }
};

std::string win_error(const char* operation, DWORD code = GetLastError()) {
    return std::string(operation) + " failed (Windows error " +
           std::to_string(static_cast<unsigned long>(code)) + ")";
}

bool wide_to_utf8(const wchar_t* value, std::string& result) {
    if (!value) return false;
    const int length = static_cast<int>(wcslen(value));
    if (length <= 0) {
        result.clear();
        return true;
    }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value,
                                         length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return false;
    result.resize(static_cast<std::size_t>(size));
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, length,
                               result.data(), size, nullptr, nullptr) == size;
}

std::wstring quote_arg(std::wstring_view value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++slashes;
        } else if (ch == L'\"') {
            result.append(slashes * 2 + 1, L'\\');
            result += L'\"';
            slashes = 0;
        } else {
            result.append(slashes, L'\\');
            slashes = 0;
            result += ch;
        }
    }
    result.append(slashes * 2, L'\\');
    result += L'\"';
    return result;
}

bool split_command_line(const std::wstring& command,
                        std::vector<std::wstring>& args) {
    std::size_t i = 0;
    while (i < command.size()) {
        while (i < command.size() && (command[i] == L' ' || command[i] == L'\t'))
            ++i;
        if (i == command.size()) break;
        std::wstring arg;
        bool quoted = false;
        while (i < command.size() &&
               (quoted || (command[i] != L' ' && command[i] != L'\t'))) {
            std::size_t slashes = 0;
            while (i < command.size() && command[i] == L'\\') {
                ++slashes;
                ++i;
            }
            if (i < command.size() && command[i] == L'\"') {
                arg.append(slashes / 2, L'\\');
                if (slashes & 1) {
                    arg += L'\"';
                    ++i;
                } else {
                    quoted = !quoted;
                    ++i;
                }
            } else {
                arg.append(slashes, L'\\');
                if (i < command.size() &&
                    (quoted || (command[i] != L' ' && command[i] != L'\t')))
                    arg += command[i++];
            }
        }
        if (quoted) return false;
        args.push_back(std::move(arg));
    }
    return true;
}


bool equal_path(const fs::path& left, const fs::path& right) {
    const std::wstring a = left.lexically_normal().native();
    const std::wstring b = right.lexically_normal().native();
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool path_prefix(const fs::path& parent, const fs::path& child) {
    const fs::path normalized_parent = parent.lexically_normal();
    const fs::path normalized_child = child.lexically_normal();
    auto p = normalized_parent.begin();
    auto c = normalized_child.begin();
    for (; p != normalized_parent.end() && c != normalized_child.end(); ++p, ++c) {
        const std::wstring a = p->native();
        const std::wstring b = c->native();
        if (CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                 static_cast<int>(b.size()), TRUE) != CSTR_EQUAL)
            return false;
    }
    return p == normalized_parent.end();
}

bool lookup_sid(const wchar_t* account, std::vector<std::uint8_t>& sid) {
    DWORD sid_size = 0, domain_size = 0;
    SID_NAME_USE use{};
    LookupAccountNameW(nullptr, account, nullptr, &sid_size, nullptr,
                       &domain_size, &use);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !sid_size) return false;
    sid.resize(sid_size);
    std::vector<wchar_t> domain(domain_size + 1);
    return LookupAccountNameW(nullptr, account, sid.data(), &sid_size,
                              domain.data(), &domain_size, &use) != FALSE;
}

bool make_well_known_sid(WELL_KNOWN_SID_TYPE type,
                         std::array<std::uint8_t, SECURITY_MAX_SID_SIZE>& sid) {
    DWORD size = static_cast<DWORD>(sid.size());
    return CreateWellKnownSid(type, nullptr, sid.data(), &size) != FALSE;
}

bool is_system_or_admin(PSID sid) {
    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> system{}, admins{};
    return make_well_known_sid(WinLocalSystemSid, system) &&
           make_well_known_sid(WinBuiltinAdministratorsSid, admins) &&
           (EqualSid(sid, system.data()) || EqualSid(sid, admins.data()));
}

bool is_trusted_owner(PSID sid) {
    if (is_system_or_admin(sid)) return true;
    std::vector<std::uint8_t> trusted_installer;
    return lookup_sid(L"NT SERVICE\\TrustedInstaller", trusted_installer) &&
           EqualSid(sid, trusted_installer.data());
}

bool check_component_chain(const fs::path& path, bool leaf_directory,
                           std::string& error) {
    if (path.empty() || !path.is_absolute() || path != path.lexically_normal()) {
        error = "broker path is not absolute and canonical";
        return false;
    }
    fs::path current = path.root_path();
    for (const fs::path& part : path.relative_path()) {
        current /= part;
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            error = win_error("checking broker path", GetLastError());
            return false;
        }
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            error = "broker path contains a reparse point";
            return false;
        }
        const bool final = equal_path(current, path);
        if (!final && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            error = "broker path parent is not a directory";
            return false;
        }
        if (final && ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != leaf_directory) {
            error = leaf_directory ? "broker root is not a directory"
                                   : "broker image is not a regular file";
            return false;
        }
    }
    return true;
}

bool no_untrusted_write(const fs::path& path, std::string& error) {
    std::wstring name = path.native();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    PSID owner = nullptr;
    const DWORD query = GetNamedSecurityInfoW(
        name.data(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, nullptr, &descriptor);
    if (query != ERROR_SUCCESS || !descriptor || !dacl || !owner) {
        if (descriptor) LocalFree(descriptor);
        error = "cannot inspect broker path permissions";
        return false;
    }
    if (!is_trusted_owner(owner)) {
        LocalFree(descriptor);
        error = "broker path is not owned by SYSTEM, Administrators, or TrustedInstaller";
        return false;
    }
    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> world{}, users{}, auth{};
    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> creator_owner{}, creator_group{};
    const bool have_world = make_well_known_sid(WinWorldSid, world);
    const bool have_users = make_well_known_sid(WinBuiltinUsersSid, users);
    const bool have_auth = make_well_known_sid(WinAuthenticatedUserSid, auth);
    const bool have_creator_owner = make_well_known_sid(WinCreatorOwnerSid, creator_owner);
    const bool have_creator_group = make_well_known_sid(WinCreatorGroupSid, creator_group);
    constexpr ACCESS_MASK write_rights = FILE_WRITE_DATA | FILE_APPEND_DATA |
        FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD |
        FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | DELETE | WRITE_DAC | WRITE_OWNER;
    GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE,
                            FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
    ACL_SIZE_INFORMATION info{};
    if (!GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation)) {
        LocalFree(descriptor);
        error = "cannot inspect broker path ACL";
        return false;
    }
    for (DWORD i = 0; i < info.AceCount; ++i) {
        void* raw = nullptr;
        if (!GetAce(dacl, i, &raw)) {
            LocalFree(descriptor);
            error = "cannot inspect broker path ACE";
            return false;
        }
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            LocalFree(descriptor);
            error = "broker parent path has an unsupported access rule";
            return false;
        }
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
        ACCESS_MASK rights = ace->Mask;
        MapGenericMask(&rights, &mapping);
        if (!(rights & write_rights)) continue;
        PSID trustee = const_cast<DWORD*>(&ace->SidStart);
        const bool trusted = is_trusted_owner(trustee);
        const bool creator =
            (have_creator_owner && EqualSid(trustee, creator_owner.data())) ||
            (have_creator_group && EqualSid(trustee, creator_group.data()));
        const bool broad = (have_world && EqualSid(trustee, world.data())) ||
                           (have_users && EqualSid(trustee, users.data())) ||
                           (have_auth && EqualSid(trustee, auth.data()));
        if (broad || (!trusted && !(creator && is_trusted_owner(owner)))) {
            LocalFree(descriptor);
            error = "broker parent path is writable by an untrusted account";
            return false;
        }
    }
    LocalFree(descriptor);
    return true;
}

bool check_safe_parents(const fs::path& leaf, std::string& error) {
    if (leaf.empty() || !leaf.is_absolute()) {
        error = "broker path is not absolute";
        return false;
    }
    fs::path current = leaf.root_path();
    if (!check_component_chain(current, true, error)) return false;
    for (const fs::path& part : leaf.relative_path()) {
        current /= part;
        if (!check_component_chain(current, true, error) ||
            !no_untrusted_write(current, error))
            return false;
    }
    return true;
}

PSECURITY_DESCRIPTOR private_descriptor(bool inherit) {
    const wchar_t* sddl = inherit
        ? L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"
        : L"D:P(A;;FA;;;SY)(A;;FA;;;BA)";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &descriptor, nullptr))
        return nullptr;
    return descriptor;
}

bool check_exact_private_acl(const fs::path& path, std::string& error,
                             ACCESS_MASK worker_access = 0) {
    std::wstring name = path.native();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    PSID owner = nullptr;
    const DWORD query = GetNamedSecurityInfoW(
        name.data(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, nullptr, &descriptor);
    if (query != ERROR_SUCCESS || !descriptor || !owner || !dacl) {
        if (descriptor) LocalFree(descriptor);
        error = "cannot verify protected broker ACL";
        return false;
    }
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    const bool protected_dacl = GetSecurityDescriptorControl(
        descriptor, &control, &revision) && (control & SE_DACL_PROTECTED);
    ACL_SIZE_INFORMATION info{};
    const bool acl_info = GetAclInformation(dacl, &info, sizeof(info),
                                            AclSizeInformation) != FALSE;
    bool system_full = false, admins_full = false, worker_exact = false;
    std::vector<std::uint8_t> worker_sid;
    const bool have_worker = !worker_access ||
                             lookup_sid(kWorkerAccountW, worker_sid);
    bool okay = protected_dacl && acl_info && have_worker &&
                is_trusted_owner(owner) &&
                info.AceCount == 2 + (worker_access ? 1 : 0);
    for (DWORD i = 0; okay && i < info.AceCount; ++i) {
        void* raw = nullptr;
        if (!GetAce(dacl, i, &raw)) { okay = false; break; }
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || header->AceFlags) {
            okay = false;
            break;
        }
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
        PSID trustee = const_cast<DWORD*>(&ace->SidStart);
        ACCESS_MASK rights = ace->Mask;
        GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE,
                                FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
        MapGenericMask(&rights, &mapping);
        std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> system{}, admins{};
        if (!make_well_known_sid(WinLocalSystemSid, system) ||
            !make_well_known_sid(WinBuiltinAdministratorsSid, admins)) {
            okay = false;
            break;
        }
        if (EqualSid(trustee, system.data()) && rights == FILE_ALL_ACCESS)
            system_full = true;
        else if (EqualSid(trustee, admins.data()) && rights == FILE_ALL_ACCESS)
            admins_full = true;
        else if (worker_access && EqualSid(trustee, worker_sid.data()) &&
                 rights == worker_access)
            worker_exact = true;
        else
            okay = false;
    }
    LocalFree(descriptor);
    if (!okay || !system_full || !admins_full ||
        (worker_access && !worker_exact)) {
        error = "broker data ACL is not the exact protected SYSTEM/Administrators and approved worker access";
        return false;
    }
    return true;
}

bool set_private_acl(const fs::path& path) {
    PSECURITY_DESCRIPTOR descriptor = private_descriptor(false);
    if (!descriptor) return false;
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    const bool got = GetSecurityDescriptorDacl(descriptor, &present, &dacl,
                                                &defaulted) != FALSE;
    const DWORD result = got && present
        ? SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION |
                                    PROTECTED_DACL_SECURITY_INFORMATION,
                                nullptr, nullptr, dacl, nullptr)
        : ERROR_INVALID_SECURITY_DESCR;
    LocalFree(descriptor);
    return result == ERROR_SUCCESS;
}
bool worker_access_matches(const fs::path& path, ACCESS_MASK rights,
                           const std::vector<std::uint8_t>& worker_sid,
                           std::string& error) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD query = GetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &dacl, nullptr, &descriptor);
    if (query != ERROR_SUCCESS || !dacl) {
        if (descriptor) LocalFree(descriptor);
        error = "cannot verify restricted worker broker-path permissions";
        return false;
    }
    ACL_SIZE_INFORMATION info{};
    bool okay = GetAclInformation(dacl, &info, sizeof(info),
                                  AclSizeInformation) != FALSE;
    unsigned matches = 0;
    for (DWORD i = 0; okay && i < info.AceCount; ++i) {
        void* raw = nullptr;
        if (!GetAce(dacl, i, &raw)) { okay = false; break; }
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        PSID trustee = nullptr;
        ACCESS_MASK mask = 0;
        if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
            trustee = const_cast<DWORD*>(&ace->SidStart);
            mask = ace->Mask;
        } else if (header->AceType == ACCESS_DENIED_ACE_TYPE) {
            const auto* ace = static_cast<const ACCESS_DENIED_ACE*>(raw);
            trustee = const_cast<DWORD*>(&ace->SidStart);
            if (EqualSid(trustee, const_cast<std::uint8_t*>(worker_sid.data()))) okay = false;
            continue;
        } else {
            continue;
        }
        if (!EqualSid(trustee, const_cast<std::uint8_t*>(worker_sid.data()))) continue;
        ++matches;
        okay = header->AceFlags == 0 && mask == rights;
    }
    LocalFree(descriptor);
    if (!okay || matches != 1) {
        error = "restricted worker access is not the exact non-inheriting broker-path grant";
        return false;
    }
    return true;
}

bool grant_worker_access(const fs::path& path, ACCESS_MASK rights,
                         std::string& error) {
    std::vector<std::uint8_t> sid;
    if (!lookup_sid(kWorkerAccountW, sid)) {
        error = "cannot resolve restricted worker SID for protected broker files";
        return false;
    }
    PACL current = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD query = GetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &current, nullptr, &descriptor);
    if (query != ERROR_SUCCESS || !current) {
        if (descriptor) LocalFree(descriptor);
        error = "cannot read broker path DACL before granting worker access";
        return false;
    }
    EXPLICIT_ACCESSW grant{};
    grant.grfAccessPermissions = rights;
    grant.grfAccessMode = SET_ACCESS;
    grant.grfInheritance = NO_INHERITANCE;
    BuildTrusteeWithSidW(&grant.Trustee, sid.data());
    PACL updated = nullptr;
    const DWORD built = SetEntriesInAclW(1, &grant, current, &updated);
    LocalFree(descriptor);
    if (built != ERROR_SUCCESS || !updated) {
        if (updated) LocalFree(updated);
        error = "cannot build restricted worker broker-path ACL";
        return false;
    }
    const DWORD set = SetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, updated, nullptr);
    LocalFree(updated);
    return set == ERROR_SUCCESS &&
           worker_access_matches(path, rights, sid, error) &&
           no_untrusted_write(path, error);
}


bool get_program_data(fs::path& path, std::string& error) {
    PWSTR known = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &known))) {
        error = "cannot resolve the protected machine ProgramData directory";
        return false;
    }
    path = known;
    CoTaskMemFree(known);
    return check_component_chain(path, true, error);
}

bool create_protected_directory(const fs::path& path, bool exact_acl,
                                std::string& error) {
    PSECURITY_DESCRIPTOR descriptor = private_descriptor(!exact_acl);
    if (!descriptor) {
        error = "cannot construct broker directory permissions";
        return false;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    const BOOL created = CreateDirectoryW(path.c_str(), &attributes);
    const DWORD code = created ? ERROR_SUCCESS : GetLastError();
    LocalFree(descriptor);
    if (!created && code != ERROR_ALREADY_EXISTS) {
        error = win_error("creating protected broker directory", code);
        return false;
    }
    if (!check_component_chain(path, true, error)) return false;
    return exact_acl ? check_exact_private_acl(path, error)
                     : no_untrusted_write(path, error);
}

fs::path broker_root_path(std::string& error) {
    fs::path program_data;
    if (!get_program_data(program_data, error)) return {};
    return program_data / L"Spirula" / L"AdminBroker";
}

std::string sha256_bytes(const std::vector<std::uint8_t>& bytes) {
    spirula::Sha256 sha;
    if (!bytes.empty()) sha.update(bytes.data(), bytes.size());
    return sha.hex();
}

std::vector<std::uint8_t> checksum_bytes(const std::string& hex) {
    std::vector<std::uint8_t> result;
    if (!valid_hex64(hex)) return result;
    auto digit = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        return ch - 'a' + 10;
    };
    result.reserve(32);
    for (std::size_t i = 0; i < 64; i += 2)
        result.push_back(static_cast<std::uint8_t>((digit(hex[i]) << 4) |
                                                   digit(hex[i + 1])));
    return result;
}

bool write_private_file(const fs::path& path,
                        const std::vector<std::uint8_t>& bytes,
                        bool replace, std::string& error) {
    if (bytes.size() > kMaxJournalBytes) {
        error = "broker durable file exceeds its size limit";
        return false;
    }
    fs::path temporary = path;
    temporary += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
                 std::to_wstring(GetTickCount64());
    PSECURITY_DESCRIPTOR descriptor = private_descriptor(false);
    if (!descriptor) {
        error = "cannot create protected broker file";
        return false;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, &attributes,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL |
                                FILE_FLAG_OPEN_REPARSE_POINT |
                                FILE_FLAG_WRITE_THROUGH,
                            nullptr));
    LocalFree(descriptor);
    if (!file) {
        error = win_error("creating durable broker state", GetLastError());
        return false;
    }
    std::size_t offset = 0;
    bool okay = true;
    while (okay && offset < bytes.size()) {
        const DWORD amount = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        okay = WriteFile(file.value, bytes.data() + offset, amount, &written,
                         nullptr) && written == amount;
        offset += written;
    }
    okay = okay && FlushFileBuffers(file.value);
    file = Handle();
    if (!okay) {
        const DWORD code = GetLastError();
        DeleteFileW(temporary.c_str());
        error = win_error("flushing durable broker state", code);
        return false;
    }
    const DWORD flags = MOVEFILE_WRITE_THROUGH |
                        (replace ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!MoveFileExW(temporary.c_str(), path.c_str(), flags)) {
        const DWORD code = GetLastError();
        DeleteFileW(temporary.c_str());
        error = win_error("publishing durable broker state", code);
        return false;
    }
    return check_exact_private_acl(path, error);
}

bool read_private_file(const fs::path& path, std::size_t maximum,
                       std::vector<std::uint8_t>& bytes, bool missing_ok,
                       bool& exists, std::string& error) {
    exists = false;
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL |
                                FILE_FLAG_OPEN_REPARSE_POINT |
                                FILE_FLAG_SEQUENTIAL_SCAN,
                            nullptr));
    if (!file) {
        const DWORD code = GetLastError();
        if (missing_ok && (code == ERROR_FILE_NOT_FOUND ||
                           code == ERROR_PATH_NOT_FOUND))
            return true;
        error = win_error("opening protected broker state", code);
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.value, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY |
                                  FILE_ATTRIBUTE_REPARSE_POINT)) ||
        info.nNumberOfLinks != 1) {
        error = "broker state is not a single-link regular file";
        return false;
    }
    if (!check_exact_private_acl(path, error)) return false;
    const std::uint64_t size =
        (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) |
        info.nFileSizeLow;
    if (size > maximum || size > std::numeric_limits<std::size_t>::max()) {
        error = "broker state exceeds its size limit";
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD amount = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD read = 0;
        if (!ReadFile(file.value, bytes.data() + offset, amount, &read, nullptr) ||
            !read) {
            error = win_error("reading protected broker state", GetLastError());
            return false;
        }
        offset += read;
    }
    exists = true;
    return true;
}

struct ConsumedIntent {
    Operation operation = Operation::Reboot;
    TargetBinding target;
    std::string intent_id;
    std::string update_id;
    std::string package_sha256;
    std::uint64_t security_version = 0;
    IntentOutcomeState outcome = IntentOutcomeState::Pending;
    std::string outcome_detail;
};

struct Journal {
    std::uint64_t security_floor = 0;
    std::string active_package_sha256;
    std::string last_good_package_sha256;
    std::string active_image;
    std::string active_build;
    std::string last_good_image;
    std::string last_good_build;
    bool activation_pending = false;
    bool rollback_pending = false;
    std::string activation_token;
    std::string activation_image;
    std::string activation_build;
    std::string activation_package_sha256;
    std::string activation_update_id;
    std::uint64_t activation_security_version = 0;
    bool reboot_pending = false;
    std::string reboot_token;
    std::string reboot_image;
    std::string reboot_build;
    std::string reboot_package_sha256;
    std::string reboot_boot_id;
    std::uint64_t reboot_deadline_unix = 0;
    std::uint64_t reboot_intent_expires_at_unix = 0;
    bool reboot_attempted = false;
    std::vector<ConsumedIntent> consumed;
};
std::mutex g_journal_mutex;
void signal_deadline_changed();

std::vector<std::uint8_t> encode_durable(const char* magic,
                                         std::size_t magic_size,
                                         const Writer& payload) {
    std::vector<std::uint8_t> result;
    result.insert(result.end(), magic, magic + magic_size);
    result.insert(result.end(), payload.bytes.begin(), payload.bytes.end());
    const std::string hash = sha256_bytes(result);
    const std::vector<std::uint8_t> checksum = checksum_bytes(hash);
    result.insert(result.end(), checksum.begin(), checksum.end());
    return result;
}

bool decode_durable(const std::vector<std::uint8_t>& bytes,
                    std::string_view magic, Reader& payload,
                    std::string& error) {
    if (bytes.size() < magic.size() + 32 ||
        std::memcmp(bytes.data(), magic.data(), magic.size()) != 0) {
        error = "protected broker state has an invalid header";
        return false;
    }
    const std::size_t data_size = bytes.size() - 32;
    const std::string hash = sha256_bytes(
        std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + data_size));
    const std::vector<std::uint8_t> expected = checksum_bytes(hash);
    if (expected.size() != 32 ||
        !std::equal(expected.begin(), expected.end(), bytes.begin() + data_size)) {
        error = "protected broker state checksum is invalid";
        return false;
    }
    payload = Reader(bytes.data() + magic.size(), data_size - magic.size());
    return true;
}

std::vector<std::uint8_t> encode_binding(const TargetBinding& binding) {
    Writer payload;
    payload.u8(2);
    payload.string(binding.worker_id, 64);
    payload.string(binding.leader_id, 64);
    payload.u64(binding.leader_epoch);
    payload.u64(binding.initial_security_version);
    return encode_durable("SABIND01", 8, payload);
}

bool decode_binding(const std::vector<std::uint8_t>& bytes,
                    TargetBinding& binding, std::string& error) {
    Reader reader(nullptr, 0);
    if (!decode_durable(bytes, "SABIND01", reader, error)) return false;
    std::uint8_t version = 0;
    if (!reader.u8(version) || version != 2 ||
        !reader.string(binding.worker_id, 64) ||
        !reader.string(binding.leader_id, 64) ||
        !reader.u64(binding.leader_epoch) ||
        !reader.u64(binding.initial_security_version) || !reader.done() ||
        !valid_hex64(binding.worker_id) || !valid_hex64(binding.leader_id) ||
        !binding.leader_epoch || binding.leader_epoch > kMaxSafeInteger ||
        !binding.initial_security_version ||
        binding.initial_security_version > kMaxSafeInteger) {
        error = "protected broker target binding is malformed";
        return false;
    }
    return true;
}

bool save_binding(const fs::path& root, const TargetBinding& binding,
                  bool replace, std::string& error) {
    const std::vector<std::uint8_t> bytes = encode_binding(binding);
    return write_private_file(root / L"target.binding", bytes, replace, error);
}

bool load_binding(const fs::path& root, TargetBinding& binding,
                  std::string& error) {
    std::vector<std::uint8_t> bytes;
    bool exists = false;
    if (!read_private_file(root / L"target.binding", 1024, bytes, false,
                           exists, error) || !exists)
        return false;
    return decode_binding(bytes, binding, error);
}

std::vector<std::uint8_t> encode_journal(const Journal& journal) {
    Writer payload;
    payload.u8(6);
    payload.u64(journal.security_floor);
    if (!payload.string(journal.active_package_sha256, 64) ||
        !payload.string(journal.last_good_package_sha256, 64) ||
        !payload.string(journal.active_image, 32760) ||
        !payload.string(journal.active_build, 128) ||
        !payload.string(journal.last_good_image, 32760) ||
        !payload.string(journal.last_good_build, 128) ||
        journal.consumed.size() > kMaxJournalEntries)
        return {};
    payload.u8(journal.activation_pending ? 1 : 0);
    payload.u8(journal.rollback_pending ? 1 : 0);
    if (!payload.string(journal.activation_token, 64) ||
        !payload.string(journal.activation_image, 32760) ||
        !payload.string(journal.activation_build, 128) ||
        !payload.string(journal.activation_package_sha256, 64) ||
        !payload.string(journal.activation_update_id, 64))
        return {};
    payload.u64(journal.activation_security_version);
    payload.u8(journal.reboot_pending ? 1 : 0);
    if (!payload.string(journal.reboot_token, 64) ||
        !payload.string(journal.reboot_image, 32760) ||
        !payload.string(journal.reboot_build, 128) ||
        !payload.string(journal.reboot_package_sha256, 64))
        return {};
    if (!payload.string(journal.reboot_boot_id, 64)) return {};
    payload.u64(journal.reboot_deadline_unix);
    payload.u64(journal.reboot_intent_expires_at_unix);
    payload.u8(journal.reboot_attempted ? 1 : 0);
    payload.u32(static_cast<std::uint32_t>(journal.consumed.size()));
    for (const ConsumedIntent& entry : journal.consumed) {
        payload.u8(static_cast<std::uint8_t>(entry.operation));
        if (!payload.string(entry.target.worker_id, 64) ||
            !payload.string(entry.target.leader_id, 64))
            return {};
        payload.u64(entry.target.leader_epoch);
        if (!payload.string(entry.intent_id, 64) ||
            !payload.string(entry.update_id, 64) ||
            !payload.string(entry.package_sha256, 64))
            return {};
        payload.u64(entry.security_version);
        payload.u8(static_cast<std::uint8_t>(entry.outcome));
        if (!payload.string(entry.outcome_detail, 2048)) return {};
    }
    std::vector<std::uint8_t> bytes = encode_durable("SABJNL01", 8, payload);
    return bytes.size() <= kMaxJournalBytes ? bytes : std::vector<std::uint8_t>{};
}

bool decode_journal(const std::vector<std::uint8_t>& bytes, Journal& journal,
                    std::string& error) {
    Reader reader(nullptr, 0);
    if (!decode_durable(bytes, "SABJNL01", reader, error)) return false;
    std::uint8_t version = 0, pending = 0, rollback = 0, reboot = 0;
    std::uint8_t reboot_attempted = 0;
    std::uint32_t count = 0;
    if (!reader.u8(version) || version != 6 ||
        !reader.u64(journal.security_floor) ||
        !reader.string(journal.active_package_sha256, 64) ||
        !reader.string(journal.last_good_package_sha256, 64) ||
        !reader.string(journal.active_image, 32760) ||
        !reader.string(journal.active_build, 128) ||
        !reader.string(journal.last_good_image, 32760) ||
        !reader.string(journal.last_good_build, 128) ||
        !reader.u8(pending) || pending > 1 ||
        !reader.u8(rollback) || rollback > 1 ||
        !reader.string(journal.activation_token, 64) ||
        !reader.string(journal.activation_image, 32760) ||
        !reader.string(journal.activation_build, 128) ||
        !reader.string(journal.activation_package_sha256, 64) ||
        !reader.string(journal.activation_update_id, 64) ||
        !reader.u64(journal.activation_security_version) ||
        !reader.u8(reboot) || reboot > 1 ||
        !reader.string(journal.reboot_token, 64) ||
        !reader.string(journal.reboot_image, 32760) ||
        !reader.string(journal.reboot_build, 128) ||
        !reader.string(journal.reboot_package_sha256, 64) ||
        !reader.string(journal.reboot_boot_id, 64) ||
        !reader.u64(journal.reboot_deadline_unix) ||
        !reader.u64(journal.reboot_intent_expires_at_unix) ||
        !reader.u8(reboot_attempted) || reboot_attempted > 1 ||
        !reader.u32(count) || count > kMaxJournalEntries) {
        error = "protected broker replay journal is malformed";
        return false;
    }
    journal.activation_pending = pending != 0;
    journal.rollback_pending = rollback != 0;
    journal.reboot_attempted = reboot_attempted != 0;
    journal.reboot_pending = reboot != 0;
    const auto valid_image = [](const std::string& value) {
        if (value.empty()) return false;
        try {
            const fs::path path = fs::u8path(value);
            return path.is_absolute() && path == path.lexically_normal();
        } catch (...) {
            return false;
        }
    };
    if (!valid_hex64(journal.active_package_sha256) ||
        journal.active_build.empty() || !valid_image(journal.active_image) ||
        (journal.last_good_package_sha256.empty() !=
         journal.last_good_image.empty()) ||
        (journal.last_good_image.empty() != journal.last_good_build.empty()) ||
        (!journal.last_good_package_sha256.empty() &&
         !valid_hex64(journal.last_good_package_sha256))) {
        error = "protected broker active-version record is malformed";
        return false;
    }
    if (journal.activation_pending) {
        if (!valid_hex64(journal.activation_token) ||
            !valid_image(journal.activation_image) ||
            journal.activation_build.empty() ||
            !valid_hex64(journal.activation_package_sha256) ||
            !valid_id(journal.activation_update_id) ||
            !journal.activation_security_version) {
            error = "protected broker activation transaction is malformed";
            return false;
        }
    } else if (journal.rollback_pending || !journal.activation_token.empty() ||
               !journal.activation_image.empty() ||
               !journal.activation_build.empty() ||
               !journal.activation_package_sha256.empty() ||
               !journal.activation_update_id.empty() ||
               journal.activation_security_version) {
        error = "protected broker journal has stray transaction state";
        return false;
    }
    if (journal.activation_pending && journal.reboot_pending) {
        error = "broker journal cannot contain simultaneous privileged transactions";
        return false;
    }
    if (journal.reboot_pending) {
        if (!valid_hex64(journal.reboot_token) ||
            !valid_image(journal.reboot_image) ||
            journal.reboot_build.empty() ||
            !valid_hex64(journal.reboot_package_sha256) ||
            !valid_hex64(journal.reboot_boot_id) ||
            !journal.reboot_deadline_unix ||
            !journal.reboot_intent_expires_at_unix ||
            journal.reboot_intent_expires_at_unix > journal.reboot_deadline_unix) {
            error = "protected broker reboot transaction is malformed";
            return false;
        }
    } else if (!journal.reboot_token.empty() || !journal.reboot_image.empty() ||
               !journal.reboot_build.empty() ||
               !journal.reboot_package_sha256.empty() ||
               !journal.reboot_boot_id.empty() ||
               journal.reboot_deadline_unix ||
               journal.reboot_intent_expires_at_unix ||
               journal.reboot_attempted) {
        error = "protected broker journal has stray reboot state";
        return false;
    }

    journal.consumed.clear();
    journal.consumed.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        ConsumedIntent entry;
        std::uint8_t operation = 0, outcome = 0;
        if (!reader.u8(operation) ||
            (operation != static_cast<std::uint8_t>(Operation::Reboot) &&
             operation != static_cast<std::uint8_t>(Operation::Activate))) {
            error = "protected broker replay journal contains an invalid operation";
            return false;
        }
        entry.operation = static_cast<Operation>(operation);
        if (!reader.string(entry.target.worker_id, 64) ||
            !reader.string(entry.target.leader_id, 64) ||
            !reader.u64(entry.target.leader_epoch) ||
            !reader.string(entry.intent_id, 64) ||
            !reader.string(entry.update_id, 64) ||
            !reader.string(entry.package_sha256, 64) ||
            !reader.u64(entry.security_version) ||
            !reader.u8(outcome) ||
            outcome < static_cast<std::uint8_t>(IntentOutcomeState::Pending) ||
            outcome > static_cast<std::uint8_t>(IntentOutcomeState::Failed) ||
            !reader.string(entry.outcome_detail, 2048) ||
            !valid_hex64(entry.target.worker_id) ||
            !valid_hex64(entry.target.leader_id) || !entry.target.leader_epoch ||
            entry.target.leader_epoch > kMaxSafeInteger ||
            !valid_id(entry.intent_id) ||
            (entry.operation == Operation::Reboot &&
             (!entry.update_id.empty() || !entry.package_sha256.empty() ||
              entry.security_version != 0)) ||
            (entry.operation == Operation::Activate &&
             (!valid_id(entry.update_id) ||
              !valid_hex64(entry.package_sha256) ||
              !entry.security_version))) {
            error = "protected broker replay journal contains an invalid record";
            return false;
        }
        entry.outcome = static_cast<IntentOutcomeState>(outcome);
        journal.consumed.push_back(std::move(entry));
    }
    if (!reader.done()) {
        error = "protected broker replay journal has trailing data";
        return false;
    }
    for (std::size_t i = 0; i < journal.consumed.size(); ++i) {
        for (std::size_t j = i + 1; j < journal.consumed.size(); ++j) {
            if (journal.consumed[i].intent_id == journal.consumed[j].intent_id ||
                (journal.consumed[i].operation == Operation::Activate &&
                 journal.consumed[j].operation == Operation::Activate &&
                 journal.consumed[i].update_id == journal.consumed[j].update_id)) {
                error = "protected broker replay journal contains duplicate authority";
                return false;
            }
        }
    }
    unsigned pending_count = 0;
    bool activation_intent_found = false;
    bool reboot_intent_found = false;
    for (const ConsumedIntent& entry : journal.consumed) {
        if (entry.outcome != IntentOutcomeState::Pending) continue;
        ++pending_count;
        if (entry.operation == Operation::Activate && journal.activation_pending &&
            entry.update_id == journal.activation_update_id &&
            entry.package_sha256 == journal.activation_package_sha256 &&
            entry.security_version == journal.activation_security_version)
            activation_intent_found = true;
        else if (entry.operation == Operation::Reboot && journal.reboot_pending)
            reboot_intent_found = true;
        else {
            error = "protected broker journal contains a pending intent without its transaction";
            return false;
        }
    }
    if (pending_count != static_cast<unsigned>(journal.activation_pending) +
                             static_cast<unsigned>(journal.reboot_pending) ||
        (journal.activation_pending && !activation_intent_found) ||
        (journal.reboot_pending && !reboot_intent_found)) {
        error = "protected broker transaction has no matching pending outcome record";
        return false;
    }
    return true;
}

bool save_journal(const fs::path& root, const Journal& journal,
                  std::string& error) {
    const std::vector<std::uint8_t> bytes = encode_journal(journal);
    if (bytes.empty()) {
        error = "broker replay journal has reached its durable size limit";
        return false;
    }
    return write_private_file(root / L"replay.journal", bytes, true, error);
}

bool load_journal(const fs::path& root, Journal& journal,
                  std::string& error) {
    std::vector<std::uint8_t> bytes;
    bool exists = false;
    if (!read_private_file(root / L"replay.journal", kMaxJournalBytes,
                           bytes, false, exists, error) || !exists)
        return false;
    return decode_journal(bytes, journal, error);
}
ConsumedIntent* find_intent(Journal& journal, const std::string& intent_id) {
    const auto found = std::find_if(
        journal.consumed.begin(), journal.consumed.end(),
        [&](const ConsumedIntent& entry) { return entry.intent_id == intent_id; });
    return found == journal.consumed.end() ? nullptr : &*found;
}

const ConsumedIntent* find_intent(const Journal& journal,
                                  const std::string& intent_id) {
    const auto found = std::find_if(
        journal.consumed.begin(), journal.consumed.end(),
        [&](const ConsumedIntent& entry) { return entry.intent_id == intent_id; });
    return found == journal.consumed.end() ? nullptr : &*found;
}
Result lookup_intent_outcome(const fs::path& root,
                             const std::string& intent_id,
                             IntentOutcome& outcome) {
    std::lock_guard<std::mutex> journal_lock(g_journal_mutex);
    Journal journal;
    std::string error;
    if (!load_journal(root, journal, error)) return failure(error);
    const ConsumedIntent* entry = find_intent(journal, intent_id);
    outcome = {};
    if (entry) {
        outcome.state = entry->outcome;
        outcome.update_id = entry->update_id;
        outcome.security_version = entry->security_version;
        outcome.detail = entry->outcome_detail;
    }
    return success();
}

bool load_policy_and_binding(const fs::path& root, Configuration& configuration,
                             Config& policy, TargetBinding& binding,
                             std::string& error) {
    const Result service_config =
        service::broker_worker_configuration(configuration);
    if (!service_config.success) {
        error = "worker service configuration is not safe for broker control: " +
                service_config.error;
        return false;
    }
    const Result worker_acl = service::verify_broker_worker_control();
    if (!worker_acl.success) {
        error = "fixed worker SCM control permissions are not broker-safe: " +
                worker_acl.error;
        return false;
    }
    try {
        policy = load_config(configuration.config_root);
    } catch (const std::exception& exception) {
        error = std::string("cannot load admin-pinned worker policy: ") +
                exception.what();
        return false;
    } catch (...) {
        error = "cannot load admin-pinned worker policy";
        return false;
    }
    if (!load_binding(root, binding, error)) return false;
    if (!valid_hex64(policy.leader_id) || binding.leader_id != policy.leader_id) {
        error = "broker target binding no longer matches the admin-pinned leader policy";
        return false;
    }
    return true;
}

std::optional<pairing::Worker> matching_local_pairing(
    const Configuration& configuration, const TargetBinding& binding,
    std::string& error) {
    auto opened = pairing::Worker::OpenMachine(configuration.state_root, &error);
    if (!opened) {
        if (error.empty()) error = "cannot load machine worker pairing state";
        return std::nullopt;
    }
    if (opened->Status() != pairing::WorkerStatus::Paired ||
        !opened->IsPairedTo(binding.worker_id, binding.leader_id) ||
        opened->LeaderEpoch() != binding.leader_epoch) {
        error = "live worker pairing does not match the broker's admin-approved target grant";
        return std::nullopt;
    }
    return opened;
}

bool sha256_open_file(HANDLE file, std::uint64_t expected_size,
                      std::string& digest, std::string& error) {
    if (SetFilePointerEx(file, LARGE_INTEGER{}, nullptr, FILE_BEGIN) == FALSE) {
        error = win_error("seeking staged package", GetLastError());
        return false;
    }
    spirula::Sha256 sha;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    std::uint64_t total = 0;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                      &got, nullptr)) {
            error = win_error("reading staged package", GetLastError());
            return false;
        }
        if (!got) break;
        total += got;
        if (total > expected_size) {
            error = "staged package is larger than its signed size";
            return false;
        }
        sha.update(buffer.data(), got);
    }
    if (total != expected_size) {
        error = "staged package size does not match the signed manifest";
        return false;
    }
    digest = sha.hex();
    return true;
}

bool image_identity(const fs::path& path, std::string& digest,
                    std::uint64_t& size, std::string& error) {
    if (!check_component_chain(path, false, error)) return false;
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                                FILE_FLAG_SEQUENTIAL_SCAN,
                            nullptr));
    if (!file) {
        error = win_error("opening protected worker image", GetLastError());
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.value, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY |
                                  FILE_ATTRIBUTE_REPARSE_POINT)) ||
        info.nNumberOfLinks != 1) {
        error = "worker image is not a single-link regular file";
        return false;
    }
    size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) |
           info.nFileSizeLow;
    return sha256_open_file(file.value, size, digest, error);
}

bool ensure_version_directory(const fs::path& path, ACCESS_MASK worker_access,
                              std::string& error) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES &&
        GetLastError() == ERROR_FILE_NOT_FOUND) {
        PSECURITY_DESCRIPTOR descriptor = private_descriptor(false);
        if (!descriptor) {
            error = "cannot construct version directory permissions";
            return false;
        }
        SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
        const BOOL created = CreateDirectoryW(path.c_str(), &security);
        const DWORD code = created ? ERROR_SUCCESS : GetLastError();
        LocalFree(descriptor);
        if (!created && code != ERROR_ALREADY_EXISTS) {
            error = win_error("creating protected update version directory", code);
            return false;
        }
        attributes = GetFileAttributesW(path.c_str());
    }
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !check_component_chain(path, true, error)) {
        if (error.empty()) error = "protected update version path is not a directory";
        return false;
    }
    bool private_acl = check_exact_private_acl(path, error);
    if (!private_acl) {
        error.clear();
        private_acl = check_exact_private_acl(path, error, worker_access);
    }
    return private_acl && grant_worker_access(path, worker_access, error);
}

bool copy_staged_offer(const fs::path& root,
                       const Configuration& configuration,
                       const update::PackageOffer& offer,
                       fs::path& image, std::string& error) {
    if (!valid_hex64(offer.manifest.sha256) || !offer.manifest.size ||
        offer.manifest.size > update::kMaxPackageBytes) {
        error = "signed package manifest is invalid";
        return false;
    }
    const fs::path source = configuration.storage_root / L"agent-updates" /
                            fs::u8path(offer.manifest.sha256) / L"spirula.exe";
    const fs::path update_root = configuration.storage_root / L"agent-updates";
    if (!check_component_chain(configuration.storage_root, true, error) ||
        !check_component_chain(update_root, true, error) ||
        !check_component_chain(source.parent_path(), true, error))
        return false;
    Handle input(CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                                 FILE_FLAG_SEQUENTIAL_SCAN,
                             nullptr));
    if (!input) {
        error = win_error("opening the fixed staged update image", GetLastError());
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(input.value, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY |
                                  FILE_ATTRIBUTE_REPARSE_POINT)) ||
        info.nNumberOfLinks != 1) {
        error = "staged update image is not a single-link regular file";
        return false;
    }
    const std::uint64_t size =
        (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) |
        info.nFileSizeLow;
    if (size != offer.manifest.size) {
        error = "staged image size differs from the signed package manifest";
        return false;
    }
    std::wstring final_name(32768, L'\0');
    const DWORD final_size = GetFinalPathNameByHandleW(
        input.value, final_name.data(), static_cast<DWORD>(final_name.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!final_size || final_size >= final_name.size()) {
        error = "cannot resolve staged package path from its open handle";
        return false;
    }
    final_name.resize(final_size);
    if (final_name.compare(0, 4, L"\\\\?\\") == 0)
        final_name.erase(0, 4);
    if (!equal_path(fs::path(final_name), source) ||
        !path_prefix(update_root, fs::path(final_name))) {
        error = "staged package handle escaped its fixed worker storage location";
        return false;
    }

    const fs::path versions = root / L"versions";
    const fs::path version = versions / fs::u8path(offer.manifest.sha256);
    if (!ensure_version_directory(versions, kWorkerTraverseAccess, error) ||
        !ensure_version_directory(version, kWorkerImageAccess, error))
        return false;
    image = version / L"spirula.exe";
    const DWORD existing = GetFileAttributesW(image.c_str());
    if (existing != INVALID_FILE_ATTRIBUTES) {
        if ((existing & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !check_exact_private_acl(image, error, kWorkerImageAccess))
            return false;
        std::string digest;
        std::uint64_t existing_size = 0;
        if (!image_identity(image, digest, existing_size, error) ||
            digest != offer.manifest.sha256 || existing_size != size) {
            if (error.empty()) error = "protected version image does not match its signed digest";
            return false;
        }
        return true;
    }
    if (GetLastError() != ERROR_FILE_NOT_FOUND) {
        error = win_error("checking protected update version image", GetLastError());
        return false;
    }
    const fs::path temporary = version /
        (L"spirula.exe.tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
         std::to_wstring(GetTickCount64()));
    PSECURITY_DESCRIPTOR descriptor = private_descriptor(false);
    if (!descriptor) {
        error = "cannot construct protected version image permissions";
        return false;
    }
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    Handle output(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, &security,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL |
                                  FILE_FLAG_OPEN_REPARSE_POINT |
                                  FILE_FLAG_WRITE_THROUGH,
                              nullptr));
    LocalFree(descriptor);
    if (!output) {
        error = win_error("creating protected update image", GetLastError());
        return false;
    }
    spirula::Sha256 sha;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    std::uint64_t total = 0;
    bool okay = true;
    while (okay) {
        DWORD got = 0;
        if (!ReadFile(input.value, buffer.data(),
                      static_cast<DWORD>(buffer.size()), &got, nullptr)) {
            error = win_error("copying staged package image", GetLastError());
            okay = false;
            break;
        }
        if (!got) break;
        total += got;
        if (total > size) {
            error = "staged package grew beyond its signed size during copy";
            okay = false;
            break;
        }
        sha.update(buffer.data(), got);
        DWORD written = 0;
        if (!WriteFile(output.value, buffer.data(), got, &written, nullptr) ||
            written != got) {
            error = win_error("writing protected update image", GetLastError());
            okay = false;
        }
    }
    okay = okay && total == size && sha.hex() == offer.manifest.sha256 &&
           FlushFileBuffers(output.value) != FALSE;
    if (!okay) {
        if (error.empty()) error = "copied package bytes do not match signed size and SHA-256";
        output = Handle();
        DeleteFileW(temporary.c_str());
        return false;
    }
    output = Handle();
    if (!MoveFileExW(temporary.c_str(), image.c_str(), MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        DeleteFileW(temporary.c_str());
        error = win_error("publishing immutable protected update image", code);
        return false;
    }
    if (!grant_worker_access(image, kWorkerImageAccess, error) ||
        !check_exact_private_acl(image, error, kWorkerImageAccess))
        return false;
    std::string copied_digest;
    std::uint64_t copied_size = 0;
    return image_identity(image, copied_digest, copied_size, error) &&
           copied_digest == offer.manifest.sha256 && copied_size == size;
}

bool find_replay(const Journal& journal, const Intent& intent,
                 std::string& error) {
    if (journal.consumed.size() >= kMaxJournalEntries) {
        error = "broker replay journal is full; refusing privileged action";
        return true;
    }
    for (const ConsumedIntent& entry : journal.consumed) {
        if (entry.intent_id == intent.intent_id ||
            (intent.operation == Operation::Activate &&
             entry.operation == Operation::Activate &&
             entry.update_id == intent.update_id)) {
            error = "broker rejected a replayed admin intent or update ID";
            return true;
        }
    }
    if (intent.operation == Operation::Activate &&
        intent.security_version <= journal.security_floor) {
        error = "broker rejected an update below its durable security-version floor";
        return true;
    }
    return false;
}

bool admin_elevated() {
    Handle token;
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
    token = Handle(raw);
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    if (!GetTokenInformation(token.value, TokenElevation, &elevation,
                             sizeof(elevation), &size) ||
        !elevation.TokenIsElevated)
        return false;
    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> admins{};
    if (!make_well_known_sid(WinBuiltinAdministratorsSid, admins)) return false;
    BOOL member = FALSE;
    return CheckTokenMembership(token.value, admins.data(), &member) && member;
}

bool current_system_service_token(std::string& error) {
    Handle token;
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) {
        error = win_error("checking broker service identity", GetLastError());
        return false;
    }
    token = Handle(raw);
    DWORD size = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !size) {
        error = "cannot inspect broker service token";
        return false;
    }
    std::vector<std::uint8_t> buffer(size);
    if (!GetTokenInformation(token.value, TokenUser, buffer.data(), size, &size)) {
        error = win_error("checking broker service identity", GetLastError());
        return false;
    }
    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> system{};
    DWORD sid_size = static_cast<DWORD>(system.size());
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system.data(), &sid_size) ||
        !EqualSid(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,
                  system.data())) {
        error = "broker service is not running as LocalSystem";
        return false;
    }
    DWORD session = MAXDWORD;
    if (!GetTokenInformation(token.value, TokenSessionId, &session,
                             sizeof(session), &size) || session != 0) {
        error = "broker service is not running in session zero";
        return false;
    }
    return true;
}

bool service_dacl_is_private(SC_HANDLE service_handle, std::string& error) {
    DWORD needed = 0;
    QueryServiceObjectSecurity(service_handle, DACL_SECURITY_INFORMATION,
                               nullptr, 0, &needed);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !needed) {
        error = win_error("querying broker service permissions", GetLastError());
        return false;
    }
    std::vector<std::uint8_t> bytes(needed);
    auto* descriptor = reinterpret_cast<PSECURITY_DESCRIPTOR>(bytes.data());
    if (!QueryServiceObjectSecurity(service_handle, DACL_SECURITY_INFORMATION,
                                    descriptor, needed, &needed)) {
        error = win_error("querying broker service permissions", GetLastError());
        return false;
    }
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    if (!GetSecurityDescriptorControl(descriptor, &control, &revision) ||
        !(control & SE_DACL_PROTECTED) ||
        !GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) ||
        !present || !dacl) {
        error = "broker service DACL is not protected";
        return false;
    }
    ACL_SIZE_INFORMATION info{};
    if (!GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation) ||
        info.AceCount != 2) {
        error = "broker service DACL is not restricted to SYSTEM and Administrators";
        return false;
    }
    bool system = false, admins = false;
    for (DWORD i = 0; i < info.AceCount; ++i) {
        void* raw = nullptr;
        if (!GetAce(dacl, i, &raw)) return false;
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || header->AceFlags) {
            error = "broker service DACL contains an unexpected ACE";
            return false;
        }
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
        if (ace->Mask != SERVICE_ALL_ACCESS) {
            error = "broker service DACL grants unexpected rights";
            return false;
        }
        PSID trustee = const_cast<DWORD*>(&ace->SidStart);
        std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> sys_sid{}, admin_sid{};
        if (!make_well_known_sid(WinLocalSystemSid, sys_sid) ||
            !make_well_known_sid(WinBuiltinAdministratorsSid, admin_sid))
            return false;
        if (EqualSid(trustee, sys_sid.data())) system = true;
        else if (EqualSid(trustee, admin_sid.data())) admins = true;
        else {
            error = "broker service DACL includes a non-administrator principal";
            return false;
        }
    }
    if (!system || !admins) {
        error = "broker service DACL omits SYSTEM or Administrators";
        return false;
    }
    return true;
}

bool query_broker_service(SC_HANDLE manager, SC_HANDLE& service_handle,
                          fs::path& image, std::string& error) {
    service_handle = OpenServiceW(manager, kBrokerServiceNameW,
                                  SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS |
                                      READ_CONTROL);
    if (!service_handle) {
        error = win_error("opening the fixed administration broker service",
                          GetLastError());
        return false;
    }
    DWORD needed = 0;
    QueryServiceConfigW(service_handle, nullptr, 0, &needed);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !needed) {
        error = win_error("reading broker service configuration", GetLastError());
        return false;
    }
    std::vector<std::uint8_t> bytes(needed);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(bytes.data());
    if (!QueryServiceConfigW(service_handle, config, needed, &needed) ||
        config->dwServiceType != SERVICE_WIN32_OWN_PROCESS ||
        !config->lpServiceStartName ||
        _wcsicmp(config->lpServiceStartName, L"LocalSystem") != 0 ||
        !config->lpBinaryPathName) {
        error = "broker SCM identity or service type is not the fixed LocalSystem contract";
        return false;
    }
    std::vector<std::wstring> args;
    if (!split_command_line(config->lpBinaryPathName, args) || args.size() != 3 ||
        args[1] != L"agent" || args[2] != L"broker-service") {
        error = "broker SCM image path is not the fixed protected command";
        return false;
    }
    image = args[0];
    if (!service_dacl_is_private(service_handle, error)) return false;
    return true;
}

bool get_module_path(fs::path& path, std::string& error) {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                             static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) {
        error = win_error("resolving broker executable path", GetLastError());
        return false;
    }
    buffer.resize(length);
    path = std::move(buffer);
    return true;
}

bool verify_runtime_install(const fs::path& root,
                            Configuration& worker_configuration,
                            std::string& error) {
    if (!check_safe_parents(root, error) ||
        !check_component_chain(root, true, error) ||
        !check_exact_private_acl(root, error, kWorkerTraverseAccess))
        return false;
    const Result worker =
        service::broker_worker_configuration(worker_configuration);
    if (!worker.success) {
        error = "cannot validate fixed worker service: " + worker.error;
        return false;
    }
    const Result worker_acl = service::verify_broker_worker_control();
    if (!worker_acl.success) {
        error = "fixed worker SCM control permissions are not broker-safe: " +
                worker_acl.error;
        return false;
    }
    fs::path module;
    if (!get_module_path(module, error)) return false;
    const fs::path expected = root / kBrokerExecutableW;
    if (!equal_path(module, expected) || equal_path(module, worker_configuration.executable)) {
        error = "broker executable is not the distinct protected image";
        return false;
    }
    if (!check_component_chain(module, false, error) ||
        !check_exact_private_acl(module, error))
        return false;
    const fs::path roots[] = {worker_configuration.config_root,
                              worker_configuration.state_root,
                              worker_configuration.storage_root,
                              worker_configuration.executable.parent_path()};
    for (const fs::path& worker_path : roots) {
        if (path_prefix(root, worker_path) || path_prefix(worker_path, root)) {
            error = "broker protected root overlaps a worker-controlled root or image directory";
            return false;
        }
    }
    ScHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager) {
        error = win_error("opening SCM to verify broker identity", GetLastError());
        return false;
    }
    SC_HANDLE raw = nullptr;
    if (!query_broker_service(manager.value, raw, module, error)) return false;
    ScHandle broker(raw);
    if (!equal_path(module, expected)) {
        error = "broker service image differs from its protected executable";
        return false;
    }
    return current_system_service_token(error);
}

std::uint64_t unix_time_now() {
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = time.dwLowDateTime;
    ticks.HighPart = time.dwHighDateTime;
    constexpr std::uint64_t kWindowsToUnixSeconds = 11644473600ULL;
    const std::uint64_t seconds = ticks.QuadPart / 10000000ULL;
    return seconds > kWindowsToUnixSeconds ? seconds - kWindowsToUnixSeconds : 0;
}

bool random_token(std::string& token, std::string& error) {
    HCRYPTPROV provider = 0;
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL,
                              CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        error = win_error("opening Windows cryptographic random source", GetLastError());
        return false;
    }
    std::array<std::uint8_t, 32> bytes{};
    const BOOL generated = CryptGenRandom(provider,
                                          static_cast<DWORD>(bytes.size()),
                                          bytes.data());
    CryptReleaseContext(provider, 0);
    if (!generated) {
        error = win_error("generating broker activation token", GetLastError());
        return false;
    }
    static constexpr char digits[] = "0123456789abcdef";
    token.clear();
    token.reserve(64);
    for (std::uint8_t byte : bytes) {
        token.push_back(digits[byte >> 4]);
        token.push_back(digits[byte & 15]);
    }
    return true;
}

bool system_boot_identifier(std::string& identifier, std::string& error) {
    struct BootEnvironmentInformation {
        GUID boot_identifier;
        ULONG firmware_type;
        ULONGLONG boot_flags;
    } information{};
    using QuerySystemInformation = LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto query = ntdll
        ? reinterpret_cast<QuerySystemInformation>(
              GetProcAddress(ntdll, "NtQuerySystemInformation"))
        : nullptr;
    constexpr ULONG kSystemBootEnvironmentInformation = 90;
    ULONG returned = 0;
    if (!query || query(kSystemBootEnvironmentInformation, &information,
                        sizeof(information), &returned) < 0 ||
        returned < sizeof(information.boot_identifier)) {
        error = "cannot obtain a system boot identifier for reboot verification";
        return false;
    }
    std::vector<std::uint8_t> bytes(sizeof(information.boot_identifier));
    std::memcpy(bytes.data(), &information.boot_identifier, bytes.size());
    identifier = sha256_bytes(bytes);
    return valid_hex64(identifier);
}

bool worker_command_matches(const Configuration& config,
                            const std::string& activation_token,
                            std::string& error);

Result process_intent(const Intent& intent, const update::PackageOffer* offer,
                      const fs::path& root) {
    Configuration configuration;
    Config policy;
    TargetBinding binding;
    std::string error;
    if (!load_policy_and_binding(root, configuration, policy, binding, error))
        return failure(error);
    if (intent.worker_id != binding.worker_id ||
        intent.leader_id != binding.leader_id ||
        intent.leader_epoch != binding.leader_epoch)
        return failure("admin intent does not match the broker's immutable worker/leader/epoch grant");
    if (!admin::VerifyIntent(policy, intent, unix_time_now(), &error))
        return failure(error.empty() ? "signed admin intent is invalid" : error);
    std::lock_guard<std::mutex> journal_lock(g_journal_mutex);
    Journal journal;
    if (!load_journal(root, journal, error)) return failure(error);
    if (find_replay(journal, intent, error)) return failure(error);
    if (journal.activation_pending || journal.reboot_pending)
        return failure("another broker-owned privileged transaction is pending");
    if (!worker_command_matches(configuration, {}, error))
        return failure("fixed worker has a stale broker readiness marker: " + error);
    if (!equal_path(configuration.executable, fs::u8path(journal.active_image)))
        return failure("fixed worker image no longer matches the broker's durable active binding");
    std::string current_digest;
    std::uint64_t current_size = 0;
    if (!image_identity(configuration.executable, current_digest, current_size,
                        error) || current_digest != journal.active_package_sha256)
        return failure(error.empty()
            ? "fixed worker image bytes differ from the broker's durable active digest"
            : error);
    auto worker = matching_local_pairing(configuration, binding, error);
    if (!worker) return failure(error);

    ConsumedIntent entry;
    entry.operation = intent.operation;
    entry.target = binding;
    entry.intent_id = intent.intent_id;
    if (intent.operation == Operation::Reboot) {
        if (offer || !intent.update_id.empty() ||
            !intent.package_sha256.empty() || intent.security_version != 0)
            return failure("reboot intent contains update-only fields");
        if (!random_token(journal.reboot_token, error)) return failure(error);
        journal.reboot_pending = true;
        journal.reboot_image = journal.active_image;
        journal.reboot_build = journal.active_build;
        journal.reboot_package_sha256 = journal.active_package_sha256;
        journal.reboot_intent_expires_at_unix = intent.expires_at_unix;
        if (!system_boot_identifier(journal.reboot_boot_id, error))
            return failure(error);
        const std::uint64_t now = unix_time_now();
        if (!now || now > std::numeric_limits<std::uint64_t>::max() -
                              kRebootHealthDeadlineSeconds)
            return failure("cannot establish a bounded reboot health deadline");
        journal.reboot_deadline_unix =
            now + kRebootHealthDeadlineSeconds;
        journal.reboot_attempted = false;
    } else {
        if (intent.operation != Operation::Activate || !offer)
            return failure("activation requires the original signed package offer");
        if (intent.security_version == 0 || !valid_id(intent.update_id) ||
            !valid_hex64(intent.package_sha256) ||
            intent.update_id != offer->update_id ||
            intent.package_sha256 != offer->manifest.sha256 ||
            intent.worker_id != offer->worker_id ||
            intent.leader_id != offer->leader_id)
            return failure("activation intent does not bind the original signed offer");
        const auto authorized = update::AuthorizePackageOffer(
            policy, *worker, *offer, {}, &error);
        if (!authorized)
            return failure(error.empty() ? "original package offer signature is invalid" : error);
        if (authorized->manifest().sha256 != intent.package_sha256 ||
            authorized->update_id() != intent.update_id ||
            authorized->worker_id() != binding.worker_id ||
            authorized->leader_id() != binding.leader_id)
            return failure("authorized package offer does not match the signed admin intent");
        if (intent.security_version <= journal.security_floor)
            return failure("broker rejected an activation below its durable security-version floor");
        fs::path candidate_image;
        if (!copy_staged_offer(root, configuration, *offer, candidate_image, error))
            return failure(error);
        Configuration candidate = configuration;
        candidate.executable = candidate_image;
        const Result candidate_check =
            service::validate_broker_worker_candidate(candidate);
        if (!candidate_check.success)
            return failure("protected candidate worker configuration is invalid: " +
                           candidate_check.error);
        if (!random_token(journal.activation_token, error))
            return failure(error);
        std::string candidate_path;
        if (!wide_to_utf8(candidate_image.native().c_str(), candidate_path))
            return failure("cannot encode protected candidate image path for the journal");
        journal.activation_pending = true;
        journal.rollback_pending = false;
        journal.activation_image = std::move(candidate_path);
        journal.activation_build = authorized->manifest().build;
        journal.activation_package_sha256 = authorized->manifest().sha256;
        journal.activation_update_id = authorized->update_id();
        journal.activation_security_version = intent.security_version;
        entry.update_id = intent.update_id;
        entry.package_sha256 = intent.package_sha256;
        entry.security_version = intent.security_version;
    }
    journal.consumed.push_back(std::move(entry));
    if (!save_journal(root, journal, error)) return failure(error);
    signal_deadline_changed();
    return success();
}

std::atomic<bool> g_service_stopping{false};
std::atomic<bool> g_transaction_active{false};
HANDLE g_stop_event = nullptr;
SERVICE_STATUS_HANDLE g_status_handle = nullptr;
HANDLE g_deadline_changed_event = nullptr;
std::mutex g_transaction_mutex;
std::thread g_transaction_thread;
std::mutex g_ready_mutex;
std::condition_variable g_ready_changed;
struct ReadinessSignal {
    std::string token;
    bool complete = false;
    bool accepted = false;
    std::string error;
};
ReadinessSignal g_readiness;
std::mutex g_health_start_mutex;
std::condition_variable g_health_start_changed;
bool g_health_listener_ready = false;
bool g_health_listener_failed = false;
std::atomic<bool> g_health_server_failed{false};

void notify_health_listener(bool ready) {
    {
        std::lock_guard<std::mutex> lock(g_health_start_mutex);
        if (ready) g_health_listener_ready = true;
        else g_health_listener_failed = true;
    }
    g_health_start_changed.notify_all();
}

void start_pending_transaction(const fs::path& root,
                               const std::string& intent_id);
bool health_pipe_server(const fs::path& root, std::string& error);
Result recover_pending_transactions(const fs::path& root);
Result cancel_pending_intent(const fs::path& root,
                             const std::string& intent_id,
                             const std::string& reason);
void signal_readiness(const std::string& token, bool accepted,
                      const std::string& error);
void reboot_deadline_watchdog(const fs::path& root);
bool repair_orphaned_worker_marker(std::string& error);
void signal_deadline_changed() {
    if (g_deadline_changed_event) SetEvent(g_deadline_changed_event);
}

bool make_worker_pipe_attributes(SECURITY_ATTRIBUTES& attributes,
                                 PSECURITY_DESCRIPTOR& descriptor,
                                 std::string& error);
void report_service_status(DWORD state, DWORD win32_error = NO_ERROR,
                           DWORD service_error = 0, DWORD wait_hint = 0) {
    if (!g_status_handle) return;
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_RUNNING
        ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    status.dwWin32ExitCode = win32_error;
    status.dwServiceSpecificExitCode = service_error;
    status.dwWaitHint = wait_hint;
    SetServiceStatus(g_status_handle, &status);
}

DWORD WINAPI broker_service_handler(DWORD control, DWORD, void*, void*) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        g_service_stopping.store(true);
        if (g_stop_event) SetEvent(g_stop_event);
        report_service_status(SERVICE_STOP_PENDING, NO_ERROR, 0, 10000);
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

bool overlapped_io(HANDLE pipe, void* buffer, DWORD size, bool read,
                   DWORD timeout_ms) {
    auto* bytes = static_cast<std::uint8_t*>(buffer);
    DWORD offset = 0;
    while (offset < size) {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event) return false;
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.value;
        DWORD transferred = 0;
        BOOL completed = read
            ? ReadFile(pipe, bytes + offset, size - offset, &transferred, &overlapped)
            : WriteFile(pipe, bytes + offset, size - offset, &transferred, &overlapped);
        if (!completed) {
            const DWORD code = GetLastError();
            if (code != ERROR_IO_PENDING) return false;
            if (WaitForSingleObject(event.value, timeout_ms) != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &overlapped);
                WaitForSingleObject(event.value, INFINITE);
                return false;
            }
            if (!GetOverlappedResult(pipe, &overlapped, &transferred, FALSE))
                return false;
        }
        if (!transferred) return false;
        offset += transferred;
    }
    return true;
}

bool connect_pipe(HANDLE pipe, HANDLE stop_event) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return false;
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.value;
    const BOOL connected = ConnectNamedPipe(pipe, &overlapped);
    if (connected) return true;
    const DWORD code = GetLastError();
    if (code == ERROR_PIPE_CONNECTED) return true;
    if (code != ERROR_IO_PENDING) return false;
    const DWORD wait = stop_event
        ? WaitForMultipleObjects(2, std::array<HANDLE, 2>{event.value, stop_event}.data(),
                                 FALSE, INFINITE)
        : WaitForSingleObject(event.value, 1000);
    if (wait == WAIT_TIMEOUT ||
        (stop_event && wait == WAIT_OBJECT_0 + 1) ||
        wait != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &overlapped);
        WaitForSingleObject(event.value, INFINITE);
        return false;
    }
    DWORD transferred = 0;
    return GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
}

bool is_worker_pipe_client(HANDLE pipe, std::string& error,
                           DWORD* client_pid_out = nullptr) {
    if (!ImpersonateNamedPipeClient(pipe)) {
        error = win_error("impersonating broker pipe client", GetLastError());
        return false;
    }
    bool okay = false;
    Handle token;
    HANDLE raw = nullptr;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw)) {
        error = win_error("querying broker pipe client token", GetLastError());
    } else {
        token = Handle(raw);
        DWORD user_size = 0;
        GetTokenInformation(token.value, TokenUser, nullptr, 0, &user_size);
        DWORD groups_size = 0;
        GetTokenInformation(token.value, TokenRestrictedSids, nullptr, 0,
                            &groups_size);
        std::vector<std::uint8_t> user_buffer(user_size);
        std::vector<std::uint8_t> groups_buffer(groups_size);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && user_size && groups_size &&
            GetTokenInformation(token.value, TokenUser, user_buffer.data(),
                                user_size, &user_size) &&
            GetTokenInformation(token.value, TokenRestrictedSids,
                                groups_buffer.data(), groups_size, &groups_size)) {
            std::vector<std::uint8_t> expected;
            if (lookup_sid(kWorkerAccountW, expected)) {
                const auto* user = reinterpret_cast<const TOKEN_USER*>(user_buffer.data());
                const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(groups_buffer.data());
                bool restricted_sid = false;
                for (DWORD i = 0; i < groups->GroupCount; ++i)
                    if (EqualSid(expected.data(), groups->Groups[i].Sid))
                        restricted_sid = true;
                DWORD session = MAXDWORD;
                DWORD returned = 0;
                const bool session_zero = GetTokenInformation(
                    token.value, TokenSessionId, &session, sizeof(session), &returned) &&
                    session == 0;
                okay = EqualSid(user->User.Sid, expected.data()) &&
                       restricted_sid && session_zero;
            }
        }
        if (!okay && error.empty())
            error = "broker pipe client is not the restricted Spirula worker service";
    }
    if (!RevertToSelf()) {
        error = "cannot revert broker pipe impersonation";
        okay = false;
    }
    if (!okay) return false;

    ULONG client_pid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &client_pid) || !client_pid) {
        error = win_error("identifying broker pipe client process", GetLastError());
        return false;
    }
    ScHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager) {
        error = win_error("opening SCM to authenticate worker process", GetLastError());
        return false;
    }
    ScHandle service(OpenServiceW(manager.value, kWorkerServiceNameW,
                                  SERVICE_QUERY_STATUS));
    if (!service) {
        error = win_error("opening the fixed worker service", GetLastError());
        return false;
    }
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    if (!QueryServiceStatusEx(service.value, SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<BYTE*>(&status), sizeof(status),
                              &bytes) || status.dwCurrentState != SERVICE_RUNNING ||
        status.dwProcessId != client_pid) {
        error = "broker pipe client is not the running fixed worker service process";
        return false;
    }
    if (client_pid_out) *client_pid_out = client_pid;
    return true;
}

bool write_pipe_reply(HANDLE pipe, const Result& result) {
    std::vector<std::uint8_t> payload;
    if (!encode_reply(result, payload) || payload.size() > 4096) return false;
    std::array<std::uint8_t, 4> prefix{};
    const std::uint32_t length = static_cast<std::uint32_t>(payload.size());
    for (int i = 0; i < 4; ++i)
        prefix[static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(length >> (24 - i * 8));
    return overlapped_io(pipe, prefix.data(), static_cast<DWORD>(prefix.size()),
                         false, kPipeTimeoutMs) &&
           overlapped_io(pipe, payload.data(), static_cast<DWORD>(payload.size()),
                         false, kPipeTimeoutMs);
}

bool write_pipe_outcome_reply(HANDLE pipe, const Result& result,
                              const IntentOutcome& outcome) {
    std::vector<std::uint8_t> payload;
    if (!encode_outcome_reply(result, outcome, payload) || payload.size() > 4096)
        return false;
    std::array<std::uint8_t, 4> prefix{};
    const std::uint32_t length = static_cast<std::uint32_t>(payload.size());
    for (int i = 0; i < 4; ++i)
        prefix[static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(length >> (24 - i * 8));
    return overlapped_io(pipe, prefix.data(), static_cast<DWORD>(prefix.size()),
                         false, kPipeTimeoutMs) &&
           overlapped_io(pipe, payload.data(), static_cast<DWORD>(payload.size()),
                         false, kPipeTimeoutMs);
}

bool read_acceptance_ack(HANDLE pipe) {
    std::array<std::uint8_t, 3> ack{};
    return overlapped_io(pipe, ack.data(), static_cast<DWORD>(ack.size()), true,
                         kPipeTimeoutMs) &&
           std::memcmp(ack.data(), "ACK", ack.size()) == 0;
}

bool read_pipe_frame(HANDLE pipe, std::vector<std::uint8_t>& payload) {
    std::array<std::uint8_t, 4> prefix{};
    if (!overlapped_io(pipe, prefix.data(), static_cast<DWORD>(prefix.size()),
                       true, kPipeTimeoutMs))
        return false;
    const std::uint32_t length =
        (static_cast<std::uint32_t>(prefix[0]) << 24) |
        (static_cast<std::uint32_t>(prefix[1]) << 16) |
        (static_cast<std::uint32_t>(prefix[2]) << 8) | prefix[3];
    if (!length || length > kMaxRequestBytes) return false;
    payload.resize(length);
    return overlapped_io(pipe, payload.data(), length, true, kPipeTimeoutMs);
}

bool run_pipe_server(const fs::path& root, std::string& error) {
    Configuration worker_configuration;
    if (!verify_runtime_install(root, worker_configuration, error)) return false;
    TargetBinding binding;
    Journal journal;
    if (!load_binding(root, binding, error)) return false;
    {
        std::lock_guard<std::mutex> journal_lock(g_journal_mutex);
        if (!load_journal(root, journal, error)) return false;
    }
    SECURITY_ATTRIBUTES attributes{};
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!make_worker_pipe_attributes(attributes, descriptor, error)) return false;
    Handle pipe(CreateNamedPipeW(
        kPipeNameW, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
            FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                PIPE_REJECT_REMOTE_CLIENTS,
            1, static_cast<DWORD>(kMaxRequestBytes + 8),
            static_cast<DWORD>(kMaxRequestBytes + 8), 5000, &attributes));
        if (!pipe) {
            error = win_error("creating protected local broker pipe", GetLastError());
            LocalFree(descriptor);
            return false;
        }
    while (!g_service_stopping.load()) {
        if (!connect_pipe(pipe.value, g_stop_event)) {
            DisconnectNamedPipe(pipe.value);
            continue;
        }
        std::string client_error;
        if (is_worker_pipe_client(pipe.value, client_error)) {
            std::vector<std::uint8_t> request;
            if (read_pipe_frame(pipe.value, request)) {
                if (request.size() >= 4 &&
                    std::memcmp(request.data(), "SABQ", 4) == 0) {
                    std::string intent_id;
                    IntentOutcome outcome;
                    Result result = decode_query_request(request, intent_id)
                        ? lookup_intent_outcome(root, intent_id, outcome)
                        : failure("broker outcome query frame is malformed");
                    write_pipe_outcome_reply(pipe.value, result, outcome);
                } else {
                    Intent intent;
                    std::optional<update::PackageOffer> offer;
                    Result result = decode_request(request, intent, offer)
                        ? process_intent(intent, offer ? &*offer : nullptr, root)
                        : failure("broker request frame is malformed");
                    const bool sent = write_pipe_reply(pipe.value, result);
                    const bool accepted = result.success && sent &&
                                          read_acceptance_ack(pipe.value);
                    DisconnectNamedPipe(pipe.value);
                    if (result.success) {
                        if (accepted)
                            start_pending_transaction(root, intent.intent_id);
                        else
                            cancel_pending_intent(root, intent.intent_id,
                                "worker did not acknowledge the durable broker acceptance");
                    }
                    continue;
                }
            }
        }
        DisconnectNamedPipe(pipe.value);
    }
    LocalFree(descriptor);
    return true;
}

void WINAPI broker_service_entry(DWORD, LPWSTR*) {
    g_status_handle = RegisterServiceCtrlHandlerExW(
        kBrokerServiceNameW, broker_service_handler, nullptr);
    if (!g_status_handle) return;
    report_service_status(SERVICE_START_PENDING, NO_ERROR, 0, 30000);
    g_service_stopping.store(false);
    g_health_server_failed.store(false);
    {
        std::lock_guard<std::mutex> lock(g_health_start_mutex);
        g_health_listener_ready = false;
        g_health_listener_failed = false;
    }
    {
        std::lock_guard<std::mutex> lock(g_ready_mutex);
        g_readiness = {};
    }
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) {
        report_service_status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 1);
        return;
    }
    g_stop_event = event.value;
    Handle deadline_event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!deadline_event) {
        g_stop_event = nullptr;
        report_service_status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 3);
        return;
    }
    g_deadline_changed_event = deadline_event.value;
    std::string error;
    const fs::path root = broker_root_path(error);
    std::string health_error;
    std::thread health_thread;
    std::thread deadline_thread;
    std::atomic<bool> watchdog_failed{false};
    std::string watchdog_error;
    bool okay = false;
    Configuration worker_configuration;
    if (root.empty() ||
        !verify_runtime_install(root, worker_configuration, error)) {
        if (error.empty()) error = "cannot validate protected broker installation";
    } else {
        try {
            health_thread = std::thread([&] {
                bool served = false;
                try {
                    served = health_pipe_server(root, health_error);
                } catch (const std::exception& exception) {
                    health_error = std::string("worker-health server exception: ") +
                                   exception.what();
                    notify_health_listener(false);
                } catch (...) {
                    health_error = "unknown worker-health server exception";
                    notify_health_listener(false);
                }
                if (!served) {
                    g_health_server_failed.store(true);
                    g_service_stopping.store(true);
                    if (g_stop_event) SetEvent(g_stop_event);
                    Journal pending;
                    std::string journal_error;
                    bool activation_pending = false;
                    {
                        std::lock_guard<std::mutex> journal_lock(g_journal_mutex);
                        activation_pending =
                            load_journal(root, pending, journal_error) &&
                            pending.activation_pending;
                    }
                    if (activation_pending)
                        signal_readiness(pending.activation_token, false,
                            health_error.empty()
                                ? "protected worker-health server stopped"
                                : health_error);
                }
            });
        } catch (const std::exception& exception) {
            error = std::string("cannot start protected worker-health server: ") +
                    exception.what();
        }
        if (health_thread.joinable()) {
            std::unique_lock<std::mutex> lock(g_health_start_mutex);
            const bool ready = g_health_start_changed.wait_for(
                lock, std::chrono::seconds(10), [] {
                    return g_health_listener_ready || g_health_listener_failed;
                });
            if (!ready || !g_health_listener_ready) {
                error = g_health_listener_failed
                    ? "protected worker-health pipe could not be established"
                    : "timed out establishing protected worker-health pipe";
            } else {
                lock.unlock();
                const Result recovered = recover_pending_transactions(root);
                if (!recovered.success) {
                    error = recovered.error;
                } else {
                    try {
                        deadline_thread = std::thread([&] {
                            try {
                                reboot_deadline_watchdog(root);
                            } catch (const std::exception& exception) {
                                watchdog_error =
                                    std::string("reboot watchdog exception: ") +
                                    exception.what();
                                watchdog_failed.store(true);
                                g_service_stopping.store(true);
                                if (g_stop_event) SetEvent(g_stop_event);
                            } catch (...) {
                                watchdog_error = "unknown reboot watchdog exception";
                                watchdog_failed.store(true);
                                g_service_stopping.store(true);
                                if (g_stop_event) SetEvent(g_stop_event);
                            }
                        });
                    } catch (const std::exception& exception) {
                        error = std::string("cannot start reboot deadline watchdog: ") +
                                exception.what();
                    }
                    if (deadline_thread.joinable()) {
                        report_service_status(SERVICE_RUNNING);
                        okay = run_pipe_server(root, error);
                    }
                }
            }
        }
    }
    g_service_stopping.store(true);
    SetEvent(g_stop_event);
    SetEvent(g_deadline_changed_event);
    if (g_transaction_thread.joinable()) g_transaction_thread.join();
    if (health_thread.joinable()) health_thread.join();
    if (deadline_thread.joinable()) deadline_thread.join();
    if (g_health_server_failed.load()) {
        okay = false;
        if (error.empty())
            error = health_error.empty()
                ? "protected worker-health server failed"
                : health_error;
    }
    if (watchdog_failed.load()) {
        okay = false;
        if (error.empty()) error = watchdog_error;
    }
    if (!okay)
        OutputDebugStringA(("admin broker stopped fail-closed: " + error + "\n").c_str());
    g_stop_event = nullptr;
    g_deadline_changed_event = nullptr;
    report_service_status(SERVICE_STOPPED,
                          okay ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR,
                          okay ? 0 : 2);
}


bool wait_service(SC_HANDLE service_handle, DWORD wanted,
                  std::string& error) {
    const ULONGLONG deadline = GetTickCount64() + 30000;
    for (;;) {
        SERVICE_STATUS_PROCESS status{};
        DWORD bytes = 0;
        if (!QueryServiceStatusEx(service_handle, SC_STATUS_PROCESS_INFO,
                                  reinterpret_cast<BYTE*>(&status),
                                  sizeof(status), &bytes)) {
            error = win_error("querying administration broker service", GetLastError());
            return false;
        }
        if (status.dwCurrentState == wanted) return true;
        if (GetTickCount64() >= deadline) {
            error = "timed out waiting for administration broker service state";
            return false;
        }
        Sleep(200);
    }
}
struct WorkerServiceHandles {
    ScHandle manager;
    ScHandle service;
};

bool open_worker_service(DWORD access, WorkerServiceHandles& handles,
                         std::string& error) {
    handles.manager = ScHandle(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!handles.manager) {
        error = win_error("opening SCM for the fixed worker service", GetLastError());
        return false;
    }
    handles.service = ScHandle(OpenServiceW(handles.manager.value,
                                             kWorkerServiceNameW, access));
    if (!handles.service) {
        error = win_error("opening the fixed worker service", GetLastError());
        return false;
    }
    return true;
}

bool query_worker_command(std::wstring& command, std::string& error) {
    WorkerServiceHandles handles;
    if (!open_worker_service(SERVICE_QUERY_CONFIG, handles, error)) return false;
    DWORD needed = 0;
    QueryServiceConfigW(handles.service.value, nullptr, 0, &needed);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !needed) {
        error = win_error("reading fixed worker SCM image", GetLastError());
        return false;
    }
    std::vector<std::uint8_t> bytes(needed);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(bytes.data());
    if (!QueryServiceConfigW(handles.service.value, config, needed, &needed) ||
        !config->lpBinaryPathName) {
        error = win_error("reading fixed worker SCM image", GetLastError());
        return false;
    }
    command = config->lpBinaryPathName;
    return true;
}

std::wstring worker_command(const Configuration& config,
                            const std::string& activation_token) {
    std::wstring command =
        quote_arg(config.executable.native()) +
        L" agent service --config-root " + quote_arg(config.config_root.native()) +
        L" --state-root " + quote_arg(config.state_root.native()) +
        L" --storage-root " + quote_arg(config.storage_root.native());
    if (!activation_token.empty()) {
        const std::wstring token(activation_token.begin(), activation_token.end());
        command += L" --broker-activation " + quote_arg(token);
    }
    return command;
}

bool worker_command_matches(const Configuration& config,
                            const std::string& activation_token,
                            std::string& error) {
    std::wstring command;
    if (!query_worker_command(command, error)) return false;
    std::vector<std::wstring> args;
    const std::size_t expected_count = activation_token.empty() ? 9 : 11;
    if (!split_command_line(command, args) || args.size() != expected_count ||
        !equal_path(fs::path(args[0]), config.executable) ||
        args[1] != L"agent" || args[2] != L"service" ||
        args[3] != L"--config-root" ||
        !equal_path(fs::path(args[4]), config.config_root) ||
        args[5] != L"--state-root" ||
        !equal_path(fs::path(args[6]), config.state_root) ||
        args[7] != L"--storage-root" ||
        !equal_path(fs::path(args[8]), config.storage_root)) {
        error = "fixed worker SCM image or roots differ from the broker transaction";
        return false;
    }
    if (!activation_token.empty()) {
        const std::wstring token(activation_token.begin(), activation_token.end());
        if (args[9] != L"--broker-activation" || args[10] != token) {
            error = "fixed worker SCM activation nonce differs from the durable transaction";
            return false;
        }
    }
    return true;
}

bool change_worker_command(const Configuration& config,
                           const std::string& activation_token,
                           std::string& error) {
    WorkerServiceHandles handles;
    if (!open_worker_service(SERVICE_CHANGE_CONFIG, handles, error)) return false;
    const std::wstring command = worker_command(config, activation_token);
    if (!ChangeServiceConfigW(handles.service.value, SERVICE_NO_CHANGE,
                              SERVICE_NO_CHANGE, SERVICE_NO_CHANGE,
                              command.c_str(), nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr)) {
        error = win_error("changing only the fixed worker SCM image", GetLastError());
        return false;
    }
    return worker_command_matches(config, activation_token, error);
}

bool query_worker_status(SERVICE_STATUS_PROCESS& status, std::string& error) {
    WorkerServiceHandles handles;
    if (!open_worker_service(SERVICE_QUERY_STATUS, handles, error)) return false;
    DWORD bytes = 0;
    if (!QueryServiceStatusEx(handles.service.value, SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<BYTE*>(&status), sizeof(status),
                              &bytes)) {
        error = win_error("querying fixed worker status", GetLastError());
        return false;
    }
    return true;
}

bool stop_worker_service(std::string& error) {
    WorkerServiceHandles handles;
    if (!open_worker_service(SERVICE_QUERY_STATUS | SERVICE_STOP, handles, error))
        return false;
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    if (!QueryServiceStatusEx(handles.service.value, SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<BYTE*>(&status), sizeof(status),
                              &bytes)) {
        error = win_error("querying worker before graceful stop", GetLastError());
        return false;
    }
    if (status.dwCurrentState == SERVICE_STOPPED) return true;
    SERVICE_STATUS ignored{};
    if (!ControlService(handles.service.value, SERVICE_CONTROL_STOP, &ignored) &&
        GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
        error = win_error("gracefully stopping fixed worker service", GetLastError());
        return false;
    }
    return wait_service(handles.service.value, SERVICE_STOPPED, error);
}

bool start_worker_service(std::string& error) {
    WorkerServiceHandles handles;
    if (!open_worker_service(SERVICE_QUERY_STATUS | SERVICE_START, handles, error))
        return false;
    if (!StartServiceW(handles.service.value, 0, nullptr)) {
        error = win_error("starting fixed worker service", GetLastError());
        return false;
    }
    if (!wait_service(handles.service.value, SERVICE_RUNNING, error)) return false;
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    if (!QueryServiceStatusEx(handles.service.value, SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<BYTE*>(&status), sizeof(status),
                              &bytes) || !status.dwProcessId) {
        error = "fixed worker reached SERVICE_RUNNING without a process ID";
        return false;
    }
    return true;
}

bool query_process_image(DWORD pid, fs::path& image, std::string& error) {
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) {
        error = win_error("opening fixed worker process identity", GetLastError());
        return false;
    }
    std::wstring buffer(32768, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(process.value, 0, buffer.data(), &size) ||
        !size || size >= buffer.size()) {
        error = win_error("reading fixed worker process image", GetLastError());
        return false;
    }
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process.value, &exit_code) || exit_code != STILL_ACTIVE) {
        error = "fixed worker process exited before health verification";
        return false;
    }
    buffer.resize(size);
    image = std::move(buffer);
    return true;
}

bool worker_failure_actions_are_default(std::string& error) {
    WorkerServiceHandles handles;
    if (!open_worker_service(SERVICE_QUERY_CONFIG, handles, error)) return false;
    DWORD needed = 0;
    QueryServiceConfig2W(handles.service.value, SERVICE_CONFIG_FAILURE_ACTIONS,
                         nullptr, 0, &needed);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !needed) {
        error = win_error("reading fixed worker restart policy", GetLastError());
        return false;
    }
    std::vector<std::uint8_t> actions_bytes(needed);
    auto* actions = reinterpret_cast<SERVICE_FAILURE_ACTIONSW*>(actions_bytes.data());
    if (!QueryServiceConfig2W(handles.service.value, SERVICE_CONFIG_FAILURE_ACTIONS,
                              reinterpret_cast<BYTE*>(actions), needed, &needed)) {
        error = win_error("reading fixed worker restart policy", GetLastError());
        return false;
    }
    const SC_ACTION expected[] = {
        {SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 30000},
        {SC_ACTION_RESTART, 300000}};
    if (actions->dwResetPeriod != 86400 || actions->cActions != 3 ||
        !actions->lpsaActions ||
        std::memcmp(actions->lpsaActions, expected, sizeof(expected)) != 0) {
        error = "fixed worker restart policy differs from its managed safe defaults";
        return false;
    }
    DWORD flag_needed = 0;
    QueryServiceConfig2W(handles.service.value, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG,
                         nullptr, 0, &flag_needed);
    SERVICE_FAILURE_ACTIONS_FLAG flag{};
    if (!QueryServiceConfig2W(
            handles.service.value, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG,
            reinterpret_cast<BYTE*>(&flag), sizeof(flag), &flag_needed) ||
        !flag.fFailureActionsOnNonCrashFailures) {
        error = "fixed worker restart policy does not restart on non-crash failure";
        return false;
    }
    return true;
}

bool set_worker_failure_actions(bool enabled, std::string& error) {
    WorkerServiceHandles handles;
    if (!open_worker_service(SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG,
                             handles, error))
        return false;
    SC_ACTION actions[] = {
        {SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 30000},
        {SC_ACTION_RESTART, 300000}};
    SERVICE_FAILURE_ACTIONS policy{};
    policy.dwResetPeriod = enabled ? 86400 : 0;
    policy.cActions = enabled ? static_cast<DWORD>(std::size(actions)) : 0;
    policy.lpsaActions = enabled ? actions : nullptr;
    SERVICE_FAILURE_ACTIONS_FLAG flag{enabled ? TRUE : FALSE};
    if (!ChangeServiceConfig2W(handles.service.value, SERVICE_CONFIG_FAILURE_ACTIONS,
                               &policy) ||
        !ChangeServiceConfig2W(handles.service.value,
                               SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag)) {
        error = win_error("updating fixed worker crash-recovery actions", GetLastError());
        return false;
    }
    if (enabled) return worker_failure_actions_are_default(error);
    DWORD needed = 0;
    QueryServiceConfig2W(handles.service.value, SERVICE_CONFIG_FAILURE_ACTIONS,
                         nullptr, 0, &needed);
    std::vector<std::uint8_t> bytes(needed);
    auto* actual = reinterpret_cast<SERVICE_FAILURE_ACTIONSW*>(bytes.data());
    if (!needed ||
        !QueryServiceConfig2W(handles.service.value, SERVICE_CONFIG_FAILURE_ACTIONS,
                              reinterpret_cast<BYTE*>(actual), needed, &needed) ||
        actual->cActions != 0) {
        error = "fixed worker restart actions did not remain disabled";
        return false;
    }
    return true;
}
void arm_readiness(const std::string& token) {
    std::lock_guard<std::mutex> lock(g_ready_mutex);
    g_readiness = {token, false, false, {}};
}

void signal_readiness(const std::string& token, bool accepted,
                      const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(g_ready_mutex);
        if (g_readiness.token != token || g_readiness.complete) return;
        g_readiness.complete = true;
        g_readiness.accepted = accepted;
        g_readiness.error = error;
    }
    g_ready_changed.notify_all();
}

bool wait_worker_readiness(const std::string& token, std::string& error) {
    std::unique_lock<std::mutex> lock(g_ready_mutex);
    const bool completed = g_ready_changed.wait_for(
        lock, std::chrono::milliseconds(kActivationReadyTimeoutMs),
        [&] { return g_readiness.token == token && g_readiness.complete; });
    if (!completed) {
        error = "timed out waiting for the restricted worker's post-initialization health report";
        return false;
    }
    if (!g_readiness.accepted) {
        error = g_readiness.error.empty()
            ? "restricted worker failed broker health verification"
            : g_readiness.error;
        return false;
    }
    return true;
}

bool verify_stable_worker(const Configuration& configuration,
                          const fs::path& image, const std::string& token,
                          const std::string& expected_digest,
                          DWORD expected_pid, std::string& error) {
    if (!worker_command_matches(configuration, token, error)) return false;
    SERVICE_STATUS_PROCESS before{};
    if (!query_worker_status(before, error) ||
        before.dwCurrentState != SERVICE_RUNNING ||
        before.dwProcessId != expected_pid) {
        if (error.empty()) error = "worker service process changed after readiness";
        return false;
    }
    fs::path process_image;
    if (!query_process_image(expected_pid, process_image, error) ||
        !equal_path(process_image, image)) {
        if (error.empty()) error = "worker image changed after readiness";
        return false;
    }
    Sleep(5000);
    SERVICE_STATUS_PROCESS after{};
    if (!query_worker_status(after, error) ||
        after.dwCurrentState != SERVICE_RUNNING ||
        after.dwProcessId != expected_pid) {
        if (error.empty()) error = "worker did not remain running after readiness";
        return false;
    }
    if (!query_process_image(expected_pid, process_image, error) ||
        !equal_path(process_image, image)) {
        if (error.empty()) error = "worker process image changed after readiness";
        return false;
    }
    std::string digest;
    std::uint64_t size = 0;
    return image_identity(image, digest, size, error) &&
           digest == expected_digest;
}
bool make_worker_pipe_attributes(SECURITY_ATTRIBUTES& attributes,
                                 PSECURITY_DESCRIPTOR& descriptor,
                                 std::string& error) {
    std::vector<std::uint8_t> sid;
    if (!lookup_sid(kWorkerAccountW, sid)) {
        error = "cannot resolve restricted worker SID for protected broker pipe";
        return false;
    }
    LPWSTR sid_text = nullptr;
    if (!ConvertSidToStringSidW(sid.data(), &sid_text)) {
        error = win_error("formatting restricted worker SID", GetLastError());
        return false;
    }
    constexpr DWORD worker_access = FILE_GENERIC_READ | FILE_WRITE_DATA |
                                    FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES;
    wchar_t rights[11]{};
    std::swprintf(rights, std::size(rights), L"0x%08lX",
                  static_cast<unsigned long>(worker_access));
    std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;";
    sddl += rights;
    sddl += L";;;";
    sddl += sid_text;
    sddl += L")";
    LocalFree(sid_text);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        error = win_error("building protected broker pipe permissions",
                          GetLastError());
        return false;
    }
    attributes = {sizeof(attributes), descriptor, FALSE};
    return true;
}

void clear_activation(Journal& journal) {
    journal.activation_pending = false;
    journal.rollback_pending = false;
    journal.activation_token.clear();
    journal.activation_image.clear();
    journal.activation_build.clear();
    journal.activation_package_sha256.clear();
    journal.activation_update_id.clear();
    journal.activation_security_version = 0;
}

void clear_reboot(Journal& journal) {
    journal.reboot_pending = false;
    journal.reboot_token.clear();
    journal.reboot_image.clear();
    journal.reboot_build.clear();
    journal.reboot_package_sha256.clear();
    journal.reboot_boot_id.clear();
    journal.reboot_deadline_unix = 0;
    journal.reboot_intent_expires_at_unix = 0;
    journal.reboot_attempted = false;
}

Result process_worker_health(DWORD client_pid,
                             const std::vector<std::uint8_t>& request,
                             const fs::path& root) {
    std::string token;
    wire::Status status;
    if (!decode_health_request(request, token, status))
        return failure("worker readiness report is malformed");
    std::lock_guard<std::mutex> journal_lock(g_journal_mutex);
    Journal journal;
    std::string error;
    if (!load_journal(root, journal, error)) return failure(error);
    const bool activation = journal.activation_pending;
    const bool reboot = journal.reboot_pending;
    const std::string expected_token = activation
        ? journal.activation_token : (reboot ? journal.reboot_token : "");
    if (token != expected_token)
        return failure("worker readiness token is not part of a pending broker transaction");
    auto reject = [&](const std::string& reason) {
        if (activation) signal_readiness(token, false, reason);
        return failure(reason);
    };
    if (status.health != wire::HealthState::Healthy ||
        status.compatibility != wire::CompatibilityState::Compatible)
        return reject("restricted worker did not report healthy compatible status");
    const bool rollback = activation && journal.rollback_pending;
    const std::string expected_build = activation
        ? (rollback ? journal.active_build : journal.activation_build)
        : journal.reboot_build;
    const std::string expected_digest = activation
        ? (rollback ? journal.active_package_sha256
                    : journal.activation_package_sha256)
        : journal.reboot_package_sha256;
    const fs::path expected_image = fs::u8path(
        activation ? (rollback ? journal.active_image
                               : journal.activation_image)
                   : journal.reboot_image);
    if (status.build != expected_build)
        return reject("restricted worker build differs from the signed/admin-bound image");
    if (reboot) {
        std::string current_boot;
        if (!system_boot_identifier(current_boot, error) ||
            current_boot == journal.reboot_boot_id)
            return reject(error.empty()
                ? "reboot outcome cannot complete before a subsequent system boot"
                : error);
    }
    if (reboot && unix_time_now() >= journal.reboot_deadline_unix)
        return reject("post-reboot worker readiness arrived after its deadline");
    Configuration configuration;
    const Result config_result =
        service::broker_worker_configuration(configuration);
    if (!config_result.success)
        return reject("fixed worker SCM configuration is not valid: " +
                      config_result.error);
    if (!equal_path(configuration.executable, expected_image))
        return reject("fixed worker SCM image differs from the pending transaction");
    if (!worker_command_matches(configuration, token, error))
        return reject(error);
    if (!service::validate_broker_worker_candidate(configuration).success)
        return reject("fixed worker configuration failed protected permission validation");
    fs::path process_image;
    if (!query_process_image(client_pid, process_image, error) ||
        !equal_path(process_image, expected_image))
        return reject(error.empty()
            ? "readiness pipe process image differs from the pending worker image"
            : error);
    SERVICE_STATUS_PROCESS service_status{};
    if (!query_worker_status(service_status, error) ||
        service_status.dwCurrentState != SERVICE_RUNNING ||
        service_status.dwProcessId != client_pid)
        return reject(error.empty()
            ? "readiness client is not the running fixed worker service process"
            : error);
    std::string digest;
    std::uint64_t size = 0;
    if (!image_identity(expected_image, digest, size, error) ||
        digest != expected_digest)
        return reject(error.empty()
            ? "worker image bytes differ from the pending transaction digest"
            : error);
    if (activation) {
        signal_readiness(token, true, {});
        return success();
    }
    if (!verify_stable_worker(configuration, expected_image, token,
                              expected_digest, client_pid, error))
        return reject(error);
    if (unix_time_now() >= journal.reboot_deadline_unix)
        return reject("post-reboot worker readiness could not be verified before its deadline");

    ConsumedIntent* entry = nullptr;
    for (ConsumedIntent& candidate : journal.consumed)
        if (candidate.operation == Operation::Reboot &&
            candidate.outcome == IntentOutcomeState::Pending) {
            entry = &candidate;
            break;
        }
    if (!entry) return failure("reboot readiness has no durable pending outcome");
    entry->outcome = IntentOutcomeState::Committed;
    entry->outcome_detail.clear();
    clear_reboot(journal);
    if (!save_journal(root, journal, error))
        return failure("cannot durably commit post-reboot worker readiness: " + error);
    signal_deadline_changed();
    if (!change_worker_command(configuration, {}, error))
        return failure("reboot outcome committed but its readiness marker remains: " +
                       error);
    return success();
}

bool health_pipe_server(const fs::path& root, std::string& error) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes{};
    if (!make_worker_pipe_attributes(attributes, descriptor, error)) {
        notify_health_listener(false);
        return false;
    }
    Handle pipe(CreateNamedPipeW(
        kHealthPipeNameW,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
            FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                PIPE_REJECT_REMOTE_CLIENTS,
            1, static_cast<DWORD>(kMaxRequestBytes + 8),
            static_cast<DWORD>(kMaxRequestBytes + 8), 5000, &attributes));
        if (!pipe) {
            error = win_error("creating protected worker health pipe", GetLastError());
            notify_health_listener(false);
            LocalFree(descriptor);
            return false;
        }
    notify_health_listener(true);
    while (!g_service_stopping.load() || g_transaction_active.load()) {
        const HANDLE stop_event =
            g_service_stopping.load() && g_transaction_active.load()
                ? nullptr : g_stop_event;
        if (!connect_pipe(pipe.value, stop_event)) {
            DisconnectNamedPipe(pipe.value);
            continue;
        }
        std::string client_error;
        DWORD client_pid = 0;
        Result outcome = failure("worker health request was not received");
        if (is_worker_pipe_client(pipe.value, client_error, &client_pid)) {
            std::vector<std::uint8_t> request;
            if (read_pipe_frame(pipe.value, request))
                outcome = process_worker_health(client_pid, request, root);
        } else {
            client_error.clear();
        }
        write_pipe_reply(pipe.value, outcome);
        DisconnectNamedPipe(pipe.value);
        if (g_service_stopping.load() && !g_transaction_active.load()) break;
    }
    LocalFree(descriptor);
    return true;
}

bool verify_managed_worker_image(const Configuration& worker,
                                 const fs::path& image,
                                 const std::string& digest,
                                 std::string& error) {
    Configuration candidate = worker;
    candidate.executable = image;
    const Result allowed = service::validate_broker_worker_candidate(candidate);
    if (!allowed.success) {
        error = "worker image is outside the protected service configuration: " +
                allowed.error;
        return false;
    }
    if (!check_exact_private_acl(image, error, kWorkerImageAccess)) return false;
    std::string actual;
    std::uint64_t size = 0;
    if (!image_identity(image, actual, size, error)) return false;
    if (actual != digest) {
        error = "worker image digest differs from the durable broker version";
        return false;
    }
    return true;
}

bool run_worker_image(const Configuration& configuration,
                      const std::string& token, const std::string& digest,
                      std::string& error) {
    if (!stop_worker_service(error) ||
        !change_worker_command(configuration, token, error))
        return false;
    arm_readiness(token);
    if (!start_worker_service(error) || !wait_worker_readiness(token, error))
        return false;
    SERVICE_STATUS_PROCESS status{};
    if (!query_worker_status(status, error) ||
        status.dwCurrentState != SERVICE_RUNNING || !status.dwProcessId) {
        if (error.empty()) error = "worker readiness lacks a live service process";
        return false;
    }
    return verify_stable_worker(configuration, configuration.executable, token,
                                digest, status.dwProcessId, error);
}

Result cancel_pending_intent(const fs::path& root,
                             const std::string& intent_id,
                             const std::string& reason) {
    std::lock_guard<std::mutex> journal_lock(g_journal_mutex);
    Journal journal;
    std::string error;
    if (!load_journal(root, journal, error)) return failure(error);
    ConsumedIntent* entry = find_intent(journal, intent_id);
    if (!entry) return failure("cannot cancel a broker intent absent from its journal");
    if (entry->outcome != IntentOutcomeState::Pending) return success();
    if (entry->operation == Operation::Activate && journal.activation_pending)
        clear_activation(journal);
    else if (entry->operation == Operation::Reboot && journal.reboot_pending)
        clear_reboot(journal);
    else
        return failure("pending broker intent has no matching transaction");
    entry->outcome = IntentOutcomeState::Failed;
    entry->outcome_detail = reason.substr(0, 2048);
    if (!save_journal(root, journal, error))
        return failure("cannot durably cancel unacknowledged broker intent: " + error);
    signal_deadline_changed();
    return success();
}

bool fail_pending_reboot(const fs::path& root, const std::string& intent_id,
                         const std::string& reason, std::string& error) {
    std::lock_guard<std::mutex> journal_lock(g_journal_mutex);
    Journal journal;
    if (!load_journal(root, journal, error)) return false;
    ConsumedIntent* entry = find_intent(journal, intent_id);
    if (!entry || entry->operation != Operation::Reboot) {
        error = "cannot fail a reboot absent from the protected replay journal";
        return false;
    }
    if (entry->outcome == IntentOutcomeState::Pending) {
        if (!journal.reboot_pending) {
            error = "pending reboot outcome has no broker transaction";
            return false;
        }
        entry->outcome = IntentOutcomeState::Failed;
        entry->outcome_detail = reason.substr(0, 2048);
        clear_reboot(journal);
        if (!save_journal(root, journal, error)) {
            error = "cannot durably fail reboot transaction: " + error;
            return false;
        }
        signal_deadline_changed();
    } else if (journal.reboot_pending || journal.activation_pending) {
        error = "terminal reboot outcome conflicts with pending broker state";
        return false;
    }
    if (!repair_orphaned_worker_marker(error)) {
        error = "reboot failure is durable but the worker marker remains: " + error;
        return false;
    }
    return true;
}


Result rollback_activation(const fs::path& root, const std::string& intent_id,
                           Journal journal, Configuration worker,
                           const std::string& reason) {
    ConsumedIntent* entry = nullptr;
    std::string error;
    {
        std::lock_guard<std::mutex> journal_lock(g_journal_mutex);
        if (!load_journal(root, journal, error)) return failure(error);
        entry = find_intent(journal, intent_id);
        if (!entry || entry->outcome != IntentOutcomeState::Pending ||
            !journal.activation_pending)
            return failure("activation rollback has no matching pending journal entry");
        journal.rollback_pending = true;
        entry->outcome_detail =
            ("rollback requested: " + reason).substr(0, 2048);
        if (!save_journal(root, journal, error))
            return failure("cannot durably mark activation rollback before SCM changes: " +
                           error);
    }


    const fs::path old_image = fs::u8path(journal.active_image);
    worker.executable = old_image;
    if (!verify_managed_worker_image(worker, old_image,
                                     journal.active_package_sha256, error))
        return failure("cannot verify last-known-good worker for rollback: " + error);
    if (!set_worker_failure_actions(false, error))
        return failure("cannot disable worker auto-restart for rollback: " + error);
    if (!run_worker_image(worker, journal.activation_token,
                          journal.active_package_sha256, error))
        return failure("last-known-good worker failed rollback health verification: " +
                       error);
    if (!set_worker_failure_actions(true, error))
        return failure("rollback worker is healthy but restart policy restoration failed: " +
                       error);
    if (!change_worker_command(worker, {}, error))
        return failure("rollback worker is healthy but activation marker removal failed: " +
                       error);

    {
        std::lock_guard<std::mutex> journal_lock(g_journal_mutex);
        Journal committed;
        if (!load_journal(root, committed, error))
            return failure("cannot reload rollback outcome journal: " + error);
        entry = find_intent(committed, intent_id);
        if (!entry || entry->outcome != IntentOutcomeState::Pending ||
            !committed.activation_pending || !committed.rollback_pending)
            return failure("rollback outcome lost its durable pending transaction");
        entry->outcome = IntentOutcomeState::RolledBack;
        entry->outcome_detail = reason.substr(0, 2048);
        clear_activation(committed);
        if (!save_journal(root, committed, error))
            return failure("rollback is healthy but its outcome could not be committed: " +
                           error);
    }
    return success();
}

Result run_activation_transaction(const fs::path& root,
                                  const std::string& intent_id,
                                  bool recovery) {
    Journal journal;
    ConsumedIntent* entry = nullptr;
    std::string error;
    {
        std::lock_guard<std::mutex> journal_lock(g_journal_mutex);
        if (!load_journal(root, journal, error)) return failure(error);
        entry = find_intent(journal, intent_id);
        if (!entry || entry->operation != Operation::Activate ||
            entry->outcome != IntentOutcomeState::Pending ||
            !journal.activation_pending)
            return failure("activation transaction is absent from the durable broker journal");
    }
    Configuration worker;
    Config policy;
    TargetBinding binding;
    if (!load_policy_and_binding(root, worker, policy, binding, error))
        return failure(error);
    if (entry->target.worker_id != binding.worker_id ||
        entry->target.leader_id != binding.leader_id ||
        entry->target.leader_epoch != binding.leader_epoch)
        return failure("pending activation no longer matches the admin-approved worker binding");
    const fs::path old_image = fs::u8path(journal.active_image);
    if (recovery || journal.rollback_pending) {
        worker.executable = old_image;
        return rollback_activation(root, intent_id, std::move(journal),
                                   std::move(worker),
                                   "broker restarted with an unfinished activation");
    }
    if (!equal_path(worker.executable, old_image) ||
        !verify_managed_worker_image(worker, old_image,
                                     journal.active_package_sha256, error)) {
        if (error.empty()) error = "fixed worker image changed before activation";
        const Result cancelled = cancel_pending_intent(root, intent_id, error);
        return cancelled.success ? failure(error) : cancelled;
    }
    const fs::path candidate_image = fs::u8path(journal.activation_image);
    const fs::path expected_candidate =
        root / L"versions" / fs::u8path(journal.activation_package_sha256) /
        L"spirula.exe";
    if (!equal_path(candidate_image, expected_candidate) ||
        !verify_managed_worker_image(worker, candidate_image,
                                     journal.activation_package_sha256, error)) {
        if (error.empty()) error = "candidate image is outside its protected digest directory";
        const Result cancelled = cancel_pending_intent(root, intent_id, error);
        return cancelled.success ? failure(error) : cancelled;
    }
    if (!worker_failure_actions_are_default(error)) {
        const Result cancelled = cancel_pending_intent(root, intent_id, error);
        return cancelled.success ? failure(error) : cancelled;
    }
    if (!set_worker_failure_actions(false, error)) {
        std::string restore_error;
        if (set_worker_failure_actions(true, restore_error)) {
            const Result cancelled = cancel_pending_intent(root, intent_id, error);
            return cancelled.success ? failure(error) : cancelled;
        }
        return failure("worker failure actions could not be safely coordinated: " +
                       error + "; restore failed: " + restore_error);
    }
    Configuration candidate = worker;
    candidate.executable = candidate_image;
    const std::string token = journal.activation_token;
    const std::string candidate_digest = journal.activation_package_sha256;
    const std::uint64_t candidate_version = journal.activation_security_version;
    const std::string candidate_build = journal.activation_build;
    const Journal pending = journal;

    bool healthy = run_worker_image(candidate, token, candidate_digest, error);
    if (healthy && !set_worker_failure_actions(true, error)) healthy = false;
    if (healthy && !change_worker_command(candidate, {}, error)) healthy = false;
    if (!healthy)
        return rollback_activation(root, intent_id, pending, worker,
                                   error.empty() ? "candidate worker did not become healthy"
                                                 : error);

    std::string active_image;
    if (!wide_to_utf8(candidate_image.native().c_str(), active_image))
        return rollback_activation(root, intent_id, pending, worker,
                                   "cannot persist candidate worker path");
    bool committed_ok = false;
    std::string commit_error;
    {
        std::lock_guard<std::mutex> journal_lock(g_journal_mutex);
        Journal committed;
        if (!load_journal(root, committed, commit_error)) {
            commit_error = "cannot reload activation commit journal: " + commit_error;
        } else {
            entry = find_intent(committed, intent_id);
            if (!entry || entry->operation != Operation::Activate ||
                entry->outcome != IntentOutcomeState::Pending ||
                !committed.activation_pending ||
                committed.activation_token != token ||
                committed.activation_package_sha256 != candidate_digest ||
                committed.activation_security_version != candidate_version) {
                commit_error =
                    "activation commit no longer matches its durable pending transaction";
            } else {
                committed.last_good_image = committed.active_image;
                committed.last_good_build = committed.active_build;
                committed.last_good_package_sha256 =
                    committed.active_package_sha256;
                committed.active_image = std::move(active_image);
                committed.active_build = candidate_build;
                committed.active_package_sha256 = candidate_digest;
                committed.security_floor = candidate_version;
                entry->outcome = IntentOutcomeState::Committed;
                entry->outcome_detail.clear();
                clear_activation(committed);
                committed_ok = save_journal(root, committed, commit_error);
            }
        }
    }
    if (!committed_ok)
        return rollback_activation(root, intent_id, pending, worker,
                                   "cannot commit protected activation journal: " +
                                       commit_error);
    return success();
}
bool request_system_reboot(bool& scheduled, std::string& error) {
    scheduled = false;
    Handle token;
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &raw)) {
        error = win_error("opening broker token for planned system reboot",
                          GetLastError());
        return false;
    }
    token = Handle(raw);
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, L"SeShutdownPrivilege", &luid)) {
        error = win_error("resolving SeShutdownPrivilege", GetLastError());
        return false;
    }
    TOKEN_PRIVILEGES enabled{};
    enabled.PrivilegeCount = 1;
    enabled.Privileges[0].Luid = luid;
    enabled.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    TOKEN_PRIVILEGES previous{};
    DWORD previous_size = sizeof(previous);
    SetLastError(ERROR_SUCCESS);
    if (!AdjustTokenPrivileges(token.value, FALSE, &enabled, sizeof(previous),
                               &previous, &previous_size) ||
        GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
        error = win_error("temporarily enabling SeShutdownPrivilege", GetLastError());
        return false;
    }
    const BOOL initiated = InitiateSystemShutdownExW(
        nullptr, L"Spirula approved maintenance reboot", 15, FALSE, TRUE,
        SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_INSTALLATION |
            SHTDN_REASON_FLAG_PLANNED);
    const DWORD initiate_error = initiated ? ERROR_SUCCESS : GetLastError();
    scheduled = initiated != FALSE || initiate_error == ERROR_SHUTDOWN_IN_PROGRESS;
    SetLastError(ERROR_SUCCESS);
    const BOOL restored = AdjustTokenPrivileges(token.value, FALSE, &previous,
                                               0, nullptr, nullptr);
    const DWORD restore_error = GetLastError();
    if (!restored || restore_error == ERROR_NOT_ALL_ASSIGNED) {
        error = win_error("restoring broker shutdown privilege state",
                          restore_error);
        return false;
    }
    if (!scheduled) {
        error = win_error("requesting approved system reboot", initiate_error);
        return false;
    }
    return true;
}

Result run_reboot_transaction(const fs::path& root,
                              const std::string& intent_id) {
    Journal journal;
    ConsumedIntent* entry = nullptr;
    std::string error;
    {
        std::lock_guard<std::mutex> journal_lock(g_journal_mutex);
        if (!load_journal(root, journal, error)) return failure(error);
        entry = find_intent(journal, intent_id);
        if (!entry || entry->operation != Operation::Reboot ||
            entry->outcome != IntentOutcomeState::Pending ||
            !journal.reboot_pending)
            return failure("reboot transaction is absent from the durable broker journal");
    }

    std::string current_boot;
    if (!system_boot_identifier(current_boot, error)) return failure(error);
    if (current_boot != journal.reboot_boot_id || journal.reboot_attempted)
        return success();
    const auto fail_before_completion = [&](const std::string& reason) {
        std::string failure_error;
        if (!fail_pending_reboot(root, intent_id, reason, failure_error))
            return failure(failure_error);
        return failure(reason);
    };
    const std::uint64_t now = unix_time_now();
    if (!now || now >= journal.reboot_intent_expires_at_unix ||
        now >= journal.reboot_deadline_unix)
        return fail_before_completion(
            "signed reboot intent or broker deadline expired before shutdown");

    Configuration worker;
    Config policy;
    TargetBinding binding;
    if (!load_policy_and_binding(root, worker, policy, binding, error))
        return failure(error);
    if (entry->target.worker_id != binding.worker_id ||
        entry->target.leader_id != binding.leader_id ||
        entry->target.leader_epoch != binding.leader_epoch)
        return failure("pending reboot no longer matches the admin-approved worker binding");
    const fs::path active_image = fs::u8path(journal.active_image);
    if (!equal_path(worker.executable, active_image) ||
        !equal_path(active_image, fs::u8path(journal.reboot_image)) ||
        !verify_managed_worker_image(worker, active_image,
                                     journal.active_package_sha256, error) ||
        journal.reboot_package_sha256 != journal.active_package_sha256 ||
        journal.reboot_build != journal.active_build)
        return failure(error.empty()
            ? "fixed worker image differs from the durable reboot target"
            : error);
    if (!change_worker_command(worker, journal.reboot_token, error))
        return fail_before_completion(
            "cannot arm fixed worker for post-reboot health proof: " + error);

    bool scheduled = false;
    bool requested = false;
    bool expired = false;
    std::string request_error;
    {
        std::lock_guard<std::mutex> journal_lock(g_journal_mutex);
        Journal current;
        if (!load_journal(root, current, error)) return failure(error);
        entry = find_intent(current, intent_id);
        if (!entry || entry->operation != Operation::Reboot ||
            entry->outcome != IntentOutcomeState::Pending ||
            !current.reboot_pending ||
            current.reboot_token != journal.reboot_token)
            return failure("reboot transaction changed before shutdown could be issued");
        if (current.reboot_attempted) return success();
        const std::uint64_t attempt_time = unix_time_now();
        if (!attempt_time ||
            attempt_time >= current.reboot_intent_expires_at_unix ||
            attempt_time >= current.reboot_deadline_unix) {
            expired = true;
        } else {
            current.reboot_attempted = true;
            if (!save_journal(root, current, error))
                return failure("cannot persist at-most-once reboot attempt before shutdown: " +
                               error);
            signal_deadline_changed();
            const std::uint64_t after_save = unix_time_now();
            if (!after_save ||
                after_save >= current.reboot_intent_expires_at_unix ||
                after_save >= current.reboot_deadline_unix) {
                expired = true;
            } else {
                requested = request_system_reboot(scheduled, request_error);
            }
        }
    }
    if (expired)
        return fail_before_completion(
            "signed reboot intent or broker deadline expired before shutdown");
    if (!requested) {
        if (scheduled)
            return failure("reboot was accepted but broker privilege restoration failed: " +
                           request_error);
        return fail_before_completion(
            request_error.empty()
                ? "Windows did not accept the broker-owned reboot request"
                : "Windows rejected the broker-owned reboot request: " + request_error);
    }
    return success();
}

Result run_pending_transaction(const fs::path& root,
                               const std::string& intent_id, bool recovery) {
    Journal journal;
    std::string error;
    const ConsumedIntent* entry = nullptr;
    {
        std::lock_guard<std::mutex> journal_lock(g_journal_mutex);
        if (!load_journal(root, journal, error)) return failure(error);
        entry = find_intent(journal, intent_id);
        if (!entry || entry->outcome != IntentOutcomeState::Pending)
            return failure("pending broker transaction lost its replay record");
    }
    if (entry->operation == Operation::Activate)
        return run_activation_transaction(root, intent_id, recovery);
    if (entry->operation == Operation::Reboot)
        return run_reboot_transaction(root, intent_id);
    return failure("pending broker transaction has an invalid operation");
}

bool launch_pending_transaction(const fs::path& root,
                                const std::string& intent_id, bool recovery,
                                std::string& error) {
    std::lock_guard<std::mutex> lock(g_transaction_mutex);
    if (g_transaction_active.load()) {
        error = "a broker-owned privileged transaction is already running";
        return false;
    }
    if (g_transaction_thread.joinable()) g_transaction_thread.join();
    g_transaction_active.store(true);
    try {
        g_transaction_thread = std::thread([root, intent_id, recovery] {
            Result result = success();
            try {
                result = run_pending_transaction(root, intent_id, recovery);
            } catch (const std::exception& exception) {
                result = failure(std::string("broker transaction exception: ") +
                                 exception.what());
            } catch (...) {
                result = failure("unknown broker transaction exception");
            }
            if (!result.success)
                OutputDebugStringA(("admin broker transaction remains pending or failed: " +
                                    result.error + "\n").c_str());
            g_transaction_active.store(false);
        });
    } catch (const std::exception& exception) {
        g_transaction_active.store(false);
        error = std::string("cannot create broker transaction worker: ") +
                exception.what();
        return false;
    }
    return true;
}

void start_pending_transaction(const fs::path& root,
                               const std::string& intent_id) {
    std::string error;
    if (launch_pending_transaction(root, intent_id, false, error)) return;
    const Result cancelled =
        cancel_pending_intent(root, intent_id, error);
    if (!cancelled.success) error += "; cancellation failed: " + cancelled.error;
    OutputDebugStringA(("admin broker could not launch accepted transaction: " +
                        error + "\n").c_str());
}

bool repair_orphaned_worker_marker(std::string& error) {
    Configuration worker;
    const Result loaded = service::broker_worker_configuration(worker);
    if (!loaded.success) {
        error = loaded.error;
        return false;
    }
    std::wstring command;
    if (!query_worker_command(command, error)) return false;
    std::vector<std::wstring> args;
    if (!split_command_line(command, args)) {
        error = "fixed worker service command line is malformed";
        return false;
    }
    if (args.size() == 9) return true;
    if (args.size() != 11 || args[9] != L"--broker-activation" ||
        args[10].size() != 64 ||
        !std::all_of(args[10].begin(), args[10].end(), [](wchar_t ch) {
            return (ch >= L'0' && ch <= L'9') ||
                   (ch >= L'a' && ch <= L'f');
        })) {
        error = "fixed worker service has an unrecognized broker activation marker";
        return false;
    }
    return change_worker_command(worker, {}, error);
}

Result recover_pending_transactions(const fs::path& root) {
    Journal journal;
    std::string error;
    {
        std::lock_guard<std::mutex> journal_lock(g_journal_mutex);
        if (!load_journal(root, journal, error)) return failure(error);
    }
    if (!journal.activation_pending && !journal.reboot_pending) {
        if (!repair_orphaned_worker_marker(error))
            return failure("cannot repair orphaned worker activation marker: " + error);
        if (!worker_failure_actions_are_default(error))
            return failure("worker crash-recovery actions are not at their safe managed defaults: " +
                           error);
        return success();
    }
    const ConsumedIntent* pending = nullptr;
    for (const ConsumedIntent& entry : journal.consumed)
        if (entry.outcome == IntentOutcomeState::Pending) {
            pending = &entry;
            break;
        }
    if (!pending) return failure("broker transaction has no pending replay entry");
    bool recovery = false;
    if (journal.activation_pending) {
        recovery = true;
    } else {
        std::string current_boot;
        if (!system_boot_identifier(current_boot, error)) return failure(error);
        if (current_boot != journal.reboot_boot_id ||
            journal.reboot_attempted ||
            unix_time_now() >= journal.reboot_intent_expires_at_unix ||
            unix_time_now() >= journal.reboot_deadline_unix)
            return success();
    }
    if (!launch_pending_transaction(root, pending->intent_id, recovery, error))
        return failure(error);
    return success();
}

void reboot_deadline_watchdog(const fs::path& root) {
    const HANDLE wait_handles[] = {g_stop_event, g_deadline_changed_event};
    while (!g_service_stopping.load()) {
        DWORD wait_ms = INFINITE;
        std::string error;
        {
            std::lock_guard<std::mutex> journal_lock(g_journal_mutex);
            Journal journal;
            if (!load_journal(root, journal, error)) {
                wait_ms = 1000;
            } else if (journal.reboot_pending) {
                std::string current_boot;
                if (!system_boot_identifier(current_boot, error)) {
                    wait_ms = 1000;
                } else {
                    const bool subsequent_boot =
                        current_boot != journal.reboot_boot_id;
                    const std::uint64_t deadline =
                        !subsequent_boot && !journal.reboot_attempted
                            ? std::min(journal.reboot_deadline_unix,
                                       journal.reboot_intent_expires_at_unix)
                            : journal.reboot_deadline_unix;
                    const std::uint64_t now = unix_time_now();
                    if (!now) {
                        error = "cannot read the clock for reboot deadline recovery";
                        wait_ms = 1000;
                    } else if (now >= deadline) {
                        ConsumedIntent* pending = nullptr;
                        for (ConsumedIntent& entry : journal.consumed)
                            if (entry.operation == Operation::Reboot &&
                                entry.outcome == IntentOutcomeState::Pending) {
                                pending = &entry;
                                break;
                            }
                        if (!pending) {
                            error = "expired reboot transaction has no pending replay record";
                            wait_ms = 1000;
                        } else {
                            pending->outcome = IntentOutcomeState::Failed;
                            pending->outcome_detail = !subsequent_boot &&
                                    !journal.reboot_attempted
                                ? "signed reboot intent expired before shutdown was issued"
                                : (!subsequent_boot
                                    ? "broker-requested reboot did not complete within one hour"
                                    : "post-reboot worker did not report healthy readiness within one hour");
                            clear_reboot(journal);
                            if (!save_journal(root, journal, error)) {
                                wait_ms = 1000;
                            } else {
                                if (!repair_orphaned_worker_marker(error))
                                    wait_ms = 1000;
                                signal_deadline_changed();
                            }
                        }
                    } else {
                        const std::uint64_t remaining = deadline - now;
                        const std::uint64_t max_wait =
                            static_cast<std::uint64_t>(MAXDWORD) - 1;
                        wait_ms = static_cast<DWORD>(
                            remaining > max_wait / 1000
                                ? max_wait : remaining * 1000);
                    }
                }
            } else if (!journal.activation_pending) {
                if (!repair_orphaned_worker_marker(error))
                    wait_ms = 1000;
            }
        }
        if (!error.empty())
            OutputDebugStringA(("admin broker reboot watchdog: " + error + "\n").c_str());
        const DWORD wait = WaitForMultipleObjects(
            static_cast<DWORD>(std::size(wait_handles)), wait_handles, FALSE, wait_ms);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_FAILED) {
            OutputDebugStringA((win_error("waiting for reboot deadline state",
                                          GetLastError()) + "\n").c_str());
            Sleep(1000);
        }
    }
}

bool rollback_broker_service_install(std::string& error) {
    ScHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager) return false;
    ScHandle service_handle(OpenServiceW(manager.value, kBrokerServiceNameW,
                                          DELETE | SERVICE_QUERY_STATUS |
                                              SERVICE_STOP));
    if (!service_handle) return false;
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    if (!QueryServiceStatusEx(service_handle.value, SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<BYTE*>(&status), sizeof(status),
                              &bytes))
        return false;
    if (status.dwCurrentState != SERVICE_STOPPED) {
        SERVICE_STATUS ignored{};
        if (!ControlService(service_handle.value, SERVICE_CONTROL_STOP, &ignored) &&
            GetLastError() != ERROR_SERVICE_NOT_ACTIVE)
            return false;
        if (!wait_service(service_handle.value, SERVICE_STOPPED, error)) return false;
    }
    return DeleteService(service_handle.value) != FALSE;
}

bool verify_broker_pipe_server(HANDLE pipe, std::string& error) {
    ULONG server_pid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &server_pid) || !server_pid) {
        error = win_error("identifying administration pipe server process",
                          GetLastError());
        return false;
    }
    fs::path image;
    if (!query_process_image(server_pid, image, error)) return false;
    fs::path root = broker_root_path(error);
    if (root.empty()) return false;
    if (!check_component_chain(root, true, error)) return false;
    const fs::path expected = root / kBrokerExecutableW;
    if (!equal_path(image, expected)) {
        error = "named-pipe server is not the protected administration broker image";
        return false;
    }
    return true;
}

Result connect_and_submit(const std::vector<std::uint8_t>& request) {
    if (request.empty() || request.size() > kMaxRequestBytes)
        return failure("admin broker request exceeds its fixed size limit");
    if (!WaitNamedPipeW(kPipeNameW, 5000))
        return failure(win_error("waiting for the LocalSystem administration broker", GetLastError()));
    Handle pipe(CreateFileW(kPipeNameW,
                            FILE_GENERIC_READ | FILE_WRITE_DATA |
                                FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES, 0,
                            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                            nullptr));
    if (!pipe)
        return failure(win_error("connecting to the LocalSystem administration broker", GetLastError()));
    std::string server_error;
    if (!verify_broker_pipe_server(pipe.value, server_error))
        return failure(server_error);
    std::array<std::uint8_t, 4> prefix{};
    const std::uint32_t size = static_cast<std::uint32_t>(request.size());
    for (int i = 0; i < 4; ++i)
        prefix[static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(size >> (24 - i * 8));
    if (!overlapped_io(pipe.value, prefix.data(), static_cast<DWORD>(prefix.size()),
                       false, kPipeTimeoutMs) ||
        !overlapped_io(pipe.value, const_cast<std::uint8_t*>(request.data()), size,
                       false, kPipeTimeoutMs))
        return failure(win_error("sending bounded request to administration broker", GetLastError()));
    if (!overlapped_io(pipe.value, prefix.data(), static_cast<DWORD>(prefix.size()),
                       true, kPipeTimeoutMs))
        return failure(win_error("reading administration broker response", GetLastError()));
    const std::uint32_t reply_size =
        (static_cast<std::uint32_t>(prefix[0]) << 24) |
        (static_cast<std::uint32_t>(prefix[1]) << 16) |
        (static_cast<std::uint32_t>(prefix[2]) << 8) | prefix[3];
    if (!reply_size || reply_size > 4096)
        return failure("administration broker returned an invalid response size");
    std::vector<std::uint8_t> reply(reply_size);
    if (!overlapped_io(pipe.value, reply.data(), reply_size, true, kPipeTimeoutMs))
        return failure(win_error("reading administration broker response", GetLastError()));
    Result result;
    if (!decode_reply(reply, result))
        return failure("administration broker response is malformed");
    if (result.success) {
        std::array<std::uint8_t, 3> ack{'A', 'C', 'K'};
        if (!overlapped_io(pipe.value, ack.data(),
                           static_cast<DWORD>(ack.size()), false,
                           kPipeTimeoutMs))
            return failure(win_error("acknowledging durable broker acceptance",
                                     GetLastError()));
    }
    return result;
}

Result connect_and_query(const std::string& intent_id,
                         IntentOutcome& outcome) {
    std::vector<std::uint8_t> request;
    if (!encode_query_request(intent_id, request))
        return failure("broker outcome query ID is invalid");
    if (!WaitNamedPipeW(kPipeNameW, 5000))
        return failure(win_error("waiting for broker outcome query pipe", GetLastError()));
    Handle pipe(CreateFileW(kPipeNameW,
                            FILE_GENERIC_READ | FILE_WRITE_DATA |
                                FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES, 0,
                            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    if (!pipe)
        return failure(win_error("connecting to broker outcome query pipe", GetLastError()));
    std::string server_error;
    if (!verify_broker_pipe_server(pipe.value, server_error))
        return failure(server_error);
    std::array<std::uint8_t, 4> prefix{};
    const std::uint32_t size = static_cast<std::uint32_t>(request.size());
    for (int i = 0; i < 4; ++i)
        prefix[static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(size >> (24 - i * 8));
    if (!overlapped_io(pipe.value, prefix.data(), 4, false, kPipeTimeoutMs) ||
        !overlapped_io(pipe.value, request.data(), size, false, kPipeTimeoutMs) ||
        !overlapped_io(pipe.value, prefix.data(), 4, true, kPipeTimeoutMs))
        return failure(win_error("exchanging bounded broker outcome query",
                                 GetLastError()));
    const std::uint32_t reply_size =
        (static_cast<std::uint32_t>(prefix[0]) << 24) |
        (static_cast<std::uint32_t>(prefix[1]) << 16) |
        (static_cast<std::uint32_t>(prefix[2]) << 8) | prefix[3];
    if (!reply_size || reply_size > 4096)
        return failure("broker returned an invalid outcome query size");
    std::vector<std::uint8_t> reply(reply_size);
    if (!overlapped_io(pipe.value, reply.data(), reply_size, true, kPipeTimeoutMs))
        return failure(win_error("reading broker intent outcome", GetLastError()));
    Result result;
    return decode_outcome_reply(reply, result, outcome)
        ? result : failure("broker intent outcome response is malformed");
}

Result connect_and_report_ready(const std::string& token,
                                const wire::Status& status) {
    std::vector<std::uint8_t> request;
    if (!encode_health_request(token, status, request))
        return failure("worker readiness report is invalid");
    if (!WaitNamedPipeW(kHealthPipeNameW, kActivationReadyTimeoutMs))
        return failure(win_error("waiting for broker worker-health pipe", GetLastError()));
    Handle pipe(CreateFileW(kHealthPipeNameW,
                            FILE_GENERIC_READ | FILE_WRITE_DATA |
                                FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES, 0,
                            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    if (!pipe)
        return failure(win_error("connecting to broker worker-health pipe", GetLastError()));
    std::string server_error;
    if (!verify_broker_pipe_server(pipe.value, server_error))
        return failure(server_error);
    std::array<std::uint8_t, 4> prefix{};
    const std::uint32_t size = static_cast<std::uint32_t>(request.size());
    for (int i = 0; i < 4; ++i)
        prefix[static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(size >> (24 - i * 8));
    if (!overlapped_io(pipe.value, prefix.data(), 4, false, kPipeTimeoutMs) ||
        !overlapped_io(pipe.value, request.data(), size, false, kPipeTimeoutMs) ||
        !overlapped_io(pipe.value, prefix.data(), 4, true, kPipeTimeoutMs))
        return failure(win_error("exchanging worker readiness with broker",
                                 GetLastError()));
    const std::uint32_t reply_size =
        (static_cast<std::uint32_t>(prefix[0]) << 24) |
        (static_cast<std::uint32_t>(prefix[1]) << 16) |
        (static_cast<std::uint32_t>(prefix[2]) << 8) | prefix[3];
    if (!reply_size || reply_size > 4096)
        return failure("broker returned an invalid worker readiness response size");
    std::vector<std::uint8_t> reply(reply_size);
    if (!overlapped_io(pipe.value, reply.data(), reply_size, true, kPipeTimeoutMs))
        return failure(win_error("reading broker worker readiness response",
                                 GetLastError()));
    Result result;
    return decode_reply(reply, result) ? result
        : failure("broker worker readiness response is malformed");
}
#endif  // _WIN32

}  // namespace

Result install_broker(const TargetBinding& binding) {
#ifndef _WIN32
    (void)binding;
    return failure("the privileged administration broker is supported only on Windows");
#else
    if (!admin_elevated())
        return failure("installing the administration broker requires an elevated local administrator");
    if (!valid_hex64(binding.worker_id) || !valid_hex64(binding.leader_id) ||
        !binding.leader_epoch || binding.leader_epoch > kMaxSafeInteger ||
        !binding.initial_security_version ||
        binding.initial_security_version > kMaxSafeInteger)
        return failure("operator-supplied broker target and incumbent security version are invalid");
    Configuration worker_configuration;
    const Result verified = service::broker_worker_configuration(worker_configuration);
    if (!verified.success)
        return failure("cannot bind broker to an unverified worker service: " + verified.error);
    Config policy;
    try {
        policy = load_config(worker_configuration.config_root);
    } catch (const std::exception& exception) {
        return failure(std::string("cannot load admin-pinned worker policy: ") +
                       exception.what());
    } catch (...) {
        return failure("cannot load admin-pinned worker policy");
    }
    if (policy.leader_id != binding.leader_id)
        return failure("operator target leader does not match the admin-pinned worker policy");
    std::string error;
    auto pairing = matching_local_pairing(worker_configuration, binding, error);
    if (!pairing) return failure(error);
    Journal initial_journal;
    initial_journal.security_floor = binding.initial_security_version;
    std::string initial_digest;
    std::uint64_t initial_size = 0;
    if (!image_identity(worker_configuration.executable, initial_digest,
                        initial_size, error))
        return failure("cannot hash current fixed worker image: " + error);
    std::string installer_digest;
    std::uint64_t installer_size = 0;
    if (!image_identity(fs::u8path(app::exe_path()), installer_digest,
                        installer_size, error))
        return failure("cannot hash installing executable: " + error);
    if (installer_digest != initial_digest || installer_size != initial_size)
        return failure("installing executable and current worker image differ; broker cannot trust the worker build identity");
    initial_journal.active_package_sha256 = initial_digest;
    if (!wide_to_utf8(worker_configuration.executable.native().c_str(),
                      initial_journal.active_image))
        return failure("cannot encode current worker image path for broker journal");
    initial_journal.active_build = update::CurrentPlatformIdentity().build;
    if (initial_journal.active_build.empty())
        return failure("cannot determine current worker build for the broker version binding");

    fs::path root = broker_root_path(error);
    if (root.empty()) return failure(error);
    const fs::path app_root = root.parent_path();
    if (!create_protected_directory(app_root, false, error) ||
        !grant_worker_access(app_root, kWorkerTraverseAccess, error))
        return failure("unsafe Spirula machine-data parent: " + error);
    const DWORD root_attributes = GetFileAttributesW(root.c_str());
    const bool fresh_root = root_attributes == INVALID_FILE_ATTRIBUTES &&
                            GetLastError() == ERROR_FILE_NOT_FOUND;
    if (fresh_root) {
        if (!CreateDirectoryW(root.c_str(), nullptr))
            return failure(win_error("creating protected broker root", GetLastError()));
        if (!set_private_acl(root) ||
            !grant_worker_access(root, kWorkerTraverseAccess, error))
            return failure(error.empty() ? "cannot protect broker root" : error);
        if (!save_binding(root, binding, false, error) ||
            !save_journal(root, initial_journal, error))
            return failure(error);
    } else {
        bool valid_acl = check_exact_private_acl(root, error);
        if (!valid_acl) {
            error.clear();
            valid_acl = check_exact_private_acl(root, error,
                                                kWorkerTraverseAccess);
        }
        if (!check_component_chain(root, true, error) || !valid_acl ||
            !grant_worker_access(root, kWorkerTraverseAccess, error))
            return failure(error);
        TargetBinding existing;
        Journal journal;
        if (!load_binding(root, existing, error) ||
            !load_journal(root, journal, error))
            return failure("existing broker root is incomplete or corrupt; refusing to reset protected authority: " + error);
        if (existing.worker_id != binding.worker_id ||
            existing.leader_id != binding.leader_id ||
            existing.leader_epoch != binding.leader_epoch ||
            existing.initial_security_version != binding.initial_security_version)
            return failure("existing broker target grant differs; reinstall cannot silently rebind it");
        if (journal.activation_pending || journal.reboot_pending)
            return failure("cannot reinstall broker while a durable privileged transaction is pending");
        if (journal.active_image != initial_journal.active_image ||
            journal.active_package_sha256 != initial_journal.active_package_sha256 ||
            journal.active_build != initial_journal.active_build)
            return failure("current worker image differs from the broker's durable active version");
    }

    const fs::path executable = root / kBrokerExecutableW;
    const DWORD executable_attributes = GetFileAttributesW(executable.c_str());
    if (executable_attributes == INVALID_FILE_ATTRIBUTES) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND)
            return failure(win_error("checking protected broker executable", GetLastError()));
        if (equal_path(worker_configuration.executable, executable) ||
            !CopyFileW(worker_configuration.executable.c_str(), executable.c_str(), TRUE))
            return failure(win_error("copying worker executable to protected broker root", GetLastError()));
        if (!set_private_acl(executable) || !check_exact_private_acl(executable, error))
            return failure(error.empty() ? "cannot protect distinct broker executable" : error);
    } else if (!check_component_chain(executable, false, error) ||
               !check_exact_private_acl(executable, error)) {
        return failure(error);
    }
    Handle copied(CreateFileW(executable.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    Handle source(CreateFileW(worker_configuration.executable.c_str(), GENERIC_READ,
                              FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!copied || !source)
        return failure(win_error("verifying protected broker executable", GetLastError()));
    BY_HANDLE_FILE_INFORMATION copied_info{}, source_info{};
    if (!GetFileInformationByHandle(copied.value, &copied_info) ||
        !GetFileInformationByHandle(source.value, &source_info) ||
        copied_info.nNumberOfLinks != 1 || source_info.nNumberOfLinks != 1 ||
        (copied_info.dwVolumeSerialNumber == source_info.dwVolumeSerialNumber &&
         copied_info.nFileIndexHigh == source_info.nFileIndexHigh &&
         copied_info.nFileIndexLow == source_info.nFileIndexLow))
        return failure("broker executable is not a distinct single-link image");
    std::string broker_digest, source_digest;
    std::uint64_t broker_size = 0, source_size = 0;
    if (!image_identity(executable, broker_digest, broker_size, error) ||
        !image_identity(worker_configuration.executable, source_digest,
                        source_size, error) ||
        broker_digest != source_digest || source_digest != initial_digest ||
        broker_size != source_size || source_size != initial_size)
        return failure(error.empty()
            ? "protected broker executable is not an exact immutable source copy"
            : error);

    ScHandle manager(OpenSCManagerW(nullptr, nullptr,
                                    SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
    if (!manager)
        return failure(win_error("opening SCM to install administration broker", GetLastError()));
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;0x000F01FF;;;SY)(A;;0x000F01FF;;;BA)",
            SDDL_REVISION_1, &descriptor, nullptr))
        return failure(win_error("building protected broker service DACL", GetLastError()));
    const std::wstring image = quote_arg(executable.native()) +
                               L" agent broker-service";
    SC_HANDLE raw_service = CreateServiceW(
        manager.value, kBrokerServiceNameW, L"Spirula Remote Administration Broker",
        SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_START |
            SERVICE_STOP | DELETE | SERVICE_CHANGE_CONFIG |
            READ_CONTROL | WRITE_DAC,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        image.c_str(), nullptr, nullptr, nullptr, L"LocalSystem", nullptr);
    const DWORD create_error = raw_service ? ERROR_SUCCESS : GetLastError();
    if (!raw_service) {
        LocalFree(descriptor);
        return failure(create_error == ERROR_SERVICE_EXISTS ||
                       create_error == ERROR_DUPLICATE_SERVICE_NAME
            ? "administration broker service already exists; refusing to replace it"
            : win_error("installing protected LocalSystem broker service", create_error));
    }
    ScHandle broker_service(raw_service);
    if (!SetServiceObjectSecurity(
            broker_service.value,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            descriptor)) {
        const DWORD code = GetLastError();
        LocalFree(descriptor);
        std::string ignored;
        rollback_broker_service_install(ignored);
        return failure(win_error("protecting administration broker SCM permissions",
                                 code));
    }
    LocalFree(descriptor);
    if (!service_dacl_is_private(broker_service.value, error)) {
        std::string ignored;
        rollback_broker_service_install(ignored);
        return failure(error);
    }
    const Result worker_acl = service::protect_worker_service_control();
    if (!worker_acl.success) {
        std::string ignored;
        rollback_broker_service_install(ignored);
        return failure("cannot prove worker lacks SCM control rights: " + worker_acl.error);
    }
    return success();
#endif
}

Result uninstall_broker() {
#ifndef _WIN32
    return failure("the privileged administration broker is supported only on Windows");
#else
    if (!admin_elevated())
        return failure("uninstalling the administration broker requires an elevated local administrator");
    ScHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager)
        return failure(win_error("opening SCM to uninstall administration broker", GetLastError()));
    SC_HANDLE raw = OpenServiceW(manager.value, kBrokerServiceNameW,
                                 SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS |
                                     SERVICE_START | SERVICE_STOP | DELETE | READ_CONTROL);
    if (!raw) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) return success();
        return failure(win_error("opening the fixed administration broker service", GetLastError()));
    }
    ScHandle service_handle(raw);
    std::string error;
    fs::path image;
    SC_HANDLE checked_raw = nullptr;
    if (!query_broker_service(manager.value, checked_raw, image, error))
        return failure(error);
    ScHandle checked_service(checked_raw);
    Configuration worker_configuration;
    const Result worker = service::broker_worker_configuration(worker_configuration);
    fs::path root = broker_root_path(error);
    if (!worker.success || root.empty() ||
        !service::verify_broker_worker_control().success ||
        !equal_path(image, root / kBrokerExecutableW) ||
        equal_path(image, worker_configuration.executable) ||
        !check_safe_parents(root, error) ||
        !check_exact_private_acl(root, error, kWorkerTraverseAccess) ||
        !check_exact_private_acl(image, error))
        return failure("refusing to remove a broker whose protected identity cannot be verified" +
                       (error.empty() ? std::string{} : ": " + error));
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    if (!QueryServiceStatusEx(service_handle.value, SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<BYTE*>(&status), sizeof(status),
                              &bytes))
        return failure(win_error("querying administration broker state", GetLastError()));
    Journal journal;
    if (!load_journal(root, journal, error) ||
        journal.activation_pending || journal.reboot_pending)
        return failure(error.empty()
            ? "cannot uninstall broker while a privileged transaction is pending"
            : error);
    if (status.dwCurrentState != SERVICE_STOPPED) {
        SERVICE_STATUS ignored{};
        if (!ControlService(service_handle.value, SERVICE_CONTROL_STOP, &ignored) &&
            GetLastError() != ERROR_SERVICE_NOT_ACTIVE)
            return failure(win_error("stopping administration broker", GetLastError()));
        if (!wait_service(service_handle.value, SERVICE_STOPPED, error))
            return failure(error);
    }
    if (!load_journal(root, journal, error) ||
        journal.activation_pending || journal.reboot_pending) {
        if (status.dwCurrentState != SERVICE_STOPPED &&
            !StartServiceW(service_handle.value, 0, nullptr))
            return failure("broker uninstall refused a pending transaction, but restoring the stopped broker failed: " +
                           win_error("starting administration broker", GetLastError()));
        return failure(error.empty()
            ? "cannot uninstall broker while a privileged transaction is pending"
            : error);
    }
    if (!DeleteService(service_handle.value))
        return failure(win_error("removing administration broker service", GetLastError()));
    // Keep the protected replay journal, version floor, binding, and executable.
    return success();
#endif
}

Result submit_intent(const Intent& intent, const update::PackageOffer* offer) {
#ifndef _WIN32
    (void)intent;
    (void)offer;
    return failure("the privileged administration broker is supported only on Windows");
#else
    std::vector<std::uint8_t> request;
    if (!encode_request(intent, offer, request))
        return failure("admin intent request is invalid or exceeds its fixed IPC bounds");
    return connect_and_submit(request);
#endif
}
Result query_intent_outcome(const std::string& intent_id,
                            IntentOutcome& outcome) {
#ifndef _WIN32
    (void)intent_id;
    (void)outcome;
    return failure("the privileged administration broker is supported only on Windows");
#else
    return connect_and_query(intent_id, outcome);
#endif
}

Result report_worker_ready(const std::string& activation_id,
                           const wire::Status& status) {
#ifndef _WIN32
    (void)activation_id;
    (void)status;
    return failure("the privileged administration broker is supported only on Windows");
#else
    return connect_and_report_ready(activation_id, status);
#endif
}

int run_broker_service() {
#ifndef _WIN32
    std::fputs("the privileged administration broker is supported only on Windows\n",
               stderr);
    return 1;
#else
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(kBrokerServiceNameW), broker_service_entry},
        {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
        OutputDebugStringA(("admin broker must run as its explicitly installed LocalSystem service: " +
                            win_error("StartServiceCtrlDispatcherW", GetLastError()) + "\n").c_str());
        return 1;
    }
    return 0;
#endif
}

}  // namespace app::agent::admin
