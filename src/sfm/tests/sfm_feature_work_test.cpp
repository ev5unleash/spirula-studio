#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <utility>

#include "sfm/core/FeatureWork.h"
#include "sfm/core/Features.h"
#include "sfm/Pipeline.h"
#include "sfm/tests/TestMain.h"

namespace fw = sfm::feature_work;
namespace fs = std::filesystem;

static bool throws(const std::function<void()>& body) {
    try {
        body();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

static void check(bool condition, const char* message, int& failures) {
    if (condition) return;
    std::printf("  FAIL: %s\n", message);
    ++failures;
}

static fw::plan makePlan() {
    fw::plan value;
    value.extraction.frontend = "sift";
    value.extraction.descriptor = "sift-u8";
    value.extraction.descriptor_dim = 4;
    value.extraction.descriptor_dtype = 0;
    value.extraction.implementation = "test-build";
    for (const std::string& name : {"frames/a.jpg", "frames/b.jpg"}) {
        fw::plan_image image;
        image.global_index = (uint32_t)value.images.size();
        image.logical_name = name;
        image.feature_path = fw::featurePathForImage(name);
        image.source_digest = fw::sha256Text(name + " source");
        image.owner_shard = image.global_index;
        image.artifact_key = fw::imageArtifactKey(image.source_digest, image.mask_digest,
                                                  value.extraction);
        value.images.push_back(image);
    }
    value.dataset_digest = fw::datasetSnapshotDigest(value.images);
    return value;
}

static fw::receipt makeReceipt(const fw::plan& plan, const fw::request& request,
                               uint32_t index, uint32_t count, const std::string& payload) {
    fw::receipt value;
    value.request_digest = fw::requestDigest(request);
    value.global_index = index;
    value.logical_name = plan.images[index].logical_name;
    value.feature_path = plan.images[index].feature_path;
    value.artifact_key = plan.images[index].artifact_key;
    value.payload_digest = fw::sha256Text(payload);
    value.payload_bytes = payload.size();
    value.feature_count = count;
    value.descriptor_dtype = plan.extraction.descriptor_dtype;
    value.descriptor_dim = plan.extraction.descriptor_dim;
    value.width = value.height = 32;
    value.producer_build = "test";
    return value;
}
static std::string writeTestPayload(const fs::path& path, int width = 16, int height = 12) {
    sfm::FeatureSet features;
    features.width = features.extract_width = width;
    features.height = features.extract_height = height;
    features.dim = 4;
    features.dtype = sfm::DType::U8;
    fs::create_directories(path.parent_path());
    sfm::writeFeatures(path.string(), features);
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}


static int cmdFeatureWorkTest(int, char**) {
    int failures = 0;
    fw::plan plan = makePlan();
    check(!throws([&] { fw::validatePlan(plan); }), "plan identity validates", failures);
    plan.digest = fw::planDigest(plan);
    fw::validatePlan(plan);
    check(fw::planDigest(plan) == fw::planDigest(plan), "plan identity is deterministic", failures);

    fw::plan relocated = plan;
    fw::plan identical = plan;
    identical.images[1].source_digest = identical.images[0].source_digest;
    identical.images[1].artifact_key = fw::imageArtifactKey(
        identical.images[1].source_digest, identical.images[1].mask_digest,
        identical.extraction);
    identical.dataset_digest = fw::datasetSnapshotDigest(identical.images);
    identical.digest.clear();
    check(identical.images[0].artifact_key == identical.images[1].artifact_key &&
              !throws([&] { fw::validatePlan(identical); }),
          "content-identical images share an artifact key", failures);
    const std::string changedMask = fw::sha256Text("changed mask");
    check(fw::imageArtifactKey(plan.images[0].source_digest, changedMask,
                               plan.extraction) != plan.images[0].artifact_key,
          "mask content changes the artifact key", failures);
    fw::recipe changedRecipe = plan.extraction;
    changedRecipe.settings["threshold"] = "different";
    check(fw::imageArtifactKey(plan.images[0].source_digest, plan.images[0].mask_digest,
                               changedRecipe) != plan.images[0].artifact_key,
          "recipe changes the artifact key", failures);
    fw::collection_index shared;
    shared.plan_digest = fw::planDigest(identical);
    shared.complete = true;
    for (const auto& image : identical.images) {
        fw::accepted_row row;
        row.global_index = image.global_index;
        row.logical_name = image.logical_name;
        row.feature_path = image.feature_path;
        row.artifact_key = image.artifact_key;
        row.payload_digest = fw::sha256Text("shared payload");
        row.payload_bytes = 14;
        row.feature_count = 0;
        row.receipt_digest = fw::sha256Text("shared receipt");
        row.result_digest = fw::sha256Text("shared result");
        shared.rows.push_back(std::move(row));
    }
    shared.collection_digest = fw::collectionDigest(shared);
    check(!throws([&] { fw::validateCollection(identical, shared); }),
          "duplicate dependency keys accept identical verified rows", failures);
    fw::collection_index incompatible = shared;
    incompatible.rows[1].payload_digest = fw::sha256Text("other payload");
    incompatible.rows[1].payload_bytes = 13;
    incompatible.collection_digest = fw::collectionDigest(incompatible);
    check(throws([&] { fw::validateCollection(identical, incompatible); }),
          "incompatible duplicate artifacts are rejected", failures);
    relocated.digest.clear();
    check(fw::planDigest(relocated) == fw::planDigest(plan), "root relocation does not alter identity",
          failures);

    fw::plan changed = plan;
    changed.images[0].source_digest = fw::sha256Text("mutated source");
    changed.images[0].artifact_key = fw::imageArtifactKey(
        changed.images[0].source_digest, changed.images[0].mask_digest, changed.extraction);
    changed.dataset_digest = fw::datasetSnapshotDigest(changed.images);
    changed.digest = plan.digest;
    check(throws([&] { fw::validatePlan(changed); }),
          "changed content invalidates old plan digest", failures);

    fw::plan stemCollision = plan;
    stemCollision.images[1].logical_name = "frames/a.png";
    stemCollision.images[1].feature_path = fw::featurePathForImage(stemCollision.images[1].logical_name);
    stemCollision.images[1].artifact_key = fw::imageArtifactKey(
        stemCollision.images[1].source_digest, stemCollision.images[1].mask_digest,
        stemCollision.extraction);
    check(throws([&] { fw::validatePlan(stemCollision); }), "stem collision is rejected", failures);

    fw::plan caseCollision = plan;
    caseCollision.images[1].logical_name = "frames/A.jpg";
    caseCollision.images[1].feature_path = fw::featurePathForImage(caseCollision.images[1].logical_name);
    caseCollision.images[1].artifact_key = fw::imageArtifactKey(
        caseCollision.images[1].source_digest, caseCollision.images[1].mask_digest,
        caseCollision.extraction);
    caseCollision.digest.clear();
    check(throws([&] { fw::validatePlan(caseCollision); }), "case collision is rejected", failures);

    const std::string planJson = fw::writePlan(plan);
    check(fw::readPlan(planJson).digest == fw::planDigest(plan), "plan JSON round-trip", failures);
    std::string badJson = planJson;
    const size_t at = badJson.find("feature_work.plan");
    badJson.replace(at, std::string("feature_work.plan").size(), "feature_work.bad");
    check(throws([&] { fw::readPlan(badJson); }), "bad JSON identity is rejected", failures);

    fw::request request;
    request.plan_digest = fw::planDigest(plan);
    request.plan_path = "plans/plan.json";
    request.shard = 0;
    request.attempt_id = "attempt-0";
    request.image_indices = {0};
    request.digest = fw::requestDigest(request);
    fw::validateRequest(plan, request);
    check(throws([&] {
        fw::request escaped = request;
        escaped.plan_path = "../plan.json";
        fw::validateRequest(plan, escaped);
    }), "escaping plan path is rejected", failures);

    fw::image_outcome zero;
    zero.global_index = 0;
    zero.status = fw::outcome_status::zero_features;
    zero.receipt_digest = fw::receiptDigest(makeReceipt(plan, request, 0, 0, "zero"));
    zero.digest = fw::outcomeDigest(zero);
    fw::shard_result result;
    result.plan_digest = fw::planDigest(plan);
    result.request_digest = fw::requestDigest(request);
    result.shard = 0;
    result.outcomes = {zero};
    result.complete = true;
    result.digest = fw::shardResultDigest(result);
    fw::validateShardResult(plan, request, result);
    check(throws([&] {
        fw::shard_result incomplete = result;
        incomplete.outcomes.clear();
        fw::validateShardResult(plan, request, incomplete);
    }), "incomplete result is rejected", failures);

    fw::cancellation cancelled;
    cancelled.plan_digest = fw::planDigest(plan);
    cancelled.request_digest = fw::requestDigest(request);
    cancelled.shard = 0;
    cancelled.reason = "operator cancelled";
    cancelled.cancellation_digest = fw::cancellationDigest(cancelled);
    fw::validateCancellation(plan, request, cancelled);
    check(throws([&] { fw::validateShardResult(plan, request, result, &cancelled); }),
          "cancelled result is rejected", failures);

    fw::collection_index collection;
    collection.plan_digest = fw::planDigest(plan);
    collection.complete = false;
    fw::accepted_row row;
    row.global_index = 0;
    row.logical_name = plan.images[0].logical_name;
    row.feature_path = plan.images[0].feature_path;
    row.artifact_key = plan.images[0].artifact_key;
    row.payload_digest = fw::sha256Text("zero");
    row.payload_bytes = 4;
    row.feature_count = 0;
    row.receipt_digest = zero.receipt_digest;
    row.result_digest = result.digest;
    collection.rows = {row};
    collection.collection_digest = fw::collectionDigest(collection);
    fw::validateCollection(plan, collection);
    fw::collection_index reprovenanced = collection;
    reprovenanced.rows[0].receipt_digest = fw::sha256Text("new receipt");
    reprovenanced.rows[0].result_digest = fw::sha256Text("new result");
    check(fw::collectionDigest(reprovenanced) == collection.collection_digest,
          "collection identity excludes attempt provenance", failures);

    const fs::path dir = fs::temp_directory_path() / "spirula_feature_work_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    sfm::FeatureSet features;
    features.width = features.extract_width = 16;
    features.height = features.extract_height = 12;
    features.dim = 4;
    const fs::path payload = dir / "zero.bin";
    sfm::writeFeatures(payload.string(), features);
    check(sfm::readFeatures(payload.string()).count() == 0, "zero-feature payload succeeds", failures);
    std::ifstream in(payload, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), {});
    std::ofstream(payload, std::ios::binary | std::ios::trunc).write(bytes.data(), bytes.size() - 1);
    uint32_t count = 123;
    check(!sfm::peekFeatures(payload.string(), count) && count == 0,
          "truncated VKFT is rejected", failures);
    std::ofstream(payload, std::ios::binary | std::ios::trunc).write(bytes.data(), bytes.size());
    bytes.push_back('\0');
    std::ofstream(payload, std::ios::binary | std::ios::trunc).write(bytes.data(), bytes.size());
    check(!sfm::peekFeatures(payload.string(), count), "VKFT trailing bytes are rejected", failures);
    bytes.pop_back();
    bytes[50] = 9;
    std::ofstream(payload, std::ios::binary | std::ios::trunc).write(bytes.data(), bytes.size());
    check(!sfm::peekFeatures(payload.string(), count), "VKFT orientation is rejected", failures);
    const fs::path oldCollection = dir / "old";
    fs::create_directories(oldCollection, ec);
    fw::collection_index oldIndex;
    oldIndex.plan_digest = plan.digest;
    oldIndex.complete = true;
    for (const auto& image : plan.images) {
        const fs::path file = oldCollection / fs::u8path(image.feature_path);
        fs::create_directories(file.parent_path(), ec);
        const std::string payloadBytes =
            writeTestPayload(file, 16 + (int)image.global_index, 12);
        fw::accepted_row accepted;
        accepted.global_index = image.global_index;
        accepted.logical_name = image.logical_name;
        accepted.feature_path = image.feature_path;
        accepted.artifact_key = image.artifact_key;
        accepted.payload_digest = fw::sha256Text(payloadBytes);
        accepted.payload_bytes = payloadBytes.size();
        accepted.receipt_digest = fw::sha256Text("receipt " + image.logical_name);
        accepted.feature_count = 0;
        accepted.result_digest = fw::sha256Text("result " + image.logical_name);
        oldIndex.rows.push_back(std::move(accepted));
    }
    oldIndex.collection_digest = fw::collectionDigest(oldIndex);
    fw::writePlanFile((oldCollection / "plan.json").string(), plan);
    fw::writeCollectionIndexFile((oldCollection / "index.json").string(), oldIndex);

    fw::plan replanned = plan;
    replanned.images[0].logical_name = "frames/aa.jpg";
    replanned.images[0].feature_path =
        fw::featurePathForImage(replanned.images[0].logical_name);
    replanned.images[0].artifact_key = fw::imageArtifactKey(
        replanned.images[0].source_digest, replanned.images[0].mask_digest,
        replanned.extraction);
    replanned.images[0].owner_shard = 1;
    replanned.images[1].owner_shard = 0;
    replanned.dataset_digest = fw::datasetSnapshotDigest(replanned.images);
    replanned.digest.clear();
    replanned.digest = fw::planDigest(replanned);
    std::vector<std::string> requestPaths;
    for (uint32_t shard = 0; shard < 2; ++shard) {
        fw::request next =
            sfm::makeFeatureRequest(replanned, shard, "replan-" + std::to_string(shard));
        const fs::path requestPath =
            dir / ("request-" + std::to_string(shard) + ".json");
        fw::writeRequestFile(requestPath.string(), next);
        requestPaths.push_back(requestPath.string());
    }
    const fw::collection_index adopted = sfm::collectFeatureResults(
        replanned, requestPaths, {}, {}, dir / "new", {oldCollection});
    check(adopted.complete && adopted.rows.size() == replanned.images.size(),
          "replanned collection adopts complete coordinator payloads", failures);
    check(adopted.collection_digest == oldIndex.collection_digest,
          "replanned adoption preserves collection identity", failures);
    check(adopted.rows[0].receipt_digest == oldIndex.rows[0].receipt_digest,
          "replanned adoption preserves original provenance", failures);
    check(adopted.rows[0].feature_path == replanned.images[0].feature_path,
          "renamed adoption remaps the feature path", failures);
    check(adopted.rows[0].artifact_key == plan.images[0].artifact_key,
          "renamed adoption keeps the dependency key", failures);
    fw::collection_index incomplete = oldIndex;
    incomplete.complete = false;
    incomplete.collection_digest = fw::collectionDigest(incomplete);
    incomplete.digest.clear();
    const fs::path incompleteCollection = dir / "incomplete";
    fs::create_directories(incompleteCollection, ec);
    fw::writePlanFile((incompleteCollection / "plan.json").string(), plan);
    fw::writeCollectionIndexFile((incompleteCollection / "index.json").string(),
                                 incomplete);
    check(throws([&] {
        sfm::collectFeatureResults(replanned, requestPaths, {}, {},
                                   dir / "incomplete-new", {incompleteCollection});
    }), "incomplete collections refuse adoption", failures);

    fw::collection_index truncated = oldIndex;
    const fs::path truncatedCollection = dir / "truncated";
    fs::create_directories(truncatedCollection, ec);
    for (size_t i = 0; i < truncated.rows.size(); ++i) {
        const fs::path file =
            truncatedCollection / fs::u8path(truncated.rows[i].feature_path);
        std::string payloadBytes = writeTestPayload(
            file, 16 + (int)i, 12);
        if (i == 0) {
            payloadBytes.resize(payloadBytes.size() - 1);
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out.write(payloadBytes.data(), (std::streamsize)payloadBytes.size());
        }
        truncated.rows[i].payload_digest = fw::sha256Text(payloadBytes);
        truncated.rows[i].payload_bytes = payloadBytes.size();
    }
    truncated.collection_digest = fw::collectionDigest(truncated);
    truncated.digest.clear();
    fw::writePlanFile((truncatedCollection / "plan.json").string(), plan);
    fw::writeCollectionIndexFile((truncatedCollection / "index.json").string(),
                                 truncated);
    check(throws([&] {
        sfm::collectFeatureResults(replanned, requestPaths, {}, {},
                                   dir / "truncated-new", {truncatedCollection});
    }), "truncated payloads refuse adoption", failures);
    fs::remove_all(dir, ec);

    if (failures == 0) std::printf("sfm_feature_work_test: OK\n");
    return failures == 0 ? 0 : 1;
}

int main() { return sfmTestMain(0, nullptr, cmdFeatureWorkTest); }
