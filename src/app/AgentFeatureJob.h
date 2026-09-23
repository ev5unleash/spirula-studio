#pragma once

#include "app/AgentTransfer.h"
#include "sfm/core/FeatureWork.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace app::agent {

struct FeatureInputBundle {
    sfm::feature_work::FeaturePlan plan;
    sfm::feature_work::FeatureRequest request;
    std::filesystem::path bundle_root;
    std::filesystem::path plan_path;
    std::filesystem::path request_path;
    std::filesystem::path image_root;
    std::filesystem::path mask_root;
    std::vector<TransferFile> manifest;
};

// The attempt root must be an existing empty directory. Throws on invalid
// inputs, unsafe paths, digest mismatch, or a quota violation.
FeatureInputBundle StageFeatureInputs(
    const std::filesystem::path& plan_file,
    const std::filesystem::path& request_file,
    const std::filesystem::path& image_root,
    const std::filesystem::path& mask_root,
    const std::filesystem::path& attempt_root,
    std::uint64_t disk_budget_bytes);

// Validates the exact approved bundle before execution. The attempt root may
// contain the empty .agent-transfer-v1 directory left by ReceiveArtifacts.
FeatureInputBundle VerifyFeatureInputs(
    const std::filesystem::path& attempt_root,
    const sfm::feature_work::FeaturePlan& expected_plan,
    const sfm::feature_work::FeatureRequest& expected_request,
    const std::vector<TransferFile>& approved_manifest,
    std::uint64_t disk_budget_bytes);

// Post-completion only: builds the exact worker-output manifest and removes
// worker-local binding paths in place. A validated empty scheduler lease marker
// is excluded from the manifest. Do not call while result_root may be retried.
std::vector<TransferFile> FeatureOutputManifest(
    const std::filesystem::path& request_path,
    const std::filesystem::path& result_root,
    std::uint64_t disk_budget_bytes);

// Verifies against the leader's original request and exact approved manifest.
// Requires an exact received artifact tree; scheduler sidecars are not accepted.
// result_root is the leader-owned staging directory.
void VerifyFeatureOutputs(
    const std::filesystem::path& request_path,
    const std::filesystem::path& result_root,
    const std::vector<TransferFile>& approved_manifest,
    std::uint64_t disk_budget_bytes);

}  // namespace app::agent
