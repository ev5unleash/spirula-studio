#include "app/AgentTrainingJob.h"
#include "app/tests/SceneFixture.h"
#include "config/TrainConfigJson.h"
#include "core/CheckpointIO.h"
#include "data/Json.h"
#include "data/JsonWrite.h"
#include "checkpoint/Resume.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <iterator>
#include <vector>

namespace fs = std::filesystem;
using namespace app::agent;

namespace {

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

template <typename F>
void require_rejected(F&& action, const char* message) {
    try {
        action();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

struct TemporaryDirectory {
    fs::path path;
    ~TemporaryDirectory() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string config_json(const TrainConfig& config) {
    JsonWriter writer;
    writer.object().field("preset", "3dgs");
    for (const auto& [key, value] : train_config_json_pairs(config))
        writer.field_raw(key, value);
    return writer.end().str();
}

void write_text(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << value;
    if (!out) throw std::runtime_error("test fixture write failed");
}

void write_npy(std::ostream& tar, const std::string& name, std::size_t count) {
    std::string bytes = ckpt::npy_header("<f4", count);
    bytes.append(count * sizeof(float), '\0');
    ckpt::tar_write_bytes(tar, name + ".npy", bytes.data(), bytes.size());
}

void write_resumable_state(const fs::path& path, std::uint64_t step) {
    std::ofstream tar(path, std::ios::binary | std::ios::trunc);
    if (!tar) throw std::runtime_error("cannot create test checkpoint archive");
    const std::string state = "{\"format_version\":1,\"step\":" +
        std::to_string(step) +
        ",\"full_resume\":1,\"max_num_splats\":1,"
        "\"cur_num_splats\":1,\"num_sh\":0}";
    ckpt::tar_write_bytes(tar, "state.json", state.data(), state.size());
    for (const auto& [name, count] : {
             std::pair{"world.means", 3u}, std::pair{"world.quats", 4u},
             std::pair{"world.scales", 3u}, std::pair{"world.opacities", 1u},
             std::pair{"world.features_dc", 3u}, std::pair{"eng.radii", 1u},
             std::pair{"eng.accum_buffer", 2u},
             std::pair{"eng.g1_means", 3u}, std::pair{"eng.g2_means", 3u},
             std::pair{"eng.g1_quats", 4u}, std::pair{"eng.g2_quats", 4u},
             std::pair{"eng.g1_scales", 3u}, std::pair{"eng.g2_scales", 3u},
             std::pair{"eng.g1_opacities", 1u}, std::pair{"eng.g2_opacities", 1u},
             std::pair{"eng.g1_features_dc", 3u},
             std::pair{"eng.g2_features_dc", 3u}})
        write_npy(tar, name, count);
    ckpt::tar_finish(tar);
}

void write_splat(const fs::path& path) {
    write_text(path,
        "ply\nformat ascii 1.0\nelement vertex 1\n"
        "property float x\nproperty float y\nproperty float z\n"
        "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n"
        "property float opacity\nproperty float scale_0\nproperty float scale_1\n"
        "property float scale_2\nproperty float rot_0\nproperty float rot_1\n"
        "property float rot_2\nproperty float rot_3\nend_header\n"
        "0 0 0 0 0 0 0 -2 -2 -2 1 0 0 0\n");
}

void create_worker_output(const TrainingInputBundle& inputs,
                          const std::string& checkpoint_name) {
    const fs::path checkpoint = inputs.output_root / checkpoint_name;
    fs::create_directories(checkpoint);
    write_text(inputs.output_root / "config.json", config_json(inputs.config));
    write_text(inputs.output_root / "scene_transform.json", "{}\n");
    write_text(inputs.output_root / "metrics.json", "{\"avg_psnr\": 1.0}\n");
    write_text(checkpoint / "config.json", config_json(inputs.config));
    write_splat(checkpoint / "splat.ply");
    write_resumable_state(checkpoint / "state.tar", 3);
}

}  // namespace

int main() {
    try {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        TemporaryDirectory temporary{fs::temp_directory_path() /
            ("spirula-agent-training-job-" + std::to_string(nonce))};
        const fs::path dataset = temporary.path / "source-dataset";
        const fs::path workspace = temporary.path / "attempt";
        fs::create_directories(workspace);
        test::write_scene(dataset);
        write_text(dataset / "cache" / "unreferenced.log", "not a train input");

        TrainConfig config;
        config.data = dataset.u8string();
        config.data_format = "nerfstudio";
        config.save_full_checkpoint = true;
        config.disable_viewer = true;
        config.eval_mode = "all";

        constexpr std::uint64_t budget = 64u << 20;
        const fs::path colmap_dataset = temporary.path / "bad-colmap";
        fs::create_directories(colmap_dataset / "images");
        write_text(colmap_dataset / "sparse/0/cameras.txt",
                   "1 SIMPLE_PINHOLE 64 64 50 32 32\n");
        write_text(colmap_dataset / "sparse/0/images.txt",
                   "1 1 0 0 0 0 0 0 1 ../outside.png\n\n");
        write_text(colmap_dataset / "sparse/0/points3D.txt", "");
        TrainConfig unsafe_colmap = config;
        unsafe_colmap.data = colmap_dataset.u8string();
        unsafe_colmap.data_format = "colmap";
        unsafe_colmap.colmap_recon_dir = "sparse/0";
        unsafe_colmap.image_dir = "images";
        const fs::path unsafe_workspace = temporary.path / "unsafe-attempt";
        fs::create_directories(unsafe_workspace);
        require_rejected([&] {
            StageTrainingInputs(unsafe_colmap, "3dgs", {}, unsafe_workspace, budget);
        }, "COLMAP image path traversal must be rejected before parsing");

        const TrainingInputBundle staged = StageTrainingInputs(
            config, "3dgs", {}, workspace, budget);
        require(!staged.identity_sha256.empty(), "staging computes input identity");
        require(staged.config.data == "input/dataset" &&
                    staged.config.output_dir_prefix == "output" &&
                    staged.config.output_dir_name == "run",
                "staged config uses only worker-relative paths");
        bool found_image = false;
        for (const TransferFile& file : staged.files)
            found_image = found_image || file.path == "dataset/images/view0.png";
        require(found_image, "manifest includes frozen training images");
        require(!fs::exists(staged.dataset_root / "cache" / "unreferenced.log"),
                "unreferenced dataset files are excluded from the package");

        const TrainingInputBundle verified = VerifyTrainingInputs(
            staged.root, staged.files, staged.identity_sha256, budget);
        const TrainingInvocationOptions invocation = MakeTrainingInvocation(verified);
        require(invocation.mode == TrainingMode::Fresh &&
                    invocation.working_directory == workspace &&
                    invocation.output_directory == workspace / "output" / "run" &&
                    invocation.config.data == "input/dataset",
                "worker receives frozen typed train options");
        TrainingInputBundle altered = verified;
        ++altered.config.num_iterations;
        require_rejected([&] {
            (void)MakeTrainingInvocation(altered);
        }, "typed config must remain bound to the frozen input identity");
        require_rejected([&] {
            VerifyTrainingInputs(staged.root, staged.files, std::string(64, '0'), budget);
        }, "wrong input identity must be rejected");

        const std::string checkpoint_name = "step-000000003.ckpt";
        create_worker_output(verified, checkpoint_name);
        const TrainingOutputPackage package = TrainingOutputManifest(
            verified, verified.output_root, checkpoint_name, budget);
        const TrainingOutputBundle output = VerifyTrainingOutputs(
            verified, verified.output_root, package, budget);
        require(output.returned_checkpoint == checkpoint_name &&
                    output.checkpoint_dir == verified.output_root / checkpoint_name &&
                    output.resume_config.resume == output.checkpoint_dir.u8string() &&
                    output.resume_config.data == verified.dataset_root.u8string(),
                "returned full checkpoint has a valid local resume binding");
        const TrainConfig cli_resume =
            ckpt::build_resume_config(output.resume_config, "3dgs", {});
        require(cli_resume.resume == output.checkpoint_dir.u8string() &&
                    cli_resume.data == verified.dataset_root.u8string() &&
                    fs::is_directory(fs::u8path(cli_resume.data)),
                "rebound output config resolves through the native --resume path");
        require(output.worker_files.size() == package.files.size() &&
                    !output.files.empty(),
                "verified output preserves worker and locally rebound manifests");
        const JsonValue rebound = json_parse([&] {
            std::ifstream in(output.config_path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), {});
        }());
        require(rebound.find("data") &&
                    rebound.find("data")->as_string() == verified.dataset_root.u8string(),
                "returned config.json binds to the local frozen dataset");
        TrainConfig resume_config = verified.config;
        resume_config.data = verified.dataset_root.u8string();
        const fs::path resume_workspace = temporary.path / "resume-attempt";
        fs::create_directories(resume_workspace);
        const TrainingInputBundle resumed = StageTrainingInputs(
            resume_config, "3dgs", output.checkpoint_dir,
            resume_workspace, budget);
        const TrainingInputBundle resumed_verified = VerifyTrainingInputs(
            resumed.root, resumed.files, resumed.identity_sha256, budget);
        const TrainingInvocationOptions resume_invocation =
            MakeTrainingInvocation(resumed_verified);
        require(resumed_verified.mode == TrainingMode::Resume &&
                    resumed_verified.config.resume ==
                        "input/resume/" + checkpoint_name &&
                    resume_invocation.resume_checkpoint ==
                        resume_workspace / "input" / "resume" / checkpoint_name,
                "resumable checkpoint stages with explicit worker-local paths");

        write_text(verified.root / "unexpected.bin", "unapproved");
        require_rejected([&] {
            VerifyTrainingInputs(verified.root, verified.files,
                                verified.identity_sha256, budget);
        }, "unexpected staged files must be rejected");

        std::printf("agent training artifact contract: ok\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "agent_training_job_test: %s\n", error.what());
        return 1;
    }
}
