// Adapt.cpp -- see Adapt.h.

#include "checkpoint/Adapt.h"

#include "core/CheckpointIO.h"
#include "core/Tensor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <limits>
#include <numeric>
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

std::map<std::string, Buffer> read_tar_npy(const fs::path& ckpt_dir) {
    std::ifstream in((ckpt_dir / "state.tar").string(), std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " +
                                      (ckpt_dir / "state.tar").string());
    std::map<std::string, Buffer> out;
    for (const auto& m : tar_index(in)) {
        if (m.name.size() < 4 ||
            m.name.compare(m.name.size() - 4, 4, ".npy") != 0) continue;
        NpyInfo info = npy_locate(in, m.data_offset, m.size);
        Buffer b;
        b.descr = info.descr;
        b.bytes.resize((size_t)info.data_bytes);
        in.clear();
        in.seekg((std::streamoff)info.data_offset, std::ios::beg);
        in.read(b.bytes.data(), (std::streamsize)info.data_bytes);
        out.emplace(m.name.substr(0, m.name.size() - 4), std::move(b));
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

}  // namespace


// ===========================================================================
// Public entry points
// ===========================================================================

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
