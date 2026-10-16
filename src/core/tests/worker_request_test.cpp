// worker_request_test -- schema and output-boundary checks for worker requests.

#include "app/WorkerRequest.h"
#include "app/B1Group.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
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
        const app::B1WorkerPayload block{0, 10003, fs::absolute(root / "source.ckpt").u8string(),
                                        fs::absolute(root / "views.txt").u8string()};
        const auto block_json = app::b1_worker_payload(block);
        const auto decoded_block = app::parse_b1_worker_payload(block_json);
        check(decoded_block.owner == 0 && decoded_block.target == 10003 &&
              decoded_block.source == block.source && decoded_block.views == block.views,
              "block worker retains fixed owner inputs and target");
        auto rejects_block = [](const std::string& text) {
            try { app::parse_b1_worker_payload(text); return false; }
            catch (const std::exception&) { return true; }
        };
        check(rejects_block(replace_once(block_json, "\"owner\": 0", "\"owner\": 2")),
              "block worker rejects all-owned or invalid owner");
        check(rejects_block(replace_once(block_json, "\"target\": 10003", "\"target\": 10003.5")),
              "block worker rejects fractional schedule boundary");
        check(rejects_block(replace_once(block_json, "\"kind\": \"b1\"", "\"env\": \"SS_B1_OWNER=1\"")),
              "block worker rejects arbitrary environment injection");
        check(rejects_block(replace_once(block_json, json_quote(block.source), "\"relative.ckpt\"")),
              "block worker rejects relative original source");
        check(rejects_block(replace_once(block_json, "\"owner\": 0", "\"owner\": 0, \"owner\": 1")),
              "block worker rejects duplicate identity fields");
        auto bound_block = block;
        bound_block.group = fs::absolute(root / "group.json").u8string();
        bound_block.group_sha256 = std::string(64, 'c');
        const auto bound_json = app::b1_worker_payload(bound_block);
        const auto bound_decoded = app::parse_b1_worker_payload(bound_json);
        check(bound_decoded.group == bound_block.group &&
              bound_decoded.group_sha256 == bound_block.group_sha256 &&
              decoded_block.group.empty(), "new workers bind group identity; historical payload has no policy");
        check(rejects_block(replace_once(bound_json, "\"version\": 2", "\"version\": 1")),
              "bound worker rejects unknown schema version");
        check(rejects_block(replace_once(bound_json, "\"version\": 2", "\"view_policy\": \"full\"")),
              "worker rejects independently supplied view policy");
        check(rejects_block(replace_once(bound_json, "\"group_sha256\"", "\"unknown\"")),
              "worker rejects missing or unknown group identity field");
        check(rejects_block(replace_once(bound_json, json_quote(bound_block.group), "\"group.json\"")),
              "worker rejects relative group path");
        check(rejects_block(replace_once(bound_json, json_quote(bound_block.group_sha256), "\"bad\"")),
              "worker rejects malformed group digest");
        check(rejects_block(replace_once(bound_json, "\"version\": 2", "\"version\": \"2\"")),
              "worker rejects nonnumeric bound schema version");
        auto live_block = bound_block;
        live_block.refresh_attempt = fs::absolute(root / "attempts" / "1").u8string();
        const auto live_json = app::b1_worker_payload(live_block);
        check(app::parse_b1_worker_payload(live_json).refresh_attempt == live_block.refresh_attempt,
              "live worker retains immutable exchange attempt");
        check(rejects_block(replace_once(live_json, json_quote(live_block.refresh_attempt), "\"relative\"")),
              "live worker refuses relative exchange path");
        check(rejects_block(replace_once(live_json, "\"version\": 3", "\"version\": 2")),
              "frozen worker refuses live exchange fields");
        check(rejects_block(replace_once(bound_json, "\"version\": 2", "\"version\": 3")),
              "live worker requires exchange attempt");
        const std::string group_json =
            "{\"version\":2,\"source\":\"source\",\"views\":\"views\","
            "\"source_sha256\":\"a\",\"views_sha256\":\"b\",\"config_sha256\":\"c\","
            "\"shared_sha256\":\"d\",\"source_step\":10000,\"target\":10003,"
            "\"device0\":\"a\",\"device1\":\"b\",\"view_policy\":\"full\"}";
        auto rejects_group = [](const std::string& text) {
            try { app::b1_group_view_policy(json_parse(text)); return false; }
            catch (const std::exception&) { return true; }
        };
        check(app::b1_group_view_policy(json_parse(group_json)) == "full",
              "new group explicitly selects full policy");
        check(app::b1_group_view_policy(json_parse(replace_once(group_json,
              "\"view_policy\":\"full\"", "\"view_policy\":\"partitioned\""))) == "partitioned",
              "new group explicitly selects partitioned policy");
        const auto historical_group = replace_once(replace_once(group_json,
              "\"version\":2", "\"version\":1"), ",\"view_policy\":\"full\"", "");
        check(app::b1_group_view_policy(json_parse(historical_group)) == "partitioned",
              "historical group retains partitioned semantics");
        check(rejects_group(replace_once(group_json, ",\"view_policy\":\"full\"", "")),
              "new group requires policy");
        check(rejects_group(replace_once(group_json, "\"full\"", "\"automatic\"")),
              "group rejects unknown policy");
        check(rejects_group(replace_once(group_json, "\"version\":2", "\"version\":3")),
              "live group requires cadence");
        const auto live_group = replace_once(replace_once(group_json, "\"version\":2", "\"version\":3"),
            "\"view_policy\":\"full\"", "\"view_policy\":\"full\",\"refresh_cadence\":3");
        check(app::b1_group_view_policy(json_parse(live_group)) == "full",
              "live group accepts full view immutable cadence");
        for (const auto* value : {"0", "-1", "1.5", "\"3\""}) {
            check(rejects_group(replace_once(live_group, "\"refresh_cadence\":3",
                std::string("\"refresh_cadence\":") + value)), "live group rejects invalid cadence");
        }
        check(rejects_group(replace_once(live_group, "\"full\"", "\"partitioned\"")),
              "live group refuses partitioned views");
        check(rejects_group(replace_once(live_group, "\"version\":3", "\"version\":2")),
              "frozen group refuses refresh contract fields");
        const auto commit_path = root / "generation.json";
        auto read_file = [](const fs::path& path) {
            std::ifstream input(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), {});
        };
        std::ofstream(fs::path(commit_path.u8string() + ".pending")) << "interrupted old attempt";
        app::b1_publish_record(commit_path, "new committed pair", root / "new-attempt.pending");
        check(read_file(commit_path) == "new committed pair",
              "new attempt recovers same boundary despite stale unpublished global pending");
        bool refused_replacement = false;
        try { app::b1_publish_record(commit_path, "replacement", root / "replacement.pending"); }
        catch (const std::exception&) { refused_replacement = true; }
        check(refused_replacement && read_file(commit_path) == "new committed pair",
              "commit publication refuses replacing an existing coherent pair");
        const auto parent_digest = std::string(64, 'a');
        for (int side = 0; side < 2; ++side) {
            bool refused_parent = false;
            try { app::validate_b1_refresh_parent(side == 0 ? std::string(64, 'b') : parent_digest,
                side == 1 ? std::string(64, 'b') : parent_digest, parent_digest); }
            catch (const std::exception&) { refused_parent = true; }
            check(refused_parent, "same-step foreign parent commit is refused");
        }
        check(rejects_group(replace_once(group_json, "\"version\":2", "\"version\":\"2\"")),
              "group rejects nonnumeric version");
        check(rejects_group(replace_once(group_json, "\"views_sha256\"", "\"unknown\"")),
              "group rejects unknown field even at expected field count");
        check(rejects_group(replace_once(group_json, "\"version\":2", "\"version\":1")),
              "historical group cannot smuggle a policy override");
        check(rejects_group(replace_once(group_json, "\"view_policy\":\"full\"",
              "\"view_policy\":\"full\",\"view_policy\":\"partitioned\"")),
              "group rejects duplicate policies");
        ckpt::B1Contract left;
        left.owner = 0; left.source_step = 10000; left.completed_step = 10003;
        left.horizon = 30000; left.rows = 257; left.frozen_context = std::string(64, '1');
        for (auto* digest : {&left.source_state_sha256, &left.config_sha256, &left.views_sha256,
                            &left.train_indices_sha256, &left.full_train_indices_sha256,
                            &left.mask_sha256, &left.frozen_shared_sha256,
                            &left.blocks_sha256, &left.owned_ids_sha256, &left.excluded_ids_sha256,
                            &left.ply_sha256})
            *digest = std::string(64, 'a');
        auto right = left; right.owner = 1;
        auto accepts_completion = [&](const ckpt::B1Contract& a, const ckpt::B1Contract& b,
                                      int64_t target) {
            try { app::validate_b1_group_completion(a, b, target); return true; }
            catch (const std::exception&) { return false; }
        };
        check(accepts_completion(left, right, 10003), "complementary target completion accepted");
        check(!accepts_completion(left, right, 10004), "equal but incomplete owner progress rejected");
        check(!accepts_completion(left, left, 10003), "duplicate owner completion rejected");
        right.view_policy = "full";
        check(!accepts_completion(left, right, 10003), "mixed view policies never complete a group");
        right.view_policy = "partitioned";
        right.completed_step = 10002;
        check(!accepts_completion(left, right, 10003), "unequal owner progress never completes group");
        right.completed_step = 10003; right.source_state_sha256 = std::string(64, 'b');
        check(!accepts_completion(left, right, 10003), "stale source completion rejected");
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
        input.pano360.packing = app::Pano360Packing::Sphere;
        input.pano360.track_w = 4096;
        input.pano360.track_h = 1344;
        input.pano360.face = 1344;
        input.pano360.strip = 32;
        input.pano360.margin = 24;
        input.pano360_unsupported = true;
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
        prep.adaptive_fps = true;
        prep.adaptive_range = 8.0f;
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
                  decoded.adaptive_fps == prep.adaptive_fps &&
                  decoded.adaptive_range == prep.adaptive_range &&
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
                  got_input.pano360.packing == want_input.pano360.packing &&
                  got_input.pano360.track_w == want_input.pano360.track_w &&
                  got_input.pano360.track_h == want_input.pano360.track_h &&
                  got_input.pano360.face == want_input.pano360.face &&
                  got_input.pano360.strip == want_input.pano360.strip &&
                  got_input.pano360.margin == want_input.pano360.margin &&
                  got_input.pano360_unsupported ==
                      want_input.pano360_unsupported &&
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
        std::string legacy_payload = payload;
        legacy_payload = replace_once(legacy_payload, "\"pano360\":", "\"eac360\":");
        legacy_payload = replace_once(legacy_payload, "\"packing\":1,", "");
        legacy_payload = replace_once(legacy_payload, ",\"margin\":24", "");
        legacy_payload =
            replace_once(legacy_payload, ",\"pano360_unsupported\":true", "");
        const app::PrepJob legacy =
            app::worker::deserialize_prep_job(legacy_payload);
        const app::PrepInput& legacy_input = legacy.inputs.front();
        check(legacy_input.pano360.face == want_input.pano360.face &&
                  legacy_input.pano360.packing == app::Pano360Packing::Eac &&
                  legacy_input.pano360.margin == 0 &&
                  !legacy_input.pano360_unsupported,
              "prep payload reads old eac360 defaults");
        std::string masking_error;
        check(app::worker::reject_external_masking(decoded, masking_error) &&
                  masking_error.find("external Python") != std::string::npos,
              "scheduled prep rejects external Python masking");

        app::worker::Result published;
        published.outcome = "partial";
        published.exit_code = 3;
        published.outputs = {(root / "quoted output").u8string()};
        published.registered = 4;
        published.images = 6;
        check(app::worker::publish_result(request, published),
              "publishes a completed result");
        const auto result = app::worker::parse_result(result_path.u8string());
        check(result.job_id == job_id && result.attempt_id == attempt_id &&
                  result.phase == request.phase && result.outcome == "partial" &&
                  result.exit_code == 3 && result.outputs == published.outputs &&
                  result.registered == 4 && result.images == 6,
              "published partial result retains identity and usable output metadata");
        std::ifstream result_input(result_path, std::ios::binary);
        const std::string result_json(
            (std::istreambuf_iterator<char>(result_input)), {});
        result_input.close();
        auto rejects_result = [&](const std::string& text) {
            write_text(result_path, text);
            try {
                app::worker::parse_result(result_path.u8string());
                return false;
            } catch (const std::exception&) {
                return true;
            }
        };
        check(rejects_result(replace_once(result_json, "\"partial\"", "\"unknown\"")),
              "rejects an unknown result outcome");
        check(rejects_result(replace_once(result_json, "\"schema_version\": 2",
                                                       "\"schema_version\": 3")),
              "rejects an unsupported result schema");
        check(rejects_result(replace_once(result_json, "\"registered\": 4",
                                                       "\"registered\": 4.5")),
              "rejects fractional reconstruction counts");
        check(rejects_result(result_json.substr(0, result_json.size() / 2)),
              "rejects a truncated result instead of accepting partial publication");
        app::worker::Request unavailable = request;
        unavailable.result_path = (root / "missing" / "result.json").u8string();
        check(!app::worker::publish_result(unavailable, published) &&
                  !fs::exists(fs::u8path(unavailable.result_path)),
              "publication failure does not create a successful result");
    } catch (const std::exception& e) {
        std::printf("FAIL worker request test setup: %s\n", e.what());
        ++failures;
    }

    std::error_code ec;
    if (!failures) fs::remove_all(root, ec);
    if (failures) {
        std::printf("FAILED with %d error(s)\n", failures);
        return 1;
    }
    std::printf("All worker request tests passed.\n");
    return 0;
}
