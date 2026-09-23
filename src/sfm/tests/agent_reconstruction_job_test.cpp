#include "app/AgentReconstructionJob.h"
#include "core/Sha256.h"
#include "sfm/Pipeline.h"
#include "sfm/core/Model.h"
#include "sfm/tests/TestMain.h"

#include <cstdint>
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
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
namespace agent = app::agent;

namespace {

constexpr std::uint64_t kBudget = 1024 * 1024;

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

std::string digest(const std::string& bytes) {
    spirula::Sha256 hash;
    hash.update(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    return hash.hex();
}

bool fails(const std::function<void()>& body, const char* fragment = nullptr) {
    try {
        body();
    } catch (const std::exception& error) {
        return !fragment || std::string(error.what()).find(fragment) != std::string::npos;
    }
    return false;
}

bool same_path(const std::string& actual, const fs::path& expected) {
    std::error_code ec;
    return fs::equivalent(fs::u8path(actual), expected, ec) && !ec;
}

void check(bool condition, const char* message, int& failures) {
    if (condition) return;
    std::printf("  FAIL: %s\n", message);
    ++failures;
}

sfm::AutoRequest make_request(const fs::path& image_root,
                              const fs::path& manifest,
                              const fs::path& workspace) {
    sfm::AutoRequest request;
    const std::string error = sfm::parse_auto_args(
        {image_root.u8string(), "-o", workspace.u8string(), "--manifest",
         manifest.u8string(), "--quality", "low", "--camera-mode", "image",
         "--max-error", "4.25", "--threads", "3", "--pairs", "auto",
         "--max-features", "4200"}, request, false);
    if (!error.empty()) throw std::runtime_error(error);
    return request;
}

sfm::Reconstruction make_model() {
    sfm::Reconstruction model;
    model.cameras.emplace(1, sfm::Camera::defaultFor(
        1, 2, 2, 1.2, sfm::CamModel::Pinhole));
    for (std::uint32_t id = 1; id <= 2; ++id) {
        sfm::Image image;
        image.id = id;
        image.camera_id = 1;
        image.name = id == 1 ? "cam0/a.ppm" : "cam1/a.ppm";
        image.registered = true;
        image.pose = {sfm::mat3Identity(), {0, 0, 0}};
        image.points2D = {{1, 1}};
        image.point3D_ids = {10};
        model.images.emplace(id, std::move(image));
    }
    sfm::Point3D point;
    point.xyz = {0, 0, 2};
    point.rgb[0] = point.rgb[1] = point.rgb[2] = 128;
    point.track = {{1, 0}, {2, 0}};
    model.points3D.emplace(10, std::move(point));
    return model;
}

void write_model(const fs::path& sparse_root, const sfm::Reconstruction& model) {
    fs::create_directories(sparse_root / "0");
    model.writeBinary((sparse_root / "0").u8string());
}

int run_test(int, char**) {
    int failures = 0;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("spirula-reconstruction-bundle-" + std::to_string(stamp));
    try {
        const fs::path source = root / "source";
        const fs::path image_root = source / "images";
        const fs::path masks = source / "masks";
        const fs::path manifest = source / "manifest.json";
        const fs::path workspace = root / "workspace";
        const fs::path bundle_root = root / "bundle";
        const std::string ppm = "P6\n2 2\n255\n" + std::string(12, 'x');
        write_file(image_root / "cam0/a.ppm", ppm);
        write_file(image_root / "cam1/a.ppm", ppm);
        write_file(masks / "cam0/a.ppm.png", "mask-a");
        write_file(masks / "cam1/a.ppm.png", "mask-b");
        write_file(source / "capture.bin", "telemetry-payload");
        const std::string manifest_bytes =
            "{\"image_dir\":\"images\",\"mask_dir\":\"masks\","
            "\"camera_mode\":\"folder\","
            "\"cameras\":[{\"prefix\":\"cam0\",\"model\":\"pinhole\",\"focal\":1.2},"
            "{\"prefix\":\"cam1\",\"model\":\"pinhole\",\"focal\":1.2}],"
            "\"captures\":[{\"prefix\":\"clip\",\"telemetry\":\"capture.bin\","
            "\"fps\":30,\"source_export_mapping\":\"mapping-1\","
            "\"timing_estimate\":\"estimate-1\","
            "\"synchronization_decision\":\"decision-1\"}],"
            "\"rigs\":[{\"name\":\"stereo\",\"captures\":[\"clip\"],"
            "\"members\":[{\"prefix\":\"cam0\",\"rotation\":[1,0,0,0],"
            "\"translation\":[0,0,0]},{\"prefix\":\"cam1\","
            "\"rotation\":[1,0,0,0],\"translation\":[0.1,0,0]}]}]}";
        write_file(manifest, manifest_bytes);
        fs::create_directories(bundle_root);

        const sfm::AutoRequest request = make_request(image_root, manifest, workspace);
        const agent::ReconstructionInputBundle staged = agent::StageReconstructionInputs(
            request, manifest, bundle_root, kBudget);
        std::set<std::string> actual_paths;
        for (const agent::TransferFile& file : staged.files)
            actual_paths.insert(file.path);
        const std::set<std::string> expected_paths = {
            "bundle.json", "manifest.json", "request.json", "images/cam0/a.ppm",
            "images/cam1/a.ppm", "masks/cam0/a.ppm.png", "masks/cam1/a.ppm.png",
            "telemetry/00000000.bin"};
        check(actual_paths == expected_paths,
              "input bundle contains only the selected images, masks and telemetry",
              failures);
        check(staged.source_manifest_sha256 == digest(manifest_bytes) &&
                  read_file(manifest) == manifest_bytes &&
                  read_file(staged.manifest_path) != manifest_bytes,
              "portable manifest is rebased without overwriting the source manifest",
              failures);
        bool has_cam0 = false;
        bool has_cam1 = false;
        for (const sfm::ManifestCamera& camera : staged.manifest.cameras) {
            has_cam0 = has_cam0 || (camera.prefix == "cam0" &&
                                    camera.model == "pinhole" && camera.focal == 1.2);
            has_cam1 = has_cam1 || (camera.prefix == "cam1" &&
                                    camera.model == "pinhole" && camera.focal == 1.2);
        }
        check(has_cam0 && has_cam1 && staged.manifest.image_dir == "images" &&
                  staged.manifest.mask_dir == "masks" &&
                  staged.manifest.captures.size() == 1 &&
                  staged.manifest.captures[0].telemetry == "telemetry/00000000.bin" &&
                  staged.manifest.captures[0].timing_estimate == "estimate-1" &&
                  staged.manifest.rigs.size() == 1 &&
                  staged.manifest.rigs[0].name == "stereo",
              "portable manifest preserves camera, capture, rig and rooted-path semantics",
              failures);
        check(read_file(staged.image_root / "cam0/a.ppm") == ppm &&
                  read_file(bundle_root / "telemetry/00000000.bin") == "telemetry-payload",
              "staged data is copied under the attempt root", failures);

        fs::create_directories(bundle_root / ".agent-transfer-v1");
        const agent::ReconstructionInputBundle verified = agent::VerifyReconstructionInputs(
            bundle_root, staged.files, staged.identity_sha256, kBudget);
        check(verified.identity_sha256 == staged.identity_sha256,
              "receiver verification accepts the exact staged input identity", failures);
        const std::string changed_ppm = "P6\n2 2\n255\n" + std::string(12, 'y');
        write_file(verified.image_root / "cam0/a.ppm", changed_ppm);
        check(fails([&] {
                  agent::VerifyReconstructionInputs(bundle_root, staged.files,
                                                    staged.identity_sha256, kBudget);
              }, "integrity mismatch"),
              "receiver verification rejects altered input bytes", failures);
        write_file(verified.image_root / "cam0/a.ppm", ppm);
        const fs::path worker_workspace = root / "worker-workspace";
        fs::create_directories(worker_workspace);
        const sfm::AutoRequest worker_request = agent::DecodeReconstructionRequest(
            verified, worker_workspace, kBudget);
        check(worker_request.cfg.quality == request.cfg.quality &&
                  worker_request.cfg.max_error == request.cfg.max_error &&
                  worker_request.cfg.threads == request.cfg.threads &&
                  worker_request.cfg.pairs == request.cfg.pairs &&
                  worker_request.cfg.sift.max_num_features ==
                      request.cfg.sift.max_num_features &&
                  worker_request.cfg.camera_mode_pinned &&
                  worker_request.in.explicit_flags == request.in.explicit_flags &&
                  worker_request.in.preset_changes.size() ==
                      request.in.preset_changes.size(),
              "typed worker request preserves the finalized SfM options and flags",
              failures);
        check(same_path(worker_request.in.image_dir, verified.image_root) &&
                  same_path(worker_request.in.workspace, worker_workspace) &&
                  same_path(worker_request.cfg.mask_dir, verified.mask_root) &&
                  worker_request.cfg.telemetry_inputs.size() == 1 &&
                  same_path(worker_request.cfg.telemetry_inputs[0].path,
                            bundle_root / "telemetry/00000000.bin"),
              "worker request paths resolve only inside its staged inputs and workspace",
              failures);

        const fs::path sparse = root / "sparse";
        write_model(sparse, make_model());
        const std::vector<agent::TransferFile> output =
            agent::ReconstructionOutputManifest(verified, sparse, kBudget);
        check(output.size() == 3,
              "output manifest contains only the complete sparse model files", failures);
        agent::VerifyReconstructionOutputs(verified, sparse, output, kBudget);

        sfm::Reconstruction broken = make_model();
        broken.points3D.at(10).track.pop_back();
        const fs::path invalid_sparse = root / "invalid-sparse";
        write_model(invalid_sparse, broken);
        check(fails([&] {
                  agent::ReconstructionOutputManifest(verified, invalid_sparse, kBudget);
              }, "invalid point track"),
              "output verification rejects a present but unloadable sparse model", failures);

        write_file(sparse / "0/unapproved.txt", "not in output manifest");
        check(fails([&] {
                  agent::VerifyReconstructionOutputs(verified, sparse, output, kBudget);
              }, "unauthorized file"),
              "receiver rejects output files outside the exact output manifest", failures);

        const fs::path outside = root / "outside.bin";
        write_file(outside, "must not be staged");
        sfm::Manifest external_manifest;
        external_manifest.image_dir = "images";
        sfm::ManifestCapture external_capture;
        external_capture.telemetry = outside.generic_u8string();
        external_manifest.captures.push_back(external_capture);
        const fs::path bad_manifest = source / "external.json";
        write_file(bad_manifest, sfm::manifest_write(external_manifest, true));
        const sfm::AutoRequest external_request =
            make_request(image_root, bad_manifest, workspace);
        const fs::path rejected_root = root / "rejected-input";
        fs::create_directories(rejected_root);
        check(fails([&] {
                  agent::StageReconstructionInputs(external_request, bad_manifest,
                                                  rejected_root, kBudget);
              }, "must be relative"),
              "absolute manifest telemetry paths are rejected instead of copied", failures);
        check(fs::is_empty(rejected_root) && read_file(outside) == "must not be staged",
              "rejected external inputs leave source and attempt trees untouched", failures);

        sfm::AutoRequest device_request;
        const std::string device_parse_error = sfm::parse_auto_args(
            {image_root.u8string(), "-o", workspace.u8string(), "--manifest",
             manifest.u8string(), "--device", "0"}, device_request, false);
        check(device_parse_error.empty(), "source-device request parses", failures);
        if (device_parse_error.empty()) {
            const fs::path device_root = root / "device-rejected-input";
            fs::create_directories(device_root);
            check(fails([&] {
                      agent::StageReconstructionInputs(device_request, manifest,
                                                      device_root, kBudget);
                  }, "explicit source-device"),
                  "unsupported source-device overrides fail explicitly", failures);
            check(fs::is_empty(device_root),
                  "unsupported config is rejected before staging files", failures);
        }
    } catch (...) {
        std::error_code ec;
        fs::remove_all(root, ec);
        throw;
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    if (failures == 0) std::printf("agent_reconstruction_job_test: OK\n");
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) { return sfmTestMain(argc, argv, run_test); }
