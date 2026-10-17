// Adapt.cpp -- see Adapt.h.

#include "checkpoint/Adapt.h"
#include "checkpoint/Resume.h"

#include "core/CheckpointIO.h"
#include "core/Tensor.h"
#include "data/JsonWrite.h"
#include "kernels/pixelwise/PixelWise.cuh"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace ckpt {
namespace {

constexpr int64_t kBlock = 256;

// Per-splat non-SH attributes and their channel counts.
const std::map<std::string, int> kAttrCh = {
    {"means", 3}, {"quats", 4}, {"scales", 3}, {"opacities", 1},
    {"features_dc", 3},
};


// ===========================================================================
// Host-side buffer I/O
// ===========================================================================

// One .npy member, kept as raw bytes plus its numpy descr.
struct Buffer {
    std::string descr;         // "<f4", "<i4", "|u1"
    std::vector<char> bytes;

    size_t elem_size() const {
        return descr.size() >= 3 ? (size_t)(descr[2] - '0') : 1;
    }
    size_t numel() const { return bytes.size() / elem_size(); }
    const float*   f32() const { return (const float*)bytes.data(); }
    const int32_t* i32() const { return (const int32_t*)bytes.data(); }
    const uint8_t* u8()  const { return (const uint8_t*)bytes.data(); }
};

Buffer make_buffer(const char* descr, const void* src, size_t nbytes) {
    Buffer b;
    b.descr = descr;
    b.bytes.resize(nbytes);
    if (nbytes) std::memcpy(b.bytes.data(), src, nbytes);
    return b;
}

Buffer from_floats(const std::vector<float>& v) {
    return make_buffer("<f4", v.data(), v.size() * sizeof(float));
}
Buffer from_bytes(const std::vector<uint8_t>& v) {
    return make_buffer("|u1", v.data(), v.size());
}

Buffer read_npy_buffer(std::istream& in, const NpyInfo& info) {
    if (info.data_bytes > std::numeric_limits<size_t>::max() ||
        info.data_bytes > (uint64_t)std::numeric_limits<std::streamsize>::max())
        throw std::runtime_error("checkpoint array is too large for host I/O");
    Buffer b;
    b.descr = info.descr;
    b.bytes.resize((size_t)info.data_bytes);
    in.clear();
    in.seekg((std::streamoff)info.data_offset, std::ios::beg);
    if (!b.bytes.empty())
        in.read(b.bytes.data(), (std::streamsize)b.bytes.size());
    if (!in) throw std::runtime_error("cannot read checkpoint array");
    return b;
}

std::map<std::string, Buffer> read_tar_npy(const fs::path& ckpt_dir) {
    std::ifstream in((ckpt_dir / "state.tar").string(), std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " +
                                      (ckpt_dir / "state.tar").string());
    std::map<std::string, Buffer> out;
    for (const auto& m : tar_index(in)) {
        if (m.name.size() < 4 ||
            m.name.compare(m.name.size() - 4, 4, ".npy") != 0) continue;
        NpyInfo info = npy_locate(in, m.data_offset, m.size);
        out.emplace(m.name.substr(0, m.name.size() - 4),
                    read_npy_buffer(in, info));
    }
    return out;
}

void write_npy_member(std::ostream& out, const std::string& name,
                      const Buffer& b) {
    std::string hdr = npy_header(b.descr.c_str(), b.numel());
    size_t member = hdr.size() + b.bytes.size();
    tar_header(out, name, member);
    out.write(hdr.data(), (std::streamsize)hdr.size());
    out.write(b.bytes.data(), (std::streamsize)b.bytes.size());
    tar_pad(out, member);
}


// ===========================================================================
// Block-quantized buffers, through the engine's own codecs (core/Tensor.h)
// ===========================================================================
//
// The kernels reduce each block's bounds in shared memory and then encode; on
// the host the reduction is an ordinary pass. A "block" is `bbc` CONTIGUOUS
// cells: the cell-block layout uses bbc = 256, the per-splat-block (FPBO)
// layout uses bbc = 256 * cells_per_splat.

int64_t num_blocks(int64_t n, int64_t bbc) { return (n + bbc - 1) / bbc; }

template <int BITS>
void decode_linear_t(const uint8_t* packed, const float* bounds, int64_t n,
                     int64_t bbc, std::vector<float>& out) {
    using Q = QuantizedTensor<BITS>;
    out.resize((size_t)n);
    for (int64_t i = 0; i < n; i++) {
        int64_t b = i / bbc;
        float2 mm{bounds[b * 2 + 0], bounds[b * 2 + 1]};
        out[(size_t)i] = Q::decode_v(packed, i, mm);
    }
}

template <int BITS>
void encode_linear_t(const std::vector<float>& v, int64_t bbc,
                     std::vector<uint8_t>& packed, std::vector<float>& bounds) {
    using Q = QuantizedTensor<BITS>;
    const int64_t n = (int64_t)v.size();
    const int64_t nb = num_blocks(n, bbc);
    bounds.assign((size_t)nb * 2, 0.0f);
    for (int64_t b = 0; b < nb; b++) {
        int64_t lo = b * bbc, hi = std::min(n, lo + bbc);
        float mn = v[(size_t)lo], mx = mn;
        for (int64_t i = lo + 1; i < hi; i++) {
            mn = std::min(mn, v[(size_t)i]);
            mx = std::max(mx, v[(size_t)i]);
        }
        bounds[(size_t)(b * 2 + 0)] = mn;
        bounds[(size_t)(b * 2 + 1)] = mx;
    }
    packed.assign((size_t)(n * Q::kBytesPerCell), 0);
    for (int64_t i = 0; i < n; i++) {
        int64_t b = i / bbc;
        float2 mm{bounds[(size_t)(b * 2)], bounds[(size_t)(b * 2 + 1)]};
        Q::encode_v(packed.data(), i, v[(size_t)i], mm);
    }
}

template <int BITS>
void decode_adam_t(const uint8_t* packed, const float* bounds, int64_t n,
                   int64_t bbc, std::vector<float>& g1, std::vector<float>& g2) {
    using A = QuantizedAdamState<BITS>;
    g1.resize((size_t)n);
    g2.resize((size_t)n);
    for (int64_t i = 0; i < n; i++) {
        int64_t b = i / bbc;
        float4 mm{bounds[b * 4 + 0], bounds[b * 4 + 1],
                  bounds[b * 4 + 2], bounds[b * 4 + 3]};
        float2 g = A::decode_g1g2(packed, i, mm);
        g1[(size_t)i] = g.x;
        g2[(size_t)i] = g.y;
    }
}

template <int BITS>
void encode_adam_t(const std::vector<float>& g1, const std::vector<float>& g2,
                   int64_t bbc, std::vector<uint8_t>& packed,
                   std::vector<float>& bounds) {
    using A = QuantizedAdamState<BITS>;
    const int64_t n = (int64_t)g1.size();
    // The primitives (u, log_s) are what the bounds live in, so reduce those.
    std::vector<float> u((size_t)n), s((size_t)n);
    for (int64_t i = 0; i < n; i++) {
        float2 us = A::g1g2_to_us(g1[(size_t)i], g2[(size_t)i]);
        u[(size_t)i] = us.x;
        s[(size_t)i] = us.y;
    }
    const int64_t nb = num_blocks(n, bbc);
    bounds.assign((size_t)nb * 4, 0.0f);
    for (int64_t b = 0; b < nb; b++) {
        int64_t lo = b * bbc, hi = std::min(n, lo + bbc);
        float umn = u[(size_t)lo], umx = umn, smn = s[(size_t)lo], smx = smn;
        for (int64_t i = lo + 1; i < hi; i++) {
            umn = std::min(umn, u[(size_t)i]); umx = std::max(umx, u[(size_t)i]);
            smn = std::min(smn, s[(size_t)i]); smx = std::max(smx, s[(size_t)i]);
        }
        bounds[(size_t)(b * 4 + 0)] = umn; bounds[(size_t)(b * 4 + 1)] = umx;
        bounds[(size_t)(b * 4 + 2)] = smn; bounds[(size_t)(b * 4 + 3)] = smx;
    }
    packed.assign((size_t)(n * A::kBytesPerCell), 0);
    for (int64_t i = 0; i < n; i++) {
        int64_t b = i / bbc;
        float4 mm{bounds[(size_t)(b * 4 + 0)], bounds[(size_t)(b * 4 + 1)],
                  bounds[(size_t)(b * 4 + 2)], bounds[(size_t)(b * 4 + 3)]};
        A::encode_us(packed.data(), i, u[(size_t)i], s[(size_t)i], mm);
    }
}

// Runtime bit-depth dispatch. QuantizedTensor supports 8/16, QuantizedAdamState
// 4/8/16; the checkpoint only ever writes the widths listed here.
[[noreturn]] void bad_bits(int bits) {
    throw std::runtime_error("checkpoint adapt: unsupported quantization width " +
                             std::to_string(bits));
}

void decode_linear(int bits, const uint8_t* p, const float* b, int64_t n,
                   int64_t bbc, std::vector<float>& out) {
    if (bits == 8)       decode_linear_t<8>(p, b, n, bbc, out);
    else if (bits == 16) decode_linear_t<16>(p, b, n, bbc, out);
    else                 bad_bits(bits);
}
void encode_linear(int bits, const std::vector<float>& v, int64_t bbc,
                   std::vector<uint8_t>& p, std::vector<float>& b) {
    if (bits == 8)       encode_linear_t<8>(v, bbc, p, b);
    else if (bits == 16) encode_linear_t<16>(v, bbc, p, b);
    else                 bad_bits(bits);
}
void decode_adam(int bits, const uint8_t* p, const float* b, int64_t n,
                 int64_t bbc, std::vector<float>& g1, std::vector<float>& g2) {
    if (bits == 8)       decode_adam_t<8>(p, b, n, bbc, g1, g2);
    else if (bits == 16) decode_adam_t<16>(p, b, n, bbc, g1, g2);
    else if (bits == 4)  decode_adam_t<4>(p, b, n, bbc, g1, g2);
    else                 bad_bits(bits);
}
void encode_adam(int bits, const std::vector<float>& g1,
                 const std::vector<float>& g2, int64_t bbc,
                 std::vector<uint8_t>& p, std::vector<float>& b) {
    if (bits == 8)       encode_adam_t<8>(g1, g2, bbc, p, b);
    else if (bits == 16) encode_adam_t<16>(g1, g2, bbc, p, b);
    else if (bits == 4)  encode_adam_t<4>(g1, g2, bbc, p, b);
    else                 bad_bits(bits);
}


// ===========================================================================
// Per-splat slot classification
// ===========================================================================

struct SlotClass {
    bool is_sh   = false;
    int  ch      = 0;      // channels per splat, for non-SH plain buffers
    int  cpp     = 0;      // cells per splat, for quantized buffers
    bool quant   = false;
    bool adam    = false;  // quant kind: adam vs linear
    int  bits    = 0;
    bool fpbo    = false;
    bool is_int  = false;  // plain buffer holds int32, not float
};

bool starts_with(const std::string& s, const char* p) {
    return s.rfind(p, 0) == 0;
}
bool ends_with(const std::string& s, const char* p) {
    size_t n = std::strlen(p);
    return s.size() >= n && s.compare(s.size() - n, n, p) == 0;
}

// Describe a per-splat slot's transform, or nothing if the slot is not
// per-splat (appearance tables, fixed-size buffers).
std::optional<SlotClass> classify(const std::string& name,
                                  int sh_optim_bits) {
    for (const char* pfx : {"world.", "eng.g1_", "eng.g2_"}) {
        if (!starts_with(name, pfx)) continue;
        std::string attr = name.substr(std::strlen(pfx));
        if (attr == "features_sh") { SlotClass c; c.is_sh = true; return c; }
        auto it = kAttrCh.find(attr);
        if (it != kAttrCh.end()) { SlotClass c; c.ch = it->second; return c; }
    }
    if (name == "eng.radii")        { SlotClass c; c.ch = 1; return c; }
    if (name == "eng.accum_buffer") { SlotClass c; c.ch = 2; return c; }
    if (name == "eng.bias_correction_steps") {
        SlotClass c; c.ch = 1; c.is_int = true; return c;
    }
    // Quantized Adam moments, FPBO (means/quats/scales/opacities/features_dc).
    if (ends_with(name, "_qfpbo") && starts_with(name, "eng.")) {
        std::string attr = name.substr(4, name.size() - 4 - 6);
        auto it = kAttrCh.find(attr);
        if (it != kAttrCh.end()) {
            SlotClass c;
            c.cpp = it->second; c.quant = true; c.adam = true;
            c.bits = 16; c.fpbo = true;
            return c;
        }
    }
    // Quantized SH Adam state (cell-block or FPBO).
    if (name == "eng.sh_quant" || name == "eng.sh_quant_fpbo") {
        SlotClass c;
        c.is_sh = true; c.quant = true; c.adam = true;
        c.bits = sh_optim_bits; c.fpbo = ends_with(name, "_fpbo");
        return c;
    }
    // Quantized SH VALUE store.
    if (starts_with(name, "eng.world.sh_vq")) {
        SlotClass c;
        c.is_sh = true; c.quant = true; c.adam = false;
        c.bits = name.find("16") != std::string::npos ? 16 : 8;
        c.fpbo = ends_with(name, "_fpbo");
        return c;
    }
    return std::nullopt;
}


// ===========================================================================
// Transforms
// ===========================================================================

// Indices (ascending) of the `target` splats to keep out of the first `cur`:
// tail-first up to the unsaturated slack, then lowest opacity.
std::vector<int64_t> keep_indices(const float* opac, int64_t cur,
                                  int64_t max_ckpt, int64_t target) {
    std::vector<int64_t> out;
    if (target >= cur) {
        out.resize((size_t)cur);
        std::iota(out.begin(), out.end(), (int64_t)0);
        return out;
    }
    std::vector<char> keep((size_t)cur, 1);
    int64_t d = cur - target;
    int64_t n_tail = std::min(d, std::max<int64_t>(0, max_ckpt - cur));
    for (int64_t i = cur - n_tail; i < cur; i++) keep[(size_t)i] = 0;
    d -= n_tail;
    if (d > 0) {
        std::vector<int64_t> idx;
        idx.reserve((size_t)cur);
        for (int64_t i = 0; i < cur; i++) if (keep[(size_t)i]) idx.push_back(i);
        // Ascending opacity, ties broken by index -- matches numpy's stable sort.
        std::stable_sort(idx.begin(), idx.end(), [&](int64_t a, int64_t b) {
            return opac[a] < opac[b];
        });
        for (int64_t k = 0; k < d; k++) keep[(size_t)idx[(size_t)k]] = 0;
    }
    for (int64_t i = 0; i < cur; i++) if (keep[(size_t)i]) out.push_back(i);
    if ((int64_t)out.size() != target)
        throw std::runtime_error("checkpoint adapt: keep-set size mismatch");
    return out;
}

// [max_ckpt, ch] -> [max_new, ch]; kept rows at the front, rest zero.
std::vector<float> gather_rows(const float* src, int ch,
                               const std::vector<int64_t>& keep,
                               int64_t max_new) {
    std::vector<float> out((size_t)(max_new * ch), 0.0f);
    for (size_t k = 0; k < keep.size(); k++)
        std::memcpy(&out[k * ch], &src[keep[k] * ch], (size_t)ch * sizeof(float));
    return out;
}

// [max_ckpt, K_ck, 3] -> [max_new, K_new, 3], band-clamped and zero-padded.
std::vector<float> resample_sh(const float* src, int K_ck,
                               const std::vector<int64_t>& keep,
                               int K_new, int64_t max_new) {
    const int Kc = std::min(K_ck, K_new);
    std::vector<float> out((size_t)(max_new * K_new * 3), 0.0f);
    for (size_t k = 0; k < keep.size(); k++)
        std::memcpy(&out[k * K_new * 3], &src[keep[k] * K_ck * 3],
                    (size_t)Kc * 3 * sizeof(float));
    return out;
}

// Trilinear (L,H,W) grid resample with align-corners sampling and edge clamp
// -- scipy.ndimage.zoom(order=1, mode="nearest"), which is what the Python
// implementation used. Layout is [n_grids][L][H][W][C], C interleaved.
std::vector<float> resample_grid(const float* src, int64_t n_grids,
                                 std::array<int, 3> from, int C,
                                 std::array<int, 3> to) {
    const int L = from[0], H = from[1], W = from[2];
    const int L2 = to[0], H2 = to[1], W2 = to[2];
    auto step = [](int in, int out) {
        return out > 1 ? (double)(in - 1) / (double)(out - 1) : 0.0;
    };
    const double sl = step(L, L2), sh = step(H, H2), sw = step(W, W2);
    std::vector<float> out((size_t)(n_grids * L2 * H2 * W2 * C), 0.0f);

    auto at = [&](int64_t g, int l, int h, int w, int c) -> double {
        l = std::min(std::max(l, 0), L - 1);
        h = std::min(std::max(h, 0), H - 1);
        w = std::min(std::max(w, 0), W - 1);
        return src[((((g * L + l) * H + h) * W + w) * C) + c];
    };

    for (int64_t g = 0; g < n_grids; g++)
    for (int l2 = 0; l2 < L2; l2++) {
        double fl = l2 * sl; int l0 = (int)std::floor(fl); double tl = fl - l0;
        for (int h2 = 0; h2 < H2; h2++) {
            double fh = h2 * sh; int h0 = (int)std::floor(fh); double th = fh - h0;
            for (int w2 = 0; w2 < W2; w2++) {
                double fw = w2 * sw; int w0 = (int)std::floor(fw); double tw = fw - w0;
                for (int c = 0; c < C; c++) {
                    double v =
                        at(g, l0,   h0,   w0,   c) * (1-tl)*(1-th)*(1-tw) +
                        at(g, l0,   h0,   w0+1, c) * (1-tl)*(1-th)*(  tw) +
                        at(g, l0,   h0+1, w0,   c) * (1-tl)*(  th)*(1-tw) +
                        at(g, l0,   h0+1, w0+1, c) * (1-tl)*(  th)*(  tw) +
                        at(g, l0+1, h0,   w0,   c) * (  tl)*(1-th)*(1-tw) +
                        at(g, l0+1, h0,   w0+1, c) * (  tl)*(1-th)*(  tw) +
                        at(g, l0+1, h0+1, w0,   c) * (  tl)*(  th)*(1-tw) +
                        at(g, l0+1, h0+1, w0+1, c) * (  tl)*(  th)*(  tw);
                    out[(size_t)((((g * L2 + l2) * H2 + h2) * W2 + w2) * C + c)] =
                        (float)v;
                }
            }
        }
    }
    return out;
}


// ===========================================================================
// state.json helpers
// ===========================================================================

struct BilagridState {
    bool enabled = false;
    std::array<int, 3> lhw{0, 0, 0};
    int C = 0;
    int value_bits = 16;
    int optim_bits = 32;
    bool use_adagrad = false;
    std::string type;
};

int checked_state_int(const JsonValue& object, const char* key,
                      int minimum, int maximum) {
    const double raw = object.get_double(key, minimum - 1.0);
    if (!std::isfinite(raw) || std::trunc(raw) != raw ||
        raw < minimum || raw > maximum)
        throw std::runtime_error(
            "checkpoint adapt: invalid checkpoint integer " + std::string(key));
    return (int)raw;
}

int64_t checked_grid_cells(const std::array<int, 3>& lhw, int channels) {
    if (channels <= 0)
        throw std::runtime_error("checkpoint adapt: invalid bilagrid channels");
    int64_t cells = channels;
    for (int dimension : lhw) {
        if (dimension <= 0 ||
            cells > std::numeric_limits<int64_t>::max() / dimension)
            throw std::runtime_error(
                "checkpoint adapt: invalid bilagrid dimensions");
        cells *= dimension;
    }
    return cells;
}

BilagridState bilagrid_state(const JsonValue& state, const char* which) {
    BilagridState b;
    const JsonValue* g = state.find(std::string("bilagrid_") + which);
    if (!g) return b;
    const double enabled = g->get_double("enabled", 0);
    if (enabled != 0 && enabled != 1)
        throw std::runtime_error(
            "checkpoint adapt: invalid bilagrid enabled flag");
    b.enabled = enabled == 1;
    if (!b.enabled) return b;
    b.lhw = {
        checked_state_int(*g, "L", 1, std::numeric_limits<int>::max()),
        checked_state_int(*g, "H", 1, std::numeric_limits<int>::max()),
        checked_state_int(*g, "W", 1, std::numeric_limits<int>::max()),
    };
    const double value_bits = g->get_double("value_bits", 0);
    const double optim_bits = g->get_double("optim_bits", 0);
    const double use_adagrad = g->get_double("use_adagrad", -1);
    if ((value_bits != 16 && value_bits != 32) ||
        (optim_bits != 4 && optim_bits != 8 && optim_bits != 32) ||
        (use_adagrad != 0 && use_adagrad != 1))
        throw std::runtime_error(
            "checkpoint adapt: invalid bilagrid storage metadata");
    b.value_bits = (int)value_bits;
    b.optim_bits = (int)optim_bits;
    b.use_adagrad = use_adagrad == 1;
    if (std::strcmp(which, "rgb") == 0) {
        const JsonValue* type = g->find("type");
        if (!type || type->type != JsonValue::Type::String)
            throw std::runtime_error(
                "checkpoint adapt: invalid bilagrid RGB type");
        b.type = type->as_string();
    }
    if (std::strcmp(which, "rgb") == 0)
        b.C = checked_state_int(*g, "C", 1,
                                std::numeric_limits<int>::max());
    else if (std::strcmp(which, "depth") == 0) b.C = 2;
    else b.C = 3;
    checked_grid_cells(b.lhw, b.C);
    return b;
}

const std::optional<std::array<int, 3>>& target_grid(const TargetLayout& t,
                                                     const std::string& which) {
    if (which == "rgb")   return t.bilagrid_rgb;
    if (which == "depth") return t.bilagrid_depth;
    return t.bilagrid_normal;
}

[[noreturn]] void image_count_error(const char* what, int64_t ck, int64_t tgt) {
    throw std::runtime_error(
        std::string("resume: ") + what + " image count changed (" +
        std::to_string(ck) + " -> " + std::to_string(tgt) + "). The dataset, "
        "train/eval split or warp_to_pinhole changed in a way that would need "
        "the per-image " + what + " tables rewritten, which is not supported. "
        "Resume on the original dataset/split, or turn " + what + " off for "
        "this run.");
}

void update_require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error("native checkpoint update comparison: " + message);
}

void validate_update_json(const JsonValue& value) {
    if (value.type == JsonValue::Type::Number)
        update_require(std::isfinite(value.num), "nonfinite metadata number");
    if (value.is_object()) {
        std::set<std::string> keys;
        for (const auto& entry : value.obj) {
            update_require(keys.insert(entry.first).second,
                           "duplicate metadata key " + entry.first);
            validate_update_json(entry.second);
        }
    }
    for (const auto& item : value.arr) validate_update_json(item);
}

bool same_update_json(const JsonValue& a, const JsonValue& b) {
    if (a.type != b.type) return false;
    if (a.type == JsonValue::Type::Number) return a.num == b.num;
    if (a.type == JsonValue::Type::String) return a.str == b.str;
    if (a.type == JsonValue::Type::Bool) return a.b == b.b;
    if (a.arr.size() != b.arr.size() || a.obj.size() != b.obj.size()) return false;
    for (size_t i = 0; i < a.arr.size(); ++i)
        if (!same_update_json(a.arr[i], b.arr[i])) return false;
    for (const auto& entry : a.obj) {
        const JsonValue* other = b.find(entry.first);
        if (!other || !same_update_json(entry.second, *other)) return false;
    }
    return true;
}

const JsonValue& update_object(const JsonValue& state, const char* key) {
    const JsonValue* object = state.find(key);
    update_require(object && object->is_object(),
                   "missing/invalid metadata object " + std::string(key));
    return *object;
}

struct NativeUpdateArchive {
    fs::path directory;
    JsonValue state;
    std::ifstream in;
    std::map<std::string, NpyInfo> arrays;
    int step = 0, horizon = 0, capacity = 0, current = 0, num_sh = 0;
    int sh_optim_bits = 0, sh_value_bits = 0, non_sh_optim_bits = 0;
    bool row_clocks = false;

    explicit NativeUpdateArchive(const fs::path& path) {
        const auto resolved = resolve_checkpoint(path);
        directory = resolved.ckpt_dir;
        check_resumable(directory);
        state = read_state_json(directory);
        validate_update_json(state);
        const auto config = json_parse_file((resolved.run_dir / "config.json").string());
        update_require(config.is_object(), "config.json must be an object");
        const JsonValue* iterations = config.find("num_iterations");
        update_require(iterations && iterations->type == JsonValue::Type::Number,
                       "config.json must explicitly record num_iterations");
        update_require(std::count_if(config.obj.begin(), config.obj.end(),
                       [](const auto& entry) { return entry.first == "num_iterations"; }) == 1,
                       "duplicate num_iterations in config.json");
        horizon = checked_state_int(config, "num_iterations", 1, INT32_MAX);
        update_require(checked_state_int(state, "format_version", 2, 2) == 2 &&
                       checked_state_int(state, "full_resume", 1, 1) == 1 &&
                       state.find("primitive") &&
                       state.find("primitive")->as_string() == "3dgs" &&
                       checked_state_int(state, "packed", 1, 1) == 1 &&
                       checked_state_int(state, "use_fused_proj_bwd_optim", 1, 1) == 1,
                       "requires full native format-2 packed 3dgs FPBO state");
        update_require(!state.find("b1"), "B1-owned checkpoints are not native state");
        update_require(state.find("quant_codec") &&
                       state.find("quant_codec")->as_string() == "joint_u_log_sqrt_g2_v1" &&
                       checked_state_int(state, "quant_block_size", 256, 256) == 256 &&
                       state.get_double("quant_eps", 0) == 1e-15,
                       "unsupported native quantization codec metadata");
        step = checked_state_int(state, "step", 0, horizon);
        capacity = checked_state_int(state, "max_num_splats", 1, INT32_MAX);
        current = checked_state_int(state, "cur_num_splats", 0, capacity);
        num_sh = checked_state_int(state, "num_sh", 0, 15);
        update_require(num_sh == 0 || num_sh == 3 || num_sh == 8 || num_sh == 15,
                       "unsupported 3dgs SH allocation");
        checked_state_int(state, "sh_degree", 0, INT32_MAX);
        sh_optim_bits = checked_state_int(state, "sh_optim_bits", 4, 32);
        sh_value_bits = checked_state_int(state, "sh_value_bits", 8, 32);
        non_sh_optim_bits = checked_state_int(state, "non_sh_optim_bits", 16, 32);
        update_require((sh_optim_bits == 4 || sh_optim_bits == 8 || sh_optim_bits == 32) &&
                       (sh_value_bits == 8 || sh_value_bits == 16 || sh_value_bits == 32) &&
                       (non_sh_optim_bits == 16 || non_sh_optim_bits == 32),
                       "unsupported FPBO storage widths");
        row_clocks = checked_state_int(state, "use_per_splat_bias_correction", 0, 1) == 1;
        const auto& camera = update_object(state, "camera");
        checked_state_int(camera, "num", 1, INT32_MAX);
        checked_state_int(camera, "width", 1, INT32_MAX);
        checked_state_int(camera, "height", 1, INT32_MAX);
        update_require(camera.find("model") &&
                       camera.find("model")->type == JsonValue::Type::String,
                       "invalid camera model metadata");
        for (const char* which : {"rgb", "depth", "normal"}) {
            const std::string key = std::string("bilagrid_") + which;
            checked_state_int(update_object(state, key.c_str()), "enabled", 0, 1);
            bilagrid_state(state, which);
        }
        const auto& ppisp = update_object(state, "ppisp");
        if (checked_state_int(ppisp, "enabled", 0, 1)) {
            const auto* type = ppisp.find("param_type");
            update_require(type && type->type == JsonValue::Type::String,
                           "invalid PPISP param_type");
            update_require(checked_state_int(ppisp, "num_params", 1, INT32_MAX) ==
                           ppisp_param_spec(type->str).num_params,
                           "PPISP parameter width disagrees with native layout");
            checked_state_int(ppisp, "use_adagrad", 0, 1);
        }
        const auto& background = update_object(state, "background_sh");
        if (checked_state_int(background, "enabled", 0, 1))
            checked_state_int(background, "degree", 0, 4);

        in.open(directory / "state.tar", std::ios::binary);
        update_require(bool(in), "cannot open state.tar");
        for (const auto& member : tar_index(in)) {
            if (member.name == "state.json") continue;
            update_require(ends_with(member.name, ".npy"),
                           "unhandled archive member " + member.name);
            const std::string name = member.name.substr(0, member.name.size() - 4);
            update_require(arrays.emplace(name, npy_locate(in, member.data_offset,
                                                           member.size)).second,
                           "duplicate array " + name);
        }
    }
};

template<class T> T update_load(const Buffer& buffer, int64_t index) {
    T value;
    std::memcpy(&value, buffer.bytes.data() + (size_t)index * sizeof(T), sizeof(T));
    return value;
}

// 0 finite, 1 NaN, 2 +Inf, 3 -Inf; metadata and codec bounds never use this exemption.
int update_value_class(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    if ((bits & 0x7f800000u) != 0x7f800000u) return 0;
    if (bits & 0x007fffffu) return 1;
    return bits >> 31 ? 3 : 2;
}

enum class UpdateCodec { Float, Linear, Adam, Log };

struct NativeUpdateField {
    std::string name, slot, role;
    int64_t rows = 0, channels = 0, storage_cells = 0, bounds_stride = 0;
    UpdateCodec codec = UpdateCodec::Float;
    int bits = 32, allocated_bytes_per_cell = 4;
    bool gaussian = false, sh_fpbo = false;

    int64_t cells() const { return rows * channels; }
    bool adam() const { return codec == UpdateCodec::Adam; }
};

struct NativeUpdateRows {
    std::array<std::vector<uint8_t>, 4> status;
    std::vector<uint8_t> cohort;
    uint64_t cohort_rows = 0;
};

const char* const kUpdateStates[] = {"source", "nativeA", "strip", "nativeB"};

struct UpdateSampleMetric {
    uint64_t finite = 0, nan = 0, positive_inf = 0, negative_inf = 0;
    uint64_t original_inactive_world_nan = 0, outside_prefix_nonfinite = 0;
    uint64_t endpoint_inactive_mean_nan = 0;
    uint64_t unexpected_nonfinite = 0;

    void add(float value, bool original_nan, bool endpoint_inactive_mean, bool outside_prefix) {
        const int kind = update_value_class(value);
        if (!kind) { ++finite; return; }
        nan += kind == 1;
        positive_inf += kind == 2;
        negative_inf += kind == 3;
        if (kind == 1 && original_nan) ++original_inactive_world_nan;
        else if (outside_prefix) ++outside_prefix_nonfinite;
        else if (kind == 1 && endpoint_inactive_mean) ++endpoint_inactive_mean_nan;
        else ++unexpected_nonfinite;
    }

    void emit(std::ostream& out) const {
        out << "{\"finite\":" << finite << ",\"nan\":" << nan
            << ",\"positive_inf\":" << positive_inf << ",\"negative_inf\":" << negative_inf
            << ",\"original_inactive_world_nan\":" << original_inactive_world_nan
            << ",\"endpoint_inactive_mean_nan\":" << endpoint_inactive_mean_nan
            << ",\"outside_prefix_nonfinite\":" << outside_prefix_nonfinite
            << ",\"unexpected_nonfinite\":" << unexpected_nonfinite << '}';
    }
};

const char* update_norm_class(uint64_t count, double reference_sq, double error_sq) {
    if (!count) return "no_finite_cells";
    if (reference_sq != 0) return "nonzero_reference";
    return error_sq == 0 ? "zero_reference_zero_error" : "zero_reference_nonzero_error";
}

void emit_update_ratio(std::ostream& out, uint64_t count,
                       double numerator, double denominator) {
    if (!count || denominator == 0) out << "null";
    else out << std::sqrt(numerator / denominator);
}

struct UpdateDeltaMetric {
    uint64_t finite = 0, nonfinite = 0, zero = 0;
    uint64_t nan_pattern_changes = 0, nonfinite_class_changes = 0;
    double sumsq = 0, maxabs = 0;

    void add(float source, float endpoint) {
        const int a = update_value_class(source), b = update_value_class(endpoint);
        nan_pattern_changes += (a == 1) != (b == 1);
        nonfinite_class_changes += a != b;
        if (a || b) {
            ++nonfinite;
            return;
        }
        const double delta = double(endpoint) - double(source);
        ++finite;
        zero += delta == 0;
        sumsq += delta * delta;
        maxabs = std::max(maxabs, std::abs(delta));
    }

    void emit(std::ostream& out) const {
        out << "{\"finite_cells\":" << finite << ",\"nonfinite_cells\":" << nonfinite
            << ",\"zero_update_cells\":" << zero << ",\"changed_cells\":" << finite - zero
            << ",\"nan_pattern_changed_cells\":" << nan_pattern_changes
            << ",\"nonfinite_class_changed_cells\":" << nonfinite_class_changes
            << ",\"l2\":" << std::sqrt(sumsq) << ",\"maxabs\":" << maxabs
            << ",\"class\":" << json_quote(!finite ? "no_finite_cells" :
                                         sumsq == 0 ? "zero_update" : "nonzero_update") << '}';
    }
};

struct UpdatePairMetric {
    uint64_t finite = 0, nonfinite = 0, matching_nonfinite_class = 0;
    uint64_t update_finite = 0, update_nonfinite = 0;
    double maxabs = 0, error_sq = 0, reference_sq = 0;
    double update_error_sq = 0, update_reference_sq = 0;
    int64_t worst_index = -1, first_nonfinite_index = -1;

    void add(float source, float reference, float candidate, int64_t index) {
        const int a = update_value_class(reference), b = update_value_class(candidate);
        if (a || b) {
            ++nonfinite;
            matching_nonfinite_class += a != 0 && a == b;
            if (first_nonfinite_index < 0) first_nonfinite_index = index;
        } else {
            ++finite;
            const double error = double(candidate) - double(reference);
            if (worst_index < 0 || std::abs(error) > maxabs) {
                maxabs = std::abs(error);
                worst_index = index;
            }
            error_sq += error * error;
            reference_sq += double(reference) * double(reference);
        }
        if (a || b || update_value_class(source)) {
            ++update_nonfinite;
        } else {
            ++update_finite;
            const double update = double(reference) - double(source);
            const double error = double(candidate) - double(reference);
            update_error_sq += error * error;
            update_reference_sq += update * update;
        }
    }

    void emit(std::ostream& out) const {
        out << "{\"finite_endpoint_pairs\":" << finite << ",\"nonfinite_endpoint_pairs\":" << nonfinite
            << ",\"matching_nonfinite_class_pairs\":" << matching_nonfinite_class
            << ",\"endpoint_value_class\":"
            << json_quote(nonfinite ? "nonfinite_present" : finite ? "finite" : "empty")
            << ",\"maxabs\":" << maxabs << ",\"endpoint_rel_rms\":";
        emit_update_ratio(out, finite, error_sq, reference_sq);
        out << ",\"endpoint_normalization_class\":"
            << json_quote(update_norm_class(finite, reference_sq, error_sq))
            << ",\"endpoint_error_l2\":" << std::sqrt(error_sq)
            << ",\"finite_update_triples\":" << update_finite
            << ",\"nonfinite_update_triples\":" << update_nonfinite
            << ",\"update_value_class\":"
            << json_quote(update_nonfinite ? "nonfinite_present" : update_finite ? "finite" : "empty")
            << ",\"native_update_l2\":" << std::sqrt(update_reference_sq)
            << ",\"update_error_l2\":" << std::sqrt(update_error_sq)
            << ",\"update_normalized_error\":";
        emit_update_ratio(out, update_finite, update_error_sq, update_reference_sq);
        out << ",\"update_normalization_class\":"
            << json_quote(update_norm_class(update_finite, update_reference_sq, update_error_sq))
            << ",\"worst_flat_index\":" << worst_index
            << ",\"first_nonfinite_flat_index\":" << first_nonfinite_index << '}';
    }
};

struct NativeUpdateMetric {
    uint64_t cells = 0;
    std::array<UpdateSampleMetric, 4> samples;
    std::array<UpdateDeltaMetric, 3> updates;
    std::array<UpdatePairMetric, 2> pairs;

    void add(const std::array<float, 4>& values, int64_t index,
             const std::array<uint8_t, 4>& status, bool world, bool means) {
        ++cells;
        const bool original_world_nan = world && (status[0] & 7) == 7 &&
                                        update_value_class(values[0]) == 1;
        for (size_t i = 0; i < 4; ++i) {
            const bool original_nan = original_world_nan &&
                (status[i] & 7) == (status[0] & 7);
            const bool endpoint_inactive_mean = i != 0 && means && (status[i] & 7) == 7;
            samples[i].add(values[i], original_nan, endpoint_inactive_mean, (status[i] & 16) != 0);
            if (i) updates[i - 1].add(values[0], values[i]);
        }
        pairs[0].add(values[0], values[1], values[2], index);
        pairs[1].add(values[0], values[1], values[3], index);
    }

    void emit(std::ostream& out, const NativeUpdateField& field,
              const std::string& name, const char* scope) const {
        out << "{\"type\":\"field\",\"field\":" << json_quote(name)
            << ",\"role\":" << json_quote(field.role) << ",\"scope\":" << json_quote(scope)
            << ",\"logical_rows\":" << field.rows << ",\"components_per_row\":" << field.channels
            << ",\"cells\":" << cells << ",\"samples\":{";
        for (size_t i = 0; i < 4; ++i) {
            out << (i ? "," : "") << json_quote(kUpdateStates[i]) << ':';
            samples[i].emit(out);
        }
        out << "},\"source_updates\":{";
        for (size_t i = 0; i < 3; ++i) {
            out << (i ? "," : "") << json_quote(kUpdateStates[i + 1]) << ':';
            updates[i].emit(out);
        }
        out << "},\"nativeA_vs_strip\":";
        pairs[0].emit(out);
        out << ",\"nativeA_vs_nativeB\":";
        pairs[1].emit(out);
        out << "}\n";
    }
};

std::vector<NativeUpdateField> native_update_fields(const NativeUpdateArchive& archive) {
    std::vector<NativeUpdateField> fields;
    auto plain = [&](const std::string& name, const std::string& slot,
                     const char* role, int64_t rows, int64_t channels, bool gaussian = false) {
        NativeUpdateField field;
        field.name = name; field.slot = slot; field.role = role;
        field.rows = rows; field.channels = channels;
        field.storage_cells = field.cells(); field.gaussian = gaussian;
        fields.push_back(std::move(field));
    };
    auto quantized = [&](const std::string& name, const std::string& slot,
                         const char* role, int64_t rows, int64_t channels,
                         UpdateCodec codec, int bits, int allocated_bytes,
                         bool gaussian = false, bool sh = false) {
        NativeUpdateField field;
        field.name = name; field.slot = slot; field.role = role;
        field.rows = rows; field.channels = channels;
        field.codec = codec; field.bits = bits;
        field.allocated_bytes_per_cell = allocated_bytes;
        field.gaussian = gaussian; field.sh_fpbo = sh;
        field.storage_cells = sh ? sh_fpbo_cells(rows, (uint32_t)archive.num_sh) : field.cells();
        field.bounds_stride = sh ? sh_quant_addr((uint32_t)archive.num_sh, 0).bounds_stride
            : gaussian ? kBlock * channels : kBlock;
        fields.push_back(std::move(field));
    };
    const int64_t n = archive.capacity;
    for (const char* attribute : {"means", "quats", "scales", "opacities", "features_dc"}) {
        const int channels = kAttrCh.at(attribute);
        plain(std::string("world.") + attribute, std::string("world.") + attribute,
              "gaussian_parameter", n, channels, true);
        const std::string name = std::string("optim.") + attribute;
        if (archive.non_sh_optim_bits == 32) {
            for (const char* moment : {"g1", "g2"})
                plain(name + "." + moment, std::string("eng.") + moment + "_" + attribute,
                      "gaussian_adam", n, channels, true);
        } else {
            const std::string slot = std::string("eng.") + attribute + "_qfpbo";
            const auto classification = classify(slot, archive.sh_optim_bits);
            update_require(classification && classification->adam && classification->fpbo,
                           "unsupported Gaussian optimizer slot " + slot);
            quantized(name, slot, "gaussian_adam", n, classification->cpp,
                      UpdateCodec::Adam, classification->bits,
                      QuantizedAdamState<16>::kBytesPerCell, true);
        }
    }
    if (archive.num_sh) {
        const int64_t channels = 3 * archive.num_sh;
        if (archive.sh_value_bits == 32) {
            plain("world.features_sh", "world.features_sh", "gaussian_parameter", n, channels, true);
        } else {
            const std::string slot = "eng.world.sh_vq" + std::to_string(archive.sh_value_bits) + "_fpbo";
            const auto classification = classify(slot, archive.sh_optim_bits);
            update_require(classification && classification->is_sh && classification->fpbo,
                           "unsupported SH parameter slot " + slot);
            quantized("world.features_sh", slot, "gaussian_parameter", n, channels,
                      UpdateCodec::Linear, classification->bits, classification->bits / 8, true, true);
        }
        if (archive.sh_optim_bits == 32) {
            for (const char* moment : {"g1", "g2"})
                plain(std::string("optim.features_sh.") + moment,
                      std::string("eng.") + moment + "_features_sh",
                      "gaussian_adam", n, channels, true);
        } else {
            const auto classification = classify("eng.sh_quant_fpbo", archive.sh_optim_bits);
            quantized("optim.features_sh", "eng.sh_quant_fpbo", "gaussian_adam", n, channels,
                      UpdateCodec::Adam, classification->bits,
                      QuantizedAdamState<8>::kBytesPerCell, true, true);
        }
    }
    plain("eng.radii", "eng.radii", "resumed_densification_statistic", n, 1, true);
    plain("eng.accum_buffer", "eng.accum_buffer", "resumed_densification_statistic", n, 2, true);
    const auto& ppisp = update_object(archive.state, "ppisp");
    if (ppisp.get_double("enabled", 0) == 1) {
        const int channels = checked_state_int(ppisp, "num_params", 1, INT32_MAX);
        const auto& info = archive.arrays.at("eng.ppisp.params");
        const int64_t rows = (int64_t)(info.data_bytes / sizeof(float) / channels);
        update_require(rows > 0, "empty PPISP table");
        plain("eng.ppisp.params", "eng.ppisp.params", "appearance_parameter", rows, channels);
        if (ppisp.get_double("use_adagrad", 0) == 1) {
            plain("eng.ppisp.accum", "eng.ppisp.accum", "appearance_adagrad", rows, channels);
        } else {
            for (const char* moment : {"g1", "g2"}) {
                const std::string slot = std::string("eng.ppisp.") + moment;
                plain(slot, slot, "appearance_adam", rows, channels);
            }
        }
    }
    for (const char* which : {"rgb", "depth", "normal"}) {
        const BilagridState grid = bilagrid_state(archive.state, which);
        if (!grid.enabled) continue;
        const std::string prefix = std::string("eng.bg.") + which;
        const std::string value_slot = prefix + (grid.value_bits == 32 ? ".grids" : ".grids_q.q");
        const auto& info = archive.arrays.at(value_slot);
        const int64_t cells = (int64_t)(info.data_bytes / (grid.value_bits / 8));
        const int64_t channels = checked_grid_cells(grid.lhw, grid.C);
        update_require(cells > 0 && cells % channels == 0, "invalid grid table count " + prefix);
        const int64_t rows = cells / channels;
        if (grid.value_bits == 32)
            plain(prefix + ".grids", value_slot, "appearance_parameter", rows, channels);
        else
            quantized(prefix + ".grids", prefix + ".grids_q", "appearance_parameter",
                      rows, channels, UpdateCodec::Linear, 16, QuantizedTensor<16>::kBytesPerCell);
        if (grid.use_adagrad) {
            if (grid.optim_bits == 32)
                plain(prefix + ".accum", prefix + ".accum", "appearance_adagrad", rows, channels);
            else
                quantized(prefix + ".accum", prefix + ".bg_ag", "appearance_adagrad",
                          rows, channels, UpdateCodec::Log, grid.optim_bits, 1);
        } else if (grid.optim_bits == 32) {
            for (const char* moment : {"g1", "g2"}) {
                const std::string slot = prefix + "." + moment;
                plain(slot, slot, "appearance_adam", rows, channels);
            }
        } else {
            quantized(prefix + ".adam", prefix + ".bg_quant", "appearance_adam",
                      rows, channels, UpdateCodec::Adam, grid.optim_bits,
                      QuantizedAdamState<8>::kBytesPerCell);
        }
        if (std::strcmp(which, "depth") == 0)
            plain(prefix + ".scalars", prefix + ".scalars", "resumed_depth_quantile", rows, 1);
    }
    const auto& background = update_object(archive.state, "background_sh");
    if (background.get_double("enabled", 0) == 1) {
        const int side = checked_state_int(background, "degree", 0, 4) + 1;
        plain("eng.bg_sky.sh_coeffs", "eng.bg_sky.sh_coeffs", "background_parameter", side * side, 3);
        for (const char* moment : {"g1", "g2"}) {
            const std::string slot = std::string("eng.bg_sky.") + moment;
            plain(slot, slot, "background_adam", side * side, 3);
        }
    }
    if (archive.arrays.count("eng.color_shift_reg.ema"))
        plain("eng.color_shift_reg.ema", "eng.color_shift_reg.ema", "resumed_color_shift_ema", 1, 3);
    for (const char* slot : {"color_space.splat_matrix", "color_space.image_matrix"}) {
        const auto found = archive.arrays.find(slot);
        if (found == archive.arrays.end()) continue;
        const uint64_t bytes = found->second.data_bytes;
        update_require(bytes == 9 * sizeof(float) || bytes == 27 * sizeof(float),
                       "unsupported color matrix allocation " + std::string(slot));
        plain(slot, slot, "fixed_color_transform", 3, 3);
        // EngineColorSpace allocates float3[3,3]; kernels consume only the first nine floats.
        fields.back().storage_cells = (int64_t)(bytes / sizeof(float));
    }
    return fields;
}

const NpyInfo& update_array(const NativeUpdateArchive& archive, const std::string& name,
                           const char* descr, int64_t cells, int bytes_per_cell) {
    update_require(cells >= 0 && bytes_per_cell > 0 &&
                   (uint64_t)cells <= std::numeric_limits<size_t>::max() / bytes_per_cell &&
                   (uint64_t)cells <= (uint64_t)std::numeric_limits<std::streamsize>::max() / bytes_per_cell,
                   "array size exceeds host I/O " + name);
    const auto found = archive.arrays.find(name);
    update_require(found != archive.arrays.end() && found->second.descr == descr &&
                   found->second.data_bytes == (uint64_t)cells * bytes_per_cell,
                   "missing/incompatible array " + name + " in " + archive.directory.string());
    return found->second;
}

template<UpdateCodec CODEC, int BITS>
float2 decode_update_cell(const Buffer& values, const Buffer& bounds, int64_t cell, int64_t block) {
    if constexpr (CODEC == UpdateCodec::Float) {
        return {update_load<float>(values, cell), 0};
    } else if constexpr (CODEC == UpdateCodec::Adam) {
        return QuantizedAdamState<BITS>::decode_g1g2(values.u8(), cell, update_load<float4>(bounds, block));
    } else if constexpr (CODEC == UpdateCodec::Log) {
        return {QuantizedTensorLog<BITS>::decode_v(values.u8(), cell, update_load<float2>(bounds, block)), 0};
    } else {
        return {QuantizedTensor<BITS>::decode_v(values.u8(), cell, update_load<float2>(bounds, block)), 0};
    }
}

template<UpdateCodec CODEC, int BITS>
void measure_native_update_field(const NativeUpdateField& field,
                                 const std::array<Buffer, 4>& values,
                                 const std::array<Buffer, 4>& bounds,
                                 const NativeUpdateRows& rows,
                                 std::ostream& out, std::array<uint64_t, 4>& unexpected) {
    std::array<NativeUpdateMetric, 2> all, cohort;
    const bool world = field.role == "gaussian_parameter";
    const bool means = field.name == "world.means";
    const auto address = field.sh_fpbo ? sh_quant_addr((uint32_t)(field.channels / 3), 0) : ShQuantAddr{};
    constexpr int moments = CODEC == UpdateCodec::Adam ? 2 : 1;
    for (int64_t row = 0; row < field.rows; ++row) {
        std::array<uint8_t, 4> status{};
        if (field.gaussian)
            for (size_t i = 0; i < 4; ++i) status[i] = rows.status[i][(size_t)row];
        const bool unchanged = field.gaussian && rows.cohort[(size_t)row];
        const int64_t base = field.sh_fpbo ? address.base(row) : row * field.channels;
        const int64_t row_block = field.gaussian ? row / kBlock : 0;
        for (int64_t component = 0; component < field.channels; ++component) {
            const int64_t cell = field.sh_fpbo ? address.cell(base, (int)component) : base + component;
            const int64_t block = CODEC == UpdateCodec::Float ? 0 :
                field.gaussian ? row_block : cell / kBlock;
            std::array<float, 4> first, second;
            for (size_t i = 0; i < 4; ++i) {
                const float2 value = decode_update_cell<CODEC, BITS>(values[i], bounds[i], cell, block);
                first[i] = value.x;
                second[i] = value.y;
            }
            const int64_t index = row * field.channels + component;
            all[0].add(first, index, status, world, means);
            if (unchanged) cohort[0].add(first, index, status, world, means);
            if constexpr (CODEC == UpdateCodec::Adam) {
                all[1].add(second, index, status, false, false);
                if (unchanged) cohort[1].add(second, index, status, false, false);
            }
        }
    }
    for (int moment = 0; moment < moments; ++moment) {
        const std::string name = field.name + (field.adam() ? (moment ? ".g2" : ".g1") : "");
        all[moment].emit(out, field, name,
                        field.gaussian ? "all_capacity_slots_descriptive" : "all_enabled_table_cells");
        if (field.gaussian)
            cohort[moment].emit(out, field, name, "no_observed_clock_reset_or_mean_nan_change");
        for (size_t i = 0; i < 4; ++i)
            unexpected[i] += all[moment].samples[i].unexpected_nonfinite;
    }
}

void compare_native_update_field(const NativeUpdateField& field,
                                 const std::array<Buffer, 4>& values,
                                 const std::array<Buffer, 4>& bounds,
                                 const NativeUpdateRows& rows, std::ostream& out,
                                 std::array<uint64_t, 4>& unexpected) {
    if (field.codec == UpdateCodec::Float)
        measure_native_update_field<UpdateCodec::Float, 32>(field, values, bounds, rows, out, unexpected);
    else if (field.codec == UpdateCodec::Linear && field.bits == 8)
        measure_native_update_field<UpdateCodec::Linear, 8>(field, values, bounds, rows, out, unexpected);
    else if (field.codec == UpdateCodec::Linear && field.bits == 16)
        measure_native_update_field<UpdateCodec::Linear, 16>(field, values, bounds, rows, out, unexpected);
    else if (field.codec == UpdateCodec::Adam && field.bits == 4)
        measure_native_update_field<UpdateCodec::Adam, 4>(field, values, bounds, rows, out, unexpected);
    else if (field.codec == UpdateCodec::Adam && field.bits == 8)
        measure_native_update_field<UpdateCodec::Adam, 8>(field, values, bounds, rows, out, unexpected);
    else if (field.codec == UpdateCodec::Adam && field.bits == 16)
        measure_native_update_field<UpdateCodec::Adam, 16>(field, values, bounds, rows, out, unexpected);
    else if (field.codec == UpdateCodec::Log && field.bits == 4)
        measure_native_update_field<UpdateCodec::Log, 4>(field, values, bounds, rows, out, unexpected);
    else if (field.codec == UpdateCodec::Log && field.bits == 8)
        measure_native_update_field<UpdateCodec::Log, 8>(field, values, bounds, rows, out, unexpected);
    else update_require(false, "unhandled native scalar codec " + field.name);
}

void validate_native_update_inputs(const std::array<NativeUpdateArchive, 4>& archives) {
    const auto& source = archives[0];
    update_require(source.step < archives[1].step, "source must precede the completed endpoint");
    for (size_t i = 1; i < 4; ++i) {
        const auto& endpoint = archives[i];
        update_require(endpoint.step == archives[1].step && endpoint.horizon == source.horizon,
                       "endpoints must share a completed step and the unchanged source horizon");
        for (const char* key : {
                 "format_version", "full_resume", "quant_codec", "quant_block_size", "quant_eps",
                 "primitive", "max_num_splats", "num_sh", "packed", "sh_optim_bits", "sh_value_bits",
                 "non_sh_optim_bits", "use_fused_proj_bwd_optim", "use_per_splat_bias_correction",
                 "bilagrid_rgb", "bilagrid_depth", "bilagrid_normal", "ppisp", "background_sh"}) {
            const JsonValue* a = source.state.find(key), *b = endpoint.state.find(key);
            update_require(a && b && same_update_json(*a, *b),
                           "source/endpoint layout mismatch " + std::string(key));
        }
        update_require(endpoint.arrays.size() == source.arrays.size(), "checkpoint array set changed");
        for (const auto& entry : source.arrays) {
            const auto found = endpoint.arrays.find(entry.first);
            update_require(found != endpoint.arrays.end() &&
                           found->second.descr == entry.second.descr &&
                           found->second.data_bytes == entry.second.data_bytes,
                           "checkpoint array layout changed " + entry.first);
        }
        if (i == 1) continue;
        const auto& reference = archives[1].state;
        update_require(reference.obj.size() == endpoint.state.obj.size(), "endpoint metadata key set changed");
        for (const auto& entry : reference.obj) {
            if (entry.first == "arrays") continue;
            const auto* other = endpoint.state.find(entry.first);
            update_require(other && same_update_json(entry.second, *other),
                           "endpoint metadata mismatch " + entry.first);
        }
    }
}

void initialize_native_update_rows(std::array<NativeUpdateArchive, 4>& archives,
                                   const std::array<Buffer, 4>& means,
                                   NativeUpdateRows& rows, std::ostream& out) {
    const int64_t n = archives[0].capacity;
    const int64_t updates = archives[1].step - archives[0].step;
    std::array<Buffer, 4> clocks;
    for (size_t i = 0; i < 4; ++i) {
        rows.status[i].resize((size_t)n);
        if (archives[0].row_clocks)
            clocks[i] = read_npy_buffer(archives[i].in,
                update_array(archives[i], "eng.bias_correction_steps", "<i4", n, sizeof(int32_t)));
        std::array<uint64_t, 8> nan_patterns{};
        uint64_t prefix_nan = 0, inf_rows = 0;
        for (int64_t row = 0; row < n; ++row) {
            uint8_t status = row >= archives[i].current ? 16 : 0;
            for (int component = 0; component < 3; ++component) {
                const int kind = update_value_class(update_load<float>(means[i], 3 * row + component));
                if (kind == 1) status |= uint8_t(1u << component);
                if (kind >= 2) status |= 8;
            }
            rows.status[i][(size_t)row] = status;
            ++nan_patterns[status & 7];
            prefix_nan += (status & 7) != 0 && row < archives[i].current;
            inf_rows += (status & 8) != 0;
            if (!clocks[i].bytes.empty()) {
                const int32_t clock = update_load<int32_t>(clocks[i], row);
                update_require(clock >= 0 && clock <= archives[i].step,
                               "invalid row clock at " + std::to_string(row) + " in " +
                               archives[i].directory.string());
            }
        }
        out << "{\"type\":\"row_inventory\",\"state\":" << json_quote(kUpdateStates[i])
            << ",\"current_prefix_rows\":" << archives[i].current
            << ",\"current_prefix_nan_mean_rows\":" << prefix_nan
            << ",\"mean_inf_rows\":" << inf_rows << ",\"mean_nan_component_mask_histogram\":[";
        for (size_t mask = 0; mask < 8; ++mask)
            out << (mask ? "," : "") << nan_patterns[mask];
        out << "]}\n";
    }
    rows.cohort.assign((size_t)n, 0);
    for (int64_t row = 0; row < n; ++row) {
        bool unchanged = archives[0].row_clocks && rows.status[0][(size_t)row] == 0;
        for (size_t i = 1; i < 4; ++i) {
            unchanged &= rows.status[i][(size_t)row] == 0;
            if (archives[0].row_clocks)
                unchanged &= int64_t(update_load<int32_t>(clocks[i], row)) -
                             update_load<int32_t>(clocks[0], row) == updates;
        }
        rows.cohort[(size_t)row] = unchanged ? 1 : 0;
        rows.cohort_rows += unchanged;
    }
    for (size_t i = 1; i < 4; ++i) {
        uint64_t nan_changes = 0, footprint_changes = 0, birth = 0, inactive = 0;
        uint64_t clock_below = 0, clock_above = 0, clock_decreased = 0;
        std::map<int64_t, uint64_t> deltas;
        int64_t first_nan_change = -1, first_clock_change = -1;
        for (int64_t row = 0; row < n; ++row) {
            const uint8_t a = rows.status[0][(size_t)row], b = rows.status[i][(size_t)row];
            const bool nan_changed = (a & 7) != (b & 7);
            nan_changes += nan_changed;
            footprint_changes += a != b;
            birth += (a & 23) != 0 && b == 0;
            inactive += a == 0 && (b & 23) != 0;
            if (nan_changed && first_nan_change < 0) first_nan_change = row;
            if (archives[0].row_clocks) {
                const int64_t delta = int64_t(update_load<int32_t>(clocks[i], row)) -
                                      update_load<int32_t>(clocks[0], row);
                ++deltas[delta];
                clock_below += delta < updates;
                clock_above += delta > updates;
                clock_decreased += delta < 0;
                if (delta != updates && first_clock_change < 0) first_clock_change = row;
            }
        }
        out << "{\"type\":\"topology_observation\",\"endpoint\":" << json_quote(kUpdateStates[i])
            << ",\"scope\":\"all_capacity_slots_descriptive\",\"row_identity_proven\":false"
               ",\"mean_nan_pattern_changed_rows\":" << nan_changes
            << ",\"mean_nonfinite_or_prefix_status_changed_rows\":" << footprint_changes
            << ",\"birth_or_reactivation_candidates\":" << birth
            << ",\"became_nan_inactive_or_left_prefix_candidates\":" << inactive
            << ",\"first_nan_pattern_change_row\":" << first_nan_change
            << ",\"row_clocks_available\":" << (archives[0].row_clocks ? "true" : "false")
            << ",\"logical_updates\":" << updates
            << ",\"clock_delta_below_logical_updates_rows\":" << clock_below
            << ",\"clock_delta_above_logical_updates_rows\":" << clock_above
            << ",\"clock_decreased_rows\":" << clock_decreased
            << ",\"first_unexpected_clock_delta_row\":" << first_clock_change
            << ",\"clock_delta_histogram\":[";
        bool first = true;
        for (const auto& entry : deltas) {
            out << (first ? "" : ",") << "{\"delta\":" << entry.first << ",\"rows\":" << entry.second << '}';
            first = false;
        }
        out << "]}\n";
    }
    if (archives[0].row_clocks) {
        for (size_t i : {size_t(2), size_t(3)}) {
            uint64_t equal = 0;
            int64_t maxabs = 0, first_mismatch = -1;
            for (int64_t row = 0; row < n; ++row) {
                const int64_t delta = int64_t(update_load<int32_t>(clocks[i], row)) -
                                      update_load<int32_t>(clocks[1], row);
                equal += delta == 0;
                maxabs = std::max(maxabs, delta < 0 ? -delta : delta);
                if (delta && first_mismatch < 0) first_mismatch = row;
            }
            out << "{\"type\":\"clock_comparison\",\"field\":\"eng.bias_correction_steps\""
                   ",\"reference\":\"nativeA\",\"candidate\":" << json_quote(kUpdateStates[i])
                << ",\"scope\":\"all_capacity_slots_descriptive\",\"exact_equal_rows\":" << equal
                << ",\"exact_unequal_rows\":" << n - equal << ",\"maxabs\":" << maxabs
                << ",\"first_mismatch_row\":" << first_mismatch << "}\n";
        }
    }
    out << "{\"type\":\"row_cohort\",\"scope\":\"no_observed_clock_reset_or_mean_nan_change\""
           ",\"rows\":" << rows.cohort_rows
        << ",\"requires_finite_means_and_current_prefix_in_all_four\":true"
           ",\"requires_source_clock_plus_logical_updates_in_all_endpoints\":true"
           ",\"row_identity_proven\":false,\"copied_clocks_or_intermediate_births_may_be_unobservable\":true}\n";
}

}  // namespace


// ===========================================================================
// Public entry points
// ===========================================================================

void compare_native_checkpoint_updates(const fs::path& source,
                                      const fs::path& reference,
                                      const fs::path& actual,
                                      const fs::path& native_repeat,
                                      const fs::path& output_directory) {
    update_require(fs::create_directory(output_directory), "output directory must be fresh");
    std::ofstream out(output_directory / "native-checkpoint-updates.jsonl", std::ios::binary);
    update_require(bool(out), "cannot open update report");
    out.imbue(std::locale::classic());
    out << std::setprecision(17);
    out << "{\"type\":\"scope\",\"schema\":\"native_checkpoint_updates_v1\",\"cpu_only\":true"
           ",\"acceptance_tolerance\":null,\"byte_identity_qualified\":false,\"numerically_qualified\":false"
           ",\"quality_recovery_qualified\":false,\"lineage_proven\":false,\"row_identity_proven\":false"
           ",\"numeric_scope\":\"finite_endpoint_pairs_and_finite_source_endpoint_triples_only\""
           ",\"zero_denominator_ratio\":null,\"topology_may_be_active\":true"
           ",\"source_camera_runtime_metadata_and_sh_degree_counter_may_change\":true"
           ",\"config_checked_fields\":[\"num_iterations\"]}\n";
    try {
        std::array<NativeUpdateArchive, 4> archives{
            NativeUpdateArchive(source), NativeUpdateArchive(reference),
            NativeUpdateArchive(actual), NativeUpdateArchive(native_repeat)};
        validate_native_update_inputs(archives);
        for (size_t i = 0; i < 4; ++i) {
            const auto& archive = archives[i];
            out << "{\"type\":\"input\",\"state\":" << json_quote(kUpdateStates[i])
                << ",\"checkpoint\":" << json_quote(fs::absolute(archive.directory).string())
                << ",\"completed_step\":" << archive.step << ",\"horizon\":" << archive.horizon
                << ",\"current_rows\":" << archive.current << ",\"capacity\":" << archive.capacity
                << ",\"num_sh\":" << archive.num_sh << ",\"sh_optim_bits\":" << archive.sh_optim_bits
                << ",\"sh_value_bits\":" << archive.sh_value_bits
                << ",\"non_sh_optim_bits\":" << archive.non_sh_optim_bits
                << ",\"row_clocks\":" << (archive.row_clocks ? "true" : "false")
                << ",\"array_count\":" << archive.arrays.size() << "}\n";
        }
        const auto fields = native_update_fields(archives[0]);
        std::set<std::string> handled;
        for (const auto& field : fields) {
            const bool quantized = field.codec != UpdateCodec::Float;
            const std::string slot = field.slot + (quantized ? ".q" : "");
            update_require(handled.insert(slot).second, "duplicate field coverage " + slot);
            const int64_t blocks = quantized ? num_blocks(field.storage_cells, field.bounds_stride) : 0;
            for (const auto& archive : archives) {
                update_array(archive, slot, quantized ? "|u1" : "<f4",
                             field.storage_cells, field.allocated_bytes_per_cell);
                if (quantized)
                    update_array(archive, field.slot + ".qb", "<f4", blocks,
                                 field.adam() ? sizeof(float4) : sizeof(float2));
            }
            if (quantized) handled.insert(field.slot + ".qb");
        }
        if (archives[0].row_clocks) {
            for (const auto& archive : archives)
                update_array(archive, "eng.bias_correction_steps", "<i4",
                             archive.capacity, sizeof(int32_t));
            handled.insert("eng.bias_correction_steps");
        }
        uint64_t transient_arrays = 0;
        for (const auto& entry : archives[0].arrays) {
            if (handled.count(entry.first)) continue;
            PoolSlot slot;
            uint32_t sub;
            update_require(parse_saved_name(entry.first, slot, sub) && slot_save(slot) == SaveClass::Never,
                           "unhandled persistent training/appearance array " + entry.first);
            ++transient_arrays;
            out << "{\"type\":\"array_classification\",\"slot\":" << json_quote(entry.first)
                << ",\"classification\":\"transient_not_persistent_training_state\",\"compared\":false"
                   ",\"native_save_class\":\"never\",\"descr\":" << json_quote(entry.second.descr)
                << ",\"bytes_per_checkpoint\":" << entry.second.data_bytes << "}\n";
        }
        NativeUpdateRows rows;
        std::array<uint64_t, 4> unexpected{};
        uint64_t decoded_fields = 0;
        for (const auto& field : fields) {
            std::array<Buffer, 4> values, bounds;
            const bool quantized = field.codec != UpdateCodec::Float;
            const std::string slot = field.slot + (quantized ? ".q" : "");
            for (size_t i = 0; i < 4; ++i) {
                values[i] = read_npy_buffer(archives[i].in, archives[i].arrays.at(slot));
                if (!quantized) continue;
                bounds[i] = read_npy_buffer(archives[i].in, archives[i].arrays.at(field.slot + ".qb"));
                for (size_t pair = 0; pair < bounds[i].bytes.size() / sizeof(float2); ++pair) {
                    const float2 range = update_load<float2>(bounds[i], (int64_t)pair);
                    const bool logarithmic = field.codec == UpdateCodec::Log || (field.adam() && (pair & 1));
                    update_require(!update_value_class(range.x) && !update_value_class(range.y) &&
                                   range.x <= range.y && (!logarithmic || range.x >= 0),
                                   "nonfinite/unordered/invalid raw quantization bounds " +
                                   field.slot + ".qb pair " + std::to_string(pair) + " in " +
                                   archives[i].directory.string());
                }
            }
            if (field.name == "world.means")
                initialize_native_update_rows(archives, values, rows, out);
            int64_t used_bytes = field.storage_cells * field.allocated_bytes_per_cell;
            if (field.codec == UpdateCodec::Adam) used_bytes = field.storage_cells * (field.bits / 4);
            if (field.codec == UpdateCodec::Log)
                used_bytes = field.bits == 4 ? (field.storage_cells + 1) / 2 : field.storage_cells;
            const char* codec = field.codec == UpdateCodec::Float ? "fp32" :
                field.codec == UpdateCodec::Linear ? "QuantizedTensor" :
                field.adam() ? "QuantizedAdamState.decode_g1g2" : "QuantizedTensorLog";
            out << "{\"type\":\"storage\",\"field\":" << json_quote(field.name)
                << ",\"slot\":" << json_quote(field.slot) << ",\"codec\":" << json_quote(codec)
                << ",\"bits\":" << field.bits << ",\"logical_cells\":" << field.cells()
                << ",\"stored_cells\":" << field.storage_cells
                << ",\"excluded_physical_padding_cells\":" << field.storage_cells - field.cells()
                << ",\"fpbo_sh_addressing\":" << (field.sh_fpbo ? "true" : "false")
                << ",\"excluded_overallocated_packed_bytes\":"
                << field.storage_cells * field.allocated_bytes_per_cell - used_bytes
                << ",\"all_raw_bounds_validated\":" << (quantized ? "true" : "null") << "}\n";
            compare_native_update_field(field, values, bounds, rows, out, unexpected);
            decoded_fields += field.adam() ? 2 : 1;
            out.flush();
            update_require(bool(out), "cannot write update report");
        }
        out << "{\"type\":\"summary\",\"complete\":true,\"decoded_fields\":" << decoded_fields
            << ",\"covered_persistent_arrays\":" << handled.size()
            << ",\"classified_transient_arrays\":" << transient_arrays
            << ",\"logical_updates\":" << archives[1].step - archives[0].step
            << ",\"unchanged_observation_cohort_rows\":" << rows.cohort_rows
            << ",\"unexpected_nonfinite_cells\":{";
        for (size_t i = 0; i < 4; ++i)
            out << (i ? "," : "") << json_quote(kUpdateStates[i]) << ':' << unexpected[i];
        out << "},\"numerically_qualified\":false,\"row_identity_proven\":false"
               ",\"quality_recovery_qualified\":false}\n";
        out.close();
        update_require(bool(out), "cannot finish update report");
    } catch (const std::exception& error) {
        if (out.is_open()) {
            out << "{\"type\":\"error\",\"complete\":false,\"message\":" << json_quote(error.what()) << "}\n";
            out.flush();
        }
        throw;
    }
}

bool needs_adapt(const JsonValue& state, const TargetLayout& t) {
    bool shape_changed =
        (int64_t)state.get_double("max_num_splats", 0) != t.max_num_splats;
    shape_changed |= (int)state.get_double("num_sh", 0) != t.num_sh;
    shape_changed |=
        (int64_t)state.get_double("cur_num_splats", 0) > t.max_num_splats;
    const double sh_optim_bits = state.get_double("sh_optim_bits", 32);
    const double sh_value_bits = state.get_double("sh_value_bits", 32);
    const double non_sh_optim_bits =
        state.get_double("non_sh_optim_bits", 32);
    const double fused =
        state.get_double("use_fused_proj_bwd_optim", 0);
    const double bias =
        state.get_double("use_per_splat_bias_correction", 0);
    if (sh_optim_bits != t.sh_optim_bits ||
        sh_value_bits != t.sh_value_bits ||
        non_sh_optim_bits != t.non_sh_optim_bits ||
        fused != (t.use_fused_proj_bwd_optim ? 1 : 0) ||
        bias != (t.use_per_splat_bias_correction ? 1 : 0))
        throw std::runtime_error(
            "resume: changing optimizer storage layout is unsupported");
    for (const char* which : {"rgb", "depth", "normal"}) {
        BilagridState b = bilagrid_state(state, which);
        const auto& tg = target_grid(t, which);
        if (b.enabled != tg.has_value()) shape_changed = true;
        if (!b.enabled && tg)
            throw std::runtime_error(
                "resume: adding a bilagrid channel is unsupported");
        if (b.enabled && tg &&
            (b.optim_bits != t.bilagrid_optim_bits ||
             b.value_bits != t.bilagrid_value_bits ||
             b.use_adagrad != t.bilagrid_use_adagrad))
            throw std::runtime_error(
                "resume: changing bilagrid storage layout is unsupported");
        if (b.enabled && tg && std::strcmp(which, "rgb") == 0 &&
            b.type != t.bilagrid_type)
            throw std::runtime_error(
                "resume: changing bilagrid type is unsupported");
        if (b.enabled && tg && b.lhw != *tg) shape_changed = true;
    }
    const JsonValue* background = state.find("background_sh");
    if (background) {
        const double enabled = background->get_double("enabled", -1);
        if (enabled != 0 && enabled != 1)
            throw std::runtime_error(
                "resume: checkpoint has invalid background SH metadata");
        const bool ck_background_sh = enabled == 1;
        if (ck_background_sh != t.background_sh)
            throw std::runtime_error(
                "resume: changing background mode to or from SH is unsupported");
        if (ck_background_sh) {
            const int degree = checked_state_int(
                *background, "degree", 0, 1024);
            if (degree != t.background_sh_degree)
                throw std::runtime_error(
                    "resume: changing background SH degree is unsupported");
        }
    } else if (t.background_sh) {
        throw std::runtime_error(
            "resume: checkpoint has no background SH state");
    }
    const JsonValue* p = state.find("ppisp");
    bool ck_ppisp = p && p->get_double("enabled", 0) != 0;
    if (!ck_ppisp && t.ppisp)
        throw std::runtime_error("resume: adding PPISP is unsupported");
    if (ck_ppisp != t.ppisp) shape_changed = true;
    if (ck_ppisp && t.ppisp) {
        const JsonValue* type = p->find("param_type");
        if (!type || type->type != JsonValue::Type::String)
            throw std::runtime_error(
                "resume: checkpoint has invalid PPISP metadata");
        if (type->as_string() != t.ppisp_param_type)
            throw std::runtime_error(
                "resume: changing PPISP parameter type is unsupported");
        const double use_adagrad = p->get_double("use_adagrad", -1);
        if (use_adagrad != (t.ppisp_use_adagrad ? 1 : 0))
            throw std::runtime_error(
                "resume: changing PPISP optimizer is unsupported");
    }
    if (shape_changed && state.find("b1"))
        throw std::runtime_error(
            "resume: block checkpoints cannot change layout or ownership");
    return shape_changed;
}


bool adapt_checkpoint(const fs::path& ckpt_dir, const TargetLayout& t,
                      const fs::path& out_dir) {
    if (t.max_num_splats <= 0 ||
        t.max_num_splats > std::numeric_limits<int32_t>::max() ||
        t.num_sh < 0 || t.num_sh > 1024 || t.num_images < 0)
        throw std::runtime_error("checkpoint adapt: invalid target layout");
    for (const auto* grid :
         {&t.bilagrid_rgb, &t.bilagrid_depth, &t.bilagrid_normal})
        if (*grid) checked_grid_cells(**grid, 1);

    std::ifstream sin((ckpt_dir / "state.tar").string(), std::ios::binary);
    if (!sin) throw std::runtime_error("cannot open " +
                                       (ckpt_dir / "state.tar").string());
    std::string state_text;
    for (const auto& m : tar_index(sin))
        if (m.name == "state.json") { state_text = tar_read_member(sin, m); break; }
    if (state_text.empty())
        throw std::runtime_error("state.json missing in " + ckpt_dir.string());
    JsonValue state = json_parse(state_text);

    if (!needs_adapt(state, t)) return false;

    const int64_t max_ck = checked_state_int(
        state, "max_num_splats", 1, std::numeric_limits<int32_t>::max());
    const int64_t cur_ck = checked_state_int(
        state, "cur_num_splats", 0, (int)max_ck);
    const int K_ck = checked_state_int(state, "num_sh", 0, 1024);
    const int64_t max_new = t.max_num_splats;
    const int     K_new   = t.num_sh;
    const int64_t n_img   = t.num_images;

    const double raw_sh_optim_bits =
        state.get_double("sh_optim_bits", 32);
    if (raw_sh_optim_bits != 4 && raw_sh_optim_bits != 8 &&
        raw_sh_optim_bits != 32)
        throw std::runtime_error(
            "checkpoint adapt: unsupported SH optimizer width");
    const int sh_optim_bits = (int)raw_sh_optim_bits;
    std::map<std::string, Buffer> arrs = read_tar_npy(ckpt_dir);

    // Group the ".q" / ".qb" halves of a quantized slot under one logical name.
    struct Logical { const Buffer* plain = nullptr; const Buffer* q = nullptr;
                     const Buffer* qb = nullptr; };
    std::map<std::string, Logical> logical;
    for (const auto& [name, buf] : arrs) {
        if (ends_with(name, ".qb"))     logical[name.substr(0, name.size()-3)].qb = &buf;
        else if (ends_with(name, ".q")) logical[name.substr(0, name.size()-2)].q = &buf;
        else                            logical[name].plain = &buf;
    }

    auto it_op = logical.find("world.opacities");
    if (it_op == logical.end() || !it_op->second.plain)
        throw std::runtime_error(
            "checkpoint adapt: world.opacities missing -- a checkpoint saved "
            "with save_full_checkpoint is required to resume with a different "
            "number of Gaussians.");
    const int64_t target_cur = std::min(cur_ck, max_new);
    std::vector<int64_t> keep = keep_indices(it_op->second.plain->f32(),
                                             cur_ck, max_ck, target_cur);

    std::map<std::string, Buffer> emit;

    for (const auto& [base, parts] : logical) {
        std::optional<SlotClass> cls = classify(base, sh_optim_bits);

        if (cls && !cls->quant) {                       // plain per-splat float
            if (!parts.plain) continue;
            if (cls->is_int) {
                // bias_correction_steps: gather int32 rows the same way.
                const int32_t* src = parts.plain->i32();
                std::vector<int32_t> out((size_t)(max_new * cls->ch), 0);
                for (size_t k = 0; k < keep.size(); k++)
                    out[k] = src[keep[k]];
                emit[base] = make_buffer("<i4", out.data(),
                                         out.size() * sizeof(int32_t));
            } else if (cls->is_sh) {
                emit[base] = from_floats(resample_sh(parts.plain->f32(), K_ck,
                                                     keep, K_new, max_new));
            } else {
                emit[base] = from_floats(gather_rows(parts.plain->f32(), cls->ch,
                                                     keep, max_new));
            }
            continue;
        }

        if (cls && cls->quant) {                        // quantized per-splat
            if (!parts.q || !parts.qb) continue;
            const int cpp_ck  = cls->is_sh ? 3 * K_ck  : cls->cpp;
            const int cpp_new = cls->is_sh ? 3 * K_new : cls->cpp;
            const int64_t n_ck  = max_ck  * cpp_ck;
            const int64_t n_new = max_new * cpp_new;
            // The FPBO SH stream is neither splat-major nor exactly n cells
            // long (docs/notes/sh-quant-layout.md); the reindex below wants
            // splat-major, so transpose in and back out.
            const bool sh_fpbo = cls->fpbo && cls->is_sh;
            const ShQuantAddr a_ck =
                sh_quant_addr((uint32_t)K_ck, sh_fpbo ? 0 : 256);
            const ShQuantAddr a_new =
                sh_quant_addr((uint32_t)K_new, sh_fpbo ? 0 : 256);
            const int64_t lin_ck = sh_fpbo
                ? sh_fpbo_cells(max_ck, (uint32_t)K_ck) : n_ck;
            const int64_t lin_new = sh_fpbo
                ? sh_fpbo_cells(max_new, (uint32_t)K_new) : n_new;
            const int64_t bbc_ck = sh_fpbo ? a_ck.bounds_stride
                                           : (cls->fpbo ? kBlock * cpp_ck : kBlock);
            const int64_t bbc_new = sh_fpbo ? a_new.bounds_stride
                                            : (cls->fpbo ? kBlock * cpp_new : kBlock);

            auto to_splat_major = [&](std::vector<float>& v) {
                if (!sh_fpbo) return;
                std::vector<float> out((size_t)n_ck, 0.0f);
                for (int64_t i = 0; i < max_ck; i++)
                    for (int c = 0; c < cpp_ck; c++)
                        out[(size_t)(i * cpp_ck + c)] =
                            v[(size_t)a_ck.cell(a_ck.base(i), c)];
                v.swap(out);
            };
            auto from_splat_major = [&](std::vector<float>& v) {
                if (!sh_fpbo) return;
                std::vector<float> out((size_t)lin_new, 0.0f);
                for (int64_t i = 0; i < max_new; i++)
                    for (int c = 0; c < cpp_new; c++)
                        out[(size_t)a_new.cell(a_new.base(i), c)] =
                            v[(size_t)(i * cpp_new + c)];
                v.swap(out);
            };

            auto reindex_stream = [&](const std::vector<float>& v) {
                if (cls->is_sh)
                    return resample_sh(v.data(), K_ck, keep, K_new, max_new);
                return gather_rows(v.data(), cpp_ck, keep, max_new);
            };

            std::vector<uint8_t> pk;
            std::vector<float> bd;
            if (cls->adam) {
                std::vector<float> g1, g2;
                decode_adam(cls->bits, parts.q->u8(), parts.qb->f32(),
                            lin_ck, bbc_ck, g1, g2);
                to_splat_major(g1);
                to_splat_major(g2);
                std::vector<float> g1n = reindex_stream(g1);
                std::vector<float> g2n = reindex_stream(g2);
                if ((int64_t)g1n.size() != n_new)
                    throw std::runtime_error("checkpoint adapt: adam reindex size");
                from_splat_major(g1n);
                from_splat_major(g2n);
                encode_adam(cls->bits, g1n, g2n, bbc_new, pk, bd);
                if (cls->is_sh && cls->bits == 4)
                    pk.resize((size_t)lin_new *
                              QuantizedAdamState<8>::kBytesPerCell, 0);
            } else {
                std::vector<float> v;
                decode_linear(cls->bits, parts.q->u8(), parts.qb->f32(),
                              lin_ck, bbc_ck, v);
                to_splat_major(v);
                std::vector<float> vn = reindex_stream(v);
                if ((int64_t)vn.size() != n_new)
                    throw std::runtime_error("checkpoint adapt: linear reindex size");
                from_splat_major(vn);
                encode_linear(cls->bits, vn, bbc_new, pk, bd);
            }
            emit[base + ".q"]  = from_bytes(pk);
            emit[base + ".qb"] = from_floats(bd);
            continue;
        }

        // --- bilagrid ------------------------------------------------------
        if (starts_with(base, "eng.bg.") && !starts_with(base, "eng.bg_sky.")) {
            // eng.bg.<t>.<field...>
            std::string rest = base.substr(7);
            size_t dot = rest.find('.');
            if (dot == std::string::npos) continue;
            std::string which = rest.substr(0, dot);
            std::string field = rest.substr(dot + 1);
            const auto& tg = target_grid(t, which);
            if (!tg) continue;                       // target dropped it
            BilagridState bs = bilagrid_state(state, which.c_str());
            const int64_t target_cells = checked_grid_cells(*tg, bs.C);
            if (n_img > 0 &&
                target_cells > std::numeric_limits<int64_t>::max() / n_img)
                throw std::runtime_error(
                    "checkpoint adapt: bilagrid target is too large");
            const int64_t target_total = target_cells * n_img;
            const bool same = bs.lhw == *tg;

            if (field == "scalars") {                // per-image, shape-invariant
                if (parts.plain) emit[base] = *parts.plain;
                continue;
            }
            if (field == "grids") {
                if (!parts.plain) continue;
                const int64_t cells = checked_grid_cells(bs.lhw, bs.C);
                const int64_t n_grids = (int64_t)parts.plain->numel() / cells;
                if (n_grids != n_img) image_count_error("bilagrid", n_grids, n_img);
                emit[base] = same ? *parts.plain
                                  : from_floats(resample_grid(parts.plain->f32(),
                                                              n_grids, bs.lhw,
                                                              bs.C, *tg));
                continue;
            }
            if (field == "grids_q") {
                if (!parts.q || !parts.qb) continue;
                const int bits = bs.value_bits == 16 ? 16 : 8;
                const int64_t cells = checked_grid_cells(bs.lhw, bs.C);
                const int bytes_per_cell = bits == 16 ? 2 : 1;
                if (cells > std::numeric_limits<int64_t>::max() / bytes_per_cell)
                    throw std::runtime_error(
                        "checkpoint adapt: bilagrid source is too large");
                const int64_t n_grids =
                    (int64_t)parts.q->bytes.size() /
                    (cells * bytes_per_cell);
                if (n_grids != n_img) image_count_error("bilagrid", n_grids, n_img);
                if (same) {
                    emit[base + ".q"] = *parts.q;
                    emit[base + ".qb"] = *parts.qb;
                    continue;
                }
                std::vector<float> v;
                decode_linear(bits, parts.q->u8(), parts.qb->f32(),
                              n_grids * cells, kBlock, v);
                std::vector<float> vr = resample_grid(v.data(), n_grids, bs.lhw,
                                                      bs.C, *tg);
                std::vector<uint8_t> pk;
                std::vector<float> bd;
                encode_linear(bits, vr, kBlock, pk, bd);
                emit[base + ".q"]  = from_bytes(pk);
                emit[base + ".qb"] = from_floats(bd);
                continue;
            }
            if (same) {
                if (parts.plain) emit[base] = *parts.plain;
                else if (parts.q && parts.qb) {
                    emit[base + ".q"] = *parts.q;
                    emit[base + ".qb"] = *parts.qb;
                }
            } else if (parts.plain) {
                emit[base] =
                    from_floats(std::vector<float>((size_t)target_total, 0.0f));
            } else if (parts.q && parts.qb &&
                       (field == "bg_ag" || field == "bg_quant")) {
                const size_t packed_per_cell = field == "bg_quant" ? 2 : 1;
                const size_t bounds_per_block = field == "bg_quant" ? 4 : 2;
                const size_t cells = (size_t)target_total;
                emit[base + ".q"] = from_bytes(
                    std::vector<uint8_t>(cells * packed_per_cell, 0));
                emit[base + ".qb"] = from_floats(std::vector<float>(
                    ((cells + kBlock - 1) / kBlock) * bounds_per_block, 0.0f));
            }
            continue;
        }

        // --- PPISP ---------------------------------------------------------
        if (starts_with(base, "eng.ppisp.")) {
            if (!t.ppisp || !parts.plain) continue;   // target dropped it
            const JsonValue* pp = state.find("ppisp");
            const int P = pp ? (int)pp->get_double("num_params", 0) : 0;
            if (P > 0) {
                const int64_t n_cam = (int64_t)parts.plain->numel() / P;
                if (n_cam != n_img) image_count_error("PPISP", n_cam, n_img);
            }
            emit[base] = *parts.plain;               // per-image table
            continue;
        }

        // --- everything else (bg_sky, colour matrices): fixed size ---------
        if (parts.plain) emit[base] = *parts.plain;
        else if (parts.q && parts.qb) {
            emit[base + ".q"] = *parts.q;
            emit[base + ".qb"] = *parts.qb;
        }
    }

    std::string st = state_text;
    auto set_int = [&st](const char* key, int64_t v) {
        std::string pat = std::string("\"") + key + "\":";
        size_t k = st.find(pat);
        if (k == std::string::npos) return;
        size_t b = st.find_first_not_of(" \t", k + pat.size());
        size_t e = st.find_first_of(",\n}", b);
        st.replace(b, e - b, std::to_string(v));
    };
    auto set_object_int = [&st](const std::string& object,
                                const char* key, int64_t v) {
        const std::string object_pat = "\"" + object + "\"";
        const size_t object_key = st.find(object_pat);
        const size_t begin = object_key == std::string::npos
            ? std::string::npos : st.find('{', object_key + object_pat.size());
        const size_t end = begin == std::string::npos
            ? std::string::npos : st.find('}', begin + 1);
        const std::string field_pat = std::string("\"") + key + "\":";
        const size_t field = begin == std::string::npos
            ? std::string::npos : st.find(field_pat, begin + 1);
        if (field == std::string::npos || field > end)
            throw std::runtime_error(
                "checkpoint adapt: missing " + object + "." + key);
        const size_t value =
            st.find_first_not_of(" \t", field + field_pat.size());
        const size_t value_end = st.find_first_of(",}", value);
        if (value == std::string::npos || value_end > end)
            throw std::runtime_error(
                "checkpoint adapt: invalid " + object + "." + key);
        st.replace(value, value_end - value, std::to_string(v));
    };
    set_int("format_version", 2);
    set_int("cur_num_splats", target_cur);
    set_int("max_num_splats", max_new);
    set_int("num_sh", K_new);
    for (const char* which : {"rgb", "depth", "normal"}) {
        const BilagridState source = bilagrid_state(state, which);
        if (!source.enabled) continue;
        const auto& grid = target_grid(t, which);
        const std::string object = std::string("bilagrid_") + which;
        set_object_int(object, "enabled", grid ? 1 : 0);
        if (grid) {
            set_object_int(object, "L", (*grid)[0]);
            set_object_int(object, "H", (*grid)[1]);
            set_object_int(object, "W", (*grid)[2]);
        }
    }
    const JsonValue* source_ppisp = state.find("ppisp");
    if (source_ppisp && source_ppisp->get_double("enabled", 0) == 1)
        set_object_int("ppisp", "enabled", t.ppisp ? 1 : 0);

    std::ostringstream manifest;
    manifest << '[';
    bool first = true;
    for (const auto& [name, buf] : emit) {
        if (buf.bytes.empty()) continue;
        if (!first) manifest << ',';
        manifest << "\n    {\"name\": \"" << name
                 << "\", \"descr\": \"" << buf.descr
                 << "\", \"bytes\": " << buf.bytes.size() << '}';
        first = false;
    }
    if (!first) manifest << '\n';
    manifest << "  ]";

    const size_t manifest_key = st.find("\"arrays\"");
    if (manifest_key == std::string::npos) {
        const size_t object_end = st.find_last_of('}');
        if (object_end == std::string::npos)
            throw std::runtime_error("checkpoint adapt: invalid state.json");
        st.insert(object_end, ",\n  \"arrays\": " + manifest.str() + "\n");
    } else {
        const size_t array_begin = st.find('[', manifest_key);
        const size_t array_end = st.find(']', array_begin);
        if (array_begin == std::string::npos || array_end == std::string::npos)
            throw std::runtime_error("checkpoint adapt: invalid arrays manifest");
        st.replace(array_begin, array_end - array_begin + 1, manifest.str());
    }

    fs::create_directories(out_dir);
    std::ofstream out((out_dir / "state.tar").string(), std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " +
                                       (out_dir / "state.tar").string());
    tar_write_bytes(out, "state.json", st.data(), st.size());
    for (const auto& [name, buf] : emit)
        if (!buf.bytes.empty()) write_npy_member(out, name + ".npy", buf);
    tar_finish(out);
    out.close();
    if (!out) throw std::runtime_error("cannot finish " +
                                       (out_dir / "state.tar").string());
    return true;
}

}  // namespace ckpt
