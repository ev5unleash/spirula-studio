#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace sfm::feature_work {

inline constexpr uint32_t kSchema = 1;

struct recipe {
    uint32_t schema = kSchema;
    std::string frontend;
    std::string descriptor;
    uint32_t descriptor_dim = 0;
    uint32_t descriptor_dtype = 0;
    std::string implementation;
    std::string model_digest;
    std::string shader_digest;
    std::map<std::string, std::string> settings;
    std::string digest;
};

struct plan_image {
    uint32_t global_index = 0;
    std::string logical_name;
    std::string feature_path;
    std::string source_digest;
    std::string mask_path;
    std::string mask_digest;
    std::string artifact_key;
    uint32_t owner_shard = 0;
    std::vector<std::string> chunks;
};

struct chunk {
    std::string id;
    std::vector<uint32_t> image_indices;
};

struct plan {
    uint32_t schema = kSchema;
    std::string dataset_digest;
    recipe extraction;
    std::vector<plan_image> images;
    std::vector<chunk> chunks;
    std::string capture_digest;
    std::string configuration_digest;
    std::string digest;
};

struct request {
    uint32_t schema = kSchema;
    std::string plan_digest;
    std::string plan_path;
    uint32_t shard = 0;
    std::string attempt_id;
    std::vector<uint32_t> image_indices;
    std::string supersedes;
    std::string digest;
};

struct worker_binding {
    uint32_t schema = kSchema;
    request extraction_request;
    std::string request_path;
    std::string image_root;
    std::string mask_root;
    std::string model_root;
    std::string device;
    std::string output_dir;
    uint64_t max_payload_bytes = 0;
    uint32_t max_images = 0;
    std::string digest;
};

enum class outcome_status : uint8_t { success, zero_features, failed };

struct receipt {
    uint32_t schema = kSchema;
    std::string request_digest;
    uint32_t global_index = 0;
    std::string logical_name;
    std::string feature_path;
    std::string artifact_key;
    std::string payload_digest;
    uint64_t payload_bytes = 0;
    uint32_t feature_count = 0;
    uint32_t descriptor_dtype = 0;
    uint32_t descriptor_dim = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t extract_width = 0;
    int32_t extract_height = 0;
    std::string producer_build;
    std::string producer_device;
    std::string digest;
};

struct image_outcome {
    uint32_t schema = kSchema;
    uint32_t global_index = 0;
    outcome_status status = outcome_status::failed;
    std::string receipt_digest;
    std::string error;
    std::string digest;
};

struct shard_result {
    uint32_t schema = kSchema;
    std::string plan_digest;
    std::string request_digest;
    uint32_t shard = 0;
    std::vector<image_outcome> outcomes;
    bool complete = false;
    std::string digest;
};

struct cancellation {
    uint32_t schema = kSchema;
    std::string plan_digest;
    std::string request_digest;
    uint32_t shard = 0;
    std::string reason;
    std::string cancellation_digest;
};

struct accepted_row {
    uint32_t global_index = 0;
    std::string logical_name;
    std::string feature_path;
    std::string artifact_key;
    std::string payload_digest;
    uint64_t payload_bytes = 0;
    uint32_t feature_count = 0;
    std::string receipt_digest;
    std::string result_digest;
};

struct collection_index {
    uint32_t schema = kSchema;
    std::string plan_digest;
    std::vector<accepted_row> rows;
    std::string collection_digest;
    bool complete = false;
    std::string digest;
};

using Recipe = recipe;
using PlanImage = plan_image;
using Chunk = chunk;
using FeaturePlan = plan;
using FeatureRequest = request;
using WorkerBinding = worker_binding;
using ImageReceipt = receipt;
using ImageOutcome = image_outcome;
using ShardResult = shard_result;
using FeatureCancellation = cancellation;
using AcceptedRow = accepted_row;
using CollectionIndex = collection_index;
using feature_recipe = recipe;
using feature_plan = plan;
using feature_request = request;
using host_worker_binding = worker_binding;
using image_receipt = receipt;
using outcome = image_outcome;

std::string normalizeRelativePath(const std::string& path);
std::string featurePathForImage(const std::string& logical_name);
std::string sha256Text(const std::string& bytes);
std::string sha256File(const std::string& path);
inline std::string normalizePath(const std::string& path) {
    return normalizeRelativePath(path);
}
inline std::string featurePath(const std::string& logical_name) {
    return featurePathForImage(logical_name);
}

std::string imageArtifactKey(const std::string& source_digest,
                             const std::string& mask_digest,
                             const recipe& extraction);
std::string datasetSnapshotDigest(const std::vector<plan_image>& images);
std::string recipeDigest(const recipe& value);
std::string planDigest(const plan& value);
std::string requestDigest(const request& value);
std::string workerBindingDigest(const worker_binding& value);
std::string receiptDigest(const receipt& value);
std::string outcomeDigest(const image_outcome& value);
std::string shardResultDigest(const shard_result& value);
std::string cancellationDigest(const cancellation& value);
std::string collectionDigest(const collection_index& value);

void validateRecipe(const recipe& value);
void validatePlan(const plan& value);
void validateRequest(const plan& value, const request& request_value);
void validateWorkerBinding(const plan& value, const worker_binding& value_binding);
void validateReceipt(const plan& value, const request& request_value,
                     const receipt& value_receipt,
                     const std::string& payload_root = {});
void validateCancellation(const plan& value, const request& request_value,
                          const cancellation& value_cancellation);
void validateShardResult(const plan& value, const request& request_value,
                         const shard_result& value_result,
                         const cancellation* cancelled = nullptr);
void validateCollection(const plan& value, const collection_index& value_index);

std::string writeRecipe(const recipe& value);
std::string writePlan(const plan& value);
std::string writeRequest(const request& value);
std::string writeWorkerBinding(const worker_binding& value);
std::string writeReceipt(const receipt& value);
std::string writeImageOutcome(const image_outcome& value);
std::string writeShardResult(const shard_result& value);
std::string writeCancellation(const cancellation& value);
std::string writeCollectionIndex(const collection_index& value);

recipe readRecipe(const std::string& json);
plan readPlan(const std::string& json);
request readRequest(const std::string& json);
worker_binding readWorkerBinding(const std::string& json);
receipt readReceipt(const std::string& json);
image_outcome readImageOutcome(const std::string& json);
shard_result readShardResult(const std::string& json);
cancellation readCancellation(const std::string& json);
collection_index readCollectionIndex(const std::string& json);

void publishAtomic(const std::string& path, const std::string& bytes);
void writeRecipeFile(const std::string& path, const recipe& value);
void writePlanFile(const std::string& path, const plan& value);
void writeRequestFile(const std::string& path, const request& value);
void writeWorkerBindingFile(const std::string& path, const worker_binding& value);
void writeReceiptFile(const std::string& path, const receipt& value);
void writeImageOutcomeFile(const std::string& path, const image_outcome& value);
void writeShardResultFile(const std::string& path, const shard_result& value);
void writeCancellationFile(const std::string& path, const cancellation& value);
void writeCollectionIndexFile(const std::string& path, const collection_index& value);

recipe readRecipeFile(const std::string& path);
plan readPlanFile(const std::string& path);
request readRequestFile(const std::string& path);
worker_binding readWorkerBindingFile(const std::string& path);
receipt readReceiptFile(const std::string& path);
image_outcome readImageOutcomeFile(const std::string& path);
shard_result readShardResultFile(const std::string& path);
cancellation readCancellationFile(const std::string& path);
collection_index readCollectionIndexFile(const std::string& path);
inline std::string writeOutcome(const image_outcome& value) {
    return writeImageOutcome(value);
}

inline void writeRecipe(const std::string& path, const recipe& value) {
    writeRecipeFile(path, value);
}
inline void writePlan(const std::string& path, const plan& value) {
    writePlanFile(path, value);
}
inline void writeRequest(const std::string& path, const request& value) {
    writeRequestFile(path, value);
}
inline void writeWorkerBinding(const std::string& path, const worker_binding& value) {
    writeWorkerBindingFile(path, value);
}
inline void writeReceipt(const std::string& path, const receipt& value) {
    writeReceiptFile(path, value);
}
inline void writeImageOutcome(const std::string& path, const image_outcome& value) {
    writeImageOutcomeFile(path, value);
}
inline void writeShardResult(const std::string& path, const shard_result& value) {
    writeShardResultFile(path, value);
}
inline void writeCancellation(const std::string& path, const cancellation& value) {
    writeCancellationFile(path, value);
}
inline void writeCollectionIndex(const std::string& path, const collection_index& value) {
    writeCollectionIndexFile(path, value);
}

inline bool successful(outcome_status status) {
    return status == outcome_status::success || status == outcome_status::zero_features;
}

}  // namespace sfm::feature_work
