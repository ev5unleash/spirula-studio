#pragma once

// DatasetPrep prepares image datasets shared by the built-in and COLMAP SfM
// paths: frame extraction, frame selection, track splitting, masking, and
// resumable workspace handling.
//
// Built-in video/masking backends fall back to ffmpeg/Python when unavailable.

#include "app/FrameLook.h"
#include "app/FrameMask.h"
#include "app/Pano360.h"

#include <algorithm>
#include <cstdint>
#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace app {

// A mask prompt for one input/camera. `frame` is exact where available;
// `position` preserves the corresponding point when ffmpeg resamples video.
// `source` and `camera` prevent prompts crossing inputs or camera folders.
struct MaskClick {
    float x = 0.0f, y = 0.0f;   // pixels of the frame as the run writes it
    bool  positive = true;      // "this is it" vs "not this"
    int   object = 0;
    long long frame = 0;
    float position = 0.0f;      // 0..1 through the capture
    // PrepInput::path of the input it was drawn on; empty is the only one. The
    // same coordinates on another capture point at something else, so a click
    // never crosses inputs.
    std::string source;
    // And which camera folder under it (app::frame_folders), for the same
    // reason: a click on cam0 says nothing about where cam1 was pointing.
    std::string camera;
};

// One camera folder found INSIDE an input: a capture handed over already split
// into cam/, cam0/, cam1/ is several cameras in one folder, and one of them
// being a fisheye does not make the others one.
struct SubCamera {
    std::string rel;                 // "cam0", relative to the input's images
    std::string camera_model;        // empty = the input's own
    float focal_factor = 0.0f;       // 0 = no focal prior
    int rig = 0;                     // see PrepInput::rig
};

// A row's rig choice: nothing, the lenses of its own input, or one of the
// shared letters that join rows across inputs (SfmRunner::build_rigs).
inline constexpr int kRigNone = 0, kRigOwn = 1, kRigFirstShared = 2, kRigShared = 4;

// One picked video or photo folder. Jobs keep inputs together so their images
// land in one reconstruction tree.
struct PrepInput {
    std::string path;                // video file, or a folder of photos
    bool is_video = false;
    // Destination under dataset images/. Empty means the dataset root; multi-track
    // captures add camera subfolders.
    std::string subdir;
    // Masks supplied with this input, mirroring its image tree. Supplied masks are
    // read-only inputs and are never overwritten by AI masking.
    std::string mask_dir;
    // Lens metadata carried to reconstruction; preparation itself does not read
    // these fields. Empty model/factor means use the job-wide defaults.
    std::string camera_model;
    float focal_factor = 0.0f;       // fx = fy = factor * image width
    // Which rig this input's lenses belong to (kRig*). A multi-lens video
    // starts on its own; folders sharing a letter form one rig by file name.
    int rig = kRigNone;
    int video_tracks = 0;            // 0 = not probed yet
    // The camera folders found under this input, when it arrived with more
    // than one. Empty means the lens above describes all of it.
    std::vector<SubCamera> subcameras;
    // The 360 packing this file was found to carry, when it carries one: two
    // EAC tracks that the job's `pano` plan turns into ordinary views. Detected
    // rather than asked for, so a capture that is not one cannot be warped.
    app::Eac360Layout eac360;
    // Per-input pixels that are never scene; resolved per camera folder when a
    // frame stencil is fitted.
    app::FrameStencil stencil;
};

// One row of the "Camera / lens per input" list. `rel` is the prefix
// `--camera-model PREFIX=MODEL` matches on, so the panel's rows and the
// reconstruction's overrides are one list (SfmRunner::append_camera_overrides).
struct CameraGroup {
    size_t input = 0;   // index into the job's inputs
    int sub = -1;       // index into that input's subcameras; -1 = the input
    std::string rel;    // under images/; "" is the whole capture
};

// The rows, in the order they are drawn and applied.
std::vector<CameraGroup> camera_groups(const std::vector<PrepInput>& inputs);

inline int& group_rig(std::vector<PrepInput>& in, const CameraGroup& g) {
    return g.sub < 0 ? in[g.input].rig : in[g.input].subcameras[(size_t)g.sub].rig;
}
inline int group_rig(const std::vector<PrepInput>& in, const CameraGroup& g) {
    return g.sub < 0 ? in[g.input].rig : in[g.input].subcameras[(size_t)g.sub].rig;
}

// Where a row's settings are stored.
inline std::string& group_model(std::vector<PrepInput>& in, const CameraGroup& g) {
    return g.sub < 0 ? in[g.input].camera_model
                     : in[g.input].subcameras[(size_t)g.sub].camera_model;
}
inline const std::string& group_model(const std::vector<PrepInput>& in,
                                      const CameraGroup& g) {
    return g.sub < 0 ? in[g.input].camera_model
                     : in[g.input].subcameras[(size_t)g.sub].camera_model;
}
inline float& group_focal(std::vector<PrepInput>& in, const CameraGroup& g) {
    return g.sub < 0 ? in[g.input].focal_factor
                     : in[g.input].subcameras[(size_t)g.sub].focal_factor;
}
inline float group_focal(const std::vector<PrepInput>& in, const CameraGroup& g) {
    const float f = g.sub < 0 ? in[g.input].focal_factor
                              : in[g.input].subcameras[(size_t)g.sub].focal_factor;
    return f > 0 ? f : in[g.input].focal_factor;
}

// The model each row is actually fitted with. An EMPTY model means "the same
// as the row above" -- a dozen clips off one camera are one decision -- and the
// first row, having none above it, falls back to `fallback`.
std::vector<std::string> camera_group_models(const std::vector<PrepInput>& inputs,
                                             const std::vector<CameraGroup>& groups,
                                             const std::string& fallback);

// What a folder of photos does on its way into the dataset. Only `InPlace`
// leaves it pointing at a folder outside itself, and such a dataset opens
// again only if `image_dir` is set by hand -- which is why it is not default.
enum class PhotoImport {
    ConvertJpeg,   // into images/, re-encoded as JPEG where that loses nothing
    Copy,          // into images/, unchanged
    Move,          // into images/, leaving nothing behind
    InPlace,       // read where they are; only a lone folder can
};
inline constexpr int kNumPhotoImports = 4;

// Quality of the re-encode. High enough that the artefacts are below what the
// photometric loss can tell from sensor noise.
inline constexpr int kPhotoJpegQuality = 95;

struct PrepJob {
    std::vector<PrepInput> inputs;   // in the order the user added them
    std::string workspace;           // output dataset dir (created)
    bool resume = true;              // reuse what a previous run completed
    // Steps to redo on a resumed run; earlier outputs remain reusable so changing
    // a mask prompt does not repeat extraction.
    bool redo_frames = false;
    bool redo_masks = false;
    // The masks that came with the photos mark what to REMOVE, not what to
    // keep. Applied where those files are read, so everything this run writes
    // is in the one convention every reader uses (sfm/core/Mask.h).
    bool flip_found_masks = false;
    // How a photo input reaches images/. Videos ignore it -- their frames are
    // written into the dataset whatever this says.
    PhotoImport photo_import = PhotoImport::ConvertJpeg;

    // The device request for built-in decoding and masking: "auto", an ordinal,
    // a name substring or "uuid:<32 hex>". Frozen at the top of run(); a bad
    // value fails the run. External Python masking does not read this.
    std::string device;

    // ---- video extraction ----
    // What a 360 capture (PrepInput::eac360) becomes. Dataset-wide: mixing
    // panoramas and pinhole faces in one image tree describes no camera rig.
    app::Pano360Options pano;

    float video_fps = 2.0f;          // kept frames per second
    int   sharp_window = 3;          // keep the sharpest of N (1 = off)
    // Every track of a multi-lens file keeps the same instants (one sharpness
    // window over all of them), so every frame is a rig frame. Built-in decoder only.
    bool  sync_tracks = true;
    int   max_frames = 100000;
    // Turn every extracted frame by the rotation the capture asks for, so a
    // portrait clip lands upright and the written files need no metadata read
    // to be shown the right way up.
    bool  auto_rotate = true;
    bool  force_external_decode = false;
    std::string ffmpeg_exe = "ffmpeg";

    // The photographs' colour space. Frames convert to sRGB before the
    // segmenter sees them, which is what it was trained on.
    std::string image_gamut;
    std::optional<bool> image_is_linear;

    // ---- masking ----
    bool mask_enable = false;
    std::string mask_prompt;         // "people; cars; ..."
    std::string mask_negative_prompt;
    bool mask_keep_subject = false;  // prompt names what to KEEP, not remove
    // Share of its own size every matched object grows by before the mask is
    // written, so the PNGs on disk carry the margin. sam::MaskOptions.
    float mask_dilate_ratio = 0.05f;
    int  mask_max_image_size = 1600;
    float mask_threshold = 0.5f;     // detection score a match must reach
    // Box IoU above which the weaker of two detections of one phrase is
    // dropped. Low values thin out a crowd: two people who overlap by a fifth
    // of their boxes are one detection at 0.1.
    float mask_nms = 0.1f;
    // Follow the prompted objects through a video with the model's memory bank
    // rather than segmenting each frame alone: one extra model pass per live
    // instance per frame. Clicks turn it on whatever this says.
    bool mask_memory = false;
    // Memory-bank controls: detection cadence and retained frames. A zero memory
    // limit uses the model default.
    int  mask_detect_every = 1;
    int  mask_memory_frames = 0;
    // Clicked objects, each tagged with its input (MaskClick::source). The only
    // way to prompt a SAM 2 checkpoint. Without a text prompt every input needs
    // its own, and run() refuses the job rather than half-mask the capture.
    std::vector<MaskClick> mask_clicks;

    // Built-in: a checkpoint file (ModelCache resolves it). External: the
    // model name reference/scripts/mask.py understands.
    std::string mask_model_path;
    std::string mask_model_name = "sam2.1_hiera_large";
    bool  force_external_masking = false;
    std::string python_exe = "python3";
};

// Images read where they are instead of gathered into the dataset's own
// images/ (see DatasetPrep::run). Several inputs reconstruct from ONE image
// tree, so there is nowhere for a second one to be read in place from.
inline bool reads_photos_in_place(const std::vector<PrepInput>& inputs,
                                  PhotoImport mode) {
    return mode == PhotoImport::InPlace && inputs.size() == 1 &&
           !inputs[0].is_video;
}

// Where a job's images will be, before it has run: what PrepResult::image_dir
// comes out as, for the panels that must read a dataset a previous run wrote.
std::string planned_image_dir(const std::vector<PrepInput>& inputs,
                              const std::string& workspace, PhotoImport mode);

// A video the run extracted frames from, for the manifest's `captures`:
// the stems carry the source frame index (fps 0, the file's own rate) or,
// after the ffmpeg fallback, the kept-frame count at `fps`.
struct PrepCapture {
    std::string subdir;
    std::string path;
    double fps = 0;
};

struct PrepResult {
    std::vector<PrepCapture> captures;
    std::string image_dir;           // absolute; what SfM should index
    std::string image_dir_cfg;       // what the trainer's image_dir should be
    std::string mask_dir;            // "" when there are no masks
    // ... and what the trainer's mask_dir should be: "masks" for masks the run
    // put in the dataset, an absolute path for masks it only read (photos used
    // where they are bring theirs with them).
    std::string mask_dir_cfg;
    // Those masks are still the other way round -- nonzero means REMOVE. True
    // only where the run handed them on untouched; what it wrote itself is in
    // the usual convention and the readers need no flag.
    bool mask_dir_flipped = false;
    int  n_images = 0;
    // images/ came out holding one sub-folder per camera -- several inputs, or
    // a multi-track video -- so intrinsics must not be shared across them.
    bool per_folder_cameras = false;
};
// The paths and mask convention a preparation will publish before it runs.
// DatasetPrep, scheduled SfM and the GUI handoff all use this same decision;
// actual completed output folders still win when the worker reports them.
PrepResult planned_prep(const PrepJob& job);


// Build/runtime capabilities and user-facing explanations for each fallback.
// Reasons describe why a backend is unavailable; notes describe what will run.
struct Backends {
    // Build-level answers only. The runtime answers (can THIS device decode?)
    // are not here: probing them creates the inference context, which must wait
    // until the device is frozen.
    bool builtin_video = false;
    std::string video_reason;
    std::string video_note;
    bool builtin_masking = false;
    std::string masking_reason;
    std::string masking_note;
};
// What this binary was built with. Creates no device, so it is safe on the UI
// thread and before a GPU choice exists.
const Backends& backends();

// Video container extensions the GUI offers, in the file dialog and for
// drag-and-drop. Sized here so a range-for over it works from another TU.
inline constexpr int kNumVideoExtensions = 13;
extern const char* const kVideoExtensions[kNumVideoExtensions];

// Does this path name one of them? (Extension only; the file need not exist.)
bool is_video_path(const std::string& path);
// A dual-fisheye Insta360 file: two video tracks, one per lens, and a lens the
// default camera model does not fit.
bool is_dual_fisheye_path(const std::string& path);
// A GoPro MAX .360 by its name. The packing itself is what probe_eac360
// confirms; this only decides whether it is worth asking.
bool is_pano360_path(const std::string& path);

// ---- the ffmpeg fallback, for callers that are not a preparation run -------
//
// Video probing used by previews and preparation. Only ffmpeg is assumed;
// ffmpeg -i emits stream metadata even without an output file.

// What an external ffmpeg says about a video. A zero means it did not say.
struct VideoFacts {
    double duration = 0.0;    // seconds
    double fps = 0.0;
    long long frames = 0;     // duration * fps; the container's own count is
                              // not printed by `ffmpeg -i`
    int width = 0, height = 0;   // one frame, before any scaling
    // One entry per video stream, in the order ffmpeg lists them, which is the
    // order `[0:v:N]` and the built-in demuxer both number them by.
    std::vector<std::pair<int, int>> tracks;
};
bool ffmpeg_probe_video(const std::string& ffmpeg_exe, const std::string& path,
                        VideoFacts& out, const std::atomic<bool>& cancel);

// What one still has to reproduce of the run's own ffmpeg invocation.
struct FfmpegStillOpts {
    int  track = 0;
    // ffmpeg turns the picture by the container's matrix unless told not to,
    // which is what the built-in decoder's auto_rotate matches.
    bool auto_rotate = true;
    // A 360 capture: both tracks are decoded and the overlap strips cut out,
    // so what lands in `out_path` is the EAC canvas (app::pano360_graph).
    app::Eac360Layout eac;
};

// One frame, `seconds` into the file, written to `out_path` as a JPEG.
// False when ffmpeg is missing, was cancelled, or wrote nothing.
bool ffmpeg_extract_frame(const std::string& ffmpeg_exe, const std::string& video,
                          double seconds, const std::string& out_path,
                          const std::atomic<bool>& cancel,
                          const FfmpegStillOpts& opts = {});

// The 360 packing a video carries, or a layout that is not valid(). Asks the
// built-in demuxer where there is one and ffmpeg otherwise, so the answer does
// not depend on which decode path the run will take.
app::Eac360Layout probe_eac360(const std::string& ffmpeg_exe,
                               const std::string& path,
                               const std::atomic<bool>& cancel);

// How many video tracks a file carries (0 when it cannot be read).
int probe_video_tracks(const std::string& ffmpeg_exe, const std::string& path,
                       const std::atomic<bool>& cancel);

// The folders under images/<input> a video's frames go to: one per lens of a
// dual-fisheye file or per view of a 360 plan, none for a single lens.
std::vector<std::string> lens_dirs(const PrepJob& job, const PrepInput& in);

// Resolve a picked folder to its image tree and matching mask tree. This
// follows existing dataset conventions and accepts symlinked captures.
void resolve_photo_folder(const std::string& picked, std::string& images,
                          std::string& masks);

// Every folder under `dir` holding images DIRECTLY, '/'-separated, parents
// before children, "" being `dir` itself -- exactly the groups `--camera-mode
// folder` will make, grouping an image on its parent path (core/CameraSetup.h).
std::vector<std::string> camera_subfolders(const std::string& dir);

// Bounds on that walk: it runs on the UI thread and each entry becomes a panel
// row. Past them a folder still reconstructs, sharing the nearest listed
// folder's lens by the overrides' longest-prefix rule.
inline constexpr int kMaxCameraFolderDepth = 4;
inline constexpr size_t kMaxCameraFolders = 64;

// Does this folder hold any image at all, at any depth? Follows directory
// symlinks (a prepared capture's images/ is often a link into the raw one) and
// stops at the first hit, so it is cheap enough for the UI thread.
bool folder_has_images(const std::string& dir);

// Shallow check for an already reconstructed dataset. It routes UI drops; the
// dataset readers remain responsible for full parse validation.
bool folder_looks_like_dataset(const std::string& dir);

// Existing workspace outputs, used by the UI to show what a run reuses or
// replaces. Input paths determine whether the folder is raw or prepared.
struct WorkspaceState {
    bool frames = false;    // images/ this run would extract into
    bool features = false;  // features/, matches.bin, database.db -- reusable
    bool masks = false;     // masks/ this run would generate into
    // A reconstruction any dataset reader can open: this run's own sparse/, or
    // the transforms.json / Metashape export of a dataset that arrived
    // finished. A run pointed at one ADDS to it rather than rebuilding it.
    bool model = false;
    bool geometry = false;  // normals/ or depths/, which a run adds to
    // Whether the model's option stamp is present; without it reuse cannot
    // compare the requested reconstruction settings.
    bool recon_stamp = false;
    // Something a resumed run can pick up instead of redoing.
    bool resumable() const { return frames || features || masks; }
};
WorkspaceState probe_workspace(const std::string& workspace,
                               const std::vector<PrepInput>& inputs);

// Everything a run WROTE into the output folder, absolute, existing ones only:
// what "clear this project" deletes. Never an input -- the images and masks the
// user picked are not leftovers, which is probe_workspace's rule reused.
std::vector<std::string> workspace_artifacts(const std::string& workspace,
                                             const std::vector<PrepInput>& inputs);

// Is this the mask half of one of those layouts, rather than an input of its
// own? By name, which is what makes it a convention: `--mask-dir masks` is the
// SfM default and `mask_dir = "masks"` the dataparsers'.
bool is_mask_folder(const std::string& path);

// The stages preparation can report. The GUI maps these to its own progress
// model; headless callers can ignore them.
enum class Stage {
    Frames, Masks, Features, Matching, Mapping, Geometry, Finishing
};

// One frame the preparation core is producing. Pixel pointers are valid only
// during the callback; a sink that keeps them must copy them.
struct PrepFrame {
    std::string name;
    std::string image_path;
    std::string mask_path;
    const uint8_t* rgb = nullptr;
    int width = 0, height = 0;
    const uint8_t* mask = nullptr;
};

// Optional execution observers. The core never owns a GUI object; sinks may
// forward these events to a progress view or a film reel.
struct DatasetPrepSinks {
    std::function<void(Stage, const std::string&, bool)> log;
    std::function<void(Stage, const std::string&)> enter;
    std::function<void(Stage, int64_t, int64_t)> count;
    std::function<void(Stage, const std::string&)> detail;
    std::function<void(const PrepFrame&)> frame;
};

// One tally spans all inputs so a multi-input step does not rewind its bar.
// Totals begin as estimates and settle to produced counts.
struct StageTally {
    int64_t done = 0;      // items produced so far, over every input
    int64_t total = 0;     // over every input, an estimate until they are done
    int64_t started = 0;   // `done` when the input now running began

    void plan(int64_t estimate) { total += estimate; }
    void settle(int64_t produced, int64_t estimated) {
        done = std::max(done, started + produced);
        total += produced - estimated;
        clamp();
        started = done;
    }
    // A planned pass that turned out not to be needed.
    void drop(int64_t estimated) {
        total -= estimated;
        clamp();
    }
    void clamp() { total = std::max(total, done); }
};
class DatasetPrep {
public:
    DatasetPrep(const std::atomic<bool>& cancel, DatasetPrepSinks sinks = {})
        : _sinks(std::move(sinks)), _cancel(cancel) {}

    // Called before masking so UI edits to mask options are applied after frame
    // extraction has completed.
    using RefreshFn = std::function<void(PrepJob&)>;

    // False with `error` set on failure ("cancelled" when the token was set).
    bool run(const PrepJob& job, PrepResult& out, std::string& error,
             const RefreshFn& refresh_masks = {});

    // Recursive image count matching COLMAP; skip nested masks so their PNGs are
    // not mistaken for views.
    static int count_images(const std::string& dir, const std::string& skip = "");
    // Dimensions of the first image found, for the focal-length prior.
    static bool first_image_dims(const std::string& dir, int& w, int& h);
    // Every image under `dir`, named relative to it, with its pixel size --
    // zero when nothing here reads that format's header (stb has no TIFF or
    // WebP decoder; COLMAP's FreeImage does).
    struct ImageSize { std::string name; int w = 0, h = 0; };
    static std::vector<ImageSize> image_sizes(const std::string& dir,
                                              const std::string& skip = "");

private:
    void log(const std::string& s, bool detail = true);
    void enter(Stage s, const std::string& text);

    // One input's frames. `images` / `masks` are that input's own folders
    // (images/<subdir>, masks/<subdir>); `masked` comes back true only when a
    // resumed run found masks already sitting beside them.
    bool extract_video(const PrepJob& job, const PrepInput& in,
                       const std::string& images, const std::string& masks,
                       PrepResult& out, bool& masked, std::string& error);
    bool extract_video_builtin(const PrepJob& job, const PrepInput& in,
                               const std::string& images,
                               PrepResult& out, std::string& error);
    bool extract_video_ffmpeg(const PrepJob& job, const PrepInput& in,
                              const std::string& images, PrepResult& out,
                              std::string& error);
    // A 360 capture through ffmpeg: one decode writing the EAC canvas, frame
    // selection over those, then our own resampler into the views. ffmpeg is
    // never asked to warp -- see app/Pano360.h.
    bool extract_360_ffmpeg(const PrepJob& job, const PrepInput& in,
                            const std::string& images, PrepResult& out,
                            std::string& error);
    // Photos into the dataset's own images/<subdir>, by whichever of
    // PhotoImport the job asked for -- and the masks they came with into the
    // matching masks/<subdir>, so the two trees still mirror each other.
    bool gather_photos(const PrepJob& job, const PrepInput& in,
                       const std::string& images, const std::string& masks,
                       bool& have_masks, std::string& error);
    // Generate masks per input so memory banks and clicks never cross captures.
    // `folded` reports whether the input stencil was applied during generation.
    bool generate_masks(const PrepJob& job, const PrepInput& in,
                        const std::string& images, const std::string& images_rel,
                        const std::string& masks, const std::string& masks_rel,
                        bool& folded, std::string& error);
    bool generate_masks_builtin(const PrepJob& job, const PrepInput& in,
                                const std::string& images, const std::string& masks,
                                bool& folded, std::string& error);
    bool generate_masks_python(const PrepJob& job, const std::string& images_rel,
                               const std::string& masks_rel, std::string& error);
    // The static stencil on its own, for the masks segmentation did not make.
    // `merge_from` names the masks it folds in when they are not the ones it
    // writes -- the tree the photos arrived with; "" is `masks` itself.
    bool apply_stencil(const PrepJob& job, const PrepInput& in,
                       const std::string& images, const std::string& masks,
                       const std::string& merge_from, std::string& error);
    int exec(const std::vector<std::string>& argv,
             const std::function<void(const std::string&)>& on_line = {});
    // Expected frame count for the progress tally: sampled container frames, photo
    // files, or resumed output already present.
    int64_t estimate_frames(const PrepJob& job, const PrepInput& in,
                            const std::string& images);

    DatasetPrepSinks _sinks;
    Stage _stage = Stage::Frames;
    const std::atomic<bool>& _cancel;
    StageTally _frames_tally, _masks_tally;
};

}  // namespace app
