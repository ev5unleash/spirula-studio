#pragma once
#include "config/TrainConfig.h"
#include "data/Json.h"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ckpt {
inline constexpr uint64_t b1_sampler_seed = 0x423153414d504c31ULL;
struct B1Contract {
    int version = 2, owner = -1, sampler_version = 1;
    int64_t source_step = 0, completed_step = 0, horizon = 0, rows = 0;
    std::string source_state_sha256, config_sha256, views_sha256, train_indices_sha256;
    std::string view_policy = "partitioned", full_train_indices_sha256;
    std::string mask_sha256, frozen_context, frozen_shared_sha256;
    std::string blocks_sha256, owned_ids_sha256, excluded_ids_sha256;
    std::string ply_sha256;
    int64_t refresh_cadence = 0, refresh_generation = 0, refresh_parent = 0;
    std::string refresh_group_sha256, refresh_payload0_sha256, refresh_payload1_sha256;
    std::string refresh_commit_sha256;
};
std::string b1_config_hash(const TrainConfig&);
std::string b1_indices_hash(const std::vector<int32_t>&);
std::string b1_file_hash(const std::filesystem::path&);
std::string b1_ply_body_hash(const std::filesystem::path&);
std::string b1_shared_hash(const std::filesystem::path& checkpoint);
JsonValue b1_contract_json(const B1Contract&);
B1Contract parse_b1_contract(const JsonValue&);
std::optional<B1Contract> read_b1_contract(const std::filesystem::path& checkpoint);
void validate_b1_pair(const B1Contract&, const B1Contract&);
std::string serialize_b1_contract(const B1Contract&);
void validate_b1_appearance(const std::filesystem::path& merged_ply, const std::filesystem::path& source_checkpoint);
std::string b1_appearance_comment(const B1Contract&, const std::string& body_sha256);
void bind_b1_sidecars(B1Contract&, const std::filesystem::path& checkpoint);
}
