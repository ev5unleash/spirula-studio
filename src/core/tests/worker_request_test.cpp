// worker_request_test -- schema and output-boundary checks for worker requests.

#include "app/WorkerRequest.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition) {
        std::printf("ok   %s\n", message);
    } else {
        std::printf("FAIL %s\n", message);
        ++failures;
    }
}

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write worker request fixture");
    out << text;
}

std::string replace_once(std::string text, const std::string& from,
                         const std::string& to) {
    const size_t pos = text.find(from);
    if (pos == std::string::npos) throw std::runtime_error("fixture replacement failed");
    return text.replace(pos, from.size(), to);
}

bool accepts(const fs::path& path) {
    try {
        const app::worker::Request request =
            app::worker::parse_request(path.u8string());
        app::worker::validate_request(request);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool rejects(const fs::path& path, const std::string& text) {
    write_text(path, text);
    return !accepts(path);
}

}  // namespace

int main() {
    const std::string job_id = "job-0123456789abcdef";
    const std::string attempt_id = "att-0123456789abcdef";
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("spirula_worker_request_" + std::to_string(nonce));
    const fs::path request_path = root / ("request-" + attempt_id + ".json");
    const fs::path result_path = root / ("result-" + attempt_id + ".json");
    const std::string result_json_path = result_path.generic_u8string();
    const std::string valid =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"job_id\": \"" + job_id + "\",\n"
        "  \"attempt_id\": \"" + attempt_id + "\",\n"
        "  \"phase\": \"geometry\",\n"
        "  \"device\": \"uuid:0123456789abcdef0123456789abcdef\",\n"
        "  \"work_dir\": \"\",\n"
        "  \"result_path\": \"" + result_json_path + "\",\n"
        "  \"args\": [\"--check\", \"quoted value\"]\n"
        "}\n";

    try {
        fs::create_directories(root);
        write_text(request_path, valid);
        const app::worker::Request request =
            app::worker::parse_request(request_path.u8string());
        bool valid_request = true;
        try {
            app::worker::validate_request(request);
        } catch (const std::exception&) {
            valid_request = false;
        }
        check(valid_request, "accepts a valid schema-1 request");
        check(request.request_path == request_path.u8string(),
              "retains the request source path");

        check(rejects(request_path,
                       replace_once(valid, "\"schema_version\": 1",
                                    "\"schema_version\": \"1\"")),
              "rejects a non-numeric schema_version");
        check(rejects(request_path,
                       replace_once(valid,
                                    "\"device\": \"uuid:0123456789abcdef0123456789abcdef\"",
                                    "\"device\": 7")),
              "rejects a non-string device");
        check(rejects(request_path,
                       replace_once(valid, "\"work_dir\": \"\"",
                                    "\"work_dir\": false")),
              "rejects a non-string work_dir");
        check(rejects(request_path,
                       replace_once(valid, "  \"work_dir\": \"\",\n", "")),
              "rejects a missing work_dir");
        check(rejects(request_path,
                       replace_once(valid, "\"work_dir\": \"\"",
                                    "\"work_dir\": \"\\u0000\"")),
              "rejects an embedded NUL in a path");
        check(rejects(request_path,
                       replace_once(valid, "\"args\": [\"--check\", \"quoted value\"]",
                                    "\"args\": [3]")),
              "rejects a non-string args element");
        check(rejects(request_path,
                       replace_once(valid, "\"phase\": \"geometry\"",
                                    "\"phase\": \"shell\"")),
              "rejects an unsupported phase");
        check(rejects(request_path,
                       replace_once(valid, "\"job_id\": \"" + job_id + "\"",
                                    "\"job_id\": \"job-bad\"")),
              "rejects an unsafe job id");
        check(rejects(request_path,
                       replace_once(valid,
                                    "\"result_path\": \"" + result_json_path + "\"",
                                    "\"result_path\": \"" +
                                        (root / "result-other.json").generic_u8string() + "\"")),
              "rejects a result filename that does not match the attempt");
        check(rejects(request_path,
                       replace_once(valid,
                                    "\"result_path\": \"" + result_json_path + "\"",
                                    "\"result_path\": \"" +
                                        (root / ".." / "outside" /
                                         ("result-" + attempt_id + ".json")).generic_u8string() + "\"")),
              "rejects a result directory outside the request directory");
        check(rejects(request_path,
                       replace_once(valid,
                                    "\"device\": \"uuid:0123456789abcdef0123456789abcdef\"",
                                    "\"device\": \"uuid:0123456789abcdef0123456789abc\\u0000\"")),
              "rejects an embedded NUL in the device");
        check(rejects(request_path,
                       replace_once(valid, "\"quoted value\"",
                                    "\"bad\\u0000arg\"")),
              "rejects an embedded NUL in args");
        app::PrepJob prep;
        prep.workspace = "workspace";
        app::PrepInput input;
        input.path = "input/photos";
        input.is_video = true;
        input.subdir = "cam0";
        input.mask_dir = "input/masks";
        input.camera_model = "OPENCV_FISHEYE";
        input.focal_factor = 0.73f;
        input.rig = app::kRigFirstShared;
        input.video_tracks = 2;
        input.subcameras.push_back(
            {"cam1", "PINHOLE", 0.61f, app::kRigOwn});
        input.eac360 = {4096, 1344, 1344, 32};
        input.stencil.detect_border = true;
        input.stencil.shrink = 0.03f;
        input.stencil.mask.image = "input/stencil.png";
        input.stencil.mask.shapes.push_back(
            {app::MaskShape::Kind::Rect, true, 0.1f, 0.2f, 0.8f, 0.9f});
        prep.inputs.push_back(input);
        prep.resume = false;
        prep.redo_frames = true;
        prep.redo_masks = true;
        prep.flip_found_masks = true;
        prep.photo_import = app::PhotoImport::Move;
        prep.device = "uuid:0123456789abcdef0123456789abcdef";
        prep.pano = {app::Pano360Mode::Faces, 768, 12.0f, -8.0f, 180.0f};
        prep.video_fps = 1.5f;
        prep.sharp_window = 5;
        prep.sync_tracks = false;
        prep.max_frames = 321;
        prep.auto_rotate = false;
        prep.force_external_decode = true;
        prep.ffmpeg_exe = "tools/ffmpeg.exe";
        prep.image_gamut = "rec2020";
        prep.image_is_linear = true;
        prep.mask_enable = true;
        prep.mask_prompt = "people; cars";
        prep.mask_negative_prompt = "statues";
        prep.mask_keep_subject = true;
        prep.mask_dilate_ratio = 0.12f;
        prep.mask_max_image_size = 1024;
        prep.mask_threshold = 0.67f;
        prep.mask_nms = 0.24f;
        prep.mask_memory = true;
        prep.mask_detect_every = 4;
        prep.mask_memory_frames = 18;
        prep.mask_clicks.push_back(
            {123.5f, 456.25f, false, 3, 77, 0.45f,
             "input/photos", "cam1"});
        prep.mask_model_path = "models/sam.safetensors";
        prep.mask_model_name = "sam-test";
        prep.force_external_masking = true;
        prep.python_exe = "python-test";
        std::string freeze_error;
        check(app::worker::freeze_prep_job(prep, root.u8string(), freeze_error),
              "freezes prep inputs and workspace to absolute paths");
        const std::string payload = app::worker::serialize_prep_job(prep);
        const app::PrepJob decoded = app::worker::deserialize_prep_job(payload);
        const app::PrepInput& got_input = decoded.inputs.front();
        const app::PrepInput& want_input = prep.inputs.front();
        const app::SubCamera& want_sub = want_input.subcameras.front();
        const app::MaskShape& want_shape = want_input.stencil.mask.shapes.front();
        const app::MaskClick& want_click = prep.mask_clicks.front();
        const app::SubCamera* got_sub = got_input.subcameras.size() == 1
                                            ? &got_input.subcameras.front()
                                            : nullptr;
        const app::MaskShape* got_shape =
            got_input.stencil.mask.shapes.size() == 1
                ? &got_input.stencil.mask.shapes.front()
                : nullptr;
        const app::MaskClick* got_click = decoded.mask_clicks.size() == 1
                                              ? &decoded.mask_clicks.front()
                                              : nullptr;
        check(decoded.inputs.size() == 1 && decoded.workspace == prep.workspace &&
                  decoded.resume == prep.resume &&
                  decoded.redo_frames == prep.redo_frames &&
                  decoded.redo_masks == prep.redo_masks &&
                  decoded.flip_found_masks == prep.flip_found_masks &&
                  decoded.photo_import == prep.photo_import &&
                  decoded.device == prep.device &&
                  decoded.pano.mode == prep.pano.mode &&
                  decoded.pano.size == prep.pano.size &&
                  decoded.pano.yaw == prep.pano.yaw &&
                  decoded.pano.pitch == prep.pano.pitch &&
                  decoded.pano.roll == prep.pano.roll &&
                  decoded.video_fps == prep.video_fps &&
                  decoded.sharp_window == prep.sharp_window &&
                  decoded.sync_tracks == prep.sync_tracks &&
                  decoded.max_frames == prep.max_frames &&
                  decoded.auto_rotate == prep.auto_rotate &&
                  decoded.force_external_decode == prep.force_external_decode &&
                  decoded.ffmpeg_exe == prep.ffmpeg_exe &&
                  decoded.image_gamut == prep.image_gamut &&
                  decoded.image_is_linear == prep.image_is_linear &&
                  decoded.mask_enable == prep.mask_enable &&
                  decoded.mask_prompt == prep.mask_prompt &&
                  decoded.mask_negative_prompt == prep.mask_negative_prompt &&
                  decoded.mask_keep_subject == prep.mask_keep_subject &&
                  decoded.mask_dilate_ratio == prep.mask_dilate_ratio &&
                  decoded.mask_max_image_size == prep.mask_max_image_size &&
                  decoded.mask_threshold == prep.mask_threshold &&
                  decoded.mask_nms == prep.mask_nms &&
                  decoded.mask_memory == prep.mask_memory &&
                  decoded.mask_detect_every == prep.mask_detect_every &&
                  decoded.mask_memory_frames == prep.mask_memory_frames &&
                  decoded.mask_model_path == prep.mask_model_path &&
                  decoded.mask_model_name == prep.mask_model_name &&
                  decoded.force_external_masking ==
                      prep.force_external_masking &&
                  decoded.python_exe == prep.python_exe &&
                  got_input.path == want_input.path &&
                  got_input.is_video == want_input.is_video &&
                  got_input.subdir == want_input.subdir &&
                  got_input.mask_dir == want_input.mask_dir &&
                  got_input.camera_model == want_input.camera_model &&
                  got_input.focal_factor == want_input.focal_factor &&
                  got_input.rig == want_input.rig &&
                  got_input.video_tracks == want_input.video_tracks &&
                  got_input.eac360.track_w == want_input.eac360.track_w &&
                  got_input.eac360.track_h == want_input.eac360.track_h &&
                  got_input.eac360.face == want_input.eac360.face &&
                  got_input.eac360.strip == want_input.eac360.strip &&
                  got_input.stencil.detect_border ==
                      want_input.stencil.detect_border &&
                  got_input.stencil.shrink == want_input.stencil.shrink &&
                  got_input.stencil.mask.image ==
                      want_input.stencil.mask.image &&
                  got_sub && got_sub->rel == want_sub.rel &&
                  got_sub->camera_model == want_sub.camera_model &&
                  got_sub->focal_factor == want_sub.focal_factor &&
                  got_sub->rig == want_sub.rig &&
                  got_shape && got_shape->kind == want_shape.kind &&
                  got_shape->remove == want_shape.remove &&
                  got_shape->cx == want_shape.cx &&
                  got_shape->cy == want_shape.cy &&
                  got_shape->rx == want_shape.rx &&
                  got_shape->ry == want_shape.ry &&
                  got_click && got_click->x == want_click.x &&
                  got_click->y == want_click.y &&
                  got_click->positive == want_click.positive &&
                  got_click->object == want_click.object &&
                  got_click->frame == want_click.frame &&
                  got_click->position == want_click.position &&
                  got_click->source == want_click.source &&
                  got_click->camera == want_click.camera,
              "prep payload roundtrips every execution option");
        std::string masking_error;
        check(app::worker::reject_external_masking(decoded, masking_error) &&
                  masking_error.find("external Python") != std::string::npos,
              "scheduled prep rejects external Python masking");
    } catch (const std::exception& e) {
        std::printf("FAIL worker request test setup: %s\n", e.what());
        ++failures;
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    if (failures) {
        std::printf("FAILED with %d error(s)\n", failures);
        return 1;
    }
    std::printf("All worker request tests passed.\n");
    return 0;
}
