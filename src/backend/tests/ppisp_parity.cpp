// Backend parity tool for the PPISP image-transform + regularization launch
// APIs, across every param type and cam_indices on/off. One source, both
// backends: `./ppisp_parity dump ref.bin`, then `compare ref.bin` per device.
// Ref format: [nf tight floats] [nl loose floats].
//
// Both backends run the same shaders/ppisp.slang math, so per-pixel outputs
// and per-image gradients differ only by fast-math rounding -> tight channel;
// atomically accumulated buffers -> loose. The regularization BACKWARD is fed
// synthetic host-side raw_losses/v_losses so it stays deterministic (tight).

#include <kernels/pixelwise/PixelWise.cuh>
#include <engine/EngineInternal.h>
#include <core/Tensor.h>
#include <core/Env.h>

#if defined(SS_BACKEND_VULKAN)
#include "backend/vulkan/kernels/PpispBackward.h"
#include "backend/vulkan/kernels/ImageStrip.h"
#include "kernels/optim/Optimizer.cuh"
#include "kernels/bilagrid/BilagridBindings.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <vector>
#include <stdexcept>
#include <type_traits>

using backend::MemcpyKind;

static std::vector<float> g_tight, g_loose;

template <typename T>
T* upload(const std::vector<T>& host) {
    T* d = (T*)backend::device_malloc(
        ((host.size() * sizeof(T) + 3) / 4) * 4);
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
    return std::make_tuple((uint64_t)0, (uint32_t)4,
                           std::vector<int64_t>{0});
}

DeviceTensor3D<float3> dt3(const void* p, int64_t b, int64_t h, int64_t w) {
    return DeviceTensor3D<float3>(ttv(p, {b, h, w, 3}));
}

struct TypeInfo {
    const char* name;
    int n_params;
    int n_raw;
};


#if defined(SS_BACKEND_VULKAN)
static void nullable_rgb_parity(const TypeInfo* types) {
    Rng r(261003u); // Independent: do not change historical reference streams.
    constexpr int B = 3, H = 22, W = 26;
    for (int t = 0; t < 6; ++t) for (bool indexed : {false, true}) {
        const int N = indexed ? 5 : B, P = types[t].n_params;
        auto image = r.vec(B * H * W * 3, -0.2f, 3.f);
        auto params = r.vec(N * P, -0.2f, 0.2f);
        auto cot = r.vec(image.size(), -0.3f, 0.3f);
        for (size_t i = 0; i < cot.size(); ++i) {
            if ((i / 3) % 5 == 0 || i % 7 == 0) cot[i] = 0.f;
            if (i % 11 == 0) image[i] = 1e-8f;
        }
        std::vector<float> intr = {80,80,40,35,80,80,50,42,80,80,60,50};
        std::vector<int> indices = {4,1,4};
        std::vector<void*> allocations;
        auto keep = [&](auto* p) { allocations.push_back(p); return p; };
        try {
            auto* im = keep(upload(image)); auto* pa = keep(upload(params));
            auto* in = keep(upload(intr)); auto* ca = indexed ? keep(upload(indices)) : nullptr;
            auto* joint_cot = keep(upload(cot)); auto* rgb = keep(upload(cot));
            auto* joint = keep(alloc_zero<float>(image.size()));
            auto* grad = keep(alloc_zero<float>(params.size()));
            auto cam = indexed ? ttv(ca, {B}) : ttv_null();
            ppisp_backward(dt3(im,B,H,W),ttv(pa,{N,P}),ttv(in,{B,4}),104,88,
                dt3(joint_cot,B,H,W),types[t].name,cam,dt3(joint,B,H,W),ttv(grad,{N,P}));
            ppisp_backward(dt3(im,B,H,W),ttv(pa,{N,P}),ttv(in,{B,4}),104,88,
                dt3(rgb,B,H,W),types[t].name,cam,dt3(rgb,B,H,W),ttv_null());
            std::vector<float> a,b;
            readback_f(a,joint,image.size()); readback_f(b,rgb,image.size());
            if (std::memcmp(a.data(),b.data(),a.size()*4))
                throw std::runtime_error("nullable PPISP RGB exact parity failed");
            auto unchanged = [&](const auto& host, const auto* device) {
                using T = typename std::decay_t<decltype(host)>::value_type;
                std::vector<T> got(host.size());
                backend::memcpy_sync(got.data(),device,got.size()*sizeof(T),MemcpyKind::DeviceToHost);
                if (std::memcmp(got.data(),host.data(),got.size()*sizeof(T)))
                    throw std::runtime_error("nullable PPISP immutable input changed");
            };
            unchanged(image,im); unchanged(params,pa); unchanged(intr,in);
            unchanged(cot,joint_cot); if (indexed) unchanged(indices,ca);
            backend::device_synchronize();
            if (const char* error = backend::last_error()) throw std::runtime_error(error);
        } catch (...) { for (auto* p : allocations) backend::device_free(p); throw; }
        for (auto* p : allocations) backend::device_free(p);
    }
}

static double strip_difference(const std::vector<float>& reference,
                               const std::vector<float>& actual,
                               const char* label) {
    double max_abs = 0, error2 = 0, reference2 = 0;
    for (size_t i = 0; i < reference.size(); ++i) {
        if (!std::isfinite(reference[i]) || !std::isfinite(actual[i]))
            throw std::runtime_error(std::string(label) + ": non-finite value");
        const double error = double(actual[i]) - reference[i];
        max_abs = std::max(max_abs, std::abs(error));
        error2 += error * error;
        reference2 += double(reference[i]) * reference[i];
    }
    if (reference2 == 0 && error2 != 0)
        throw std::runtime_error(std::string(label) + ": expected exact zero array");
    const double relative_l2 = std::sqrt(error2 / std::max(reference2, 1e-30));
    std::printf("  %s maxabs=%.9g relRMS=%.9g\n", label, max_abs,relative_l2);
    if (max_abs > 1e-6 || relative_l2 > 1e-3)
        throw std::runtime_error(std::string(label) + ": gradient mismatch");
    return max_abs;
}

static void strip_ppisp_parity(const TypeInfo* types) {
    using namespace backend::vk;
    const char* reduction = spirula::env("PPISP_REDUCTION");
    if (reduction && std::strcmp(reduction, "native") != 0)
        throw std::runtime_error("ppisp strip fixture needs SS_PPISP_REDUCTION=native");
    Rng r(261017u);
    constexpr int B = 3, H = 67, W = 29, cut = 32;
    for (int t = 0; t < 6; ++t) for (bool indexed : {false, true}) {
        const int N = indexed ? 5 : B, P = types[t].n_params;
        auto image = r.vec(B * H * W * 3, -0.05f, 1.2f);
        auto params = r.vec(N * P, -0.08f, 0.08f);
        auto cot = r.vec(image.size(), -0.05f/(B*H*W), 0.05f/(B*H*W));
        for (size_t i = 0; i < cot.size(); ++i)
            if ((i / 3) % 5 == 0 || i % 7 == 0) cot[i] = 0;
        std::vector<float> intr = {80,80,14,33,80,80,13,32,80,80,15,31};
        std::vector<int> indices = {4,1,4};
        std::vector<void*> allocations;
        auto keep = [&](auto* p) { allocations.push_back(p); return p; };
        try {
            set_image_strip({});
            auto* im = keep(upload(image)); auto* pa = keep(upload(params));
            auto* in = keep(upload(intr)); auto* ca = indexed ? keep(upload(indices)) : nullptr;
            auto* co = keep(upload(cot));
            auto* rgb = keep(alloc_zero<float>(image.size()));
            auto* grad = keep(alloc_zero<float>(params.size()));
            const auto cameras = indexed ? ttv(ca,{B}) : ttv_null();
            const size_t count = ppisp_backward_staged_scratch_floats(B,H,W,types[t].name);
            auto* scratch = keep(alloc_zero<float>(count));
            auto backward = [&](bool staged) {
                backend::memset_sync(grad,0,params.size()*sizeof(float));
                if (staged)
                    ppisp_backward_staged(dt3(im,B,H,W),ttv(pa,{N,P}),ttv(in,{B,4}),
                        W,H,dt3(co,B,H,W),types[t].name,cameras,dt3(rgb,B,H,W),
                        ttv(grad,{N,P}),scratch,count);
                else
                    ppisp_backward(dt3(im,B,H,W),ttv(pa,{N,P}),ttv(in,{B,4}),
                        W,H,dt3(co,B,H,W),types[t].name,cameras,dt3(rgb,B,H,W),
                        ttv(grad,{N,P}));
                std::vector<float> g;
                readback_f(g,grad,params.size());
                return g;
            };
            auto forward = [&] {
                ppisp_forward(dt3(im,B,H,W),ttv(pa,{N,P}),ttv(in,{B,4}),W,H,
                    types[t].name,cameras,dt3(rgb,B,H,W));
            };
            forward();
            std::vector<float> full_forward;
            readback_f(full_forward,rgb,image.size());
            auto native = backward(false);
            std::vector<float> native_rgb;
            readback_f(native_rgb,rgb,image.size());
            auto repeat = backward(false);
            auto full = backward(true);
            std::vector<float> full_rgb;
            readback_f(full_rgb,rgb,image.size());
            if (std::memcmp(native_rgb.data(),full_rgb.data(),image.size()*sizeof(float)))
                throw std::runtime_error("staged PPISP RGB differs from native");
            std::vector<float> combined(params.size(),0);
            for (int role = 0; role < 3; ++role) {
                const uint32_t begin = role == 0 ? 0 : cut;
                const uint32_t end = role == 1 ? H : cut;
                set_image_strip({H,begin,end,5,{}});
                forward();
                std::vector<float> strip_forward;
                readback_f(strip_forward,rgb,image.size());
                const auto rows = image_strip_rows(H);
                for (int b = 0; b < B; ++b)
                    for (uint32_t y = rows.work_begin; y < rows.work_end; ++y)
                        for (int x = 0; x < W * 3; ++x) {
                            const size_t i = (b * H + y) * W * 3 + x;
                            if (strip_forward[i] != full_forward[i])
                                throw std::runtime_error("strip PPISP forward differs");
                        }
                auto partial = backward(true);
                if (role == 2 && std::any_of(partial.begin(),partial.end(),
                                             [](float v) { return v != 0; }))
                    throw std::runtime_error("empty PPISP strip gradient is not exact zero");
                for (size_t i = 0; i < combined.size(); ++i) combined[i] += partial[i];
                std::vector<float> strip_rgb;
                readback_f(strip_rgb,rgb,image.size());
                for (size_t i = 0; i < image.size(); ++i) {
                    const uint32_t y = uint32_t(i / (W * 3)) % H;
                    const float expected = y >= begin && y < end ? native_rgb[i] : 0;
                    if (strip_rgb[i] != expected)
                        throw std::runtime_error("strip PPISP owned RGB differs");
                }
            }
            set_image_strip({});
            std::printf("PPISP strip layout=%s indexed=%d\n",types[t].name,int(indexed));
            strip_difference(native,repeat,"native-repeat");
            strip_difference(native,full,"native-staged");
            strip_difference(full,combined,"staged-combined");
            auto* update_full = keep(upload(params));
            auto* update_strips = keep(upload(params));
            auto* accum_full = keep(upload(std::vector<float>(params.size(),0.01f)));
            auto* accum_strips = keep(upload(std::vector<float>(params.size(),0.01f)));
            auto* gradient_full = keep(upload(native));
            auto* gradient_strips = keep(upload(combined));
            fused_adagrad_step(ttv(update_full,{N*P,1}),ttv(gradient_full,{N*P,1}),
                               ttv(accum_full,{N*P,1}),0.001f);
            fused_adagrad_step(ttv(update_strips,{N*P,1}),ttv(gradient_strips,{N*P,1}),
                               ttv(accum_strips,{N*P,1}),0.001f);
            std::vector<float> a,b;
            readback_f(a,update_full,params.size());
            readback_f(b,update_strips,params.size());
            strip_difference(a,b,"native-combined-adagrad");
            a.clear(); b.clear();
            readback_f(a,accum_full,params.size());
            readback_f(b,accum_strips,params.size());
            strip_difference(a,b,"native-combined-adagrad-accum");
            backend::memset_sync(co,0,image.size()*sizeof(float));
            const auto zero_grad = backward(true);
            if (std::any_of(zero_grad.begin(),zero_grad.end(),[](float v) { return v != 0; }))
                throw std::runtime_error("zero PPISP cotangent produced nonzero gradient");
            a.clear();
            readback_f(a,rgb,image.size());
            if (std::any_of(a.begin(),a.end(),[](float v) { return v != 0; }))
                throw std::runtime_error("zero PPISP cotangent produced nonzero RGB");
            if (const char* error = backend::last_error()) throw std::runtime_error(error);
        } catch (...) {
            set_image_strip({});
            for (auto* p : allocations) backend::device_free(p);
            throw;
        }
        for (auto* p : allocations) backend::device_free(p);
    }
}

static void strip_bilagrid_parity() {
    using namespace backend::vk;
    Rng r(261018u);
    constexpr int B = 3, N = 5, H = 67, W = 29, cut = 32;
    constexpr int L = 8, Hg = 16, Wg = 16, P = 9;
    constexpr size_t pixels = B * H * W * 3;
    constexpr size_t cells = N * L * Hg * Wg * P;
    constexpr size_t grad_cells = B * L * Hg * Wg * P;
    for (bool quantized : {false,true}) for (int arm = 0; arm < 3; ++arm) {
        std::vector<void*> allocations;
        auto keep = [&](auto* p) { allocations.push_back(p); return p; };
        try {
            set_image_strip({});
            auto* image = keep(upload(r.vec(pixels,0.01f,0.9f)));
            auto* params = keep(upload(r.vec(N*P,-0.03f,0.03f)));
            auto* intr = keep(upload(std::vector<float>{80,80,14,33,80,80,13,32,80,80,15,31}));
            auto* cams = keep(upload(std::vector<int>{4,1,4}));
            auto* grid = keep(upload(r.vec(cells,-0.03f,0.03f)));
            auto* q16 = keep(alloc_zero<uint16_t>(cells));
            auto* bounds = keep(alloc_zero<float2>((cells+255)/256));
            bilagrid_encode_q16_launch(grid,q16,bounds,cells,backend::kDefaultStream);
            const BilagridReader reader = quantized
                ? BilagridReader::from_q16(q16,bounds) : BilagridReader(grid);
            auto cot = r.vec(pixels,-0.05f/(B*H*W),0.05f/(B*H*W));
            for (size_t i = 0; i < pixels; ++i)
                if ((i/3)%5 == 0 || i%7 == 0) cot[i] = 0;
            auto* vout = keep(upload(cot));
            auto* pre = keep(alloc_zero<float>(pixels));
            auto* post = keep(alloc_zero<float>(pixels));
            auto* rgb = keep(alloc_zero<float>(pixels));
            auto* pg = keep(alloc_zero<float>(N*P));
            auto* bg = keep(alloc_zero<float>(grad_cells));
            const size_t count = ppisp_backward_staged_scratch_floats(B,H,W,"no_crf_no_vig");
            auto* scratch = keep(alloc_zero<float>(count));
            auto forward = [&] {
                ppisp_forward(dt3(image,B,H,W),ttv(params,{N,P}),ttv(intr,{B,4}),W,H,
                    "no_crf_no_vig",ttv(cams,{B}),dt3(pre,B,H,W));
                bilagrid_ppisp_uniform_sample_forward(reader,pre,post,B,L,Hg,Wg,H,W,
                    backend::kDefaultStream,cams);
            };
            auto backward = [&](bool staged) {
                backend::memset_sync(bg,0,grad_cells*sizeof(float));
                backend::memset_sync(pg,0,N*P*sizeof(float));
                if (arm == 2)
                    bilagrid_ppisp_uniform_sample_backward_v2(reader,pre,vout,bg,rgb,
                        B,L,Hg,Wg,H,W,backend::kDefaultStream,cams);
                else
                    bilagrid_ppisp_uniform_sample_backward_v1(reader,pre,vout,bg,rgb,
                        B,L,Hg,Wg,H,W,arm == 0 ? 5 : 1,backend::kDefaultStream,cams);
                if (staged)
                    ppisp_backward_staged(dt3(image,B,H,W),ttv(params,{N,P}),ttv(intr,{B,4}),
                        W,H,dt3(rgb,B,H,W),"no_crf_no_vig",ttv(cams,{B}),
                        dt3(rgb,B,H,W),ttv(pg,{N,P}),scratch,count);
                else
                    ppisp_backward(dt3(image,B,H,W),ttv(params,{N,P}),ttv(intr,{B,4}),
                        W,H,dt3(rgb,B,H,W),"no_crf_no_vig",ttv(cams,{B}),
                        dt3(rgb,B,H,W),ttv(pg,{N,P}));
                std::vector<float> gradients;
                readback_f(gradients,pg,N*P);
                readback_f(gradients,bg,grad_cells);
                return gradients;
            };
            forward();
            std::vector<float> native_forward;
            readback_f(native_forward,post,pixels);
            auto native = backward(false);
            std::vector<float> native_rgb;
            readback_f(native_rgb,rgb,pixels);
            auto repeat = backward(false);
            auto full = backward(true);
            std::vector<float> combined(full.size(),0);
            for (int role = 0; role < 3; ++role) {
                const uint32_t begin = role == 0 ? 0 : cut;
                const uint32_t end = role == 1 ? H : cut;
                set_image_strip({H,begin,end,5,{}});
                forward();
                std::vector<float> strip_forward;
                readback_f(strip_forward,post,pixels);
                const auto rows = image_strip_rows(H);
                for (size_t i = 0; i < pixels; ++i) {
                    const uint32_t y = uint32_t(i/(W*3))%H;
                    if (y >= rows.work_begin && y < rows.work_end &&
                        strip_forward[i] != native_forward[i])
                        throw std::runtime_error("early PPISP/bilagrid strip forward differs");
                }
                auto partial = backward(true);
                if (role == 2 && std::any_of(partial.begin(),partial.end(),
                                             [](float v) { return v != 0; }))
                    throw std::runtime_error("empty bilagrid strip gradient is not exact zero");
                for (size_t i = 0; i < full.size(); ++i) combined[i] += partial[i];
                std::vector<float> strip_rgb;
                readback_f(strip_rgb,rgb,pixels);
                for (size_t i = 0; i < pixels; ++i) {
                    const uint32_t y = uint32_t(i/(W*3))%H;
                    const float expected = y >= begin && y < end ? native_rgb[i] : 0;
                    if (strip_rgb[i] != expected)
                        throw std::runtime_error("early PPISP/bilagrid owned RGB differs");
                }
            }
            set_image_strip({});
            std::printf("early PPISP/bilagrid strip quantized=%d arm=%d\n",int(quantized),arm);
            auto compare = [&](const auto& a, const auto& b, const char* label) {
                std::printf("  %s\n",label);
                strip_difference(std::vector<float>(a.begin(),a.begin()+N*P),
                    std::vector<float>(b.begin(),b.begin()+N*P),"ppisp");
                strip_difference(std::vector<float>(a.begin()+N*P,a.end()),
                    std::vector<float>(b.begin()+N*P,b.end()),"bilagrid");
            };
            compare(native,repeat,"native-repeat");
            compare(native,full,"native-staged");
            compare(full,combined,"staged-combined");
            if (const char* error = backend::last_error()) throw std::runtime_error(error);
        } catch (...) {
            set_image_strip({});
            for (auto* p : allocations) backend::device_free(p);
            throw;
        }
        for (auto* p : allocations) backend::device_free(p);
    }
}
#endif
int main(int argc, char** argv) {
#if defined(SS_BACKEND_VULKAN)
    if (argc == 2 && std::strcmp(argv[1], "strip") == 0) {
        const TypeInfo types[] = {
            {"original",36,0}, {"rqs",39,0}, {"no_crf",24,0},
            {"no_crf_clamp",24,0}, {"no_crf_no_vig",9,0},
            {"no_crf_no_vig_clamp",9,0}};
        try { strip_ppisp_parity(types); strip_bilagrid_parity(); }
        catch (const std::exception& e) {
            std::fprintf(stderr,"ppisp strip: %s\n",e.what());
            return 1;
        }
        std::printf("ppisp strip: PASSED\n");
        return 0;
    }
#endif
    if (argc != 3 ||
        (std::strcmp(argv[1], "dump") && std::strcmp(argv[1], "compare"))) {
        std::fprintf(stderr, "usage: %s strip | dump|compare <ref.bin>\n", argv[0]);
        return 2;
    }
    const bool dumping = std::strcmp(argv[1], "dump") == 0;

    Rng r(260719u), r_arith(260914u);

    const TypeInfo types[6] = {
        {"original", 36, (int)RawPPISPRegLossIndex::length},
        {"rqs", 39, (int)RawPPISPRegLossIndexRQS::length},
        {"no_crf", 24, (int)RawPPISPRegLossIndexNoCRF::length},
        {"no_crf_clamp", 24, (int)RawPPISPRegLossIndexNoCRF::length},
        {"no_crf_no_vig", 9, (int)RawPPISPRegLossIndexNoCRFNoVig::length},
        {"no_crf_no_vig_clamp", 9,
         (int)RawPPISPRegLossIndexNoCRFNoVig::length},
    };
#if defined(SS_BACKEND_VULKAN)
    nullable_rgb_parity(types);
#endif

    const int64_t B = 3, H = 22, W = 26, N_cam = 5;
    const float aw = 104.0f, ah = 88.0f;

    for (const TypeInfo& t : types) {
        for (int with_cams = 0; with_cams < 2; with_cams++) {
            const int64_t n_tab = with_cams ? N_cam : B;
            // Small parameters keep the CRF / homography paths well-
            // conditioned; the raw parametrization passes through softplus /
            // sigmoid so 0-centered draws are the intended regime.
            float* params =
                upload(r.vec(n_tab * t.n_params, -0.4f, 0.4f));
            std::vector<float> intr_h;
            for (int64_t b = 0; b < B; b++) {
                intr_h.push_back(80.0f);           // fx (unused)
                intr_h.push_back(80.0f);           // fy (unused)
                intr_h.push_back(r.uf(40.0f, 60.0f));  // cx
                intr_h.push_back(r.uf(35.0f, 50.0f));  // cy
            }
            float* intrins = upload(intr_h);
            std::vector<int> cam_h = {4, 1, 4};  // duplicate camera
            int* cams = with_cams ? upload(cam_h) : nullptr;

            int64_t np = B * H * W;
            float* in_image = upload(r.vec(3 * np, 0.0f, 1.0f));
            float* out_image = alloc_zero<float>(3 * np);
            ppisp_forward(dt3(in_image, B, H, W), ttv(params, {n_tab, t.n_params}),
                          ttv(intrins, {B, 4}), aw, ah, t.name,
                          cams ? ttv(cams, {B}) : ttv_null(),
                          dt3(out_image, B, H, W));
            readback_f(g_tight, out_image, 3 * np);

            float* v_out = upload(r.vec(3 * np, -0.3f, 0.3f));
            float* v_in = alloc_zero<float>(3 * np);
            float* v_params = alloc_zero<float>(n_tab * t.n_params);
            ppisp_backward(dt3(in_image, B, H, W),
                           ttv(params, {n_tab, t.n_params}),
                           ttv(intrins, {B, 4}), aw, ah,
                           dt3(v_out, B, H, W), t.name,
                           cams ? ttv(cams, {B}) : ttv_null(),
                           dt3(v_in, B, H, W),
                           ttv(v_params, {n_tab, t.n_params}));
            readback_f(g_tight, v_in, 3 * np);
            readback_f(g_loose, v_params, n_tab * t.n_params);
        }

        // Regularization forward: per-image raw rows are deterministic
        // (tight); the summed tail row and the weighted losses derived from
        // it are atomic-order dependent (loose).
        for (int arith = 0; arith < 2; arith++) {
            // Own stream for the added mode, so every later block keeps its data.
            Rng& rr = arith ? r_arith : r;
            const int64_t Bp = 6;
            std::vector<float> params_h = rr.vec(Bp * t.n_params, -0.5f, 0.5f);
            float* params = upload(params_h);
            std::array<float, (int)PPISPRegLossIndex::length> weights{};
            for (auto& w : weights) w = rr.uf(0.1f, 2.0f);
            float* losses = alloc_zero<float>((int)PPISPRegLossIndex::length);
            float* raw_losses = alloc_zero<float>((Bp + 1) * t.n_raw);
            compute_ppsip_regularization_forward(
                ttv(params, {Bp, t.n_params}), weights, t.name, arith != 0,
                ttv(losses, {(int)PPISPRegLossIndex::length}),
                ttv(raw_losses, {Bp + 1, t.n_raw}));
            size_t raw_at = g_tight.size(), loss_at = g_loose.size() + t.n_raw;
            readback_f(g_tight, raw_losses, Bp * t.n_raw);
            readback_f(g_loose, raw_losses + Bp * t.n_raw, t.n_raw);
            readback_f(g_loose, losses, (int)PPISPRegLossIndex::length);

            // Backward with synthetic (host-fixed) summed raw losses so the
            // whole chain stays deterministic. log2 needs a positive gain sum.
            std::vector<float> raw_h = rr.vec((Bp + 1) * t.n_raw, -1.0f, 1.0f);
            if (arith) raw_h[Bp * t.n_raw] = rr.uf(0.5f, 2.0f) * (float)Bp;
            float* raw_fixed = upload(raw_h);
            std::vector<float> vl_h =
                rr.vec((int)PPISPRegLossIndex::length, -1.0f, 1.0f);
            float* v_losses = upload(vl_h);
            float* v_params = alloc_zero<float>(Bp * t.n_params);
            compute_ppsip_regularization_backward(
                ttv(params, {Bp, t.n_params}), weights,
                ttv(raw_fixed, {Bp + 1, t.n_raw}),
                ttv(v_losses, {(int)PPISPRegLossIndex::length}), t.name,
                arith != 0, ttv(v_params, {Bp, t.n_params}));
            size_t vp_at = g_tight.size();
            readback_f(g_tight, v_params, Bp * t.n_params);
            if (!arith) continue;

            // Closed form of the exposure terms, which touch nothing else.
            auto dsl1 = [](double x) {
                return std::fabs(x) < 0.1 ? x / 0.1 : (x > 0 ? 1.0 : -1.0);
            };
            auto sl1 = [](double x) {
                return std::fabs(x) < 0.1 ? 0.5 * x * x / 0.1 : std::fabs(x) - 0.05;
            };
            auto off = [](double got, double want) {
                return std::fabs(got - want) > 1e-4 + 1e-4 * std::fabs(want);
            };
            double gain_sum = 0.0;
            for (int64_t b = 0; b < Bp; b++) {
                double gain = std::exp2((double)params_h[b * t.n_params]);
                gain_sum += gain;
                if (off(g_tight[raw_at + b * t.n_raw], gain)) {
                    std::fprintf(stderr, "ppisp_parity: %s raw gain[%lld] = %g, "
                                 "expected %g\n", t.name, (long long)b,
                                 g_tight[raw_at + b * t.n_raw], gain);
                    return 1;
                }
            }
            double want_loss = weights[0] * sl1(std::log2(gain_sum / Bp));
            if (off(g_loose[loss_at], want_loss)) {
                std::fprintf(stderr, "ppisp_parity: %s exposure mean loss = %g, "
                             "expected %g\n", t.name, g_loose[loss_at], want_loss);
                return 1;
            }
            double S = raw_h[Bp * t.n_raw];
            double v_sum = vl_h[0] * weights[0] *
                           dsl1(std::log2(S / Bp)) / (S * std::log(2.0));
            for (int64_t b = 0; b < Bp; b++) {
                double p0 = params_h[b * t.n_params];
                double want = v_sum * std::exp2(p0) * std::log(2.0);
                if (off(g_tight[vp_at + b * t.n_params], want)) {
                    std::fprintf(stderr, "ppisp_parity: %s v_exposure[%lld] = %g, "
                                 "expected %g\n", t.name, (long long)b,
                                 g_tight[vp_at + b * t.n_params], want);
                    return 1;
                }
            }
        }
    }

    // Negative-intensity pixels, exposure and vignetting neutralized so the
    // colour stage stands alone: ~half of them sit below the RGI denominator
    // floor, which is the only thing holding intensity through that branch.
    {
        const int64_t Bx = 2, np = Bx * H * W;
        const int n_params = 24;  // no_crf: exposure(1) + vignetting(15) + colour(8)
        std::vector<float> ph(Bx * n_params, 0.0f);
        for (int64_t b = 0; b < Bx; b++)
            for (int i = 16; i < n_params; i++)
                ph[b * n_params + i] = r.uf(-0.4f, 0.4f);
        float* params = upload(ph);
        std::vector<float> intr_h;
        for (int64_t b = 0; b < Bx; b++) {
            intr_h.push_back(80.0f);
            intr_h.push_back(80.0f);
            intr_h.push_back(52.0f);
            intr_h.push_back(44.0f);
        }
        float* intrins = upload(intr_h);
        std::vector<float> in_h = r.vec(3 * np, -1.0f, 1.0f);
        float* in_image = upload(in_h);
        float* out_image = alloc_zero<float>(3 * np);
        ppisp_forward(dt3(in_image, Bx, H, W), ttv(params, {Bx, n_params}),
                      ttv(intrins, {Bx, 4}), aw, ah, "no_crf", ttv_null(),
                      dt3(out_image, Bx, H, W));

        std::vector<float> out_h(3 * np);
        backend::memcpy_sync(out_h.data(), out_image,
                             3 * np * sizeof(float), MemcpyKind::DeviceToHost);
        double worst_di = 0.0, worst_mag = 0.0;
        for (int64_t i = 0; i < np; i++) {
            double si = (double)in_h[3 * i] + in_h[3 * i + 1] + in_h[3 * i + 2];
            double so = (double)out_h[3 * i] + out_h[3 * i + 1] + out_h[3 * i + 2];
            worst_di = std::max(worst_di, std::fabs(so - si));
            for (int c = 0; c < 3; c++)
                worst_mag = std::max(worst_mag, std::fabs((double)out_h[3 * i + c]));
        }
        std::printf("ppisp_parity: negative-intensity block, "
                    "max |sum(out) - sum(in)| %.3g, max |out| %.3g\n",
                    worst_di, worst_mag);
        // Two-sided: unfloored these leave at |out| ~ 3 (H is near identity),
        // floored they cap near 20x that, and without the floor at all the old
        // 1e-5 denominator sent them past 1e5.
        if (!(worst_di < 1e-3) || !(worst_mag > 10.0) || !(worst_mag < 1e3)) {
            std::fprintf(stderr, "ppisp_parity: colour stage lost intensity, or "
                                 "the denominator floor never bound / blew up\n");
            return 1;
        }
        readback_f(g_tight, out_image, 3 * np);

        float* v_out = upload(r.vec(3 * np, -0.3f, 0.3f));
        float* v_in = alloc_zero<float>(3 * np);
        float* v_params = alloc_zero<float>(Bx * n_params);
        ppisp_backward(dt3(in_image, Bx, H, W), ttv(params, {Bx, n_params}),
                       ttv(intrins, {Bx, 4}), aw, ah, dt3(v_out, Bx, H, W),
                       "no_crf", ttv_null(), dt3(v_in, Bx, H, W),
                       ttv(v_params, {Bx, n_params}));
        readback_f(g_tight, v_in, 3 * np);
        readback_f(g_loose, v_params, Bx * n_params);
    }

    // The clamp axis against the unclamped mode it shares a table with. The
    // exposure seed is what pushes a good share of the pixels out of [0, 1];
    // without it the whole check passes on an identity.
    {
        const int64_t Bc = 2, np = Bc * H * W;
        const int n_params = 24;
        std::vector<float> ph(Bc * n_params, 0.0f);
        for (int64_t b = 0; b < Bc; b++) {
            ph[b * n_params] = 0.8f;                       // exposure
            for (int i = 16; i < n_params; i++)
                ph[b * n_params + i] = r.uf(-0.3f, 0.3f);  // colour
        }
        float* params = upload(ph);
        std::vector<float> intr_h;
        for (int64_t b = 0; b < Bc; b++) {
            intr_h.push_back(80.0f);
            intr_h.push_back(80.0f);
            intr_h.push_back(52.0f);
            intr_h.push_back(44.0f);
        }
        float* intrins = upload(intr_h);
        float* in_image = upload(r.vec(3 * np, 0.0f, 1.0f));
        float* plain = alloc_zero<float>(3 * np);
        float* clamped = alloc_zero<float>(3 * np);
        for (int c = 0; c < 2; c++)
            ppisp_forward(dt3(in_image, Bc, H, W),
                          ttv(params, {Bc, n_params}), ttv(intrins, {Bc, 4}),
                          aw, ah, c ? "no_crf_clamp" : "no_crf", ttv_null(),
                          dt3(c ? clamped : plain, Bc, H, W));

        std::vector<float> hp(3 * np), hc(3 * np);
        backend::memcpy_sync(hp.data(), plain, 3 * np * sizeof(float),
                             MemcpyKind::DeviceToHost);
        backend::memcpy_sync(hc.data(), clamped, 3 * np * sizeof(float),
                             MemcpyKind::DeviceToHost);
        int64_t clipped = 0;
        for (int64_t i = 0; i < 3 * np; i++) {
            float want = std::min(std::max(hp[i], 0.0f), 1.0f);
            if (want != hp[i]) clipped++;
            if (hc[i] != want) {
                std::fprintf(stderr,
                             "ppisp_parity: no_crf_clamp[%lld] = %g, expected "
                             "clamp(%g) = %g\n",
                             (long long)i, hc[i], hp[i], want);
                return 1;
            }
        }
        if (clipped < 3 * np / 20) {
            std::fprintf(stderr,
                         "ppisp_parity: only %lld of %lld channels clipped -- "
                         "the clamp path is not being exercised\n",
                         (long long)clipped, (long long)(3 * np));
            return 1;
        }
        std::printf("ppisp_parity: clamp block, %lld / %lld channels clipped\n",
                    (long long)clipped, (long long)(3 * np));

        // Gradient: zero wherever the clip bound.
        std::vector<float> vo_h = r.vec(3 * np, -0.3f, 0.3f);
        float* v_out = upload(vo_h);
        float* v_in_plain = alloc_zero<float>(3 * np);
        float* v_in_clamp = alloc_zero<float>(3 * np);
        float* v_params_plain = alloc_zero<float>(Bc * n_params);
        float* v_params_clamp = alloc_zero<float>(Bc * n_params);
        for (int c = 0; c < 2; c++)
            ppisp_backward(dt3(in_image, Bc, H, W),
                           ttv(params, {Bc, n_params}), ttv(intrins, {Bc, 4}),
                           aw, ah, dt3(v_out, Bc, H, W),
                           c ? "no_crf_clamp" : "no_crf", ttv_null(),
                           dt3(c ? v_in_clamp : v_in_plain, Bc, H, W),
                           ttv(c ? v_params_clamp : v_params_plain,
                               {Bc, n_params}));
        std::vector<float> hvc(3 * np);
        backend::memcpy_sync(hvc.data(), v_in_clamp, 3 * np * sizeof(float),
                             MemcpyKind::DeviceToHost);
        // A pixel is clipped per channel, but the colour stage mixes the three,
        // so only an all-clipped pixel has a strictly zero input gradient.
        for (int64_t i = 0; i < np; i++) {
            bool all_clipped = true;
            for (int c = 0; c < 3; c++) {
                float v = hp[3 * i + c];
                if (v >= 0.0f && v <= 1.0f) all_clipped = false;
            }
            if (!all_clipped) continue;
            for (int c = 0; c < 3; c++) {
                if (hvc[3 * i + c] != 0.0f) {
                    std::fprintf(stderr,
                                 "ppisp_parity: clipped pixel %lld kept "
                                 "gradient %g\n",
                                 (long long)i, hvc[3 * i + c]);
                    return 1;
                }
            }
        }
        readback_f(g_tight, clamped, 3 * np);
        readback_f(g_tight, v_in_clamp, 3 * np);
        readback_f(g_loose, v_params_clamp, Bc * n_params);
    }

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
        std::printf("ppisp_parity: dumped %zu + %zu floats to %s\n",
                    g_tight.size(), g_loose.size(), argv[2]);
        return 0;
    }

    if (const char* dump_got = spirula::env("PPISP_DUMP_GOT"))
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

    auto cmp_f = [](const std::vector<float>& got,
                    const std::vector<float>& want, int64_t& viol,
                    double& max_abs) {
        viol = 0;
        max_abs = 0;
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
        }
    };
    int64_t fviol = 0, lviol = 0;
    double fmax = 0, lmax = 0;
    cmp_f(g_tight, ref, fviol, fmax);
    cmp_f(g_loose, lref, lviol, lmax);
    double ffrac = nf ? (double)fviol / (double)nf : 0.0;
    double lfrac = nl ? (double)lviol / (double)nl : 0.0;
    std::printf(
        "ppisp_parity: %lld tight floats (max_abs %.3g, violations %lld = "
        "%.5f%%), %lld loose floats (max_abs %.3g, violations %lld = "
        "%.5f%%)\n",
        (long long)nf, fmax, (long long)fviol, 100.0 * ffrac, (long long)nl,
        lmax, (long long)lviol, 100.0 * lfrac);
    bool pass = ffrac <= 2e-3 && lfrac <= 2e-2;
    std::printf(pass ? "ppisp_parity: PASSED\n" : "ppisp_parity: FAILED\n");
    return pass ? 0 : 1;
}
