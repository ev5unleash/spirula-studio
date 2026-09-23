#include "app/AgentFeatureJob.h"
#include "sfm/core/Features.h"
#include "sfm/tests/TestMain.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace fw = sfm::feature_work;
namespace agent = app::agent;

namespace {

void write_file(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("test file write failed");
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("test file read failed");
    return std::string(std::istreambuf_iterator<char>(input), {});
}

bool fails(const std::function<void()>& body, const char* fragment = nullptr) {
    try {
        body();
    } catch (const std::exception& error) {
        return !fragment || std::string(error.what()).find(fragment) != std::string::npos;
    }
    return false;
}

void check(bool condition, const char* message, int& failures) {
    if (condition) return;
    std::printf("  FAIL: %s\n", message);
    ++failures;
}

fw::FeaturePlan make_plan(const std::vector<std::string>& image_bytes,
                          const std::vector<std::string>& mask_bytes) {
    fw::FeaturePlan plan;
    plan.extraction.frontend = "sift";
    plan.extraction.descriptor = "sift-u8";
    plan.extraction.descriptor_dim = 4;
    plan.extraction.descriptor_dtype = 0;
    plan.extraction.implementation = "test-build";
    const std::vector<std::string> names = {
        "group/a.jpg", "group/b.jpg", "group/c.jpg"};
    for (std::uint32_t i = 0; i < names.size(); ++i) {
        fw::PlanImage image;
        image.global_index = i;
        image.logical_name = names[i];
        image.feature_path = fw::featurePathForImage(image.logical_name);
        image.source_digest = fw::sha256Text(image_bytes[i]);
        image.owner_shard = i < 2 ? 7 : 9;
        if (i < 2) {
            image.mask_path = "group/" + std::string(i == 0 ? "a" : "b") + ".png";
            image.mask_digest = fw::sha256Text(mask_bytes[i]);
        }
        image.artifact_key = fw::imageArtifactKey(
            image.source_digest, image.mask_digest, plan.extraction);
        plan.images.push_back(std::move(image));
    }
    plan.dataset_digest = fw::datasetSnapshotDigest(plan.images);
    plan.digest = fw::planDigest(plan);
    fw::validatePlan(plan);
    return plan;
}

int run_test(int, char**) {
    int failures = 0;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("spirula-feature-bundle-" + std::to_string(stamp));
    const std::vector<std::string> image_bytes = {"image A", "image B", "image C"};
    const std::vector<std::string> mask_bytes = {"mask A", "mask B"};
    try {
        const fs::path source = root / "source";
        const fs::path images = source / "images";
        const fs::path masks = source / "masks";
        const fs::path records = source / "records";
        const fs::path plan_file = records / "nested" / "feature-plan.json";
        const fs::path request_file = records / "request.json";
        const fs::path bundle_root = root / "bundle";
        fs::create_directories(images);
        fs::create_directories(masks);
        fs::create_directories(plan_file.parent_path());
        fs::create_directories(bundle_root);
        for (std::size_t i = 0; i < image_bytes.size(); ++i)
            write_file(images / fs::u8path("group/" + std::string(1, "abc"[i]) + ".jpg"),
                       image_bytes[i]);
        for (std::size_t i = 0; i < mask_bytes.size(); ++i)
            write_file(masks / fs::u8path("group/" + std::string(1, "ab"[i]) + ".png"),
                       mask_bytes[i]);
        write_file(masks / "group/decoy.png", "wrong mask");

        const fw::FeaturePlan plan = make_plan(image_bytes, mask_bytes);
        fw::FeatureRequest request;
        request.plan_digest = fw::planDigest(plan);
        request.plan_path = "nested/feature-plan.json";
        request.shard = 7;
        request.attempt_id = "attempt-test";
        request.image_indices = {0, 1};
        request.digest = fw::requestDigest(request);
        fw::validateRequest(plan, request);
        fw::writePlanFile(plan_file.u8string(), plan);
        fw::writeRequestFile(request_file.u8string(), request);

        const agent::FeatureInputBundle staged = agent::StageFeatureInputs(
            plan_file, request_file, images, masks, bundle_root, 1024 * 1024);
        const std::set<std::string> expected_paths = {
            "nested/feature-plan.json", "request.json", "images/group/a.jpg",
            "images/group/b.jpg", "masks/group/a.png", "masks/group/b.png"};
        std::set<std::string> actual_paths;
        for (const agent::TransferFile& file : staged.manifest) {
            actual_paths.insert(file.path);
            if (file.path == "images/group/a.jpg")
                check(file.size == image_bytes[0].size() &&
                          file.sha256 == fw::sha256Text(image_bytes[0]),
                      "manifest binds the first selected image bytes", failures);
            if (file.path == "masks/group/b.png")
                check(file.size == mask_bytes[1].size() &&
                          file.sha256 == fw::sha256Text(mask_bytes[1]),
                      "manifest binds the selected mask bytes", failures);
        }
        check(actual_paths == expected_paths,
              "manifest contains metadata and only the two assigned image/mask pairs",
              failures);
        check(staged.request.digest == request.digest &&
                  staged.request.plan_path == request.plan_path,
              "noncanonical plan path is preserved without changing request identity",
              failures);
        check(!fs::exists(bundle_root / "images/group/c.jpg") &&
                  !fs::exists(bundle_root / "masks/group/c.png") &&
                  !fs::exists(bundle_root / "masks/group/decoy.png"),
              "unassigned images and unrelated masks are not staged", failures);
        check(read_file(staged.image_root / "group/a.jpg") == image_bytes[0] &&
                  read_file(staged.mask_root / "group/b.png") == mask_bytes[1],
              "staged feature inputs retain the selected source and mask bytes", failures);

        const agent::FeatureInputBundle verified = agent::VerifyFeatureInputs(
            bundle_root, plan, request, staged.manifest, 1024 * 1024);
        check(fw::planDigest(verified.plan) == fw::planDigest(plan) &&
                  fw::requestDigest(verified.request) == fw::requestDigest(request),
              "receiver verification returns the immutable plan and request", failures);

        const fs::path output_root = root / "feature-output";
        fs::create_directories(output_root);
        fw::writeRequestFile((output_root / "request.json").u8string(), request);
        fw::WorkerBinding binding;
        binding.extraction_request = request;
        binding.request_path = "request.json";
        binding.image_root = images.u8string();
        binding.mask_root = masks.u8string();
        binding.model_root = (root / "worker-model").u8string();
        binding.device = "fixture-device";
        binding.output_dir = (root / "worker-output").u8string();
        binding.max_payload_bytes = 1024 * 1024;
        binding.max_images = static_cast<std::uint32_t>(request.image_indices.size());
        binding.digest = fw::workerBindingDigest(binding);
        fw::writeWorkerBindingFile((output_root / "binding.json").u8string(),
                                   binding);

        fw::ShardResult output_result;
        output_result.plan_digest = fw::planDigest(plan);
        output_result.request_digest = fw::requestDigest(request);
        output_result.shard = request.shard;
        output_result.complete = true;
        for (const std::uint32_t index : request.image_indices) {
            const fw::PlanImage& image = plan.images[index];
            const fs::path payload =
                output_root / "payload" / fs::u8path(image.feature_path);
            fs::create_directories(payload.parent_path());
            sfm::FeatureSet empty_features;
            empty_features.width = 32;
            empty_features.height = 24;
            empty_features.dim = plan.extraction.descriptor_dim;
            empty_features.dtype =
                static_cast<sfm::DType>(plan.extraction.descriptor_dtype);
            sfm::writeFeatures(payload.u8string(), empty_features);

            fw::ImageReceipt receipt;
            receipt.request_digest = request.digest;
            receipt.global_index = index;
            receipt.logical_name = image.logical_name;
            receipt.feature_path = image.feature_path;
            receipt.artifact_key = image.artifact_key;
            receipt.payload_digest = fw::sha256File(payload.u8string());
            receipt.payload_bytes = fs::file_size(payload);
            receipt.feature_count = 0;
            receipt.descriptor_dtype = plan.extraction.descriptor_dtype;
            receipt.descriptor_dim = plan.extraction.descriptor_dim;
            receipt.width = empty_features.width;
            receipt.height = empty_features.height;
            receipt.producer_build = plan.extraction.implementation;
            receipt.producer_device = "fixture-device";
            receipt.digest = fw::receiptDigest(receipt);
            std::string receipt_name = std::to_string(index);
            if (receipt_name.size() < 8)
                receipt_name.insert(0, 8 - receipt_name.size(), '0');
            fw::writeReceiptFile(
                (output_root / "receipts" / (receipt_name + ".json")).u8string(),
                receipt);

            fw::ImageOutcome outcome;
            outcome.global_index = index;
            outcome.status = fw::outcome_status::zero_features;
            outcome.receipt_digest = receipt.digest;
            outcome.digest = fw::outcomeDigest(outcome);
            output_result.outcomes.push_back(std::move(outcome));
        }
        output_result.digest = fw::shardResultDigest(output_result);
        fw::writeShardResultFile((output_root / "result.json").u8string(),
                                 output_result);

        const fs::path lease_marker = output_root / ".spirula-output.lock";
        write_file(lease_marker, "");
        const std::vector<agent::TransferFile> output_manifest =
            agent::FeatureOutputManifest(request_file, output_root, 1024 * 1024);
        std::set<std::string> output_paths;
        for (const agent::TransferFile& file : output_manifest)
            output_paths.insert(file.path);
        std::set<std::string> expected_output_paths = {
            "binding.json", "result.json", "request.json"};
        for (const std::uint32_t index : request.image_indices) {
            std::string digits = std::to_string(index);
            if (digits.size() < 8) digits.insert(0, 8 - digits.size(), '0');
            expected_output_paths.insert("receipts/" + digits + ".json");
            expected_output_paths.insert("payload/" + plan.images[index].feature_path);
        }
        check(output_manifest.size() == expected_output_paths.size() &&
                  output_paths == expected_output_paths,
              "output manifest covers only metadata and each selected receipt/payload",
              failures);
        const fw::WorkerBinding redacted_binding = fw::readWorkerBindingFile(
            (output_root / "binding.json").u8string());
        check(redacted_binding.image_root.empty() &&
                  redacted_binding.mask_root.empty() &&
                  redacted_binding.model_root.empty() &&
                  redacted_binding.device.empty() &&
                  redacted_binding.output_dir.empty(),
              "manifest removes worker-local absolute paths before transfer", failures);
        check(fails([&] {
                  agent::VerifyFeatureOutputs(request_file, output_root,
                                              output_manifest, 1024 * 1024);
              }, "unauthorized file"),
              "leader-side verification rejects the scheduler lease marker",
              failures);
        write_file(lease_marker, "occupied");
        check(fails([&] {
                  agent::FeatureOutputManifest(request_file, output_root,
                                               1024 * 1024);
              }, "worker output lease marker is not empty"),
              "manifest rejects a nonempty scheduler lease marker", failures);
        fs::remove(lease_marker);
        agent::VerifyFeatureOutputs(request_file, output_root, output_manifest,
                                    1024 * 1024);
        write_file(output_root / "payload/unapproved.bin", "unapproved");
        check(fails([&] {
                  agent::VerifyFeatureOutputs(request_file, output_root,
                                              output_manifest, 1024 * 1024);
              }, "unauthorized file"),
              "output verification rejects files outside the exact result manifest",
              failures);
        fs::remove(output_root / "payload/unapproved.bin");

        const fs::path hash_reject_root = root / "hash-reject";
        fs::create_directories(hash_reject_root);
        write_file(images / "group/a.jpg", "changed image");
        check(fails([&] {
                  agent::StageFeatureInputs(plan_file, request_file, images, masks,
                                            hash_reject_root, 1024 * 1024);
              }, "digest mismatch"),
              "staging rejects changed image content", failures);
        check(fs::is_empty(hash_reject_root),
              "hash failure removes all partial bundle files", failures);
        write_file(images / "group/a.jpg", image_bytes[0]);

        const fs::path missing_mask_root = root / "missing-mask-reject";
        fs::create_directories(missing_mask_root);
        fs::remove(masks / "group/a.png");
        check(fails([&] {
                  agent::StageFeatureInputs(plan_file, request_file, images, masks,
                                            missing_mask_root, 1024 * 1024);
              }, "group/a.png"),
              "staging rejects a missing selected mask instead of using a decoy", failures);
        check(fs::is_empty(missing_mask_root),
              "missing-mask failure removes all partial bundle files", failures);

        std::string traversal_request = fw::writeRequest(request);
        const std::string path_field = "nested/feature-plan.json";
        const std::size_t path_at = traversal_request.find(path_field);
        if (path_at == std::string::npos)
            throw std::runtime_error("test request path field was not serialized");
        traversal_request.replace(path_at, path_field.size(), "../outside-plan.json");
        const fs::path hostile_request = records / "hostile-request.json";
        write_file(hostile_request, traversal_request);
        write_file(source / "outside-plan.json", "untouched");
        const fs::path traversal_root = root / "traversal-reject";
        fs::create_directories(traversal_root);
        check(fails([&] {
                  agent::StageFeatureInputs(plan_file, hostile_request, images, masks,
                                            traversal_root, 1024 * 1024);
              }, "dot-dot"),
              "request plan traversal is rejected before staging", failures);
        check(read_file(source / "outside-plan.json") == "untouched" &&
                  fs::is_empty(traversal_root),
              "traversal rejection leaves outside files and bundle root untouched", failures);
    } catch (...) {
        std::error_code ec;
        fs::remove_all(root, ec);
        throw;
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    if (failures == 0) std::printf("agent_feature_job_test: OK\n");
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) { return sfmTestMain(argc, argv, run_test); }
