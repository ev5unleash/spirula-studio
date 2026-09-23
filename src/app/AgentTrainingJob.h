#pragma once

#include "app/AgentTransfer.h"
#include "config/TrainConfig.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace app::agent {

enum class TrainingMode { Fresh, Resume };

struct TrainingInputBundle {
    std::filesystem::path workspace_root;
    std::filesystem::path root;                 // <workspace>/input
    std::filesystem::path dataset_root;          // <workspace>/input/dataset
    std::filesystem::path config_path;
    std::filesystem::path resume_checkpoint;     // empty for Fresh
    std::filesystem::path output_root;           // <workspace>/output/run
    TrainingMode mode = TrainingMode::Fresh;
    std::string preset;
    TrainConfig config;                          // paths are workspace-relative
    std::string identity_sha256;
    std::vector<TransferFile> files;
};

// Worker-only typed invocation. Run `spirula train` with this complete config,
// the named preset, and working_directory as the process cwd. The fields are
// deliberately typed rather than accepted as peer-provided argv tokens.
struct TrainingInvocationOptions {
    TrainingMode mode = TrainingMode::Fresh;
    std::string preset;
    TrainConfig config;
    std::filesystem::path working_directory;
    std::filesystem::path output_directory;
    std::filesystem::path resume_checkpoint;
};

struct TrainingOutputPackage {
    std::vector<TransferFile> files;
    std::string input_identity_sha256;
    std::string returned_checkpoint;             // exact step-*.ckpt name
};

struct TrainingOutputBundle {
    std::filesystem::path root;
    std::filesystem::path config_path;
    std::filesystem::path checkpoint_dir;
    std::filesystem::path splat_path;
    std::string returned_checkpoint;
    std::string preset;
    std::string input_identity_sha256;
    TrainConfig resume_config;                    // rebound to local input/output
    std::vector<TransferFile> files;              // local hashes after rebinding
    std::vector<TransferFile> worker_files;       // exact received manifest
};

// Stages dataset and optional resumable checkpoint under an empty attempt root.
// Only config.data is portable; the copied dataset is parser-validated.
TrainingInputBundle StageTrainingInputs(
    const TrainConfig& config, const std::string& preset,
    const std::filesystem::path& resume_checkpoint,
    const std::filesystem::path& attempt_root,
    std::uint64_t disk_budget_bytes);

// Verifies the exact received input tree and its digests/identity, parses the
// frozen config, and validates the staged dataset with the existing parser.
// input_root is the `input/` child of the attempt workspace.
TrainingInputBundle VerifyTrainingInputs(
    const std::filesystem::path& input_root,
    const std::vector<TransferFile>& approved_files,
    const std::string& expected_identity_sha256,
    std::uint64_t disk_budget_bytes);

// Returns only typed, frozen options. Caller must run from working_directory
// and explicitly pass every TrainConfig field to the existing train CLI; this
// is required for --resume so checkpoint defaults cannot override the package.
TrainingInvocationOptions MakeTrainingInvocation(
    const TrainingInputBundle& inputs);

// The worker calls this only after successful training and names the exact
// checkpoint to return. It rejects unexpected files, linked paths, invalid
// config/checkpoint files, and any checkpoint that is not resumable.
TrainingOutputPackage TrainingOutputManifest(
    const TrainingInputBundle& inputs,
    const std::filesystem::path& output_root,
    const std::string& returned_checkpoint,
    std::uint64_t disk_budget_bytes);

// Verifies received output and rebinds configs to the leader's staged dataset.
// `worker_files` retain wire hashes; `files` describe the rewritten local tree.
TrainingOutputBundle VerifyTrainingOutputs(
    const TrainingInputBundle& inputs,
    const std::filesystem::path& result_root,
    const TrainingOutputPackage& approved_output,
    std::uint64_t disk_budget_bytes);

}  // namespace app::agent
