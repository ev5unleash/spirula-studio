#include "app/AgentConfig.h"

#include "data/JsonWrite.h"
#include "data/Json.h"

#include <atomic>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace app::agent {
namespace {

namespace fs = std::filesystem;

constexpr char kPolicyFilename[] = "agent-policy.json";
constexpr std::size_t kMaxPolicyBytes = 64 * 1024;
constexpr std::size_t kMaxAllowedDevices = 64;
constexpr std::uint64_t kMaxConcurrentJobs = 64;
// JsonValue stores numbers as doubles; stay within its exact-integer range.
constexpr std::uint64_t kMaxDiskBudgetBytes = 9007199254740991ULL;

[[noreturn]] void fail(const char* message) {
    throw std::runtime_error(std::string("agent config: ") + message);
}

const JsonValue& required(const JsonValue& object, const char* name) {
    const JsonValue* value = object.find(name);
    if (!value) fail("missing field");
    return *value;
}

void require_keys(const JsonValue& value,
                  std::initializer_list<const char*> allowed,
                  std::initializer_list<const char*> required_keys) {
    if (!value.is_object()) fail("expected object");
    for (std::size_t i = 0; i < value.obj.size(); ++i) {
        const std::string& key = value.obj[i].first;
        for (std::size_t j = 0; j < i; ++j)
            if (value.obj[j].first == key) fail("duplicate field");
        bool known = false;
        for (const char* name : allowed)
            if (key == name) known = true;
        if (!known) fail("unknown field");
    }
    for (const char* name : required_keys)
        if (!value.has(name)) fail("missing field");
}

std::uint64_t integer_value(const JsonValue& value, std::uint64_t minimum,
                            std::uint64_t maximum) {
    if (value.type != JsonValue::Type::Number || !std::isfinite(value.num) ||
        std::trunc(value.num) != value.num || value.num < static_cast<double>(minimum) ||
        value.num > static_cast<double>(maximum))
        fail("numeric field is out of range");
    return static_cast<std::uint64_t>(value.num);
}

std::string string_value(const JsonValue& value) {
    if (value.type != JsonValue::Type::String) fail("string field has the wrong type");
    return value.str;
}

bool parse_private_ipv4(std::string_view text) {
    std::array<unsigned, 4> octets{};
    std::size_t start = 0;
    for (std::size_t i = 0; i < octets.size(); ++i) {
        const std::size_t end = text.find('.', start);
        if ((i < 3 && end == std::string_view::npos) ||
            (i == 3 && end != std::string_view::npos))
            return false;
        const std::size_t stop = end == std::string_view::npos ? text.size() : end;
        if (stop == start || stop - start > 3 ||
            (stop - start > 1 && text[start] == '0'))
            return false;
        unsigned value = 0;
        for (std::size_t j = start; j < stop; ++j) {
            if (text[j] < '0' || text[j] > '9') return false;
            value = value * 10 + static_cast<unsigned>(text[j] - '0');
        }
        if (value > 255) return false;
        octets[i] = value;
        start = stop + 1;
    }
    const unsigned a = octets[0], b = octets[1];
    return a == 10 || a == 127 || (a == 172 && b >= 16 && b <= 31) ||
           (a == 192 && b == 168) || (a == 169 && b == 254);
}

bool append_ipv6_side(std::string_view side, std::vector<std::uint16_t>& groups) {
    if (side.empty()) return true;
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = side.find(':', start);
        const std::size_t stop = end == std::string_view::npos ? side.size() : end;
        if (stop == start || stop - start > 4) return false;
        unsigned value = 0;
        for (std::size_t i = start; i < stop; ++i) {
            const char c = side[i];
            unsigned digit;
            if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
            else return false;
            value = (value << 4) | digit;
        }
        groups.push_back(static_cast<std::uint16_t>(value));
        if (end == std::string_view::npos) return true;
        start = end + 1;
    }
}

bool parse_private_ipv6(std::string_view text) {
    const std::size_t compression = text.find("::");
    const bool compressed = compression != std::string_view::npos;
    if (compressed && text.find("::", compression + 2) != std::string_view::npos)
        return false;
    const std::string_view left = compressed ? text.substr(0, compression) : text;
    const std::string_view right = compressed ? text.substr(compression + 2)
                                               : std::string_view{};
    std::vector<std::uint16_t> before, after;
    if (!append_ipv6_side(left, before) || !append_ipv6_side(right, after)) return false;
    if (before.size() + after.size() > 8 ||
        (compressed ? before.size() + after.size() >= 8
                    : before.size() + after.size() != 8))
        return false;
    std::array<std::uint16_t, 8> groups{};
    std::copy(before.begin(), before.end(), groups.begin());
    std::copy(after.begin(), after.end(), groups.end() - after.size());
    const std::uint16_t first = groups[0];
    const bool private_range =
        (first & 0xfe00u) == 0xfc00u || (first & 0xffc0u) == 0xfe80u;
    const bool loopback = groups[0] == 0 && groups[1] == 0 && groups[2] == 0 &&
                          groups[3] == 0 && groups[4] == 0 && groups[5] == 0 &&
                          groups[6] == 0 && groups[7] == 1;
    return private_range || loopback;
}

bool valid_leader_address(const std::string& address) {
    if (address.empty() || address.size() > 45) return false;
    bool numeric_dotted = true;
    for (unsigned char c : address)
        if (!((c >= '0' && c <= '9') || c == '.')) numeric_dotted = false;
    if (numeric_dotted) return parse_private_ipv4(address);
    return address.find(':') != std::string::npos && parse_private_ipv6(address);
}

bool valid_dns_name(const std::string& name) {
    if (name.empty() || name.size() > 253) return false;
    bool has_letter = false;
    std::size_t label_start = 0;
    for (std::size_t i = 0; i <= name.size(); ++i) {
        if (i == name.size() || name[i] == '.') {
            const std::size_t length = i - label_start;
            if (length == 0 || length > 63 || name[label_start] == '-' ||
                name[i - 1] == '-')
                return false;
            label_start = i + 1;
            continue;
        }
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            has_letter = true;
        } else if (!((c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return has_letter;
}

bool canonical_uuid(const std::string& value) {
    if (value.size() != 37 || value.compare(0, 5, "uuid:") != 0) return false;
    bool nonzero = false;
    for (std::size_t i = 5; i < value.size(); ++i) {
        const char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        if (c != '0') nonzero = true;
    }
    return nonzero;
}
bool canonical_sha256(const std::string& value) {
    if (value.size() != 64) return false;
    for (char c : value)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}


Config parse_policy(const std::string& text) {
    JsonValue root;
    try {
        root = json_parse(text);
    } catch (const std::runtime_error&) {
        fail("invalid JSON");
    }

    require_keys(root,
                 {"schema_version", "leader", "allowed_vulkan_uuids",
                  "max_concurrent_jobs", "disk_budget_bytes", "grants",
                  "update_signer_sha256"},
                 {"schema_version", "leader", "allowed_vulkan_uuids",
                  "max_concurrent_jobs", "disk_budget_bytes"});
    if (integer_value(required(root, "schema_version"), 1,
                      kMaxDiskBudgetBytes) != 1)
        fail("unsupported schema version");

    const JsonValue& leader = required(root, "leader");
    require_keys(leader, {"address", "server_name", "port", "id"},
                 {"address", "server_name", "port"});
    Config config;
    config.leader_address = string_value(required(leader, "address"));
    if (!valid_leader_address(config.leader_address))
        fail("leader address must be a private numeric address");
    config.leader_server_name = string_value(required(leader, "server_name"));
    if (!valid_dns_name(config.leader_server_name))
        fail("leader server name is not a DNS name");
    config.leader_port = static_cast<std::uint16_t>(
        integer_value(required(leader, "port"), 1, 65535));
    if (const JsonValue* id = leader.find("id"))
        config.leader_id = string_value(*id);
    if (!config.leader_id.empty() && !canonical_sha256(config.leader_id))
        fail("leader identity must be a lowercase SHA-256 fingerprint");

    const JsonValue& devices = required(root, "allowed_vulkan_uuids");
    if (!devices.is_array() || devices.arr.empty() ||
        devices.arr.size() > kMaxAllowedDevices)
        fail("allowed Vulkan UUID list is out of range");
    config.allowed_vulkan_uuids.reserve(devices.arr.size());
    for (const JsonValue& item : devices.arr) {
        const std::string uuid = string_value(item);
        if (!canonical_uuid(uuid)) fail("allowed Vulkan UUID is not canonical");
        for (const std::string& previous : config.allowed_vulkan_uuids)
            if (previous == uuid) fail("duplicate Vulkan UUID");
        config.allowed_vulkan_uuids.push_back(uuid);
    }

    config.max_concurrent_jobs = static_cast<std::uint32_t>(
        integer_value(required(root, "max_concurrent_jobs"), 1,
                      kMaxConcurrentJobs));
    config.disk_budget_bytes = integer_value(required(root, "disk_budget_bytes"),
                                              1, kMaxDiskBudgetBytes);

    if (const JsonValue* grants = root.find("grants")) {
        require_keys(*grants, {"remote_update", "reboot"}, {});
        if (const JsonValue* grant = grants->find("remote_update")) {
            if (grant->type != JsonValue::Type::Bool)
                fail("remote_update grant must be boolean");
            config.allow_remote_update = grant->b;
        }
        if (const JsonValue* grant = grants->find("reboot")) {
            if (grant->type != JsonValue::Type::Bool)
                fail("reboot grant must be boolean");
            config.allow_reboot = grant->b;
        }
    }
    if (const JsonValue* pin = root.find("update_signer_sha256"))
        config.update_signer_sha256 = string_value(*pin);
    if (!config.update_signer_sha256.empty() &&
        !canonical_sha256(config.update_signer_sha256))
        fail("update signer pin must be a lowercase SHA-256 fingerprint");
    if (config.allow_remote_update &&
        (config.leader_id.empty() || config.update_signer_sha256.empty()))
        fail("remote updates require an approved leader and update signer pin");
    if (config.allow_remote_update &&
        config.leader_id == config.update_signer_sha256)
        fail("update signer pin must be independent of the leader identity");
    return config;
}

std::string serialize_policy(const Config& config) {
    JsonWriter writer;
    writer.object()
        .field("schema_version", 1)
        .key("leader").object()
            .field("address", config.leader_address)
            .field("server_name", config.leader_server_name)
            .field("port", static_cast<int>(config.leader_port))
            .field("id", config.leader_id)
        .end()
        .key("allowed_vulkan_uuids").array();
    for (const std::string& uuid : config.allowed_vulkan_uuids)
        writer.value(uuid);
    writer.end()
        .field_raw("max_concurrent_jobs",
                   std::to_string(config.max_concurrent_jobs))
        .field_raw("disk_budget_bytes",
                   std::to_string(config.disk_budget_bytes))
        .field("update_signer_sha256", config.update_signer_sha256)
        .key("grants").object()
            .field("remote_update", config.allow_remote_update)
            .field("reboot", config.allow_reboot)
        .end()
    .end();
    std::string text = writer.str();
    if (text.size() > kMaxPolicyBytes) fail("policy file is too large");
    (void)parse_policy(text);
    return text;
}

#ifndef _WIN32
void require_safe_posix_owner_and_mode(const struct stat& info) {
    if ((info.st_uid != ::geteuid() && info.st_uid != 0) ||
        (info.st_mode & (S_IWGRP | S_IWOTH)) != 0)
        fail("policy permissions are unsafe");
}
#endif

#ifdef _WIN32
struct NativeHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~NativeHandle() {
        if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value);
    }
    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;
    NativeHandle() = default;
};

struct LocalSid {
    PSID value = nullptr;
    ~LocalSid() { if (value) FreeSid(value); }
};

bool elevated_local_administrator() {
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token))
        return false;
    NativeHandle token;
    token.value = raw_token;
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    if (!GetTokenInformation(token.value, TokenElevation, &elevation,
                             sizeof(elevation), &returned) ||
        !elevation.TokenIsElevated)
        return false;
    std::array<unsigned char, SECURITY_MAX_SID_SIZE> admins{};
    DWORD size = static_cast<DWORD>(admins.size());
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
                            admins.data(), &size))
        return false;
    BOOL member = FALSE;
    return CheckTokenMembership(nullptr, admins.data(), &member) && member;
}

struct PolicyFileSecurity {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL acl = nullptr;
    ~PolicyFileSecurity() {
        if (acl) LocalFree(acl);
        if (descriptor) LocalFree(descriptor);
    }

    bool initialize() {
        HANDLE raw_token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token))
            return false;
        NativeHandle token;
        token.value = raw_token;
        DWORD bytes = 0;
        GetTokenInformation(token.value, TokenUser, nullptr, 0, &bytes);
        if (bytes == 0) return false;
        std::vector<unsigned char> token_data(bytes);
        if (!GetTokenInformation(token.value, TokenUser, token_data.data(),
                                 bytes, &bytes))
            return false;
        PSID user = reinterpret_cast<TOKEN_USER*>(token_data.data())->User.Sid;

        SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
        LocalSid system, admins;
        if (!AllocateAndInitializeSid(&nt_authority, 1, SECURITY_LOCAL_SYSTEM_RID,
                                      0, 0, 0, 0, 0, 0, 0, &system.value) ||
            !AllocateAndInitializeSid(&nt_authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                      DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                      &admins.value))
            return false;

        PSID trustees[] = {user, system.value, admins.value};
        EXPLICIT_ACCESSW entries[3]{};
        for (std::size_t i = 0; i < 3; ++i) {
            entries[i].grfAccessPermissions = FILE_ALL_ACCESS;
            entries[i].grfAccessMode = SET_ACCESS;
            entries[i].grfInheritance = NO_INHERITANCE;
            entries[i].Trustee.TrusteeForm = TRUSTEE_IS_SID;
            entries[i].Trustee.TrusteeType =
                i == 2 ? TRUSTEE_IS_WELL_KNOWN_GROUP : TRUSTEE_IS_USER;
            entries[i].Trustee.ptstrName =
                reinterpret_cast<LPWSTR>(trustees[i]);
        }
        if (SetEntriesInAclW(3, entries, nullptr, &acl) != ERROR_SUCCESS)
            return false;
        descriptor = static_cast<PSECURITY_DESCRIPTOR>(
            LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH));
        return descriptor &&
               InitializeSecurityDescriptor(descriptor,
                                            SECURITY_DESCRIPTOR_REVISION) &&
               SetSecurityDescriptorDacl(descriptor, TRUE, acl, FALSE) &&
               SetSecurityDescriptorControl(descriptor, SE_DACL_PROTECTED,
                                            SE_DACL_PROTECTED);
    }
};

bool trusted_sid(PSID sid, PSID owner, PSID user, PSID system, PSID admins) {
    if (!sid || !IsValidSid(sid)) return false;
    return (owner && IsValidSid(owner) && EqualSid(sid, owner)) ||
           EqualSid(sid, user) || EqualSid(sid, system) || EqualSid(sid, admins);
}

void require_safe_windows_acl(HANDLE handle) {
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetSecurityInfo(handle, SE_FILE_OBJECT,
                        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                        &owner, nullptr, &dacl, nullptr, &descriptor) != ERROR_SUCCESS ||
        !descriptor || !owner || !dacl) {
        if (descriptor) LocalFree(descriptor);
        fail("policy permissions are unsafe");
    }

    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        LocalFree(descriptor);
        fail("policy permissions are unsafe");
    }
    NativeHandle token_handle;
    token_handle.value = token;
    DWORD token_bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &token_bytes);
    std::vector<unsigned char> token_data(token_bytes);
    if (token_bytes == 0 ||
        !GetTokenInformation(token, TokenUser, token_data.data(), token_bytes,
                             &token_bytes)) {
        LocalFree(descriptor);
        fail("policy permissions are unsafe");
    }
    PSID user = reinterpret_cast<TOKEN_USER*>(token_data.data())->User.Sid;

    SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
    LocalSid system_sid, admins_sid;
    if (!AllocateAndInitializeSid(&nt_authority, 1, SECURITY_LOCAL_SYSTEM_RID,
                                  0, 0, 0, 0, 0, 0, 0, &system_sid.value) ||
        !AllocateAndInitializeSid(&nt_authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                  &admins_sid.value) ||
        !trusted_sid(owner, nullptr, user, system_sid.value, admins_sid.value)) {
        LocalFree(descriptor);
        fail("policy permissions are unsafe");
    }

    ACL_SIZE_INFORMATION acl_info{};
    if (!GetAclInformation(dacl, &acl_info, sizeof(acl_info), AclSizeInformation)) {
        LocalFree(descriptor);
        fail("policy permissions are unsafe");
    }
    constexpr ACCESS_MASK kWriteRights =
        FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES |
        FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE |
        GENERIC_ALL;
    for (DWORD i = 0; i < acl_info.AceCount; ++i) {
        void* raw = nullptr;
        if (!GetAce(dacl, i, &raw)) {
            LocalFree(descriptor);
            fail("policy permissions are unsafe");
        }
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        if (header->AceFlags & INHERIT_ONLY_ACE) continue;
        ACCESS_MASK mask = 0;
        PSID trustee = nullptr;
        if (header->AceType == ACCESS_ALLOWED_ACE_TYPE ||
            header->AceType == ACCESS_ALLOWED_CALLBACK_ACE_TYPE) {
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
            mask = ace->Mask;
            trustee = const_cast<DWORD*>(&ace->SidStart);
        } else if (header->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE ||
                   header->AceType == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE) {
            const auto* ace = static_cast<const ACCESS_ALLOWED_OBJECT_ACE*>(raw);
            mask = ace->Mask;
            std::size_t sid_offset = offsetof(ACCESS_ALLOWED_OBJECT_ACE, ObjectType);
            if (ace->Flags & ACE_OBJECT_TYPE_PRESENT) sid_offset += sizeof(GUID);
            if (ace->Flags & ACE_INHERITED_OBJECT_TYPE_PRESENT) sid_offset += sizeof(GUID);
            if (sid_offset >= header->AceSize) {
                LocalFree(descriptor);
                fail("policy permissions are unsafe");
            }
            trustee = reinterpret_cast<BYTE*>(const_cast<ACE_HEADER*>(header)) + sid_offset;
        } else if (header->AceType == ACCESS_ALLOWED_COMPOUND_ACE_TYPE) {
            LocalFree(descriptor);
            fail("policy permissions are unsafe");
        } else {
            continue;
        }
        if ((mask & kWriteRights) != 0 &&
            !trusted_sid(trustee, owner, user, system_sid.value, admins_sid.value)) {
            LocalFree(descriptor);
            fail("policy permissions are unsafe");
        }
    }
    LocalFree(descriptor);
}

DWORD attributes_for(const fs::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) fail("config root is unsafe");
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) fail("config root is unsafe");
    return attributes;
}
#endif

void require_local_administrator() {
#ifdef _WIN32
    if (!elevated_local_administrator())
        fail("saving machine policy requires an elevated local administrator");
#else
    if (::geteuid() != 0)
        fail("saving machine policy requires local administrator privileges (root)");
#endif
}

void require_config_root(const fs::path& root) {
    if (root.empty() || !root.is_absolute() || root == root.root_path())
        fail("config root must be an absolute directory");
    for (const fs::path& component : root.relative_path())
        if (component == "." || component == "..")
            fail("config root is unsafe");

    fs::path current = root.root_path();
    for (const fs::path& component : root.relative_path()) {
        current /= component;
#ifdef _WIN32
        const DWORD attributes = attributes_for(current);
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) fail("config root is unsafe");
#else
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(current, ec);
        if (ec || status.type() == fs::file_type::not_found ||
            fs::is_symlink(status) || !fs::is_directory(status))
            fail("config root is unsafe");
#endif
    }

#ifdef _WIN32
    const DWORD attributes = attributes_for(root);
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) fail("config root is unsafe");
    NativeHandle directory;
    directory.value = CreateFileW(root.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                  nullptr);
    if (directory.value == INVALID_HANDLE_VALUE) fail("config root is unsafe");
    require_safe_windows_acl(directory.value);
#else
    struct stat info {};
    if (::stat(root.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
        fail("config root is unsafe");
    require_safe_posix_owner_and_mode(info);
#endif
}

std::string read_policy(const fs::path& root) {
    const fs::path path = root / kPolicyFilename;
#ifdef _WIN32
    NativeHandle file;
    file.value = CreateFileW(path.c_str(), GENERIC_READ | READ_CONTROL,
                             FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file.value == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            fail("policy file is missing");
        fail("policy file is unsafe or unreadable");
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.value, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT |
                                  FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
        info.nNumberOfLinks != 1)
        fail("policy file is unsafe");
    require_safe_windows_acl(file.value);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.value, &size) || size.QuadPart < 0)
        fail("policy file is unreadable");
    if (static_cast<std::uint64_t>(size.QuadPart) > kMaxPolicyBytes)
        fail("policy file is too large");
    std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t offset = 0;
    while (offset < text.size()) {
        DWORD got = 0;
        const DWORD request = static_cast<DWORD>(text.size() - offset);
        if (!ReadFile(file.value, text.data() + offset, request, &got, nullptr) || got == 0)
            fail("policy file changed while reading");
        offset += got;
    }
    return text;
#else
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory ||
        status.type() == fs::file_type::not_found)
        fail("policy file is missing");
    if (ec || fs::is_symlink(status) || !fs::is_regular_file(status))
        fail("policy file is unsafe");

    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int fd = ::open(path.c_str(), flags);
    if (fd < 0) {
        if (errno == ELOOP) fail("policy file is unsafe");
        fail("policy file is unreadable");
    }
    struct FdGuard {
        int fd;
        ~FdGuard() { if (fd >= 0) ::close(fd); }
    } guard{fd};
    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1)
        fail("policy file is unsafe");
    require_safe_posix_owner_and_mode(info);
    if (info.st_size < 0) fail("policy file is unreadable");
    if (static_cast<std::uint64_t>(info.st_size) > kMaxPolicyBytes)
        fail("policy file is too large");
    std::string text(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t offset = 0;
    while (offset < text.size()) {
        const ssize_t got = ::read(fd, text.data() + offset, text.size() - offset);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) fail("policy file changed while reading");
        offset += static_cast<std::size_t>(got);
    }
    char extra = 0;
    ssize_t got;
    do { got = ::read(fd, &extra, 1); } while (got < 0 && errno == EINTR);
    if (got != 0) fail("policy file changed while reading");
    return text;
#endif
}

#ifdef _WIN32
void require_safe_publish_target(const fs::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return;
        fail("policy target is unsafe or unreadable");
    }
    if (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
        fail("policy target is unsafe");

    NativeHandle file;
    file.value = CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT,
                             nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    if (file.value == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(file.value, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT |
                                  FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
        info.nNumberOfLinks != 1)
        fail("policy target is unsafe");
    require_safe_windows_acl(file.value);
}

void publish_policy(const fs::path& root, const std::string& text) {
    const fs::path target = root / kPolicyFilename;
    require_safe_publish_target(target);

    PolicyFileSecurity security;
    if (!security.initialize()) fail("could not restrict temporary policy permissions");
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),
                                  security.descriptor, FALSE};
    static std::atomic<std::uint64_t> sequence{0};
    fs::path temporary;
    NativeHandle file;
    bool created = false;
    bool published = false;
    try {
        for (int attempt = 0; attempt < 128; ++attempt) {
            temporary = root / (L"agent-policy.json.tmp-" +
                std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed)));
            file.value = CreateFileW(temporary.c_str(), GENERIC_WRITE | READ_CONTROL,
                                     0, &attributes, CREATE_NEW,
                                     FILE_ATTRIBUTE_NORMAL |
                                         FILE_FLAG_OPEN_REPARSE_POINT,
                                     nullptr);
            if (file.value != INVALID_HANDLE_VALUE) {
                created = true;
                break;
            }
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
                fail("could not create temporary policy file");
        }
        if (file.value == INVALID_HANDLE_VALUE)
            fail("could not allocate temporary policy file");

        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file.value, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT |
                                      FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
            info.nNumberOfLinks != 1)
            fail("temporary policy file is unsafe");
        require_safe_windows_acl(file.value);

        std::size_t offset = 0;
        while (offset < text.size()) {
            DWORD written = 0;
            const DWORD request = static_cast<DWORD>(text.size() - offset);
            if (!WriteFile(file.value, text.data() + offset, request, &written, nullptr) ||
                written == 0)
                fail("could not write temporary policy file");
            offset += written;
        }
        if (!FlushFileBuffers(file.value)) fail("could not flush temporary policy file");
        if (!CloseHandle(file.value)) {
            file.value = INVALID_HANDLE_VALUE;
            fail("could not close temporary policy file");
        }
        file.value = INVALID_HANDLE_VALUE;
        if (!MoveFileExW(temporary.c_str(), target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            fail("could not atomically replace policy file");
        published = true;
    } catch (...) {
        if (file.value != INVALID_HANDLE_VALUE) {
            CloseHandle(file.value);
            file.value = INVALID_HANDLE_VALUE;
        }
        if (created && !published) DeleteFileW(temporary.c_str());
        throw;
    }
}
#else
struct PosixFd {
    int value = -1;
    explicit PosixFd(int fd = -1) : value(fd) {}
    ~PosixFd() { if (value >= 0) ::close(value); }
    PosixFd(const PosixFd&) = delete;
    PosixFd& operator=(const PosixFd&) = delete;
};

int open_config_root(const fs::path& root) {
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int directory = ::open(root.root_path().c_str(), flags);
    if (directory < 0) fail("config root is unsafe");
    for (const fs::path& component : root.relative_path()) {
        const int next = ::openat(directory, component.c_str(), flags);
        const int saved_errno = errno;
        ::close(directory);
        if (next < 0) {
            errno = saved_errno;
            fail("config root is unsafe");
        }
        directory = next;
    }
    struct stat info {};
    if (::fstat(directory, &info) != 0 || !S_ISDIR(info.st_mode)) {
        ::close(directory);
        fail("config root is unsafe");
    }
    try {
        require_safe_posix_owner_and_mode(info);
    } catch (...) {
        ::close(directory);
        throw;
    }
    return directory;
}

void require_safe_publish_target(int directory) {
    struct stat entry {};
    if (::fstatat(directory, kPolicyFilename, &entry, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return;
        fail("policy target is unsafe or unreadable");
    }
    if (!S_ISREG(entry.st_mode) || entry.st_nlink != 1)
        fail("policy target is unsafe");

    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_NONBLOCK
    flags |= O_NONBLOCK;
#endif
    PosixFd file(::openat(directory, kPolicyFilename, flags));
    struct stat info {};
    if (file.value < 0 || ::fstat(file.value, &info) != 0 ||
        !S_ISREG(info.st_mode) || info.st_nlink != 1 ||
        info.st_dev != entry.st_dev || info.st_ino != entry.st_ino)
        fail("policy target is unsafe");
    require_safe_posix_owner_and_mode(info);
}

void publish_policy(const fs::path& root, const std::string& text) {
    PosixFd directory(open_config_root(root));
    require_safe_publish_target(directory.value);

    static std::atomic<std::uint64_t> sequence{0};
    std::string temporary;
    PosixFd file;
    bool created = false;
    bool published = false;
    try {
        int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        for (int attempt = 0; attempt < 128; ++attempt) {
            temporary = std::string(kPolicyFilename) + ".tmp-" +
                std::to_string(static_cast<unsigned long>(::getpid())) + "-" +
                std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
            file.value = ::openat(directory.value, temporary.c_str(), flags, 0600);
            if (file.value >= 0) {
                created = true;
                break;
            }
            if (errno != EEXIST) fail("could not create temporary policy file");
        }
        if (file.value < 0) fail("could not allocate temporary policy file");
        if (::fchmod(file.value, 0600) != 0)
            fail("could not restrict temporary policy permissions");
        struct stat info {};
        if (::fstat(file.value, &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_nlink != 1 || (info.st_mode & 0077) != 0)
            fail("temporary policy file is unsafe");

        std::size_t offset = 0;
        while (offset < text.size()) {
            const ssize_t written = ::write(file.value, text.data() + offset,
                                            text.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) fail("could not write temporary policy file");
            offset += static_cast<std::size_t>(written);
        }
        if (::fsync(file.value) != 0) fail("could not flush temporary policy file");
        const int raw_file = file.value;
        file.value = -1;
        if (::close(raw_file) != 0) fail("could not close temporary policy file");
        if (::renameat(directory.value, temporary.c_str(),
                       directory.value, kPolicyFilename) != 0)
            fail("could not atomically replace policy file");
        published = true;
        if (::fsync(directory.value) != 0)
            fail("could not flush policy directory");
    } catch (...) {
        if (file.value >= 0) {
            ::close(file.value);
            file.value = -1;
        }
        if (created && !published)
            ::unlinkat(directory.value, temporary.c_str(), 0);
        throw;
    }
}
#endif

}  // namespace

Config load_config(const std::filesystem::path& config_root) {
    require_config_root(config_root);
    return parse_policy(read_policy(config_root));
}

void save_config(const std::filesystem::path& config_root, const Config& config) {
    require_local_administrator();
    require_config_root(config_root);
    publish_policy(config_root, serialize_policy(config));
}

}  // namespace app::agent
