#include "app/AgentConfig.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <iterator>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#else
#include <unistd.h>
#endif


namespace fs = std::filesystem;
namespace agent = app::agent;

namespace {

void write_policy(const fs::path& root, const std::string& text) {
    std::ofstream output(root / "agent-policy.json", std::ios::binary | std::ios::trunc);
    output << text;
    output.close();
    if (!output) throw std::runtime_error("could not write test policy");
}

std::string failure(const fs::path& root) {
    try {
        (void)agent::load_config(root);
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

std::string save_failure(const fs::path& root, const agent::Config& config) {
    try {
        agent::save_config(root, config);
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

std::string read_policy_text(const fs::path& root) {
    std::ifstream input(root / "agent-policy.json", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

bool local_admin() {
#ifdef _WIN32
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const bool elevated = GetTokenInformation(
        token, TokenElevation, &elevation, sizeof(elevation), &returned) &&
        elevation.TokenIsElevated;
    CloseHandle(token);
    if (!elevated) return false;
    std::array<unsigned char, SECURITY_MAX_SID_SIZE> admins{};
    DWORD size = static_cast<DWORD>(admins.size());
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
                            admins.data(), &size))
        return false;
    BOOL member = FALSE;
    return CheckTokenMembership(nullptr, admins.data(), &member) && member;
#else
    return ::geteuid() == 0;
#endif
}

#ifdef _WIN32
bool create_secure_test_root(const fs::path& path) {
    struct Token {
        HANDLE value = nullptr;
        ~Token() { if (value) CloseHandle(value); }
    } token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value))
        return false;

    DWORD token_bytes = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &token_bytes);
    if (token_bytes == 0) return false;
    std::vector<unsigned char> token_data(token_bytes);
    if (!GetTokenInformation(token.value, TokenUser, token_data.data(),
                             token_bytes, &token_bytes))
        return false;
    PSID user = reinterpret_cast<TOKEN_USER*>(token_data.data())->User.Sid;

    std::array<unsigned char, SECURITY_MAX_SID_SIZE> system{}, admins{};
    DWORD system_size = static_cast<DWORD>(system.size());
    DWORD admins_size = static_cast<DWORD>(admins.size());
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system.data(), &system_size) ||
        !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins.data(),
                            &admins_size))
        return false;

    PSID trustees[] = {user, system.data(), admins.data()};
    EXPLICIT_ACCESSW entries[3]{};
    for (std::size_t i = 0; i < 3; ++i) {
        entries[i].grfAccessPermissions = FILE_ALL_ACCESS;
        entries[i].grfAccessMode = SET_ACCESS;
        entries[i].grfInheritance = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
        entries[i].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entries[i].Trustee.TrusteeType =
            i == 2 ? TRUSTEE_IS_WELL_KNOWN_GROUP : TRUSTEE_IS_USER;
        entries[i].Trustee.ptstrName = reinterpret_cast<LPWSTR>(trustees[i]);
    }
    struct Acl {
        PACL value = nullptr;
        ~Acl() { if (value) LocalFree(value); }
    } acl;
    if (SetEntriesInAclW(3, entries, nullptr, &acl.value) != ERROR_SUCCESS)
        return false;

    SECURITY_DESCRIPTOR descriptor{};
    if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(&descriptor, TRUE, acl.value, FALSE) ||
        !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED,
                                      SE_DACL_PROTECTED))
        return false;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};
    return CreateDirectoryW(path.c_str(), &attributes) != FALSE;
}
#endif


}  // namespace

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", message);
        if (!ok) ++failures;
    };

    const fs::path requested_root = fs::temp_directory_path() /
        ("spirula-agent-config-" + std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
#ifdef _WIN32
        if (!create_secure_test_root(requested_root))
            throw std::runtime_error("could not create secure test config root");
#else
        fs::create_directories(requested_root);
#endif
        const fs::path root = fs::canonical(requested_root);
        check(failure(root) == "agent config: policy file is missing",
              "missing machine policy is reported distinctly");
        const std::string valid =
            "{\"schema_version\":1,\"leader\":{\"address\":\"192.168.10.7\","
            "\"server_name\":\"leader.lan\",\"port\":7443,"
            "\"id\":\"0123456789abcdef0123456789abcdef"
            "0123456789abcdef0123456789abcdef\"},"
            "\"allowed_vulkan_uuids\":[\"uuid:00112233445566778899aabbccddeeff\"],"
            "\"max_concurrent_jobs\":2,\"disk_budget_bytes\":8589934592,"
            "\"update_signer_sha256\":\"fedcba9876543210fedcba9876543210"
            "fedcba9876543210fedcba9876543210\"}";
        write_policy(root, valid);
        const agent::Config config = agent::load_config(root);

        check(config.leader_address == "192.168.10.7" &&
                  config.leader_server_name == "leader.lan" &&
                  config.leader_port == 7443 &&
                  config.leader_id ==
                      "0123456789abcdef0123456789abcdef"
                      "0123456789abcdef0123456789abcdef" &&
                  config.update_signer_sha256 ==
                      "fedcba9876543210fedcba9876543210"
                      "fedcba9876543210fedcba9876543210" &&
                  config.allowed_vulkan_uuids.size() == 1 &&
                  config.allowed_vulkan_uuids[0] ==
                      "uuid:00112233445566778899aabbccddeeff" &&
                  config.max_concurrent_jobs == 2 &&
                  config.disk_budget_bytes == 8589934592ULL &&
                  !config.allow_remote_update && !config.allow_reboot,
              "valid explicit machine policy loads and omitted grants deny by default");
        std::string granted = valid;
        granted.insert(granted.size() - 1,
                       ",\"grants\":{\"remote_update\":true,\"reboot\":true}");
        write_policy(root, granted);
        const agent::Config granted_config = agent::load_config(root);
        check(granted_config.allow_remote_update && granted_config.allow_reboot,
              "explicit update and reboot grants are preserved");
        write_policy(root, valid);
        std::string loopback = valid;
        const std::size_t address_at = loopback.find("192.168.10.7");
        loopback.replace(address_at, std::string("192.168.10.7").size(), "127.0.0.1");
        write_policy(root, loopback);
        check(agent::load_config(root).leader_address == "127.0.0.1",
              "loopback is accepted for isolated host-only runs");
        write_policy(root, valid);

        write_policy(root, "{\"schema_version\":1,");
        const std::string malformed_first = failure(root);
        check(!malformed_first.empty() && malformed_first == failure(root),
              "malformed JSON fails closed with a deterministic diagnostic");

        write_policy(root,
            "{\"schema_version\":1,\"schema_version\":1,\"leader\":{},"
            "\"allowed_vulkan_uuids\":[\"uuid:00112233445566778899aabbccddeeff\"],"
            "\"max_concurrent_jobs\":1,\"disk_budget_bytes\":1}");
        check(!failure(root).empty(), "duplicate JSON keys are rejected");
        std::string unknown = valid;
        unknown.insert(unknown.size() - 1, ",\"unexpected\":true");
        write_policy(root, unknown);
        check(!failure(root).empty(), "unknown policy fields are rejected");
        std::string excessive_jobs = valid;
        const std::string jobs_field = "\"max_concurrent_jobs\":2";
        const std::size_t jobs_at = excessive_jobs.find(jobs_field);
        excessive_jobs.replace(jobs_at, jobs_field.size(),
                               "\"max_concurrent_jobs\":65");
        write_policy(root, excessive_jobs);
        check(!failure(root).empty(), "concurrency outside the policy bound is rejected");

        std::string duplicate_uuid = valid;
        const std::string uuid = "\"uuid:00112233445566778899aabbccddeeff\"";
        const std::size_t uuid_at = duplicate_uuid.find(uuid);
        duplicate_uuid.replace(uuid_at, uuid.size(), uuid + "," + uuid);
        write_policy(root, duplicate_uuid);
        check(!failure(root).empty(), "duplicate Vulkan UUID selectors are rejected");

        std::string unsafe = valid;
        const std::string safe_host = "192.168.10.7";
        const std::size_t host_at = unsafe.find(safe_host);
        unsafe.replace(host_at, safe_host.size(), "https://leader.local/path");
        write_policy(root, unsafe);
        check(!failure(root).empty(), "URL and path syntax is rejected as a leader address");

        write_policy(root, valid);
        const bool is_admin = local_admin();
        if (is_admin) {
            fs::remove(root / "agent-policy.json");
            const std::string save_error = save_failure(root, granted_config);
            bool round_trip = false;
            if (save_error.empty()) {
                try {
                    const agent::Config saved = agent::load_config(root);
                    round_trip = saved.leader_address == granted_config.leader_address &&
                        saved.leader_server_name == granted_config.leader_server_name &&
                        saved.leader_port == granted_config.leader_port &&
                        saved.allowed_vulkan_uuids ==
                            granted_config.allowed_vulkan_uuids &&
                        saved.max_concurrent_jobs ==
                            granted_config.max_concurrent_jobs &&
                        saved.disk_budget_bytes == granted_config.disk_budget_bytes &&
                        saved.allow_remote_update && saved.allow_reboot;
                } catch (...) {
                }
            }
            check(round_trip, "elevated administrator policy save round-trips");

            const std::string corrupt = "{\"schema_version\":1,";
            write_policy(root, corrupt);
            agent::Config unsafe_config = granted_config;
            unsafe_config.leader_address = "https://leader.local/path";
            check(!save_failure(root, unsafe_config).empty() &&
                      read_policy_text(root) == corrupt,
                  "invalid input preserves an existing corrupt policy");

            write_policy(root, valid);
#ifndef _WIN32
            const fs::path policy = root / "agent-policy.json";
            const fs::path hardlink = root / "policy-link.json";
            fs::create_hard_link(policy, hardlink);
            const std::string before = read_policy_text(root);
            check(!save_failure(root, granted_config).empty() &&
                      read_policy_text(root) == before &&
                      fs::hard_link_count(policy) == 2,
                  "hard-linked policy target is rejected without overwrite");
            fs::remove(hardlink);
#endif
        } else {
            check(!save_failure(root, config).empty(),
                  "non-administrator policy save is rejected");
        }

#ifndef _WIN32
        const fs::path policy = root / "agent-policy.json";
        const fs::path target = root / "target.json";
        {
            std::ofstream output(target, std::ios::binary | std::ios::trunc);
            output << valid;
        }
        fs::remove(policy);
        fs::create_symlink(target, policy);
        check(!failure(root).empty(), "symlinked policy files are rejected");
        if (is_admin)
            check(!save_failure(root, config).empty() &&
                      fs::is_symlink(policy) && read_policy_text(root) == valid,
                  "symlinked policy target is rejected without overwrite");

        fs::remove(policy);
        write_policy(root, valid);
        fs::permissions(policy,
                        fs::perms::owner_read | fs::perms::owner_write |
                            fs::perms::group_write,
                        fs::perm_options::replace);
        check(!failure(root).empty(), "group-writable policy files are rejected");
#endif
    } catch (const std::exception& error) {
        std::printf("FAIL unexpected exception: %s\n", error.what());
        ++failures;
    }

    std::error_code ec;
    fs::remove_all(requested_root, ec);
    return failures ? 1 : 0;
}
