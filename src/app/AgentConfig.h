#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace app::agent {

// Machine-scoped policy. Loading it performs no network, authentication, or
// Vulkan initialization; the leader is unusable until pairing exists.
struct Config {
    std::string leader_address;
    std::string leader_server_name;
    std::uint16_t leader_port = 0;
    // Stable CA-SPKI identity of the locally approved paired leader.
    std::string leader_id;
    std::vector<std::string> allowed_vulkan_uuids;
    std::uint32_t max_concurrent_jobs = 0;
    std::uint64_t disk_budget_bytes = 0;
    bool allow_remote_update = false;
    bool allow_reboot = false;
    // SHA-256 of the detached update-signing public key's DER SubjectPublicKeyInfo.
    std::string update_signer_sha256;
};

// Reads agent-policy.json from an explicit, existing machine config root.
// Missing or unsafe policy always throws; no user-profile/default fallback.
Config load_config(const std::filesystem::path& config_root);
// Writes agent-policy.json only when explicitly requested by an elevated
// local administrator. The existing absolute config root is never created.
void save_config(const std::filesystem::path& config_root, const Config& config);

}  // namespace app::agent
