// WorkerRequest -- one scheduled phase, JSON codec.
// Validation stops malformed requests before the worker changes state.

#include "app/WorkerRequest.h"

#include "data/Json.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;

namespace app::worker {
namespace {

constexpr size_t kMaxArgs = 65536;
constexpr size_t kMaxPayload = 32u * 1024u * 1024u;

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + path);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

bool has_nul(const std::string& s) { return s.find('\0') != std::string::npos; }

void reject_nul(const std::string& value, const char* key) {
    if (has_nul(value))
        throw std::runtime_error(std::string("request: \"") + key +
                                 " contains an embedded NUL");
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", (unsigned)c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

std::string quote(const std::string& s) { return "\"" + json_escape(s) + "\""; }

const JsonValue* required(const JsonValue& v, const char* key) {
    const JsonValue* p = v.find(key);
    if (!p) throw std::runtime_error(std::string("payload: missing \"") + key + "\"");
    return p;
}

const JsonValue* optional(const JsonValue& v, const char* key) { return v.find(key); }

std::string string_value(const JsonValue& v, const char* key) {
    const JsonValue* p = required(v, key);
    if (p->type != JsonValue::Type::String)
        throw std::runtime_error(std::string("payload: \"") + key + "\" must be a string");
    if (has_nul(p->str))
        throw std::runtime_error(std::string("payload: \"") + key + "\" contains an embedded NUL");
    return p->str;
}

std::string optional_string(const JsonValue& v, const char* key, std::string def = {}) {
    const JsonValue* p = optional(v, key);
    if (!p) return def;
    if (p->type != JsonValue::Type::String)
        throw std::runtime_error(std::string("payload: \"") + key + "\" must be a string");
    if (has_nul(p->str))
        throw std::runtime_error(std::string("payload: \"") + key + "\" contains an embedded NUL");
    return p->str;
}

bool bool_value(const JsonValue& v, const char* key) {
    const JsonValue* p = required(v, key);
    if (p->type != JsonValue::Type::Bool)
        throw std::runtime_error(std::string("payload: \"") + key + "\" must be a bool");
    return p->b;
}

bool optional_bool(const JsonValue& v, const char* key, bool def = false) {
    const JsonValue* p = optional(v, key);
    if (!p) return def;
    if (p->type != JsonValue::Type::Bool)
        throw std::runtime_error(std::string("payload: \"") + key + "\" must be a bool");
    return p->b;
}

double number_value(const JsonValue& v, const char* key) {
    const JsonValue* p = required(v, key);
    if (p->type != JsonValue::Type::Number || !std::isfinite(p->num))
        throw std::runtime_error(std::string("payload: \"") + key + "\" must be finite");
    return p->num;
}

double optional_number(const JsonValue& v, const char* key, double def = 0.0) {
    const JsonValue* p = optional(v, key);
    if (!p) return def;
    if (p->type != JsonValue::Type::Number || !std::isfinite(p->num))
        throw std::runtime_error(std::string("payload: \"") + key + "\" must be finite");
    return p->num;
}

int int_value(const JsonValue& v, const char* key, int min = std::numeric_limits<int>::min(),
              int max = std::numeric_limits<int>::max()) {
    const double n = number_value(v, key);
    if (std::trunc(n) != n || n < (double)min || n > (double)max)
        throw std::runtime_error(std::string("payload: \"") + key + "\" is out of range");
    return (int)n;
}

int optional_int(const JsonValue& v, const char* key, int def,
                 int min = std::numeric_limits<int>::min(),
                 int max = std::numeric_limits<int>::max()) {
    const JsonValue* p = optional(v, key);
    if (!p) return def;
    if (p->type != JsonValue::Type::Number || !std::isfinite(p->num) ||
        std::trunc(p->num) != p->num || p->num < (double)min || p->num > (double)max)
        throw std::runtime_error(std::string("payload: \"") + key + "\" is out of range");
    return (int)p->num;
}

float float_value(const JsonValue& v, const char* key, float min, float max) {
    const double n = number_value(v, key);
    if (n < min || n > max)
        throw std::runtime_error(std::string("payload: \"") + key + "\" is out of range");
    return (float)n;
}

float optional_float(const JsonValue& v, const char* key, float def, float min, float max) {
    const JsonValue* p = optional(v, key);
    if (!p) return def;
    if (p->type != JsonValue::Type::Number || !std::isfinite(p->num) ||
        p->num < min || p->num > max)
        throw std::runtime_error(std::string("payload: \"") + key + "\" is out of range");
    return (float)p->num;
}

const JsonValue& object_value(const JsonValue& v, const char* key) {
    const JsonValue* p = required(v, key);
    if (p->type != JsonValue::Type::Object)
        throw std::runtime_error(std::string("payload: \"") + key + "\" must be an object");
    return *p;
}

const JsonValue& array_value(const JsonValue& v, const char* key) {
    const JsonValue* p = required(v, key);
    if (p->type != JsonValue::Type::Array)
        throw std::runtime_error(std::string("payload: \"") + key + "\" must be an array");
    return *p;
}

std::vector<std::string> strings_value(const JsonValue& v, const char* key) {
    const JsonValue& a = array_value(v, key);
    std::vector<std::string> out;
    out.reserve(a.arr.size());
    for (const JsonValue& item : a.arr) {
        if (item.type != JsonValue::Type::String || has_nul(item.str))
            throw std::runtime_error(std::string("payload: \"") + key + " must contain strings");
        out.push_back(item.str);
    }
    return out;
}

bool generated_id(const std::string& value, const char* prefix) {
    constexpr size_t digits = 16;
    const size_t n = std::char_traits<char>::length(prefix);
    if (value.size() != n + digits || value.compare(0, n, prefix) != 0) return false;
    for (size_t i = n; i < value.size(); ++i) {
        const char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool is_auto_device(const std::string& device) {
    std::string s = device;
    size_t first = s.find_first_not_of(" \t\r\n");
    size_t last = s.find_last_not_of(" \t\r\n");
    s = first == std::string::npos ? std::string() : s.substr(first, last - first + 1);
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s.empty() || s == "auto" || s == "-1";
}

fs::path canonical_parent(const fs::path& path, const char* key) {
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(path, ec);
    if (ec)
        throw std::runtime_error(std::string("request: cannot canonicalize ") + key + ": " + ec.message());
    return canonical.parent_path();
}

void add_string(std::string& out, const char* key, const std::string& value, bool& first) {
    if (!first) out += ',';
    first = false;
    out += quote(key) + ':' + quote(value);
}
void add_bool(std::string& out, const char* key, bool value, bool& first) {
    if (!first) out += ',';
    first = false;
    out += quote(key) + (value ? ":true" : ":false");
}
void add_int(std::string& out, const char* key, long long value, bool& first) {
    if (!first) out += ',';
    first = false;
    out += quote(key) + ':' + std::to_string(value);
}
void add_num(std::string& out, const char* key, double value, bool& first) {
    if (!std::isfinite(value)) throw std::runtime_error("payload: non-finite number");
    if (!first) out += ',';
    first = false;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.17g", value);
    out += quote(key) + ':' + buf;
}
void add_json(std::string& out, const char* key, const std::string& value, bool& first) {
    if (!first) out += ',';
    first = false;
    out += quote(key) + ':' + value;
}
void add_strings(std::string& out, const char* key, const std::vector<std::string>& values,
                bool& first) {
    if (!first) out += ',';
    first = false;
    out += quote(key) + ":[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out += ',';
        out += quote(values[i]);
    }
    out += ']';
}

std::string mask_shape_json(const app::MaskShape& s) {
    std::string out = "{";
    bool first = true;
    add_int(out, "kind", s.kind == app::MaskShape::Kind::Rect ? 1 : 0, first);
    add_bool(out, "remove", s.remove, first);
    add_num(out, "cx", s.cx, first); add_num(out, "cy", s.cy, first);
    add_num(out, "rx", s.rx, first); add_num(out, "ry", s.ry, first);
    out += '}';
    return out;
}

std::string frame_mask_json(const app::FrameMask& m) {
    std::string out = "{";
    bool first = true;
    if (!first) out += ',';
    first = false;
    out += "\"shapes\":[";
    for (size_t i = 0; i < m.shapes.size(); ++i) {
        if (i) out += ',';
        out += mask_shape_json(m.shapes[i]);
    }
    out += ']';
    add_string(out, "image", m.image, first);
    out += '}';
    return out;
}

std::string stencil_json(const app::FrameStencil& s) {
    std::string out = "{";
    bool first = true;
    add_json(out, "mask", frame_mask_json(s.mask), first);
    add_bool(out, "detect_border", s.detect_border, first);
    add_num(out, "shrink", s.shrink, first);
    out += '}';
    return out;
}

std::string subcamera_json(const app::SubCamera& s) {
    std::string out = "{";
    bool first = true;
    add_string(out, "rel", s.rel, first);
    add_string(out, "camera_model", s.camera_model, first);
    add_num(out, "focal_factor", s.focal_factor, first);
    add_int(out, "rig", s.rig, first);
    out += '}';
    return out;
}

std::string click_json(const app::MaskClick& c) {
    std::string out = "{";
    bool first = true;
    add_num(out, "x", c.x, first); add_num(out, "y", c.y, first);
    add_bool(out, "positive", c.positive, first); add_int(out, "object", c.object, first);
    add_int(out, "frame", c.frame, first); add_num(out, "position", c.position, first);
    add_string(out, "source", c.source, first); add_string(out, "camera", c.camera, first);
    out += '}';
    return out;
}

std::string input_json(const app::PrepInput& in) {
    std::string out = "{";
    bool first = true;
    add_string(out, "path", in.path, first); add_bool(out, "is_video", in.is_video, first);
    add_string(out, "subdir", in.subdir, first); add_string(out, "mask_dir", in.mask_dir, first);
    add_string(out, "camera_model", in.camera_model, first);
    add_num(out, "focal_factor", in.focal_factor, first); add_int(out, "rig", in.rig, first);
    add_int(out, "video_tracks", in.video_tracks, first);
    if (!first) out += ',';
    first = false; out += "\"subcameras\":[";
    for (size_t i = 0; i < in.subcameras.size(); ++i) {
        if (i) out += ',';
        out += subcamera_json(in.subcameras[i]);
    }
    out += ']';
    std::string pano = "{";
    bool pf = true;
    add_int(pano, "packing", (int)in.pano360.packing, pf);
    add_int(pano, "track_w", in.pano360.track_w, pf);
    add_int(pano, "track_h", in.pano360.track_h, pf);
    add_int(pano, "face", in.pano360.face, pf);
    add_int(pano, "strip", in.pano360.strip, pf);
    add_int(pano, "margin", in.pano360.margin, pf);
    pano += '}';
    add_json(out, "pano360", pano, first);
    add_bool(out, "pano360_unsupported", in.pano360_unsupported, first);
    add_json(out, "stencil", stencil_json(in.stencil), first);
    out += '}';
    return out;
}

std::string prep_json(const app::PrepJob& job) {
    std::string out = "{";
    bool first = true;
    if (!first) out += ',';
    first = false; out += "\"inputs\":[";
    for (size_t i = 0; i < job.inputs.size(); ++i) {
        if (i) out += ',';
        out += input_json(job.inputs[i]);
    }
    out += ']';
    add_string(out, "workspace", job.workspace, first);
    add_bool(out, "resume", job.resume, first);
    add_bool(out, "redo_frames", job.redo_frames, first);
    add_bool(out, "redo_masks", job.redo_masks, first);
    add_bool(out, "flip_found_masks", job.flip_found_masks, first);
    add_int(out, "photo_import", (int)job.photo_import, first);
    add_string(out, "device", job.device, first);
    std::string pano = "{";
    bool pf = true;
    add_int(pano, "mode", (int)job.pano.mode, pf); add_int(pano, "size", job.pano.size, pf);
    add_num(pano, "yaw", job.pano.yaw, pf); add_num(pano, "pitch", job.pano.pitch, pf);
    add_num(pano, "roll", job.pano.roll, pf); pano += '}';
    add_json(out, "pano", pano, first);
    add_num(out, "video_fps", job.video_fps, first);
    add_bool(out, "adaptive_fps", job.adaptive_fps, first);
    add_num(out, "adaptive_range", job.adaptive_range, first);
    add_int(out, "sharp_window", job.sharp_window, first);
    add_bool(out, "sync_tracks", job.sync_tracks, first); add_int(out, "max_frames", job.max_frames, first);
    add_bool(out, "auto_rotate", job.auto_rotate, first);
    add_bool(out, "force_external_decode", job.force_external_decode, first);
    add_string(out, "ffmpeg_exe", job.ffmpeg_exe, first);
    add_string(out, "image_gamut", job.image_gamut, first);
    if (!first) out += ',';
    first = false; out += "\"image_is_linear\":";
    if (job.image_is_linear.has_value()) out += (*job.image_is_linear ? "true" : "false"); else out += "null";
    add_bool(out, "mask_enable", job.mask_enable, first);
    add_string(out, "mask_prompt", job.mask_prompt, first);
    add_string(out, "mask_negative_prompt", job.mask_negative_prompt, first);
    add_bool(out, "mask_keep_subject", job.mask_keep_subject, first);
    add_num(out, "mask_dilate_ratio", job.mask_dilate_ratio, first);
    add_int(out, "mask_max_image_size", job.mask_max_image_size, first);
    add_num(out, "mask_threshold", job.mask_threshold, first);
    add_num(out, "mask_nms", job.mask_nms, first);
    add_bool(out, "mask_memory", job.mask_memory, first);
    add_int(out, "mask_detect_every", job.mask_detect_every, first);
    add_int(out, "mask_memory_frames", job.mask_memory_frames, first);
    if (!first) out += ',';
    first = false; out += "\"mask_clicks\":[";
    for (size_t i = 0; i < job.mask_clicks.size(); ++i) {
        if (i) out += ',';
        out += click_json(job.mask_clicks[i]);
    }
    out += ']';
    add_string(out, "mask_model_path", job.mask_model_path, first);
    add_string(out, "mask_model_name", job.mask_model_name, first);
    add_bool(out, "force_external_masking", job.force_external_masking, first);
    add_string(out, "python_exe", job.python_exe, first);
    out += '}';
    return out;
}

app::MaskShape decode_shape(const JsonValue& v) {
    if (v.type != JsonValue::Type::Object) throw std::runtime_error("payload: invalid mask shape");
    app::MaskShape s;
    const int kind = int_value(v, "kind", 0, 1);
    s.kind = kind ? app::MaskShape::Kind::Rect : app::MaskShape::Kind::Ellipse;
    s.remove = bool_value(v, "remove");
    s.cx = float_value(v, "cx", -1e6f, 1e6f); s.cy = float_value(v, "cy", -1e6f, 1e6f);
    s.rx = float_value(v, "rx", -1e6f, 1e6f); s.ry = float_value(v, "ry", -1e6f, 1e6f);
    return s;
}

app::FrameMask decode_mask(const JsonValue& v) {
    if (v.type != JsonValue::Type::Object) throw std::runtime_error("payload: invalid frame mask");
    app::FrameMask m;
    const JsonValue& a = array_value(v, "shapes");
    m.shapes.reserve(a.arr.size());
    for (const JsonValue& shape : a.arr) m.shapes.push_back(decode_shape(shape));
    m.image = string_value(v, "image");
    return m;
}

app::FrameStencil decode_stencil(const JsonValue& v) {
    if (v.type != JsonValue::Type::Object) throw std::runtime_error("payload: invalid stencil");
    app::FrameStencil s;
    s.mask = decode_mask(object_value(v, "mask"));
    s.detect_border = bool_value(v, "detect_border");
    s.shrink = float_value(v, "shrink", 0.0f, 1.0f);
    return s;
}

app::SubCamera decode_subcamera(const JsonValue& v) {
    if (v.type != JsonValue::Type::Object) throw std::runtime_error("payload: invalid subcamera");
    app::SubCamera s;
    s.rel = string_value(v, "rel"); s.camera_model = string_value(v, "camera_model");
    s.focal_factor = float_value(v, "focal_factor", 0.0f, 100.0f);
    s.rig = int_value(v, "rig", -100, 100);
    return s;
}

app::MaskClick decode_click(const JsonValue& v) {
    if (v.type != JsonValue::Type::Object) throw std::runtime_error("payload: invalid mask click");
    app::MaskClick c;
    c.x = float_value(v, "x", -1e7f, 1e7f); c.y = float_value(v, "y", -1e7f, 1e7f);
    c.positive = bool_value(v, "positive"); c.object = int_value(v, "object", 0, 1 << 30);
    const double frame = number_value(v, "frame");
    if (std::trunc(frame) != frame || frame < std::numeric_limits<long long>::min() ||
        frame > std::numeric_limits<long long>::max())
        throw std::runtime_error("payload: mask click frame is out of range");
    c.frame = (long long)frame;
    c.position = float_value(v, "position", 0.0f, 1.0f);
    c.source = string_value(v, "source"); c.camera = string_value(v, "camera");
    return c;
}

app::PrepInput decode_input(const JsonValue& v) {
    if (v.type != JsonValue::Type::Object) throw std::runtime_error("payload: invalid prep input");
    app::PrepInput in;
    in.path = string_value(v, "path"); in.is_video = bool_value(v, "is_video");
    in.subdir = string_value(v, "subdir"); in.mask_dir = string_value(v, "mask_dir");
    in.camera_model = string_value(v, "camera_model");
    in.focal_factor = float_value(v, "focal_factor", 0.0f, 100.0f);
    in.rig = int_value(v, "rig", -100, 100); in.video_tracks = int_value(v, "video_tracks", 0, 1024);
    const JsonValue& subs = array_value(v, "subcameras");
    in.subcameras.reserve(subs.arr.size());
    for (const JsonValue& sub : subs.arr) in.subcameras.push_back(decode_subcamera(sub));
    const JsonValue* pano = optional(v, "pano360");
    if (!pano) pano = required(v, "eac360");
    if (pano->type != JsonValue::Type::Object)
        throw std::runtime_error("payload: panorama layout must be an object");
    in.pano360.packing = (app::Pano360Packing)
        optional_int(*pano, "packing", 0, 0, 1);
    in.pano360.track_w = int_value(*pano, "track_w", 0, 1 << 20);
    in.pano360.track_h = int_value(*pano, "track_h", 0, 1 << 20);
    in.pano360.face = int_value(*pano, "face", 0, 1 << 20);
    in.pano360.strip = int_value(*pano, "strip", 0, 1 << 20);
    in.pano360.margin = optional_int(*pano, "margin", 0, 0, 1 << 20);
    in.pano360_unsupported = optional_bool(v, "pano360_unsupported");
    in.stencil = decode_stencil(object_value(v, "stencil"));
    return in;
}

app::PrepJob decode_prep(const JsonValue& root) {
    if (root.type != JsonValue::Type::Object) throw std::runtime_error("payload: prep root must be an object");
    app::PrepJob job;
    const JsonValue& inputs = array_value(root, "inputs");
    if (inputs.arr.empty() || inputs.arr.size() > 65536) throw std::runtime_error("payload: invalid inputs");
    job.inputs.reserve(inputs.arr.size());
    for (const JsonValue& input : inputs.arr) job.inputs.push_back(decode_input(input));
    job.workspace = string_value(root, "workspace");
    job.resume = bool_value(root, "resume"); job.redo_frames = bool_value(root, "redo_frames");
    job.redo_masks = bool_value(root, "redo_masks"); job.flip_found_masks = bool_value(root, "flip_found_masks");
    job.photo_import = (app::PhotoImport)int_value(root, "photo_import", 0, app::kNumPhotoImports - 1);
    job.device = string_value(root, "device");
    const JsonValue& pano = object_value(root, "pano");
    job.pano.mode = (app::Pano360Mode)int_value(pano, "mode", 0, 2);
    job.pano.size = int_value(pano, "size", 0, 1 << 24);
    job.pano.yaw = float_value(pano, "yaw", -360.0f, 360.0f);
    job.pano.pitch = float_value(pano, "pitch", -360.0f, 360.0f);
    job.pano.roll = float_value(pano, "roll", -360.0f, 360.0f);
    job.video_fps = float_value(root, "video_fps", 0.0f, 100000.0f);
    job.adaptive_fps = bool_value(root, "adaptive_fps");
    job.adaptive_range = float_value(root, "adaptive_range", 1.0f, 16.0f);
    job.sharp_window = int_value(root, "sharp_window", 1, 100000);
    job.sync_tracks = bool_value(root, "sync_tracks"); job.max_frames = int_value(root, "max_frames", 1, 1000000000);
    job.auto_rotate = bool_value(root, "auto_rotate"); job.force_external_decode = bool_value(root, "force_external_decode");
    job.ffmpeg_exe = string_value(root, "ffmpeg_exe"); job.image_gamut = string_value(root, "image_gamut");
    if (const JsonValue* linear = optional(root, "image_is_linear")) {
        if (linear->type == JsonValue::Type::Null) job.image_is_linear.reset();
        else if (linear->type == JsonValue::Type::Bool) job.image_is_linear = linear->b;
        else throw std::runtime_error("payload: image_is_linear must be bool or null");
    }
    job.mask_enable = bool_value(root, "mask_enable");
    job.mask_prompt = string_value(root, "mask_prompt"); job.mask_negative_prompt = string_value(root, "mask_negative_prompt");
    job.mask_keep_subject = bool_value(root, "mask_keep_subject");
    job.mask_dilate_ratio = float_value(root, "mask_dilate_ratio", 0.0f, 100.0f);
    job.mask_max_image_size = int_value(root, "mask_max_image_size", 1, 1 << 24);
    job.mask_threshold = float_value(root, "mask_threshold", 0.0f, 1.0f);
    job.mask_nms = float_value(root, "mask_nms", 0.0f, 1.0f);
    job.mask_memory = bool_value(root, "mask_memory"); job.mask_detect_every = int_value(root, "mask_detect_every", 1, 1000000);
    job.mask_memory_frames = int_value(root, "mask_memory_frames", 0, 1000000000);
    const JsonValue& clicks = array_value(root, "mask_clicks");
    job.mask_clicks.reserve(clicks.arr.size());
    for (const JsonValue& click : clicks.arr) job.mask_clicks.push_back(decode_click(click));
    job.mask_model_path = string_value(root, "mask_model_path"); job.mask_model_name = string_value(root, "mask_model_name");
    job.force_external_masking = bool_value(root, "force_external_masking"); job.python_exe = string_value(root, "python_exe");
    return job;
}

void reject_payload_nuls(const app::PrepJob& job) {
    auto check = [](const std::string& s, const char* key) {
        if (has_nul(s)) throw std::runtime_error(std::string("payload: \"") + key + " contains an embedded NUL");
    };
    check(job.workspace, "workspace"); check(job.device, "device"); check(job.ffmpeg_exe, "ffmpeg_exe");
    check(job.image_gamut, "image_gamut"); check(job.mask_prompt, "mask_prompt");
    check(job.mask_negative_prompt, "mask_negative_prompt"); check(job.mask_model_path, "mask_model_path");
    check(job.mask_model_name, "mask_model_name"); check(job.python_exe, "python_exe");
    for (const auto& in : job.inputs) {
        check(in.path, "input.path"); check(in.subdir, "input.subdir"); check(in.mask_dir, "input.mask_dir");
        check(in.camera_model, "input.camera_model"); check(in.stencil.mask.image, "stencil.image");
        for (const auto& sub : in.subcameras) { check(sub.rel, "subcamera.rel"); check(sub.camera_model, "subcamera.camera_model"); }
    }
    for (const auto& c : job.mask_clicks) { check(c.source, "mask_click.source"); check(c.camera, "mask_click.camera"); }
}

void require_absolute(const std::string& value, const char* key) {
    if (value.empty() || !fs::u8path(value).is_absolute())
        throw std::runtime_error(std::string("payload: ") + key + " must be absolute");
}

}  // namespace

Request parse_request(const std::string& path) {
    if (has_nul(path)) throw std::runtime_error("request: request path contains an embedded NUL");
    const std::string text = read_file(path);
    if (text.size() > kMaxPayload) throw std::runtime_error("request: file is too large");
    const JsonValue root = json_parse(text);
    if (root.type != JsonValue::Type::Object) throw std::runtime_error("request: root must be an object");
    Request r;
    r.request_path = path;
r.schema_version = int_value(root, "schema_version", 0, 2);
    r.job_id = string_value(root, "job_id"); r.attempt_id = string_value(root, "attempt_id");
    r.phase = string_value(root, "phase"); r.device = string_value(root, "device");
    r.device_name = optional_string(root, "device_name"); r.work_dir = string_value(root, "work_dir");
    r.workspace = optional_string(root, "workspace"); r.result_path = string_value(root, "result_path");
    r.args = strings_value(root, "args");
    if (const JsonValue* payload = optional(root, "payload")) {
        if (payload->type != JsonValue::Type::String) throw std::runtime_error("request: payload must be a JSON string");
        r.payload = payload->str;
    }
    return r;
}

void validate_request(const Request& r) {
    reject_nul(r.request_path, "request_path"); reject_nul(r.job_id, "job_id");
    reject_nul(r.attempt_id, "attempt_id"); reject_nul(r.phase, "phase");
    reject_nul(r.device, "device"); reject_nul(r.device_name, "device_name");
    reject_nul(r.work_dir, "work_dir"); reject_nul(r.workspace, "workspace");
    reject_nul(r.result_path, "result_path"); reject_nul(r.payload, "payload");
    if (r.args.size() > kMaxArgs) throw std::runtime_error("request: too many args");
    for (const std::string& arg : r.args) reject_nul(arg, "args");
    if (r.schema_version != 1 && r.schema_version != 2)
        throw std::runtime_error("request: unsupported schema_version " + std::to_string(r.schema_version));
    if (!generated_id(r.job_id, "job-")) throw std::runtime_error("request: invalid job_id");
    if (!generated_id(r.attempt_id, "att-")) throw std::runtime_error("request: invalid attempt_id");
    if (r.phase != "prep" && r.phase != "train" && r.phase != "sfm" && r.phase != "geometry")
        throw std::runtime_error("request: unsupported phase " + r.phase);
    if (r.schema_version == 1 && r.phase == "prep")
        throw std::runtime_error("request: prep requires schema_version 2");
    if (r.device.empty() || is_auto_device(r.device)) throw std::runtime_error("request: device must be resolved");
    if (r.result_path.empty() || r.request_path.empty()) throw std::runtime_error("request: path is empty");
    if (r.payload.size() > kMaxPayload) throw std::runtime_error("request: payload is too large");
    if (r.phase == "prep") {
        const JsonValue payload = json_parse(r.payload);
        if (payload.type != JsonValue::Type::Object) throw std::runtime_error("request: prep payload must be an object");
        const app::PrepJob job = decode_prep(payload);
        require_absolute(job.workspace, "workspace");
        for (const auto& in : job.inputs) require_absolute(in.path, "input.path");
    }
    const fs::path request_path = fs::u8path(r.request_path);
    const fs::path result_path = fs::u8path(r.result_path);
    const std::string expected_name = "result-" + r.attempt_id + ".json";
    if (result_path.filename().u8string() != expected_name)
        throw std::runtime_error("request: result filename does not match attempt_id");
    if (!result_path.is_absolute()) throw std::runtime_error("request: result_path must be absolute");
    if (canonical_parent(request_path, "request_path") != canonical_parent(result_path, "result_path"))
        throw std::runtime_error("request: result directory differs from request directory");
}

Result parse_result(const std::string& path) {
    const JsonValue root = json_parse(read_file(path));
    if (root.type != JsonValue::Type::Object) throw std::runtime_error("result: root must be an object");
    Result out;
    out.schema_version = int_value(root, "schema_version", 0, 2);
    out.job_id = string_value(root, "job_id");
    out.attempt_id = string_value(root, "attempt_id");
    out.phase = string_value(root, "phase");
    out.outcome = string_value(root, "outcome");
    out.exit_code = int_value(root, "exit_code", std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
    out.message = string_value(root, "message");
    if (const JsonValue* p = optional(root, "outputs")) {
        if (p->type != JsonValue::Type::Array) throw std::runtime_error("result: outputs must be an array");
        for (const JsonValue& v : p->arr) {
            if (v.type != JsonValue::Type::String || has_nul(v.str)) throw std::runtime_error("result: invalid output");
            out.outputs.push_back(v.str);
        }
    }
    auto result_count = [&](const char* key) {
        const double n = optional_number(root, key, 0);
        if (std::trunc(n) != n ||
            n < (double)std::numeric_limits<int64_t>::min() ||
            n > (double)std::numeric_limits<int64_t>::max())
            throw std::runtime_error(std::string("result: invalid ") + key);
        return (int64_t)n;
    };
    out.registered = result_count("registered");
    out.images = result_count("images");
    out.points = result_count("points");
    out.models = result_count("models");
    out.mean_reprojection = optional_number(root, "mean_reprojection", 0);
    out.median_reprojection = optional_number(root, "median_reprojection", 0);
    out.sparse_path = optional_string(root, "sparse_path");
    if (out.schema_version != 1 && out.schema_version != 2)
        throw std::runtime_error("result: unsupported schema_version");
    if (out.outcome != "success" && out.outcome != "partial" && out.outcome != "nonmetric" &&
        out.outcome != "failed" && out.outcome != "stopped" && out.outcome != "spawn_failed")
        throw std::runtime_error("result: invalid outcome");
    return out;
}

bool publish_result(const Request& r, const Result& res) {
    const fs::path target = fs::u8path(r.result_path);
    const fs::path tmp = target.parent_path() / (target.filename().u8string() + ".tmp-" + r.attempt_id);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << "{\n"
          << "  \"schema_version\": 2,\n"
          << "  \"job_id\": " << quote(r.job_id) << ",\n"
          << "  \"attempt_id\": " << quote(r.attempt_id) << ",\n"
          << "  \"phase\": " << quote(r.phase) << ",\n"
          << "  \"outcome\": " << quote(res.outcome) << ",\n"
          << "  \"exit_code\": " << res.exit_code << ",\n"
          << "  \"message\": " << quote(res.message) << ",\n"
          << "  \"partial\": " << (res.partial ? "true" : "false") << ",\n"
          << "  \"metric\": " << (res.metric ? "true" : "false") << ",\n"
          << "  \"registered\": " << res.registered << ",\n"
          << "  \"images\": " << res.images << ",\n"
          << "  \"points\": " << res.points << ",\n"
          << "  \"models\": " << res.models << ",\n"
          << "  \"mean_reprojection\": " << res.mean_reprojection << ",\n"
          << "  \"median_reprojection\": " << res.median_reprojection << ",\n"
          << "  \"sparse_path\": " << quote(res.sparse_path) << ",\n"
          << "  \"outputs\": [";
        for (size_t i = 0; i < res.outputs.size(); ++i) {
            if (i) f << ", ";
            f << quote(res.outputs[i]);
        }
        f << "]\n}\n";
        f.flush();
        if (!f) { std::error_code ec; fs::remove(tmp, ec); return false; }
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) { std::error_code ec2; fs::remove(tmp, ec2); return false; }
    return true;
}

std::string serialize_prep_job(const app::PrepJob& job) {
    reject_payload_nuls(job);
    if (job.inputs.empty()) throw std::runtime_error("prep: at least one input is required");
    return prep_json(job);
}

app::PrepJob deserialize_prep_job(const std::string& payload) {
    if (payload.size() > kMaxPayload || has_nul(payload)) throw std::runtime_error("prep: invalid payload");
    app::PrepJob job = decode_prep(json_parse(payload));
    reject_payload_nuls(job);
    return job;
}

bool freeze_prep_job(app::PrepJob& job, const std::string& base_dir, std::string& error) {
    try {
        if (has_nul(base_dir) || base_dir.empty()) throw std::runtime_error("base directory is empty or contains NUL");
        const fs::path base = fs::absolute(fs::u8path(base_dir)).lexically_normal();
        auto freeze = [&](std::string& value, const char* key, bool required_path) {
            if (has_nul(value)) throw std::runtime_error(std::string(key) + " contains an embedded NUL");
            if (value.empty()) {
                if (required_path) throw std::runtime_error(std::string(key) + " is empty");
                return;
            }
            value = fs::absolute(base / fs::u8path(value)).lexically_normal().u8string();
        };
        freeze(job.workspace, "workspace", true);
        if (job.workspace == base.u8string()) {
            // The workspace may be the submission directory; it is still frozen.
        }
        for (auto& in : job.inputs) {
            freeze(in.path, "input path", true);
            freeze(in.mask_dir, "mask directory", false);
            freeze(in.stencil.mask.image, "stencil image", false);
        }
        freeze(job.mask_model_path, "mask model", false);
        reject_payload_nuls(job);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool reject_external_masking(const app::PrepJob& job, std::string& error) {
    if (!job.force_external_masking) return false;
    error = "scheduled prep rejects external Python masking; use built-in native masking (ffmpeg CPU decoding remains allowed)";
    return true;
}

}  // namespace app::worker
