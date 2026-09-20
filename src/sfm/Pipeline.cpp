// Pipeline.cpp -- the stages of a run, and `auto`'s ordering of them.
//
// This is the half of the old sfm_main.cpp that is not argument parsing: what
// a stage does, in what order, and what it reports. It lives here so the CLI
// and an in-process front end drive one implementation rather than two
// (docs/notes/sfm-in-process-plan.md).
//
// Every stage still reads and writes the same files, so any one of them can
// still be replaced by COLMAP's equivalent to bisect a failure.

#include "sfm/Pipeline.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/ColorSpace.h"
#include "data/Json.h"
#include "core/Env.h"
#include "core/ExrImage.h"
#include "sfm/core/Cancel.h"
#include "sfm/core/Events.h"
#include "sfm/core/Progress.h"
#include "sfm/core/CameraSetup.h"
#include "sfm/core/FeatureCompaction.h"
#include "sfm/core/Log.h"
#include "sfm/core/Features.h"
#include "sfm/core/Image.h"
#include "sfm/core/ImageLoader.h"
#include "sfm/core/Manifest.h"
#include "sfm/core/Mask.h"
#include "sfm/core/Matches.h"
#include "sfm/feature/Matcher.h"
#include "sfm/feature/PairSelection.h"
#include "sfm/feature/Pairing.h"
#include "sfm/feature/Sift.h"
#include "sfm/feature/Verification.h"
#include "sfm/vk/EmbeddedSpirv.h"
#if SS_HAVE_ALIKED
#include "aliked/model/Fetch.h"
#endif
#if SS_HAVE_LOMA
#include "loma/Loma.h"
#include "loma/model/Fetch.h"
#endif
#include "sfm/geometry/TwoView.h"
#include "sfm/map/Assemble.h"
#include "sfm/map/Mapper.h"
#include "sfm/map/MetricGauge.h"
#include "sfm/map/Orient.h"
#include "sfm/map/SensorGauge.h"
#include "sfm/map/Merge.h"

#include "i18n/TimeFormat.h"
#include "i18n/catalog/Sfm.h"

namespace fs = std::filesystem;

namespace sfm {

namespace L = sfm::slog;
namespace M = spirula::i18n::msg::sfm;
using sfm::slog::Tag;
using spirula::i18n::format_duration;

bool isImageExt(const std::string& e) {
    std::string s;
    for (char c : e) s += (char)std::tolower((unsigned char)c);
    return s == ".jpg" || s == ".jpeg" || s == ".png" || s == ".bmp" || s == ".tga" ||
           s == ".ppm" || s == ".pgm" || s == ".exr";
}

// macOS AppleDouble sidecars (`._<name>`, written on exFAT / NTFS / SMB) keep
// the original's extension, so `._00333.jpg` enumerates as an image and
// `._00168.bin` stops the reconstruction on a magic the reader cannot place.
bool isSidecar(const fs::path& p) {
    const std::string n = p.filename().string();
    return n.size() > 2 && n[0] == '.' && n[1] == '_';
}

// Does `root` hold an image outside `nested`? Reinterpreting `auto DATASET`
// as `auto DATASET/images` is allowed only when it cannot lose one: otherwise
// a capture of two folders loses half, under names the trainer cannot resolve.
bool holdsImagesOutside(const fs::path& root, const fs::path& nested) {
    std::error_code walk, ec;
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::follow_directory_symlink, walk);
         !walk && it != fs::recursive_directory_iterator(); it.increment(walk)) {
        if (it->is_directory(ec)) {
            if (fs::equivalent(it->path(), nested, ec)) it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec) && isImageExt(it->path().extension().string()) &&
            !isSidecar(it->path()))
            return true;
    }
    return false;
}

// Path of `p` relative to the directory it was enumerated from. NOT
// fs::relative(): it resolves symlinks, so a directory of symlinked images
// relativizes to "../.." and `extract` writes features outside -o.
fs::path relativeTo(const fs::path& p, const fs::path& root) {
    fs::path r = root;
    // "dir/" iterates to "dir/x.jpg" but has a trailing empty element, which
    // lexically_relative would mismatch into "../x.jpg". Drop it.
    if (!r.empty() && r.filename().empty()) r = r.parent_path();
    return p.lexically_relative(r);
}

// Wall-clock seconds since an arbitrary epoch, for the stage timings `auto`
// reports.
double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Mean/median reprojection error over a reconstruction's observations. The one
// number that says whether a model is sane; reported by both `map` and `auto`.
void reprojStats(const Reconstruction& rec, const std::vector<FeatureSet>& feats,
                        double& mean, double& median, size_t& nobs) {
    std::vector<double> e;
    for (const auto& kv : rec.points3D)
        for (const TrackElement& t : kv.second.track) {
            auto img = rec.images.find(t.image_id);
            if (img == rec.images.end() || !img->second.registered) continue;
            auto cam = rec.cameras.find(img->second.camera_id);
            if (cam == rec.cameras.end()) continue;
            Vec3 pc = mul(img->second.pose.R, kv.second.xyz) + img->second.pose.t;
            if (pc.z <= 0) continue;
            Vec2 px = cam->second.project(pc);
            const Keypoint& k = feats[t.image_id].keypoints[t.point2D_idx];
            e.push_back(std::hypot(px.x - k.x, px.y - k.y));
        }
    nobs = e.size();
    mean = median = 0;
    if (e.empty()) return;
    double s = 0;
    for (double v : e) s += v;
    mean = s / e.size();
    std::nth_element(e.begin(), e.begin() + e.size() / 2, e.end());
    median = e[e.size() / 2];
}

// Sample an RGB color at every keypoint from the (already decoded) source image
// and store it in the feature set, so the point cloud can be colored without a
// second decode pass. No-op if the image was decoded without color.
void sampleFeatureColors(FeatureSet& fs, const GrayImage& img) {
    if (!img.hasColor()) return;
    fs.colors.resize((size_t)fs.count() * 3);
    for (uint32_t i = 0; i < fs.count(); i++)
        sampleColor(img, fs.keypoints[i].x, fs.keypoints[i].y, &fs.colors[(size_t)i * 3]);
}

// Put a freshly extracted set into the source image's frame and attach what
// EXIF said about the camera. Called once per image, after anything that
// indexes the *decoded* image (colors, masks) is done.
void finishFeatures(FeatureSet& fs, const GrayImage& img) {
    scaleKeypoints(fs, img.orig_width, img.orig_height);
    fs.exif_focal = exifFocalPx(img.exif, fs.width, fs.height);
    fs.exif_camera = exifCameraKey(img.exif, fs.width, fs.height);
    fs.exif_orientation = (uint8_t)img.exif.orientation;
}

// Why a metric fit was refused, with the numbers, so one line is a complete
// bug report.
std::string metricReason(const MetricFit& f) {
    switch (f.reason) {
        case MetricFail::Pairs:
            return spirula::i18n::format(M::metric_fail_pairs, {(long long)f.n});
        case MetricFail::Spread:
            return spirula::i18n::format(M::metric_fail_spread, {L::num(f.spread, 3)});
        case MetricFail::Inliers:
            return spirula::i18n::format(M::metric_fail_inliers,
                                {L::num(f.max_error, 3), (long long)f.inliers,
                                 (long long)f.n});
        case MetricFail::Collinear:
            return spirula::i18n::format(
                M::metric_fail_collinear,
                {L::num(100.0 * f.perp_frac, 2), L::num(100.0 * kMetricMinPerpFraction, 1),
                 L::num(f.perp_frac > 0 ? 1.0 / f.perp_frac : 0.0, 0)});
        case MetricFail::None: break;
    }
    return {};
}

// A telemetry file read once per run, with the queries the gauge fit makes.
struct LoadedCapture {
    SensorCapture cap;
    SensorTimeline timeline;
};

std::vector<std::unique_ptr<LoadedCapture>> loadCaptures(const SfmConfig& cfg, bool verbose) {
    std::vector<std::unique_ptr<LoadedCapture>> out;
    if (cfg.sensor_gauge == "none") return out;
    for (const TelemetryInput& in : cfg.telemetry_inputs) {
        Telemetry t;
        std::string err;
        if (!telemetry_read(in.path, t, err) || t.empty()) {
            L::warn(Tag::Orient, M::sensor_file_bad, {in.path, err.empty() ? "-" : err});
            continue;
        }
        const TelemetryCheck c = telemetry_check(t);
        auto lc = std::make_unique<LoadedCapture>();
        if (!lc->timeline.init(t, c, err)) {
            L::warn(Tag::Orient, M::sensor_file_bad, {in.path, err});
            continue;
        }
        lc->cap.prefix = in.prefix;
        lc->cap.path = in.path;
        lc->cap.camera = t.camera;
        lc->cap.fps = in.fps > 0 ? in.fps : t.video_fps;
        lc->cap.time_offset = in.time_offset;
        lc->cap.readout = t.frame_readout;
        lc->cap.timeline = &lc->timeline;
        L::out(Tag::Orient, M::sensor_file,
               {in.path, t.camera.empty() ? "?" : t.camera, L::num(c.gyro.rate_hz, 0),
                L::num(c.accel.rate_hz, 0), L::num(c.orientation.rate_hz, 0),
                (long long)c.gps_distinct, L::num(c.gps_path_m, 0)});
        if (!(lc->cap.fps > 0)) {
            L::warn(Tag::Orient, M::sensor_file_no_fps, {in.path});
            continue;
        }
        if (verbose)
            for (const std::string& w : c.warnings) L::err_raw(Tag::Orient, w);
        out.push_back(std::move(lc));
    }
    return out;
}

const spirula::i18n::Msg& extrinsicReason(ExtrinsicFail f) {
    switch (f) {
        case ExtrinsicFail::Frames: return M::sensor_calib_fail_frames;
        case ExtrinsicFail::Pairs: return M::sensor_calib_fail_pairs;
        case ExtrinsicFail::Disagree: return M::sensor_calib_fail_disagree;
        case ExtrinsicFail::NoStream: return M::sensor_calib_fail_nostream;
        case ExtrinsicFail::Degenerate: return M::sensor_calib_fail_degenerate;
        case ExtrinsicFail::None: break;
    }
    return M::sensor_calib_fail_pairs;
}

const spirula::i18n::Msg& noScaleReason(SensorNoScale r) {
    switch (r) {
        case SensorNoScale::NotAsked: return M::sensor_no_scale_not_asked;
        case SensorNoScale::ImuWeak: return M::sensor_no_scale_imu_weak;
        case SensorNoScale::GpsRefused: return M::sensor_no_scale_gps_refused;
        case SensorNoScale::Disagree: return M::sensor_no_scale_disagree;
        case SensorNoScale::NoSource:
        case SensorNoScale::None: break;
    }
    return M::sensor_no_scale_no_source;
}

void reportSensorGauge(size_t i, const SensorGaugeResult& r,
                       const std::vector<std::unique_ptr<LoadedCapture>>& caps, bool verbose) {
    const long long model = (long long)i;
    if (r.fail == SensorFail::NoFrames || r.fail == SensorFail::NoTelemetry) {
        L::err(Tag::Orient, M::sensor_declined, {model, M::sensor_fail_frames.get()});
        return;
    }
    if (r.frames_untimed > 0)
        L::err(Tag::Orient, M::sensor_untimed,
               {model, (long long)r.frames_untimed, (long long)(r.frames_untimed + r.frames_timed)});
    for (size_t c = 0; c < r.time_offsets.size() && c < caps.size(); c++)
        if (r.time_offsets[c].found)
            L::err(Tag::Orient, M::sensor_time_offset,
                   {caps[c]->cap.path, L::num(1000.0 * r.time_offsets[c].offset, 1),
                    (long long)r.time_offsets[c].pairs});
    for (const SensorGroupReport& g : r.groups) {
        const std::string name = g.name.empty() ? std::string(".") : g.name;
        if (!g.fit.ok) {
            L::err(Tag::Orient, M::sensor_calib_failed, {name, extrinsicReason(g.fit.reason).get()});
            continue;
        }
        if (verbose) {
            L::err(Tag::Orient, M::sensor_calib,
                   {name, (long long)g.fit.frames, L::num(g.fit.sig_rot_deg, 2),
                    L::num(g.fit.sig_grav_deg, 2)});
            if (g.triples > 0)
                L::err(Tag::Orient, M::sensor_calib_scale,
                       {name, L::num(g.scale, 6), L::num(100.0 * g.scale_sigma, 2),
                        (long long)g.triples, L::num(g.g_norm, 2), L::num(g.g_angle_deg, 1)});
        }
        if (g.fit.mirrored && !g.fit.degenerate)
            L::err(Tag::Orient, M::sensor_calib_mirrored, {name});
        if (g.fit.degenerate) L::err(Tag::Orient, M::sensor_calib_yaw_free, {name});
    }
    if (r.up_from_imu)
        L::out(Tag::Orient, M::sensor_up,
               {model, (long long)r.up.votes, L::num(r.up.spread_deg, 2), (long long)r.up.outliers});
    if (r.fail == SensorFail::NoUp) {
        L::err(Tag::Orient, M::sensor_declined, {model, M::sensor_fail_noup.get()});
        return;
    }
    if (r.scale_imu_sigma > 0) {
        int triples = 0;
        double g_norm = 0, g_angle = 0;
        for (const SensorGroupReport& g : r.groups) {
            triples += g.triples;
            if (g.triples > 0 && g.g_norm > 0) { g_norm = g.g_norm; g_angle = g.g_angle_deg; }
        }
        if (r.scale_from_imu || r.disagree)
            L::out(Tag::Orient, M::sensor_scale_imu,
                   {model, L::num(r.scale_imu, 6), L::num(100.0 * r.scale_imu_sigma, 2),
                    (long long)triples, L::num(g_norm, 2), L::num(g_angle, 1)});
        else
            L::err(Tag::Orient, M::sensor_scale_imu_weak,
                   {model, L::num(100.0 * r.scale_imu_sigma, 1), (long long)triples});
    }
    if (r.gps_frames > 0) {
        if (r.gps.ok)
            L::out(Tag::Orient, M::sensor_scale_gps,
                   {model, L::num(r.scale_gps, 6), L::num(100.0 * r.scale_gps_sigma, 2),
                    (long long)r.gps_frames, L::num(r.gps.max_error, 1), (long long)r.gps.inliers,
                    (long long)r.gps.n, L::num(r.gps.rms, 2)});
        else
            L::err(Tag::Orient, M::sensor_scale_gps_failed, {model, metricReason(r.gps)});
    }
    if (r.disagree)
        L::warn(Tag::Orient, M::sensor_disagree, {model, L::num(r.scale_imu, 6), L::num(r.scale_gps, 6)});
    if (!r.applied) return;
    if (!r.metric) {
        L::out(Tag::Orient, M::sensor_up_only,
               {model, L::num(r.T.scale, 4), noScaleReason(r.no_scale).get()});
        return;
    }
    const spirula::i18n::Msg& src = r.scale_from_imu && r.scale_from_gps ? M::sensor_src_both
                                    : r.scale_from_imu                    ? M::sensor_src_imu
                                                                          : M::sensor_src_gps;
    L::out(Tag::Orient, M::sensor_done,
           {model, src.get(), L::num(r.T.scale, 6), L::num(100.0 * r.scale_sigma, 2),
            L::num(r.tilt_sigma_deg, 2)});
}

// The gauge every finished model is written in: the video's sensors when
// named and the fit holds, else a metric reference when given and fitted,
// else the orient frame. False when a metric frame was asked for and missed.
bool fixGauge(std::vector<Reconstruction>& models, const SfmConfig& cfg,
                     const std::string& imagedir, bool verbose,
                     std::vector<ModelGauge>& gauge) {
    const bool gps = cfg.metric_gps != "none";
    const bool flat = cfg.metric_gps == "horizontal";
    const bool file = !cfg.metric_positions.empty();
    // A portrait capture's up is 90 degrees off its images'; `apply` already
    // turned the pixels, so only `orient` corrects anything. The tags arrive on
    // the models; a caller that read them off disk calls fillExifOrientations.
    const bool exif_up = cfg.exif_orientation == "orient";

    // `gauge[i]` is the state, not just the record: `oriented` and `metric` say
    // what a source has already settled, and every source below reads them
    // before touching what an earlier one answered.
    gauge.assign(models.size(), ModelGauge());

    // ---- the video's own sensors -----------------------------------------
    const std::vector<std::unique_ptr<LoadedCapture>> loaded = loadCaptures(cfg, verbose);
    if (!loaded.empty()) {
        std::vector<SensorCapture> caps;
        for (const auto& lc : loaded) caps.push_back(lc->cap);
        SensorGaugeOptions opt;
        opt.mode = cfg.sensor_gauge == "up" ? SensorMode::Up : SensorMode::Auto;
        opt.gps_full = cfg.metric_gps == "full";
        opt.gps_max_error = gps ? cfg.metric_max_error : 5.0;
        opt.gps_max_error_frac = cfg.metric_max_error_frac;
        opt.verbose = verbose;
        for (size_t i = 0; i < models.size(); i++) {
            const SensorGaugeResult r = fitSensorGauge(models[i], caps, opt);
            reportSensorGauge(i, r, loaded, verbose);
            if (!r.applied) continue;
            applySim3(models[i], r.T);
            gauge[i].oriented = true;
            gauge[i].up = "sensors";
            if (r.metric) {
                gauge[i].metric = true;
                gauge[i].scale = r.scale_from_imu && r.scale_from_gps ? "imu+gps"
                                 : r.scale_from_imu                   ? "imu"
                                                                     : "gps";
                gauge[i].scale_sigma = r.scale_sigma;
            }
        }
    }

    // ---- an outside metric reference --------------------------------------
    bool all = true;
    std::map<std::string, Vec3> positions;
    if (file) {
        std::string err;
        if (!readMetricPositions(cfg.metric_positions, positions, err)) {
            L::fail(Tag::Orient, M::metric_positions_bad, {cfg.metric_positions, err});
            all = false;
        }
    }
    for (size_t i = 0; i < models.size() && all && (gps || file); i++) {
        // A metric sensor frame settles the model; a second reference over it
        // could only disagree with the one already applied.
        if (gauge[i].metric) continue;
        MetricRef ref;
        if (file) {
            const MetricPairCounts pc = pairMetricRef(models[i], positions, ref);
            L::out(Tag::Orient, M::metric_matched,
                   {(long long)pc.matched, (long long)(pc.matched + pc.unmatched_model),
                    (long long)pc.unmatched_file});
        } else {
            const MetricGpsCounts gc = metricRefFromGps(models[i], imagedir, ref);
            L::out(Tag::Orient, M::metric_gps_read,
                   {(long long)gc.matched, (long long)(gc.matched + gc.no_gps),
                    (long long)gc.no_alt});
        }
        // Horizontal mode takes the tilt from the caller's up axis, so its fit
        // -- scale, heading and place -- runs in an upright frame. Where a
        // sensor already levelled the model, that frame is the one it is in.
        const Sim3 pre =
            flat && !gauge[i].oriented ? uprightTransform(models[i], exif_up) : Sim3{};
        for (Vec3& c : ref.centres) c = transformPoint(pre, c);
        const MetricFit fit =
            fitMetricGauge(ref, cfg.metric_max_error,
                           flat ? MetricAxes::Horizontal : MetricAxes::Full,
                           cfg.metric_max_error_frac);
        // A refused fit leaves the model in the frame it came in with -- the
        // sensors', or the normalized one the fallback below writes. Applying
        // the identity it returns would still claim the metre.
        if (!fit.ok) {
            all = false;
            L::warn(Tag::Orient, M::metric_failed, {(long long)i, metricReason(fit)});
            continue;
        }
        applySim3(models[i], composeSim3(fit.T, pre));
        gauge[i].metric = true;
        gauge[i].scale = file ? "positions" : "gps";
        gauge[i].scale_sigma = fit.scale_unc;
        // Horizontal fits scale, heading and place only, so which way is up is
        // still whatever `pre` left it as: the sensors, or the cameras.
        if (!flat) {
            gauge[i].oriented = true;
            gauge[i].up = file ? "positions" : "gps";
        } else if (!gauge[i].oriented) {
            gauge[i].up = "cameras";
        }
        L::out(Tag::Orient, M::metric_done,
               {(long long)i,
                spirula::i18n::format(!gps    ? M::metric_source_positions
                                      : flat  ? M::metric_source_gps_flat
                                              : M::metric_source_gps,
                                      {}),
                L::num(fit.T.scale, 6), L::num(fit.max_error, 3), (long long)fit.inliers,
                (long long)fit.n, L::num(fit.rms, 4), L::num(fit.scale_unc, 3),
                L::num(fit.rot_unc_deg, 3)});
        if (!verbose) continue;
        // A drifting altitude offset is absorbed by the rotation as a tilt and
        // leaves the RMS looking fine; these two numbers are what show it.
        double e = 0, n = 0, u = 0;
        for (size_t k = 0; k < ref.centres.size(); k++) {
            if (!fit.inlier_mask[k]) continue;
            const Vec3 r = ref.targets[k] - transformPoint(fit.T, ref.centres[k]);
            e += r.x * r.x;
            n += r.y * r.y;
            u += r.z * r.z;
        }
        const double m = std::max(fit.inliers, 1);
        L::err(Tag::Orient, M::metric_axes,
               {L::num(std::sqrt(e / m), 4), L::num(std::sqrt(n / m), 4),
                L::num(std::sqrt(u / m), 4),
                L::num(metricUpDisagreementDeg(models[i]), 2)});
    }

    // ---- whatever no source settled ---------------------------------------
    // The only place that falls back on the cameras' mean up axis, so a model
    // something measured cannot be re-levelled by the guess it replaced.
    if (cfg.orient)
        for (size_t i = 0; i < models.size(); i++) {
            if (gauge[i].oriented || gauge[i].metric) continue;
            const Sim3 T = orientModel(models[i], exif_up);
            gauge[i].up = exif_up ? "cameras+exif" : "cameras";
            if (verbose) L::err(Tag::Orient, M::orient_done, {(long long)i, L::num(T.scale, 4)});
        }
    return all;
}

// An EXR carries its own colour space. Reading it needs no declaration -- the
// decoder falls back to the file's own -- but --point-color and the reported
// space both do, so adopt it before any stage runs.
void adoptExrColorSpace(SfmConfig& cfg, const std::string& imagedir,
                        const std::set<std::string>& seen) {
    const bool take_gamut = !seen.count("image-gamut");
    const bool take_linear = !seen.count("image-linear");
    if (!take_gamut && !take_linear) return;
    std::error_code ec, walk;
    for (auto it = fs::recursive_directory_iterator(
             imagedir, fs::directory_options::follow_directory_symlink, walk);
         !walk && it != fs::recursive_directory_iterator(); it.increment(walk)) {
        if (!it->is_regular_file(ec)) continue;
        if (!isImageExt(it->path().extension().string()) || isSidecar(it->path()))
            continue;
        exr::Info info;
        if (!exr::declared_color_space(it->path().string(), info)) return;
        if (take_gamut) cfg.image_gamut = info.gamut;
        if (take_linear) cfg.image_is_linear = info.is_linear;
        const std::string name =
            cfg.image_gamut.empty() ? "Rec.709" : cfg.image_gamut;
        if (take_linear) L::out(Tag::Run, M::run_exr_color, {name});
        else             L::out(Tag::Run, M::run_exr_gamut_from_file, {name});
        if (take_gamut && !info.gamut_known)
            L::warn(Tag::Run, M::run_exr_gamut_unknown, {});
        return;
    }
}

void reportFeatureCompaction(const FeatureCompactionStats& stats) {
    const double removed_pct =
        stats.original_features ? 100.0 * stats.removedFeatures() / stats.original_features : 0.0;
    L::out(Tag::Map, M::map_feature_compaction,
           {(long long)stats.original_features, (long long)stats.compact_features,
            (long long)stats.removedFeatures(), L::num(removed_pct, 2), (long long)stats.images,
            (long long)stats.zero_feature_images, (long long)stats.pairs,
            (long long)stats.correspondences});
}

// Point colours are sampled from images the loader converted to sRGB, which is
// where "srgb" leaves them. "image" puts them back in the photographs' space,
// for a trainer run with convert_initial_point_cloud_color off.
void recolorPoints(std::vector<Reconstruction>& models, const SfmConfig& cfg) {
    if (cfg.point_color_space != "image") return;
    if (colorspace::is_identity(cfg.image_gamut, cfg.image_is_linear)) return;
    for (Reconstruction& m : models)
        for (auto& kv : m.points3D)
            colorspace::from_srgb_inplace(kv.second.rgb, 1, cfg.image_gamut,
                                          cfg.image_is_linear);
}

// A COLMAP camera *is* a frame size, and grouping buckets sizes within 2%
// (CameraSetup.h), so give each size in a group its own camera before writing.
// The parameters stay the group's: the images shared them through BA.
void splitCamerasBySize(std::vector<Reconstruction>& models,
                               const std::vector<FeatureSet>& feats) {
    uint32_t next = 0;
    for (const Reconstruction& rec : models)
        for (const auto& kv : rec.cameras) next = std::max(next, kv.first);
    // One id space over all the models: they overlap by design (D41), and a
    // merge keeps the destination's camera for an id both sides use.
    std::map<std::pair<uint32_t, uint64_t>, uint32_t> key2id;
    std::set<uint32_t> kept;  // groups that a first size took the id of
    for (Reconstruction& rec : models) {
        std::map<uint32_t, Camera> cams;
        for (auto& kv : rec.images) {
            Image& im = kv.second;
            if (!im.registered) continue;
            auto c = rec.cameras.find(im.camera_id);
            if (c == rec.cameras.end()) continue;
            int w = c->second.width, h = c->second.height;
            if (im.id < feats.size() && feats[im.id].width > 0 && feats[im.id].height > 0) {
                w = feats[im.id].width;
                h = feats[im.id].height;
            }
            const std::pair<uint32_t, uint64_t> key{
                im.camera_id, ((uint64_t)(uint32_t)w << 32) | (uint32_t)h};
            auto it = key2id.find(key);
            if (it == key2id.end())
                // The group keeps its id for the first size written with it, so
                // a capture of one frame size writes what it always did.
                it = key2id.emplace(
                    key, kept.insert(im.camera_id).second ? im.camera_id : ++next).first;
            if (!cams.count(it->second)) {
                Camera nc = c->second;
                nc.id = it->second;
                nc.width = w;
                nc.height = h;
                cams[nc.id] = nc;
            }
            im.camera_id = it->second;
        }
        if (!cams.empty()) rec.cameras = std::move(cams);
    }
}

// gauge.txt beside the model: whether +Z is up and whether a unit is a metre.
// Plain text and not hidden, because it is as much for the user reading the
// folder as for the viewer that stops guessing an up axis when it is there.
void writeGauge(const fs::path& dir, const ModelGauge& g) {
    std::ofstream f(dir / "gauge.txt", std::ios::trunc);
    if (!f) return;
    f << "# What this model's frame means, from spirula sfm.\n";
    f << "oriented " << (g.oriented ? 1 : 0) << "\n";
    f << "metric " << (g.metric ? 1 : 0) << "\n";
    f << "up " << g.up << "\n";
    f << "scale " << g.scale << "\n";
    if (g.scale_sigma > 0) f << "scale_sigma " << g.scale_sigma << "\n";
}

// rigs.txt beside a model that used one: each member's cam_from_rig as the
// run settled it, in the model's own units -- what a later run could be
// handed back as a manifest's `rotation` / `translation`.
void writeRigs(const fs::path& dir, const Reconstruction& m, const RigTable* rigs) {
    if (!rigs || m.rigs.empty()) return;
    std::ofstream f(dir / "rigs.txt", std::ios::trunc);
    if (!f) return;
    f << "# Rig calibration from spirula sfm, cam_from_rig per member as\n"
         "# rig member reference qw qx qy qz tx ty tz frames spread_deg, in this model's units.\n";
    f.precision(12);
    for (size_t r = 0; r < rigs->rigs.size() && r < m.rigs.size(); r++) {
        const RigSpec& spec = rigs->rigs[r];
        const RigCalib& c = m.rigs[r];
        if (c.ref < 0) continue;
        for (size_t k = 0; k < spec.members.size() && k < c.cam_from_rig.size(); k++) {
            if (!c.established[k]) continue;
            const Quat q = rotationToQuaternion(c.cam_from_rig[k].R);
            const Vec3& t = c.cam_from_rig[k].t;
            f << spec.name << ' ' << spec.members[k].prefix << ' '
              << spec.members[(size_t)c.ref].prefix << ' ' << q[0] << ' ' << q[1] << ' ' << q[2]
              << ' ' << q[3] << ' ' << t.x << ' ' << t.y << ' ' << t.z << ' ' << c.support[k]
              << ' ' << c.spread_deg[k] << "\n";
        }
    }
}

// Every reconstruction as <dir>/0, <dir>/1, ... (D41) -- COLMAP's layout for a
// view graph that is not connected. `sparse/0` has the most 3D points, so a
// single-model dataset still writes exactly `sparse/0`.
void writeModels(const std::vector<Reconstruction>& models, const fs::path& dir,
                 bool verbose, const std::vector<ModelGauge>& gauge, const RigTable* rigs) {
    for (size_t i = 0; i < models.size(); i++) {
        fs::path p = dir / std::to_string(i);
        fs::create_directories(p);
        models[i].writeBinary(p.string());
        if (i < gauge.size()) writeGauge(p, gauge[i]);
        writeRigs(p, models[i], rigs);
        if (verbose)
            L::err(Tag::Map, M::map_wrote_model,
                   {(long long)i, (long long)models[i].numRegistered(),
                    (long long)models[i].points3D.size(), p.string()});
    }
    // A re-run that produces fewer models than the last one left numbered
    // directories behind, and `--resume` would read them back as if they were
    // part of this reconstruction. Remove them.
    if (!fs::is_directory(dir)) return;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (!e.is_directory()) continue;
        const std::string name = e.path().filename().string();
        if (name.find_first_not_of("0123456789") != std::string::npos) continue;
        size_t idx = std::stoull(name);
        if (idx < models.size()) continue;
        if (!fs::exists(e.path() / "images.bin")) continue;
        std::error_code ec;
        fs::remove_all(e.path(), ec);
        if (verbose && !ec)
            L::err(Tag::Map, M::map_removed_stale, {e.path().string()});
    }
}

// Distinct images covered by any model. Not the sum of the model sizes: models
// deliberately overlap by up to max_model_overlap images (D41), which is what a
// merge step aligns on, so summing double-counts the joins.
size_t distinctRegistered(const std::vector<Reconstruction>& models) {
    std::set<uint32_t> ids;
    for (const Reconstruction& m : models)
        for (const auto& kv : m.images)
            if (kv.second.registered) ids.insert(kv.first);
    return ids.size();
}

// One line per model beyond the first, so a fragmented capture is visible in
// the summary rather than only in the directory listing.
void printExtraModels(const std::vector<Reconstruction>& models,
                             const std::vector<FeatureSet>& feats) {
    for (size_t i = 1; i < models.size(); i++) {
        double mn = 0, md = 0;
        size_t nobs = 0;
        reprojStats(models[i], feats, mn, md, nobs);
        L::out(Tag::Run, M::map_model_line_error,
               {(long long)i, (long long)models[i].numRegistered(),
                (long long)models[i].points3D.size(), L::num(mn, 3)});
    }
}

// Registration per top-level image sub-folder: one folder per input, and an
// input that contributed nothing is a dataset silently describing half of what
// was handed over. Only printed for more than one folder.
void printFolderCoverage(const std::vector<Reconstruction>& models,
                         const MatchesDatabase& db) {
    auto group_of = [](const std::string& name) {
        size_t slash = name.find('/');
        return slash == std::string::npos ? std::string(".") : name.substr(0, slash);
    };
    std::map<std::string, std::pair<size_t, size_t>> per;  // folder -> {registered, total}
    for (const auto& im : db.images) per[group_of(im.name)].second++;
    if (per.size() < 2) return;
    std::set<uint32_t> ids;
    for (const Reconstruction& m : models)
        for (const auto& kv : m.images)
            if (kv.second.registered) ids.insert(kv.first);
    for (uint32_t id : ids)
        if (id < db.images.size()) per[group_of(db.images[id].name)].first++;
    L::out(Tag::Run, M::sum_per_folder);
    for (const auto& kv : per)
        L::out(Tag::Run, M::sum_folder_line,
               {kv.first, (long long)kv.second.first, (long long)kv.second.second});
    for (const auto& kv : per)
        if (kv.second.first == 0) L::warn(Tag::Run, M::sum_folder_empty, {kv.first});
}

// Feature stems carry no extension; map every file under the images folder
// from "<relative path without extension>" to its real name.
static std::map<std::string, std::string> imageStemMap(const std::string& imagedir) {
    std::map<std::string, std::string> stem2name;
    if (imagedir.empty()) return stem2name;
    for (const auto& e : fs::recursive_directory_iterator(imagedir))
        if (e.is_regular_file() && !isSidecar(e.path())) {
            fs::path rel = relativeTo(e.path(), imagedir);
            fs::path stem = rel;
            stem.replace_extension();
            stem2name[stem.generic_string()] = rel.generic_string();
        }
    return stem2name;
}

// The unregistered list as a data file, when SS_UNREG_LOG names one: per
// folder, every image no model took. Data only -- names need no translation;
// full coverage writes nothing.
static void writeUnregisteredList(const std::vector<Reconstruction>& models,
                                  const MatchesDatabase& db,
                                  const std::string& imagedir) {
    const char* path = spirula::env("UNREG_LOG");
    if (!path || !*path) return;
    std::set<uint32_t> ids;
    for (const Reconstruction& m : models)
        for (const auto& kv : m.images)
            if (kv.second.registered) ids.insert(kv.first);
    const std::map<std::string, std::string> stem2name = imageStemMap(imagedir);
    std::map<std::string, std::vector<std::string>> missing;
    for (size_t i = 0; i < db.images.size(); i++) {
        if (ids.count(uint32_t(i))) continue;
        std::string name = db.images[i].name;
        const auto stem = stem2name.find(name);
        if (stem != stem2name.end()) name = stem->second;
        const size_t slash = name.find('/');
        missing[slash == std::string::npos ? std::string(".")
                                           : name.substr(0, slash)]
            .push_back(name);
    }
    if (missing.empty()) return;
    std::ofstream f(path, std::ios::trunc);
    if (!f) return;
    size_t unreg = 0;
    for (const auto& kv : missing) unreg += kv.second.size();
    f << "unregistered " << unreg << '/' << db.images.size() << "\n";
    for (const auto& kv : missing) {
        f << '\n' << '[' << (kv.first == "." ? "(root)" : kv.first.c_str())
          << "] " << kv.second.size() << '\n';
        for (const std::string& n : kv.second) f << n << '\n';
    }
}


// What became of the mapper's models: one line, because a capture that comes
// back in several pieces is the case a user has to be able to reason about, and
// the counts say whether that was the view graph's doing or a refused merge.
void printAssembly(const AssembleStats& ast, size_t models, Tag tag) {
    if (!ast.models_in) return;
    const ManagerStats& f = ast.finish;
    L::out(tag, M::map_assembled,
           {format_duration(ast.t_merge + ast.t_ba + ast.t_grow + ast.finishSecs()),
            (long long)ast.models_in,
            (long long)models, (long long)ast.rounds, (long long)ast.merges,
            (long long)ast.merges_refused, (long long)ast.grown_images,
            (long long)f.covered_before, (long long)f.covered_after});
    L::out(tag, M::map_finishing,
           {format_duration(ast.finishSecs()), (long long)f.splits,
            (long long)f.duplicate_splits, (long long)f.reseeded_models,
            (long long)f.dropped_redundant, (long long)f.audited_repaired,
            (long long)f.audited_out});
}

// Flat or bottom-up, per --mapper; flat is the default and what the
// measurements are on. Either way the same schedule assembles the models (D63,
// sfm/map/Assemble.h) -- there is no separate manage stage.
RigTable buildRigs(const MatchesDatabase& db, const SfmConfig& cfg, bool verbose) {
    std::vector<std::string> names;
    names.reserve(db.images.size());
    for (const ImageEntry& im : db.images) names.push_back(im.name);
    RigTable rigs = buildRigTable(names, cfg.rigs);
    if (!verbose) return rigs;
    for (const RigSpec& r : rigs.rigs) {
        size_t full = 0;
        for (const auto& fr : r.frames) {
            bool all = true;
            for (uint32_t img : fr) all = all && img != kNoImage;
            full += all ? 1 : 0;
        }
        std::string members;
        for (const RigMemberDef& m : r.members)
            members += (members.empty() ? "" : ", ") + m.prefix;
        L::out(Tag::Map, M::rig_table,
               {r.name, members, (long long)r.frames.size(), (long long)full,
                r.anyKnownExt() ? M::rig_ext_given.get() : M::rig_ext_estimated.get()});
    }
    return rigs;
}

std::vector<Reconstruction> runMapper(Mapper& mapper, const MatchesDatabase& db,
                                      const std::vector<FeatureSet>& feats, SfmConfig& cfg,
                                      AssembleStats& ast) {
    const bool bup = cfg.mapper_mode == "bottom-up" || cfg.mapper_mode == "hierarchical";
    AssembleOptions ao = cfg.assemble;
    ao.verbose = !cfg.quiet;
    std::vector<Reconstruction> models;
    if (!bup) {
        ao.tag = "map";
        models = assembleModels(mapper, mapper.run(), cfg.manager, ao, ast);
    } else {
        ao.tag = "bup";
        BottomUpStats bs;
        BottomUpOptions bo = cfg.bup;
        bo.verbose = !cfg.quiet;
        models = bottomUpReconstruct(mapper, db, feats, bo, cfg.manager, ao, bs);
        ast = bs.assemble;
    }
    // The assembly passes move images between models, so the last snapshot the
    // mapper took is not what came out. Leave the largest result on screen.
    if (!models.empty()) sfm::progress::model(models.front(), /*force=*/true);
    return models;
}

// The finishing passes: one global bundle adjustment per model releasing what
// the mapper held, then optionally another with every image on its own
// intrinsics. src/sfm/README.md, "The finishing passes", has the reasoning.
std::vector<Reconstruction> finishModels(Mapper& mapper,
                                                std::vector<Reconstruction> models,
                                                const SfmConfig& cfg, bool verbose,
                                                double& secs) {
    const double t0 = now();
    // Nothing to release is a solve that ends where it started, and a line in
    // the log saying it ran.
    if (cfg.final_principal_point ||
        (cfg.final_extra_params && !cfg.mapper.refine_extra_params)) {
        for (Reconstruction& m : models)
            if (m.numRegistered() >= 2)
                m = mapper.polish(m, cfg.final_principal_point, cfg.final_extra_params);
        if (verbose)
            L::err(Tag::Map, M::map_final_intrinsics,
                   {(long long)models.size(), format_duration(now() - t0)});
    }
    if (cfg.final_per_image_intrinsics) {
        const double t1 = now();
        for (Reconstruction& m : models)
            m = mapper.perImageIntrinsics(m, cfg.final_extra_params);
        if (verbose)
            L::err(Tag::Map, M::map_per_image_done,
                   {(long long)models.size(), format_duration(now() - t1)});
    }
    if (cfg.final_free_rig && mapper.rigs()) {
        const double t1 = now();
        for (Reconstruction& m : models) m = mapper.releaseRigs(m);
        if (verbose)
            L::err(Tag::Map, M::map_free_rig_done,
                   {(long long)models.size(), format_duration(now() - t1)});
    }
    secs = now() - t0;
    return models;
}

// Feature stems carry no extension; put the real filename back into every
// model for COLMAP tooling. Walked once, not once per model -- a fragmented
// capture can produce dozens (D41).
void resolveImageNames(std::vector<Reconstruction>& models, const std::string& imagedir) {
    if (imagedir.empty()) return;
    const std::map<std::string, std::string> stem2name = imageStemMap(imagedir);
    for (Reconstruction& rec : models)
        for (auto& kv : rec.images) {
            auto it = stem2name.find(kv.second.name);
            if (it != stem2name.end()) kv.second.name = it->second;
        }
}
void resolveImageNames(std::vector<Reconstruction>& models,
                       const feature_work::FeaturePlan& plan) {
    for (Reconstruction& reconstruction : models)
        for (auto& entry : reconstruction.images) {
            if (entry.first >= plan.images.size())
                throw std::runtime_error("model image index is outside the feature plan");
            entry.second.name = plan.images[entry.first].logical_name;
        }
}


// Report the grouping decision, one line per camera. Worth printing in full:
// a wrong --camera-mode is otherwise invisible until the intrinsics come out
// strange, and a mixed-model capture is exactly where it goes wrong.
void printCameraSetup(Tag tag, const CameraSetup& cs,
                      const CameraSetupOptions& sopt, size_t nimages) {
    std::map<uint32_t, size_t> counts;
    for (uint32_t id : cs.ids) counts[id]++;
    L::err(tag, M::match_camera_mode,
           {cameraModeName(cs.mode_used), (long long)cs.count(), (long long)nimages});
    if (cs.mode_switched)
        L::err(tag, M::match_camera_mode_switched,
               {(long long)cs.dim_buckets, (long long)nimages});
    if (cs.size_split_groups)
        L::warn(tag, M::match_camera_size_split, {(long long)cs.size_split_groups});
    if (cs.exif_focal_images)
        L::err(tag, sopt.exif_focal ? M::match_exif_focals : M::match_exif_focals_ignored,
               {(long long)cs.exif_focal_images, (long long)nimages});
    // Capped: --camera-mode image on an internet collection makes one camera
    // per image, and a thousand lines of stderr helps nobody.
    const size_t kMaxLines = 8;
    size_t shown = 0;
    for (const auto& kv : cs.cameras) {
        if (shown++ >= kMaxLines) {
            L::err(tag, M::match_more_cameras, {(long long)(cs.cameras.size() - kMaxLines)});
            break;
        }
        const Camera& c = kv.second;
        L::err(tag, M::match_camera_line,
               {(long long)kv.first, (long long)counts[kv.first], (long long)c.width,
                (long long)c.height, camInfo(c.model).cli_name, c.focal(),
                (cs.focal_known.count(kv.first)   ? M::focal_prior
                 : cs.focal_given.count(kv.first) ? M::focal_given
                                                  : M::focal_guessed).get()});
    }
}


// Warn once per distinct (mask, image) size pair that disagrees in *aspect*.
// Differing size is fine -- masks are sampled in uv (D39) -- but a differing
// aspect means a mask cut for another crop, stretched over the wrong content.
void checkMaskShape(const std::string& mask_path, const Mask& m,
                    const std::pair<int, int>& img_dims) {
    static std::set<std::array<int, 4>> warned;
    if (m.empty() || img_dims.first <= 0 || img_dims.second <= 0) return;
    const double am = (double)m.width / m.height;
    const double ai = (double)img_dims.first / img_dims.second;
    if (std::fabs(am - ai) <= 0.01 * ai) return;
    if (!warned.insert({m.width, m.height, img_dims.first, img_dims.second}).second) return;
    L::warn(Tag::Extract, M::extract_mask_aspect,
            {mask_path, (long long)m.width, (long long)m.height,
             (long long)img_dims.first, (long long)img_dims.second});
}

// A mask that drops nearly every keypoint is more often inverted than meant:
// ours is white = keep, some tools export the excluded region instead. A
// warning, not a flip -- an object-centric capture masks away all but the
// object, and only the user knows which they have.
void warnIfMasksLookInverted(const ExtractStats& st) {
    // Over the images this run extracted: a resumed one cannot know what a
    // mask took out of a feature file somebody else wrote.
    const uint64_t before = st.features_new + st.masked_out;
    if (!st.masked_images || before == 0) return;
    const double dropped = (double)st.masked_out / (double)before;
    if (dropped < 0.7) return;
    L::warn(Tag::Extract, M::extract_masks_look_inverted, {(long long)(100.0 * dropped)});
}

namespace {

std::string stableFileDigest(const fs::path& path) {
    std::error_code ec;
    const uint64_t before_size = fs::file_size(path, ec);
    if (ec) throw std::runtime_error("cannot stat " + path.string());
    const auto before_time = fs::last_write_time(path, ec);
    if (ec) throw std::runtime_error("cannot stat " + path.string());
    const std::string digest = feature_work::sha256File(path.string());
    const uint64_t after_size = fs::file_size(path, ec);
    if (ec) throw std::runtime_error("cannot stat " + path.string());
    const auto after_time = fs::last_write_time(path, ec);
    if (ec || before_size != after_size || before_time != after_time)
        throw std::runtime_error("input changed while hashing " + path.string());
    if (digest.empty()) throw std::runtime_error("cannot hash " + path.string());
    return digest;
}

std::string asciiFold(std::string value) {
    for (char& c : value)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return value;
}

std::string digestRows(const std::vector<std::string>& rows) {
    std::string bytes;
    for (const std::string& row : rows) {
        bytes += std::to_string(row.size());
        bytes += ':';
        bytes += row;
    }
    return feature_work::sha256Text(bytes);
}

struct PlanUnit {
    std::vector<uint32_t> images;
    uint64_t cost = 0;
    std::string capture;
};

std::vector<PlanUnit> planUnits(const feature_work::FeaturePlan& plan,
                                const std::vector<uint64_t>& costs,
                                const SfmConfig& cfg) {
    std::vector<std::string> stems;
    stems.reserve(plan.images.size());
    for (const auto& image : plan.images) {
        fs::path p(image.feature_path);
        p.replace_extension();
        stems.push_back(p.generic_string());
    }
    const RigTable rigs = buildRigTable(stems, cfg.rigs);
    std::vector<bool> used(plan.images.size(), false);
    std::vector<PlanUnit> units;
    for (const RigSpec& rig : rigs.rigs)
        for (const auto& frame : rig.frames) {
            PlanUnit unit;
            for (uint32_t image : frame)
                if (image != kNoImage) {
                    if (used[image])
                        throw std::runtime_error("an image belongs to more than one rig frame");
                    used[image] = true;
                    unit.images.push_back(image);
                    unit.cost += costs[image];
                }
            if (!unit.images.empty()) units.push_back(std::move(unit));
        }
    for (uint32_t image = 0; image < plan.images.size(); ++image)
        if (!used[image]) units.push_back({{image}, costs[image], {}});
    for (PlanUnit& unit : units) {
        std::sort(unit.images.begin(), unit.images.end());
        const std::string& name = plan.images[unit.images.front()].logical_name;
        const size_t slash = name.find('/');
        unit.capture = slash == std::string::npos ? "." : name.substr(0, slash);
    }
    std::sort(units.begin(), units.end(), [](const PlanUnit& a, const PlanUnit& b) {
        return a.images.front() < b.images.front();
    });
    return units;
}

void addChunk(feature_work::FeaturePlan& plan, const std::string& id,
              std::vector<uint32_t> images, std::set<std::string>& ids) {
    if (id.empty() || id.size() > 128 || !ids.insert(id).second)
        throw std::runtime_error("feature plan has an empty, duplicate or oversized chunk id");
    std::sort(images.begin(), images.end());
    if (std::adjacent_find(images.begin(), images.end()) != images.end())
        throw std::runtime_error("feature plan chunk '" + id + "' repeats an image");
    for (uint32_t image : images) {
        if (image >= plan.images.size())
            throw std::runtime_error("feature plan chunk '" + id + "' has an invalid image");
        plan.images[image].chunks.push_back(id);
    }
    plan.chunks.push_back({id, std::move(images)});
}

void readExplicitChunks(feature_work::FeaturePlan& plan, const std::string& path,
                        const std::vector<PlanUnit>& units) {
    std::error_code ec;
    if (fs::file_size(path, ec) > (64u << 20) || ec)
        throw std::runtime_error("cannot read chunk memberships " + path);
    const JsonValue root = json_parse_file(path);
    const JsonValue* chunks = root.is_array() ? &root : root.find("chunks");
    if (!chunks || !chunks->is_array())
        throw std::runtime_error("chunk memberships must be an array or an object with \"chunks\"");
    std::map<std::string, uint32_t> by_name;
    for (const auto& image : plan.images)
        by_name.emplace(image.logical_name, image.global_index);
    std::set<std::string> ids;
    for (const JsonValue& item : chunks->arr) {
        if (!item.is_object()) throw std::runtime_error("each chunk must be an object");
        const JsonValue* id = item.find("id");
        const JsonValue* images = item.find("images");
        if (!id || id->type != JsonValue::Type::String ||
            !images || !images->is_array())
            throw std::runtime_error("each chunk needs string \"id\" and array \"images\"");
        std::vector<uint32_t> indices;
        for (const JsonValue& image : images->arr) {
            if (image.type != JsonValue::Type::String)
                throw std::runtime_error("chunk image names must be strings");
            const std::string name = feature_work::normalizeRelativePath(image.str);
            const auto found = by_name.find(name);
            if (found == by_name.end())
                throw std::runtime_error("chunk '" + id->str + "' names unknown image " + name);
            indices.push_back(found->second);
        }
        std::set<uint32_t> members(indices.begin(), indices.end());
        for (const PlanUnit& unit : units) {
            size_t included = 0;
            for (uint32_t image : unit.images) included += members.count(image);
            if (included && included != unit.images.size())
                throw std::runtime_error("chunk '" + id->str + "' splits a rig frame");
        }
        addChunk(plan, id->str, std::move(indices), ids);
    }
}

void makeWindowChunks(feature_work::FeaturePlan& plan,
                      const FeaturePlanOptions& options,
                      const std::vector<PlanUnit>& units) {
    if (!options.chunk_window) return;
    if (options.chunk_overlap >= options.chunk_window)
        throw std::runtime_error("--chunk-overlap must be smaller than --chunk-window");
    std::map<std::string, std::vector<const PlanUnit*>> captures;
    for (const PlanUnit& unit : units) captures[unit.capture].push_back(&unit);
    std::set<std::string> ids;
    size_t ordinal = 0;
    const size_t step = options.chunk_window - options.chunk_overlap;
    for (const auto& capture : captures)
        for (size_t begin = 0; begin < capture.second.size(); begin += step) {
            std::vector<uint32_t> images;
            const size_t end =
                std::min(capture.second.size(), begin + options.chunk_window);
            for (size_t i = begin; i < end; ++i)
                images.insert(images.end(), capture.second[i]->images.begin(),
                              capture.second[i]->images.end());
            char name[32];
            std::snprintf(name, sizeof name, "window-%06zu", ordinal++);
            addChunk(plan, name, std::move(images), ids);
            if (end == capture.second.size()) break;
        }
}

void applyFeatureRecipe(const feature_work::Recipe& recipe, SfmConfig& cfg) {
    static const std::set<std::string> local_paths = {
        "masks", "aliked-model", "loma-detector-model", "loma-descriptor-model"};
    const auto found = recipe.settings.find("extraction_signature");
    if (found == recipe.settings.end())
        throw std::runtime_error("feature recipe has no extraction signature");
    std::set<std::string> seen;
    const std::string& signature = found->second;
    for (size_t begin = 0; begin < signature.size();) {
        const size_t end = signature.find('\n', begin);
        const std::string line =
            signature.substr(begin, end == std::string::npos ? end : end - begin);
        const size_t equals = line.find('=');
        if (equals == std::string::npos || equals == 0)
            throw std::runtime_error("feature recipe has an invalid extraction signature");
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        if (!seen.insert(key).second)
            throw std::runtime_error("feature recipe repeats extraction setting " + key);
        if (!local_paths.count(key)) {
            std::string flag = "--" + key;
            std::string arg = value;
            if (value == "on") arg.clear();
            else if (value == "off") flag = "--no-" + key;
            char program[] = "recipe";
            char* argv[] = {program, flag.data(), arg.data()};
            int index = 1;
            std::set<std::string> parsed;
            std::string error;
            const int argc = arg.empty() ? 2 : 3;
            const FieldResult result =
                setConfigField(cfg, CMD_EXTRACT, flag, argc, argv, index, parsed, error);
            if (result == FieldResult::Unknown)
                throw std::runtime_error("unsupported feature recipe setting " + key);
            if (result == FieldResult::Error)
                throw std::runtime_error(error);
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    std::string error = cfg.finalize(CMD_EXTRACT);
    if (!error.empty()) throw std::runtime_error(error);
}

}  // namespace

feature_work::Recipe makeFeatureRecipe(const SfmConfig& cfg) {
    feature_work::Recipe value;
    value.frontend = cfg.features;
    value.implementation = "spirula-sfm-feature-v1";
    value.settings["max_image_size"] = std::to_string(cfg.max_image_size);
    value.settings["image_gamut"] = cfg.image_gamut;
    value.settings["image_linear"] = cfg.image_is_linear ? "1" : "0";
    value.settings["flip_mask"] = cfg.flip_mask ? "1" : "0";
    value.settings["exif_orientation"] = cfg.exif_orientation;
    if (cfg.features == "sift") {
        value.descriptor = "sift";
        value.descriptor_dim = 128;
        value.descriptor_dtype = (uint32_t)DType::U8;
        if (!cfg.sift.spv_path.empty()) {
            value.shader_digest = stableFileDigest(cfg.sift.spv_path);
        } else {
            size_t words = 0;
            const uint32_t* code = findSpirv("sift", &words);
            if (!code) throw std::runtime_error("sift shader not built into this binary");
            value.shader_digest = feature_work::sha256Text(
                std::string(reinterpret_cast<const char*>(code), words * sizeof(uint32_t)));
        }
    } else if (isAlikedType(cfg.features)) {
        value.descriptor = "aliked";
        value.descriptor_dim = 128;
        value.descriptor_dtype = (uint32_t)DType::F32;
#if SS_HAVE_ALIKED
        value.model_digest =
            stableFileDigest(aliked::resolve_model(cfg.aliked.model));
#else
        throw std::runtime_error("this build has no ALIKED feature extractor");
#endif
    } else if (isLomaType(cfg.features)) {
        value.descriptor = "loma";
        value.descriptor_dim = (uint32_t)lomaDescriptorDim(cfg.features);
        value.descriptor_dtype = (uint32_t)DType::F32;
#if SS_HAVE_LOMA
        const std::string detector = cfg.loma.detector_model.empty()
                                         ? "loma-dad"
                                         : cfg.loma.detector_model;
        const std::string descriptor =
            cfg.loma.descriptor_model.empty()
                ? loma::descriptor_for_matcher(cfg.features)
                : cfg.loma.descriptor_model;
        const std::string detector_hash =
            stableFileDigest(loma::resolve_model(detector, "detector"));
        const std::string descriptor_hash =
            stableFileDigest(loma::resolve_model(descriptor, "descriptor"));
        value.model_digest =
            feature_work::sha256Text(detector_hash + ":" + descriptor_hash);
#else
        throw std::runtime_error("this build has no LoMa feature extractor");
#endif
    } else {
        throw std::runtime_error("unknown feature extractor " + cfg.features);
    }
    SfmConfig portable = cfg;
    portable.mask_dir.clear();
    portable.sift.spv_path.clear();
    if (isAlikedType(cfg.features))
        portable.aliked.model = "sha256:" + value.model_digest;
    if (isLomaType(cfg.features)) {
        portable.loma.detector_model.clear();
        portable.loma.descriptor_model = "sha256:" + value.model_digest;
    }
    value.settings["extraction_signature"] =
        stageSignature(portable, CMD_EXTRACT);
    value.digest = feature_work::recipeDigest(value);
    feature_work::validateRecipe(value);
    return value;
}

feature_work::FeaturePlan makeFeaturePlan(const std::string& image_root,
                                         const SfmConfig& cfg,
                                         const FeaturePlanOptions& options) {
    if (!options.shards) throw std::runtime_error("--shards must be positive");
    if (!options.chunk_memberships.empty() && options.chunk_window)
        throw std::runtime_error("explicit chunk memberships and window chunks are exclusive");
    feature_work::FeaturePlan plan;
    plan.extraction = makeFeatureRecipe(cfg);
    std::error_code ec;
    if (!fs::is_directory(image_root, ec))
        throw std::runtime_error("image root is not a directory: " + image_root);
    MaskIndex masks(cfg.mask_dir);
    if (!cfg.mask_dir.empty() && !masks.valid())
        throw std::runtime_error("mask root is not a directory: " + cfg.mask_dir);
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(
             image_root, fs::directory_options::follow_directory_symlink);
         it != fs::recursive_directory_iterator(); ++it)
        if (it->is_regular_file() && isImageExt(it->path().extension().string()) &&
            !isSidecar(it->path()))
            files.push_back(it->path());
    if (files.empty()) throw std::runtime_error("no images under " + image_root);

    struct Pending {
        feature_work::PlanImage image;
        uint64_t cost = 0;
    };
    std::vector<Pending> pending;
    std::map<std::string, std::string> folded_features;
    for (const fs::path& path : files) {
        Pending item;
        item.image.logical_name = feature_work::normalizeRelativePath(
            relativeTo(path, image_root).generic_string());
        item.image.feature_path =
            feature_work::featurePathForImage(item.image.logical_name);
        const std::string folded = asciiFold(item.image.feature_path);
        const auto collision =
            folded_features.emplace(folded, item.image.logical_name);
        if (!collision.second)
            throw std::runtime_error(
                "feature path collision between " + collision.first->second +
                " and " + item.image.logical_name);
        int width = 0, height = 0;
        if (!imageSize(path.string(), width, height) || width <= 0 || height <= 0)
            throw std::runtime_error("cannot decode image header " + path.string());
        const double scale =
            cfg.max_image_size > 0 && std::max(width, height) > cfg.max_image_size
                ? (double)cfg.max_image_size / std::max(width, height)
                : 1.0;
        item.cost = (uint64_t)std::max(
            1.0, std::ceil(width * scale) * std::ceil(height * scale));
        item.image.source_digest = stableFileDigest(path);
        if (masks.valid()) {
            const std::string mask = masks.find(item.image.logical_name);
            if (!mask.empty()) {
                item.image.mask_path = feature_work::normalizeRelativePath(
                    relativeTo(mask, cfg.mask_dir).generic_string());
                item.image.mask_digest = stableFileDigest(mask);
            }
        }
        item.image.artifact_key = feature_work::imageArtifactKey(
            item.image.source_digest, item.image.mask_digest, plan.extraction);
        pending.push_back(std::move(item));
    }
    if (masks.valid() &&
        std::none_of(pending.begin(), pending.end(), [](const Pending& item) {
            return !item.image.mask_path.empty();
        }))
        throw std::runtime_error("mask root matches none of the planned images");
    std::sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) {
        return a.image.feature_path < b.image.feature_path;
    });
    if (options.shards > pending.size())
        throw std::runtime_error("--shards exceeds the number of images");
    std::vector<uint64_t> costs;
    costs.reserve(pending.size());
    for (uint32_t i = 0; i < pending.size(); ++i) {
        pending[i].image.global_index = i;
        costs.push_back(pending[i].cost);
        plan.images.push_back(std::move(pending[i].image));
    }
    const std::vector<PlanUnit> units = planUnits(plan, costs, cfg);
    std::vector<const PlanUnit*> by_cost;
    by_cost.reserve(units.size());
    for (const PlanUnit& unit : units) by_cost.push_back(&unit);
    std::sort(by_cost.begin(), by_cost.end(), [](const PlanUnit* a, const PlanUnit* b) {
        if (a->cost != b->cost) return a->cost > b->cost;
        return a->images.front() < b->images.front();
    });
    std::vector<uint64_t> shard_cost(options.shards, 0);
    for (const PlanUnit* unit : by_cost) {
        const uint32_t shard = (uint32_t)std::distance(
            shard_cost.begin(),
            std::min_element(shard_cost.begin(), shard_cost.end()));
        for (uint32_t image : unit->images) plan.images[image].owner_shard = shard;
        shard_cost[shard] += unit->cost;
    }
    if (!options.chunk_memberships.empty())
        readExplicitChunks(plan, options.chunk_memberships, units);
    else
        makeWindowChunks(plan, options, units);
    for (auto& image : plan.images)
        std::sort(image.chunks.begin(), image.chunks.end());

    plan.dataset_digest = feature_work::datasetSnapshotDigest(plan.images);
    std::vector<std::string> capture_rows;
    for (const PlanUnit& unit : units) {
        std::string row = unit.capture;
        for (uint32_t image : unit.images)
            row += ":" + plan.images[image].logical_name;
        capture_rows.push_back(std::move(row));
    }
    for (const auto& chunk : plan.chunks) capture_rows.push_back(chunk.id);
    plan.capture_digest = digestRows(capture_rows);
    plan.configuration_digest = plan.extraction.digest;
    plan.digest = feature_work::planDigest(plan);
    feature_work::validatePlan(plan);
    return plan;
}

feature_work::FeatureRequest makeFeatureRequest(
    const feature_work::FeaturePlan& plan, uint32_t shard,
    const std::string& attempt_id, const std::string& supersedes) {
    feature_work::FeatureRequest request;
    request.plan_digest = plan.digest;
    request.plan_path = "plan.json";
    request.shard = shard;
    request.attempt_id = attempt_id;
    request.supersedes = supersedes;
    for (const auto& image : plan.images)
        if (image.owner_shard == shard)
            request.image_indices.push_back(image.global_index);
    if (request.image_indices.empty())
        throw std::runtime_error("feature request shard has no images");
    request.digest = feature_work::requestDigest(request);
    feature_work::validateRequest(plan, request);
    return request;
}
void validateFeaturePlanInputs(const feature_work::FeaturePlan& plan,
                               const std::string& image_root,
                               const std::string& mask_root) {
    feature_work::validatePlan(plan);
    for (const auto& image : plan.images) {
        const fs::path source =
            fs::u8path(image_root) / fs::u8path(image.logical_name);
        if (stableFileDigest(source) != image.source_digest)
            throw std::runtime_error("source image changed: " +
                                     image.logical_name);
        if (image.mask_path.empty()) continue;
        if (mask_root.empty())
            throw std::runtime_error("the feature plan requires a mask root");
        const fs::path mask =
            fs::u8path(mask_root) / fs::u8path(image.mask_path);
        if (stableFileDigest(mask) != image.mask_digest)
            throw std::runtime_error("mask changed: " + image.mask_path);
    }
}



namespace {
std::string indexedJson(uint32_t index) {
    char name[32];
    std::snprintf(name, sizeof name, "%08u.json", index);
    return name;
}

fs::path prepareContainedPath(const fs::path& root,
                              const std::string& relative, bool directory) {
    const fs::path target =
        root / fs::u8path(feature_work::normalizeRelativePath(relative));
    std::error_code ec;
    fs::create_directories(directory ? target : target.parent_path(), ec);
    if (ec)
        throw std::runtime_error("cannot prepare artifact path " +
                                 target.string());
    const fs::path canonical_root = fs::weakly_canonical(root, ec);
    if (ec)
        throw std::runtime_error("cannot resolve artifact root " +
                                 root.string());
    const fs::path canonical_target = fs::weakly_canonical(target, ec);
    if (ec)
        throw std::runtime_error("cannot resolve artifact path " +
                                 target.string());
    const fs::path inside = canonical_target.lexically_relative(canonical_root);
    if (inside.empty() || inside.is_absolute() ||
        (!inside.empty() && *inside.begin() == ".."))
        throw std::runtime_error("artifact path escapes its root");
    return target;
}
fs::path existingContainedPath(const fs::path& root, const std::string& relative) {
    const fs::path target =
        root / fs::u8path(feature_work::normalizeRelativePath(relative));
    std::error_code ec;
    const fs::path canonical_root = fs::weakly_canonical(root, ec);
    if (ec)
        throw std::runtime_error("cannot resolve artifact root " + root.string());
    const fs::path canonical_target = fs::weakly_canonical(target, ec);
    if (ec)
        throw std::runtime_error("cannot resolve artifact path " + target.string());
    const fs::path inside = canonical_target.lexically_relative(canonical_root);
    if (inside.empty() || inside.is_absolute() ||
        (!inside.empty() && *inside.begin() == ".."))
        throw std::runtime_error("artifact path escapes its root");
    return target;
}

FeatureSet verifiedPayload(const fs::path& path, const std::string& digest,
                           uint64_t bytes) {
    if (stableFileDigest(path) != digest)
        throw std::runtime_error("feature payload digest differs");
    std::error_code ec;
    if ((uint64_t)fs::file_size(path, ec) != bytes || ec)
        throw std::runtime_error("feature payload size differs");
    return readFeatures(path.string(), false);
}

void verifyReceiptPayload(const fs::path& payload_root,
                          const feature_work::ImageReceipt& receipt,
                          const feature_work::Recipe& recipe) {
    const FeatureSet features = verifiedPayload(
        existingContainedPath(payload_root, receipt.feature_path),
        receipt.payload_digest, receipt.payload_bytes);
    if (features.count() != receipt.feature_count ||
        features.dim != receipt.descriptor_dim ||
        (uint32_t)features.dtype != receipt.descriptor_dtype ||
        features.width != receipt.width || features.height != receipt.height ||
        features.extract_width != receipt.extract_width ||
        features.extract_height != receipt.extract_height ||
        features.dim != recipe.descriptor_dim ||
        (uint32_t)features.dtype != recipe.descriptor_dtype)
        throw std::runtime_error("feature payload metadata differs");
}

void verifyRowPayload(const fs::path& payload_root,
                      const feature_work::AcceptedRow& row,
                      const feature_work::Recipe& recipe) {
    const FeatureSet features = verifiedPayload(
        existingContainedPath(payload_root, row.feature_path),
        row.payload_digest, row.payload_bytes);
    if (features.count() != row.feature_count ||
        features.dim != recipe.descriptor_dim ||
        (uint32_t)features.dtype != recipe.descriptor_dtype)
        throw std::runtime_error("feature payload metadata differs");
}


void copyImmutable(const fs::path& source, const fs::path& destination,
                   const std::string& expected_digest) {
    std::error_code ec;
    if (fs::exists(destination, ec)) {
        if (stableFileDigest(destination) != expected_digest)
            throw std::runtime_error("conflicting artifact " +
                                     destination.string());
        return;
    }
    fs::create_directories(destination.parent_path());
    fs::path temporary = destination;
    temporary += ".tmp";
    fs::remove(temporary, ec);
    if (!fs::copy_file(source, temporary, fs::copy_options::none, ec) || ec) {
        fs::remove(temporary, ec);
        throw std::runtime_error("cannot copy artifact to " +
                                 destination.string());
    }
    if (stableFileDigest(temporary) != expected_digest) {
        fs::remove(temporary, ec);
        throw std::runtime_error("artifact changed while copying " +
                                 source.string());
    }
    fs::rename(temporary, destination, ec);
    if (ec) {
        fs::remove(temporary, ec);
        throw std::runtime_error("cannot publish artifact " +
                                 destination.string());
    }
}


// The feature files as matching indexes them -- name, size and write time --
// folded into one token, since its leftovers are indices into this set. The
// write time is what sees a RE-extracted image: its size is the same budget.
std::string featureDirDigest(const fs::path& featdir) {
    std::vector<std::string> rows;
    std::error_code walk, ec;
    for (auto it = fs::recursive_directory_iterator(featdir, walk);
         !walk && it != fs::recursive_directory_iterator(); it.increment(walk))
        if (it->is_regular_file(ec))
            rows.push_back(
                it->path().generic_string() + ":" +
                std::to_string((uint64_t)fs::file_size(it->path(), ec)) + ":" +
                std::to_string((int64_t)fs::last_write_time(it->path(), ec)
                                   .time_since_epoch()
                                   .count()));
    std::sort(rows.begin(), rows.end());
    uint64_t h = 1469598103934665603ull;   // FNV-1a
    for (const std::string& s : rows)
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llx.%zu", (unsigned long long)h, rows.size());
    return buf;
}

// Anything under `outdir` that is not one of `live`: matching reads every .bin
// in the tree, and one left for an image this run no longer has joins it as a
// phantom view.
void sweepStaleFeatures(const fs::path& outdir, const std::set<fs::path>& live) {
    std::error_code walk, ec;
    std::vector<fs::path> dead;
    for (auto it = fs::recursive_directory_iterator(outdir, walk);
         !walk && it != fs::recursive_directory_iterator(); it.increment(walk))
        if (it->is_regular_file(ec) && !live.count(it->path())) dead.push_back(it->path());
    for (const fs::path& p : dead) fs::remove(p, ec);
}

// Is `feat` a whole feature file that describes `img` as it stands now? An
// mtime comparison, because a re-run that regenerated the frames or the masks
// leaves everything else about the settings identical.
bool featuresAreCurrent(const fs::path& feat, const fs::path& img,
                        const std::string& mask, uint32_t& count) {
    std::error_code fe, ie, me;
    const auto t = fs::last_write_time(feat, fe);
    if (fe || t < fs::last_write_time(img, ie) || ie) return false;
    if (!mask.empty() && t < fs::last_write_time(mask, me) && !me) return false;
    return peekFeatures(feat.string(), count);
}

using FeatureWritten =
    std::function<void(size_t, const FeatureSet&, const GrayImage&)>;
using FeatureFailed = std::function<void(size_t, const std::string&)>;

int extractBatch(const std::string& image_root, const SfmConfig& cfg,
                 std::vector<std::string>& paths,
                 std::vector<std::pair<int, int>>& dims,
                 std::vector<std::string>& mask_paths,
                 std::vector<fs::path>& outputs, size_t total,
                 ExtractStats& stats, const FeatureWritten& written = {},
                 const FeatureFailed& failed = {}) {
    ImageLoadOptions lopt;
    lopt.max_image_size = cfg.max_image_size;
    lopt.num_threads = cfg.decode_threads;
    lopt.want_color = true;
    lopt.gamut = cfg.image_gamut;
    lopt.is_linear = cfg.image_is_linear;
    lopt.flip_mask = cfg.flip_mask;
    lopt.apply_exif_orientation = cfg.exif_orientation == "apply";
    lopt.mask_paths = mask_paths;
    if (cfg.decode_budget_mb > 0)
        lopt.memory_budget_bytes = (size_t)cfg.decode_budget_mb << 20;
    if (paths.empty()) return 0;

    const ImageLoadPlan plan = planImageLoad(dims, lopt);
    if (cfg.sift.verbose)
        L::err(Tag::Extract, M::extract_plan,
               {(long long)paths.size(), (long long)plan.num_threads,
                (long long)plan.window,
                (long long)(((size_t)plan.num_threads * plan.decode_peak_bytes +
                             (size_t)plan.window * plan.held_bytes) >> 20)});
    std::unique_ptr<IFeatureExtractor> ext =
        createFeatureExtractor(cfg.features, cfg.sift, cfg.aliked, cfg.loma);
    if (cfg.sift.verbose)
        L::err(Tag::Extract, M::extract_frontend, {ext->name()});
    loadImagesInOrder(
        paths, plan, lopt,
        [&](size_t k, GrayImage& img) {
            cancel::check();
            if (img.exif_mirror_dropped && !stats.warned_exif_mirror) {
                stats.warned_exif_mirror = true;
                L::warn(Tag::Extract, M::extract_exif_mirror_dropped,
                        {fs::path(paths[k]).filename().string()});
            }
            FeatureSet features = ext->extract(img);
            sampleFeatureColors(features, img);
            uint32_t dropped = 0;
            if (!mask_paths.empty() && !mask_paths[k].empty()) {
                if (img.mask.empty()) {
                    stats.mask_unreadable++;
                    L::warn(Tag::Extract, M::extract_mask_undecodable,
                            {mask_paths[k],
                             fs::path(paths[k]).filename().string()});
                } else {
                    checkMaskShape(mask_paths[k], img.mask,
                                   {img.orig_width, img.orig_height});
                    const uint32_t before = features.count();
                    dropped = applyMask(features, img.mask);
                    stats.masked_out += dropped;
                    if (before && dropped == before && !stats.warned_empty) {
                        stats.warned_empty = true;
                        L::warn(Tag::Extract, M::extract_mask_empty,
                                {mask_paths[k],
                                 fs::path(paths[k]).filename().string()});
                    }
                }
            }
            finishFeatures(features, img);
            fs::create_directories(outputs[k].parent_path());
            writeFeatures(outputs[k].string(), features);
            if (written) written(k, features, img);
            stats.features += features.count();
            stats.features_new += features.count();
            stats.images++;
            Event event;
            event.kind = Event::Kind::ImageExtracted;
            event.stage = Stage::Extract;
            event.done = stats.images;
            event.total = (int64_t)total;
            event.name = fs::path(paths[k]).filename().string();
            event.width = img.orig_width;
            event.height = img.orig_height;
            event.features = features.count();
            event.masked = dropped;
            events::emit(event);
            if (progress::enabled()) {
                fs::path stem = relativeTo(paths[k], image_root);
                stem.replace_extension();
                progress::thumbnail(stem.generic_string(), img.rgb.data(),
                                    img.width, img.height);
            }
        },
        [&](size_t k, const std::string& error) {
            L::fail(Tag::Extract, M::extract_failed_file,
                    {fs::path(paths[k]).filename().string(), error});
            stats.failed++;
            if (failed) failed(k, error);
        });
    return 0;
}

}  // namespace

int extractDirectory(const std::string& imagedir, const fs::path& outdir,
                     const SfmConfig& cfg, ExtractStats& stats, bool reuse) {
    const std::string& maskdir = cfg.mask_dir;
    // Recursive: per-folder intrinsics (ppisp) keep images in images/<camera>/
    // and the folder is the grouping key (D17). A mask directory nested inside
    // is skipped -- masks are PNGs too, and would double the image count with
    // garbage views.
    std::error_code skip_ec;
    const bool skip_masks = !maskdir.empty() && fs::is_directory(maskdir, skip_ec);
    std::vector<fs::path> found;
    for (auto it = fs::recursive_directory_iterator(imagedir, fs::directory_options::follow_directory_symlink);
         it != fs::recursive_directory_iterator(); ++it) {
        if (skip_masks && it->is_directory() && fs::equivalent(it->path(), maskdir, skip_ec)) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file() && isImageExt(it->path().extension().string()) &&
            !isSidecar(it->path()))
            found.push_back(it->path());
    }
    if (found.empty()) {
        L::fail(Tag::Extract, M::extract_no_images, {imagedir});
        return 1;
    }
    std::sort(found.begin(), found.end());

    // Probe every header once (the old comparator re-probed O(n log n) times),
    // both to order the batch and to size the decoder's memory.
    std::vector<fs::path> imgs;
    std::vector<std::pair<int, int>> dims;
    for (const fs::path& p : found) {
        int w = 0, h = 0;
        if (!imageSize(p.string(), w, h) || w <= 0 || h <= 0) {
            L::warn(Tag::Extract, M::extract_skipping_file,
                    {p.filename().string()});
            stats.unreadable++;
            continue;
        }
        imgs.push_back(p);
        dims.emplace_back(w, h);
    }
    if (imgs.empty()) {
        L::fail(Tag::Extract, M::extract_no_decodable, {imagedir});
        return 1;
    }

    // Largest-first so the extractor allocates device buffers exactly once.
    std::vector<size_t> order(imgs.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return (int64_t)dims[a].first * dims[a].second > (int64_t)dims[b].first * dims[b].second;
    });
    std::vector<std::string> paths(order.size());
    std::vector<std::pair<int, int>> sorted_dims(order.size());
    for (size_t k = 0; k < order.size(); k++) {
        paths[k] = imgs[order[k]].string();
        sorted_dims[k] = dims[order[k]];
    }

    fs::create_directories(outdir);

    std::vector<std::string> mask_paths;

    // Resolved up front (D39), in `paths` order, so the decode pool can pick
    // them up -- and so a mask directory matching nothing is reported before
    // the GPU stage burns an hour on unmasked features.
    MaskIndex masks(maskdir);
    if (!maskdir.empty() && !masks.valid())
        L::warn(Tag::Extract, M::extract_mask_dir_missing, {maskdir});
    if (masks.valid()) {
        mask_paths.assign(paths.size(), std::string());
        for (size_t k = 0; k < paths.size(); k++) {
            std::string rel = relativeTo(paths[k], imagedir).generic_string();
            mask_paths[k] = masks.find(rel);
            if (mask_paths[k].empty()) {
                stats.unmasked_images++;
                if (stats.first_unmasked.empty()) stats.first_unmasked = rel;
            } else {
                stats.masked_images++;
            }
        }
        L::out(Tag::Extract, M::extract_masks_matched,
               {(long long)stats.masked_images, (long long)paths.size(), maskdir});
        if (stats.masked_images == 0) {
            L::fail(Tag::Extract, M::extract_no_mask_matches,
                    {maskdir, stats.first_unmasked});
            return 1;
        }
        if (stats.unmasked_images)
            L::warn(Tag::Extract, M::extract_some_unmasked,
                    {(long long)stats.unmasked_images, stats.first_unmasked});
    }

    // Where each image's features belong, and what a previous run already put
    // there. The total the bar counts is the capture, not the work left.
    const size_t n_all = paths.size();
    std::vector<fs::path> outs(n_all);
    std::set<fs::path> live;
    for (size_t k = 0; k < n_all; k++) {
        outs[k] = outdir / relativeTo(paths[k], imagedir);
        outs[k].replace_extension(".bin");
        live.insert(outs[k]);
    }
    sweepStaleFeatures(outdir, live);

    events::stage_begin(Stage::Extract, (int64_t)n_all);
    if (reuse) {
        std::vector<size_t> todo;
        for (size_t k = 0; k < n_all; k++) {
            uint32_t count = 0;
            const std::string mask =
                k < mask_paths.size() ? mask_paths[k] : std::string();
            if (!featuresAreCurrent(outs[k], paths[k], mask, count)) {
                todo.push_back(k);
                continue;
            }
            stats.reused++;
            stats.features += count;
            stats.images++;
            Event ev;
            ev.kind = Event::Kind::ImageExtracted;
            ev.stage = Stage::Extract;
            ev.done = (int64_t)stats.images;
            ev.total = (int64_t)n_all;
            ev.name = fs::path(paths[k]).filename().string();
            ev.width = sorted_dims[k].first;
            ev.height = sorted_dims[k].second;
            ev.features = count;
            events::emit(ev);
        }
        if (stats.reused) {
            L::out(Tag::Extract, M::extract_reusing,
                   {(long long)stats.reused, (long long)n_all});
            for (size_t i = 0; i < todo.size(); i++) {
                const size_t k = todo[i];
                if (k == i) continue;   // self-move empties a std::string
                paths[i] = std::move(paths[k]);
                sorted_dims[i] = sorted_dims[k];
                outs[i] = std::move(outs[k]);
                if (!mask_paths.empty()) mask_paths[i] = std::move(mask_paths[k]);
            }
            paths.resize(todo.size());
            sorted_dims.resize(todo.size());
            outs.resize(todo.size());
            if (!mask_paths.empty()) mask_paths.resize(todo.size());
        }
    }
    if (paths.empty()) {
        events::stage_end(Stage::Extract);
        return 0;
    }

    extractBatch(imagedir, cfg, paths, sorted_dims, mask_paths, outs, n_all,
                 stats);
    events::stage_end(Stage::Extract);
    return 0;
}
int extractFeatureRequest(const std::string& image_root,
                          const std::string& mask_root,
                          const std::string& request_path,
                          const fs::path& output_dir,
                          const SfmConfig& cfg, ExtractStats& stats,
                          const std::string& adopt_from) {
    const fs::path request_file = fs::absolute(fs::u8path(request_path));
    const feature_work::FeatureRequest request =
        feature_work::readRequestFile(request_file.string());
    const fs::path plan_file =
        request_file.parent_path() / fs::u8path(request.plan_path);
    const feature_work::FeaturePlan plan =
        feature_work::readPlanFile(plan_file.string());
    feature_work::validateRequest(plan, request);
    SfmConfig effective = cfg;
    applyFeatureRecipe(plan.extraction, effective);
    if (makeFeatureRecipe(effective).digest != plan.extraction.digest)
        throw std::runtime_error("worker extraction recipe does not match the plan");

    const fs::path root = fs::absolute(output_dir);
    fs::create_directories(root);
    const fs::path payload_root =
        prepareContainedPath(root, "payload", true);
    const fs::path receipt_root =
        prepareContainedPath(root, "receipts", true);

    feature_work::WorkerBinding binding;
    binding.extraction_request = request;
    binding.request_path = "request.json";
    binding.image_root = fs::absolute(fs::u8path(image_root)).string();
    binding.mask_root =
        mask_root.empty() ? std::string()
                          : fs::absolute(fs::u8path(mask_root)).string();
    binding.device = effective.device_selector;
    binding.output_dir = root.string();
    binding.max_images = (uint32_t)request.image_indices.size();
    binding.max_payload_bytes =
        (uint64_t)request.image_indices.size() * (1ull << 30);
    binding.digest = feature_work::workerBindingDigest(binding);
    feature_work::validateWorkerBinding(plan, binding);
    feature_work::writeRequestFile((root / binding.request_path).string(),
                                   request);
    const fs::path binding_path = root / "binding.json";
    if (fs::exists(binding_path)) {
        const auto previous =
            feature_work::readWorkerBindingFile(binding_path.string());
        if (previous.digest != binding.digest)
            throw std::runtime_error("attempt directory belongs to another worker binding");
    } else {
        feature_work::writeWorkerBindingFile(binding_path.string(), binding);
    }

    bool plan_uses_masks = false;
    for (const auto& image : plan.images)
        plan_uses_masks = plan_uses_masks || !image.mask_path.empty();
    if (plan_uses_masks && mask_root.empty())
        throw std::runtime_error("the feature plan requires a mask root");

    struct Reusable {
        feature_work::ImageReceipt receipt;
        fs::path payload_root;
    };
    std::map<std::string, Reusable> reusable;
    std::set<std::string> ambiguous;
    auto samePayload = [](const feature_work::ImageReceipt& a,
                          const feature_work::ImageReceipt& b) {
        return a.payload_digest == b.payload_digest &&
               a.payload_bytes == b.payload_bytes &&
               a.feature_count == b.feature_count &&
               a.descriptor_dtype == b.descriptor_dtype &&
               a.descriptor_dim == b.descriptor_dim &&
               a.width == b.width && a.height == b.height &&
               a.extract_width == b.extract_width &&
               a.extract_height == b.extract_height;
    };
    auto addReusable = [&](const fs::path& payload_root,
                           feature_work::ImageReceipt receipt) {
        if (ambiguous.count(receipt.artifact_key)) return;
        const auto found = reusable.find(receipt.artifact_key);
        if (found == reusable.end()) {
            reusable.emplace(receipt.artifact_key,
                             Reusable{std::move(receipt), payload_root});
            return;
        }
        if (samePayload(found->second.receipt, receipt)) return;
        reusable.erase(found);
        ambiguous.insert(receipt.artifact_key);
    };
    auto loadReusableAttempt = [&](const fs::path& attempt_root) {
        try {
            const feature_work::WorkerBinding candidate =
                feature_work::readWorkerBindingFile(
                    (attempt_root / "binding.json").string());
            const feature_work::FeatureRequest& old_request =
                candidate.extraction_request;
            if (old_request.shard != request.shard ||
                (old_request.digest != request.digest &&
                 old_request.digest != request.supersedes))
                return;
            const feature_work::ShardResult result =
                feature_work::readShardResultFile(
                    (attempt_root / "result.json").string());
            if (!result.complete || result.plan_digest != old_request.plan_digest ||
                result.request_digest != old_request.digest ||
                result.shard != old_request.shard)
                return;
            std::set<uint32_t> expected(old_request.image_indices.begin(),
                                        old_request.image_indices.end());
            std::set<uint32_t> seen;
            for (const feature_work::ImageOutcome& outcome : result.outcomes) {
                if (!expected.count(outcome.global_index) ||
                    !seen.insert(outcome.global_index).second ||
                    !feature_work::successful(outcome.status))
                    return;
            }
            if (seen != expected) return;
            for (const feature_work::ImageOutcome& outcome : result.outcomes) {
                try {
                    feature_work::ImageReceipt receipt =
                        feature_work::readReceiptFile(
                            (attempt_root / "receipts" /
                             indexedJson(outcome.global_index)).string());
                    if (receipt.request_digest != old_request.digest ||
                        receipt.global_index != outcome.global_index ||
                        receipt.digest != outcome.receipt_digest)
                        continue;
                    verifyReceiptPayload(attempt_root / "payload", receipt,
                                         plan.extraction);
                    addReusable(attempt_root / "payload", std::move(receipt));
                } catch (const std::exception&) {
                }
            }
        } catch (const std::exception&) {
        }
    };
    loadReusableAttempt(root);
    if (!adopt_from.empty())
        loadReusableAttempt(fs::absolute(fs::u8path(adopt_from)));

    std::map<uint32_t, feature_work::ImageOutcome> outcomes;
    auto accept_receipt = [&](feature_work::ImageReceipt receipt, bool reused) {
        feature_work::validateReceipt(plan, request, receipt,
                                      payload_root.string());
        feature_work::ImageOutcome outcome;
        outcome.global_index = receipt.global_index;
        outcome.status = receipt.feature_count
                             ? feature_work::outcome_status::success
                             : feature_work::outcome_status::zero_features;
        outcome.receipt_digest = receipt.digest;
        outcome.digest = feature_work::outcomeDigest(outcome);
        outcomes[receipt.global_index] = std::move(outcome);
        stats.images++;
        stats.features += receipt.feature_count;
        stats.reused += reused ? 1 : 0;
    };

    struct WorkImage {
        uint32_t global = 0;
        std::string source;
        std::string mask;
        fs::path output;
        std::pair<int, int> dims;
    };
    std::vector<WorkImage> work;
    for (uint32_t index : request.image_indices) {
        try {
        const feature_work::PlanImage& image = plan.images.at(index);
        const fs::path source =
            fs::u8path(binding.image_root) / fs::u8path(image.logical_name);
        if (stableFileDigest(source) != image.source_digest)
            throw std::runtime_error("source image changed: " +
                                     image.logical_name);
        std::string mask;
        if (!image.mask_path.empty()) {
            const fs::path path =
                fs::u8path(binding.mask_root) / fs::u8path(image.mask_path);
            if (stableFileDigest(path) != image.mask_digest)
                throw std::runtime_error("mask changed: " + image.mask_path);
            mask = path.string();
        }
        if (plan_uses_masks) {
            if (mask.empty()) {
                stats.unmasked_images++;
                if (stats.first_unmasked.empty())
                    stats.first_unmasked = image.logical_name;
            } else {
                stats.masked_images++;
            }
        }

        const fs::path current_receipt = prepareContainedPath(
            root, "receipts/" + indexedJson(index), false);
        const auto reusable_image = reusable.find(image.artifact_key);
        if (reusable_image != reusable.end()) {
            feature_work::ImageReceipt receipt = reusable_image->second.receipt;
            const fs::path source_payload = existingContainedPath(
                reusable_image->second.payload_root, receipt.feature_path);
            copyImmutable(
                source_payload,
                prepareContainedPath(root, "payload/" + image.feature_path, false),
                receipt.payload_digest);
            receipt.request_digest = request.digest;
            receipt.global_index = image.global_index;
            receipt.logical_name = image.logical_name;
            receipt.feature_path = image.feature_path;
            receipt.artifact_key = image.artifact_key;
            receipt.digest = feature_work::receiptDigest(receipt);
            feature_work::writeReceiptFile(current_receipt.string(), receipt);
            accept_receipt(std::move(receipt), true);
            continue;
        }
        int width = 0, height = 0;
        if (!imageSize(source.string(), width, height) || width <= 0 || height <= 0)
            throw std::runtime_error("cannot decode image header " +
                                     image.logical_name);
        work.push_back({index, source.string(), mask,
                        prepareContainedPath(
                            root, "payload/" + image.feature_path, false),
                        {width, height}});
        } catch (const std::exception& error) {
            feature_work::ImageOutcome outcome;
            outcome.global_index = index;
            outcome.status = feature_work::outcome_status::failed;
            outcome.error = error.what();
            outcome.digest = feature_work::outcomeDigest(outcome);
            outcomes[index] = std::move(outcome);
        }
    }

    std::sort(work.begin(), work.end(), [](const WorkImage& a,
                                           const WorkImage& b) {
        const int64_t ap = (int64_t)a.dims.first * a.dims.second;
        const int64_t bp = (int64_t)b.dims.first * b.dims.second;
        if (ap != bp) return ap > bp;
        return a.global < b.global;
    });
    std::vector<std::string> paths, masks;
    std::vector<std::pair<int, int>> dims;
    std::vector<fs::path> outputs;
    paths.reserve(work.size());
    masks.reserve(work.size());
    dims.reserve(work.size());
    outputs.reserve(work.size());
    for (const WorkImage& image : work) {
        paths.push_back(image.source);
        masks.push_back(image.mask);
        dims.push_back(image.dims);
        outputs.push_back(image.output);
    }

    events::stage_begin(Stage::Extract, (int64_t)request.image_indices.size());
    extractBatch(
        binding.image_root, effective, paths, dims, masks, outputs,
        request.image_indices.size(), stats,
        [&](size_t local, const FeatureSet& features, const GrayImage& image) {
            const uint32_t index = work[local].global;
            const auto& planned = plan.images[index];
            feature_work::ImageReceipt receipt;
            receipt.request_digest = request.digest;
            receipt.global_index = index;
            receipt.logical_name = planned.logical_name;
            receipt.feature_path = planned.feature_path;
            receipt.artifact_key = planned.artifact_key;
            receipt.payload_digest = stableFileDigest(outputs[local]);
            receipt.payload_bytes = fs::file_size(outputs[local]);
            receipt.feature_count = features.count();
            receipt.descriptor_dtype = (uint32_t)features.dtype;
            receipt.descriptor_dim = features.dim;
            receipt.width = features.width;
            receipt.height = features.height;
            receipt.extract_width = image.width;
            receipt.extract_height = image.height;
            receipt.producer_build = plan.extraction.implementation;
            receipt.producer_device = effective.device_selector;
            receipt.digest = feature_work::receiptDigest(receipt);
            feature_work::validateReceipt(plan, request, receipt,
                                          payload_root.string());
            feature_work::writeReceiptFile(
                prepareContainedPath(root, "receipts/" + indexedJson(index),
                                     false).string(),
                receipt);
            feature_work::ImageOutcome outcome;
            outcome.global_index = index;
            outcome.status = receipt.feature_count
                                 ? feature_work::outcome_status::success
                                 : feature_work::outcome_status::zero_features;
            outcome.receipt_digest = receipt.digest;
            outcome.digest = feature_work::outcomeDigest(outcome);
            outcomes[index] = std::move(outcome);
        },
        [&](size_t local, const std::string& error) {
            feature_work::ImageOutcome outcome;
            outcome.global_index = work[local].global;
            outcome.status = feature_work::outcome_status::failed;
            outcome.error = error;
            outcome.digest = feature_work::outcomeDigest(outcome);
            outcomes[outcome.global_index] = std::move(outcome);
        });
    events::stage_end(Stage::Extract);

    feature_work::ShardResult result;
    result.plan_digest = plan.digest;
    result.request_digest = request.digest;
    result.shard = request.shard;
    result.complete = true;
    for (uint32_t index : request.image_indices) {
        auto found = outcomes.find(index);
        if (found == outcomes.end()) {
            feature_work::ImageOutcome outcome;
            outcome.global_index = index;
            outcome.status = feature_work::outcome_status::failed;
            outcome.error = "feature extraction produced no receipt";
            outcome.digest = feature_work::outcomeDigest(outcome);
            found = outcomes.emplace(index, std::move(outcome)).first;
        }
        result.complete =
            result.complete && feature_work::successful(found->second.status);
        result.outcomes.push_back(found->second);
    }
    result.digest = feature_work::shardResultDigest(result);
    feature_work::validateShardResult(plan, request, result);
    feature_work::writeShardResultFile((root / "result.json").string(),
                                       result);
    return result.complete ? 0 : 1;
}




static int loadFeatureFiles(const std::vector<fs::path>& files,
                            const std::vector<std::string>& names,
                            const SfmConfig& cfg, bool with_descriptors,
                            std::vector<FeatureSet>& feats,
                            MatchesDatabase& db) {
    if (files.size() < 2) {
        L::fail(Tag::Match, M::match_need_two,
                {files.empty() ? std::string() : files.front().parent_path().string()});
        return 1;
    }
    feats.assign(files.size(), FeatureSet());
    db.images.resize(files.size());
    events::stage_begin(Stage::Load, (int64_t)files.size());
    const unsigned concurrency = std::thread::hardware_concurrency();
    int threads = cfg.threads > 0 ? cfg.threads
                                  : (concurrency > 0 ? (int)concurrency : 1);
    threads = std::max(1, std::min<int>(threads, (int)files.size()));
    std::atomic<size_t> next{0}, done{0};
    std::mutex error_mutex;
    std::string first_error;
    std::vector<std::thread> pool;
    const size_t step = std::max<size_t>(1, files.size() / 200);
    for (int thread = 0; thread < threads; ++thread)
        pool.emplace_back([&] {
            for (size_t i = next++; i < files.size(); i = next++) {
                try {
                    feats[i] = readFeatures(files[i].string(), with_descriptors);
                } catch (const std::exception& error) {
                    std::lock_guard<std::mutex> lock(error_mutex);
                    if (first_error.empty()) first_error = error.what();
                }
                const size_t count = ++done;
                if (count % step == 0 || count == files.size())
                    events::progress(Stage::Load, (int64_t)count,
                                     (int64_t)files.size());
            }
        });
    for (std::thread& thread : pool) thread.join();
    events::stage_end(Stage::Load);
    if (!first_error.empty()) {
        L::err_raw(Tag::Match, first_error);
        return 1;
    }
    for (size_t i = 0; i < files.size(); ++i)
        db.images[i] = {names[i], feats[i].count()};
    return 0;
}

int loadFeatureDir(const std::string& featdir, const SfmConfig& cfg,
                   bool with_descriptors, std::vector<FeatureSet>& feats,
                   MatchesDatabase& db) {
    std::vector<fs::path> files;
    std::error_code walk, ec;
    for (auto it = fs::recursive_directory_iterator(featdir, walk);
         !walk && it != fs::recursive_directory_iterator(); it.increment(walk))
        if (it->is_regular_file(ec) && it->path().extension() == ".bin" &&
            !isSidecar(it->path()))
            files.push_back(it->path());
    std::sort(files.begin(), files.end());
    std::vector<std::string> names;
    names.reserve(files.size());
    for (const fs::path& file : files) {
        fs::path relative = relativeTo(file, featdir);
        relative.replace_extension();
        names.push_back(relative.generic_string());
    }
    return loadFeatureFiles(files, names, cfg, with_descriptors, feats, db);
}
void validateFeatureResult(const std::string& request_path,
                           const fs::path& result_root) {
    const fs::path request_file = fs::absolute(fs::u8path(request_path));
    const auto request =
        feature_work::readRequestFile(request_file.string());
    const auto plan = feature_work::readPlanFile(
        (request_file.parent_path() / fs::u8path(request.plan_path)).string());
    feature_work::validateRequest(plan, request);
    const fs::path root = fs::absolute(result_root);
    const auto binding = feature_work::readWorkerBindingFile(
        (root / "binding.json").string());
    feature_work::validateWorkerBinding(plan, binding);
    if (binding.extraction_request.digest != request.digest)
        throw std::runtime_error("worker binding names another request");
    const auto result = feature_work::readShardResultFile(
        (root / "result.json").string());
    feature_work::validateShardResult(plan, request, result);
    if (!result.complete)
        throw std::runtime_error("feature result is incomplete");
    for (const auto& outcome : result.outcomes) {
        if (!feature_work::successful(outcome.status))
            throw std::runtime_error("feature result contains a failed image");
        const auto receipt = feature_work::readReceiptFile(
            (root / "receipts" / indexedJson(outcome.global_index)).string());
        feature_work::validateReceipt(plan, request, receipt,
                                      (root / "payload").string());
        if (receipt.digest != outcome.receipt_digest)
            throw std::runtime_error("result and receipt digests disagree");
    }
}

feature_work::CollectionIndex collectFeatureResults(
    const feature_work::FeaturePlan& plan,
    const std::vector<std::string>& request_paths,
    const std::vector<std::string>& cancellation_paths,
    const std::vector<std::string>& result_roots,
    const fs::path& feature_dir,
    const std::vector<fs::path>& adopt_from) {
    feature_work::validatePlan(plan);
    std::map<std::string, feature_work::FeatureRequest> requests;
    std::map<std::string, size_t> children;
    std::map<uint32_t, size_t> roots;
    for (const std::string& path : request_paths) {
        feature_work::FeatureRequest request =
            feature_work::readRequestFile(path);
        feature_work::validateRequest(plan, request);
        const auto inserted = requests.emplace(request.digest, request);
        if (!inserted.second) {
            if (feature_work::writeRequest(inserted.first->second) !=
                feature_work::writeRequest(request))
                throw std::runtime_error("conflicting feature request " +
                                         request.digest);
            continue;
        }
        if (request.supersedes.empty())
            roots[request.shard]++;
        else
            children[request.supersedes]++;
    }
    if (requests.empty()) throw std::runtime_error("no feature requests supplied");
    for (const auto& entry : requests) {
        const auto& request = entry.second;
        if (request.supersedes.empty()) continue;
        const auto previous = requests.find(request.supersedes);
        if (previous == requests.end() ||
            previous->second.shard != request.shard)
            throw std::runtime_error("feature request has a missing or cross-shard predecessor");
        if (children[request.supersedes] != 1)
            throw std::runtime_error("feature request supersession fork");
    }
    for (const auto& entry : roots)
        if (entry.second != 1)
            throw std::runtime_error("feature request shard has multiple roots");

    std::map<std::string, feature_work::FeatureCancellation> cancellations;
    for (const std::string& path : cancellation_paths) {
        feature_work::FeatureCancellation cancellation =
            feature_work::readCancellationFile(path);
        const auto request = requests.find(cancellation.request_digest);
        if (request == requests.end())
            throw std::runtime_error("cancellation names an unknown request");
        feature_work::validateCancellation(plan, request->second, cancellation);
        if (!cancellations.emplace(cancellation.request_digest,
                                   cancellation).second)
            throw std::runtime_error("duplicate cancellation for a request");
    }

    std::map<uint32_t, feature_work::FeatureRequest> active;
    for (const auto& entry : requests) {
        const auto& request = entry.second;
        if (children.count(request.digest) || cancellations.count(request.digest))
            continue;
        if (!active.emplace(request.shard, request).second)
            throw std::runtime_error("feature request shard has multiple active attempts");
    }
    std::set<uint32_t> expected_shards;
    for (const auto& image : plan.images)
        expected_shards.insert(image.owner_shard);
    for (uint32_t shard : expected_shards)
        if (!active.count(shard))
            throw std::runtime_error("feature request shard has no active attempt");

    struct Accepted {
        feature_work::AcceptedRow row;
        fs::path payload;
    };
    std::map<uint32_t, Accepted> accepted;
    std::map<std::string, std::vector<const feature_work::PlanImage*>> planned_by_key;
    for (const auto& image : plan.images)
        planned_by_key[image.artifact_key].push_back(&image);
    auto acceptAdopted = [&](feature_work::AcceptedRow row, const fs::path& payload) {
        const auto inserted = accepted.emplace(row.global_index, Accepted{row, payload});
        if (!inserted.second &&
            (inserted.first->second.row.artifact_key != row.artifact_key ||
             inserted.first->second.row.payload_digest != row.payload_digest ||
             inserted.first->second.row.payload_bytes != row.payload_bytes ||
             inserted.first->second.row.feature_count != row.feature_count))
            throw std::runtime_error("adoption sources conflict for an image");
    };
    for (const fs::path& source_path : adopt_from) {
        const fs::path source = fs::absolute(source_path);
        const auto old_plan = feature_work::readPlanFile(
            (source / "plan.json").string());
        const auto old_index = feature_work::readCollectionIndexFile(
            (source / "index.json").string());
        feature_work::validateCollection(old_plan, old_index);
        if (!old_index.complete)
            throw std::runtime_error("adoption source is not a complete collection");
        for (const auto& old_row : old_index.rows) {
            const auto target = planned_by_key.find(old_row.artifact_key);
            if (target == planned_by_key.end()) continue;
            verifyRowPayload(source, old_row, old_plan.extraction);
            const fs::path payload =
                existingContainedPath(source, old_row.feature_path);
            for (const feature_work::PlanImage* image : target->second) {
                feature_work::AcceptedRow row = old_row;
                row.global_index = image->global_index;
                row.logical_name = image->logical_name;
                row.feature_path = image->feature_path;
                row.artifact_key = image->artifact_key;
                acceptAdopted(std::move(row), payload);
            }
        }
    }
    std::map<std::string, std::string> active_results;
    for (const std::string& result_root_string : result_roots) {
        const fs::path result_root = fs::absolute(fs::u8path(result_root_string));
        const feature_work::WorkerBinding binding =
            feature_work::readWorkerBindingFile(
                (result_root / "binding.json").string());
        const auto request = requests.find(binding.extraction_request.digest);
        if (request == requests.end())
            throw std::runtime_error("worker result names an unknown request");
        feature_work::validateWorkerBinding(plan, binding);
        const auto current = active.find(request->second.shard);
        if (current == active.end() ||
            current->second.digest != request->second.digest)
            continue;
        const feature_work::ShardResult result =
            feature_work::readShardResultFile(
                (result_root / "result.json").string());
        feature_work::validateShardResult(plan, request->second, result);
        const auto result_entry =
            active_results.emplace(result.request_digest, result.digest);
        if (!result_entry.second) {
            if (result_entry.first->second != result.digest)
                throw std::runtime_error(
                    "conflicting results for one active request");
            continue;
        }
        for (const auto& outcome : result.outcomes) {
            if (!feature_work::successful(outcome.status)) continue;
            const fs::path receipt_path =
                result_root / "receipts" / indexedJson(outcome.global_index);
            const feature_work::ImageReceipt receipt =
                feature_work::readReceiptFile(receipt_path.string());
            feature_work::validateReceipt(plan, request->second, receipt,
                                          (result_root / "payload").string());
            if (receipt.digest != outcome.receipt_digest)
                throw std::runtime_error("result and receipt digests disagree");
            feature_work::AcceptedRow row;
            row.global_index = receipt.global_index;
            row.logical_name = receipt.logical_name;
            row.feature_path = receipt.feature_path;
            row.artifact_key = receipt.artifact_key;
            row.payload_digest = receipt.payload_digest;
            row.payload_bytes = receipt.payload_bytes;
            row.feature_count = receipt.feature_count;
            row.receipt_digest = receipt.digest;
            row.result_digest = result.digest;
            const auto inserted = accepted.emplace(
                row.global_index,
                Accepted{row, result_root / "payload" /
                                  fs::u8path(row.feature_path)});
            if (!inserted.second &&
                (inserted.first->second.row.artifact_key != row.artifact_key ||
                 inserted.first->second.row.payload_digest != row.payload_digest ||
                 inserted.first->second.row.payload_bytes != row.payload_bytes ||
                 inserted.first->second.row.feature_count != row.feature_count))
                throw std::runtime_error("more than one payload produced an image");
        }
    }

    const fs::path index_path = feature_dir / "index.json";
    if (fs::exists(feature_dir)) {
        if (!fs::exists(index_path))
            throw std::runtime_error("feature destination exists but is not a sealed collection");
        feature_work::CollectionIndex existing =
            feature_work::readCollectionIndexFile(index_path.string());
        feature_work::validateCollection(plan, existing);
        for (const auto& row : existing.rows)
            verifyRowPayload(feature_dir, row, plan.extraction);
        return existing;
    }

    fs::path staging =
        fs::u8path(feature_dir.string() + ".import-" + plan.digest.substr(0, 12));
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging);
    for (const auto& entry : accepted)
        copyImmutable(entry.second.payload,
                      staging / fs::u8path(entry.second.row.feature_path),
                      entry.second.row.payload_digest);
    feature_work::CollectionIndex index;
    index.plan_digest = plan.digest;
    index.complete = accepted.size() == plan.images.size();
    for (const auto& entry : accepted)
        index.rows.push_back(entry.second.row);
    index.collection_digest = feature_work::collectionDigest(index);
    feature_work::validateCollection(plan, index);
    if (!index.complete) {
        feature_work::writeCollectionIndexFile(
            (staging / "incomplete.json").string(), index);
        return feature_work::readCollectionIndexFile(
            (staging / "incomplete.json").string());
    }
    feature_work::writePlanFile((staging / "plan.json").string(), plan);
    feature_work::writeCollectionIndexFile((staging / "index.json").string(),
                                           index);
    fs::create_directories(feature_dir.parent_path());
    fs::rename(staging, feature_dir, ec);
    if (ec)
        throw std::runtime_error("cannot seal feature collection: " +
                                 ec.message());
    return feature_work::readCollectionIndexFile(
        (feature_dir / "index.json").string());
}

int loadFeatureCollection(const fs::path& feature_dir,
                          const feature_work::FeaturePlan& plan,
                          const feature_work::CollectionIndex& index,
                          const SfmConfig& cfg, bool with_descriptors,
                          std::vector<FeatureSet>& feats,
                          MatchesDatabase& db) {
    feature_work::validatePlan(plan);
    feature_work::validateCollection(plan, index);
    if (!index.complete || index.rows.size() != plan.images.size())
        throw std::runtime_error("feature collection is not sealed");
    std::vector<fs::path> files;
    std::vector<std::string> names;
    files.reserve(index.rows.size());
    names.reserve(index.rows.size());
    for (size_t i = 0; i < index.rows.size(); ++i) {
        const auto& row = index.rows[i];
        const auto& image = plan.images[i];
        if (row.global_index != i || row.logical_name != image.logical_name ||
            row.feature_path != image.feature_path ||
            row.artifact_key != image.artifact_key)
            throw std::runtime_error("feature collection order does not match the plan");
        const fs::path file = feature_dir / fs::u8path(row.feature_path);
        if (stableFileDigest(file) != row.payload_digest)
            throw std::runtime_error("feature collection payload changed: " +
                                     row.feature_path);
        files.push_back(file);
        fs::path stem(row.feature_path);
        stem.replace_extension();
        names.push_back(stem.generic_string());
    }
    const int rc =
        loadFeatureFiles(files, names, cfg, with_descriptors, feats, db);
    if (rc) return rc;
    for (size_t i = 0; i < feats.size(); ++i) {
        if (feats[i].count() != index.rows[i].feature_count ||
            feats[i].dim != plan.extraction.descriptor_dim ||
            (uint32_t)feats[i].dtype != plan.extraction.descriptor_dtype)
            throw std::runtime_error("feature payload metadata disagrees with collection index");
    }
    return 0;
}


namespace {

std::string matchRecipeSignature(const SfmConfig& cfg) {
    const std::string full = stageSignature(cfg, CMD_MATCH);
    std::string filtered;
    for (size_t begin = 0; begin < full.size();) {
        const size_t end = full.find('\n', begin);
        const std::string line =
            full.substr(begin, end == std::string::npos ? end : end - begin);
        if (line.rfind("pairs=", 0) != 0 && line.rfind("overlap=", 0) != 0 &&
            line.rfind("loop-closure=", 0) != 0 &&
            line.rfind("prefilter-features=", 0) != 0 &&
            line.rfind("prefilter-train=", 0) != 0 &&
            line.rfind("prefilter-neighbors=", 0) != 0 &&
            line.rfind("prefilter-min-score=", 0) != 0 &&
            line.rfind("prefilter-ratio=", 0) != 0)
            filtered += line + "\n";
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return "match-journal-v2\n" + filtered;
}

struct FeatureProductIdentity {
    std::string key;
    std::string digest;
};

std::vector<FeatureProductIdentity> featureProducts(
    const fs::path& featdir, const std::vector<FeatureSet>& feats,
    const MatchesDatabase& db,
    const feature_work::FeaturePlan* plan,
    const feature_work::CollectionIndex* index,
    const std::string& recipe_digest) {
    std::vector<FeatureProductIdentity> products;
    products.reserve(feats.size());
    for (size_t i = 0; i < feats.size(); ++i) {
        std::string digest;
        std::string key;
        if (plan && index) {
            const auto& row = index->rows.at(i);
            digest = row.payload_digest;
            key = row.artifact_key;
        } else {
            fs::path path = fs::u8path(db.images.at(i).name);
            path += ".bin";
            digest = stableFileDigest(featdir / path);
            key = feature_work::sha256Text(
                "sfm-local-feature-v2\n" + digest + "\n" + recipe_digest);
        }
        if (digest.empty() || key.empty())
            throw std::runtime_error("feature product identity is incomplete");
        products.push_back({std::move(key), std::move(digest)});
    }
    return products;
}

std::string featureSetSignature(const std::vector<FeatureProductIdentity>& products,
                                const MatchesDatabase& db) {
    std::string bytes = "sfm-pair-list-v2";
    for (size_t i = 0; i < products.size(); ++i) {
        const auto& product = products[i];
        bytes += std::to_string(i) + ":" + std::to_string(product.key.size()) + ":" +
                 product.key + std::to_string(product.digest.size()) + ":" +
                 product.digest + ":" + std::to_string(db.images[i].num_features) + ";";
    }
    return feature_work::sha256Text(bytes);
}

std::vector<resume::PairDependency> pairDependencies(
    const std::vector<std::pair<uint32_t, uint32_t>>& pairs,
    const std::vector<FeatureProductIdentity>& products,
    const MatchesDatabase& db,
    const feature_work::FeaturePlan* plan,
    const std::string& recipe_digest) {
    std::vector<resume::PairDependency> dependencies;
    dependencies.reserve(pairs.size());
    for (const auto& pair : pairs) {
        resume::PairDependency dependency;
        dependency.image1 = pair.first;
        dependency.image2 = pair.second;
        dependency.image_name1 =
            plan ? plan->images.at(pair.first).logical_name : db.images.at(pair.first).name;
        dependency.image_name2 =
            plan ? plan->images.at(pair.second).logical_name : db.images.at(pair.second).name;
        dependency.feature_key1 = products.at(pair.first).key;
        dependency.feature_digest1 = products.at(pair.first).digest;
        dependency.feature_key2 = products.at(pair.second).key;
        dependency.feature_digest2 = products.at(pair.second).digest;
        dependency.recipe_digest = recipe_digest;
        dependency.dependency_key = resume::pairDependencyKey(dependency);
        dependencies.push_back(std::move(dependency));
    }
    return dependencies;
}

}  // namespace

int matchFeatureDir(const std::string& featdir, const SfmConfig& cfg,
                    PairMode mode, bool verify,
                    std::vector<FeatureSet>& feats, MatchesDatabase& db,
                    MatchStats& stats, VerifyCalibration* calib,
                    const MatchResume* res,
                    const feature_work::FeaturePlan* plan,
                    const feature_work::CollectionIndex* index) {
    const MatchOptions& opt = cfg.match;
    const PairSelectionOptions& popt = cfg.prefilter;
    const TwoViewOptions& tvopt = cfg.twoview;
    const bool verbose = !cfg.quiet;
    const int load_rc =
        plan && index
            ? loadFeatureCollection(featdir, *plan, *index, cfg, true, feats, db)
            : loadFeatureDir(featdir, cfg, true, feats, db);
    if (load_rc) return load_rc;
    const std::string feature_recipe_digest =
        plan ? feature_work::recipeDigest(plan->extraction)
             : makeFeatureRecipe(cfg).digest;
    const std::vector<FeatureProductIdentity> products =
        featureProducts(featdir, feats, db, plan, index, feature_recipe_digest);
    const std::string match_recipe_digest =
        feature_work::sha256Text(matchRecipeSignature(cfg));
    const std::string feature_set_signature = featureSetSignature(products, db);
    const std::string pair_plan_signature =
        feature_work::sha256Text(stageSignature(cfg, CMD_MATCH));
    const size_t n_images = feats.size();
    stats.images = n_images;

    std::vector<std::pair<uint32_t, uint32_t>> pairs;
    // Pair selection is minutes on a large capture and used to look like a
    // frozen program between the two stages that have a bar; it reports through
    // the same event stream everything else does.
    std::function<void(size_t, size_t)> sp = [&](size_t done, size_t total) {
        events::progress(Stage::Select, (int64_t)done, (int64_t)total);
        if (!verbose) return;
        // Redrawn in place, so it carries the tag itself rather than
        // going through L::err(), which always ends its line.
        fprintf(stderr, "\r%s%s", L::prefix(Tag::Match).c_str(),
                spirula::i18n::format(M::match_pairs_scored,
                             {(long long)done, (long long)total}).c_str());
    };
    // A pair list an interrupted run already chose. Selecting it again is a
    // fraction of matching, but not a small one, and it has to produce the same
    // list for the journal below to line up with it.
    const std::string pair_list_signature =
        res ? res->signature + "\npair-mode=" + std::to_string((int)mode) +
                  "\npair-plan=" + pair_plan_signature +
                  "\nfeature-set=" + feature_set_signature
            : std::string();
    const bool reused_pairs =
        res && resume::readPairs(res->dir / "pairs.bin", pair_list_signature, pairs);
    if (reused_pairs) {
        L::out(Tag::Match, M::match_reusing_pairs, {(long long)pairs.size()});
    } else if (mode == PairMode::Prefilter) {
        stats.scored = n_images * (n_images - 1) / 2;
        double t0 = now();
        events::stage_begin(Stage::Select, (int64_t)(n_images * (n_images - 1)));
        pairs = prefilterPairs(feats, popt, sp);
        events::stage_end(Stage::Select);
        stats.select_seconds = now() - t0;
        if (verbose)
            fprintf(stderr, "\n");
            L::err(Tag::Match, M::match_prefilter_kept,
                   {(long long)pairs.size(), (long long)stats.scored,
                    popt.num_features, popt.num_neighbors,
                    format_duration(stats.select_seconds)});
    } else {
        pairs = generatePairs((uint32_t)n_images, mode, cfg.overlap);
        // Loop closure. A sequential chain has no link between the start and
        // end of a walk that comes back on itself, so one weak step splits the
        // reconstruction; the pair-selection shortlist supplies the missing
        // links from image content, the way COLMAP's loop_detection does from a
        // vocabulary tree. Exhaustive already has every pair.
        if (mode == PairMode::Sequential && cfg.loop_closure && n_images > 2) {
            const size_t seq = pairs.size();
            stats.scored = n_images * (n_images - 1) / 2;
            double t0 = now();
            events::stage_begin(Stage::Select, (int64_t)(n_images * (n_images - 1)));
            std::vector<std::pair<uint32_t, uint32_t>> extra = prefilterPairs(feats, popt, sp);
            events::stage_end(Stage::Select);
            stats.select_seconds = now() - t0;
            pairs.insert(pairs.end(), extra.begin(), extra.end());
            std::sort(pairs.begin(), pairs.end());
            pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
            if (verbose)
                fprintf(stderr, "\n");
                L::err(Tag::Match, M::match_loop_closure_added,
                       {(long long)(pairs.size() - seq), (long long)seq,
                        (long long)extra.size(),
                        format_duration(stats.select_seconds)});
        }
    }
    if (!reused_pairs && plan) {
        for (const auto& chunk : plan->chunks) {
            const auto local =
                generatePairs((uint32_t)chunk.image_indices.size(),
                              PairMode::Sequential, cfg.overlap);
            for (const auto& pair : local)
                pairs.emplace_back(chunk.image_indices[pair.first],
                                   chunk.image_indices[pair.second]);
        }
        std::sort(pairs.begin(), pairs.end());
    }
    if (res && !reused_pairs)
        resume::writePairs(res->dir / "pairs.bin", pair_list_signature, pairs);
    stats.pairs = pairs.size();
    if (verbose)
        L::err(Tag::Match, M::match_plan,
               {(long long)n_images, (long long)pairs.size(),
                mode == PairMode::Exhaustive ? "exhaustive"
                : mode == PairMode::Sequential
                    ? (cfg.loop_closure ? "sequential + loop closure" : "sequential")
                    : "prefilter"});
    if (verbose && mode == PairMode::Prefilter)
        L::err(Tag::Match, M::match_prefilter_params,
               {popt.num_features, popt.num_neighbors});
    const std::vector<resume::PairDependency> dependencies =
        pairDependencies(pairs, products, db, plan, match_recipe_digest);

    // What an interrupted verification already finished, and so the subset this
    // run has left. Keyed on the image pair rather than on its position: the
    // workers finish out of order, so the journal is not in the list's order.
    std::unordered_map<uint64_t, TwoViewMatches> done_kept;
    std::vector<uint64_t> done_keys;
    const fs::path journal_path = res ? res->dir / "matches.part" : fs::path();
    const bool resumed_verify =
        res && verify &&
        resume::readJournal(journal_path, res->signature, dependencies, db.images, done_kept,
                            done_keys, stats.putative);
    std::set<uint64_t> done_set(done_keys.begin(), done_keys.end());
    std::vector<std::pair<uint32_t, uint32_t>> todo;
    if (resumed_verify && !done_set.empty()) {
        for (const auto& p : pairs)
            if (!done_set.count(resume::pairKey(p.first, p.second))) todo.push_back(p);
        L::out(Tag::Match, M::match_resuming,
               {(long long)(pairs.size() - todo.size()), (long long)pairs.size()});
    } else {
        todo = pairs;
    }

    std::unique_ptr<IFeatureMatcher> matcher =
        createFeatureMatcher(cfg.matcher, opt, cfg.lightglue, cfg.loma_match);
    if (verbose && cfg.matcher != "bruteforce")
        L::err(Tag::Match, M::match_matcher_name, {matcher->name()});
    auto matchFn = [&](size_t b, size_t e, std::vector<std::vector<FeatureMatch>>& mout) {
        matcher->matchBatch(feats, todo, b, e, mout);
    };
    // Rate-limiting the printed line lives in the CLI's event sink, which is
    // the only thing that prints it; verifyPairs emits the fraction either way.
    std::function<void(size_t, size_t)> progress;

    if (verify) {
        // Verification runs on a worker pool fed by the (serial, GPU-bound)
        // matcher -- it is the pipeline's dominant cost, see sfm/feature/Verification.h.
        VerificationOptions vopt;
        vopt.two_view = tvopt;
        vopt.num_threads = cfg.threads;
        vopt.match_batch_pairs = opt.batch_pairs;

        // Calibrated verification (D45), for fisheye captures only: see
        // VerifyCalibration. Everything else keeps the pixel path, where the
        // pinhole assumption is exact and results are long settled.
        BearingCache bc;
        if (calib) {
            CameraSetup& cs = calib->cameras;
            cs = buildCameras(db.images, feats, calib->setup);
            if (verbose) printCameraSetup(Tag::Match, cs, calib->setup, feats.size());
            // Both focal searches want the same thing: putative matches for a
            // sample of pairs, spread over the list (a prefix would sample one
            // part of the capture, since pair lists are ordered). The fisheye
            // one has to run before verification because the focal decides the
            // bearings it verifies on; the rectilinear one could run after, but
            // sharing this sample costs nothing and keeps one code path.
            const bool want_rect = cs.anyGuessedRectilinear();
            std::vector<std::pair<uint32_t, uint32_t>> sample;
            std::vector<std::vector<FeatureMatch>> sm;
            if (cs.anyWide() || want_rect) {
                const size_t want = calib->sample_pairs * std::max<size_t>(1, cs.count());
                size_t stride = std::max<size_t>(1, pairs.size() / std::max<size_t>(1, want));
                for (size_t p = 0; p < pairs.size() && sample.size() < want; p += stride)
                    sample.push_back(pairs[p]);
                std::vector<std::vector<FeatureMatch>> chunk;
                for (size_t b = 0; b < sample.size(); b += 16) {
                    size_t e = std::min(b + 16, sample.size());
                    matcher->matchBatch(feats, sample, b, e, chunk);
                    for (size_t k = b; k < e; k++) sm.push_back(std::move(chunk[k - b]));
                }
            }
            if (want_rect) {
                double t_f = now();
                bootstrapRectilinearFocals(feats, cs.ids, sample, sm, cs.cameras, cs.focal_given,
                                           cs.focal_measured, tvopt, calib->sample_pairs,
                                           cfg.threads, verbose);
                if (verbose && !cs.focal_measured.empty())
                    L::err(Tag::Match, M::focal_epipolar_search,
                           {format_duration(now() - t_f)});
            }
            if (cs.anyWide()) {
                bootstrapGroupFocals(feats, cs.ids, sample, sm, cs.cameras, cs.focal_given,
                                     cs.focal_measured, tvopt, calib->sample_pairs, cfg.threads,
                                     verbose);
                std::vector<Camera> percam(feats.size());
                for (size_t i = 0; i < feats.size(); i++) percam[i] = cs.cameras.at(cs.ids[i]);
                double t_b = now();
                // A mixed capture calibrates *everything*: its cross pairs have
                // a fisheye on one side, and the pixel path cannot represent
                // those at all. A wholly rectilinear capture keeps the pixel
                // path, where the pinhole assumption is exact and results are
                // long settled.
                bc = precomputeBearings(feats, percam, /*wide_only=*/!cs.mixed(), cfg.threads);
                vopt.bearings = &bc;
                calib->used_bearings = true;
                if (verbose) {
                    // The focal list is identifiers and numbers, built here and
                    // passed through the message as one argument.
                    std::string focals;
                    for (const auto& kv : cs.cameras) {
                        if (!focals.empty()) focals += ", ";
                        focals += "cam " + std::to_string(kv.first) + ": " +
                                  L::num(kv.second.focal(), 1);
                    }
                    L::err(Tag::Match, M::match_bearings,
                           {format_duration(now() - t_b),
                            L::num(bc.bytes() / 1048576.0, 0), focals});
                }
            }
        }
        // A focal either search measured was measured *on that group's own
        // pairs*, which is what focal_known means: the mapper's per-image sweep
        // has nothing to add to it and every reason to leave it alone. On a
        // dual-fisheye rig the sweep was seen taking a group from the 552 px
        // the two-view stage measured to 397 px on one image's 80 inliers, with
        // the joint refinement then dragging it back to 517. It is deliberately
        // *not* focal_given: the mapper's probe reconstruction still runs and
        // bundle-adjusts the value, which measured better than the raw vote.
        if (calib)
            for (uint32_t id : calib->cameras.focal_measured)
                calib->cameras.focal_known.insert(id);
        // Hand the setup on through the database (D47), so `spirula-sfm map` inherits
        // the grouping and the focals this stage measured instead of
        // re-deriving them from the inliers it is about to produce.
        if (calib) storeCameraSetup(db, calib->cameras);
        if (verbose)
            L::err(Tag::Match, M::match_verifying, {(long long)verificationThreadCount(vopt)});
        sfm::progress::begin_matching((uint32_t)feats.size(), pairs);
        {
            std::vector<std::string> names;
            std::vector<uint32_t> nfeat;
            names.reserve(db.images.size());
            nfeat.reserve(db.images.size());
            for (const ImageEntry& im : db.images) {
                names.push_back(im.name);
                nfeat.push_back(im.num_features);
            }
            sfm::progress::live_matches_begin(names, nfeat);
        }
        // The pairs the journal carries are not re-verified, so nothing else
        // will report them: without this the match map draws a resumed run's
        // first half as "not reached yet" for the rest of the stage.
        for (uint64_t key : done_keys) {
            const auto it = done_kept.find(key);
            sfm::progress::pair((uint32_t)(key >> 32), (uint32_t)key,
                                it == done_kept.end() ? 0u
                                                      : (uint32_t)it->second.matches.size());
        }
        resume::MatchJournal journal;
        if (res) journal.open(journal_path, res->signature, resumed_verify, dependencies);
        vopt.journal = &journal;
        vopt.progress_done_base = pairs.size() - todo.size();
        vopt.progress_total = pairs.size();
        events::stage_begin(Stage::Match, (int64_t)pairs.size());
        // Added, not assigned: the journal already counted what an earlier run
        // offered the verifier, and verifyPairs writes its own total.
        uint64_t putative = 0;
        std::vector<TwoViewMatches> fresh =
            verifyPairs(feats, todo, matchFn, vopt, &putative, progress);
        stats.putative += putative;
        journal.close();
        sfm::progress::flush();
        events::stage_end(Stage::Match);
        // Back into the pair list's order, whichever run produced each entry:
        // the mapper's seed ranking breaks ties on it, so a resumed run must
        // hand it over in the order a single run would have.
        if (done_kept.empty()) {
            db.pairs = std::move(fresh);
        } else {
            std::unordered_map<uint64_t, size_t> at;
            at.reserve(fresh.size() * 2);
            for (size_t i = 0; i < fresh.size(); i++)
                at[resume::pairKey(fresh[i].image1, fresh[i].image2)] = i;
            db.pairs.reserve(done_kept.size() + fresh.size());
            for (const auto& p : pairs) {
                const uint64_t key = resume::pairKey(p.first, p.second);
                const auto old = done_kept.find(key);
                if (old != done_kept.end()) db.pairs.push_back(std::move(old->second));
                else if (const auto n = at.find(key); n != at.end())
                    db.pairs.push_back(std::move(fresh[n->second]));
            }
        }
        for (const TwoViewMatches& tvm : db.pairs) stats.inliers += tvm.matches.size();
    } else {
        const size_t batch = std::max(1, opt.batch_pairs);
        std::vector<std::vector<FeatureMatch>> mout;
        for (size_t b = 0; b < todo.size(); b += batch) {
            size_t e = std::min(b + batch, todo.size());
            matchFn(b, e, mout);
            for (size_t p = b; p < e; p++) {
                uint32_t i = todo[p].first, j = todo[p].second;
                std::vector<FeatureMatch>& m = mout[p - b];
                stats.putative += m.size();
                if (!m.empty()) {
                    stats.inliers += m.size();
                    db.pairs.push_back({i, j, 0, std::move(m)});
                }
                if (progress) progress(p + 1, todo.size());
            }
        }
    }
    stats.kept = db.pairs.size();
    return 0;
}
// ---------------------------------------------------------------------------
// `auto`
// ---------------------------------------------------------------------------

AutoResult run_auto(SfmConfig& cfg, const AutoInputs& in) {
    AutoResult r;
    std::string _imagedir = in.image_dir;
    const std::string& _workspace = in.workspace;
    const bool verbose = !cfg.quiet;

    // ---- where the images and masks are (D39/D40) ----
    // The default layout is a dataset directory holding `images/` and `masks/`,
    // which is Spirula Studio's and nerfstudio's. Pointing straight at an image
    // directory still works: `masks` is then looked for as its sibling, so
    // `spirula-sfm auto DATASET/images` and `spirula-sfm auto DATASET` behave the same.
    if (_imagedir.empty()) _imagedir = "images";
    if (!fs::is_directory(_imagedir)) {
        L::fail(Tag::Run, M::run_not_a_directory, {_imagedir});
        { r.exit_code = 1; return r; }
    }
    {
        fs::path nested = fs::path(_imagedir) / "images";
        if (fs::is_directory(nested) && !holdsImagesOutside(_imagedir, nested)) {
            L::out(Tag::Run, M::run_nested_images, {_imagedir, nested.string()});
            _imagedir = nested.string();
        }
    }
    if (!in.mask_dir_explicit) {
        fs::path p = fs::path(_imagedir);
        if (!p.empty() && p.filename().empty()) p = p.parent_path();  // drop a trailing '/'
        fs::path sibling = p.parent_path() / "masks";
        if (fs::is_directory(sibling)) cfg.mask_dir = sibling.string();
    }
    adoptExrColorSpace(cfg, _imagedir, in.explicit_flags);

    fs::path ws(_workspace);
    fs::create_directories(ws);
    const fs::path featdir = ws / "features";
    const fs::path matchpath = ws / "matches.bin";
    // COLMAP's layout: sparse/<i> per reconstruction, sparse/0 the largest.
    const fs::path sparsedir = ws / "sparse";
    std::optional<feature_work::FeaturePlan> imported_plan;
    std::optional<feature_work::CollectionIndex> imported_index;
    if (!in.feature_plan.empty()) {
        imported_plan =
            feature_work::readPlanFile(fs::absolute(in.feature_plan).string());
        applyFeatureRecipe(imported_plan->extraction, cfg);
        if (makeFeatureRecipe(cfg).digest != imported_plan->extraction.digest)
            throw std::runtime_error(
                "central extraction settings disagree with the feature plan");
        imported_index = feature_work::readCollectionIndexFile(
            (featdir / "index.json").string());
        feature_work::validateCollection(*imported_plan, *imported_index);
        validateFeaturePlanInputs(*imported_plan, _imagedir, cfg.mask_dir);
    }

    L::out(Tag::Run, M::run_header, {_imagedir, _workspace});
    L::out(Tag::Run, M::run_quality,
           {cfg.quality, (long long)cfg.max_image_size,
            (long long)cfg.sift.max_num_features});
    L::out(Tag::Run, M::run_data_type, {cfg.data_type});
    L::out(Tag::Run, M::run_cameras, {cfg.camera_model, cfg.camera_mode});
    if (!cfg.mask_dir.empty()) L::out(Tag::Run, M::run_masks, {cfg.mask_dir});
    // What the two knobs moved, so a surprising run is explainable from its own
    // output rather than from reading the preset table.
    for (const PresetChange& p : in.preset_changes)
        L::out(Tag::Run, M::run_preset_moved, {"--" + p.flag, p.to, p.from});

    // ---- what an interrupted run left, and whether it is still ours ----
    // The signature is stored BEFORE the stage rather than after it, so that a
    // run interrupted half way through leaves files the next one may reuse.
    const fs::path rdir = resume::dir(_workspace);
    const std::string extract_sig =
        imported_plan
            ? "feature-plan=" + imported_plan->digest + "\ncollection=" +
                  imported_index->collection_digest + "\n"
            : stageSignature(cfg, CMD_EXTRACT) + "images=" + _imagedir + "\n";
    std::error_code rm_ec;
    bool reuse = cfg.reuse;
    if (!reuse || resume::recorded(rdir / "extract.sig") != extract_sig) {
        reuse = false;
        if (!cfg.reuse) {
            resume::clear(_workspace);
        } else {
            resume::forget(rdir / "pairs.bin");
        }
        if (!imported_plan) fs::remove_all(featdir, rm_ec);
        fs::remove(matchpath, rm_ec);
    }
    if (cfg.reuse) resume::store(rdir / "extract.sig", extract_sig);

    // ---- 1. extract ----
    double t0 = now();
    ExtractStats est;
    if (imported_plan) {
        est.images = imported_index->rows.size();
        est.reused = est.images;
        for (const auto& row : imported_index->rows)
            est.features += row.feature_count;
        L::out(Tag::Extract, M::imported_features,
               {(long long)est.images, (unsigned long long)est.features,
                imported_index->collection_digest});
    } else if (int rc =
                   extractDirectory(_imagedir, featdir, cfg, est, reuse)) {
        r.exit_code = rc;
        return r;
    }
    double t_extract = now() - t0;
    if (est.images < 2) {
        L::fail(Tag::Run, M::run_too_few_images, {(long long)est.images});
        { r.exit_code = 1; return r; }
    }
    warnIfMasksLookInverted(est);

    // Pairing: what --pairs says, except that "auto" only knows the image count
    // once extraction has run. Above COLMAP's exhaustive cutoff -- where COLMAP
    // switches to vocabulary-tree retrieval -- we switch to GPU pair selection
    // (sfm/feature/PairSelection.h, D35).
    //
    // The same cutoff retires the video preset's sequential pairing. A temporal
    // window is a chain, and a capture long enough to be worth this many frames
    // is long enough to come back on itself; pair selection finds those links
    // from content, at a cost that is a fraction of matching. Measured on a
    // 262-frame walk: sequential gave four models (144 / 74 / 19 / 12 images),
    // pair selection one with 254. Below the cutoff the temporal prior is still
    // the cheaper way to get the same pairs, and --loop-closure covers its blind
    // spot. `--pairs sequential` explicitly still means sequential.
    PairMode mode = cfg.pairMode();
    if ((mode == PairMode::Exhaustive || mode == PairMode::Sequential) &&
        est.images >= 100 && !in.explicit_flags.count("pairs")) {
        const char* was = mode == PairMode::Exhaustive ? "exhaustive" : "sequential";
        mode = PairMode::Prefilter;
        L::out(Tag::Match, M::match_switch_to_selection, {(long long)est.images, was});
    } else if (mode == PairMode::Exhaustive && est.images >= 100) {
        L::warn(Tag::Match, M::match_exhaustive_quadratic,
                {(long long)est.images, (long long)(est.images * (est.images - 1) / 2)});
    }

    // ---- 2. match + geometric verification ----
    t0 = now();
    std::vector<FeatureSet> feats;
    MatchesDatabase db;
    MatchStats mstats;
    VerifyCalibration calib;
    calib.setup = cfg.camera;
    // What this stage's output depends on: its own settings, the extraction
    // that produced its input, and the feature files themselves -- the pair
    // lists and the journal are indices into a particular set of those.
    MatchResume mres;
    mres.dir = rdir;
    mres.signature = matchRecipeSignature(cfg);
    bool reused_matches = false;
    // A finished matches.bin is the whole of this stage; the mapper wants
    // keypoints and colours, so the descriptors are never read at all.
    if (cfg.reuse && fs::exists(matchpath, rm_ec)) {
        try {
            MatchesDatabase disk = readMatches(matchpath.string());
            const int load_rc = imported_plan
                                    ? loadFeatureCollection(
                                          featdir, *imported_plan,
                                          *imported_index, cfg, false, feats, db)
                                    : loadFeatureDir(featdir.string(), cfg, false,
                                                     feats, db);
            if (load_rc == 0) {
                if (disk.images.size() != db.images.size())
                    throw std::runtime_error(
                        "matches image count disagrees with feature collection");
                for (size_t i = 0; i < db.images.size(); ++i)
                    if (disk.images[i].name != db.images[i].name ||
                        disk.images[i].num_features !=
                            db.images[i].num_features)
                        throw std::runtime_error(
                            "matches image order disagrees with feature collection");
                const std::string feature_recipe_digest =
                    imported_plan ? feature_work::recipeDigest(imported_plan->extraction)
                                  : makeFeatureRecipe(cfg).digest;
                const std::string match_recipe_digest =
                    feature_work::sha256Text(matchRecipeSignature(cfg));
                const std::vector<FeatureProductIdentity> products =
                    featureProducts(featdir, feats, db, imported_plan ? &*imported_plan : nullptr,
                                    imported_index ? &*imported_index : nullptr,
                                    feature_recipe_digest);
                const std::string cache_signature =
                    mres.signature + "\npair-mode=" + std::to_string((int)mode) +
                    "\npair-plan=" +
                    feature_work::sha256Text(stageSignature(cfg, CMD_MATCH)) +
                    "\nfeature-set=" + featureSetSignature(products, db);
                if (resume::recorded(rdir / "match.sig") != cache_signature)
                    throw std::runtime_error("match cache signature differs");
                std::vector<std::pair<uint32_t, uint32_t>> cached_pairs;
                cached_pairs.reserve(disk.pairs.size());
                for (const TwoViewMatches& pair : disk.pairs)
                    cached_pairs.emplace_back(pair.image1, pair.image2);
                const std::vector<resume::PairDependency> dependencies =
                    pairDependencies(cached_pairs, products, db,
                                     imported_plan ? &*imported_plan : nullptr,
                                     match_recipe_digest);
                std::unordered_map<uint64_t, TwoViewMatches> verified;
                std::vector<uint64_t> completed;
                uint64_t ignored_putative = 0;
                if (!resume::readJournal(rdir / "matches.part", mres.signature,
                                          dependencies, db.images, verified, completed,
                                          ignored_putative) ||
                    verified.size() != disk.pairs.size())
                    throw std::runtime_error("match journal is missing or incompatible");
                std::unordered_map<uint64_t, resume::PairDependency> by_pair;
                for (const auto& dependency : dependencies)
                    by_pair.emplace(resume::pairKey(dependency.image1, dependency.image2),
                                    dependency);
                for (const TwoViewMatches& pair : disk.pairs) {
                    const uint64_t key = resume::pairKey(pair.image1, pair.image2);
                    const auto dep = by_pair.find(key);
                    const auto old = verified.find(key);
                    if (dep == by_pair.end() || old == verified.end() ||
                        resume::pairPayloadDigest(dep->second, pair) !=
                            resume::pairPayloadDigest(dep->second, old->second))
                        throw std::runtime_error("match payload is not journaled");
                }
                db.pairs = std::move(disk.pairs);
                db.cameras = std::move(disk.cameras);
                db.camera_ids = std::move(disk.camera_ids);
                db.focal_prior = std::move(disk.focal_prior);
                db.focal_measured = std::move(disk.focal_measured);
                loadCameraSetup(db, calib.cameras);
                mstats.images = feats.size();
                mstats.kept = mstats.pairs = db.pairs.size();
                for (const TwoViewMatches& tvm : db.pairs) mstats.inliers += tvm.matches.size();
                reused_matches = true;
            }
        } catch (const std::exception& e) {
            L::warn(Tag::Match, M::match_reuse_failed, {e.what()});
            feats.clear();
            db = MatchesDatabase();
        }
    }
    if (!reused_matches && fs::exists(matchpath, rm_ec)) {
        fs::remove(matchpath, rm_ec);
        if (rm_ec)
            throw std::runtime_error("cannot discard incompatible matches: " +
                                     rm_ec.message());
    }
    if (reused_matches) {
        L::out(Tag::Match, M::match_reusing_matches,
               {(long long)mstats.kept, matchpath.string()});
    } else if (int rc = matchFeatureDir(
                   featdir.string(), cfg, mode, true, feats, db, mstats, &calib,
                   cfg.reuse ? &mres : nullptr,
                   imported_plan ? &*imported_plan : nullptr,
                   imported_index ? &*imported_index : nullptr)) {
        r.exit_code = rc;
        return r;
    }
    double t_match = now() - t0;
    if (!reused_matches) {
        writeMatches(matchpath.string(), db);
        // Keep the journal as the dependency manifest for this immutable publication.
        resume::forget(rdir / "pairs.bin");
        if (cfg.reuse) {
            const std::string feature_recipe_digest =
                imported_plan ? feature_work::recipeDigest(imported_plan->extraction)
                              : makeFeatureRecipe(cfg).digest;
            const std::vector<FeatureProductIdentity> products =
                featureProducts(featdir, feats, db,
                                imported_plan ? &*imported_plan : nullptr,
                                imported_index ? &*imported_index : nullptr,
                                feature_recipe_digest);
            resume::store(
                rdir / "match.sig",
                mres.signature + "\npair-mode=" + std::to_string((int)mode) +
                    "\npair-plan=" +
                    feature_work::sha256Text(stageSignature(cfg, CMD_MATCH)) +
                    "\nfeature-set=" + featureSetSignature(products, db));
        }
    }
    // Nothing past this point reads a descriptor -- the mapper works on
    // keypoints, the correspondence graph and the per-keypoint colors -- and on
    // a large capture they are the biggest thing in the process: 8k features
    // per image at 128 bytes is a gigabyte per thousand images, held for the
    // whole of mapping for nothing.
    for (FeatureSet& fs : feats) {
        std::vector<uint8_t>().swap(fs.descriptors);
    }
    // After writeMatches, never before: the file on disk indexes the feature
    // files, which keep every row.
    if (cfg.compact_unused_features) {
        FeatureCompactionPlan plan = buildFeatureCompactionPlan(db);
        for (size_t i = 0; i < feats.size(); i++)
            feats[i] = compactFeatureSet(std::move(feats[i]), plan.old_to_new[i],
                                         plan.compact_counts[i]);
        remapMatches(db, plan, feats);
        if (verbose) reportFeatureCompaction(plan.stats);
    }

    // ---- 3. incremental mapping ----
    // The grouping and the focals the two-view stage settled on carry straight
    // into mapping: a focal it measured beats the geometric default the mapper
    // would otherwise start the group from, and every group starting from a
    // measurement is what stops small components inventing their own
    // intrinsics (D45/D46).
    MapperOptions& mapopt = cfg.mapper;
    const CameraSetup& cs = calib.cameras;
    mapopt.initial_cameras = cs.cameras;
    mapopt.known_focal_cameras = cs.focal_known;
    mapopt.given_focal_cameras = cs.focal_given;
    mapopt.measured_focal_cameras = cs.focal_measured;

    t0 = now();
    events::stage_begin(Stage::Map, (int64_t)db.images.size());
    events::map_begin(db.images.size());
    RigTable rigs;
    try {
        rigs = buildRigs(db, cfg, verbose);
    } catch (const std::runtime_error& e) {
        L::fail(Tag::Map, M::rig_bad, {e.what()});
        r.exit_code = 2;
        return r;
    }
    Mapper mapper(db, feats, mapopt, cs.ids, &rigs);
    AssembleStats ast;
    std::vector<Reconstruction> models = runMapper(mapper, db, feats, cfg, ast);
    double t_map = now() - t0;

    {
        // A global solve per model, and up to two more passes over them: minutes
        // on a large capture, with the last image long since placed.
        events::stage_begin(Stage::Refine);
        double t_finish = 0;
        models = finishModels(mapper, std::move(models), cfg, verbose, t_finish);
        t_map += t_finish;
        events::stage_end(Stage::Refine);
    }
    events::stage_end(Stage::Map);

    if (imported_plan) resolveImageNames(models, *imported_plan);
    else resolveImageNames(models, _imagedir);
    std::vector<ModelGauge> gauge;
    const bool auto_metric = fixGauge(models, cfg, _imagedir, verbose, gauge);
    recolorPoints(models, cfg);
    // The gauge is what a screen was missing: every snapshot before this one is
    // in the seed pair's frame, so a run watched to the end left a tilted model
    // on display until the user opened the written one.
    if (!gauge.empty()) progress::gauge(gauge[0].oriented, gauge[0].metric);
    if (!models.empty()) progress::model(models.front(), /*force=*/true);
    // Before the split: the summary reports what was estimated, and the file's
    // one camera per frame size is not that.
    const size_t n_cameras = models.empty() ? 0 : models.front().cameras.size();
    splitCamerasBySize(models, feats);
    if (imported_plan) {
        validateFeaturePlanInputs(*imported_plan, _imagedir, cfg.mask_dir);
        const fs::path staging =
            ws / ("sparse.import-" + imported_plan->digest.substr(0, 12));
        const fs::path backup =
            ws / ("sparse.previous-" + imported_plan->digest.substr(0, 12));
        std::error_code publish_error;
        fs::remove_all(staging, publish_error);
        if (publish_error)
            throw std::runtime_error("cannot clear sparse staging: " +
                                     publish_error.message());
        fs::remove_all(backup, publish_error);
        if (publish_error)
            throw std::runtime_error("cannot clear sparse backup: " +
                                     publish_error.message());
        writeModels(models, staging, verbose, gauge, &rigs);
        const bool replacing = fs::exists(sparsedir);
        if (replacing) {
            fs::rename(sparsedir, backup, publish_error);
            if (publish_error)
                throw std::runtime_error("cannot preserve sparse model: " +
                                         publish_error.message());
        }
        fs::rename(staging, sparsedir, publish_error);
        if (publish_error) {
            if (replacing) {
                std::error_code restore_error;
                fs::rename(backup, sparsedir, restore_error);
            }
            throw std::runtime_error("cannot publish sparse model: " +
                                     publish_error.message());
        }
        if (replacing) fs::remove_all(backup, publish_error);
    } else {
        writeModels(models, sparsedir, verbose, gauge, &rigs);
    }

    // The mapper reports its own breakdown when `run()` returns; the passes
    // that assemble its models accumulate into the same counters.
    g_map_prof.report(t_map, "map");

    // ---- report ----
    const Reconstruction& rec = models.front();
    double mean = 0, median = 0;
    size_t nobs = 0;
    reprojStats(rec, feats, mean, median, nobs);
    const uint32_t reg = rec.numRegistered();
    L::out(Tag::Run, M::sum_header);
    L::out(Tag::Run, M::sum_extract,
           {format_duration(t_extract), (long long)est.images,
            (long long)est.features});
    if (est.masked_images) {
        // Over what this run extracted, like the warning above: a reused
        // feature file does not say what a mask took out of it.
        const uint64_t before = est.features_new + est.masked_out;
        L::out(Tag::Run, M::sum_masks,
               {(long long)est.masked_images, (long long)est.images,
                (long long)est.masked_out,
                L::num(before ? 100.0 * est.masked_out / before : 0.0, 1)});
    }
    if (reused_matches)
        L::out(Tag::Run, M::sum_match_reused,
               {(long long)mstats.kept, (long long)mstats.inliers});
    else
        L::out(Tag::Run, M::sum_match,
               {format_duration(t_match), (long long)mstats.kept, (long long)mstats.pairs,
                (long long)mstats.inliers, (long long)mstats.putative});
    L::out(Tag::Run, M::sum_map,
           {format_duration(t_map), (long long)reg, (long long)est.images,
            (long long)rec.points3D.size(), (long long)n_cameras});
    printAssembly(ast, models.size(), Tag::Run);
    printFolderCoverage(models, db);
    writeUnregisteredList(models, db, _imagedir);
    L::out(Tag::Run, M::sum_total,
           {format_duration(t_extract + t_match + t_map)});
    L::out(Tag::Run, M::sum_model_error,
           {L::num(mean, 3), L::num(median, 3), (long long)nobs});
    if (models.size() > 1) {
        // A fragmented capture: sparse/0 is the largest component, the rest are
        // separate reconstructions with no known transform between them (D41).
        L::out(Tag::Run, M::sum_components,
               {(long long)models.size(), (long long)distinctRegistered(models),
                (long long)est.images});
        printExtraModels(models, feats);
    }
    L::out(Tag::Run, M::sum_written,
           {models.size() > 1
                ? sparsedir.string() + "/{0.." + std::to_string(models.size() - 1) + "}"
                : (sparsedir / "0").string()});

    // Verdict, so a batch run can be scanned without reading every number.
    // Thresholds are deliberately loose -- this flags "obviously broken", not
    // "not as good as COLMAP".
    const double frac = est.images ? (double)reg / est.images : 0.0;
    // Everything the exit code says, said as data. A front end reads this
    // instead of the status: 3 and 4 cannot both be reported, and this can.
    auto emitResult = [&](bool part) {
        r.registered = reg;
        r.images = est.images;
        r.points = (int64_t)rec.points3D.size();
        r.models = (int64_t)models.size();
        r.mean_reproj = mean;
        r.median_reproj = median;
        r.partial = part;
        r.metric = auto_metric;
        r.sparse_dir = sparsedir;
        Event ev;
        ev.kind = Event::Kind::Result;
        ev.stage = Stage::Finish;
        ev.registered = reg;
        ev.images = est.images;
        ev.points = (int64_t)rec.points3D.size();
        ev.models = (int64_t)models.size();
        ev.mean_reproj = mean;
        ev.partial = part;
        ev.metric = auto_metric;
        events::emit(ev);
    };
    if (reg < 2 || rec.points3D.empty()) {
        L::out(Tag::Run, M::result_failed);
        emitResult(true);
        { r.exit_code = 2; return r; }
    }
    const bool partial = frac < 0.5 || mean > 2.0;
    emitResult(partial);
    if (partial) L::out(Tag::Run, M::result_partial, {L::num(100 * frac, 0), L::num(mean, 2)});
    // A sound model in the wrong gauge is still a sound model, so the metric
    // verdict takes the exit status only when nothing about the reconstruction
    // itself claims it -- but it is always printed, and the GUI reads the line.
    if (!auto_metric) L::out(Tag::Run, M::result_not_metric);
    if (!partial && auto_metric)
        L::out(Tag::Run, M::result_ok, {L::num(100 * frac, 0), L::num(mean, 2)});
    r.exit_code = partial ? 3 : (auto_metric ? 0 : 4);
    return r;
}

// ---------------------------------------------------------------------------
// RunContext
// ---------------------------------------------------------------------------

// Restoring on destruction is what lets a front end run a second job, and what
// keeps a thrown Cancelled from leaving the sinks pointing at a dead object.
RunContext::~RunContext() {
    slog::set_sink({});
    events::set_sink({});
    cancel::set_token(nullptr);
    progress::set_dir("");
}

void RunContext::set_log(slog::Sink s) { slog::set_sink(std::move(s)); }
void RunContext::set_events(events::Sink s) { events::set_sink(std::move(s)); }
void RunContext::set_cancel(const std::atomic<bool>* flag) { cancel::set_token(flag); }
void RunContext::set_progress_dir(const std::string& dir) { progress::set_dir(dir); }

// ---------------------------------------------------------------------------
// Reading a settings list
// ---------------------------------------------------------------------------

namespace {

// --camera-model / --focal / --distortion: a bare value sets the dataset-wide
// default (which is a table field), PREFIX=VALUE names one camera group
// (which is not).
bool cameraOverrideArg(SfmConfig& cfg, OverrideKind kind, const std::string& v,
                       std::set<std::string>& seen, std::string& err) {
    const char* flag = kind == OverrideKind::Focal        ? "focal"
                       : kind == OverrideKind::Distortion ? "distortion"
                                                          : "camera-model";
    const char* form = kind == OverrideKind::Focal        ? "F or PREFIX=F"
                       : kind == OverrideKind::Distortion ? "k1,k2,... or PREFIX=k1,k2,..."
                                                          : "MODEL or PREFIX=MODEL";
    if (!parseCameraOverride(v, kind, cfg.camera.overrides)) {
        err = std::string("bad --") + flag + " '" + v + "' (" + form + ")";
        return false;
    }
    if (v.find('=') != std::string::npos) return true;  // per-group only
    switch (kind) {
        case OverrideKind::Focal: cfg.focal = std::atof(v.c_str()); break;
        case OverrideKind::Distortion: cfg.distortion = v; break;
        case OverrideKind::Model: {
            CamModel m;
            if (!parseCamModelName(v, m)) {
                err = "unknown --camera-model '" + v + "'";
                return false;
            }
            cfg.camera_model = v;
            break;
        }
    }
    seen.insert(flag);
    return true;
}

bool cameraOverrideName(const std::string& a, OverrideKind& kind) {
    if (a == "--camera-model") kind = OverrideKind::Model;
    else if (a == "--focal") kind = OverrideKind::Focal;
    else if (a == "--distortion") kind = OverrideKind::Distortion;
    else return false;
    return true;
}

}  // namespace

std::string parse_auto_args(const std::vector<std::string>& args, AutoRequest& out,
                            bool resolve_device) {
    // setConfigField consumes argv the way main() gets it; this is that view.
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    const int argc = (int)argv.size();

    SfmConfig& cfg = out.cfg;
    std::set<std::string> seen;
    std::string imagedir, workspace, manifest_path, feature_plan;
    bool maskdir_explicit = false;

    for (int i = 0; i < argc; i++) {
        const std::string a = args[(size_t)i];
        if (a == "--help" || a == "-h") { out.wants_help = true; return {}; }
        if (a == "--output" || a == "-o") {
            if (i + 1 >= argc) return "--output: missing value";
            workspace = args[(size_t)++i];
            continue;
        }
        if (a == "--manifest") {
            if (i + 1 >= argc) return "--manifest: missing value";
            manifest_path = args[(size_t)++i];
            continue;
        }
        if (a == "--feature-plan") {
            if (i + 1 >= argc) return "--feature-plan: missing value";
            feature_plan = args[(size_t)++i];
            continue;
        }
        if (a == "--rig") {
            if (i + 1 >= argc) return "--rig: missing value";
            RigDef d;
            if (std::string err = parseRigArg(args[(size_t)++i], d); !err.empty()) return err;
            cfg.rigs.push_back(std::move(d));
            continue;
        }
        if (a == "--progress-dir") {
            if (i + 1 >= argc) return "--progress-dir: missing value";
            out.progress_dir = args[(size_t)++i];
            continue;
        }
        // Claimed, not merely cleared: manifest_apply fills in anything the
        // command line did not claim, and a manifest naming a mask_dir would
        // otherwise hand the masks back to a run that just refused them.
        if (a == "--no-masks") {
            cfg.mask_dir.clear();
            maskdir_explicit = true;
            seen.insert("masks");
            continue;
        }
        if (a == "--no-manage") {
            cfg.manager.do_merge = cfg.manager.do_grow = cfg.manager.do_reseed = false;
            cfg.manager.do_audit = cfg.manager.do_split = cfg.manager.do_duplicate_split = false;
            continue;
        }
        if (OverrideKind kind; cameraOverrideName(a, kind)) {
            if (i + 1 >= argc) return a + ": missing value";
            std::string err;
            if (!cameraOverrideArg(cfg, kind, args[(size_t)++i], seen, err)) return err;
            continue;
        }
        std::string err;
        const FieldResult r =
            setConfigField(cfg, CMD_AUTO, a, argc, argv.data(), i, seen, err);
        if (r == FieldResult::Error) return err;
        if (r == FieldResult::Ok) continue;
        if (a[0] == '-') return "unknown option " + a;
        if (!imagedir.empty()) return "unexpected argument '" + a + "'";
        imagedir = a;
    }
    if (workspace.empty()) return "--output WORKSPACE is required";
    maskdir_explicit = maskdir_explicit || seen.count("masks") || seen.count("mask-dir");

    // Presets first, the file next, the command line over both: applyPresets
    // skips anything already claimed, and manifest_apply is told the same set.
    std::vector<PresetChange> moved;
    if (std::string err = applyPresets(cfg, seen, moved); !err.empty()) return err;
    if (!manifest_path.empty()) {
        Manifest man;
        try {
            man = manifest_read(manifest_path);
        } catch (const std::exception& e) {
            return e.what();
        }
        if (std::string err = manifest_apply(man, cfg, seen, imagedir); !err.empty())
            return manifest_path + ": " + err;
        if (!man.mask_dir.empty()) maskdir_explicit = true;
    }
    if (!feature_plan.empty()) {
        try {
            const feature_work::FeaturePlan plan =
                feature_work::readPlanFile(feature_plan);
            if (seen.count("features") && cfg.features != plan.extraction.frontend)
                return "--features disagrees with --feature-plan";
            cfg.features = plan.extraction.frontend;
        } catch (const std::exception& error) {
            return std::string("--feature-plan: ") + error.what();
        }
    }
    if (std::string err = cfg.finalize(CMD_AUTO); !err.empty()) return err;
    if (resolve_device)
        if (std::string err = cfg.resolveDevice(); !err.empty()) return err;

    out.in.image_dir = imagedir;
    out.in.workspace = workspace;
    out.in.feature_plan = feature_plan;
    out.in.mask_dir_explicit = maskdir_explicit;
    out.in.explicit_flags = std::move(seen);
    out.in.preset_changes = std::move(moved);
    return {};
}

}  // namespace sfm
