// DatasetRecovery.cpp -- see DatasetRecovery.h.

#include "app/gui/DatasetRecovery.h"

#include "app/AppPaths.h"
#include "data/Json.h"
#include "data/Yaml.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace gui::dataset_recovery {
namespace {
using app::FrameMask;
using app::FrameStencil;
using app::MaskShape;

constexpr int kVersion = 1;
constexpr int kEngineSfm = 0;
constexpr int kEngineColmap = 1;
constexpr const char* kFileName = "dataset-recovery.json";

fs::path record_path() { return fs::path(app::config_dir()) / kFileName; }

// ---------------------------------------------------------------------------
// JSON value helpers
// ---------------------------------------------------------------------------

JsonValue num_v(double d) {
    JsonValue v;
    v.type = JsonValue::Type::Number;
    v.num = d;
    return v;
}
JsonValue int_v(long long n) { return num_v((double)n); }
JsonValue bool_v(bool b) {
    JsonValue v;
    v.type = JsonValue::Type::Bool;
    v.b = b;
    return v;
}
JsonValue str_v(std::string s) {
    JsonValue v;
    v.type = JsonValue::Type::String;
    v.str = std::move(s);
    return v;
}
JsonValue arr_v() {
    JsonValue v;
    v.type = JsonValue::Type::Array;
    return v;
}
JsonValue obj_v() {
    JsonValue v;
    v.type = JsonValue::Type::Object;
    return v;
}
JsonValue opt_bool_v(const std::optional<bool>& o) { return o ? bool_v(*o) : JsonValue{}; }

void add(JsonValue& o, const char* key, JsonValue v) {
    o.obj.emplace_back(key, std::move(v));
}

// Absent fields keep defaults; present fields must have their expected type.
struct Reader {
    const JsonValue& o;
    bool ok = true;

    const JsonValue* find(const char* key) const { return o.find(key); }

    void str(const char* key, std::string& dst) {
        const JsonValue* v = find(key);
        if (!v) return;
        if (v->type != JsonValue::Type::String ||
            v->str.find('\0') != std::string::npos) {
            ok = false;
            return;
        }
        dst = v->str;
    }
    bool num(const char* key, double& dst) {
        const JsonValue* v = find(key);
        if (!v) return false;
        if (v->type != JsonValue::Type::Number || !std::isfinite(v->num)) {
            ok = false;
            return false;
        }
        dst = v->num;
        return true;
    }
    void i32(const char* key, int& dst) {
        double d = 0.0;
        if (!num(key, d)) return;
        const long double x = d;
        if (x != std::trunc(x) ||
            x < (long double)std::numeric_limits<int>::min() ||
            x > (long double)std::numeric_limits<int>::max()) {
            ok = false;
            return;
        }
        dst = (int)d;
    }
    void i64(const char* key, long long& dst) {
        double d = 0.0;
        if (!num(key, d)) return;
        constexpr double kI64Limit = 0x1p63;
        if (d != std::trunc(d) || d < -kI64Limit || d >= kI64Limit) {
            ok = false;
            return;
        }
        dst = (long long)d;
    }
    void f32(const char* key, float& dst) {
        double d = 0.0;
        if (!num(key, d)) return;
        if (d < -(double)std::numeric_limits<float>::max() ||
            d > (double)std::numeric_limits<float>::max()) {
            ok = false;
            return;
        }
        dst = (float)d;
    }
    void b(const char* key, bool& dst) {
        const JsonValue* v = find(key);
        if (!v) return;
        if (v->type != JsonValue::Type::Bool) { ok = false; return; }
        dst = v->b;
    }
    void opt_bool(const char* key, std::optional<bool>& dst) {
        const JsonValue* v = find(key);
        if (!v || v->is_null()) return;
        if (v->type != JsonValue::Type::Bool) { ok = false; return; }
        dst = v->b;
    }
    const std::vector<JsonValue>* arr(const char* key) {
        const JsonValue* v = find(key);
        if (!v) return nullptr;
        if (v->type != JsonValue::Type::Array) { ok = false; return nullptr; }
        return &v->arr;
    }
    const JsonValue* obj(const char* key) {
        const JsonValue* v = find(key);
        if (!v) return nullptr;
        if (!v->is_object()) { ok = false; return nullptr; }
        return v;
    }
};

// ---------------------------------------------------------------------------
// Enum spellings
// ---------------------------------------------------------------------------

const char* shape_kind_name(MaskShape::Kind k) {
    return k == MaskShape::Kind::Rect ? "rect" : "ellipse";
}
bool shape_kind_from(const std::string& s, MaskShape::Kind& out) {
    if (s == "ellipse") { out = MaskShape::Kind::Ellipse; return true; }
    if (s == "rect") { out = MaskShape::Kind::Rect; return true; }
    return false;
}

const char* pano_mode_name(app::Pano360Mode m) {
    switch (m) {
        case app::Pano360Mode::Faces: return "faces";
        case app::Pano360Mode::Equirect: return "equirect";
        case app::Pano360Mode::Off: break;
    }
    return "off";
}
bool pano_mode_from(const std::string& s, app::Pano360Mode& out) {
    if (s == "off") { out = app::Pano360Mode::Off; return true; }
    if (s == "faces") { out = app::Pano360Mode::Faces; return true; }
    if (s == "equirect") { out = app::Pano360Mode::Equirect; return true; }
    return false;
}

const char* photo_import_name(PhotoImport m) {
    switch (m) {
        case PhotoImport::Copy: return "copy";
        case PhotoImport::Move: return "move";
        case PhotoImport::InPlace: return "in-place";
        case PhotoImport::ConvertJpeg: break;
    }
    return "convert-jpeg";
}
bool photo_import_from(const std::string& s, PhotoImport& out) {
    if (s == "convert-jpeg") { out = PhotoImport::ConvertJpeg; return true; }
    if (s == "copy") { out = PhotoImport::Copy; return true; }
    if (s == "move") { out = PhotoImport::Move; return true; }
    if (s == "in-place") { out = PhotoImport::InPlace; return true; }
    return false;
}

// ---------------------------------------------------------------------------
// Masks
// ---------------------------------------------------------------------------

JsonValue write_mask_shape(const MaskShape& s) {
    JsonValue o = obj_v();
    add(o, "kind", str_v(shape_kind_name(s.kind)));
    add(o, "remove", bool_v(s.remove));
    add(o, "cx", num_v(s.cx));
    add(o, "cy", num_v(s.cy));
    add(o, "rx", num_v(s.rx));
    add(o, "ry", num_v(s.ry));
    return o;
}

bool read_mask_shape(const JsonValue& v, MaskShape& s) {
    if (!v.is_object()) return false;
    Reader r{v};
    if (const JsonValue* k = r.find("kind")) {
        if (k->type != JsonValue::Type::String || !shape_kind_from(k->str, s.kind)) r.ok = false;
    }
    r.b("remove", s.remove);
    r.f32("cx", s.cx);
    r.f32("cy", s.cy);
    r.f32("rx", s.rx);
    r.f32("ry", s.ry);
    return r.ok;
}

JsonValue write_frame_mask(const FrameMask& m) {
    JsonValue o = obj_v();
    JsonValue shapes = arr_v();
    for (const MaskShape& s : m.shapes) shapes.arr.push_back(write_mask_shape(s));
    add(o, "shapes", std::move(shapes));
    add(o, "image", str_v(m.image));
    return o;
}

bool read_frame_mask(const JsonValue& v, FrameMask& m) {
    if (!v.is_object()) return false;
    Reader r{v};
    m.shapes.clear();
    if (const std::vector<JsonValue>* a = r.arr("shapes")) {
        m.shapes.reserve(a->size());
        for (const JsonValue& e : *a) {
            MaskShape s;
            if (!read_mask_shape(e, s)) return false;
            m.shapes.push_back(std::move(s));
        }
    }
    r.str("image", m.image);
    return r.ok;
}

JsonValue write_frame_stencil(const FrameStencil& s) {
    JsonValue o = obj_v();
    add(o, "mask", write_frame_mask(s.mask));
    add(o, "detect_border", bool_v(s.detect_border));
    add(o, "shrink", num_v(s.shrink));
    return o;
}

bool read_frame_stencil(const JsonValue& v, FrameStencil& s) {
    if (!v.is_object()) return false;
    Reader r{v};
    if (const JsonValue* m = r.obj("mask")) {
        if (!read_frame_mask(*m, s.mask)) return false;
    }
    r.b("detect_border", s.detect_border);
    r.f32("shrink", s.shrink);
    return r.ok;
}

// ---------------------------------------------------------------------------
// Panorama
// ---------------------------------------------------------------------------

JsonValue write_eac360(const app::Eac360Layout& l) {
    JsonValue o = obj_v();
    add(o, "track_w", int_v(l.track_w));
    add(o, "track_h", int_v(l.track_h));
    add(o, "face", int_v(l.face));
    add(o, "strip", int_v(l.strip));
    return o;
}

bool read_eac360(const JsonValue& v, app::Eac360Layout& l) {
    if (!v.is_object()) return false;
    Reader r{v};
    r.i32("track_w", l.track_w);
    r.i32("track_h", l.track_h);
    r.i32("face", l.face);
    r.i32("strip", l.strip);
    return r.ok;
}

JsonValue write_pano(const app::Pano360Options& p) {
    JsonValue o = obj_v();
    add(o, "mode", str_v(pano_mode_name(p.mode)));
    add(o, "size", int_v(p.size));
    add(o, "yaw", num_v(p.yaw));
    add(o, "pitch", num_v(p.pitch));
    add(o, "roll", num_v(p.roll));
    return o;
}

bool read_pano(const JsonValue& v, app::Pano360Options& p) {
    if (!v.is_object()) return false;
    Reader r{v};
    if (const JsonValue* m = r.find("mode")) {
        if (m->type != JsonValue::Type::String || !pano_mode_from(m->str, p.mode)) r.ok = false;
    }
    r.i32("size", p.size);
    r.f32("yaw", p.yaw);
    r.f32("pitch", p.pitch);
    r.f32("roll", p.roll);
    return r.ok;
}

// ---------------------------------------------------------------------------
// Inputs
// ---------------------------------------------------------------------------

JsonValue write_sub_camera(const SubCamera& c) {
    JsonValue o = obj_v();
    add(o, "rel", str_v(c.rel));
    add(o, "camera_model", str_v(c.camera_model));
    add(o, "focal_factor", num_v(c.focal_factor));
    add(o, "rig", int_v(c.rig));
    return o;
}

bool read_sub_camera(const JsonValue& v, SubCamera& c) {
    if (!v.is_object()) return false;
    Reader r{v};
    r.str("rel", c.rel);
    r.str("camera_model", c.camera_model);
    r.f32("focal_factor", c.focal_factor);
    r.i32("rig", c.rig);
    return r.ok;
}

JsonValue write_mask_click(const MaskClick& c) {
    JsonValue o = obj_v();
    add(o, "x", num_v(c.x));
    add(o, "y", num_v(c.y));
    add(o, "positive", bool_v(c.positive));
    add(o, "object", int_v(c.object));
    add(o, "frame", int_v(c.frame));
    add(o, "position", num_v(c.position));
    add(o, "source", str_v(c.source));
    add(o, "camera", str_v(c.camera));
    return o;
}

bool read_mask_click(const JsonValue& v, MaskClick& c) {
    if (!v.is_object()) return false;
    Reader r{v};
    r.f32("x", c.x);
    r.f32("y", c.y);
    r.b("positive", c.positive);
    r.i32("object", c.object);
    r.i64("frame", c.frame);
    r.f32("position", c.position);
    r.str("source", c.source);
    r.str("camera", c.camera);
    return r.ok;
}

JsonValue write_prep_input(const PrepInput& in) {
    JsonValue o = obj_v();
    add(o, "path", str_v(in.path));
    add(o, "is_video", bool_v(in.is_video));
    add(o, "subdir", str_v(in.subdir));
    add(o, "mask_dir", str_v(in.mask_dir));
    add(o, "camera_model", str_v(in.camera_model));
    add(o, "focal_factor", num_v(in.focal_factor));
    add(o, "rig", int_v(in.rig));
    add(o, "video_tracks", int_v(in.video_tracks));
    JsonValue subs = arr_v();
    for (const SubCamera& c : in.subcameras) subs.arr.push_back(write_sub_camera(c));
    add(o, "subcameras", std::move(subs));
    add(o, "eac360", write_eac360(in.eac360));
    add(o, "stencil", write_frame_stencil(in.stencil));
    return o;
}

bool read_prep_input(const JsonValue& v, PrepInput& in) {
    if (!v.is_object()) return false;
    Reader r{v};
    r.str("path", in.path);
    r.b("is_video", in.is_video);
    r.str("subdir", in.subdir);
    r.str("mask_dir", in.mask_dir);
    r.str("camera_model", in.camera_model);
    r.f32("focal_factor", in.focal_factor);
    r.i32("rig", in.rig);
    r.i32("video_tracks", in.video_tracks);
    if (const std::vector<JsonValue>* a = r.arr("subcameras")) {
        in.subcameras.clear();
        in.subcameras.reserve(a->size());
        for (const JsonValue& e : *a) {
            SubCamera c;
            if (!read_sub_camera(e, c)) return false;
            in.subcameras.push_back(std::move(c));
        }
    }
    if (const JsonValue* e = r.obj("eac360")) {
        if (!read_eac360(*e, in.eac360)) return false;
    }
    if (const JsonValue* s = r.obj("stencil")) {
        if (!read_frame_stencil(*s, in.stencil)) return false;
    }
    return r.ok && !in.path.empty();
}

JsonValue write_inputs(const std::vector<PrepInput>& inputs) {
    JsonValue a = arr_v();
    for (const PrepInput& in : inputs) a.arr.push_back(write_prep_input(in));
    return a;
}

bool read_inputs(const JsonValue& v, std::vector<PrepInput>& inputs) {
    if (!v.is_array()) return false;
    inputs.clear();
    inputs.reserve(v.arr.size());
    for (const JsonValue& e : v.arr) {
        PrepInput in;
        if (!read_prep_input(e, in)) return false;
        inputs.push_back(std::move(in));
    }
    return true;
}

JsonValue write_mask_clicks(const std::vector<MaskClick>& clicks) {
    JsonValue a = arr_v();
    for (const MaskClick& c : clicks) a.arr.push_back(write_mask_click(c));
    return a;
}

bool read_mask_clicks(const JsonValue& v, std::vector<MaskClick>& clicks) {
    if (!v.is_array()) return false;
    clicks.clear();
    clicks.reserve(v.arr.size());
    for (const JsonValue& e : v.arr) {
        MaskClick c;
        if (!read_mask_click(e, c)) return false;
        clicks.push_back(std::move(c));
    }
    return true;
}

// ---------------------------------------------------------------------------
// PrepJob
// ---------------------------------------------------------------------------

JsonValue write_prep_job(const PrepJob& j) {
    JsonValue o = obj_v();
    add(o, "inputs", write_inputs(j.inputs));
    add(o, "workspace", str_v(j.workspace));
    add(o, "resume", bool_v(j.resume));
    add(o, "redo_frames", bool_v(j.redo_frames));
    add(o, "redo_masks", bool_v(j.redo_masks));
    add(o, "flip_found_masks", bool_v(j.flip_found_masks));
    add(o, "photo_import", str_v(photo_import_name(j.photo_import)));
    add(o, "sync_tracks", bool_v(j.sync_tracks));
    add(o, "auto_rotate", bool_v(j.auto_rotate));
    add(o, "force_external_decode", bool_v(j.force_external_decode));
    add(o, "mask_enable", bool_v(j.mask_enable));
    add(o, "mask_keep_subject", bool_v(j.mask_keep_subject));
    add(o, "mask_memory", bool_v(j.mask_memory));
    add(o, "force_external_masking", bool_v(j.force_external_masking));
    add(o, "pano", write_pano(j.pano));
    add(o, "video_fps", num_v(j.video_fps));
    add(o, "sharp_window", int_v(j.sharp_window));
    add(o, "max_frames", int_v(j.max_frames));
    add(o, "ffmpeg_exe", str_v(j.ffmpeg_exe));
    add(o, "image_gamut", str_v(j.image_gamut));
    add(o, "image_is_linear", opt_bool_v(j.image_is_linear));
    add(o, "mask_prompt", str_v(j.mask_prompt));
    add(o, "mask_negative_prompt", str_v(j.mask_negative_prompt));
    add(o, "mask_dilate_ratio", num_v(j.mask_dilate_ratio));
    add(o, "mask_max_image_size", int_v(j.mask_max_image_size));
    add(o, "mask_threshold", num_v(j.mask_threshold));
    add(o, "mask_nms", num_v(j.mask_nms));
    add(o, "mask_detect_every", int_v(j.mask_detect_every));
    add(o, "mask_memory_frames", int_v(j.mask_memory_frames));
    add(o, "mask_clicks", write_mask_clicks(j.mask_clicks));
    add(o, "mask_model_path", str_v(j.mask_model_path));
    add(o, "mask_model_name", str_v(j.mask_model_name));
    add(o, "python_exe", str_v(j.python_exe));
    return o;
}

bool read_prep_job(const JsonValue& v, PrepJob& j) {
    if (!v.is_object()) return false;
    Reader r{v};
    if (const JsonValue* a = r.find("inputs")) {
        if (!read_inputs(*a, j.inputs)) return false;
    }
    r.str("workspace", j.workspace);
    r.b("resume", j.resume);
    r.b("redo_frames", j.redo_frames);
    r.b("redo_masks", j.redo_masks);
    r.b("flip_found_masks", j.flip_found_masks);
    if (const JsonValue* pi = r.find("photo_import")) {
        if (pi->type != JsonValue::Type::String || !photo_import_from(pi->str, j.photo_import))
            r.ok = false;
    }
    r.b("sync_tracks", j.sync_tracks);
    r.b("auto_rotate", j.auto_rotate);
    r.b("force_external_decode", j.force_external_decode);
    r.b("mask_enable", j.mask_enable);
    r.b("mask_keep_subject", j.mask_keep_subject);
    r.b("mask_memory", j.mask_memory);
    r.b("force_external_masking", j.force_external_masking);
    if (const JsonValue* p = r.obj("pano")) {
        if (!read_pano(*p, j.pano)) return false;
    }
    r.f32("video_fps", j.video_fps);
    r.i32("sharp_window", j.sharp_window);
    r.i32("max_frames", j.max_frames);
    r.str("ffmpeg_exe", j.ffmpeg_exe);
    r.str("image_gamut", j.image_gamut);
    r.opt_bool("image_is_linear", j.image_is_linear);
    r.str("mask_prompt", j.mask_prompt);
    r.str("mask_negative_prompt", j.mask_negative_prompt);
    r.f32("mask_dilate_ratio", j.mask_dilate_ratio);
    r.i32("mask_max_image_size", j.mask_max_image_size);
    r.f32("mask_threshold", j.mask_threshold);
    r.f32("mask_nms", j.mask_nms);
    r.i32("mask_detect_every", j.mask_detect_every);
    r.i32("mask_memory_frames", j.mask_memory_frames);
    if (const std::vector<JsonValue>* a = r.arr("mask_clicks")) {
        j.mask_clicks.clear();
        j.mask_clicks.reserve(a->size());
        for (const JsonValue& e : *a) {
            MaskClick c;
            if (!read_mask_click(e, c)) return false;
            j.mask_clicks.push_back(std::move(c));
        }
    }
    r.str("mask_model_path", j.mask_model_path);
    r.str("mask_model_name", j.mask_model_name);
    r.str("python_exe", j.python_exe);
    return r.ok;
}

// ---------------------------------------------------------------------------
// GeometryJob
// ---------------------------------------------------------------------------

JsonValue write_geometry_job(const GeometryJob& g) {
    JsonValue o = obj_v();
    add(o, "enable", bool_v(g.enable));
    add(o, "model", str_v(g.model));
    add(o, "max_size", int_v(g.max_size));
    add(o, "num_tokens", int_v(g.num_tokens));
    add(o, "want_normal", bool_v(g.want_normal));
    add(o, "want_depth", bool_v(g.want_depth));
    add(o, "normal_jpg", bool_v(g.normal_jpg));
    add(o, "jpeg_quality", int_v(g.jpeg_quality));
    add(o, "depth_mm", bool_v(g.depth_mm));
    add(o, "ray_depth", int_v(g.ray_depth));
    add(o, "split", int_v(g.split));
    add(o, "overwrite", bool_v(g.overwrite));
    add(o, "image_gamut", str_v(g.image_gamut));
    add(o, "image_is_linear", opt_bool_v(g.image_is_linear));
    return o;
}

bool read_geometry_job(const JsonValue& v, GeometryJob& g) {
    if (!v.is_object()) return false;
    Reader r{v};
    r.b("enable", g.enable);
    r.str("model", g.model);
    r.i32("max_size", g.max_size);
    r.i32("num_tokens", g.num_tokens);
    r.b("want_normal", g.want_normal);
    r.b("want_depth", g.want_depth);
    r.b("normal_jpg", g.normal_jpg);
    r.i32("jpeg_quality", g.jpeg_quality);
    r.b("depth_mm", g.depth_mm);
    r.i32("ray_depth", g.ray_depth);
    r.i32("split", g.split);
    r.b("overwrite", g.overwrite);
    r.str("image_gamut", g.image_gamut);
    r.opt_bool("image_is_linear", g.image_is_linear);
    return r.ok;
}

// ---------------------------------------------------------------------------
// SfmJob
// ---------------------------------------------------------------------------

JsonValue write_sfm_job(const SfmJob& j) {
    JsonValue o = obj_v();
    add(o, "prep", write_prep_job(j.prep));
    add(o, "geometry", write_geometry_job(j.geometry));
    add(o, "redo_model", bool_v(j.redo_model));
    add(o, "quality", int_v(j.quality));
    add(o, "data_type", int_v(j.data_type));
    add(o, "camera_model", str_v(j.camera_model));
    add(o, "camera_mode", int_v(j.camera_mode));
    add(o, "pairs", int_v(j.pairs));
    add(o, "overlap", int_v(j.overlap));
    add(o, "loop_closure", bool_v(j.loop_closure));
    add(o, "init_focal_px", num_v(j.init_focal_px));
    add(o, "init_distortion", str_v(j.init_distortion));
    add(o, "distortion_refine", int_v(j.distortion_refine));
    add(o, "final_per_image_intrinsics", bool_v(j.final_per_image_intrinsics));
    add(o, "final_free_rig", bool_v(j.final_free_rig));
    add(o, "max_features", int_v(j.max_features));
    add(o, "max_image_size", int_v(j.max_image_size));
    add(o, "mapper", int_v(j.mapper));
    add(o, "features", int_v(j.features));
    add(o, "matcher", int_v(j.matcher));
    add(o, "metric_gps", int_v(j.metric_gps));
    add(o, "sensor_gauge", int_v(j.sensor_gauge));
    add(o, "keep_intermediate", bool_v(j.keep_intermediate));
    add(o, "ba_cpu", bool_v(j.ba_cpu));
    add(o, "subprocess", bool_v(j.subprocess));
    add(o, "image_gamut", str_v(j.image_gamut));
    add(o, "image_is_linear", opt_bool_v(j.image_is_linear));
    add(o, "point_color_in_image_space", bool_v(j.point_color_in_image_space));
    add(o, "extra_args", str_v(j.extra_args));
    return o;
}

bool read_sfm_job(const JsonValue& v, SfmJob& j) {
    if (!v.is_object()) return false;
    Reader r{v};
    if (const JsonValue* p = r.obj("prep")) {
        if (!read_prep_job(*p, j.prep)) return false;
    }
    if (const JsonValue* g = r.obj("geometry")) {
        if (!read_geometry_job(*g, j.geometry)) return false;
    }
    r.b("redo_model", j.redo_model);
    r.i32("quality", j.quality);
    r.i32("data_type", j.data_type);
    r.str("camera_model", j.camera_model);
    r.i32("camera_mode", j.camera_mode);
    r.i32("pairs", j.pairs);
    r.i32("overlap", j.overlap);
    r.b("loop_closure", j.loop_closure);
    r.f32("init_focal_px", j.init_focal_px);
    r.str("init_distortion", j.init_distortion);
    r.i32("distortion_refine", j.distortion_refine);
    r.b("final_per_image_intrinsics", j.final_per_image_intrinsics);
    r.b("final_free_rig", j.final_free_rig);
    r.i32("max_features", j.max_features);
    r.i32("max_image_size", j.max_image_size);
    r.i32("mapper", j.mapper);
    r.i32("features", j.features);
    r.i32("matcher", j.matcher);
    r.i32("metric_gps", j.metric_gps);
    r.i32("sensor_gauge", j.sensor_gauge);
    r.b("keep_intermediate", j.keep_intermediate);
    r.b("ba_cpu", j.ba_cpu);
    r.b("subprocess", j.subprocess);
    r.str("image_gamut", j.image_gamut);
    r.opt_bool("image_is_linear", j.image_is_linear);
    r.b("point_color_in_image_space", j.point_color_in_image_space);
    r.str("extra_args", j.extra_args);
    return r.ok;
}

// ---------------------------------------------------------------------------
// ColmapJob
// ---------------------------------------------------------------------------

JsonValue write_colmap_job(const ColmapJob& j) {
    JsonValue o = obj_v();
    add(o, "inputs", write_inputs(j.inputs));
    add(o, "workspace", str_v(j.workspace));
    add(o, "resume", bool_v(j.resume));
    add(o, "colmap_exe", str_v(j.colmap_exe));
    add(o, "ffmpeg_exe", str_v(j.ffmpeg_exe));
    add(o, "python_exe", str_v(j.python_exe));
    add(o, "force_external_decode", bool_v(j.force_external_decode));
    add(o, "force_external_masking", bool_v(j.force_external_masking));
    add(o, "redo_frames", bool_v(j.redo_frames));
    add(o, "redo_masks", bool_v(j.redo_masks));
    add(o, "redo_model", bool_v(j.redo_model));
    add(o, "photo_import", str_v(photo_import_name(j.photo_import)));
    add(o, "camera_model", str_v(j.camera_model));
    add(o, "camera_mode", int_v(j.camera_mode));
    add(o, "init_focal_factor", num_v(j.init_focal_factor));
    add(o, "camera_params", str_v(j.camera_params));
    add(o, "feature_type", int_v(j.feature_type));
    add(o, "lightglue", bool_v(j.lightglue));
    add(o, "quality", int_v(j.quality));
    add(o, "matcher", int_v(j.matcher));
    add(o, "seq_loop_closure", bool_v(j.seq_loop_closure));
    add(o, "video_fps", num_v(j.video_fps));
    add(o, "sharp_window", int_v(j.sharp_window));
    add(o, "pano", write_pano(j.pano));
    add(o, "max_frames", int_v(j.max_frames));
    add(o, "max_num_features", int_v(j.max_num_features));
    add(o, "max_image_size", int_v(j.max_image_size));
    add(o, "seq_overlap", int_v(j.seq_overlap));
    add(o, "seq_quadratic_overlap", bool_v(j.seq_quadratic_overlap));
    add(o, "estimate_affine_shape", bool_v(j.estimate_affine_shape));
    add(o, "ba_use_gpu", bool_v(j.ba_use_gpu));
    add(o, "mapper_extra_params", int_v(j.mapper_extra_params));
    add(o, "min_num_matches", int_v(j.min_num_matches));
    add(o, "match_max_ratio", num_v(j.match_max_ratio));
    add(o, "min_inliers_per_pair", int_v(j.min_inliers_per_pair));
    add(o, "abs_pose_min_num_inliers", int_v(j.abs_pose_min_num_inliers));
    add(o, "abs_pose_min_inlier_ratio", num_v(j.abs_pose_min_inlier_ratio));
    add(o, "abs_pose_max_error", num_v(j.abs_pose_max_error));
    add(o, "merge_models", bool_v(j.merge_models));
    add(o, "final_bundle_adjust", bool_v(j.final_bundle_adjust));
    add(o, "vocab_tree_path", str_v(j.vocab_tree_path));
    add(o, "mask_enable", bool_v(j.mask_enable));
    add(o, "mask_prompt", str_v(j.mask_prompt));
    add(o, "mask_negative_prompt", str_v(j.mask_negative_prompt));
    add(o, "mask_keep_subject", bool_v(j.mask_keep_subject));
    add(o, "mask_model_path", str_v(j.mask_model_path));
    add(o, "mask_model", str_v(j.mask_model));
    add(o, "mask_max_image_size", int_v(j.mask_max_image_size));
    add(o, "mask_dilate_ratio", num_v(j.mask_dilate_ratio));
    add(o, "mask_threshold", num_v(j.mask_threshold));
    add(o, "mask_nms", num_v(j.mask_nms));
    add(o, "mask_memory", bool_v(j.mask_memory));
    add(o, "mask_detect_every", int_v(j.mask_detect_every));
    add(o, "mask_memory_frames", int_v(j.mask_memory_frames));
    add(o, "mask_clicks", write_mask_clicks(j.mask_clicks));
    add(o, "image_gamut", str_v(j.image_gamut));
    add(o, "image_is_linear", opt_bool_v(j.image_is_linear));
    add(o, "geometry", write_geometry_job(j.geometry));
    return o;
}

bool read_colmap_job(const JsonValue& v, ColmapJob& j) {
    if (!v.is_object()) return false;
    Reader r{v};
    if (const JsonValue* a = r.find("inputs")) {
        if (!read_inputs(*a, j.inputs)) return false;
    }
    r.str("workspace", j.workspace);
    r.b("resume", j.resume);
    r.str("colmap_exe", j.colmap_exe);
    r.str("ffmpeg_exe", j.ffmpeg_exe);
    r.str("python_exe", j.python_exe);
    r.b("force_external_decode", j.force_external_decode);
    r.b("force_external_masking", j.force_external_masking);
    r.b("redo_frames", j.redo_frames);
    r.b("redo_masks", j.redo_masks);
    r.b("redo_model", j.redo_model);
    if (const JsonValue* pi = r.find("photo_import")) {
        if (pi->type != JsonValue::Type::String || !photo_import_from(pi->str, j.photo_import))
            r.ok = false;
    }
    r.str("camera_model", j.camera_model);
    r.i32("camera_mode", j.camera_mode);
    r.f32("init_focal_factor", j.init_focal_factor);
    r.str("camera_params", j.camera_params);
    r.i32("feature_type", j.feature_type);
    r.b("lightglue", j.lightglue);
    r.i32("quality", j.quality);
    r.i32("matcher", j.matcher);
    r.b("seq_loop_closure", j.seq_loop_closure);
    r.f32("video_fps", j.video_fps);
    r.i32("sharp_window", j.sharp_window);
    if (const JsonValue* p = r.obj("pano")) {
        if (!read_pano(*p, j.pano)) return false;
    }
    r.i32("max_frames", j.max_frames);
    r.i32("max_num_features", j.max_num_features);
    r.i32("max_image_size", j.max_image_size);
    r.i32("seq_overlap", j.seq_overlap);
    r.b("seq_quadratic_overlap", j.seq_quadratic_overlap);
    r.b("estimate_affine_shape", j.estimate_affine_shape);
    r.b("ba_use_gpu", j.ba_use_gpu);
    r.i32("mapper_extra_params", j.mapper_extra_params);
    r.i32("min_num_matches", j.min_num_matches);
    r.f32("match_max_ratio", j.match_max_ratio);
    r.i32("min_inliers_per_pair", j.min_inliers_per_pair);
    r.i32("abs_pose_min_num_inliers", j.abs_pose_min_num_inliers);
    r.f32("abs_pose_min_inlier_ratio", j.abs_pose_min_inlier_ratio);
    r.f32("abs_pose_max_error", j.abs_pose_max_error);
    r.b("merge_models", j.merge_models);
    r.b("final_bundle_adjust", j.final_bundle_adjust);
    r.str("vocab_tree_path", j.vocab_tree_path);
    r.b("mask_enable", j.mask_enable);
    r.str("mask_prompt", j.mask_prompt);
    r.str("mask_negative_prompt", j.mask_negative_prompt);
    r.b("mask_keep_subject", j.mask_keep_subject);
    r.str("mask_model_path", j.mask_model_path);
    r.str("mask_model", j.mask_model);
    r.i32("mask_max_image_size", j.mask_max_image_size);
    r.f32("mask_dilate_ratio", j.mask_dilate_ratio);
    r.f32("mask_threshold", j.mask_threshold);
    r.f32("mask_nms", j.mask_nms);
    r.b("mask_memory", j.mask_memory);
    r.i32("mask_detect_every", j.mask_detect_every);
    r.i32("mask_memory_frames", j.mask_memory_frames);
    if (const std::vector<JsonValue>* a = r.arr("mask_clicks")) {
        j.mask_clicks.clear();
        j.mask_clicks.reserve(a->size());
        for (const JsonValue& e : *a) {
            MaskClick c;
            if (!read_mask_click(e, c)) return false;
            j.mask_clicks.push_back(std::move(c));
        }
    }
    r.str("image_gamut", j.image_gamut);
    r.opt_bool("image_is_linear", j.image_is_linear);
    if (const JsonValue* g = r.obj("geometry")) {
        if (!read_geometry_job(*g, j.geometry)) return false;
    }
    return r.ok;
}

// ---------------------------------------------------------------------------
// Publication
// ---------------------------------------------------------------------------

void remove_file(const fs::path& p) noexcept {
    std::error_code ec;
    fs::remove(p, ec);
}

// Temp file then atomic replace, so a failed write never damages the record
// already on disk.
bool publish(const fs::path& path, const std::string& text, std::string& error) {
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            error = "cannot write " + tmp.string();
            remove_file(tmp);
            return false;
        }
        f << text;
        f.close();
        if (f.fail()) {
            error = "cannot write " + tmp.string();
            remove_file(tmp);
            return false;
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "cannot replace " + path.string();
        remove_file(tmp);
        return false;
    }
#else
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        error = "cannot replace " + path.string() + ": " + ec.message();
        remove_file(tmp);
        return false;
    }
#endif
    return true;
}

}  // namespace

std::optional<State> load() {
    try {
        const fs::path path = record_path();
        std::error_code ec;
        if (!fs::exists(path, ec)) return std::nullopt;

        JsonValue root;
        try {
            root = json_parse_file(path.string());
        } catch (...) {
            return std::nullopt;
        }
        if (!root.is_object()) return std::nullopt;

        const JsonValue* ver = root.find("version");
        const JsonValue* eng = root.find("engine");
        const JsonValue* cur = root.find("current");
        const JsonValue* done = root.find("completed");
        if (!ver || !eng || !cur || !done || !done->is_array())
            return std::nullopt;

        Reader r{root};
        int version = 0;
        int engine = 0;
        int current = (int)Stage::Frames;
        r.i32("version", version);
        r.i32("engine", engine);
        r.i32("current", current);
        if (!r.ok || version != kVersion ||
            (engine != kEngineSfm && engine != kEngineColmap) ||
            current < 0 || current >= kNumStages)
            return std::nullopt;

        const std::vector<JsonValue>* a = r.arr("completed");
        if (!r.ok || !a || a->size() != (size_t)kNumStages)
            return std::nullopt;

        State st;
        st.engine = engine;
        st.current = (Stage)current;
        for (size_t i = 0; i < a->size(); i++) {
            if ((*a)[i].type != JsonValue::Type::Bool) return std::nullopt;
            st.completed[i] = (*a)[i].b;
        }

        const JsonValue* job =
            root.find(engine == kEngineColmap ? "colmap" : "sfm");
        if (!job || !job->is_object()) return std::nullopt;
        if (engine == kEngineColmap) {
            if (!read_colmap_job(*job, st.colmap)) return std::nullopt;
            if (st.colmap.workspace.empty() ||
                st.colmap.workspace.find('\0') != std::string::npos ||
                st.colmap.inputs.empty())
                return std::nullopt;
        } else {
            if (!read_sfm_job(*job, st.sfm)) return std::nullopt;
            if (st.sfm.prep.workspace.empty() ||
                st.sfm.prep.workspace.find('\0') != std::string::npos ||
                st.sfm.prep.inputs.empty())
                return std::nullopt;
        }
        return st;
    } catch (...) {
        return std::nullopt;
    }
}

bool save(const State& state, std::string& error) {
    error.clear();
    const bool valid_engine =
        state.engine == kEngineSfm || state.engine == kEngineColmap;
    const bool valid_stage =
        (int)state.current >= 0 && (int)state.current < kNumStages;
    if (!valid_engine) {
        error = "unknown engine";
        return false;
    }
    if (!valid_stage) {
        error = "stage out of range";
        return false;
    }

    const auto valid_inputs = [](const std::vector<PrepInput>& inputs) {
        if (inputs.empty()) return false;
        for (const PrepInput& in : inputs)
            if (in.path.empty() || in.path.find('\0') != std::string::npos)
                return false;
        return true;
    };
    const std::string& workspace =
        state.engine == kEngineColmap ? state.colmap.workspace
                                      : state.sfm.prep.workspace;
    const std::vector<PrepInput>& inputs =
        state.engine == kEngineColmap ? state.colmap.inputs
                                      : state.sfm.prep.inputs;
    if (workspace.empty() || workspace.find('\0') != std::string::npos ||
        !valid_inputs(inputs)) {
        error = "dataset job is incomplete";
        return false;
    }

    JsonValue root = obj_v();
    add(root, "version", int_v(kVersion));
    add(root, "engine", int_v(state.engine));
    add(root, "current", int_v((int)state.current));
    JsonValue done = arr_v();
    for (int i = 0; i < kNumStages; i++)
        done.arr.push_back(bool_v(state.completed[(size_t)i]));
    add(root, "completed", std::move(done));
    if (state.engine == kEngineColmap)
        add(root, "colmap", write_colmap_job(state.colmap));
    else
        add(root, "sfm", write_sfm_job(state.sfm));
    return publish(record_path(), json_write(root), error);
}

void clear() noexcept {
    try {
        remove_file(record_path());
    } catch (...) {
    }
}

}  // namespace gui::dataset_recovery
