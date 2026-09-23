#include "app/AgentServiceManager.h"

#include "app/Subprocess.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <cwchar>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <cerrno>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace app::agent::service {
namespace {

[[maybe_unused]] constexpr char kUnitName[] = "spirula-remote-worker.service";
[[maybe_unused]] constexpr char kLaunchdLabel[] = "com.spirula.remote-worker";
[[maybe_unused]] constexpr char kServiceUser[] = "spirula-worker";
[[maybe_unused]] constexpr char kServiceGroup[] = "spirula-worker";
[[maybe_unused]] constexpr std::size_t kMaxDefinitionBytes = 64 * 1024;

Result success() { return {true, {}}; }
Result failure(std::string message) { return {false, std::move(message)}; }
Status status_failure(std::string message) {
    Status result;
    result.error = std::move(message);
    return result;
}

bool has_dot_component(const fs::path& path) {
    for (const fs::path& part : path)
        if (part == "." || part == "..") return true;
    return false;
}

[[maybe_unused]] bool has_control(const std::string& value) {
    for (unsigned char c : value)
        if (c < 0x20 || c == 0x7f) return true;
    return false;
}

[[maybe_unused]] bool path_prefix(const fs::path& parent, const fs::path& child) {
    auto p = parent.begin();
    auto c = child.begin();
    for (; p != parent.end() && c != child.end(); ++p, ++c)
        if (*p != *c) return false;
    return p == parent.end();
}

[[maybe_unused]] bool roots_disjoint(const Configuration& config, std::string& error) {
    const fs::path* roots[] = {&config.config_root, &config.state_root,
                               &config.storage_root};
    for (int i = 0; i != 3; ++i) {
        for (int j = i + 1; j != 3; ++j) {
            if (path_prefix(*roots[i], *roots[j]) ||
                path_prefix(*roots[j], *roots[i])) {
                error = "config, state, and storage roots must be distinct and non-overlapping";
                return false;
            }
        }
    }
    const fs::path executable_dir = config.executable.parent_path();
    for (const fs::path* root : roots) {
        if (path_prefix(executable_dir, *root) ||
            path_prefix(*root, executable_dir)) {
            error = "service roots must not overlap the executable directory";
            return false;
        }
    }
    return true;
}

#ifdef _WIN32

constexpr wchar_t kServiceName[] = L"SpirulaRemoteWorker";
constexpr wchar_t kServiceAccount[] = L"NT SERVICE\\SpirulaRemoteWorker";
constexpr wchar_t kTrustedInstaller[] = L"NT SERVICE\\TrustedInstaller";

bool same_path(const fs::path& left, const fs::path& right) {
    std::wstring a = left.native();
    std::wstring b = right.native();
    while (a.size() > 3 && (a.back() == L'\\' || a.back() == L'/')) a.pop_back();
    while (b.size() > 3 && (b.back() == L'\\' || b.back() == L'/')) b.pop_back();
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool path_prefix_win(const fs::path& parent, const fs::path& child) {
    auto p = parent.begin();
    auto c = child.begin();
    for (; p != parent.end() && c != child.end(); ++p, ++c) {
        const std::wstring a = p->native();
        const std::wstring b = c->native();
        if (CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                 b.data(), static_cast<int>(b.size()), TRUE) !=
            CSTR_EQUAL)
            return false;
    }
    return p == parent.end();
}

bool win_roots_disjoint(const Configuration& config, std::string& error) {
    const fs::path* roots[] = {&config.config_root, &config.state_root,
                               &config.storage_root};
    for (int i = 0; i != 3; ++i) {
        for (int j = i + 1; j != 3; ++j) {
            if (path_prefix_win(*roots[i], *roots[j]) ||
                path_prefix_win(*roots[j], *roots[i])) {
                error = "config, state, and storage roots must be distinct and non-overlapping";
                return false;
            }
        }
    }
    const fs::path executable_dir = config.executable.parent_path();
    for (const fs::path* root : roots) {
        if (path_prefix_win(executable_dir, *root) ||
            path_prefix_win(*root, executable_dir)) {
            error = "service roots must not overlap the executable directory";
            return false;
        }
    }
    return true;
}

std::vector<fs::path> path_components(const fs::path& path) {
    std::vector<fs::path> result;
    fs::path current = path.root_path();
    result.push_back(current);
    for (const fs::path& part : path.relative_path()) {
        current /= part;
        result.push_back(current);
    }
    return result;
}

bool lookup_sid(const wchar_t* name, std::vector<std::uint8_t>& storage) {
    DWORD sid_size = 0, domain_size = 0;
    SID_NAME_USE use{};
    LookupAccountNameW(nullptr, name, nullptr, &sid_size, nullptr,
                       &domain_size, &use);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || sid_size == 0) return false;
    storage.resize(sid_size);
    std::vector<wchar_t> domain(domain_size + 1);
    return LookupAccountNameW(nullptr, name, storage.data(), &sid_size,
                              domain.data(), &domain_size, &use) != FALSE;
}

bool trusted_owner(PSID owner) {
    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> sid{};
    DWORD size = static_cast<DWORD>(sid.size());
    if (CreateWellKnownSid(WinLocalSystemSid, nullptr, sid.data(), &size) &&
        EqualSid(owner, sid.data()))
        return true;
    size = static_cast<DWORD>(sid.size());
    if (CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, sid.data(),
                           &size) && EqualSid(owner, sid.data()))
        return true;
    std::vector<std::uint8_t> ti;
    return lookup_sid(kTrustedInstaller, ti) && EqualSid(owner, ti.data());
}

bool sid_is_trusted_writer(PSID sid) {
    return trusted_owner(sid);
}
bool is_nt_service_sid(PSID sid) {
    if (!sid || !IsValidSid(sid)) return false;
    const SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
    const SID_IDENTIFIER_AUTHORITY* authority = GetSidIdentifierAuthority(sid);
    const UCHAR* count = GetSidSubAuthorityCount(sid);
    return authority && count &&
           std::memcmp(authority, &nt_authority, sizeof(nt_authority)) == 0 &&
           *count >= 6 && *GetSidSubAuthority(sid, 0) == 80;
}

bool secure_acl(const fs::path& path, PSID service_sid,
                bool allow_service_write, std::string& error,
                bool allow_service_owner = false) {
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
        error = "cannot verify machine-owned Windows service path permissions";
        return false;
    }
    std::vector<std::uint8_t> service_account_sid;
    const bool have_service_account_sid =
        allow_service_owner && lookup_sid(kServiceAccount, service_account_sid);
    const bool service_account_owner =
        have_service_account_sid && EqualSid(owner, service_account_sid.data());
    if (!trusted_owner(owner) &&
        !(allow_service_owner &&
          ((service_sid && EqualSid(owner, service_sid)) ||
           service_account_owner))) {
        LocalFree(descriptor);
        error = "service executable and roots must be owned by SYSTEM, Administrators, or TrustedInstaller";
        return false;
    }

    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> world{}, users{}, auth{};
    DWORD n = static_cast<DWORD>(world.size());
    const bool have_world = CreateWellKnownSid(WinWorldSid, nullptr,
                                               world.data(), &n) != FALSE;
    n = static_cast<DWORD>(users.size());
    const bool have_users = CreateWellKnownSid(WinBuiltinUsersSid, nullptr,
                                               users.data(), &n) != FALSE;
    n = static_cast<DWORD>(auth.size());
    const bool have_auth = CreateWellKnownSid(WinAuthenticatedUserSid, nullptr,
                                              auth.data(), &n) != FALSE;
    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> creator_owner{}, creator_group{};
    n = static_cast<DWORD>(creator_owner.size());
    const bool have_creator_owner = CreateWellKnownSid(
        WinCreatorOwnerSid, nullptr, creator_owner.data(), &n) != FALSE;
    n = static_cast<DWORD>(creator_group.size());
    const bool have_creator_group = CreateWellKnownSid(
        WinCreatorGroupSid, nullptr, creator_group.data(), &n) != FALSE;
    constexpr ACCESS_MASK kWriteRights =
        FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_ADD_FILE |
        FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD | FILE_WRITE_ATTRIBUTES |
        FILE_WRITE_EA | DELETE | WRITE_DAC | WRITE_OWNER;
    GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE,
                            FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
    ACL_SIZE_INFORMATION info{};
    if (!GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation)) {
        LocalFree(descriptor);
        error = "cannot inspect Windows service path permissions";
        return false;
    }
    for (DWORD i = 0; i < info.AceCount; ++i) {
        void* raw = nullptr;
        if (!GetAce(dacl, i, &raw)) continue;
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
        if (header->AceType == ACCESS_DENIED_OBJECT_ACE_TYPE ||
            header->AceType == ACCESS_DENIED_CALLBACK_ACE_TYPE ||
            header->AceType == ACCESS_DENIED_CALLBACK_OBJECT_ACE_TYPE ||
            header->AceType == ACCESS_ALLOWED_COMPOUND_ACE_TYPE ||
            header->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE ||
            header->AceType == ACCESS_ALLOWED_CALLBACK_ACE_TYPE ||
            header->AceType == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE) {
            LocalFree(descriptor);
            error = "service path has an unsupported access rule";
            return false;
        }
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
        ACCESS_MASK rights = ace->Mask;
        MapGenericMask(&rights, &mapping);
        if ((rights & kWriteRights) == 0) continue;
        if ((header->AceFlags & INHERIT_ONLY_ACE) &&
            !(header->AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)))
            continue;
        PSID trustee = const_cast<DWORD*>(&ace->SidStart);
        const bool is_service = service_sid && EqualSid(trustee, service_sid);
        const bool is_service_account =
            have_service_account_sid &&
            EqualSid(trustee, service_account_sid.data());
        if (((is_service || is_service_account) && allow_service_write) ||
            (!service_sid && allow_service_write && is_nt_service_sid(trustee)))
            continue;
        const bool is_world = have_world && EqualSid(trustee, world.data());
        const bool is_users = have_users && EqualSid(trustee, users.data());
        const bool is_auth = have_auth && EqualSid(trustee, auth.data());
        const bool creator_placeholder =
            (have_creator_owner && EqualSid(trustee, creator_owner.data())) ||
            (have_creator_group && EqualSid(trustee, creator_group.data()));
        if (is_world || is_users || is_auth ||
            (!creator_placeholder && !sid_is_trusted_writer(trustee))) {
            LocalFree(descriptor);
            error = "service executable and roots must not be writable by ordinary users";
            return false;
        }
    }
    LocalFree(descriptor);
    return true;
}

bool win_absolute_canonical(const fs::path& path, bool directory,
                            const char* name, PSID service_sid,
                            bool allow_service_write, std::string& error) {
    if (path.empty() || !path.is_absolute() || path == path.root_path() ||
        has_dot_component(path) || path.lexically_normal() != path) {
        error = std::string(name) + " must be an absolute canonical non-root path";
        return false;
    }
    for (wchar_t c : path.native()) {
        if (c < 0x20 || c == 0x7f) {
            error = std::string(name) + " contains an unsupported control character";
            return false;
        }
    }
    const std::wstring root_name = path.root_name().native();
    if (root_name.size() != 2 || root_name[1] != L':' ||
        !((root_name[0] >= L'A' && root_name[0] <= L'Z') ||
          (root_name[0] >= L'a' && root_name[0] <= L'z'))) {
        error = std::string(name) + " must use a local drive-letter path";
        return false;
    }
    if (GetDriveTypeW(path.root_path().c_str()) == DRIVE_REMOTE) {
        error = std::string(name) + " must be on a local filesystem";
        return false;
    }
    std::error_code ec;
    const fs::path canonical = fs::canonical(path, ec);
    if (ec || !same_path(canonical, path)) {
        error = std::string(name) + " must already exist at its canonical path";
        return false;
    }

    const std::vector<fs::path> components = path_components(path);
    for (std::size_t i = 0; i < components.size(); ++i) {
        const DWORD attributes = GetFileAttributesW(components[i].c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            error = std::string(name) + " cannot be inspected";
            return false;
        }
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            error = std::string(name) + " must not contain symlinks or reparse points";
            return false;
        }
        const bool leaf = i + 1 == components.size();
        if ((!leaf || directory) && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            error = std::string(name) + " contains a non-directory path component";
            return false;
        }
        if (leaf && !directory && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            error = std::string(name) + " must be a regular executable file";
            return false;
        }
        const bool service_write_here = leaf && allow_service_write;
        if (!secure_acl(components[i], service_sid, service_write_here, error))
            return false;
    }
    if (!directory) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
            error = std::string(name) + " must be a regular executable file";
            return false;
        }
    }
    return true;
}

bool validate_definition_shape(const Configuration& config,
                               std::string& error) {
    if (_wcsicmp(config.executable.filename().c_str(), L"spirula.exe") != 0) {
        error = "service executable must be spirula.exe";
        return false;
    }
    const fs::path* paths[] = {
        &config.executable, &config.config_root, &config.state_root,
        &config.storage_root};
    for (const fs::path* path : paths) {
        if (path->empty() || !path->is_absolute() ||
            *path == path->root_path() || has_dot_component(*path) ||
            path->lexically_normal() != *path) {
            error = "service executable and roots must be absolute canonical non-root paths";
            return false;
        }
        const std::wstring root = path->root_name().native();
        if (root.size() != 2 || root[1] != L':' ||
            !((root[0] >= L'A' && root[0] <= L'Z') ||
              (root[0] >= L'a' && root[0] <= L'z'))) {
            error = "service executable and roots must use local drive-letter paths";
            return false;
        }
        for (wchar_t c : path->native())
            if (c < 0x20 || c == 0x7f) {
                error = "service executable and roots contain an unsupported control character";
                return false;
            }
    }
    return win_roots_disjoint(config, error);
}
bool validate_state_file_permissions(const fs::path& state_root, PSID sid,
                                     std::string& error);
bool validate_configuration(const Configuration& config, PSID service_sid,
                            std::string& error) {
    if (!validate_definition_shape(config, error)) return false;
    if (!win_absolute_canonical(config.executable, false, "executable",
                                service_sid, false, error) ||
        !win_absolute_canonical(config.config_root, true, "config root",
                                service_sid, false, error) ||
        !win_absolute_canonical(config.state_root, true, "state root",
                                service_sid, true, error) ||
        !win_absolute_canonical(config.storage_root, true, "storage root",
                                service_sid, true, error))
        return false;
    return validate_state_file_permissions(config.state_root, service_sid,
                                           error);
}

std::wstring quote_windows_arg(const std::wstring& arg) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
        } else if (c == L'\"') {
            result.append(slashes * 2 + 1, L'\\');
            result += L'\"';
            slashes = 0;
        } else {
            result.append(slashes, L'\\');
            slashes = 0;
            result += c;
        }
    }
    result.append(slashes * 2, L'\\');
    result += L'\"';
    return result;
}

std::wstring service_image(const Configuration& config) {
    const std::wstring args[] = {
        config.executable.native(), L"agent", L"service",
        L"--config-root", config.config_root.native(),
        L"--state-root", config.state_root.native(),
        L"--storage-root", config.storage_root.native()};
    std::wstring result;
    for (const std::wstring& arg : args) {
        if (!result.empty()) result += L' ';
        result += quote_windows_arg(arg);
    }
    return result;
}

bool split_windows_command_line(const std::wstring& command,
                               std::vector<std::wstring>& args) {
    std::size_t i = 0;
    while (i < command.size()) {
        while (i < command.size() && (command[i] == L' ' || command[i] == L'\t'))
            ++i;
        if (i == command.size()) break;
        std::wstring arg;
        bool quoted = false;
        while (i < command.size() && (quoted ||
               (command[i] != L' ' && command[i] != L'\t'))) {
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
                } else if (quoted && i + 1 < command.size() &&
                           command[i + 1] == L'\"') {
                    arg += L'\"';
                    i += 2;
                } else {
                    quoted = !quoted;
                    ++i;
                }
            } else {
                arg.append(slashes, L'\\');
                if (i < command.size() && (quoted ||
                    (command[i] != L' ' && command[i] != L'\t')))
                    arg += command[i++];
            }
        }
        if (quoted) return false;
        args.push_back(std::move(arg));
    }
    return true;
}

bool config_from_image(const std::wstring& image, Configuration& config,
                       std::string& error) {
    std::vector<std::wstring> args;
    if (!split_windows_command_line(image, args) ||
        (args.size() != 9 && args.size() != 11) ||
        args[1] != L"agent" || args[2] != L"service" ||
        args[3] != L"--config-root" || args[5] != L"--state-root" ||
        args[7] != L"--storage-root" ||
        (args.size() == 11 &&
         (args[9] != L"--broker-activation" || args[10].size() != 64))) {
        error = "existing service command is not the Spirula service contract";
        return false;
    }
    if (args.size() == 11)
        for (wchar_t ch : args[10])
            if (!((ch >= L'0' && ch <= L'9') ||
                  (ch >= L'a' && ch <= L'f'))) {
                error = "service activation token is malformed";
                return false;
            }
    config.executable = args[0];
    config.config_root = args[4];
    config.state_root = args[6];
    config.storage_root = args[8];
    return true;
}

bool elevated_administrator() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const bool is_elevated = GetTokenInformation(
        token, TokenElevation, &elevation, sizeof(elevation), &returned) &&
        elevation.TokenIsElevated;
    CloseHandle(token);
    if (!is_elevated) return false;
    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> admins{};
    DWORD size = static_cast<DWORD>(admins.size());
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
                            admins.data(), &size))
        return false;
    BOOL member = FALSE;
    return CheckTokenMembership(nullptr, admins.data(), &member) && member;
}

Result require_admin() {
    return elevated_administrator()
        ? success()
        : failure("this service-management operation requires an elevated local administrator");
}

std::string windows_error(const char* operation, DWORD code) {
    if (code == ERROR_ACCESS_DENIED)
        return "service-management permission denied; run as an elevated local administrator";
    if (code == ERROR_SERVICE_LOGON_FAILED || code == ERROR_NO_SUCH_LOGON_SESSION)
        return "the restricted Windows service account is unavailable or cannot log on";
    return std::string(operation) + " failed (Windows error " +
           std::to_string(code) + ")";
}

bool add_service_ace(const fs::path& path, PSID sid, ACCESS_MASK rights,
                     DWORD inheritance, std::string& error) {
    std::wstring name = path.native();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL old_acl = nullptr;
    const DWORD query = GetNamedSecurityInfoW(
        name.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
        nullptr, &old_acl, nullptr, &descriptor);
    if (query != ERROR_SUCCESS || !descriptor || !old_acl) {
        if (descriptor) LocalFree(descriptor);
        error = "cannot configure restricted service-account permissions";
        return false;
    }
    EXPLICIT_ACCESSW entry{};
    entry.grfAccessPermissions = rights;
    entry.grfAccessMode = GRANT_ACCESS;
    entry.grfInheritance = inheritance;
    BuildTrusteeWithSidW(&entry.Trustee, sid);
    PACL new_acl = nullptr;
    const DWORD built = SetEntriesInAclW(1, &entry, old_acl, &new_acl);
    LocalFree(descriptor);
    if (built != ERROR_SUCCESS || !new_acl) {
        if (new_acl) LocalFree(new_acl);
        error = "cannot configure restricted service-account permissions";
        return false;
    }
    const DWORD set = SetNamedSecurityInfoW(
        name.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
        nullptr, new_acl, nullptr);
    LocalFree(new_acl);
    if (set != ERROR_SUCCESS) {
        error = "cannot configure restricted service-account permissions";
        return false;
    }
    return true;
}

bool grant_service_tree(const fs::path& directory, PSID sid,
                        ACCESS_MASK leaf_rights, std::string& error) {
    const std::vector<fs::path> components = path_components(directory);
    for (std::size_t i = 1; i + 1 < components.size(); ++i) {
        if (!add_service_ace(components[i], sid,
                             FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                             NO_INHERITANCE, error))
            return false;
    }
    return add_service_ace(directory, sid, leaf_rights,
                           CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE, error);
}
bool grant_service_traverse_tree(const fs::path& directory, PSID sid,
                                 std::string& error) {
    const std::vector<fs::path> components = path_components(directory);
    for (std::size_t i = 1; i < components.size(); ++i)
        if (!add_service_ace(components[i], sid,
                             FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                             NO_INHERITANCE, error))
            return false;
    return true;
}

bool acl_has_rights(const fs::path& path, PSID sid, ACCESS_MASK required) {
    std::wstring name = path.native();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    if (GetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION, nullptr, nullptr,
                              &dacl, nullptr, &descriptor) != ERROR_SUCCESS ||
        !descriptor || !dacl) {
        if (descriptor) LocalFree(descriptor);
        return false;
    }
    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> world{}, users{}, auth{};
    DWORD size = static_cast<DWORD>(world.size());
    const bool have_world = CreateWellKnownSid(WinWorldSid, nullptr,
                                               world.data(), &size) != FALSE;
    size = static_cast<DWORD>(users.size());
    const bool have_users = CreateWellKnownSid(WinBuiltinUsersSid, nullptr,
                                               users.data(), &size) != FALSE;
    size = static_cast<DWORD>(auth.size());
    const bool have_auth = CreateWellKnownSid(WinAuthenticatedUserSid, nullptr,
                                              auth.data(), &size) != FALSE;
    GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE,
                            FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
    ACCESS_MASK granted = 0;
    ACL_SIZE_INFORMATION info{};
    bool okay = GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation) != FALSE;
    for (DWORD i = 0; okay && i < info.AceCount; ++i) {
        void* raw = nullptr;
        if (!GetAce(dacl, i, &raw)) continue;
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        if (header->AceFlags & INHERIT_ONLY_ACE) continue;
        if (header->AceType == ACCESS_DENIED_ACE_TYPE) {
            const auto* ace = static_cast<const ACCESS_DENIED_ACE*>(raw);
            PSID trustee = const_cast<DWORD*>(&ace->SidStart);
            ACCESS_MASK denied = ace->Mask;
            MapGenericMask(&denied, &mapping);
            const bool applies = EqualSid(trustee, sid) ||
                (have_world && EqualSid(trustee, world.data())) ||
                (have_users && EqualSid(trustee, users.data())) ||
                (have_auth && EqualSid(trustee, auth.data()));
            if (applies && (denied & required)) okay = false;
        } else if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
            if (EqualSid(const_cast<DWORD*>(&ace->SidStart), sid)) {
                ACCESS_MASK rights = ace->Mask;
                MapGenericMask(&rights, &mapping);
                granted |= rights;
            }
        }
    }
    LocalFree(descriptor);
    return okay && (granted & required) == required;
}
bool acl_has_inheritable_rights(const fs::path& path, PSID sid,
                                ACCESS_MASK required) {
    std::wstring name = path.native();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    if (GetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION, nullptr, nullptr,
                              &dacl, nullptr, &descriptor) != ERROR_SUCCESS ||
        !descriptor || !dacl) {
        if (descriptor) LocalFree(descriptor);
        return false;
    }
    ACL_SIZE_INFORMATION info{};
    bool accepted = GetAclInformation(dacl, &info, sizeof(info),
                                      AclSizeInformation) != FALSE;
    GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE,
                            FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
    for (DWORD i = 0; accepted && i < info.AceCount; ++i) {
        void* raw = nullptr;
        if (!GetAce(dacl, i, &raw)) { accepted = false; break; }
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE ||
            (header->AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) !=
                (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) continue;
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
        if (!EqualSid(const_cast<DWORD*>(&ace->SidStart), sid)) continue;
        ACCESS_MASK rights = ace->Mask;
        MapGenericMask(&rights, &mapping);
        if ((rights & required) == required) {
            LocalFree(descriptor);
            return true;
        }
    }
    LocalFree(descriptor);
    return false;
}

bool acl_tree_has_rights(const fs::path& path, PSID sid,
                         ACCESS_MASK leaf_rights) {
    const std::vector<fs::path> components = path_components(path);
    for (std::size_t i = 1; i + 1 < components.size(); ++i)
        if (!acl_has_rights(components[i], sid, FILE_TRAVERSE))
            return false;
    return acl_has_rights(path, sid, leaf_rights);
}
bool validate_state_file_permissions(const fs::path& state_root, PSID sid,
                                     std::string& error) {
    if (sid) {
        std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> admins{};
        DWORD size = static_cast<DWORD>(admins.size());
        if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
                                admins.data(), &size) ||
            !acl_has_rights(state_root, admins.data(), FILE_ALL_ACCESS) ||
            !acl_has_inheritable_rights(state_root, admins.data(),
                                        FILE_ALL_ACCESS)) {
            error = "state root must grant inheritable full access to local Administrators";
            return false;
        }
    }
    const fs::path state_file = state_root / L"agent-state.json";
    const DWORD attributes = GetFileAttributesW(state_file.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            return true;
        error = "cannot inspect the existing agent-state.json permissions";
        return false;
    }
    if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
        error = "agent-state.json must be a regular non-reparse file";
        return false;
    }
    std::error_code ec;
    const fs::path canonical = fs::canonical(state_file, ec);
    if (ec || !same_path(canonical, state_file)) {
        error = "agent-state.json must remain at its canonical path";
        return false;
    }
    if (!secure_acl(state_file, sid, true, error, true)) return false;
    constexpr ACCESS_MASK required = FILE_GENERIC_READ | FILE_GENERIC_WRITE | DELETE;
    if (sid && !acl_has_rights(state_file, sid, required)) {
        error = "existing agent-state.json does not grant the restricted service account the required access";
        return false;
    }
    if (sid) {
        std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> admins{};
        DWORD size = static_cast<DWORD>(admins.size());
        constexpr ACCESS_MASK access =
            FILE_GENERIC_READ | FILE_GENERIC_WRITE | DELETE;
        if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
                                admins.data(), &size) ||
            !acl_has_rights(state_file, admins.data(), access)) {
            error = "agent-state.json does not grant local Administrators access";
            return false;
        }
    }
    return true;
}

bool verify_service_permissions(const Configuration& config, PSID sid,
                                std::string& error) {
    constexpr ACCESS_MASK read_execute = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
    constexpr ACCESS_MASK write_tree = FILE_GENERIC_READ | FILE_GENERIC_WRITE |
                                       FILE_GENERIC_EXECUTE | DELETE |
                                       FILE_DELETE_CHILD;
    if (!acl_tree_has_rights(config.executable, sid, read_execute) ||
        !acl_tree_has_rights(config.config_root, sid, read_execute) ||
        !acl_tree_has_rights(config.state_root, sid, write_tree) ||
        !acl_tree_has_rights(config.storage_root, sid, write_tree)) {
        error = "preprovisioned service roots do not grant the restricted service account the required access";
        return false;
    }
    return true;
}

bool service_sid(std::vector<std::uint8_t>& sid, std::string& error) {
    if (!lookup_sid(kServiceAccount, sid)) {
        error = "the Spirula virtual service account is unavailable; reinstall through the local Service Control Manager";
        return false;
    }
    return true;
}

bool query_service_definition(SC_HANDLE service, Configuration& config,
                             DWORD& start_type,
                             std::vector<std::uint8_t>& sid,
                             std::string& error) {
    DWORD needed = 0;
    QueryServiceConfigW(service, nullptr, 0, &needed);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed == 0) {
        error = windows_error("reading service configuration", GetLastError());
        return false;
    }
    std::vector<std::uint8_t> buffer(needed);
    auto* info = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    if (!QueryServiceConfigW(service, info, needed, &needed)) {
        error = windows_error("reading service configuration", GetLastError());
        return false;
    }
    if (info->dwServiceType != SERVICE_WIN32_OWN_PROCESS ||
        !info->lpServiceStartName ||
        _wcsicmp(info->lpServiceStartName, kServiceAccount) != 0) {
        error = "existing service is not the restricted Spirula service; refusing to control it";
        return false;
    }
    SERVICE_SID_INFO sid_info{};
    DWORD sid_info_size = 0;
    if (!QueryServiceConfig2W(service, SERVICE_CONFIG_SERVICE_SID_INFO,
                              reinterpret_cast<BYTE*>(&sid_info),
                              sizeof(sid_info), &sid_info_size)) {
        error = windows_error("reading restricted service identity",
                              GetLastError());
        return false;
    }
    if (sid_info.dwServiceSidType != SERVICE_SID_TYPE_RESTRICTED) {
        error = "existing service does not use the required restricted service SID";
        return false;
    }
    if (!info->lpBinaryPathName) {
        error = "existing service has no executable image path";
        return false;
    }
    if (!service_sid(sid, error) ||
        !config_from_image(info->lpBinaryPathName, config, error) ||
        !validate_definition_shape(config, error))
        return false;
    start_type = info->dwStartType;
    return true;
}

bool query_service_config(SC_HANDLE service, Configuration& config,
                          DWORD& start_type, std::vector<std::uint8_t>& sid,
                          std::string& error) {
    if (!query_service_definition(service, config, start_type, sid, error) ||
        !validate_configuration(config, sid.data(), error) ||
        !verify_service_permissions(config, sid.data(), error))
        return false;
    return true;
}

bool open_service(SC_HANDLE& manager, SC_HANDLE& service, DWORD access,
                  std::string& error) {
    manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        error = windows_error("opening the local Service Control Manager", GetLastError());
        return false;
    }
    service = OpenServiceW(manager, kServiceName, access);
    if (!service) {
        const DWORD code = GetLastError();
        CloseServiceHandle(manager);
        manager = nullptr;
        if (code == ERROR_SERVICE_DOES_NOT_EXIST) {
            error = "Spirula service is not installed";
        } else {
            error = windows_error("opening the Spirula service", code);
        }
        return false;
    }
    return true;
}

bool wait_service_state(SC_HANDLE service, DWORD wanted, std::string& error) {
    const ULONGLONG deadline = GetTickCount64() + 30000;
    for (;;) {
        SERVICE_STATUS_PROCESS status{};
        DWORD bytes = 0;
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                  reinterpret_cast<BYTE*>(&status),
                                  sizeof(status), &bytes)) {
            error = windows_error("querying the Spirula service", GetLastError());
            return false;
        }
        if (status.dwCurrentState == wanted) return true;
        if (GetTickCount64() >= deadline) {
            error = "service manager timed out waiting for the requested state";
            return false;
        }
        Sleep(200);
    }
}

Result control_windows_service(bool do_start) {
    if (Result admin = require_admin(); !admin.success) return admin;
    SC_HANDLE manager = nullptr, service = nullptr;
    std::string error;
    const DWORD access = SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS |
        (do_start ? SERVICE_START | SERVICE_CHANGE_CONFIG : SERVICE_STOP);
    if (!open_service(manager, service, access, error)) return failure(error);
    Configuration config;
    DWORD start_type = 0;
    std::vector<std::uint8_t> sid;
    const bool valid = do_start
        ? query_service_config(service, config, start_type, sid, error)
        : query_service_definition(service, config, start_type, sid, error);
    if (!valid) {
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return failure(error);
    }
    bool okay = true;
    if (do_start) {
        SERVICE_STATUS_PROCESS current{};
        DWORD bytes = 0;
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                  reinterpret_cast<BYTE*>(&current),
                                  sizeof(current), &bytes)) {
            error = windows_error("querying the Spirula service", GetLastError());
            okay = false;
        }
        bool restore_disabled = false;
        if (okay && current.dwCurrentState != SERVICE_RUNNING) {
            if (start_type == SERVICE_DISABLED) {
                if (!ChangeServiceConfigW(service, SERVICE_NO_CHANGE,
                                          SERVICE_DEMAND_START, SERVICE_NO_CHANGE,
                                          nullptr, nullptr, nullptr, nullptr,
                                          nullptr, nullptr, nullptr)) {
                    error = windows_error(
                        "temporarily enabling the Spirula service",
                        GetLastError());
                    okay = false;
                } else {
                    restore_disabled = true;
                }
            }
            if (okay && !StartServiceW(service, 0, nullptr) &&
                GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
                error = windows_error("starting the Spirula service", GetLastError());
                okay = false;
            }
            if (okay) okay = wait_service_state(service, SERVICE_RUNNING, error);
        }
        if (restore_disabled &&
            !ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_DISABLED,
                                  SERVICE_NO_CHANGE, nullptr, nullptr, nullptr,
                                  nullptr, nullptr, nullptr, nullptr)) {
            const std::string restore_error = windows_error(
                "restoring disabled startup for the Spirula service",
                GetLastError());
            error = error.empty()
                ? restore_error
                : error + "; startup configuration was not restored: " +
                      restore_error;
            okay = false;
        }
    } else {
        SERVICE_STATUS_PROCESS current{};
        DWORD bytes = 0;
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                  reinterpret_cast<BYTE*>(&current),
                                  sizeof(current), &bytes)) {
            error = windows_error("querying the Spirula service", GetLastError());
            okay = false;
        } else if (current.dwCurrentState != SERVICE_STOPPED) {
            SERVICE_STATUS ignored{};
            if (!ControlService(service, SERVICE_CONTROL_STOP, &ignored) &&
                GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
                error = windows_error("stopping the Spirula service", GetLastError());
                okay = false;
            }
            if (okay) okay = wait_service_state(service, SERVICE_STOPPED, error);
        }
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return okay ? success() : failure(error);
}

#elif defined(__linux__) || defined(__APPLE__)

struct ServiceAccount {
    uid_t uid = 0;
    gid_t gid = 0;
};

bool lookup_service_account(ServiceAccount& account, std::string& error) {
    long size = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (size < 1024) size = 16384;
    std::vector<char> buffer(static_cast<std::size_t>(size));
    struct passwd pwd{};
    struct passwd* found = nullptr;
    int rc = getpwnam_r(kServiceUser, &pwd, buffer.data(), buffer.size(), &found);
    if (rc != 0 || !found || found->pw_uid == 0) {
        error = "dedicated non-root service account spirula-worker is missing or unusable";
        return false;
    }
    account.uid = found->pw_uid;
    account.gid = found->pw_gid;

    struct group grp{};
    struct group* group = nullptr;
    rc = getgrnam_r(kServiceGroup, &grp, buffer.data(), buffer.size(), &group);
    if (rc != 0 || !group || group->gr_gid != account.gid) {
        error = "dedicated service group spirula-worker is missing or does not match the service account";
        return false;
    }
    return true;
}

struct PosixPathInfo {
    std::vector<struct stat> components;
};

bool inspect_posix_path(const fs::path& path, bool directory, const char* name,
                        PosixPathInfo& info, std::string& error) {
    if (path.empty() || !path.is_absolute() || path == path.root_path() ||
        has_dot_component(path) || path.lexically_normal() != path ||
        has_control(path.native())) {
        error = std::string(name) + " must be an absolute canonical non-root path";
        return false;
    }
    std::error_code ec;
    const fs::path canonical = fs::canonical(path, ec);
    if (ec || canonical != path) {
        error = std::string(name) + " must already exist at its canonical path";
        return false;
    }

    fs::path current = path.root_path();
    std::vector<fs::path> pieces{current};
    for (const fs::path& part : path.relative_path()) {
        current /= part;
        pieces.push_back(current);
    }
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        struct stat st{};
        if (lstat(pieces[i].c_str(), &st) != 0) {
            error = std::string(name) + " does not exist or cannot be inspected";
            return false;
        }
        if (S_ISLNK(st.st_mode)) {
            error = std::string(name) + " must not contain symbolic links";
            return false;
        }
        const bool leaf = i + 1 == pieces.size();
        if ((!leaf || directory) && !S_ISDIR(st.st_mode)) {
            error = std::string(name) + " contains a non-directory path component";
            return false;
        }
        if (leaf && !directory && !S_ISREG(st.st_mode)) {
            error = std::string(name) + " must be a regular executable file";
            return false;
        }
        info.components.push_back(st);
    }
    return true;
}

unsigned int effective_mode(const struct stat& st, uid_t uid, gid_t gid) {
    if (st.st_uid == uid) return (st.st_mode >> 6) & 7;
    if (st.st_gid == gid) return (st.st_mode >> 3) & 7;
    return st.st_mode & 7;
}

bool validate_definition_shape(const Configuration& config,
                              std::string& error) {
    if (config.executable.filename() != "spirula") {
        error = "service executable must be named spirula";
        return false;
    }
    const fs::path* paths[] = {
        &config.executable, &config.config_root, &config.state_root,
        &config.storage_root};
    for (const fs::path* path : paths)
        if (path->empty() || !path->is_absolute() ||
            *path == path->root_path() || has_dot_component(*path) ||
            path->lexically_normal() != *path || has_control(path->native())) {
            error = "service executable and roots must be absolute canonical non-root paths";
            return false;
        }
    return roots_disjoint(config, error);
}

bool validate_state_file_permissions(const fs::path& state_root, uid_t uid,
                                     std::string& error) {
    const fs::path state_file = state_root / "agent-state.json";
    struct stat st{};
    if (lstat(state_file.c_str(), &st) != 0) {
        if (errno == ENOENT) return true;
        error = "cannot inspect the existing agent-state.json permissions";
        return false;
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != uid ||
        (st.st_mode & 07777) != 0600) {
        error = "agent-state.json must be a private regular file owned by spirula-worker";
        return false;
    }
    return true;
}

bool validate_configuration(const Configuration& config, ServiceAccount account,
                            std::string& error) {
    if (!validate_definition_shape(config, error)) return false;
    PosixPathInfo executable, config_root, state_root, storage_root;
    if (!inspect_posix_path(config.executable, false, "executable", executable, error) ||
        !inspect_posix_path(config.config_root, true, "config root", config_root, error) ||
        !inspect_posix_path(config.state_root, true, "state root", state_root, error) ||
        !inspect_posix_path(config.storage_root, true, "storage root", storage_root, error))
        return false;
#ifdef __linux__
    for (const fs::path* path : {&config.executable, &config.config_root,
                                 &config.state_root, &config.storage_root}) {
        if (path_prefix("/home", *path) || path_prefix("/root", *path) ||
            path_prefix("/run/user", *path)) {
            error = "system service paths must not be inside a user home or session directory";
            return false;
        }
    }
#endif

    auto secure_parents = [&](const PosixPathInfo& path) {
        for (std::size_t i = 0; i + 1 < path.components.size(); ++i) {
            const struct stat& st = path.components[i];
            if (st.st_uid != 0 || (st.st_mode & (S_IWGRP | S_IWOTH)) ||
                (effective_mode(st, account.uid, account.gid) & 1) == 0)
                return false;
        }
        return true;
    };
    if (!secure_parents(executable) || !secure_parents(config_root) ||
        !secure_parents(state_root) || !secure_parents(storage_root)) {
        error = "service executable and roots must be below root-owned, non-writable machine directories";
        return false;
    }
    const struct stat& binary = executable.components.back();
    const unsigned int binary_access = effective_mode(binary, account.uid,
                                                       account.gid);
    if (binary.st_uid != 0 || (binary.st_mode & (S_IWGRP | S_IWOTH)) ||
        (binary_access & (4 | 1)) != (4 | 1)) {
        error = "service executable must be root-owned, non-writable by ordinary users, and executable by spirula-worker";
        return false;
    }

    const struct stat& config_dir = config_root.components.back();
    const unsigned int config_access = effective_mode(config_dir, account.uid,
                                                      account.gid);
    if (config_dir.st_uid != 0 || (config_dir.st_mode & (S_IWGRP | S_IWOTH)) ||
        (config_access & (4 | 1)) != (4 | 1) ||
        (config_access & 2)) {
        error = "config root must be machine-owned, readable by spirula-worker, and not writable by the service";
        return false;
    }
    for (const PosixPathInfo* root : {&state_root, &storage_root}) {
        const struct stat& st = root->components.back();
        if (st.st_uid != account.uid || (st.st_mode & 07777) != 0700) {
            error = "state and storage roots must be private, owned by spirula-worker, and writable by that account";
            return false;
        }
    }
    return validate_state_file_permissions(config.state_root, account.uid,
                                           error);
}

Result require_admin() {
    return geteuid() == 0
        ? success()
        : failure("this service-management operation requires local administrator privileges (root)");
}

struct CommandResult {
    bool success = false;
    bool timed_out = false;
    int exit_code = -1;
    std::string output;
    std::string error;
};

CommandResult run_manager(const std::vector<std::string>& argv,
                          std::chrono::seconds timeout = std::chrono::seconds(20)) {
    CommandResult result;
    std::atomic<bool> cancel{false};
    std::mutex mutex;
    std::condition_variable finished_cv;
    bool finished = false;
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lock(mutex);
        if (!finished_cv.wait_for(lock, timeout, [&] { return finished; })) {
            result.timed_out = true;
            cancel.store(true);
        }
    });

    app::proc::ProcessResult process;
    try {
        app::proc::ProcessOptions options;
        options.argv = argv;
        options.cancel = &cancel;
        options.env_overrides = {
            {"PATH", "/usr/bin:/bin:/usr/sbin:/sbin"},
            {"HOME", "/root"}, {"LANG", "C"}, {"LC_ALL", "C"},
            {"TMPDIR", "/tmp"}, {"LD_PRELOAD", ""}, {"LD_AUDIT", ""},
            {"LD_DEBUG", ""}, {"LD_DEBUG_OUTPUT", ""},
            {"LD_ORIGIN_PATH", ""}, {"LD_PROFILE", ""},
            {"GLIBC_TUNABLES", ""}, {"MALLOC_TRACE", ""},
            {"DYLD_INSERT_LIBRARIES", ""},
            {"DYLD_FALLBACK_LIBRARY_PATH", "/usr/lib"},
            {"DYLD_LIBRARY_PATH", "/usr/lib"},
            {"DYLD_FRAMEWORK_PATH", "/System/Library/Frameworks"},
            {"DYLD_FALLBACK_FRAMEWORK_PATH", "/System/Library/Frameworks"},
            {"WORKER_CONTROL", "0"}, {"SS_WORKER_CONTROL", "0"},
            {"SSPLAT_WORKER_CONTROL", "0"}};
#ifdef __linux__
        options.env_overrides.emplace_back("LD_LIBRARY_PATH", "/lib:/usr/lib");
#endif
#ifdef __linux__
        options.env_overrides.emplace_back(
            "SYSTEMD_BUS_ADDRESS", "unix:path=/run/systemd/private");
        options.env_overrides.emplace_back(
            "DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/run/systemd/private");
        options.env_overrides.emplace_back("SYSTEMD_OFFLINE", "0");
        options.env_overrides.emplace_back(
            "SYSTEMD_UNIT_PATH",
            "/etc/systemd/system:/run/systemd/system:/usr/local/lib/systemd/system:/usr/lib/systemd/system:/lib/systemd/system");
        options.env_overrides.emplace_back("SYSTEMD_PAGER", "cat");
        options.env_overrides.emplace_back("SYSTEMD_COLORS", "0");
#endif
        options.on_line = [&](const std::string& line) {
            if (result.output.size() < kMaxDefinitionBytes) {
                const std::size_t count = std::min(
                    kMaxDefinitionBytes - result.output.size(), line.size());
                result.output.append(line.data(), count);
                if (result.output.size() < kMaxDefinitionBytes)
                    result.output += '\n';
            }
        };
        process = app::proc::run_process(options);
    } catch (...) {
        process.outcome = app::proc::ProcessOutcome::SpawnFailed;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        finished = true;
    }
    finished_cv.notify_one();
    watchdog.join();

    result.exit_code = process.exit_code;
    result.success = process.outcome == app::proc::ProcessOutcome::Success &&
                     process.exit_code == 0;
    if (!result.success) {
        if (result.timed_out || process.outcome == app::proc::ProcessOutcome::Cancelled) {
            result.error = "service manager command timed out";
        } else if (process.outcome == app::proc::ProcessOutcome::SpawnFailed) {
            result.error = "the local service manager is unavailable";
        } else {
            std::string lower = result.output;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower.find("permission denied") != std::string::npos ||
                lower.find("not authorized") != std::string::npos ||
                lower.find("authentication is required") != std::string::npos) {
                result.error = "service-management permission denied; run as local administrator";
            } else if (lower.find("unknown user") != std::string::npos ||
                       lower.find("no such user") != std::string::npos ||
                       lower.find("failed to look up user") != std::string::npos ||
                       lower.find("bad user") != std::string::npos) {
                result.error = "dedicated non-root service account spirula-worker is missing or unusable";
            } else {
                result.error = "local service manager rejected the request";
            }
        }
    }
    return result;
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool read_secure_definition(const fs::path& path, std::string& contents,
                            bool& exists, std::string& error) {
    exists = false;
    struct stat before{};
    if (lstat(path.c_str(), &before) != 0) {
        if (errno == ENOENT) return true;
        error = "cannot inspect the installed service definition";
        return false;
    }
    exists = true;
    if (S_ISLNK(before.st_mode) || !S_ISREG(before.st_mode) ||
        before.st_uid != 0 || (before.st_mode & (S_IWGRP | S_IWOTH)) ||
        before.st_size < 0 || static_cast<std::uintmax_t>(before.st_size) >
                                  kMaxDefinitionBytes) {
        error = "installed service definition is insecure or not managed by Spirula";
        return false;
    }
    int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int fd = open(path.c_str(), flags);
    if (fd < 0) {
        error = "cannot read the installed service definition";
        return false;
    }
    struct stat opened{};
    if (fstat(fd, &opened) != 0 || opened.st_dev != before.st_dev ||
        opened.st_ino != before.st_ino || !S_ISREG(opened.st_mode) ||
        opened.st_uid != 0 || (opened.st_mode & (S_IWGRP | S_IWOTH)) ||
        opened.st_size < 0 || static_cast<std::uintmax_t>(opened.st_size) >
                                  kMaxDefinitionBytes) {
        close(fd);
        error = "installed service definition changed or is insecure";
        return false;
    }
    contents.resize(static_cast<std::size_t>(opened.st_size));
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t got = read(fd, contents.data() + offset,
                                 contents.size() - offset);
        if (got <= 0) {
            close(fd);
            error = "cannot read the installed service definition";
            return false;
        }
        offset += static_cast<std::size_t>(got);
    }
    close(fd);
    return true;
}

bool safe_manager_directory(const fs::path& directory, std::string& error) {
    PosixPathInfo info;
    if (!inspect_posix_path(directory, true, "service definition directory",
                            info, error))
        return false;
    for (const struct stat& st : info.components)
        if (st.st_uid != 0 || (st.st_mode & (S_IWGRP | S_IWOTH))) {
            error = "service definition directory must be root-owned and not writable by ordinary users";
            return false;
        }
    return true;
}

bool write_exclusive_definition(const fs::path& path, const std::string& text,
                               std::string& error) {
    int flags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int fd = open(path.c_str(), flags, 0644);
    if (fd < 0) {
        error = errno == EEXIST
            ? "a service definition already exists; refusing to replace it"
            : "cannot create the service definition";
        return false;
    }
    bool okay = fchmod(fd, 0644) == 0;
    std::size_t offset = 0;
    while (okay && offset < text.size()) {
        const ssize_t written = write(fd, text.data() + offset,
                                      text.size() - offset);
        if (written <= 0) {
            okay = false;
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (okay) okay = fsync(fd) == 0;
    if (close(fd) != 0) okay = false;
    if (!okay) {
        unlink(path.c_str());
        error = "cannot write the service definition";
        return false;
    }
    return true;
}

#ifdef __linux__
std::string systemd_quote(const std::string& value,
                          bool escape_dollar = true) {
    std::string result = "\"";
    for (char c : value) {
        if (c == '\\' || c == '"') result += '\\';
        if (c == '%' || (c == '$' && escape_dollar)) result += c;
        result += c;
    }
    result += '"';
    return result;
}

bool decode_systemd_quoted(const std::string& value, std::string& decoded,
                           bool decode_dollar = true) {
    if (value.size() < 2 || value.front() != '"' || value.back() != '"')
        return false;
    for (std::size_t i = 1; i + 1 < value.size(); ++i) {
        const char c = value[i];
        if (c == '\\') {
            if (++i + 1 >= value.size() ||
                (value[i] != '\\' && value[i] != '"'))
                return false;
            decoded += value[i];
        } else if ((c == '%' || (decode_dollar && c == '$')) &&
                   i + 1 < value.size() - 1 && value[i + 1] == c) {
            decoded += c;
            ++i;
        } else {
            decoded += c;
        }
    }
    return !has_control(decoded);
}

bool decode_systemd_words(const std::string& value,
                          std::vector<std::string>& words,
                          bool decode_dollar = true) {
    std::size_t i = 0;
    while (i < value.size()) {
        if (value[i] != '"') return false;
        const std::size_t begin = i++;
        bool escaped = false;
        while (i < value.size()) {
            if (escaped) {
                escaped = false;
            } else if (value[i] == '\\') {
                escaped = true;
            } else if (value[i] == '"') {
                ++i;
                break;
            }
            ++i;
        }
        if (i > value.size() || value[i - 1] != '"') return false;
        std::string word;
        if (!decode_systemd_quoted(value.substr(begin, i - begin), word,
                                   decode_dollar))
            return false;
        words.push_back(std::move(word));
        if (i == value.size()) break;
        if (value[i++] != ' ') return false;
    }
    return !words.empty();
}

std::string make_systemd_unit(const Configuration& config) {
    const std::string argv[] = {
        config.executable.string(), "agent", "run", "--config-root",
        config.config_root.string(), "--state-root", config.state_root.string(),
        "--storage-root", config.storage_root.string()};
    std::string exec = "ExecStart=";
    for (std::size_t i = 0; i < std::size(argv); ++i) {
        if (i) exec += ' ';
        exec += systemd_quote(argv[i]);
    }
    return "# Managed by Spirula AgentServiceManager v1\n"
           "[Unit]\n"
           "Description=Spirula Remote Worker\n"
           "[Service]\n"
           "Type=simple\n"
           "User=spirula-worker\n"
           "Group=spirula-worker\n" + exec + "\n" +
           "WorkingDirectory=" + systemd_quote(config.storage_root.string(), false) + "\n"
           "Restart=on-failure\n"
           "RestartSec=5\n"
           "NoNewPrivileges=true\n"
           "PrivateTmp=true\n"
           "ProtectSystem=strict\n"
           "ProtectHome=true\n"
           "UMask=0077\n"
           "ReadWritePaths=" + systemd_quote(config.state_root.string(), false) + " " +
           systemd_quote(config.storage_root.string(), false) + "\n"
           "[Install]\n"
           "WantedBy=multi-user.target\n";
}

bool parse_systemd_definition(const std::string& contents,
                             Configuration& config, std::string& error,
                             bool validate_live_paths) {
    std::string exec, working, writable;
    std::istringstream input(contents);
    std::string line;
    while (std::getline(input, line)) {
        if (line.compare(0, 10, "ExecStart=") == 0) {
            if (!exec.empty()) goto malformed;
            exec = line.substr(10);
        } else if (line.compare(0, 17, "WorkingDirectory=") == 0) {
            if (!working.empty()) goto malformed;
            working = line.substr(17);
        } else if (line.compare(0, 15, "ReadWritePaths=") == 0) {
            if (!writable.empty()) goto malformed;
            writable = line.substr(15);
        } else if (line == "# Managed by Spirula AgentServiceManager v1" ||
                   line == "[Unit]" || line == "Description=Spirula Remote Worker" ||
                   line == "[Service]" || line == "Type=simple" ||
                   line == "User=spirula-worker" || line == "Group=spirula-worker" ||
                   line == "Restart=on-failure" || line == "RestartSec=5" ||
                   line == "NoNewPrivileges=true" || line == "PrivateTmp=true" ||
                   line == "ProtectSystem=strict" || line == "ProtectHome=true" ||
                   line == "UMask=0077" || line == "[Install]" ||
                   line == "WantedBy=multi-user.target") {
            continue;
        } else {
            goto malformed;
        }
    }
    {
        std::vector<std::string> args, write_paths;
        std::string working_dir;
        if (!decode_systemd_words(exec, args) || args.size() != 9 ||
            args[1] != "agent" || args[2] != "run" ||
            args[3] != "--config-root" || args[5] != "--state-root" ||
            args[7] != "--storage-root" ||
            !decode_systemd_quoted(working, working_dir, false) ||
            !decode_systemd_words(writable, write_paths, false) ||
            write_paths.size() != 2)
            goto malformed;
        config.executable = fs::path(args[0]);
        config.config_root = fs::path(args[4]);
        config.state_root = fs::path(args[6]);
        config.storage_root = fs::path(args[8]);
        if (working_dir != config.storage_root.string() ||
            write_paths[0] != config.state_root.string() ||
            write_paths[1] != config.storage_root.string())
            goto malformed;
    }
    if (validate_live_paths) {
        ServiceAccount account;
        if (!lookup_service_account(account, error) ||
            !validate_configuration(config, account, error))
            return false;
    } else if (!validate_definition_shape(config, error)) {
        return false;
    }
    if (make_systemd_unit(config) != contents) goto malformed;
    return true;

malformed:
    error = "existing systemd unit is not an unchanged Spirula service definition";
    return false;
}

constexpr const char* kSystemdUnitPath =
    "/etc/systemd/system/spirula-remote-worker.service";
constexpr const char* kSystemdDir = "/etc/systemd/system";

bool managed_systemd_config(Configuration& config, bool& exists,
                            std::string& error, bool validate_live_paths = true) {
    std::string contents;
    if (!read_secure_definition(kSystemdUnitPath, contents, exists, error) || !exists)
        return !exists && error.empty();
    return parse_systemd_definition(contents, config, error, validate_live_paths);
}

std::map<std::string, std::string> parse_properties(const std::string& text) {
    std::map<std::string, std::string> values;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t equal = line.find('=');
        if (equal == std::string::npos) continue;
        const std::string key = line.substr(0, equal);
        const std::string value = line.substr(equal + 1);
        const auto [it, inserted] = values.emplace(key, value);
        if (!inserted) it->second.assign(1, '\0');
    }
    return values;
}

bool split_manager_words(const std::string& value,
                         std::vector<std::string>& words) {
    words.clear();
    std::string word;
    char quote = 0;
    bool escaped = false;
    bool started = false;
    for (char c : value) {
        if (escaped) {
            word += c;
            escaped = false;
            started = true;
        } else if (c == '\\' && quote != '\'') {
            escaped = true;
            started = true;
        } else if (quote) {
            if (c == quote) quote = 0;
            else word += c;
        } else if (c == '\'' || c == '"') {
            quote = c;
            started = true;
        } else if (c == ' ' || c == '\t') {
            if (started) {
                words.push_back(std::move(word));
                word.clear();
                started = false;
            }
        } else {
            word += c;
            started = true;
        }
    }
    if (escaped || quote) return false;
    if (started) words.push_back(std::move(word));
    return true;
}

bool systemd_exec_matches(const std::string& value,
                          const Configuration& config) {
    constexpr char kPathPrefix[] = "{ path=";
    constexpr char kArgvMarker[] = " ; argv[]=";
    constexpr char kIgnoreMarker[] = " ; ignore_errors=no ;";
    if (value.compare(0, sizeof(kPathPrefix) - 1, kPathPrefix) != 0)
        return false;
    const std::size_t path_begin = sizeof(kPathPrefix) - 1;
    const std::size_t argv_marker = value.find(kArgvMarker, path_begin);
    if (argv_marker == std::string::npos) return false;
    std::vector<std::string> path_words;
    if (!split_manager_words(value.substr(path_begin, argv_marker - path_begin),
                             path_words) ||
        path_words.size() != 1 || path_words[0] != config.executable.string())
        return false;
    const std::size_t argv_begin = argv_marker + sizeof(kArgvMarker) - 1;
    const std::size_t ignore_marker = value.find(kIgnoreMarker, argv_begin);
    if (ignore_marker == std::string::npos ||
        value.find(" ; start_time=", ignore_marker) == std::string::npos)
        return false;
    std::vector<std::string> argv;
    if (!split_manager_words(value.substr(argv_begin, ignore_marker - argv_begin),
                             argv))
        return false;
    const std::vector<std::string> expected = {
        config.executable.string(), "agent", "run", "--config-root",
        config.config_root.string(), "--state-root", config.state_root.string(),
        "--storage-root", config.storage_root.string()};
    return argv == expected;
}

bool systemd_effective_config_matches(
    const std::map<std::string, std::string>& properties,
    const Configuration& config, std::string& error) {
    auto equals = [&](const char* name, const std::string& expected) {
        const auto it = properties.find(name);
        return it != properties.end() && it->second == expected;
    };
    std::vector<std::string> writable, working;
    const auto paths = properties.find("ReadWritePaths");
    const auto working_directory = properties.find("WorkingDirectory");
    const std::vector<std::string> expected_paths = {
        config.state_root.string(), config.storage_root.string()};
    const std::vector<std::string> expected_working = {
        config.storage_root.string()};
    if (!equals("LoadState", "loaded") ||
        !equals("FragmentPath", kSystemdUnitPath) ||
        !equals("DropInPaths", "") ||
        !equals("NeedDaemonReload", "no") ||
        !equals("Type", "simple") || !equals("User", kServiceUser) ||
        !equals("Group", kServiceGroup) ||
        !equals("Restart", "on-failure") ||
        !equals("RestartUSec", "5s") ||
        !equals("NoNewPrivileges", "yes") ||
        !equals("PrivateTmp", "yes") ||
        !equals("ProtectSystem", "strict") ||
        !equals("ProtectHome", "yes") || !equals("UMask", "0077") ||
        paths == properties.end() ||
        !split_manager_words(paths->second, writable) ||
        writable != expected_paths ||
        working_directory == properties.end() ||
        !split_manager_words(working_directory->second, working) ||
        working != expected_working) {
        error = "systemd's loaded effective service configuration differs from the managed definition; refusing to control it";
        return false;
    }
    const auto exec = properties.find("ExecStart");
    if (exec == properties.end() || !systemd_exec_matches(exec->second, config)) {
        error = "systemd's loaded ExecStart differs from the managed argument contract; refusing to control it";
        return false;
    }
    return true;
}

CommandResult systemd_show() {
    return run_manager({"/usr/bin/systemctl", "--system", "--no-pager",
                        "--no-ask-password", "show",
                        "--property=FragmentPath", "--property=LoadState",
                        "--property=ActiveState", "--property=UnitFileState",
                        "--property=DropInPaths", "--property=NeedDaemonReload",
                        "--property=ExecStart", "--property=Type",
                        "--property=User", "--property=Group",
                        "--property=WorkingDirectory", "--property=ReadWritePaths",
                        "--property=Restart", "--property=RestartUSec",
                        "--property=MainPID",
                        "--property=NoNewPrivileges", "--property=PrivateTmp",
                        "--property=ProtectSystem", "--property=ProtectHome",
                        "--property=UMask", kUnitName});
}

bool require_managed_systemd_unit(std::string& error,
                                  bool validate_live_paths = true) {
    Configuration config;
    bool exists = false;
    if (!managed_systemd_config(config, exists, error, validate_live_paths))
        return false;
    const CommandResult shown = systemd_show();
    const auto properties = parse_properties(shown.output);
    if (!exists) {
        const auto load = properties.find("LoadState");
        if (load != properties.end() && load->second == "not-found") {
            error = "Spirula service is not installed";
        } else if (!shown.success) {
            error = shown.error;
        } else {
            error = "a systemd job with the Spirula service name is loaded without its managed unit file; refusing to control it";
        }
        return false;
    }
    if (!shown.success) {
        error = shown.error;
        return false;
    }
    return systemd_effective_config_matches(properties, config, error);
}

bool systemd_absent_is_clean(std::string& error) {
    const CommandResult shown = systemd_show();
    const auto properties = parse_properties(shown.output);
    const auto load = properties.find("LoadState");
    if (load != properties.end() && load->second == "not-found") return true;
    if (!shown.success) {
        error = shown.error;
        return false;
    }
    error = "systemd reports a Spirula-named job without its managed unit file; refusing to treat it as absent";
    return false;
}

bool systemd_entry_exists(const fs::path& path) {
    struct stat st{};
    return lstat(path.c_str(), &st) == 0;
}
#endif  // __linux__

#ifdef __APPLE__

constexpr const char* kLaunchdPath =
    "/Library/LaunchDaemons/com.spirula.remote-worker.plist";
constexpr const char* kLaunchdDir = "/Library/LaunchDaemons";
const std::string kLaunchdTarget = "system/com.spirula.remote-worker";

std::string xml_escape(const std::string& value) {
    std::string result;
    for (char c : value) {
        switch (c) {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '\"': result += "&quot;"; break;
            case '\'': result += "&apos;"; break;
            default: result += c; break;
        }
    }
    return result;
}

bool xml_unescape(const std::string& value, std::string& result) {
    result.clear();
    for (std::size_t i = 0; i < value.size();) {
        if (value[i] != '&') {
            result += value[i++];
            continue;
        }
        const std::size_t end = value.find(';', i);
        if (end == std::string::npos) return false;
        const std::string entity = value.substr(i, end - i + 1);
        if (entity == "&amp;") result += '&';
        else if (entity == "&lt;") result += '<';
        else if (entity == "&gt;") result += '>';
        else if (entity == "&quot;") result += '\"';
        else if (entity == "&apos;") result += '\'';
        else return false;
        i = end + 1;
    }
    return !has_control(result);
}

std::string make_launchd_plist(const Configuration& config) {
    const std::string args[] = {
        config.executable.string(), "agent", "run", "--config-root",
        config.config_root.string(), "--state-root", config.state_root.string(),
        "--storage-root", config.storage_root.string()};
    std::string result =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<!-- Managed by Spirula AgentServiceManager v1 -->\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "    <key>Label</key>\n"
        "    <string>com.spirula.remote-worker</string>\n"
        "    <key>UserName</key>\n"
        "    <string>spirula-worker</string>\n"
        "    <key>GroupName</key>\n"
        "    <string>spirula-worker</string>\n"
        "    <key>ProgramArguments</key>\n"
        "    <array>\n";
    for (const std::string& arg : args)
        result += "        <string>" + xml_escape(arg) + "</string>\n";
    result +=
        "    </array>\n"
        "    <key>RunAtLoad</key>\n"
        "    <true/>\n"
        "    <key>KeepAlive</key>\n"
        "    <true/>\n"
        "    <key>ProcessType</key>\n"
        "    <string>Background</string>\n"
        "    <key>Umask</key>\n"
        "    <integer>63</integer>\n"
        "</dict>\n"
        "</plist>\n";
    return result;
}

bool parse_launchd_definition(const std::string& contents,
                             Configuration& config, std::string& error,
                             bool validate_live_paths) {
    std::vector<std::string> strings;
    std::istringstream input(contents);
    std::string line;
    constexpr const char* prefix = "        <string>";
    while (std::getline(input, line)) {
        if (line.compare(0, std::strlen(prefix), prefix) == 0 &&
            line.size() >= std::strlen(prefix) + 9 &&
            line.compare(line.size() - 9, 9, "</string>") == 0) {
            const std::string encoded = line.substr(
                std::strlen(prefix), line.size() - std::strlen(prefix) - 9);
            std::string decoded;
            if (!xml_unescape(encoded, decoded) || xml_escape(decoded) != encoded)
                goto malformed;
            strings.push_back(std::move(decoded));
        }
    }
    if (strings.size() != 13 || strings[0] != kLaunchdLabel ||
        strings[1] != kServiceUser || strings[2] != kServiceGroup ||
        strings[12] != "Background")
        goto malformed;
    {
        const std::size_t first = 3;
        if (strings[first + 1] != "agent" || strings[first + 2] != "run" ||
            strings[first + 3] != "--config-root" ||
            strings[first + 5] != "--state-root" ||
            strings[first + 7] != "--storage-root")
            goto malformed;
        config.executable = fs::path(strings[first]);
        config.config_root = fs::path(strings[first + 4]);
        config.state_root = fs::path(strings[first + 6]);
        config.storage_root = fs::path(strings[first + 8]);
    }
    if (validate_live_paths) {
        ServiceAccount account;
        if (!lookup_service_account(account, error) ||
            !validate_configuration(config, account, error))
            return false;
    } else if (!validate_definition_shape(config, error)) {
        return false;
    }
    if (make_launchd_plist(config) != contents) goto malformed;
    return true;

malformed:
    error = "existing LaunchDaemon is not an unchanged Spirula service definition";
    return false;
}

bool managed_launchd_config(Configuration& config, bool& exists,
                            std::string& error, bool validate_live_paths = true) {
    std::string contents;
    if (!read_secure_definition(kLaunchdPath, contents, exists, error) || !exists)
        return !exists && error.empty();
    return parse_launchd_definition(contents, config, error, validate_live_paths);
}

bool launchd_not_loaded(const CommandResult& result) {
    const std::string output = lower_ascii(result.output);
    return output.find("could not find service") != std::string::npos ||
           output.find("service not found") != std::string::npos ||
           output.find("no such process") != std::string::npos;
}

CommandResult launchd_print() {
    return run_manager({"/bin/launchctl", "print", kLaunchdTarget});
}

bool launchd_startup_enabled(bool& enabled, std::string& error) {
    const CommandResult result = run_manager(
        {"/bin/launchctl", "print-disabled", "system"});
    if (!result.success) {
        error = result.error;
        return false;
    }
    enabled = true;
    const std::string marker = "\"" + std::string(kLaunchdLabel) + "\"";
    const std::size_t at = result.output.find(marker);
    if (at != std::string::npos) {
        const std::size_t end = result.output.find('\n', at);
        const std::string line = lower_ascii(result.output.substr(
            at, end == std::string::npos ? std::string::npos : end - at));
        if (line.find("=> disabled") != std::string::npos) {
            enabled = false;
        } else if (line.find("=> enabled") == std::string::npos) {
            error = "cannot determine whether the LaunchDaemon is enabled";
            return false;
        }
    } else if (result.output.size() >= kMaxDefinitionBytes) {
        error = "cannot determine whether the LaunchDaemon is enabled";
        return false;
    }
    return true;
}

std::string trim_manager_line(std::string value) {
    const std::size_t first = value.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    const std::size_t last = value.find_last_not_of(" \t\r");
    return value.substr(first, last - first + 1);
}

bool launchd_effective_config_matches(const std::string& output,
                                     const Configuration& config) {
    std::map<std::string, std::string> fields;
    std::vector<std::string> arguments;
    bool in_arguments = false;
    bool arguments_seen = false;
    bool arguments_closed = false;
    bool target_seen = false;
    std::istringstream input(output);
    std::string raw_line;
    while (std::getline(input, raw_line)) {
        const std::string line = trim_manager_line(raw_line);
        if (line.empty()) continue;
        if (in_arguments) {
            if (line == "}") {
                in_arguments = false;
                arguments_closed = true;
                continue;
            }
            if (arguments_closed) return false;
            std::string argument = line;
            if (line.size() >= 2 &&
                ((line.front() == '"' && line.back() == '"') ||
                 (line.front() == '\'' && line.back() == '\''))) {
                const char quote = line.front();
                argument.clear();
                for (std::size_t i = 1; i + 1 < line.size(); ++i) {
                    if (line[i] == '\\' && quote == '"' && i + 2 < line.size())
                        ++i;
                    argument += line[i];
                }
            }
            if (has_control(argument)) return false;
            arguments.push_back(std::move(argument));
            continue;
        }
        if (line == std::string("system/") + kLaunchdLabel + " = {") {
            if (target_seen) return false;
            target_seen = true;
            continue;
        }
        if (line == "arguments = {") {
            if (arguments_seen) return false;
            arguments_seen = true;
            in_arguments = true;
            continue;
        }
        const std::size_t equal = line.find(" = ");
        if (equal == std::string::npos) continue;
        const std::string key = line.substr(0, equal);
        if (key != "path" && key != "program" &&
            key != "username" && key != "groupname")
            continue;
        const auto [it, inserted] =
            fields.emplace(key, line.substr(equal + 3));
        if (!inserted) return false;
    }
    if (in_arguments || !arguments_closed) return false;
    const std::vector<std::string> expected = {
        config.executable.string(), "agent", "run", "--config-root",
        config.config_root.string(), "--state-root", config.state_root.string(),
        "--storage-root", config.storage_root.string()};
    auto is = [&](const char* name, const std::string& value) {
        const auto it = fields.find(name);
        return it != fields.end() && it->second == value;
    };
    return target_seen && is("path", kLaunchdPath) &&
           is("program", config.executable.string()) &&
           is("username", kServiceUser) && is("groupname", kServiceGroup) &&
           arguments == expected;
}

bool launchd_absent_is_clean(std::string& error) {
    const CommandResult loaded = launchd_print();
    if (loaded.success) {
        error = "a LaunchDaemon with the Spirula service label is loaded without its managed plist; refusing to treat it as absent";
        return false;
    }
    if (launchd_not_loaded(loaded)) return true;
    error = loaded.error;
    return false;
}

bool require_managed_launchd(std::string& error,
                             bool validate_live_paths = true) {
    Configuration config;
    bool exists = false;
    if (!managed_launchd_config(config, exists, error, validate_live_paths))
        return false;
    if (!exists) {
        if (launchd_absent_is_clean(error))
            error = "Spirula service is not installed";
        return false;
    }
    const CommandResult loaded = launchd_print();
    if (!loaded.success) {
        if (launchd_not_loaded(loaded)) return true;
        error = loaded.error;
        return false;
    }
    if (!launchd_effective_config_matches(loaded.output, config)) {
        error = "launchd's loaded job identity or effective configuration differs from the managed plist; refusing to control it";
        return false;
    }
    return true;
}

#endif  // __APPLE__

#endif  // !_WIN32

}  // namespace

#if defined(__linux__) || defined(__APPLE__)
Result validate_state_security(const fs::path& state_root) {
    ServiceAccount account;
    std::string error;
    if (!lookup_service_account(account, error)) return failure(error);
    PosixPathInfo info;
    if (!inspect_posix_path(state_root, true, "state root", info, error))
        return failure(error);
#ifdef __linux__
    if (path_prefix("/home", state_root) || path_prefix("/root", state_root) ||
        path_prefix("/run/user", state_root))
        return failure("system service state must not be inside a user home or session directory");
#endif
    for (std::size_t i = 0; i + 1 < info.components.size(); ++i) {
        const struct stat& parent = info.components[i];
        if (parent.st_uid != 0 || (parent.st_mode & (S_IWGRP | S_IWOTH)) ||
            (effective_mode(parent, account.uid, account.gid) & 1) == 0)
            return failure("state root must be below root-owned, non-writable machine directories");
    }
    const struct stat& root = info.components.back();
    if (root.st_uid != account.uid || (root.st_mode & 07777) != 0700)
        return failure("state root must be private, owned by spirula-worker, and writable by that account");
    return validate_state_file_permissions(state_root, account.uid, error)
        ? success() : failure(error);
}
#endif

#ifdef _WIN32

Result install(const Configuration& configuration) {
    if (Result admin = require_admin(); !admin.success) return admin;
    std::string error;
    if (!validate_configuration(configuration, nullptr, error))
        return failure(error);

    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr,
        SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!manager)
        return failure(windows_error("opening the local Service Control Manager", GetLastError()));
    const std::wstring image = service_image(configuration);
    SC_HANDLE service = CreateServiceW(
        manager, kServiceName, L"Spirula Remote Worker",
        SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS |
            SERVICE_START | SERVICE_STOP | DELETE,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        image.c_str(), nullptr, nullptr, nullptr, kServiceAccount, nullptr);
    if (!service) {
        const DWORD code = GetLastError();
        CloseServiceHandle(manager);
        if (code == ERROR_SERVICE_EXISTS || code == ERROR_DUPLICATE_SERVICE_NAME)
            return failure("SpirulaRemoteWorker already exists; refusing to replace it");
        return failure(windows_error("installing the Spirula service", code));
    }

    SERVICE_SID_INFO sid_info{SERVICE_SID_TYPE_RESTRICTED};
    SERVICE_FAILURE_ACTIONS actions{};
    SC_ACTION restart_actions[] = {
        {SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 30000},
        {SC_ACTION_RESTART, 300000}};
    actions.dwResetPeriod = 86400;
    actions.cActions = static_cast<DWORD>(std::size(restart_actions));
    actions.lpsaActions = restart_actions;
    SERVICE_FAILURE_ACTIONS_FLAG failure_flag{TRUE};
    bool okay = ChangeServiceConfig2W(service, SERVICE_CONFIG_SERVICE_SID_INFO,
                                      &sid_info) != FALSE &&
                ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS,
                                      &actions) != FALSE &&
                ChangeServiceConfig2W(service,
                                      SERVICE_CONFIG_FAILURE_ACTIONS_FLAG,
                                      &failure_flag) != FALSE;
    std::vector<std::uint8_t> sid;
    if (okay && !service_sid(sid, error)) okay = false;
    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> admins{};
    DWORD admins_size = static_cast<DWORD>(admins.size());
    if (okay && !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
                                    admins.data(), &admins_size)) {
        error = "cannot resolve local Administrators for machine-state permissions";
        okay = false;
    }
    constexpr ACCESS_MASK read_execute = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
    constexpr ACCESS_MASK write_tree = FILE_GENERIC_READ | FILE_GENERIC_WRITE |
                                       FILE_GENERIC_EXECUTE | DELETE |
                                       FILE_DELETE_CHILD;
    if (okay) {
        okay = grant_service_traverse_tree(
                   configuration.executable.parent_path(), sid.data(), error) &&
               add_service_ace(configuration.executable, sid.data(),
                               read_execute, NO_INHERITANCE, error) &&
               grant_service_tree(configuration.config_root, sid.data(),
                                  read_execute, error) &&
               grant_service_tree(configuration.state_root, sid.data(),
                                  write_tree, error) &&
               add_service_ace(configuration.state_root, admins.data(),
                               FILE_ALL_ACCESS,
                               CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE, error) &&
               (GetFileAttributesW(
                    (configuration.state_root / L"agent-state.json").c_str()) ==
                    INVALID_FILE_ATTRIBUTES ||
                add_service_ace(configuration.state_root / L"agent-state.json",
                                admins.data(), FILE_ALL_ACCESS,
                                NO_INHERITANCE, error)) &&
               grant_service_tree(configuration.storage_root, sid.data(),
                                  write_tree, error) &&
               validate_configuration(configuration, sid.data(), error) &&
               verify_service_permissions(configuration, sid.data(), error);
    }
    if (!okay) {
        DeleteService(service);
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return failure(error.empty()
            ? "cannot configure the restricted Spirula service account"
            : error);
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return success();
}

Result uninstall() {
    if (Result admin = require_admin(); !admin.success) return admin;
    SC_HANDLE manager = nullptr, service = nullptr;
    std::string error;
    if (!open_service(manager, service,
                      SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS |
                          SERVICE_STOP | DELETE,
                      error))
        return error == "Spirula service is not installed" ? success()
                                                            : failure(error);
    Configuration config;
    DWORD start_type = 0;
    std::vector<std::uint8_t> sid;
    if (!query_service_definition(service, config, start_type, sid, error)) {
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return failure(error);
    }
    SERVICE_STATUS_PROCESS current{};
    DWORD bytes = 0;
    bool okay = QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                     reinterpret_cast<BYTE*>(&current),
                                     sizeof(current), &bytes) != FALSE;
    if (!okay) error = windows_error("querying the Spirula service", GetLastError());
    if (okay && current.dwCurrentState != SERVICE_STOPPED) {
        SERVICE_STATUS ignored{};
        if (!ControlService(service, SERVICE_CONTROL_STOP, &ignored) &&
            GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
            error = windows_error("stopping the Spirula service", GetLastError());
            okay = false;
        }
        if (okay) okay = wait_service_state(service, SERVICE_STOPPED, error);
    }
    if (okay && !DeleteService(service)) {
        error = windows_error("removing the Spirula service", GetLastError());
        okay = false;
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return okay ? success() : failure(error);
}

Result start() { return control_windows_service(true); }
Result stop() { return control_windows_service(false); }

Result set_windows_startup(bool enabled) {
    if (Result admin = require_admin(); !admin.success) return admin;
    SC_HANDLE manager = nullptr, service = nullptr;
    std::string error;
    if (!open_service(manager, service,
                      SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG,
                      error))
        return failure(error);
    Configuration config;
    DWORD start_type = 0;
    std::vector<std::uint8_t> sid;
    const bool valid = enabled
        ? query_service_config(service, config, start_type, sid, error)
        : query_service_definition(service, config, start_type, sid, error);
    const bool changed = valid && ChangeServiceConfigW(
        service, SERVICE_NO_CHANGE,
        enabled ? SERVICE_AUTO_START : SERVICE_DISABLED,
        SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr);
    if (valid && !changed)
        error = windows_error("changing Spirula startup configuration", GetLastError());
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return changed ? success() : failure(error);
}

Result enable_startup() { return set_windows_startup(true); }
Result disable_startup() { return set_windows_startup(false); }

Status query_status() {
    SC_HANDLE manager = nullptr, service = nullptr;
    std::string error;
    if (!open_service(manager, service, SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS,
                      error)) {
        if (error == "Spirula service is not installed") {
            Status result;
            result.state = State::NotInstalled;
            return result;
        }
        return status_failure(error);
    }
    Configuration config;
    DWORD start_type = 0;
    std::vector<std::uint8_t> sid;
    if (!query_service_definition(service, config, start_type, sid, error)) {
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return status_failure(error);
    }
    SERVICE_STATUS_PROCESS current{};
    DWORD bytes = 0;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<BYTE*>(&current),
                              sizeof(current), &bytes)) {
        error = windows_error("querying the Spirula service", GetLastError());
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return status_failure(error);
    }
    Status result;
    result.configuration = std::move(config);
    result.startup_enabled = start_type == SERVICE_AUTO_START;
    switch (current.dwCurrentState) {
        case SERVICE_STOPPED:
            result.state = current.dwWin32ExitCode == NO_ERROR
                ? State::Stopped : State::Failed;
            break;
        case SERVICE_START_PENDING: result.state = State::StartPending; break;
        case SERVICE_RUNNING: result.state = State::Running; break;
        case SERVICE_STOP_PENDING: result.state = State::StopPending; break;
        case SERVICE_PAUSED: result.state = State::Paused; break;
        default: result.state = State::Unknown; break;
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return result;
}

Result broker_worker_configuration(Configuration& configuration) {
    SC_HANDLE manager = nullptr;
    SC_HANDLE service = nullptr;
    std::string error;
    if (!open_service(manager, service, SERVICE_QUERY_CONFIG, error))
        return failure(error);
    DWORD start_type = 0;
    std::vector<std::uint8_t> sid;
    const bool okay = query_service_config(service, configuration, start_type,
                                           sid, error);
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if (!okay) return failure(error);
    if (start_type != SERVICE_AUTO_START)
        return failure("worker service must start automatically for broker activation and post-reboot health recovery");
    return success();
}

Result verify_broker_worker_control() {
    SC_HANDLE manager = nullptr;
    SC_HANDLE service = nullptr;
    std::string error;
    if (!open_service(manager, service, READ_CONTROL, error))
        return failure(error);
    DWORD needed = 0;
    QueryServiceObjectSecurity(service, DACL_SECURITY_INFORMATION, nullptr, 0,
                               &needed);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !needed) {
        error = windows_error("reading restricted worker SCM permissions",
                              GetLastError());
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return failure(error);
    }
    std::vector<std::uint8_t> bytes(needed);
    auto* descriptor = reinterpret_cast<PSECURITY_DESCRIPTOR>(bytes.data());
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    PACL dacl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    bool okay = QueryServiceObjectSecurity(
                    service, DACL_SECURITY_INFORMATION, descriptor, needed,
                    &needed) != FALSE &&
                GetSecurityDescriptorControl(descriptor, &control, &revision) &&
                (control & SE_DACL_PROTECTED) &&
                GetSecurityDescriptorDacl(descriptor, &present, &dacl,
                                          &defaulted) &&
                present && dacl;
    ACL_SIZE_INFORMATION info{};
    okay = okay && GetAclInformation(dacl, &info, sizeof(info),
                                     AclSizeInformation) &&
           info.AceCount == 3;
    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> system{}, admins{}, auth{};
    DWORD system_size = static_cast<DWORD>(system.size());
    DWORD admins_size = static_cast<DWORD>(admins.size());
    DWORD auth_size = static_cast<DWORD>(auth.size());
    okay = okay &&
        CreateWellKnownSid(WinLocalSystemSid, nullptr, system.data(),
                           &system_size) &&
        CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins.data(),
                           &admins_size) &&
        CreateWellKnownSid(WinAuthenticatedUserSid, nullptr, auth.data(),
                           &auth_size);
    bool have_system = false, have_admins = false, have_auth = false;
    for (DWORD i = 0; okay && i < info.AceCount; ++i) {
        void* raw = nullptr;
        if (!GetAce(dacl, i, &raw)) { okay = false; break; }
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || header->AceFlags) {
            okay = false;
            break;
        }
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
        PSID sid = const_cast<DWORD*>(&ace->SidStart);
        if (EqualSid(sid, system.data()) && ace->Mask == 0x000F01FF &&
            !have_system)
            have_system = true;
        else if (EqualSid(sid, admins.data()) && ace->Mask == 0x000F01FF &&
                 !have_admins)
            have_admins = true;
        else if (EqualSid(sid, auth.data()) && ace->Mask == 0x00020085 &&
                 !have_auth)
            have_auth = true;
        else
            okay = false;
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return okay && have_system && have_admins && have_auth
        ? success()
        : failure("fixed worker SCM DACL is not the exact protected SYSTEM/Administrators/query-only contract");
}

Result protect_worker_service_control() {
    if (Result admin = require_admin(); !admin.success) return admin;
    Configuration configuration;
    if (Result valid = broker_worker_configuration(configuration);
        !valid.success)
        return valid;
    SC_HANDLE manager = nullptr;
    SC_HANDLE service = nullptr;
    std::string error;
    if (!open_service(manager, service, READ_CONTROL | WRITE_DAC, error))
        return failure(error);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const wchar_t* sddl =
        L"D:P(A;;0x000F01FF;;;SY)(A;;0x000F01FF;;;BA)(A;;0x00020085;;;AU)";
    const bool built = ConvertStringSecurityDescriptorToSecurityDescriptorW(
        sddl, SDDL_REVISION_1, &descriptor, nullptr) != FALSE;
    const bool set = built && SetServiceObjectSecurity(
        service,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        descriptor) != FALSE;
    if (descriptor) LocalFree(descriptor);
    if (!set) error = windows_error("protecting worker service control rights",
                                    GetLastError());
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if (!set) return failure(error);
    return verify_broker_worker_control();
}

Result validate_broker_worker_candidate(const Configuration& candidate) {
    if (Result acl = verify_broker_worker_control(); !acl.success) return acl;
    Configuration current;
    if (Result active = broker_worker_configuration(current); !active.success)
        return active;
    if (!same_path(candidate.config_root, current.config_root) ||
        !same_path(candidate.state_root, current.state_root) ||
        !same_path(candidate.storage_root, current.storage_root))
        return failure("broker candidate must preserve the fixed worker roots");
    std::vector<std::uint8_t> sid;
    std::string error;
    if (!service_sid(sid, error) ||
        !validate_configuration(candidate, sid.data(), error) ||
        !verify_service_permissions(candidate, sid.data(), error))
        return failure(error);
    return success();
}

Result validate_state_security(const fs::path& state_root) {
    std::vector<std::uint8_t> sid;
    std::string error;
    if (!service_sid(sid, error)) return failure(error);
    if (!win_absolute_canonical(state_root, true, "state root", sid.data(),
                                true, error))
        return failure(error);
    constexpr ACCESS_MASK write_tree = FILE_GENERIC_READ | FILE_GENERIC_WRITE |
                                       FILE_GENERIC_EXECUTE | DELETE |
                                       FILE_DELETE_CHILD;
    if (!acl_tree_has_rights(state_root, sid.data(), write_tree)) {
        error = "state root does not grant the restricted service account the required access";
        return failure(error);
    }
    return validate_state_file_permissions(state_root, sid.data(), error)
        ? success() : failure(error);
}
bool is_managed_service_process() { return false; }
#elif defined(__linux__)

Result install(const Configuration& configuration) {
    if (Result admin = require_admin(); !admin.success) return admin;
    ServiceAccount account;
    std::string error;
    if (!lookup_service_account(account, error) ||
        !validate_configuration(configuration, account, error))
        return failure(error);
    if (!safe_manager_directory(kSystemdDir, error)) return failure(error);

    const char* search_dirs[] = {
        "/etc/systemd/system", "/run/systemd/system",
        "/usr/local/lib/systemd/system", "/usr/lib/systemd/system",
        "/lib/systemd/system"};
    for (const char* dir : search_dirs) {
        const fs::path candidate = fs::path(dir) / kUnitName;
        if (systemd_entry_exists(candidate))
            return failure("a systemd unit with the Spirula service name already exists; refusing to replace it");
    }
    if (!systemd_absent_is_clean(error)) return failure(error);
    if (!write_exclusive_definition(kSystemdUnitPath,
                                    make_systemd_unit(configuration), error))
        return failure(error);
    const CommandResult reload = run_manager(
        {"/usr/bin/systemctl", "--system", "--no-ask-password", "daemon-reload"});
    if (!reload.success) {
        unlink(kSystemdUnitPath);
        (void)run_manager({"/usr/bin/systemctl", "--system",
                           "--no-ask-password", "daemon-reload"});
        return failure(reload.error);
    }
    if (!require_managed_systemd_unit(error)) {
        unlink(kSystemdUnitPath);
        (void)run_manager({"/usr/bin/systemctl", "--system",
                           "--no-ask-password", "daemon-reload"});
        return failure(error);
    }
    const CommandResult enabled = run_manager(
        {"/usr/bin/systemctl", "--system", "--no-ask-password", "enable", kUnitName});
    if (!enabled.success) {
        (void)run_manager({"/usr/bin/systemctl", "--system", "--no-ask-password",
                           "disable", kUnitName});
        unlink(kSystemdUnitPath);
        (void)run_manager({"/usr/bin/systemctl", "--system",
                           "--no-ask-password", "daemon-reload"});
        return failure(enabled.error);
    }
    return success();
}

Result uninstall() {
    if (Result admin = require_admin(); !admin.success) return admin;
    Configuration config;
    bool exists = false;
    std::string error;
    if (!managed_systemd_config(config, exists, error, false))
        return failure(error);
    if (!exists)
        return systemd_absent_is_clean(error) ? success() : failure(error);
    if (!require_managed_systemd_unit(error, false)) return failure(error);
    const CommandResult removed = run_manager(
        {"/usr/bin/systemctl", "--system", "--no-ask-password",
         "disable", "--now", kUnitName});
    if (!removed.success) return failure(removed.error);
    if (unlink(kSystemdUnitPath) != 0)
        return failure("service stopped, but its managed unit definition could not be removed");
    const CommandResult reload = run_manager(
        {"/usr/bin/systemctl", "--system", "--no-ask-password", "daemon-reload"});
    return reload.success ? success() : failure(reload.error);
}

Result start() {
    if (Result admin = require_admin(); !admin.success) return admin;
    std::string error;
    if (!require_managed_systemd_unit(error)) return failure(error);
    const CommandResult result = run_manager(
        {"/usr/bin/systemctl", "--system", "--no-ask-password",
         "start", kUnitName});
    return result.success ? success() : failure(result.error);
}

Result stop() {
    if (Result admin = require_admin(); !admin.success) return admin;
    std::string error;
    if (!require_managed_systemd_unit(error, false)) return failure(error);
    const CommandResult result = run_manager(
        {"/usr/bin/systemctl", "--system", "--no-ask-password",
         "stop", kUnitName});
    return result.success ? success() : failure(result.error);
}

Result set_systemd_startup(bool enabled) {
    if (Result admin = require_admin(); !admin.success) return admin;
    std::string error;
    if (!require_managed_systemd_unit(error, enabled)) return failure(error);
    const CommandResult result = enabled
        ? run_manager({"/usr/bin/systemctl", "--system", "--no-ask-password",
                       "enable", kUnitName})
        : run_manager({"/usr/bin/systemctl", "--system", "--no-ask-password",
                       "disable", kUnitName});
    return result.success ? success() : failure(result.error);
}

Result enable_startup() { return set_systemd_startup(true); }
Result disable_startup() { return set_systemd_startup(false); }

Status query_status() {
    Configuration config;
    bool exists = false;
    std::string error;
    if (!managed_systemd_config(config, exists, error, false))
        return status_failure(error);
    if (!exists) {
        if (!systemd_absent_is_clean(error)) return status_failure(error);
        Status result;
        result.state = State::NotInstalled;
        return result;
    }
    const CommandResult shown = systemd_show();
    if (!shown.success) return status_failure(shown.error);
    const auto values = parse_properties(shown.output);
    if (!systemd_effective_config_matches(values, config, error))
        return status_failure(error);
    Status result;
    result.configuration = std::move(config);
    const auto active = values.find("ActiveState");
    if (active == values.end()) {
        result.state = State::Unknown;
    } else if (active->second == "active") {
        result.state = State::Running;
    } else if (active->second == "activating") {
        result.state = State::StartPending;
    } else if (active->second == "deactivating") {
        result.state = State::StopPending;
    } else if (active->second == "failed") {
        result.state = State::Failed;
    } else if (active->second == "inactive") {
        result.state = State::Stopped;
    } else {
        result.state = State::Unknown;
    }
    const auto startup = values.find("UnitFileState");
    result.startup_enabled = startup != values.end() &&
        (startup->second == "enabled" || startup->second == "enabled-runtime");
    return result;
}

bool is_managed_service_process() {
    ServiceAccount account;
    std::string error;
    if (!lookup_service_account(account, error) ||
        ::geteuid() != account.uid)
        return false;
    Configuration config;
    bool exists = false;
    if (!managed_systemd_config(config, exists, error) || !exists)
        return false;
    const CommandResult shown = systemd_show();
    if (!shown.success) return false;
    const auto values = parse_properties(shown.output);
    const auto main_pid = values.find("MainPID");
    if (main_pid == values.end() ||
        main_pid->second != std::to_string(::getpid()) ||
        !systemd_effective_config_matches(values, config, error))
        return false;
    const auto active = values.find("ActiveState");
    return active != values.end() && active->second == "active";
}

#elif defined(__APPLE__)

Result install(const Configuration& configuration) {
    if (Result admin = require_admin(); !admin.success) return admin;
    ServiceAccount account;
    std::string error;
    if (!lookup_service_account(account, error) ||
        !validate_configuration(configuration, account, error))
        return failure(error);
    if (!safe_manager_directory(kLaunchdDir, error)) return failure(error);
    struct stat existing{};
    if (lstat(kLaunchdPath, &existing) == 0)
        return failure("a LaunchDaemon with the Spirula service name already exists; refusing to replace it");
    if (errno != ENOENT)
        return failure("cannot inspect the LaunchDaemon destination");
    const CommandResult loaded = launchd_print();
    if (loaded.success)
        return failure("the Spirula LaunchDaemon label is already loaded; refusing to replace it");
    if (!launchd_not_loaded(loaded)) return failure(loaded.error);

    if (!write_exclusive_definition(kLaunchdPath,
                                    make_launchd_plist(configuration), error))
        return failure(error);
    const CommandResult enabled = run_manager(
        {"/bin/launchctl", "enable", kLaunchdTarget});
    if (!enabled.success) {
        unlink(kLaunchdPath);
        return failure(enabled.error);
    }
    return success();
}

Result uninstall() {
    if (Result admin = require_admin(); !admin.success) return admin;
    Configuration config;
    bool exists = false;
    std::string error;
    if (!managed_launchd_config(config, exists, error, false))
        return failure(error);
    if (!exists)
        return launchd_absent_is_clean(error) ? success() : failure(error);
    const CommandResult loaded = launchd_print();
    if (loaded.success) {
        if (!launchd_effective_config_matches(loaded.output, config))
            return failure("launchd's loaded job identity or effective configuration differs from the managed plist; refusing to control it");
        const CommandResult stopped = run_manager(
            {"/bin/launchctl", "bootout", kLaunchdTarget});
        if (!stopped.success) return failure(stopped.error);
    } else if (!launchd_not_loaded(loaded)) {
        return failure(loaded.error);
    }
    const CommandResult disabled = run_manager(
        {"/bin/launchctl", "disable", kLaunchdTarget});
    if (!disabled.success) return failure(disabled.error);
    if (unlink(kLaunchdPath) != 0)
        return failure("service unloaded, but its managed LaunchDaemon definition could not be removed");
    return success();
}

Result start() {
    if (Result admin = require_admin(); !admin.success) return admin;
    std::string error;
    if (!require_managed_launchd(error)) return failure(error);
    const CommandResult loaded = launchd_print();
    if (loaded.success) {
        const std::string output = lower_ascii(loaded.output);
        if (output.find("state = running") != std::string::npos) return success();
        const CommandResult result = run_manager(
            {"/bin/launchctl", "kickstart", kLaunchdTarget});
        return result.success ? success() : failure(result.error);
    }
    if (!launchd_not_loaded(loaded)) return failure(loaded.error);
    bool startup_enabled = false;
    if (!launchd_startup_enabled(startup_enabled, error)) return failure(error);
    const bool temporarily_enabled = !startup_enabled;
    if (temporarily_enabled) {
        const CommandResult enabled = run_manager(
            {"/bin/launchctl", "enable", kLaunchdTarget});
        if (!enabled.success) return failure(enabled.error);
    }
    const CommandResult result = run_manager(
        {"/bin/launchctl", "bootstrap", "system", kLaunchdPath});
    if (!result.success) {
        if (temporarily_enabled) {
            const CommandResult restored = run_manager(
                {"/bin/launchctl", "disable", kLaunchdTarget});
            if (!restored.success)
                return failure(result.error +
                    "; could not restore the disabled startup state: " +
                    restored.error);
        }
        return failure(result.error);
    }
    if (temporarily_enabled) {
        const CommandResult restored = run_manager(
            {"/bin/launchctl", "disable", kLaunchdTarget});
        if (!restored.success)
            return failure("service started, but its disabled startup state "
                           "could not be restored: " + restored.error);
    }
    return success();
}

Result stop() {
    if (Result admin = require_admin(); !admin.success) return admin;
    std::string error;
    if (!require_managed_launchd(error, false)) return failure(error);
    const CommandResult loaded = launchd_print();
    if (!loaded.success)
        return launchd_not_loaded(loaded) ? success() : failure(loaded.error);
    const CommandResult result = run_manager(
        {"/bin/launchctl", "bootout", kLaunchdTarget});
    return result.success ? success() : failure(result.error);
}

Result set_launchd_startup(bool enabled) {
    if (Result admin = require_admin(); !admin.success) return admin;
    std::string error;
    if (!require_managed_launchd(error, enabled)) return failure(error);
    const CommandResult result = enabled
        ? run_manager({"/bin/launchctl", "enable", kLaunchdTarget})
        : run_manager({"/bin/launchctl", "disable", kLaunchdTarget});
    return result.success ? success() : failure(result.error);
}

Result enable_startup() { return set_launchd_startup(true); }
Result disable_startup() { return set_launchd_startup(false); }

Status query_status() {
    Configuration config;
    bool exists = false;
    std::string error;
    if (!managed_launchd_config(config, exists, error, false))
        return status_failure(error);
    if (!exists) {
        if (!launchd_absent_is_clean(error)) return status_failure(error);
        Status result;
        result.state = State::NotInstalled;
        return result;
    }
    const CommandResult printed = launchd_print();
    Status result;
    if (printed.success) {
        if (!launchd_effective_config_matches(printed.output, config))
            return status_failure("launchd's loaded job identity or effective configuration differs from the managed plist");
        const std::string output = lower_ascii(printed.output);
        if (output.find("state = running") != std::string::npos)
            result.state = State::Running;
        else if (output.find("state = spawn scheduled") != std::string::npos)
            result.state = State::StartPending;
        else
            result.state = State::Stopped;
    } else if (launchd_not_loaded(printed)) {
        result.state = State::Stopped;
    } else {
        return status_failure(printed.error);
    }
    if (!launchd_startup_enabled(result.startup_enabled, error))
        return status_failure(error);
    result.configuration = std::move(config);
    return result;
}

bool is_managed_service_process() {
    ServiceAccount account;
    std::string error;
    if (!lookup_service_account(account, error) ||
        ::geteuid() != account.uid)
        return false;
    Configuration config;
    bool exists = false;
    if (!managed_launchd_config(config, exists, error) || !exists)
        return false;
    const CommandResult loaded = launchd_print();
    if (!loaded.success ||
        !launchd_effective_config_matches(loaded.output, config))
        return false;
    bool running = false;
    bool current_pid = false;
    std::istringstream input(loaded.output);
    std::string raw_line;
    while (std::getline(input, raw_line)) {
        const std::string line = trim_manager_line(raw_line);
        if (line.compare(0, 8, "state = ") == 0) {
            if (running || line != "state = running") return false;
            running = true;
        } else if (line.compare(0, 6, "pid = ") == 0) {
            if (current_pid ||
                line.substr(6) != std::to_string(::getpid()))
                return false;
            current_pid = true;
        }
    }
    return running && current_pid;
}

#else

Result unsupported() {
    return failure("native agent service management is supported only on Windows, Linux systemd, and macOS LaunchDaemon");
}
Result validate_state_security(const fs::path&) { return unsupported(); }
bool is_managed_service_process() { return false; }

Result install(const Configuration&) { return unsupported(); }
Result uninstall() { return unsupported(); }
Result start() { return unsupported(); }
Result stop() { return unsupported(); }
Result enable_startup() { return unsupported(); }
Result disable_startup() { return unsupported(); }
Status query_status() { return status_failure(unsupported().error); }

#endif

#ifndef _WIN32
Result broker_worker_configuration(Configuration&) {
    return failure("the LocalSystem administration broker is supported only on Windows");
}
Result protect_worker_service_control() {
    return failure("the LocalSystem administration broker is supported only on Windows");
}
Result verify_broker_worker_control() {
    return failure("the LocalSystem administration broker is supported only on Windows");
}
Result validate_broker_worker_candidate(const Configuration&) {
    return failure("the LocalSystem administration broker is supported only on Windows");
}
#endif
}  // namespace app::agent::service
