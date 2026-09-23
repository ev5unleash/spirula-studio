#pragma once

#include "app/AgentServiceManager.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>
namespace app::agent {
struct Config;
namespace pairing {
class Leader;
class Worker;
}
}

namespace app::agent::update {

inline constexpr std::uint64_t kMaxPackageBytes = 1024ULL * 1024 * 1024;

struct PlatformIdentity {
    std::string os;
    std::string architecture;
    std::string build;
};

// Manifest values are untrusted until their complete offer is authorized.
// SHA-256 provides package integrity, not authorship.
struct PackageManifest {
    std::string os;
    std::string architecture;
    std::string build;
    std::string release;
    std::uint64_t size = 0;
    std::string sha256;
};

struct PackageOffer {
    std::string worker_id;
    std::string leader_id;
    std::uint64_t expires_at_unix = 0;
    std::string update_id;
    PackageManifest manifest;
    std::string signer_public_key_pem;
    std::vector<std::uint8_t> signature;
};

namespace detail {
struct OfferAuthority;
}

// Can only be produced by AuthorizePackageOffer; staging accepts no raw manifest.
class AuthorizedPackageOffer final {
public:
    const PackageManifest& manifest() const noexcept { return manifest_; }
    const std::string& update_id() const noexcept { return update_id_; }
    const std::string& worker_id() const noexcept { return worker_id_; }
    const std::string& leader_id() const noexcept { return leader_id_; }
    const std::string& signer_sha256() const noexcept { return signer_sha256_; }
    std::uint64_t expires_at_unix() const noexcept { return expires_at_unix_; }
private:
    AuthorizedPackageOffer(PackageManifest manifest, std::string update_id,
                           std::string worker_id, std::string leader_id,
                           std::string signer_sha256,
                           std::uint64_t expires_at_unix)
        : manifest_(std::move(manifest)), update_id_(std::move(update_id)),
          worker_id_(std::move(worker_id)), leader_id_(std::move(leader_id)),
          signer_sha256_(std::move(signer_sha256)),
          expires_at_unix_(expires_at_unix) {}
    PackageManifest manifest_;
    std::string update_id_;
    std::string worker_id_;
    std::string leader_id_;
    std::string signer_sha256_;
    std::uint64_t expires_at_unix_ = 0;
    friend struct detail::OfferAuthority;
};
std::optional<PackageOffer> SignPackageOffer(
    pairing::Leader& leader, const std::string& worker_id,
    std::uint64_t expires_at_unix, std::string update_id,
    PackageManifest manifest, std::string* error = nullptr);

// consumed_update_ids must come from the caller's durable replay journal; the
// caller must durably record the returned offer ID before invoking StageUpdatePackage.
std::optional<AuthorizedPackageOffer> AuthorizePackageOffer(
    const Config& local_policy, const pairing::Worker& paired_worker,
    const PackageOffer& offer,
    const std::vector<std::string>& consumed_update_ids,
    std::string* error = nullptr);


enum class StageError {
    None,
    InvalidArgument,
    InvalidManifest,
    Incompatible,
    TooLarge,
    DiskSpace,
    Filesystem,
    Integrity,
    Conflict,
};

struct StageResult {
    StageError error = StageError::None;
    std::string message;
    // Set only when the complete package has been verified and published.
    std::filesystem::path staged_executable;
    explicit operator bool() const noexcept { return error == StageError::None; }
};

PlatformIdentity CurrentPlatformIdentity();

// Stages only an offer that passed detached-signature and local-policy
// authorization. This remains staging only; activation is a separate privilege boundary.
StageResult StageUpdatePackage(
    const service::Configuration& configuration,
    const Config& local_policy,
    const pairing::Worker& paired_worker,
    const std::filesystem::path& package_root,
    const std::filesystem::path& package_relative_path,
    const AuthorizedPackageOffer& authorized_offer);

}  // namespace app::agent::update
