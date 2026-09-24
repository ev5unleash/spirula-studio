// dataset_prep_test -- the two places DatasetPrep (app/gui/DatasetPrep.h) meets
// the mask editor's layer folder: a re-run re-applies hand corrections over
// the masks it rewrites, and the camera scan never takes mask_edits/ for a
// camera. Real DatasetPrep::run, no model: the re-mask is the frame stencil.

#include "app/FrameMask.h"
#include "app/gui/DatasetPrep.h"
#include "app/gui/FrameSelect.h"
#include "app/gui/mask/MaskLayer.h"
#include "core/SourcePath.h"
#include "external/stb_image_write.h"
#include "i18n/catalog/MaskEdit.h"
#include "i18n/catalog/Log.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace mk = gui::mask;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_failures++;
}

fs::path scratch(const char* name) {
    const fs::path d = fs::temp_directory_path() / "spirula_dataset_prep_test" / name;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

void write_jpg(const fs::path& p, int w, int h, int seed) {
    std::vector<uint8_t> px((size_t)w * h * 3);
    for (size_t i = 0; i < px.size(); i++) px[i] = (uint8_t)((i * 7 + (size_t)seed * 31) & 255);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    stbi_write_jpg(p.string().c_str(), w, h, 3, px.data(), 90);
}

void write_png(const fs::path& p, int w, int h, uint8_t v) {
    std::vector<uint8_t> px((size_t)w * h, v);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    stbi_write_png(p.string().c_str(), w, h, 1, px.data(), w);
}

// One box of 255 on 0, [x0, x1) x [y0, y1).
std::vector<uint8_t> box(int w, int h, int x0, int y0, int x1, int y1) {
    std::vector<uint8_t> px((size_t)w * h, 0);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) px[(size_t)y * w + x] = 255;
    return px;
}

uint8_t at(const std::vector<uint8_t>& px, int w, int x, int y) { return px[(size_t)y * w + x]; }

bool run_prep(const gui::PrepJob& job, gui::RunProgress& prog, std::string& error) {
    std::atomic<bool> cancel{false};
    gui::DatasetPrep prep(&prog, gui::RunFilms{}, cancel);
    gui::PrepResult out;
    return prep.run(job, out, error);
}

bool logged(gui::RunProgress& prog, const std::string& line) {
    for (const gui::RunLine& l : prog.drain())
        if (l.text == line) return true;
    return false;
}

// The re-mask is DatasetPrep::run's stencil pass, which folds the stencil into
// the masks already there -- so it re-drops the band a hand "keep" had put
// back, and only the re-apply after it restores that keep.
void test_rerun_reapplies_corrections() {
    const int W = 64, H = 48;
    const fs::path root = scratch("rerun");
    const fs::path photos = root / "photos", ws = root / "dataset";
    for (int i = 0; i < 3; i++) write_jpg(photos / (std::string(1, (char)('a' + i)) + ".jpg"), W, H, i);
    gui::PrepJob job;
    job.workspace = ws.string();
    job.photo_import = gui::PhotoImport::InPlace;
    gui::PrepInput in;
    in.path = photos.string();
    std::string err;
    check(app::parse_mask_shapes("-rect 0,0.5,1,0.75", in.stencil.mask.shapes, err),
          "fixture: the stencil drops rows 24..35: " + err);
    job.inputs = {in};
    gui::RunProgress prog;
    check(run_prep(job, prog, err), "first run: " + err);
    const fs::path mask_a = ws / "masks" / "a.png";
    std::vector<uint8_t> run1;
    int w = 0, h = 0;
    check(app::load_stencil(mask_a.string(), w, h, run1) && w == W && h == H,
          "first run wrote masks/a.png at 64x48");
    if (run1.size() != (size_t)W * H) return;
    check(at(run1, W, 15, 30) == 0 && at(run1, W, 45, 8) == 255,
          "fixture: the stencil band is dropped, the top is kept");

    // What the editor's save writes: keep a box inside the band, drop one above it.
    const std::string layer_root = (ws / mk::kLayerDirName).string();
    const std::string mask_root = mk::normalize_dir((ws / "masks").string());
    const std::vector<uint8_t> keep = box(W, H, 10, 28, 20, 34);
    const std::vector<uint8_t> drop = box(W, H, 40, 5, 50, 12);
    mk::LayerIndex idx;
    idx.mask_root = mask_root;
    check(mk::save_frame(layer_root, mask_root, "a", W, H, run1.data(), drop.data(), keep.data(),
                         true, idx, err),
          "the correction saves: " + err);
    std::vector<uint8_t> saved;
    app::load_stencil(mask_a.string(), w, h, saved);
    check(at(saved, W, 15, 30) == 255 && at(saved, W, 45, 8) == 0,
          "fixture: the saved composite carries the keep and the drop");

    prog.drain();
    check(run_prep(job, prog, err), "second run: " + err);
    const std::string one = spirula::i18n::format(spirula::i18n::msg::maskedit::log_recomposited,
                                                  {1LL});
    check(logged(prog, one), "the second run logs one frame re-applied");
    std::vector<uint8_t> after;
    check(app::load_stencil(mask_a.string(), w, h, after) && after.size() == (size_t)W * H,
          "masks/a.png readable after the re-run");
    if (after.size() != (size_t)W * H) return;
    check(at(after, W, 15, 30) == 255,
          "re-run: the hand keep inside the stencil band survives the re-mask");
    // Not a discriminator: the stencil's fold is an intersection, so it keeps a drop by itself.
    check(at(after, W, 45, 8) == 0, "re-run: the hand drop is still dropped");
    check(at(after, W, 30, 30) == 0 && at(after, W, 5, 3) == 255,
          "re-run: pixels no correction covers are the stencil's");
    std::vector<uint8_t> b;
    app::load_stencil((ws / "masks" / "b.png").string(), w, h, b);
    check(b.size() == run1.size() && at(b, W, 15, 30) == 0,
          "re-run: a's keep is not applied to uncorrected frame b");
}

// A sibling holding the same PNGs under another name IS a camera, so only
// the name guard keeps mask_edits/ out of the list.
void test_camera_scan_skips_mask_edits() {
    const fs::path root = scratch("scan");
    write_jpg(root / "cam0" / "f0.jpg", 16, 12, 0);
    write_jpg(root / "cam1" / "f0.jpg", 16, 12, 1);
    for (const char* dir : {mk::kLayerDirName, "lookalike"})
        for (const char* f : {"cam0/f0.base.png", "cam0/f0.drop.png", "cam0/f0.keep.png"})
            write_png(root / dir / f, 16, 12, 255);
    const std::vector<std::string> cams = gui::camera_subfolders(root.string());
    std::string listed;
    for (const std::string& c : cams) listed += c + " ";
    check(std::find(cams.begin(), cams.end(), "lookalike/cam0") != cams.end(),
          "fixture: the layer PNGs under another name are taken for a camera: " + listed);
    bool edits = false;
    for (const std::string& c : cams) edits |= c.rfind(mk::kLayerDirName, 0) == 0;
    check(!edits, "camera scan: nothing under mask_edits/ is listed: " + listed);
    check(cams.size() == 3 && cams[0] == "cam0" && cams[1] == "cam1",
          "camera scan: cam0, cam1 and the lookalike, nothing else: " + listed);
    check(gui::is_mask_edits_folder((root / mk::kLayerDirName).string()) &&
              !gui::is_mask_edits_folder((root / "lookalike").string()),
          "is_mask_edits_folder: by name");
}

void test_pts_synchronized_selector() {
    const fs::path root = scratch("pts_sync");
    gui::FrameSelectGroup group;
    group.candidate_dirs = {(root / "cand0").string(),
                            (root / "cand1").string()};
    group.output_dirs = {(root / "out0").string(),
                         (root / "out1").string()};
    group.options.group = 2;
    group.sync_tracks = true;
    for (int track = 0; track < 2; track++)
        for (int frame = 0; frame < 4; frame++)
            write_jpg(root / ("cand" + std::to_string(track)) /
                          ("c_" + std::to_string(100 + frame * 100) + ".jpg"),
                      64, 48, track * 4 + frame);
    std::vector<gui::FrameSelectGroup> groups{group};
    std::atomic<bool> cancel{false};
    std::string error;
    check(gui::select_sharpest_frame_groups(groups, {}, cancel, error),
          "synchronized candidates select: " + error);
    auto names = [&](const char* dir) {
        std::vector<std::string> found;
        std::error_code ec;
        for (fs::directory_iterator it(root / dir, ec), end;
             !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec))
                found.push_back(it->path().filename().string());
        return found;
    };
    std::vector<std::string> left = names("out0");
    std::vector<std::string> right = names("out1");
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    check(left.size() == 2 && left == right,
          "both tracks publish the same two selected instants");

    const fs::path mismatch = scratch("pts_mismatch");
    gui::FrameSelectGroup bad;
    bad.candidate_dirs = {(mismatch / "cand0").string(),
                          (mismatch / "cand1").string()};
    bad.output_dirs = {(mismatch / "out0").string(),
                       (mismatch / "out1").string()};
    bad.sync_tracks = true;
    write_jpg(mismatch / "cand0" / "c_100.jpg", 64, 48, 1);
    write_jpg(mismatch / "cand0" / "c_200.jpg", 64, 48, 2);
    write_jpg(mismatch / "cand1" / "c_100.jpg", 64, 48, 3);
    write_jpg(mismatch / "cand1" / "c_300.jpg", 64, 48, 4);
    std::vector<gui::FrameSelectGroup> invalid{bad};
    check(!gui::select_sharpest_frame_groups(invalid, {}, cancel, error) &&
              !fs::exists(mismatch / "out0") &&
              !fs::exists(mismatch / "out1"),
          "unequal presentation timestamps are rejected before publication");
}

void test_adaptive_selector_tracks_frames_and_publishes_plan() {
    const fs::path root = scratch("adaptive_selector");
    gui::FrameSelectGroup group;
    group.candidate_dirs = {(root / "candidates").string()};
    group.output_dirs = {(root / "selected").string()};
    group.options.adaptive = true;
    group.options.group = 2;
    group.options.range = 4.0f;
    group.options.window = 1;
    group.options.max_frames = 3;
    group.options.view = app::MotionView::Fisheye;
    group.options.out_fov = 2.2f;
    size_t measured = 0;
    int64_t planned_frames = 0;
    std::vector<int64_t> plan;
    group.options.measured =
        [&](int64_t, int64_t, float) { measured++; };
    group.options.planned = [&](const std::vector<int64_t>& selected,
                                int64_t count) {
        plan = selected;
        planned_frames = count;
    };
    for (int frame = 0; frame < 8; frame++)
        write_jpg(root / "candidates" /
                      ("c_" + std::to_string(100 + frame * 100) + ".jpg"),
                  128, 96, frame);

    std::vector<gui::FrameSelectGroup> groups{group};
    std::atomic<bool> cancel{false};
    std::string error;
    check(gui::select_sharpest_frame_groups(groups, {}, cancel, error),
          "adaptive candidates select: " + error);
    check(measured == 7,
          "adaptive selection measures every adjacent frame transition");
    check(planned_frames == 8 && !plan.empty() && plan.size() <= 3 &&
              groups[0].kept == (int)plan.size(),
          "adaptive selection returns a bounded, nonempty motion plan");

    size_t published = 0;
    std::error_code ec;
    for (fs::directory_iterator it(root / "selected", ec), end;
         !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) published++;
    check(!ec && published > 0 && published == (size_t)groups[0].kept,
          "adaptive plan publishes exactly its selected frames");
}

void test_cancelled_selector_does_not_publish() {
    const fs::path root = scratch("cancelled_selector");
    gui::FrameSelectGroup group;
    group.candidate_dirs = {(root / "candidates").string()};
    group.output_dirs = {(root / "selected").string()};
    std::vector<gui::FrameSelectGroup> groups{group};
    std::atomic<bool> cancel{true};
    std::string error;
    check(!gui::select_sharpest_frame_groups(groups, {}, cancel, error) &&
              error == spirula::i18n::msg::log::err_cancelled.get() &&
              !fs::exists(root / "selected"),
          "pre-cancelled selection reports cancellation without publishing");
}

void test_missing_ffmpeg_fails_before_output() {
    const fs::path root = scratch("missing_ffmpeg");
    gui::PrepJob job;
    job.workspace = (root / "dataset").string();
    job.force_external_decode = true;
    job.ffmpeg_exe = (root / "missing-ffmpeg.exe").string();
    gui::PrepInput input;
    input.path = (root / "capture.mp4").string();
    input.is_video = true;
    job.inputs = {input};

    gui::RunProgress progress;
    std::string error;
    const bool ok = run_prep(job, progress, error);
    check(!ok && error.find(job.ffmpeg_exe) != std::string::npos,
          "missing FFmpeg failure includes the configured executable path");
    check(!fs::exists(root / "dataset") &&
              !fs::exists(root / "dataset" / "images") &&
              !fs::exists(root / "dataset" / gui::kFramesStampFile),
          "missing FFmpeg fails before creating workspace output");
}

}  // namespace

int main() {
    test_rerun_reapplies_corrections();
    test_camera_scan_skips_mask_edits();
    test_pts_synchronized_selector();
    test_adaptive_selector_tracks_frames_and_publishes_plan();
    test_cancelled_selector_does_not_publish();
    test_missing_ffmpeg_fails_before_output();
    std::printf("%s: %d failure(s)\n", SS_FILE, g_failures);
    return g_failures;
}
