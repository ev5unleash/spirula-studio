#include "sfm/core/FeatureWork.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <tuple>
#include <stdexcept>
#include <utility>

#include "core/Sha256.h"
#include "core/FilesystemPath.h"
#include "data/Json.h"
#include "data/JsonWrite.h"
#include "sfm/core/Features.h"

namespace sfm::feature_work {
namespace {

using spirula::Sha256;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("feature-work: " + message);
}

void text(const std::string& value, const char* field) {
    if (value.find('\0') != std::string::npos) fail(std::string(field) + " contains NUL");
    const unsigned char* p = (const unsigned char*)value.data();
    const size_t n = value.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = p[i++];
        if (c < 0x80) continue;
        size_t tail = 0;
        uint32_t code = 0;
        if (c >= 0xc2 && c <= 0xdf) {
            tail = 1;
            code = c & 0x1f;
        } else if (c >= 0xe0 && c <= 0xef) {
            tail = 2;
            code = c & 0x0f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            tail = 3;
            code = c & 0x07;
        } else {
            fail(std::string(field) + " is not UTF-8");
        }
        if (i + tail > n) fail(std::string(field) + " is not UTF-8");
        for (size_t j = 0; j < tail; ++j) {
            const unsigned char d = p[i++];
            if ((d & 0xc0) != 0x80) fail(std::string(field) + " is not UTF-8");
            code = (code << 6) | (d & 0x3f);
        }
        if ((tail == 1 && code < 0x80) || (tail == 2 && code < 0x800) ||
            (tail == 3 && code < 0x10000) || code > 0x10ffff ||
            (code >= 0xd800 && code <= 0xdfff))
            fail(std::string(field) + " is not UTF-8");
    }
}

bool hexDigest(const std::string& value) {
    if (value.size() != 64) return false;
    for (char c : value)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

void digestField(const std::string& value, const char* field, bool optional = false) {
    if (optional && value.empty()) return;
    if (!hexDigest(value)) fail(std::string(field) + " is not lowercase SHA-256");
}

std::string folded(std::string value) {
    for (char& c : value)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return value;
}

void pathField(const std::string& value, const char* field, bool optional = false) {
    if (optional && value.empty()) return;
    if (normalizeRelativePath(value) != value) fail(std::string(field) + " is not normalized");
}

void nonempty(const std::string& value, const char* field) {
    text(value, field);
    if (value.empty()) fail(std::string(field) + " is empty");
}

uint32_t number32(const JsonValue& v, const char* field) {
    if (v.type != JsonValue::Type::Number || !std::isfinite(v.num) || v.num < 0 ||
        std::floor(v.num) != v.num || v.num > std::numeric_limits<uint32_t>::max())
        fail(std::string(field) + " is not a uint32");
    return (uint32_t)v.num;
}

uint64_t number64(const JsonValue& v, const char* field) {
    if (v.type != JsonValue::Type::Number || !std::isfinite(v.num) || v.num < 0 ||
        std::floor(v.num) != v.num ||
        v.num > (double)std::numeric_limits<int64_t>::max())
        fail(std::string(field) + " is not a uint64");
    return (uint64_t)v.num;
}

int32_t signed32(const JsonValue& v, const char* field) {
    if (v.type != JsonValue::Type::Number || !std::isfinite(v.num) ||
        std::floor(v.num) != v.num || v.num < std::numeric_limits<int32_t>::min() ||
        v.num > std::numeric_limits<int32_t>::max())
        fail(std::string(field) + " is not an int32");
    return (int32_t)v.num;
}

bool boolean(const JsonValue& v, const char* field) {
    if (v.type != JsonValue::Type::Bool) fail(std::string(field) + " is not a boolean");
    return v.b;
}

std::string stringValue(const JsonValue& v, const char* field) {
    if (v.type != JsonValue::Type::String) fail(std::string(field) + " is not a string");
    text(v.str, field);
    return v.str;
}

const JsonValue& objectValue(const JsonValue& v, const char* field) {
    if (!v.is_object()) fail(std::string(field) + " is not an object");
    return v;
}

const JsonValue& required(const JsonValue& object, const char* key) {
    const JsonValue* value = object.find(key);
    if (!value) fail(std::string("missing ") + key);
    return *value;
}

void exactKeys(const JsonValue& object, std::initializer_list<const char*> keys,
               const char* kind) {
    std::set<std::string> allowed;
    for (const char* key : keys) allowed.insert(key);
    std::set<std::string> seen;
    for (const auto& [key, ignored] : object.obj) {
        if (!allowed.count(key)) fail(std::string(kind) + " has unknown field " + key);
        if (!seen.insert(key).second) fail(std::string(kind) + " repeats field " + key);
    }
}

JsonValue recordObject(const std::string& json, const char* kind,
                       std::initializer_list<const char*> keys) {
    JsonValue value;
    try {
        value = json_parse(json);
    } catch (const std::exception& e) {
        fail(e.what());
    }
    const JsonValue& object = objectValue(value, kind);
    exactKeys(object, keys, kind);
    if (stringValue(required(object, "kind"), "kind") != kind)
        fail(std::string(kind) + " has wrong kind");
    if (number32(required(object, "schema"), "schema") != kSchema)
        fail(std::string(kind) + " has unsupported schema");
    return value;
}

std::vector<uint32_t> numbers32(const JsonValue& value, const char* field) {
    if (!value.is_array()) fail(std::string(field) + " is not an array");
    std::vector<uint32_t> out;
    out.reserve(value.arr.size());
    for (const JsonValue& item : value.arr) out.push_back(number32(item, field));
    return out;
}

std::vector<std::string> strings(const JsonValue& value, const char* field) {
    if (!value.is_array()) fail(std::string(field) + " is not an array");
    std::vector<std::string> out;
    out.reserve(value.arr.size());
    for (const JsonValue& item : value.arr) out.push_back(stringValue(item, field));
    return out;
}

void emitStrings(JsonWriter& writer, const std::vector<std::string>& values) {
    writer.array();
    for (const std::string& value : values) writer.value(value);
    writer.end();
}

void emitNumbers(JsonWriter& writer, const std::vector<uint32_t>& values) {
    writer.array();
    for (uint32_t value : values) writer.value((long long)value);
    writer.end();
}

std::string statusName(outcome_status status) {
    switch (status) {
        case outcome_status::success: return "success";
        case outcome_status::zero_features: return "zero_features";
        case outcome_status::failed: return "failed";
    }
    fail("unknown outcome status");
}

outcome_status statusValue(const std::string& status) {
    if (status == "success") return outcome_status::success;
    if (status == "zero_features") return outcome_status::zero_features;
    if (status == "failed") return outcome_status::failed;
    fail("unknown outcome status");
}

void emitRecipe(JsonWriter& writer, const recipe& value, bool withDigest) {
    writer.object().field("schema", (long long)kSchema).field("kind", "feature_work.recipe")
        .field("frontend", value.frontend).field("descriptor", value.descriptor)
        .field("descriptor_dim", (long long)value.descriptor_dim)
        .field("descriptor_dtype", (long long)value.descriptor_dtype)
        .field("implementation", value.implementation)
        .field("model_digest", value.model_digest).field("shader_digest", value.shader_digest);
    writer.key("settings").object();
    for (const auto& [key, setting] : value.settings) writer.field(key.c_str(), setting);
    writer.end();
    if (withDigest) writer.field("digest", recipeDigest(value));
    writer.end();
}

void emitImage(JsonWriter& writer, const plan_image& value) {
    writer.object()
        .field("global_index", (long long)value.global_index)
        .field("logical_name", value.logical_name)
        .field("feature_path", value.feature_path)
        .field("source_digest", value.source_digest)
        .field("mask_path", value.mask_path)
        .field("mask_digest", value.mask_digest)
        .field("artifact_key", value.artifact_key)
        .field("owner_shard", (long long)value.owner_shard);
    writer.key("chunks");
    emitStrings(writer, value.chunks);
    writer.end();
}

void emitChunk(JsonWriter& writer, const chunk& value) {
    writer.object().field("id", value.id).key("image_indices");
    emitNumbers(writer, value.image_indices);
    writer.end();
}

void emitPlan(JsonWriter& writer, const plan& value, bool withDigest) {
    plan normalized = value;
    normalized.extraction.digest = recipeDigest(value.extraction);
    writer.object().field("schema", (long long)kSchema).field("kind", "feature_work.plan")
        .field("dataset_digest", value.dataset_digest).key("recipe");
    emitRecipe(writer, normalized.extraction, true);
    writer.key("images").array();
    for (const plan_image& image : value.images) emitImage(writer, image);
    writer.end();
    writer.key("chunks").array();
    for (const chunk& item : value.chunks) emitChunk(writer, item);
    writer.end();
    writer.field("capture_digest", value.capture_digest)
        .field("configuration_digest", value.configuration_digest);
    if (withDigest) writer.field("digest", planDigest(value));
    writer.end();
}

void emitRequest(JsonWriter& writer, const request& value, bool withDigest) {
    writer.object().field("schema", (long long)kSchema).field("kind", "feature_work.request")
        .field("plan_digest", value.plan_digest).field("plan_path", value.plan_path)
        .field("shard", (long long)value.shard).field("attempt_id", value.attempt_id)
        .key("image_indices");
    emitNumbers(writer, value.image_indices);
    writer.field("supersedes", value.supersedes);
    if (withDigest) writer.field("digest", requestDigest(value));
    writer.end();
}

void emitBinding(JsonWriter& writer, const worker_binding& value, bool withDigest) {
    writer.object().field("schema", (long long)kSchema)
        .field("kind", "feature_work.worker_binding").key("request");
    emitRequest(writer, value.extraction_request, true);
    writer.field("request_path", value.request_path).field("image_root", value.image_root)
        .field("mask_root", value.mask_root).field("model_root", value.model_root)
        .field("device", value.device).field("output_dir", value.output_dir)
        .field("max_payload_bytes", (long long)value.max_payload_bytes)
        .field("max_images", (long long)value.max_images);
    if (withDigest) writer.field("digest", workerBindingDigest(value));
    writer.end();
}

void emitReceipt(JsonWriter& writer, const receipt& value, bool withDigest) {
    writer.object().field("schema", (long long)kSchema).field("kind", "feature_work.receipt")
        .field("request_digest", value.request_digest)
        .field("global_index", (long long)value.global_index)
        .field("logical_name", value.logical_name).field("feature_path", value.feature_path)
        .field("artifact_key", value.artifact_key).field("payload_digest", value.payload_digest)
        .field("payload_bytes", (long long)value.payload_bytes)
        .field("feature_count", (long long)value.feature_count)
        .field("descriptor_dtype", (long long)value.descriptor_dtype)
        .field("descriptor_dim", (long long)value.descriptor_dim)
        .field("width", (long long)value.width).field("height", (long long)value.height)
        .field("extract_width", (long long)value.extract_width)
        .field("extract_height", (long long)value.extract_height)
        .field("producer_build", value.producer_build)
        .field("producer_device", value.producer_device);
    if (withDigest) writer.field("digest", receiptDigest(value));
    writer.end();
}

void emitOutcome(JsonWriter& writer, const image_outcome& value, bool withDigest) {
    writer.object().field("schema", (long long)kSchema)
        .field("kind", "feature_work.image_outcome")
        .field("global_index", (long long)value.global_index)
        .field("status", statusName(value.status)).field("receipt_digest", value.receipt_digest)
        .field("error", value.error);
    if (withDigest) writer.field("digest", outcomeDigest(value));
    writer.end();
}

void emitShardResult(JsonWriter& writer, const shard_result& value, bool withDigest) {
    writer.object().field("schema", (long long)kSchema)
        .field("kind", "feature_work.shard_result")
        .field("plan_digest", value.plan_digest)
        .field("request_digest", value.request_digest)
        .field("shard", (long long)value.shard).key("outcomes").array();
    for (const image_outcome& outcome : value.outcomes) emitOutcome(writer, outcome, true);
    writer.end().field("complete", value.complete);
    if (withDigest) writer.field("digest", shardResultDigest(value));
    writer.end();
}

void emitCancellation(JsonWriter& writer, const cancellation& value, bool withDigest) {
    writer.object().field("schema", (long long)kSchema)
        .field("kind", "feature_work.cancellation")
        .field("plan_digest", value.plan_digest)
        .field("request_digest", value.request_digest)
        .field("shard", (long long)value.shard).field("reason", value.reason);
    if (withDigest) writer.field("cancellation_digest", cancellationDigest(value));
    writer.end();
}

void emitRow(JsonWriter& writer, const accepted_row& value) {
    writer.object().field("global_index", (long long)value.global_index)
        .field("logical_name", value.logical_name).field("feature_path", value.feature_path)
        .field("artifact_key", value.artifact_key).field("payload_digest", value.payload_digest)
        .field("payload_bytes", (long long)value.payload_bytes)
        .field("feature_count", (long long)value.feature_count)
        .field("receipt_digest", value.receipt_digest).field("result_digest", value.result_digest)
        .end();
}

std::string collectionRecordDigest(const collection_index& value);

void emitCollection(JsonWriter& writer, const collection_index& value, bool withDigest) {
    writer.object().field("schema", (long long)kSchema)
        .field("kind", "feature_work.collection_index")
        .field("plan_digest", value.plan_digest).key("rows").array();
    for (const accepted_row& row : value.rows) emitRow(writer, row);
    writer.end().field("collection_digest", collectionDigest(value)).field("complete", value.complete);
    if (withDigest) writer.field("digest", collectionRecordDigest(value));
    writer.end();
}

std::string encoded(JsonWriter& writer) { return writer.str(); }

std::string digestJson(JsonWriter& writer) { return sha256Text(encoded(writer)); }

void validateRecipeFields(const recipe& value) {
    if (value.schema != kSchema) fail("recipe schema is unsupported");
    nonempty(value.frontend, "recipe.frontend");
    nonempty(value.descriptor, "recipe.descriptor");
    nonempty(value.implementation, "recipe.implementation");
    if (value.descriptor_dim == 0 || value.descriptor_dtype > 1)
        fail("recipe descriptor is invalid");
    digestField(value.model_digest, "recipe.model_digest", true);
    digestField(value.shader_digest, "recipe.shader_digest", true);
    for (const auto& [key, setting] : value.settings) {
        nonempty(key, "recipe setting key");
        text(setting, "recipe setting");
    }
}

void validateImageFields(const plan_image& image) {
    text(image.logical_name, "logical_name");
    text(image.feature_path, "feature_path");
    text(image.source_digest, "source_digest");
    text(image.mask_path, "mask_path");
    text(image.mask_digest, "mask_digest");
    text(image.artifact_key, "artifact_key");
    pathField(image.logical_name, "logical_name");
    pathField(image.feature_path, "feature_path");
    pathField(image.mask_path, "mask_path", true);
    digestField(image.source_digest, "source_digest");
    if (image.mask_path.empty() != image.mask_digest.empty())
        fail("mask path and digest must agree");
    digestField(image.mask_digest, "mask_digest", true);
}

void validatePlanFields(const plan& value) {
    if (value.schema != kSchema) fail("plan schema is unsupported");
    digestField(value.dataset_digest, "dataset_digest");
    digestField(value.capture_digest, "capture_digest", true);
    digestField(value.configuration_digest, "configuration_digest", true);
    validateRecipeFields(value.extraction);
    std::set<std::string> names, foldedNames, features, foldedFeatures, masks, foldedMasks;
    std::string previousFeaturePath;
    for (size_t i = 0; i < value.images.size(); ++i) {
        const plan_image& image = value.images[i];
        if (image.global_index != i) fail("global image indices are not dense");
        validateImageFields(image);
        if (!previousFeaturePath.empty() && image.feature_path <= previousFeaturePath)
            fail("global image order is not feature-path order");
        previousFeaturePath = image.feature_path;
        if (featurePathForImage(image.logical_name) != image.feature_path)
            fail("feature path does not match logical image name");
        if (!names.insert(image.logical_name).second ||
            !foldedNames.insert(folded(image.logical_name)).second)
            fail("logical image collision");
        if (!features.insert(image.feature_path).second ||
            !foldedFeatures.insert(folded(image.feature_path)).second)
            fail("feature path collision");
        if (!image.mask_path.empty() &&
            (!masks.insert(image.mask_path).second || !foldedMasks.insert(folded(image.mask_path)).second))
            fail("mask path collision");
        if (image.artifact_key != imageArtifactKey(image.source_digest,
                                                   image.mask_digest, value.extraction))
            fail("image artifact key is stale");
        std::set<std::string> imageChunks;
        if (!std::is_sorted(image.chunks.begin(), image.chunks.end()))
            fail("image chunk memberships are not ordered");
        for (const std::string& id : image.chunks) {
            nonempty(id, "chunk id");
            if (!imageChunks.insert(id).second) fail("duplicate image chunk membership");
        }
    }
    std::set<std::string> chunkIds;
    std::vector<std::set<std::string>> memberships(value.images.size());
    for (const chunk& item : value.chunks) {
        nonempty(item.id, "chunk.id");
        if (!chunkIds.insert(item.id).second) fail("duplicate chunk id");
        if (!std::is_sorted(item.image_indices.begin(), item.image_indices.end()))
            fail("chunk image indices are not ordered");
        std::set<uint32_t> seen;
        for (uint32_t index : item.image_indices) {
            if (index >= value.images.size() || !seen.insert(index).second)
                fail("invalid chunk image coverage");
            memberships[index].insert(item.id);
        }
    }
    for (size_t i = 0; i < value.images.size(); ++i) {
        std::set<std::string> listed(value.images[i].chunks.begin(), value.images[i].chunks.end());
        if (listed != memberships[i]) fail("chunk membership mismatch");
        for (const std::string& id : listed)
            if (!chunkIds.count(id)) fail("image references unknown chunk");
    }
    if (value.dataset_digest != datasetSnapshotDigest(value.images))
        fail("dataset snapshot digest is stale");
}

void validateRequestFields(const request& value) {
    if (value.schema != kSchema) fail("request schema is unsupported");
    digestField(value.plan_digest, "request.plan_digest");
    text(value.plan_path, "request.plan_path");
    pathField(value.plan_path, "request.plan_path");
    nonempty(value.attempt_id, "request.attempt_id");
    if (!std::is_sorted(value.image_indices.begin(), value.image_indices.end()))
        fail("request image indices are not ordered");
    digestField(value.supersedes, "request.supersedes", true);
    std::set<uint32_t> seen;
    for (uint32_t index : value.image_indices)
        if (!seen.insert(index).second) fail("request repeats an image");
}

void validateBindingFields(const worker_binding& value) {
    if (value.schema != kSchema) fail("worker binding schema is unsupported");
    validateRequestFields(value.extraction_request);
    if (!value.extraction_request.digest.empty() &&
        value.extraction_request.digest != requestDigest(value.extraction_request))
        fail("worker binding request digest is stale");
    text(value.request_path, "worker binding request_path");
    pathField(value.request_path, "worker binding request_path");
    text(value.image_root, "worker binding image_root");
    text(value.mask_root, "worker binding mask_root");
    text(value.model_root, "worker binding model_root");
    text(value.device, "worker binding device");
    text(value.output_dir, "worker binding output_dir");
    if (value.max_payload_bytes == 0 || value.max_images == 0)
        fail("worker binding limits are invalid");
}

void validateReceiptFields(const receipt& value) {
    if (value.schema != kSchema) fail("receipt schema is unsupported");
    digestField(value.request_digest, "receipt.request_digest");
    pathField(value.logical_name, "receipt.logical_name");
    pathField(value.feature_path, "receipt.feature_path");
    digestField(value.artifact_key, "receipt.artifact_key");
    digestField(value.payload_digest, "receipt.payload_digest");
    if (value.payload_bytes == 0 || value.descriptor_dim == 0 || value.descriptor_dtype > 1 ||
        value.width <= 0 || value.height <= 0)
        fail("receipt payload metadata is invalid");
    if ((value.extract_width == 0) != (value.extract_height == 0) ||
        value.extract_width < 0 || value.extract_height < 0 ||
        (value.extract_width &&
         (value.extract_width > value.width || value.extract_height > value.height)))
        fail("receipt extraction dimensions are invalid");
    text(value.producer_build, "receipt.producer_build");
    text(value.producer_device, "receipt.producer_device");
}

void validateOutcomeFields(const image_outcome& value) {
    if (value.schema != kSchema) fail("outcome schema is unsupported");
    text(value.error, "outcome.error");
    digestField(value.receipt_digest, "outcome.receipt_digest", true);
    if (successful(value.status)) {
        if (value.receipt_digest.empty() || !value.error.empty())
            fail("successful outcome lacks a receipt or has an error");
    } else if (value.error.empty() || !value.receipt_digest.empty()) {
        fail("failed outcome lacks an error or has a receipt");
    }
}

void validateCancellationFields(const cancellation& value) {
    if (value.schema != kSchema) fail("cancellation schema is unsupported");
    digestField(value.plan_digest, "cancellation.plan_digest");
    digestField(value.request_digest, "cancellation.request_digest");
    nonempty(value.reason, "cancellation.reason");
    text(value.reason, "cancellation.reason");
}

void validateRowFields(const accepted_row& value) {
    pathField(value.logical_name, "row.logical_name");
    pathField(value.feature_path, "row.feature_path");
    digestField(value.artifact_key, "row.artifact_key");
    digestField(value.payload_digest, "row.payload_digest");
    digestField(value.receipt_digest, "row.receipt_digest");
    digestField(value.result_digest, "row.result_digest");
    if (value.payload_bytes == 0) fail("row payload is empty");
}


recipe recipeFromObject(const JsonValue& object) {
    exactKeys(object, {"schema", "kind", "frontend", "descriptor", "descriptor_dim",
                       "descriptor_dtype", "implementation", "model_digest", "shader_digest",
                       "settings", "digest"},
              "feature_work.recipe");
    recipe value;
    if (stringValue(required(object, "kind"), "kind") != "feature_work.recipe")
        fail("recipe has wrong kind");
    value.schema = number32(required(object, "schema"), "schema");
    value.frontend = stringValue(required(object, "frontend"), "frontend");
    value.descriptor = stringValue(required(object, "descriptor"), "descriptor");
    value.descriptor_dim = number32(required(object, "descriptor_dim"), "descriptor_dim");
    value.descriptor_dtype = number32(required(object, "descriptor_dtype"), "descriptor_dtype");
    value.implementation = stringValue(required(object, "implementation"), "implementation");
    value.model_digest = stringValue(required(object, "model_digest"), "model_digest");
    value.shader_digest = stringValue(required(object, "shader_digest"), "shader_digest");
    const JsonValue& settings = objectValue(required(object, "settings"), "settings");
    for (const auto& [key, item] : settings.obj) {
        text(key, "recipe setting key");
        if (!value.settings.emplace(key, stringValue(item, "recipe setting")).second)
            fail("recipe repeats a setting");
    }
    value.digest = stringValue(required(object, "digest"), "digest");
    validateRecipe(value);
    return value;
}

JsonValue parseJson(const std::string& json) {
    try {
        return json_parse(json);
    } catch (const std::exception& e) {
        fail(e.what());
    }
}

std::string readFile(const std::string& path) {
    std::ifstream f(spirula::NativeFilesystemPath(std::filesystem::path(path)),
                    std::ios::binary);
    if (!f) fail("cannot read " + path);
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size < 0 || (uint64_t)size > (64ull << 20)) fail("record file is too large");
    f.seekg(0, std::ios::beg);
    std::string data((size_t)size, '\0');
    if (size) f.read(data.data(), size);
    if (!f && !data.empty()) fail("cannot read " + path);
    return data;
}

}  // namespace

std::string normalizeRelativePath(const std::string& path) {
    text(path, "path");
    if (path.empty()) fail("path is empty");
    std::string slashes = path;
    std::replace(slashes.begin(), slashes.end(), '\\', '/');
    if (slashes.empty() || slashes[0] == '/' || slashes.rfind("//", 0) == 0 ||
        (slashes.size() >= 2 && slashes[1] == ':' &&
         ((slashes[0] >= 'A' && slashes[0] <= 'Z') ||
          (slashes[0] >= 'a' && slashes[0] <= 'z'))))
        fail("path must be relative");
    std::vector<std::string> components;
    size_t begin = 0;
    while (begin <= slashes.size()) {
        const size_t end = slashes.find('/', begin);
        const size_t stop = end == std::string::npos ? slashes.size() : end;
        const std::string component = slashes.substr(begin, stop - begin);
        if (component == "..") fail("path contains dot-dot");
        if (!component.empty() && component != ".") components.push_back(component);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    if (components.empty()) fail("path is empty");
    std::string out;
    for (const std::string& component : components) {
        if (!out.empty()) out += '/';
        out += component;
    }
    return out;
}

std::string featurePathForImage(const std::string& logical_name) {
    const std::string normalized = normalizeRelativePath(logical_name);
    std::filesystem::path path(normalized);
    path.replace_extension(".bin");
    const std::string result = path.generic_string();
    if (result.empty() || result == ".bin") fail("image name has no representable stem");
    return normalizeRelativePath(result);
}

std::string sha256Text(const std::string& bytes) {
    Sha256 sha;
    sha.update((const uint8_t*)bytes.data(), bytes.size());
    return sha.hex();
}

std::string sha256File(const std::string& path) {
    const std::string digest = spirula::sha256_file(path);
    if (digest.empty()) fail("cannot hash " + path);
    return digest;
}

std::string recipeDigest(const recipe& value) {
    validateRecipeFields(value);
    JsonWriter writer;
    emitRecipe(writer, value, false);
    return digestJson(writer);
}

std::string imageArtifactKey(const std::string& source_digest, const std::string& mask_digest,
                             const recipe& extraction) {
    digestField(source_digest, "source_digest");
    digestField(mask_digest, "mask_digest", true);
    const std::string extractionDigest = recipeDigest(extraction);
    JsonWriter writer;
    writer.object().field("source_digest", source_digest)
        .field("mask_digest", mask_digest).field("recipe_digest", extractionDigest).end();
    return digestJson(writer);
}

std::string datasetSnapshotDigest(const std::vector<plan_image>& images) {
    JsonWriter writer;
    writer.array();
    for (const plan_image& image : images) {
        validateImageFields(image);
        writer.object().field("logical_name", normalizeRelativePath(image.logical_name))
            .field("source_digest", image.source_digest).field("mask_path", image.mask_path)
            .field("mask_digest", image.mask_digest).end();
    }
    writer.end();
    return digestJson(writer);
}

std::string planDigest(const plan& value) {
    validatePlanFields(value);
    JsonWriter writer;
    emitPlan(writer, value, false);
    return digestJson(writer);
}

std::string requestDigest(const request& value) {
    validateRequestFields(value);
    JsonWriter writer;
    emitRequest(writer, value, false);
    return digestJson(writer);
}

std::string workerBindingDigest(const worker_binding& value) {
    validateBindingFields(value);
    JsonWriter writer;
    emitBinding(writer, value, false);
    return digestJson(writer);
}

std::string receiptDigest(const receipt& value) {
    validateReceiptFields(value);
    JsonWriter writer;
    emitReceipt(writer, value, false);
    return digestJson(writer);
}

std::string outcomeDigest(const image_outcome& value) {
    validateOutcomeFields(value);
    JsonWriter writer;
    emitOutcome(writer, value, false);
    return digestJson(writer);
}

std::string shardResultDigest(const shard_result& value) {
    if (value.schema != kSchema) fail("shard result schema is unsupported");
    digestField(value.plan_digest, "shard result.plan_digest");
    digestField(value.request_digest, "shard result.request_digest");
    JsonWriter writer;
    emitShardResult(writer, value, false);
    return digestJson(writer);
}

std::string cancellationDigest(const cancellation& value) {
    validateCancellationFields(value);
    JsonWriter writer;
    emitCancellation(writer, value, false);
    return digestJson(writer);
}

std::string collectionDigest(const collection_index& value) {
    if (value.schema != kSchema) fail("collection schema is unsupported");
    digestField(value.plan_digest, "collection.plan_digest");
    std::vector<const accepted_row*> rows;
    rows.reserve(value.rows.size());
    for (const accepted_row& row : value.rows) {
        validateRowFields(row);
        rows.push_back(&row);
    }
    std::sort(rows.begin(), rows.end(), [](const accepted_row* a, const accepted_row* b) {
        if (a->artifact_key != b->artifact_key) return a->artifact_key < b->artifact_key;
        if (a->payload_digest != b->payload_digest)
            return a->payload_digest < b->payload_digest;
        if (a->payload_bytes != b->payload_bytes)
            return a->payload_bytes < b->payload_bytes;
        return a->feature_count < b->feature_count;
    });
    JsonWriter writer;
    writer.array();
    for (const accepted_row* row : rows)
        writer.object()
            .field("artifact_key", row->artifact_key)
            .field("payload_digest", row->payload_digest)
            .field("payload_bytes", (long long)row->payload_bytes)
            .field("feature_count", (long long)row->feature_count)
            .end();
    writer.end();
    return digestJson(writer);
}
namespace {
std::string collectionRecordDigest(const collection_index& value) {
    JsonWriter writer;
    emitCollection(writer, value, false);
    return digestJson(writer);
}
}  // namespace

void validateRecipe(const recipe& value) {
    validateRecipeFields(value);
    if (!value.digest.empty() && value.digest != recipeDigest(value)) fail("recipe digest is stale");
}

void validatePlan(const plan& value) {
    validatePlanFields(value);
    if (!value.extraction.digest.empty() && value.extraction.digest != recipeDigest(value.extraction))
        fail("plan recipe digest is stale");
    if (!value.digest.empty() && value.digest != planDigest(value)) fail("plan digest is stale");
}

void validateRequest(const plan& value, const request& request_value) {
    validatePlan(value);
    validateRequestFields(request_value);
    const std::string pd = planDigest(value);
    if (request_value.plan_digest != pd) fail("request names a different plan");
    std::vector<uint32_t> expected;
    for (const plan_image& image : value.images)
        if (image.owner_shard == request_value.shard) expected.push_back(image.global_index);
    if (expected != request_value.image_indices) fail("request image coverage is stale");
    if (!request_value.digest.empty() && request_value.digest != requestDigest(request_value))
        fail("request digest is stale");
}

void validateWorkerBinding(const plan& value, const worker_binding& value_binding) {
    validateBindingFields(value_binding);
    validateRequest(value, value_binding.extraction_request);
    if (!value_binding.digest.empty() && value_binding.digest != workerBindingDigest(value_binding))
        fail("worker binding digest is stale");
}

void validateReceipt(const plan& value, const request& request_value,
                     const receipt& value_receipt, const std::string& payload_root) {
    validateRequest(value, request_value);
    validateReceiptFields(value_receipt);
    if (value_receipt.request_digest != requestDigest(request_value))
        fail("receipt names a different request");
    const auto it = std::find_if(value.images.begin(), value.images.end(), [&](const plan_image& image) {
        return image.global_index == value_receipt.global_index;
    });
    if (it == value.images.end()) fail("receipt image is not in the plan");
    if (std::find(request_value.image_indices.begin(), request_value.image_indices.end(),
                  value_receipt.global_index) == request_value.image_indices.end())
        fail("receipt image is not assigned to the request");
    if (value_receipt.logical_name != it->logical_name || value_receipt.feature_path != it->feature_path ||
        value_receipt.artifact_key != it->artifact_key)
        fail("receipt image identity is stale");
    if (value_receipt.descriptor_dim != value.extraction.descriptor_dim ||
        value_receipt.descriptor_dtype != value.extraction.descriptor_dtype)
        fail("receipt descriptor does not match recipe");
    if (!value_receipt.digest.empty() && value_receipt.digest != receiptDigest(value_receipt))
        fail("receipt digest is stale");
    if (!payload_root.empty()) {
        const std::filesystem::path root(payload_root);
        const std::filesystem::path payload = root / value_receipt.feature_path;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(spirula::NativeFilesystemPath(payload), ec) || ec)
            fail("receipt payload is missing");
        const std::filesystem::path rootCanonical =
            std::filesystem::weakly_canonical(spirula::NativeFilesystemPath(root), ec);
        if (ec) fail("receipt payload root cannot be resolved");
        const std::filesystem::path payloadCanonical =
            std::filesystem::weakly_canonical(spirula::NativeFilesystemPath(payload), ec);
        if (ec) fail("receipt payload cannot be resolved");
        const std::filesystem::path relative = payloadCanonical.lexically_relative(rootCanonical);
        if (relative.empty() || relative == ".." ||
            relative.generic_string().rfind("../", 0) == 0)
            fail("receipt payload escapes its root");
        if ((uint64_t)std::filesystem::file_size(spirula::NativeFilesystemPath(payload), ec) !=
                value_receipt.payload_bytes || ec)
            fail("receipt payload size differs");
        if (sha256File(payload.string()) != value_receipt.payload_digest)
            fail("receipt payload digest differs");
        const FeatureSet fs = readFeatures(payload.string());
        if (fs.count() != value_receipt.feature_count || fs.dim != value_receipt.descriptor_dim ||
            (uint32_t)fs.dtype != value_receipt.descriptor_dtype || fs.width != value_receipt.width ||
            fs.height != value_receipt.height || fs.extract_width != value_receipt.extract_width ||
            fs.extract_height != value_receipt.extract_height)
            fail("receipt payload metadata differs");
    }
}

void validateCancellation(const plan& value, const request& request_value,
                          const cancellation& value_cancellation) {
    validateRequest(value, request_value);
    validateCancellationFields(value_cancellation);
    if (value_cancellation.plan_digest != planDigest(value) ||
        value_cancellation.request_digest != requestDigest(request_value) ||
        value_cancellation.shard != request_value.shard)
        fail("cancellation identity differs");
    if (value_cancellation.cancellation_digest != cancellationDigest(value_cancellation))
        fail("cancellation digest is stale");
}

void validateShardResult(const plan& value, const request& request_value,
                         const shard_result& value_result, const cancellation* cancelled) {
    validateRequest(value, request_value);
    if (cancelled) validateCancellation(value, request_value, *cancelled);
    if (value_result.schema != kSchema) fail("shard result schema is unsupported");
    digestField(value_result.plan_digest, "shard result.plan_digest");
    digestField(value_result.request_digest, "shard result.request_digest");
    if (value_result.plan_digest != planDigest(value) ||
        value_result.request_digest != requestDigest(request_value) ||
        value_result.shard != request_value.shard)
        fail("shard result identity differs");
    std::set<uint32_t> seen;
    size_t position = 0;
    for (const image_outcome& outcome : value_result.outcomes) {
        validateOutcomeFields(outcome);
        if (position >= request_value.image_indices.size() ||
            outcome.global_index != request_value.image_indices[position] ||
            !seen.insert(outcome.global_index).second)
            fail("shard result coverage is invalid");
        ++position;
        if (!outcome.digest.empty() && outcome.digest != outcomeDigest(outcome))
            fail("outcome digest is stale");
    }
    if (seen.size() != request_value.image_indices.size()) fail("shard result is incomplete");
    bool complete = true;
    for (const image_outcome& outcome : value_result.outcomes)
        complete = complete && successful(outcome.status);
    if (value_result.complete != complete) fail("shard result completion is stale");
    if (!value_result.digest.empty() && value_result.digest != shardResultDigest(value_result))
        fail("shard result digest is stale");
    if (cancelled) fail("shard result is cancelled");
}

void validateCollection(const plan& value, const collection_index& value_index) {
    validatePlan(value);
    if (value_index.schema != kSchema) fail("collection schema is unsupported");
    digestField(value_index.plan_digest, "collection.plan_digest");
    if (value_index.plan_digest != planDigest(value)) fail("collection names a different plan");
    std::set<uint32_t> seen;
    std::set<std::string> names, features, foldedNames, foldedFeatures;
    std::map<std::string, std::tuple<std::string, uint64_t, uint32_t>> artifacts;
    uint32_t previous = 0;
    bool first = true;
    for (const accepted_row& row : value_index.rows) {
        validateRowFields(row);
        if (row.global_index >= value.images.size() || (!first && row.global_index <= previous) ||
            !seen.insert(row.global_index).second)
            fail("collection rows are not ordered");
        first = false;
        previous = row.global_index;
        const plan_image& image = value.images[row.global_index];
        if (row.logical_name != image.logical_name || row.feature_path != image.feature_path ||
            row.artifact_key != image.artifact_key)
            fail("collection row identity differs");
        const auto artifact = std::make_tuple(row.payload_digest, row.payload_bytes,
                                              row.feature_count);
        const auto previousArtifact = artifacts.emplace(row.artifact_key, artifact);
        if (!previousArtifact.second && previousArtifact.first->second != artifact)
            fail("collection duplicate artifact has incompatible payload");
        if (!names.insert(row.logical_name).second ||
            !foldedNames.insert(folded(row.logical_name)).second ||
            !features.insert(row.feature_path).second ||
            !foldedFeatures.insert(folded(row.feature_path)).second)
            fail("collection path collision");
    }
    if (value_index.complete &&
        (value_index.rows.size() != value.images.size() || (!value_index.rows.empty() &&
         value_index.rows.back().global_index + 1 != value.images.size())))
        fail("complete collection lacks images");
    if (value_index.collection_digest != collectionDigest(value_index))
        fail("collection digest is stale");
    if (!value_index.digest.empty() &&
        value_index.digest != collectionRecordDigest(value_index))
        fail("collection record digest is stale");
}

std::string writeRecipe(const recipe& value) {
    validateRecipe(value);
    JsonWriter writer;
    emitRecipe(writer, value, true);
    return encoded(writer);
}

std::string writePlan(const plan& value) {
    validatePlan(value);
    JsonWriter writer;
    emitPlan(writer, value, true);
    return encoded(writer);
}

std::string writeRequest(const request& value) {
    validateRequestFields(value);
    if (!value.digest.empty() && value.digest != requestDigest(value))
        fail("request digest is stale");
    JsonWriter writer;
    emitRequest(writer, value, true);
    return encoded(writer);
}

std::string writeWorkerBinding(const worker_binding& value) {
    validateBindingFields(value);
    if (!value.digest.empty() && value.digest != workerBindingDigest(value))
        fail("worker binding digest is stale");
    JsonWriter writer;
    emitBinding(writer, value, true);
    return encoded(writer);
}

std::string writeReceipt(const receipt& value) {
    validateReceiptFields(value);
    if (!value.digest.empty() && value.digest != receiptDigest(value))
        fail("receipt digest is stale");
    JsonWriter writer;
    emitReceipt(writer, value, true);
    return encoded(writer);
}

std::string writeImageOutcome(const image_outcome& value) {
    validateOutcomeFields(value);
    if (!value.digest.empty() && value.digest != outcomeDigest(value))
        fail("outcome digest is stale");
    JsonWriter writer;
    emitOutcome(writer, value, true);
    return encoded(writer);
}

std::string writeShardResult(const shard_result& value) {
    if (value.schema != kSchema) fail("shard result schema is unsupported");
    bool complete = true;
    uint32_t previous = 0;
    bool first = true;
    for (const image_outcome& outcome : value.outcomes) {
        validateOutcomeFields(outcome);
        if (!first && outcome.global_index <= previous)
            fail("shard result outcomes are not ordered");
        first = false;
        previous = outcome.global_index;
        complete = complete && successful(outcome.status);
        if (!outcome.digest.empty() && outcome.digest != outcomeDigest(outcome))
            fail("outcome digest is stale");
    }
    if (value.complete != complete) fail("shard result completion is stale");
    if (!value.digest.empty() && value.digest != shardResultDigest(value))
        fail("shard result digest is stale");
    JsonWriter writer;
    emitShardResult(writer, value, true);
    return encoded(writer);
}

std::string writeCancellation(const cancellation& value) {
    validateCancellationFields(value);
    if (!value.cancellation_digest.empty() &&
        value.cancellation_digest != cancellationDigest(value))
        fail("cancellation digest is stale");
    JsonWriter writer;
    emitCancellation(writer, value, true);
    return encoded(writer);
}

std::string writeCollectionIndex(const collection_index& value) {
    if (value.schema != kSchema) fail("collection schema is unsupported");
    digestField(value.plan_digest, "collection.plan_digest");
    uint32_t previous = 0;
    bool first = true;
    for (const accepted_row& row : value.rows) {
        validateRowFields(row);
        if (!first && row.global_index <= previous) fail("collection rows are not ordered");
        first = false;
        previous = row.global_index;
    }
    if (!value.collection_digest.empty() && value.collection_digest != collectionDigest(value))
        fail("collection digest is stale");
    if (!value.digest.empty() && value.digest != collectionRecordDigest(value))
        fail("collection record digest is stale");
    JsonWriter writer;
    emitCollection(writer, value, true);
    return encoded(writer);
}

recipe readRecipe(const std::string& json) {
    const JsonValue root = parseJson(json);
    return recipeFromObject(objectValue(root, "feature_work.recipe"));
}

plan readPlan(const std::string& json) {
    const JsonValue root = parseJson(json);
    const JsonValue& object = objectValue(root, "feature_work.plan");

    exactKeys(object, {"schema", "kind", "dataset_digest", "recipe", "images", "chunks",
                       "capture_digest", "configuration_digest", "digest"},
              "feature_work.plan");
    if (stringValue(required(object, "kind"), "kind") != "feature_work.plan")
        fail("plan has wrong kind");
    if (number32(required(object, "schema"), "schema") != kSchema)
        fail("plan has unsupported schema");
    plan value;
    value.schema = number32(required(object, "schema"), "schema");
    value.dataset_digest = stringValue(required(object, "dataset_digest"), "dataset_digest");
    value.extraction = recipeFromObject(objectValue(required(object, "recipe"), "recipe"));
    const JsonValue& images = required(object, "images");
    if (!images.is_array()) fail("plan.images is not an array");
    for (const JsonValue& item : images.arr) {
        const JsonValue& image = objectValue(item, "plan image");
        exactKeys(image, {"global_index", "logical_name", "feature_path", "source_digest",
                          "mask_path", "mask_digest", "artifact_key", "owner_shard", "chunks"},
                  "plan image");
        plan_image out;
        out.global_index = number32(required(image, "global_index"), "global_index");
        out.logical_name = stringValue(required(image, "logical_name"), "logical_name");
        out.feature_path = stringValue(required(image, "feature_path"), "feature_path");
        out.source_digest = stringValue(required(image, "source_digest"), "source_digest");
        out.mask_path = stringValue(required(image, "mask_path"), "mask_path");
        out.mask_digest = stringValue(required(image, "mask_digest"), "mask_digest");
        out.artifact_key = stringValue(required(image, "artifact_key"), "artifact_key");
        out.owner_shard = number32(required(image, "owner_shard"), "owner_shard");
        out.chunks = strings(required(image, "chunks"), "chunks");
        value.images.push_back(std::move(out));
    }
    const JsonValue& chunks = required(object, "chunks");
    if (!chunks.is_array()) fail("plan.chunks is not an array");
    for (const JsonValue& item : chunks.arr) {
        const JsonValue& chunkObject = objectValue(item, "chunk");
        exactKeys(chunkObject, {"id", "image_indices"}, "chunk");
        chunk out;
        out.id = stringValue(required(chunkObject, "id"), "chunk.id");
        out.image_indices = numbers32(required(chunkObject, "image_indices"), "image_indices");
        value.chunks.push_back(std::move(out));
    }
    value.capture_digest = stringValue(required(object, "capture_digest"), "capture_digest");
    value.configuration_digest = stringValue(required(object, "configuration_digest"),
                                              "configuration_digest");
    value.digest = stringValue(required(object, "digest"), "digest");
    validatePlan(value);
    return value;
}

request readRequest(const std::string& json) {
    const JsonValue& object = recordObject(json, "feature_work.request",
                                            {"schema", "kind", "plan_digest", "plan_path", "shard",
                                             "attempt_id", "image_indices", "supersedes", "digest"});
    request value;
    value.schema = number32(required(object, "schema"), "schema");
    value.plan_digest = stringValue(required(object, "plan_digest"), "plan_digest");
    value.plan_path = stringValue(required(object, "plan_path"), "plan_path");
    value.shard = number32(required(object, "shard"), "shard");
    value.attempt_id = stringValue(required(object, "attempt_id"), "attempt_id");
    value.image_indices = numbers32(required(object, "image_indices"), "image_indices");
    value.supersedes = stringValue(required(object, "supersedes"), "supersedes");
    value.digest = stringValue(required(object, "digest"), "digest");
    validateRequestFields(value);
    if (value.digest != requestDigest(value)) fail("request digest is stale");
    return value;
}

worker_binding readWorkerBinding(const std::string& json) {
    const JsonValue& object = recordObject(json, "feature_work.worker_binding",
                                            {"schema", "kind", "request", "request_path", "image_root",
                                             "mask_root", "model_root", "device", "output_dir",
                                             "max_payload_bytes", "max_images", "digest"});
    worker_binding value;
    value.schema = number32(required(object, "schema"), "schema");
    const JsonValue& requestObject = objectValue(required(object, "request"), "request");
    request parsed;
    exactKeys(requestObject, {"schema", "kind", "plan_digest", "plan_path", "shard", "attempt_id",
                              "image_indices", "supersedes", "digest"},
              "feature_work.request");
    if (stringValue(required(requestObject, "kind"), "kind") != "feature_work.request")
        fail("request has wrong kind");
    if (number32(required(requestObject, "schema"), "schema") != kSchema)
        fail("request has unsupported schema");
    parsed.schema = number32(required(requestObject, "schema"), "schema");
    parsed.plan_digest = stringValue(required(requestObject, "plan_digest"), "plan_digest");
    parsed.plan_path = stringValue(required(requestObject, "plan_path"), "plan_path");
    parsed.shard = number32(required(requestObject, "shard"), "shard");
    parsed.attempt_id = stringValue(required(requestObject, "attempt_id"), "attempt_id");
    parsed.image_indices = numbers32(required(requestObject, "image_indices"), "image_indices");
    parsed.supersedes = stringValue(required(requestObject, "supersedes"), "supersedes");
    parsed.digest = stringValue(required(requestObject, "digest"), "digest");
    validateRequestFields(parsed);
    if (parsed.digest != requestDigest(parsed)) fail("request digest is stale");
    value.extraction_request = std::move(parsed);
    value.request_path = stringValue(required(object, "request_path"), "request_path");
    value.image_root = stringValue(required(object, "image_root"), "image_root");
    value.mask_root = stringValue(required(object, "mask_root"), "mask_root");
    value.model_root = stringValue(required(object, "model_root"), "model_root");
    value.device = stringValue(required(object, "device"), "device");
    value.output_dir = stringValue(required(object, "output_dir"), "output_dir");
    value.max_payload_bytes = number64(required(object, "max_payload_bytes"), "max_payload_bytes");
    value.max_images = number32(required(object, "max_images"), "max_images");
    value.digest = stringValue(required(object, "digest"), "digest");
    validateBindingFields(value);
    if (value.digest != workerBindingDigest(value)) fail("worker binding digest is stale");
    return value;
}

receipt readReceipt(const std::string& json) {
    const JsonValue& object = recordObject(json, "feature_work.receipt",
                                            {"schema", "kind", "request_digest", "global_index",
                                             "logical_name", "feature_path", "artifact_key", "payload_digest",
                                             "payload_bytes", "feature_count", "descriptor_dtype",
                                             "descriptor_dim", "width", "height", "extract_width",
                                             "extract_height", "producer_build", "producer_device", "digest"});
    receipt value;
    value.schema = number32(required(object, "schema"), "schema");
    value.request_digest = stringValue(required(object, "request_digest"), "request_digest");
    value.global_index = number32(required(object, "global_index"), "global_index");
    value.logical_name = stringValue(required(object, "logical_name"), "logical_name");
    value.feature_path = stringValue(required(object, "feature_path"), "feature_path");
    value.artifact_key = stringValue(required(object, "artifact_key"), "artifact_key");
    value.payload_digest = stringValue(required(object, "payload_digest"), "payload_digest");
    value.payload_bytes = number64(required(object, "payload_bytes"), "payload_bytes");
    value.feature_count = number32(required(object, "feature_count"), "feature_count");
    value.descriptor_dtype = number32(required(object, "descriptor_dtype"), "descriptor_dtype");
    value.descriptor_dim = number32(required(object, "descriptor_dim"), "descriptor_dim");
    value.width = signed32(required(object, "width"), "width");
    value.height = signed32(required(object, "height"), "height");
    value.extract_width = signed32(required(object, "extract_width"), "extract_width");
    value.extract_height = signed32(required(object, "extract_height"), "extract_height");
    value.producer_build = stringValue(required(object, "producer_build"), "producer_build");
    value.producer_device = stringValue(required(object, "producer_device"), "producer_device");
    value.digest = stringValue(required(object, "digest"), "digest");
    validateReceiptFields(value);
    if (value.digest != receiptDigest(value)) fail("receipt digest is stale");
    return value;
}

image_outcome readImageOutcome(const std::string& json) {
    const JsonValue& object = recordObject(json, "feature_work.image_outcome",
                                            {"schema", "kind", "global_index", "status",
                                             "receipt_digest", "error", "digest"});
    image_outcome value;
    value.schema = number32(required(object, "schema"), "schema");
    value.global_index = number32(required(object, "global_index"), "global_index");
    value.status = statusValue(stringValue(required(object, "status"), "status"));
    value.receipt_digest = stringValue(required(object, "receipt_digest"), "receipt_digest");
    value.error = stringValue(required(object, "error"), "error");
    value.digest = stringValue(required(object, "digest"), "digest");
    validateOutcomeFields(value);
    if (value.digest != outcomeDigest(value)) fail("outcome digest is stale");
    return value;
}

shard_result readShardResult(const std::string& json) {
    const JsonValue& object = recordObject(json, "feature_work.shard_result",
                                            {"schema", "kind", "plan_digest", "request_digest", "shard",
                                             "outcomes", "complete", "digest"});
    shard_result value;
    value.schema = number32(required(object, "schema"), "schema");
    value.plan_digest = stringValue(required(object, "plan_digest"), "plan_digest");
    value.request_digest = stringValue(required(object, "request_digest"), "request_digest");
    value.shard = number32(required(object, "shard"), "shard");
    const JsonValue& outcomes = required(object, "outcomes");
    if (!outcomes.is_array()) fail("shard result.outcomes is not an array");
    uint32_t previous = 0;
    bool first = true;
    for (const JsonValue& item : outcomes.arr) {
        const JsonValue& outcomeObject = objectValue(item, "outcome");
        exactKeys(outcomeObject, {"schema", "kind", "global_index", "status", "receipt_digest",
                                  "error", "digest"},
                  "feature_work.image_outcome");
        image_outcome outcome;
        outcome.schema = number32(required(outcomeObject, "schema"), "schema");
        if (stringValue(required(outcomeObject, "kind"), "kind") != "feature_work.image_outcome")
            fail("outcome has wrong kind");
        outcome.global_index = number32(required(outcomeObject, "global_index"), "global_index");
        outcome.status = statusValue(stringValue(required(outcomeObject, "status"), "status"));
        outcome.receipt_digest = stringValue(required(outcomeObject, "receipt_digest"), "receipt_digest");
        outcome.error = stringValue(required(outcomeObject, "error"), "error");
        if (!first && outcome.global_index <= previous)
            fail("shard result outcomes are not ordered");
        first = false;
        previous = outcome.global_index;
        outcome.digest = stringValue(required(outcomeObject, "digest"), "digest");
        validateOutcomeFields(outcome);
        if (outcome.digest != outcomeDigest(outcome)) fail("outcome digest is stale");
        value.outcomes.push_back(std::move(outcome));
    }
    value.complete = boolean(required(object, "complete"), "complete");
    value.digest = stringValue(required(object, "digest"), "digest");
    bool complete = true;
    for (const image_outcome& outcome : value.outcomes)
        complete = complete && successful(outcome.status);
    if (value.complete != complete) fail("shard result completion is stale");
    if (value.digest != shardResultDigest(value)) fail("shard result digest is stale");
    return value;
}

cancellation readCancellation(const std::string& json) {
    const JsonValue& object = recordObject(json, "feature_work.cancellation",
                                            {"schema", "kind", "plan_digest", "request_digest", "shard",
                                             "reason", "cancellation_digest"});
    cancellation value;
    value.schema = number32(required(object, "schema"), "schema");
    value.plan_digest = stringValue(required(object, "plan_digest"), "plan_digest");
    value.request_digest = stringValue(required(object, "request_digest"), "request_digest");
    value.shard = number32(required(object, "shard"), "shard");
    value.reason = stringValue(required(object, "reason"), "reason");
    value.cancellation_digest = stringValue(required(object, "cancellation_digest"),
                                             "cancellation_digest");
    validateCancellationFields(value);
    if (value.cancellation_digest != cancellationDigest(value)) fail("cancellation digest is stale");
    return value;
}

collection_index readCollectionIndex(const std::string& json) {
    const JsonValue& object = recordObject(json, "feature_work.collection_index",
                                            {"schema", "kind", "plan_digest", "rows",
                                             "collection_digest", "complete", "digest"});
    collection_index value;
    value.schema = number32(required(object, "schema"), "schema");
    value.plan_digest = stringValue(required(object, "plan_digest"), "plan_digest");
    const JsonValue& rows = required(object, "rows");
    if (!rows.is_array()) fail("collection.rows is not an array");
    for (const JsonValue& item : rows.arr) {
        const JsonValue& rowObject = objectValue(item, "accepted row");
        exactKeys(rowObject, {"global_index", "logical_name", "feature_path", "artifact_key",
                              "payload_digest", "payload_bytes", "feature_count", "receipt_digest",
                              "result_digest"},
                  "accepted row");
        accepted_row row;
        row.global_index = number32(required(rowObject, "global_index"), "global_index");
        row.logical_name = stringValue(required(rowObject, "logical_name"), "logical_name");
        row.feature_path = stringValue(required(rowObject, "feature_path"), "feature_path");
        row.artifact_key = stringValue(required(rowObject, "artifact_key"), "artifact_key");
        row.payload_digest = stringValue(required(rowObject, "payload_digest"), "payload_digest");
        row.payload_bytes = number64(required(rowObject, "payload_bytes"), "payload_bytes");
        row.feature_count = number32(required(rowObject, "feature_count"), "feature_count");
        row.receipt_digest = stringValue(required(rowObject, "receipt_digest"), "receipt_digest");
        row.result_digest = stringValue(required(rowObject, "result_digest"), "result_digest");
        value.rows.push_back(std::move(row));
    }
    uint32_t previous = 0;
    bool first = true;
    for (const accepted_row& row : value.rows) {
        validateRowFields(row);
        if (!first && row.global_index <= previous) fail("collection rows are not ordered");
        first = false;
        previous = row.global_index;
    }
    value.collection_digest = stringValue(required(object, "collection_digest"), "collection_digest");
    value.complete = boolean(required(object, "complete"), "complete");
    value.digest = stringValue(required(object, "digest"), "digest");
    if (value.collection_digest != collectionDigest(value)) fail("collection digest is stale");
    if (value.digest != collectionRecordDigest(value))
        fail("collection record digest is stale");
    return value;
}
void publishAtomic(const std::string& path, const std::string& bytes) {
    const std::filesystem::path destination(path);
    if (destination.empty()) fail("publication path is empty");
    std::error_code ec;
    if (std::filesystem::exists(spirula::NativeFilesystemPath(destination), ec) && !ec) {
        const std::string current = readFile(path);
        if (current == bytes) return;
        fail("immutable publication conflicts with " + path);
    }
    if (ec) fail("cannot inspect publication path " + path);
    if (!destination.parent_path().empty()) {
        std::filesystem::create_directories(
            spirula::NativeFilesystemPath(destination.parent_path()), ec);
        if (ec) fail("cannot create publication directory " + destination.parent_path().string());
    }
    static std::atomic<uint64_t> serial{0};
    const uint64_t stamp =
        (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
    std::string temp;
    for (uint64_t i = 0; i != 1000; ++i) {
        temp = path + ".part." + std::to_string(stamp) + "." +
               std::to_string(serial.fetch_add(1));
        if (!std::filesystem::exists(spirula::NativeFilesystemPath(
                std::filesystem::path(temp)), ec) && !ec) break;
        if (i == 999) fail("cannot create publication sibling " + path);
    }
    {
        std::ofstream out(spirula::NativeFilesystemPath(std::filesystem::path(temp)),
                          std::ios::binary | std::ios::trunc);
        if (!out) fail("cannot write publication " + path);
        out.write(bytes.data(), (std::streamsize)bytes.size());
        out.flush();
        if (!out) {
            std::error_code removeError;
            std::filesystem::remove(spirula::NativeFilesystemPath(
                                        std::filesystem::path(temp)), removeError);
            fail("cannot write publication " + path);
        }
    }
    std::filesystem::rename(spirula::NativeFilesystemPath(std::filesystem::path(temp)),
                            spirula::NativeFilesystemPath(destination), ec);
    if (ec) {
        std::error_code existsError;
        if (std::filesystem::exists(spirula::NativeFilesystemPath(destination), existsError) &&
            !existsError &&
            readFile(path) == bytes) {
            std::filesystem::remove(spirula::NativeFilesystemPath(
                                        std::filesystem::path(temp)), existsError);
            return;
        }
        std::filesystem::remove(spirula::NativeFilesystemPath(
                                    std::filesystem::path(temp)), existsError);
        fail("cannot publish " + path);
    }
}

#define FEATURE_WORK_FILE_FUNCS(Name, type)                                                    \
    void write##Name##File(const std::string& path, const type& value) {                       \
        publishAtomic(path, write##Name(value));                                               \
    }                                                                                          \
    type read##Name##File(const std::string& path) { return read##Name(readFile(path)); }

FEATURE_WORK_FILE_FUNCS(Recipe, recipe)
FEATURE_WORK_FILE_FUNCS(Plan, plan)
FEATURE_WORK_FILE_FUNCS(Request, request)
FEATURE_WORK_FILE_FUNCS(WorkerBinding, worker_binding)
FEATURE_WORK_FILE_FUNCS(Receipt, receipt)
FEATURE_WORK_FILE_FUNCS(ImageOutcome, image_outcome)
FEATURE_WORK_FILE_FUNCS(ShardResult, shard_result)
FEATURE_WORK_FILE_FUNCS(Cancellation, cancellation)
FEATURE_WORK_FILE_FUNCS(CollectionIndex, collection_index)

#undef FEATURE_WORK_FILE_FUNCS

}  // namespace sfm::feature_work
