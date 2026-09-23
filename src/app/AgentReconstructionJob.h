#pragma once

#include "app/AgentTransfer.h"
#include "sfm/core/Manifest.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sfm {
struct AutoRequest;
}

namespace app::agent {

struct ReconstructionInputBundle {
    std::filesystem::path root;
    std::filesystem::path manifest_path;
    std::filesystem::path request_path;
    std::filesystem::path image_root;
    std::filesystem::path mask_root;
    std::filesystem::path metric_positions_path;
    sfm::Manifest manifest;
    std::string build_id;
    std::string source_manifest_sha256;
    std::string identity_sha256;
    std::vector<TransferFile> files;
};

// SS_VERSION includes the configured project version and, when available, the
// source commit. Inputs and outputs are accepted only by the exact same build.
const std::string& ReconstructionBuildIdentity();

// The source manifest is hashed; the staged copy rebases every reference.
// The parsed AutoRequest must not use feature-plan mode; attempt_root is empty.
ReconstructionInputBundle StageReconstructionInputs(
    const sfm::AutoRequest& source,
    const std::filesystem::path& source_manifest,
    const std::filesystem::path& attempt_root,
    std::uint64_t disk_budget_bytes);

// Rechecks the exact approved tree, every file digest, the portable manifest,
// the bundle identity and build compatibility before returning local paths.
ReconstructionInputBundle VerifyReconstructionInputs(
    const std::filesystem::path& attempt_root,
    const std::vector<TransferFile>& approved_files,
    const std::string& expected_identity_sha256,
    std::uint64_t disk_budget_bytes);

// Rebuilds typed SfM options from staged paths, never peer-provided argv.
// The worker selects a leased device and finalizes before run_auto.
sfm::AutoRequest DecodeReconstructionRequest(
    const ReconstructionInputBundle& inputs,
    const std::filesystem::path& workspace,
    std::uint64_t disk_budget_bytes);

// Returns hashes only for sparse models accepted by both production parsers.
// This does not publish artifacts into a dataset.
std::vector<TransferFile> ReconstructionOutputManifest(
    const ReconstructionInputBundle& inputs,
    const std::filesystem::path& sparse_root,
    std::uint64_t disk_budget_bytes);

// Receiver-side check before an output staging tree may be published. It binds
// model image references to the verified input bundle and loads each sparse
// model through both existing parsers.
void VerifyReconstructionOutputs(
    const ReconstructionInputBundle& inputs,
    const std::filesystem::path& sparse_root,
    const std::vector<TransferFile>& approved_files,
    std::uint64_t disk_budget_bytes);

}  // namespace app::agent
