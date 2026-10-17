// Backend parity tool for the multi-scale per-pixel loss stack
// (compute_multi_scale_per_pixel_losses: per-pixel fused losses, fused SSIM
// backward, image pyramid, edge-aware densification maps) plus the batched
// quantile radix-select. The SAME source builds under both backends:
//
//   CUDA build:   ./msloss_parity dump ref.bin
//   Vulkan build: ./msloss_parity compare ref.bin   (per device)
//
// Ref format: [nf tight floats] [nl loose floats]. Which values go in which
// channel, and the one expected CUDA/Vulkan mismatch: docs/testing.md.

#include <kernels/loss/PerPixelLoss.cuh>
#include <kernels/densify/Densify.cuh>
#include <core/Tensor.h>
#ifdef SS_BACKEND_VULKAN
#include "backend/vulkan/kernels/ImageStrip.h"
#include <kernels/pixelwise/PixelWise.cuh>
#endif

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <random>
#include <vector>
#include <stdexcept>
#include <string>
#include <utility>

using backend::MemcpyKind;

template <bool invert_quantile>
int batch_quantile_masked_radix_select(
    const float* d_x, int B, int N, float q, float* d_out, uint32_t* temp,
    backend::Stream stream);

template <bool invert_quantile>
int batch_quantile_positive_radix_select(
    const float* d_x, int B, int N, float q, float* d_out, uint32_t* temp,
    backend::Stream stream);

static std::vector<float> g_tight, g_loose;

template <typename T>
T* upload(const std::vector<T>& host) {
    T* d = (T*)backend::device_malloc(((host.size() * sizeof(T) + 3) / 4) * 4);
    backend::memcpy_sync(d, host.data(), host.size() * sizeof(T),
                         MemcpyKind::HostToDevice);
    return d;
}

template <typename T>
T* alloc_zero(int64_t n) {
    int64_t bytes = ((n * (int64_t)sizeof(T) + 3) / 4) * 4;
    T* d = (T*)backend::device_malloc(bytes);
    backend::memset_sync(d, 0, bytes);
    return d;
}

void readback_f(std::vector<float>& acc, const float* d, int64_t n) {
    size_t off = acc.size();
    acc.resize(off + n);
    backend::memcpy_sync(acc.data() + off, d, n * sizeof(float),
                         MemcpyKind::DeviceToHost);
}

struct Rng {
    std::mt19937 rng;
    explicit Rng(uint32_t seed) : rng(seed) {}
    float uf(float lo, float hi) {
        return lo + (hi - lo) * (float)(rng() & 0xffffff) / 16777215.0f;
    }
    std::vector<float> vec(int64_t n, float lo, float hi) {
        std::vector<float> v(n);
        for (auto& x : v) x = uf(lo, hi);
        return v;
    }
};

TorchTensorView ttv(const void* p, std::vector<int64_t> shape) {
    return std::make_tuple((uint64_t)p, (uint32_t)4, std::move(shape));
}
TorchTensorView ttv_null() {
    return std::make_tuple((uint64_t)0, (uint32_t)4, std::vector<int64_t>{0});
}

// ---------------------------------------------------------------------------
// One multi-scale loss configuration.
// ---------------------------------------------------------------------------

struct MsCfg {
    const char* name;
    int64_t B, H, W;
    int num_scales;
    // GT resolutions (0 = same as render). Depth/normal bilinear, alpha
    // nearest.
    int64_t Hd, Wd, Hn, Wn, Ha, Wa;
    bool with_alpha;      // ref_alpha buffer + has_mask
    bool with_cams;       // camera_indices ({3,1}) with num_train_images=4
    bool minimal;         // rgb+Ts only (all other modalities null)
    int loss_map_mode;    // DensifyLossMapMode
    float quantile;
    float nms_falloff;    // *_nms modes only; 0 = the hard canny suppression
    float sat;            // blown-highlight cutoff; <= 0 disables it
};

void run_ms_cfg(Rng& r, const MsCfg& c) {
    const int64_t np = c.B * c.H * c.W;
    const int64_t Hd = c.Hd ? c.Hd : c.H, Wd = c.Wd ? c.Wd : c.W;
    const int64_t Hn = c.Hn ? c.Hn : c.H, Wn = c.Wn ? c.Wn : c.W;
    const int64_t Ha = c.Ha ? c.Ha : c.H, Wa = c.Wa ? c.Wa : c.W;
    const int64_t npd = c.B * Hd * Wd, npn = c.B * Hn * Wn, npa = c.B * Ha * Wa;

    auto rgb1 = r.vec(3 * np, 0.0f, 1.0f);
    auto rgb2 = r.vec(3 * np, 0.0f, 1.0f);
    if (c.sat > 0.0f) {
        // One contiguous blown-out block rather than scattered pixels, so the
        // window coverage divide is exercised across its edge too.
        for (int64_t b = 0; b < c.B; b++)
            for (int64_t y = c.H / 4; y < c.H / 2; y++)
                for (int64_t x = c.W / 4; x < c.W / 2; x++)
                    for (int k = 0; k < 3; k++) {
                        size_t i = (size_t)((b * c.H + y) * c.W + x) * 3 + k;
                        rgb1[i] = 0.985f + 0.015f * r.uf(0.0f, 1.0f);
                        rgb2[i] = 0.985f + 0.015f * r.uf(0.0f, 1.0f);
                    }
    }
    float* render_rgb = upload(rgb1);
    float* ref_rgb = upload(rgb2);
    float* render_Ts = upload(r.vec(np, 0.0f, 1.0f));

    float* render_depth = nullptr;
    float* ref_depth = nullptr;
    float* render_normal = nullptr;
    float* depth_normal = nullptr;
    float* ref_normal = nullptr;
    float* rgb_dist = nullptr;
    float* depth_dist = nullptr;
    float* normal_dist = nullptr;
    float* median_depth = nullptr;
    float* median_normal = nullptr;
    if (!c.minimal) {
        render_depth = upload(r.vec(np, 0.3f, 5.0f));
        {
            // ~15% zero sentinel ("no GT depth").
            auto v = r.vec(npd, 0.3f, 5.0f);
            for (auto& x : v)
                if (r.uf(0, 1) < 0.15f) x = 0.0f;
            ref_depth = upload(v);
        }
        render_normal = upload(r.vec(3 * np, -1.0f, 1.0f));
        depth_normal = upload(r.vec(3 * np, -1.0f, 1.0f));
        ref_normal = upload(r.vec(3 * npn, -1.0f, 1.0f));
        rgb_dist = upload(r.vec(3 * np, 0.0f, 0.1f));
        depth_dist = upload(r.vec(np, 0.0f, 0.1f));
        normal_dist = upload(r.vec(3 * np, 0.0f, 0.1f));
        {
            auto v = r.vec(np, 0.3f, 5.0f);
            for (auto& x : v)
                if (r.uf(0, 1) < 0.1f) x = 0.0f;
            median_depth = upload(v);
        }
        median_normal = upload(r.vec(3 * np, -1.0f, 1.0f));
    }

    uint8_t* ref_alpha = nullptr;
    if (c.with_alpha) {
        std::vector<uint8_t> m(npa);
        for (auto& x : m) x = r.uf(0, 1) < 0.8f ? 1 : 0;
        ref_alpha = upload(m);
    }

    int64_t* cams = nullptr;
    long num_train = c.B;
    if (c.with_cams) {
        std::vector<int64_t> ch = {3, 1};
        ch.resize((size_t)c.B, 3);
        cams = upload(ch);
        num_train = 4;
    }

    std::array<float, (int)LossWeightIndex::length> weights{};
    for (auto& w : weights) w = r.uf(0.1f, 1.0f);
    const float w_ssim = r.uf(0.1f, 0.5f);

    float* v_losses = upload(r.vec((int)LossIndex::length, -1.0f, 1.0f));
    float* loss_map_out = alloc_zero<float>(np);

    const int n_grads = c.minimal ? 1 : 13;
    std::vector<bool> needs(13, !c.minimal);
    needs[0] = true;

    PerPixelGrads grads = {};
    grads.v_render_rgb = ttv(alloc_zero<float>(3 * np), {c.B, c.H, c.W, 3});
    if (!c.minimal) {
        grads.v_ref_rgb = ttv(alloc_zero<float>(3 * np), {c.B, c.H, c.W, 3});
        grads.v_render_depth = ttv(alloc_zero<float>(np), {c.B, c.H, c.W, 1});
        grads.v_ref_depth = ttv(alloc_zero<float>(npd), {c.B, Hd, Wd, 1});
        grads.v_render_normal =
            ttv(alloc_zero<float>(3 * np), {c.B, c.H, c.W, 3});
        grads.v_depth_normal =
            ttv(alloc_zero<float>(3 * np), {c.B, c.H, c.W, 3});
        grads.v_ref_normal = ttv(alloc_zero<float>(3 * npn), {c.B, Hn, Wn, 3});
        grads.v_render_Ts = ttv(alloc_zero<float>(np), {c.B, c.H, c.W, 1});
        grads.v_rgb_dist = ttv(alloc_zero<float>(3 * np), {c.B, c.H, c.W, 3});
        grads.v_depth_dist = ttv(alloc_zero<float>(np), {c.B, c.H, c.W, 1});
        grads.v_normal_dist =
            ttv(alloc_zero<float>(3 * np), {c.B, c.H, c.W, 3});
        grads.v_median_depth = ttv(alloc_zero<float>(np), {c.B, c.H, c.W, 1});
        grads.v_median_normal =
            ttv(alloc_zero<float>(3 * np), {c.B, c.H, c.W, 3});
    }

    auto run_once = [&]() -> LossValues {
        return compute_multi_scale_per_pixel_losses(
            c.num_scales,
            ttv(render_rgb, {c.B, c.H, c.W, 3}),
            ttv(ref_rgb, {c.B, c.H, c.W, 3}),
            render_depth ? ttv(render_depth, {c.B, c.H, c.W, 1}) : ttv_null(),
            ref_depth ? ttv(ref_depth, {c.B, Hd, Wd, 1}) : ttv_null(),
            render_normal ? ttv(render_normal, {c.B, c.H, c.W, 3})
                          : ttv_null(),
            depth_normal ? ttv(depth_normal, {c.B, c.H, c.W, 3}) : ttv_null(),
            ref_normal ? ttv(ref_normal, {c.B, Hn, Wn, 3}) : ttv_null(),
            ttv(render_Ts, {c.B, c.H, c.W, 1}),
            rgb_dist ? ttv(rgb_dist, {c.B, c.H, c.W, 3}) : ttv_null(),
            depth_dist ? ttv(depth_dist, {c.B, c.H, c.W, 1}) : ttv_null(),
            normal_dist ? ttv(normal_dist, {c.B, c.H, c.W, 3}) : ttv_null(),
            median_depth ? ttv(median_depth, {c.B, c.H, c.W, 1}) : ttv_null(),
            median_normal ? ttv(median_normal, {c.B, c.H, c.W, 3})
                          : ttv_null(),
            ref_alpha
                ? std::make_tuple((uint64_t)ref_alpha, (uint32_t)1,
                                  std::vector<int64_t>{c.B, Ha, Wa, 1})
                : ttv_null(),
            /*has_mask=*/c.with_alpha, weights, w_ssim, c.sat,
            ttv(v_losses, {(int)LossIndex::length}), needs, num_train,
            cams ? ttv(cams, {c.B}) : ttv_null(),
            ttv(loss_map_out, {c.B, c.H, c.W, 1}), c.loss_map_mode,
            c.quantile, c.nms_falloff, grads);
    };

    run_once();  // primes the one-behind readouts
    LossValues lv = run_once();
    backend::device_synchronize();

    const bool gt_scaled = c.Hd != 0 || c.Hn != 0;

    readback_f(g_tight, (const float*)std::get<0>(grads.v_render_rgb), 3 * np);
    if (!c.minimal) {
        readback_f(g_tight, (const float*)std::get<0>(grads.v_ref_rgb), 3 * np);
        readback_f(g_tight, (const float*)std::get<0>(grads.v_render_depth), np);
        readback_f(gt_scaled ? g_loose : g_tight,
                   (const float*)std::get<0>(grads.v_ref_depth), npd);
        readback_f(g_tight, (const float*)std::get<0>(grads.v_render_normal), 3 * np);
        readback_f(g_tight, (const float*)std::get<0>(grads.v_depth_normal), 3 * np);
        readback_f(gt_scaled ? g_loose : g_tight,
                   (const float*)std::get<0>(grads.v_ref_normal), 3 * npn);
        readback_f(g_tight, (const float*)std::get<0>(grads.v_render_Ts), np);
        readback_f(g_tight, (const float*)std::get<0>(grads.v_rgb_dist), 3 * np);
        readback_f(g_tight, (const float*)std::get<0>(grads.v_depth_dist), np);
        readback_f(g_tight, (const float*)std::get<0>(grads.v_normal_dist), 3 * np);
        readback_f(g_tight, (const float*)std::get<0>(grads.v_median_depth), np);
        readback_f(g_tight, (const float*)std::get<0>(grads.v_median_normal), 3 * np);
    }
    if (c.loss_map_mode != (int)DensifyLossMapMode::None)
        readback_f(g_tight, loss_map_out, np);
    g_loose.push_back(lv.rgb_loss);
    g_loose.push_back(lv.rgb_psnr);
    g_loose.push_back(lv.depth_sup);
    g_loose.push_back(lv.normal_sup);
    g_loose.push_back(lv.alpha_sup);
    g_loose.push_back(lv.normal_reg);
    g_loose.push_back(lv.alpha_reg);
    g_loose.push_back(lv.rgb_dist_reg);
    g_loose.push_back(lv.depth_dist_reg);
    g_loose.push_back(lv.normal_dist_reg);
    g_loose.push_back(lv.mean_median_depth_sup);
    g_loose.push_back(lv.median_depth_normal_reg);
    g_loose.push_back(lv.median_normal_sup);
    g_loose.push_back(lv.median_render_normal_reg);
    g_loose.push_back(lv.ssim);

    (void)n_grads;
    std::printf("msloss_parity: cfg %-12s done\n", c.name);
}

// ---------------------------------------------------------------------------
// Direct quantile radix-select checks (bit-exact integer selection).
// ---------------------------------------------------------------------------

void run_quantile(Rng& r) {
    const int B = 5, N = 10000;
    std::vector<float> h = r.vec((int64_t)B * N, -2.0f, 2.0f);
    // Sprinkle non-finite / zero values the selector must ignore.
    for (size_t i = 0; i < h.size(); i += 97) h[i] = 0.0f;
    for (size_t i = 13; i < h.size(); i += 331)
        h[i] = std::numeric_limits<float>::infinity();
    for (size_t i = 51; i < h.size(); i += 613)
        h[i] = std::numeric_limits<float>::quiet_NaN();
    float* d_x = upload(h);
    uint32_t* temp = (uint32_t*)alloc_zero<uint32_t>((256 + 5) * B);

    for (float q : {0.5f, 0.9f}) {
        float* out_a = alloc_zero<float>(B);
        float* out_b = alloc_zero<float>(B);
        float* out_c = alloc_zero<float>(B);
        batch_quantile_masked_radix_select<false>(d_x, B, N, q, out_a, temp,
                                                  backend::kDefaultStream);
        batch_quantile_masked_radix_select<true>(d_x, B, N, q, out_b, temp,
                                                 backend::kDefaultStream);
        // Same rows, negatives dropped instead of folded in as |x|.
        batch_quantile_positive_radix_select<false>(d_x, B, N, q, out_c, temp,
                                                    backend::kDefaultStream);
        backend::device_synchronize();
        readback_f(g_tight, out_a, B);
        readback_f(g_tight, out_b, B);
        readback_f(g_tight, out_c, B);
    }
    std::printf("msloss_parity: cfg %-12s done\n", "quantile");
}

// ---------------------------------------------------------------------------
// Loss-map conditioning (median normalize + quantile clip), in place.
// ---------------------------------------------------------------------------

void run_normalize_clip(Rng& r) {
    const int64_t B = 3, H = 37, W = 53, N = H * W;
    std::vector<float> h = r.vec(B * N, 0.0f, 1.0f);
    // The population the quantiles are taken over excludes these. The tiny
    // negatives are the real map's flat-region background: folded in as |x|
    // they would put the median in the noise floor.
    for (size_t i = 0; i < h.size(); i += 3) h[i] = -h[i] * 1e-8f;
    for (size_t i = 0; i < h.size(); i += 11) h[i] = 0.0f;
    for (size_t i = 5; i < h.size(); i += 401)
        h[i] = std::numeric_limits<float>::infinity();
    for (size_t i = 7; i < h.size(); i += 509)
        h[i] = std::numeric_limits<float>::quiet_NaN();
    // Spikes: what the clip exists to flatten.
    for (size_t i = 3; i < h.size(); i += 257) h[i] = 1e4f;

    const struct { bool norm; float q; float p; } cases[] = {
        {true, 1.0f, 1.0f}, {false, 0.98f, 1.0f}, {true, 0.98f, 1.0f},
        {false, 1.0f, 1.0f}, {false, 1.0f, 0.5f}, {true, 0.98f, 2.0f},
    };
    for (const auto& c : cases) {
        float* d = upload(h);
        normalize_clip_map_inplace_tensor(ttv(d, {B, H, W, 1}), c.norm, c.q,
                                          c.p);
        backend::device_synchronize();
        readback_f(g_tight, d, B * N);
    }
    std::printf("msloss_parity: cfg %-12s done\n", "norm_clip");
}

#ifdef SS_BACKEND_VULKAN
static void strip_compare(const char* label, const std::vector<float>& got,
                          const std::vector<float>& ref, double abs_tol = 1e-6,
                          size_t row_width = 0) {
    if (got.size() != ref.size()) throw std::runtime_error("strip result shape mismatch");
    double max_abs = 0, error2 = 0, ref2 = 0;
    size_t max_index = 0;
    for (size_t i = 0; i < got.size(); ++i) {
        if (!std::isfinite(got[i]) || !std::isfinite(ref[i])) {
            std::fprintf(stderr, "strip %s nonfinite entry=%zu got=%.9g ref=%.9g\n",
                         label, i, double(got[i]), double(ref[i]));
            throw std::runtime_error(std::string(label) + ": non-finite result");
        }
        const double error = double(got[i]) - ref[i];
        if (std::abs(error) > max_abs) {
            max_abs = std::abs(error);
            max_index = i;
        }
        error2 += error * error;
        ref2 += double(ref[i]) * ref[i];
    }
    const double rel_rms = ref2 ? std::sqrt(error2 / ref2) : 0;
    std::printf("strip %-22s max_abs=%.9g rel_rms=%.9g\n", label, max_abs, rel_rms);
    if ((!ref2 && error2) || max_abs > abs_tol || rel_rms > 1e-3) {
        std::fprintf(stderr,
            "strip %s worst_entry=%zu got=%.9g ref=%.9g delta=%.9g "
            "max_abs_limit=%.9g rel_rms=%.9g ref_norm=%.9g\n",
            label, max_index, double(got[max_index]), double(ref[max_index]),
            double(got[max_index]) - ref[max_index], abs_tol, rel_rms, std::sqrt(ref2));
        if (row_width)
            std::fprintf(stderr, "strip %s plane=%zu coordinate=%zu\n",
                         label, max_index / row_width, max_index % row_width);
        throw std::runtime_error(std::string(label) + ": numerical mismatch");
    }
}

static void run_strip_pixel_checks() {
    using namespace backend::vk;
    constexpr int64_t B = 2, H = 67, W = 43, np = B * H * W;
    Rng rng(171027u);
    std::vector<void*> allocations;
    auto own = [&](auto* pointer) {
        allocations.push_back(pointer);
        return pointer;
    };
    float* rgb = own(upload(rng.vec(3 * np, -0.2f, 2.0f)));
    float* Ts = own(upload(rng.vec(np, 0.05f, 0.95f)));
    float* blended = own(alloc_zero<float>(3 * np));
    float* encoded = own(alloc_zero<float>(3 * np));
    float* v_rgb = own(alloc_zero<float>(3 * np));
    float* v_Ts = own(alloc_zero<float>(np));
    float* matrix = own(upload(std::vector<float>{
        0.9f, 0.08f, 0.02f, 0.05f, 0.9f, 0.05f, 0.02f, 0.08f, 0.9f}));
    const auto seed = rng.vec(3 * np, -0.3f, 0.3f);
    auto t3 = [&](float* p) { return DeviceTensor3D<float3>(ttv(p, {B, H, W, 3})); };
    auto t1 = [&](float* p) { return DeviceTensor3D<float>(ttv(p, {B, H, W, 1})); };
    const auto cm = DeviceTensor2D<float3>(ttv(matrix, {3, 1, 3}));
    struct Result {
        std::vector<float> image, gradient, Ts;
        ImageStripRows rows;
    };
    for (int transfer : {0, 2}) for (bool linear : {false, true}) {
        auto run = [&](ImageStripContext strip) {
            set_image_strip(std::move(strip));
            backend::memcpy_sync(v_rgb, seed.data(), seed.size() * sizeof(float),
                                 MemcpyKind::HostToDevice);
            backend::memset_sync(blended, 0x3f, 3 * np * sizeof(float));
            backend::memset_sync(encoded, 0x3f, 3 * np * sizeof(float));
            backend::memset_sync(v_Ts, 0x3f, np * sizeof(float));
            const float3 background = make_float3(0.15f, 0.2f, 0.1f);
            blend_background_color_forward(t3(rgb), t1(Ts), background, t3(blended));
            working_to_display_forward(transfer, linear, t3(blended), cm, t3(encoded));
            working_to_display_backward(transfer, linear, t3(blended), cm, t3(v_rgb), t3(v_rgb));
            overexposure_grad_add(t3(blended), 0.17f, t3(v_rgb));
            blend_background_color_backward(t3(rgb), t1(Ts), background, 0.13f,
                                            t3(v_rgb), t3(v_rgb), t1(v_Ts));
            Result result;
            result.rows = image_strip_rows(uint32_t(H));
            readback_f(result.image, encoded, 3 * np);
            readback_f(result.gradient, v_rgb, 3 * np);
            readback_f(result.Ts, v_Ts, np);
            return result;
        };
        const Result native = run({});
        for (uint32_t cut : {0u, 32u, uint32_t(H)}) {
            const Result a = run({uint32_t(H), 0, cut, 10, {}});
            const Result b = run({uint32_t(H), cut, uint32_t(H), 10, {}});
            std::vector<float> gradient(3 * np), grad_Ts(np);
            for (size_t i = 0; i < gradient.size(); ++i)
                gradient[i] = a.gradient[i] + b.gradient[i];
            for (size_t i = 0; i < grad_Ts.size(); ++i)
                grad_Ts[i] = a.Ts[i] + b.Ts[i];
            for (int64_t batch = 0; batch < B; ++batch)
                for (uint32_t y = 0; y < uint32_t(H); ++y)
                    for (uint32_t x = 0; x < uint32_t(W); ++x) {
                        const size_t i = (size_t(batch) * H + y) * W + x;
                        const Result& owner = y < cut ? a : b;
                        const Result& other = y < cut ? b : a;
                        if (other.Ts[i] != 0)
                            throw std::runtime_error("strip pixelwise transmittance halo is not zero");
                        for (int k = 0; k < 3; ++k) {
                            const size_t j = 3 * i + k;
                            if (std::memcmp(&owner.image[j], &native.image[j], sizeof(float)) ||
                                !std::isfinite(owner.image[j]) || other.gradient[j] != 0)
                                throw std::runtime_error("strip pointwise RGB/ownership mismatch");
                            for (const Result* result : {&a, &b})
                                if ((y < result->rows.work_begin || y >= result->rows.work_end) &&
                                    result->image[j] != 0)
                                    throw std::runtime_error("strip pointwise forward halo is not zero");
                        }
                    }
            strip_compare("pixelwise_rgb", gradient, native.gradient);
            strip_compare("pixelwise_Ts", grad_Ts, native.Ts);
        }
    }
    set_image_strip({});
    for (void* pointer : allocations) backend::device_free(pointer);
}

static int run_strip_checks() {
    using namespace backend::vk;
    auto require = [](bool valid, const char* message) {
        if (!valid) throw std::runtime_error(message);
    };
    auto reject = [&](auto operation) {
        bool rejected = false;
        try { operation(); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "image strip accepted invalid rows");
        set_image_strip({});
    };
    reject([] { set_image_strip({65, 40, 20, 10, {}}); });
    reject([] { set_image_strip({65, 0, 66, 10, {}}); });
    reject([] { set_image_strip({65, 0, 32, 10, {}}); image_strip_rows(33); });
    reject([] { set_image_strip({65, 0, 31, 10, {}}); image_strip_rows(32); });
    reject([] { image_strip_halo(0); });
    reject([] { image_strip_halo(5); });
    require(image_strip_halo(1) == 10 && image_strip_halo(4) == 87 &&
            image_strip_halo(4, true) == 89, "image strip halo support changed");
    run_strip_pixel_checks();
    std::puts("strip collective=capture_actual_A_then_B_sum_then_replay; full-owned reference checked separately");

    struct Case {
        const char* name;
        uint32_t H, W, cut, scales;
        int mask;  // 0 none, 1 mixed, 2 all masked, 3 separate resolution
        DensifyLossMapMode map_mode;
        bool identical_rgb = false;
        int B = 2;
    };
    const Case cases[] = {
        {"even_boundary", 256, 80, 128, 3, 0, DensifyLossMapMode::SsimFull},
        {"odd_four", 257, 79, 128, 4, 1, DensifyLossMapMode::SsimContrastStruct},
        {"scaled_mask", 193, 67, 64, 2, 3, DensifyLossMapMode::SsimStructure},
        {"all_masked", 65, 31, 32, 3, 2, DensifyLossMapMode::LossFull},
        {"empty_top", 129, 35, 0, 3, 1, DensifyLossMapMode::LossFull},
        {"empty_bottom", 129, 35, 129, 2, 0, DensifyLossMapMode::SsimFull},
        {"odd_nms", 67, 49, 32, 1, 1, DensifyLossMapMode::LossFullNms},
        {"tiny_pyramid", 3, 5, 0, 4, 1, DensifyLossMapMode::SsimFull},
        {"scalar_reduction", 2880, 2880, 1472, 2, 0,
         DensifyLossMapMode::SsimFull, true, 1},
    };
    Rng rng(171026u);
    for (const Case& c : cases) {
        const int B = c.B;
        const int64_t np = int64_t(B) * c.H * c.W;
        const uint32_t mask_h = c.mask == 3 ? 97 : c.H;
        const uint32_t mask_w = c.mask == 3 ? 35 : c.W;
        std::vector<void*> allocations;
        auto own = [&](auto* pointer) {
            allocations.push_back(pointer);
            return pointer;
        };
        auto input_rgb = rng.vec(3 * np, 0.05f, 0.95f);
        auto ref = c.identical_rgb ? input_rgb : rng.vec(3 * np, 0.05f, 0.95f);
        const auto depth = c.identical_rgb ? std::vector<float>(size_t(np), 1.0f) :
                                            rng.vec(np, 0.3f, 3.0f);
        const auto transmittance = rng.vec(np, 0.05f, 0.95f);
        for (int b = 0; b < B; ++b)
            for (uint32_t y = c.cut > 5 ? c.cut - 5 : 0;
                 y < std::min(c.H, c.cut + 5); ++y)
                for (uint32_t x = 5; x < std::min(c.W, 17u); ++x)
                    for (int k = 0; k < 3; ++k) {
                        const size_t i = ((size_t(b) * c.H + y) * c.W + x) * 3 + k;
                        input_rgb[i] = 0.995f;
                        ref[i] = 0.995f;
                    }
        // A nonzero MSE keeps the canonical PSNR VJP finite.
        if (c.identical_rgb)
            for (float& value : input_rgb) value += 1e-5f;
        std::vector<uint8_t> mask(size_t(B) * mask_h * mask_w);
        for (size_t i = 0; i < mask.size(); ++i)
            mask[i] = c.mask != 2 && (i % 11 < 7 || i % 23 == 0) ? 1 : 0;
        float* d_rgb = own(upload(input_rgb));
        float* d_ref = own(upload(ref));
        float* d_depth = own(upload(depth));
        float* d_Ts = own(upload(transmittance));
        uint8_t* d_mask = c.mask ? own(upload(mask)) : nullptr;
        int64_t* d_cameras = own(upload(std::vector<int64_t>(size_t(B), 3)));
        std::array<float, (int)LossWeightIndex::length> weights{};
        weights[(int)LossWeightIndex::RgbSupL1] = 0.7f;
        weights[(int)LossWeightIndex::RgbSupL2] = 0.3f;
        weights[(int)LossWeightIndex::YSupL2] = 0.2f;
        weights[(int)LossWeightIndex::AlphaSup] = 0.15f;
        weights[(int)LossWeightIndex::AlphaSupUnder] = 0.25f;
        weights[(int)LossWeightIndex::AlphaReg] = c.identical_rgb ? 0.0f : 0.1f;
        std::vector<float> v_loss((int)LossIndex::length, 1.0f);
        v_loss[(int)LossIndex::RgbPSNR] = 0;
        float* d_v_loss = own(upload(v_loss));
        float* d_v_rgb = own(alloc_zero<float>(3 * np));
        float* d_v_depth = own(alloc_zero<float>(np));
        float* d_v_Ts = own(alloc_zero<float>(np));
        float* d_map = own(alloc_zero<float>(np));
        PerPixelGrads grads{};
        grads.v_render_rgb = ttv(d_v_rgb, {B, c.H, c.W, 3});
        grads.v_render_depth = ttv(d_v_depth, {B, c.H, c.W, 1});
        grads.v_render_Ts = ttv(d_v_Ts, {B, c.H, c.W, 1});
        std::vector<bool> needs(13, false);
        needs[0] = needs[2] = needs[7] = true;
        struct Result {
            std::vector<float> rgb, depth, Ts, map, losses, raw;
        };
        using Phases = std::vector<std::vector<float>>;
        auto run = [&](ImageStripContext strip) {
            set_image_strip(std::move(strip));
            std::vector<float> source;
            const float* source_data = input_rgb.data();
            if (image_strip().full_height) {
                source = input_rgb;
                source_data = source.data();
                const auto rows = image_strip_rows(c.H);
                for (int b = 0; b < B; ++b)
                    for (uint32_t y = 0; y < c.H; ++y)
                        if (y < rows.work_begin || y >= rows.work_end)
                            std::fill_n(source.begin() +
                                (size_t(b) * c.H + y) * c.W * 3, c.W * 3,
                                std::numeric_limits<float>::quiet_NaN());
            }
            backend::memcpy_sync(d_rgb, source_data, input_rgb.size() * sizeof(float),
                                 MemcpyKind::HostToDevice);
            for (auto buffer : {std::pair<float*, int64_t>{d_v_rgb, 3 * np},
                                {d_v_depth, np}, {d_v_Ts, np}, {d_map, np}})
                backend::memset_sync(buffer.first, 0x3f,
                                     size_t(buffer.second) * sizeof(float));
            compute_multi_scale_per_pixel_losses(
                int(c.scales), ttv(d_rgb, {B, c.H, c.W, 3}),
                ttv(d_ref, {B, c.H, c.W, 3}),
                ttv(d_depth, {B, c.H, c.W, 1}), ttv_null(), ttv_null(),
                ttv_null(), ttv_null(), ttv(d_Ts, {B, c.H, c.W, 1}),
                ttv_null(), ttv_null(), ttv_null(), ttv_null(), ttv_null(),
                d_mask ? std::make_tuple(uint64_t(d_mask), uint32_t(1),
                    std::vector<int64_t>{B, mask_h, mask_w, 1}) : ttv_null(),
                c.mask != 0, weights, 0.2f, 0.98f,
                ttv(d_v_loss, {(int)LossIndex::length}), needs, 4,
                ttv(d_cameras, {B}), ttv(d_map, {B, c.H, c.W, 1}),
                int(c.map_mode), 0.9f, 0.5f, grads);
            Result result;
            readback_f(result.rgb, d_v_rgb, 3 * np);
            readback_f(result.depth, d_v_depth, np);
            readback_f(result.Ts, d_v_Ts, np);
            readback_f(result.map, d_map, np);
            readback_f(result.losses, DevicePool::global().acquire<float>(
                PoolSlot::PplTotalLosses, (int)LossIndex::length),
                (int)LossIndex::length);
            readback_f(result.losses, DevicePool::global().acquire<float>(
                PoolSlot::SsimScalar, 1), 1);
            readback_f(result.raw, DevicePool::global().acquire<float>(
                PoolSlot::PplRawLosses, 5 * (int)RawLossIndex::length),
                5 * (int)RawLossIndex::length);
            return result;
        };
        std::printf("strip case=%s H=%u W=%u cut=%u scales=%u\n",
                    c.name, c.H, c.W, c.cut, c.scales);
        const Result native = run({});
        const Result repeat = run({});
        strip_compare("native_repeat_rgb", repeat.rgb, native.rgb);
        Phases full;
        auto capture = [&](Phases& phases, const Phases* add, Phases* local = nullptr) {
            return [&, add, local](float* device, size_t count) {
                phases.emplace_back(count);
                backend::memcpy_sync(phases.back().data(), device, count * sizeof(float),
                                     MemcpyKind::DeviceToHost);
                if (local) local->push_back(phases.back());
                if (add) {
                    const size_t phase = phases.size() - 1;
                    require(phase < add->size() && (*add)[phase].size() == count,
                            "strip collective phase mismatch");
                    for (size_t i = 0; i < count; ++i)
                        phases.back()[i] += (*add)[phase][i];
                    backend::memcpy_sync(device, phases.back().data(), count * sizeof(float),
                                         MemcpyKind::HostToDevice);
                }
            };
        };
        const uint32_t halo = image_strip_halo(c.scales);
        const Result active_full = run({c.H, 0, c.H, halo, capture(full, nullptr)});
        strip_compare("active_full_rgb", active_full.rgb, native.rgb);
        Phases partial_a, partial_b, global;
        run({c.H, 0, c.cut, halo, capture(partial_a, nullptr)});
        run({c.H, c.cut, c.H, halo, capture(global, &partial_a, &partial_b)});
        require(global.size() == c.scales + 1 && full.size() == global.size() &&
                partial_a.size() == global.size() && partial_b.size() == global.size(),
                "strip collective phase count mismatch");
        uint32_t phase_height = c.H;
        for (size_t phase = 0; phase < global.size(); ++phase) {
            char label[80];
            std::snprintf(label, sizeof(label), "statistics.%s.scale%zu.phase%zu",
                          phase == 1 ? "ssim" : "raw_rows", phase < 2 ? 0 : phase - 1, phase);
            strip_compare(label, global[phase], full[phase], 0.03,
                          phase == 1 ? 0 : size_t(phase_height));
            if (phase != 1) phase_height = std::max(1u, phase_height / 2);
        }
        uint32_t height = c.H, width = c.W;
        std::vector<float> expected_raw(5 * (int)RawLossIndex::length, 0.0f);
        for (uint32_t scale = 0; scale < c.scales; ++scale) {
            const auto& sum = global[scale == 0 ? 0 : scale + 1];
            const auto& whole = full[scale == 0 ? 0 : scale + 1];
            const auto& a_rows = partial_a[scale == 0 ? 0 : scale + 1];
            const auto& b_rows = partial_b[scale == 0 ? 0 : scale + 1];
            const uint32_t owned_cut = image_strip_rows(height).owned_begin;
            require(sum.size() == size_t(B) * (int)RawLossIndex::length * height,
                    "strip raw row-statistic extent mismatch");
            for (int batch = 0; batch < B; ++batch)
                for (uint32_t y = 0; y < height; ++y) {
                    const size_t base = size_t(batch) * (int)RawLossIndex::length * height + y;
                    require(sum[base + (int)RawLossIndex::PixelsTotal * height] == float(width),
                            "strip pixel population is not the full logical row");
                    for (int index = (int)RawLossIndex::PixelsTotal;
                         index < (int)RawLossIndex::length; ++index)
                        require(sum[base + index * height] == whole[base + index * height],
                                "strip mask/alpha row population mismatch");
                    for (int index = 0; index < (int)RawLossIndex::length; ++index) {
                        const size_t element = base + index * height;
                        require((y < owned_cut ? b_rows[element] : a_rows[element]) == 0,
                                "strip raw statistic is not zero outside owned rows");
                    }
                }
            if (scale + 1 == c.scales) {
                for (int index = 0; index < (int)RawLossIndex::length; ++index) {
                    double total = 0;
                    for (int batch = 0; batch < B; ++batch)
                        for (uint32_t y = 0; y < height; ++y)
                            total += sum[(size_t(batch) * (int)RawLossIndex::length + index) *
                                         height + y];
                    const bool per_camera = index >= (int)RawLossIndex::DepthSupX &&
                                            index <= (int)RawLossIndex::DepthSupXY;
                    if (!per_camera) expected_raw[index] = float(total);
                    if (per_camera || index == (int)RawLossIndex::DepthMaskTotal)
                        expected_raw[4 * (int)RawLossIndex::length + index] = float(total);
                }
            }
            height = std::max(1u, height / 2);
            width = std::max(1u, width / 2);
        }
        strip_compare("routed_native_raw", native.raw, expected_raw, 0.03);
        size_t phase = 0;
        auto replay = [&](float* device, size_t count) {
            require(phase < global.size() && global[phase].size() == count,
                    "strip collective replay mismatch");
            backend::memcpy_sync(device, global[phase].data(), count * sizeof(float),
                                 MemcpyKind::HostToDevice);
            ++phase;
        };
        const Result a = run({c.H, 0, c.cut, halo, replay});
        require(phase == global.size(), "strip A missed collective");
        phase = 0;
        const Result b = run({c.H, c.cut, c.H, halo, replay});
        require(phase == global.size(), "strip B missed collective");
        auto combine = [](const std::vector<float>& a, const std::vector<float>& b) {
            std::vector<float> sum(a.size());
            for (size_t i = 0; i < sum.size(); ++i) sum[i] = a[i] + b[i];
            return sum;
        };
        auto check_ownership = [&](const Result& result, uint32_t begin, uint32_t end) {
            for (int batch = 0; batch < B; ++batch)
                for (uint32_t y = 0; y < c.H; ++y)
                    if (y < begin || y >= end)
                        for (uint32_t x = 0; x < c.W; ++x) {
                            const size_t i = (size_t(batch) * c.H + y) * c.W + x;
                            require(result.depth[i] == 0 && result.Ts[i] == 0 &&
                                    result.map[i] == 0, "strip scalar halo output is not zero");
                            for (int k = 0; k < 3; ++k)
                                require(result.rgb[3 * i + k] == 0,
                                        "strip RGB halo output is not zero");
                        }
        };
        check_ownership(a, 0, c.cut);
        check_ownership(b, c.cut, c.H);
        strip_compare("combined_rgb", combine(a.rgb, b.rgb), native.rgb);
        strip_compare("combined_depth", combine(a.depth, b.depth), native.depth);
        strip_compare("combined_Ts", combine(a.Ts, b.Ts), native.Ts);
        strip_compare("combined_map", combine(a.map, b.map), native.map);
        strip_compare("global_losses_A", a.losses, native.losses, 2e-5);
        strip_compare("global_losses_B", b.losses, native.losses, 2e-5);
        strip_compare("routed_raw_A", a.raw, expected_raw, 0.03);
        strip_compare("routed_raw_B", b.raw, expected_raw, 0.03);
        strip_compare("global_ssim_A", {a.losses.back()}, {native.losses.back()});
        strip_compare("global_ssim_B", {b.losses.back()}, {native.losses.back()});
        if (c.identical_rgb)
            strip_compare("identity_ssim", {native.losses.back()}, {1.0f});
        set_image_strip({});
        for (void* pointer : allocations) backend::device_free(pointer);
    }
    std::puts("msloss_parity: STRIP PASSED");
    return 0;
}
#endif

int main(int argc, char** argv) {
#ifdef SS_BACKEND_VULKAN
    if (argc == 2 && std::strcmp(argv[1], "strip") == 0) {
        try { return run_strip_checks(); }
        catch (const std::exception& error) {
            backend::vk::set_image_strip({});
            std::fprintf(stderr, "msloss_parity: STRIP FAILED: %s\n", error.what());
            return 1;
        }
    }
#endif
    if (argc != 3 ||
        (std::strcmp(argv[1], "dump") && std::strcmp(argv[1], "compare"))) {
        std::fprintf(stderr, "usage: %s dump|compare <ref.bin> | strip\n", argv[0]);
        return 2;
    }
    const bool dumping = std::strcmp(argv[1], "dump") == 0;

    Rng r(260720u);

    const MsCfg cfgs[] = {
        // Full modality set, equal-shape GT, mask, 3 scales, LossFull map.
        {"full", 2, 64, 80, 3, 0, 0, 0, 0, 0, 0, true, false, false,
         (int)DensifyLossMapMode::LossFull, 0.9f, 0.5f, -1.0f},
        // Scaled GT modalities (bilinear paths), camera indices, no mask,
        // 2 scales, full-SSIM map; checks the SSIM scalar.
        {"scaled_gt", 2, 40, 48, 2, 80, 96, 30, 36, 0, 0, false, true, false,
         (int)DensifyLossMapMode::SsimFull, 0.9f, 0.5f, -1.0f},
        // Minimal modalities, robust edge-aware map (residual + quantile +
        // tukey + canny), equal-res mask (canny indexes the mask at render
        // resolution), single scale.
        {"robust_edge", 1, 48, 64, 1, 0, 0, 0, 0, 0, 0, true, false, true,
         (int)DensifyLossMapMode::RobustEdgeAware, 0.85f, 0.5f, -1.0f},
        // Plain edge-aware map (canny of GT rgb), 2 scales.
        {"edge_aware", 1, 32, 32, 2, 0, 0, 0, 0, 0, 0, true, false, true,
         (int)DensifyLossMapMode::EdgeAware, 0.9f, 0.5f, -1.0f},
        // SSIM contrast*structure and structure-only map variants; the
        // latter with a smaller-resolution mask (SSIM's clamped mask path).
        {"ssim_cs", 1, 32, 32, 1, 0, 0, 0, 0, 0, 0, false, false, true,
         (int)DensifyLossMapMode::SsimContrastStruct, 0.9f, 0.5f, -1.0f},
        {"ssim_str", 1, 32, 32, 1, 0, 0, 0, 0, 16, 16, true, false, true,
         (int)DensifyLossMapMode::SsimStructure, 0.9f, 0.5f, -1.0f},
        // The NMS variants: same maps, thinned to their ridges. Multi-scale
        // on the loss_full one, since NMS runs per scale.
        {"loss_nms", 1, 40, 48, 2, 0, 0, 0, 0, 0, 0, true, false, false,
         (int)DensifyLossMapMode::LossFullNms, 0.9f, 0.5f, -1.0f},
        {"ssim_f_nms", 1, 32, 32, 1, 0, 0, 0, 0, 0, 0, false, false, true,
         (int)DensifyLossMapMode::SsimFullNms, 0.9f, 0.0f, -1.0f},
        {"ssim_cs_nms", 1, 32, 32, 1, 0, 0, 0, 0, 0, 0, false, false, true,
         (int)DensifyLossMapMode::SsimContrastStructNms, 0.9f, 0.25f, -1.0f},
        {"ssim_str_nms", 1, 32, 32, 1, 0, 0, 0, 0, 16, 16, true, false, true,
         (int)DensifyLossMapMode::SsimStructureNms, 0.9f, 1.0f, -1.0f},
        // Blown-highlight cutoff: the drop-and-renormalize path the mask takes,
        // once with a mask over it and once alone (the no-mask coverage divide).
        // Dims are multiples of both SSIM tile edges at every scale.
        {"sat_mask", 2, 96, 96, 2, 0, 0, 0, 0, 0, 0, true, false, false,
         (int)DensifyLossMapMode::LossFull, 0.9f, 0.5f, 0.98f},
        {"sat_only", 1, 48, 48, 1, 0, 0, 0, 0, 0, 0, false, false, false,
         (int)DensifyLossMapMode::SsimFull, 0.9f, 0.5f, 0.98f},
    };
    for (const MsCfg& c : cfgs) run_ms_cfg(r, c);
    run_quantile(r);
    run_normalize_clip(r);

    auto write_all = [&](const char* path) {
        std::ofstream f(path, std::ios::binary);
        int64_t nf = (int64_t)g_tight.size(), nl = (int64_t)g_loose.size();
        f.write((const char*)&nf, 8);
        f.write((const char*)g_tight.data(), nf * 4);
        f.write((const char*)&nl, 8);
        f.write((const char*)g_loose.data(), nl * 4);
    };

    if (dumping) {
        write_all(argv[2]);
        std::printf("msloss_parity: dumped %zu + %zu floats to %s\n",
                    g_tight.size(), g_loose.size(), argv[2]);
        return 0;
    }

    if (const char* dump_got = std::getenv("MSLOSS_DUMP_GOT"))
        write_all(dump_got);

    std::ifstream f(argv[2], std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", argv[2]);
        return 2;
    }
    int64_t nf = 0, nl = 0;
    f.read((char*)&nf, 8);
    if (nf != (int64_t)g_tight.size()) {
        std::fprintf(stderr, "tight count mismatch: ref %lld vs got %zu\n",
                     (long long)nf, g_tight.size());
        return 1;
    }
    std::vector<float> ref(nf);
    f.read((char*)ref.data(), nf * 4);
    f.read((char*)&nl, 8);
    if (nl != (int64_t)g_loose.size()) {
        std::fprintf(stderr, "loose count mismatch: ref %lld vs got %zu\n",
                     (long long)nl, g_loose.size());
        return 1;
    }
    std::vector<float> lref(nl);
    f.read((char*)lref.data(), nl * 4);

    // Paired with a relative-RMS gate: the NMS / quantile / clip modes make
    // discrete per-pixel choices that flip where an architecture's rounding
    // differs, so a few large outliers are expected (docs/testing.md).
    auto cmp_f = [](const std::vector<float>& got,
                    const std::vector<float>& want, int64_t& viol,
                    double& max_abs, double& rel_rms) {
        viol = 0;
        max_abs = 0;
        double se = 0, sr = 0;
        for (size_t i = 0; i < got.size(); i++) {
            bool gfin = std::isfinite(got[i]), wfin = std::isfinite(want[i]);
            if (!gfin || !wfin) {
                if (gfin != wfin) viol++;
                continue;
            }
            double d = std::fabs((double)got[i] - (double)want[i]);
            double tol = 5e-3 + 1e-3 * std::fabs((double)want[i]);
            max_abs = std::max(max_abs, d);
            if (d > tol) viol++;
            se += d * d;
            sr += (double)want[i] * (double)want[i];
        }
        rel_rms = sr > 0 ? std::sqrt(se / sr) : 0.0;
    };
    int64_t fviol = 0, lviol = 0;
    double fmax = 0, lmax = 0, frms = 0, lrms = 0;
    cmp_f(g_tight, ref, fviol, fmax, frms);
    cmp_f(g_loose, lref, lviol, lmax, lrms);
    double ffrac = nf ? (double)fviol / (double)nf : 0.0;
    double lfrac = nl ? (double)lviol / (double)nl : 0.0;
    std::printf(
        "msloss_parity: %lld tight floats (max_abs %.3g, violations %lld = "
        "%.5f%%, rel_rms %.3g), %lld loose floats (max_abs %.3g, violations "
        "%lld = %.5f%%, rel_rms %.3g)\n",
        (long long)nf, fmax, (long long)fviol, 100.0 * ffrac, frms,
        (long long)nl, lmax, (long long)lviol, 100.0 * lfrac, lrms);
    bool pass = ffrac <= 5e-3 && frms <= 5e-3 &&
                lfrac <= 2e-2 && lrms <= 5e-2;
    std::printf(pass ? "msloss_parity: PASSED\n" : "msloss_parity: FAILED\n");
    return pass ? 0 : 1;
}
