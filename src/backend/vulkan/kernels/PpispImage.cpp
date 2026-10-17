// Vulkan implementations of the PPISP image transform + regularization
// launch APIs (kernels/pixelwise/PixelWise.cuh PPISP subset) and the PpispInit.cu
// helpers (engine/EngineInternal.h). Device work: shaders/
// ppisp_image.slang (canonical shaders/ppisp.slang math) + the init kernels in
// bilagrid_tv.slang.

#include <kernels/pixelwise/PixelWise.cuh>
#include <engine/EngineInternal.h>
#include <core/Tensor.h>

#include "backend/vulkan/kernels/KernelCommon.h"
#include "backend/vulkan/kernels/PpispBackward.h"
#include "backend/vulkan/kernels/ImageStrip.h"

#include <limits>

namespace {

// Mirrors PpispImageParams in shaders/ppisp_image.slang.
struct PpispImageParams {
    uint64_t in_image, ppisp_params, intrins, cam_indices, out_image,
        v_in_image, v_ppisp_params;
    int32_t B, H, W;
    float actual_image_width, actual_image_height;
    int32_t has_cam_indices;
    uint32_t wgs_per_row;
    uint32_t row_begin, row_end;
};
static_assert(sizeof(PpispImageParams) == 96, "layout");

struct PpispStagedParams {
    PpispImageParams image;
    uint64_t partials;
    uint32_t groups_per_image, num_cameras;
};
static_assert(sizeof(PpispStagedParams) == 112, "layout");

// Mirrors PpispRegParams.
struct PpispRegParams {
    uint64_t ppisp_params, raw_losses, losses, v_ppisp_params;
    float loss_weights[(int)PPISPRegLossIndex::length];
    int32_t B;
    int32_t _pad0, _pad1, _pad2;
};
static_assert(sizeof(PpispRegParams) ==
                  4 * 8 + ((int)PPISPRegLossIndex::length + 4) * 4,
              "layout");

struct PpispInitParams {
    uint64_t params;
    uint32_t total, wgs_per_row;
};
static_assert(sizeof(PpispInitParams) == 8 + 2 * 4, "layout");

struct AddIntoGradParams {
    uint64_t src, dst;
    uint32_t total, wgs_per_row;
};
static_assert(sizeof(AddIntoGradParams) == 2 * 8 + 2 * 4, "layout");

// (kParamType, kClampOutput, kExposureArithmeticMean), in ppisp_image.slang
// declaration order.
backend::vk::SpecList spec_list(const PpispParamSpec& spec,
                                bool exposure_arithmetic_mean) {
    return backend::vk::SpecList{(uint32_t)spec.layout,
                                 spec.clamp_output ? 1u : 0u,
                                 exposure_arithmetic_mean ? 1u : 0u};
}

// Fold the pixel axis across (gx, gz); gy carries the batch index.
void dispatch_image(const char* entry, const PpispParamSpec& spec,
                    int64_t pixels, uint32_t block, int64_t B,
                    PpispImageParams& p) {
    if (pixels <= 0 || B <= 0) return;
    vkk::Fold f = vkk::fold_1d(pixels, block);
    p.wgs_per_row = f.per_row;
    if (B > 65535 || f.rows > 65535)
        throw std::runtime_error("ppisp: image grid dimension exceeds 65535");
    vkk::dispatch(entry, spec_list(spec, false), f.per_row, (uint32_t)B,
                  f.rows, &p, sizeof(p));
}

size_t staged_scratch_floats(int64_t B, int64_t H, int64_t W, int np) {
    constexpr int64_t max_index = std::numeric_limits<int32_t>::max();
    if (B < 0 || B > 65535 || H < 0 || H > max_index ||
        W < 0 || W > max_index)
        throw std::runtime_error("ppisp staged: invalid image dimensions");
    if (B == 0 || H == 0 || W == 0) return 0;
    if (H > max_index / W || B > max_index / (H * W) / 3)
        throw std::runtime_error("ppisp staged: image indexing overflow");
    const int64_t groups = (H * W + 63) / 64;
    if (groups > max_index / B / np)
        throw std::runtime_error("ppisp staged: scratch indexing overflow");
    const uint64_t count = uint64_t(B) * uint64_t(groups) * uint64_t(np);
    if (count > std::numeric_limits<size_t>::max() / sizeof(float))
        throw std::runtime_error("ppisp staged: scratch byte size overflow");
    const vkk::Fold f = vkk::fold_1d(H * W, 64);
    if (f.rows > 65535)
        throw std::runtime_error("ppisp staged: image grid exceeds 65535");
    return size_t(count);
}

bool staged_reduction() {
    static const bool enabled = [] {
        const char* mode = spirula::env("PPISP_REDUCTION");
        if (!mode || std::strcmp(mode, "native") == 0) return false;
        if (std::strcmp(mode, "staged") == 0) return true;
        throw std::runtime_error("SS_PPISP_REDUCTION must be native or staged");
    }();
    return enabled;
}

void image_rows(PpispImageParams& p, bool backward) {
    const auto rows = backend::vk::image_strip_rows(uint32_t(p.H));
    p.row_begin = backward ? rows.owned_begin : rows.work_begin;
    p.row_end = backward ? rows.owned_end : rows.work_end;
}

}  // namespace

void ppisp_forward(
    DeviceTensor3D<float3> in_image, TorchTensorView ppisp_params,
    TorchTensorView intrins, const float actual_image_width,
    const float actual_image_height, std::string param_type,
    TorchTensorView cam_indices, DeviceTensor3D<float3> out_image
) {
    const int64_t b = in_image.size<0>();
    const int64_t h = in_image.size<1>();
    const int64_t w = in_image.size<2>();
    const PpispParamSpec spec = ppisp_param_spec(param_type);
    const uint64_t cam_ptr = std::get<0>(cam_indices);

    PpispImageParams p{};
    p.in_image = (uint64_t)in_image.data_ptr();
    p.ppisp_params = std::get<0>(ppisp_params);
    p.intrins = std::get<0>(intrins);
    p.cam_indices = vkk::or_fallback(cam_ptr);
    p.out_image = (uint64_t)out_image.data_ptr();
    p.v_in_image = vkk::or_fallback(nullptr);
    p.v_ppisp_params = vkk::or_fallback(nullptr);
    p.B = (int32_t)b;
    p.H = (int32_t)h;
    p.W = (int32_t)w;
    p.actual_image_width = actual_image_width;
    p.actual_image_height = actual_image_height;
    p.has_cam_indices = cam_ptr != 0 ? 1 : 0;
    image_rows(p, false);
    dispatch_image("ppisp_image.ppisp_image_fwd", spec, h * w, 256, b, p);
}

void ppisp_backward(
    DeviceTensor3D<float3> in_image, TorchTensorView ppisp_params,
    TorchTensorView intrins, const float actual_image_width,
    const float actual_image_height, DeviceTensor3D<float3> v_out_image,
    std::string param_type, TorchTensorView cam_indices,
    DeviceTensor3D<float3> v_in_image, TorchTensorView v_ppisp_params
) {
    if (staged_reduction() && std::get<0>(v_ppisp_params) != 0) {
        static const std::string scratch_name = "vk.ppisp.staged_partials";
        const size_t count = backend::vk::ppisp_backward_staged_scratch_floats(
            in_image.size<0>(), in_image.size<1>(), in_image.size<2>(), param_type);
        float* scratch = count == 0 ? nullptr :
            (float*)DevicePool::global().acquire_dynamic(
                VramCategory::Image, scratch_name, count * sizeof(float));
        backend::vk::ppisp_backward_staged(
            in_image, ppisp_params, intrins, actual_image_width,
            actual_image_height, v_out_image, param_type, cam_indices,
            v_in_image, v_ppisp_params, scratch, count);
        return;
    }
    const int64_t b = in_image.size<0>();
    const int64_t h = in_image.size<1>();
    const int64_t w = in_image.size<2>();
    const PpispParamSpec spec = ppisp_param_spec(param_type);
    const uint64_t cam_ptr = std::get<0>(cam_indices);

    PpispImageParams p{};
    p.in_image = (uint64_t)in_image.data_ptr();
    p.ppisp_params = std::get<0>(ppisp_params);
    p.intrins = std::get<0>(intrins);
    p.cam_indices = vkk::or_fallback(cam_ptr);
    p.out_image = (uint64_t)v_out_image.data_ptr();
    p.v_in_image = (uint64_t)v_in_image.data_ptr();
    p.v_ppisp_params = std::get<0>(v_ppisp_params);
    p.B = (int32_t)b;
    p.H = (int32_t)h;
    p.W = (int32_t)w;
    p.actual_image_width = actual_image_width;
    p.actual_image_height = actual_image_height;
    p.has_cam_indices = cam_ptr != 0 ? 1 : 0;
    image_rows(p, true);
    dispatch_image("ppisp_image.ppisp_image_bwd", spec, h * w, 64, b, p);
}

size_t backend::vk::ppisp_backward_staged_scratch_floats(
    int64_t B, int64_t H, int64_t W, const std::string& param_type
) {
    return staged_scratch_floats(
        B, H, W, ppisp_param_spec(param_type).num_params);
}

void backend::vk::ppisp_backward_staged(
    DeviceTensor3D<float3> in_image, TorchTensorView ppisp_params,
    TorchTensorView intrins, float actual_image_width,
    float actual_image_height, DeviceTensor3D<float3> v_out_image,
    std::string param_type, TorchTensorView cam_indices,
    DeviceTensor3D<float3> v_in_image, TorchTensorView v_ppisp_params,
    float* partials, size_t partial_capacity_floats
) {
    const int64_t b = in_image.size<0>();
    const int64_t h = in_image.size<1>();
    const int64_t w = in_image.size<2>();
    const PpispParamSpec spec = ppisp_param_spec(param_type);
    const size_t required = staged_scratch_floats(b, h, w, spec.num_params);
    if (required == 0) return;
    if (v_out_image.size<0>() != b || v_out_image.size<1>() != h ||
        v_out_image.size<2>() != w || v_in_image.size<0>() != b ||
        v_in_image.size<1>() != h || v_in_image.size<2>() != w ||
        !in_image.data_ptr() || !v_out_image.data_ptr() ||
        !v_in_image.data_ptr())
        throw std::runtime_error("ppisp staged: invalid image gradient views");
    const auto& shape = std::get<2>(ppisp_params);
    if (shape.size() != 2 || shape[0] <= 0 ||
        shape[0] > std::numeric_limits<int32_t>::max() / spec.num_params ||
        shape[1] != spec.num_params || std::get<1>(ppisp_params) != 4 ||
        std::get<0>(ppisp_params) == 0)
        throw std::runtime_error("ppisp staged: invalid parameter table");
    const auto& intr_shape = std::get<2>(intrins);
    if (intr_shape.size() != 2 || intr_shape[0] != b ||
        intr_shape[1] != 4 || std::get<1>(intrins) != 4 ||
        std::get<0>(intrins) == 0)
        throw std::runtime_error("ppisp staged: invalid intrinsics");
    const uint64_t cam_ptr = std::get<0>(cam_indices);
    const auto& cam_shape = std::get<2>(cam_indices);
    if (cam_ptr != 0) {
        if (cam_shape.size() != 1 || cam_shape[0] != b ||
            std::get<1>(cam_indices) != 4)
            throw std::runtime_error("ppisp staged: invalid camera index view");
    } else if (shape[0] < b) {
        throw std::runtime_error("ppisp staged: missing batch parameter rows");
    }
    if (std::get<0>(v_ppisp_params) == 0) {
        ::ppisp_backward(in_image, ppisp_params, intrins, actual_image_width,
                         actual_image_height, v_out_image, param_type,
                         cam_indices, v_in_image, v_ppisp_params);
        return;
    }
    if (std::get<2>(v_ppisp_params) != shape ||
        std::get<1>(v_ppisp_params) != 4)
        throw std::runtime_error("ppisp staged: invalid parameter gradient table");
    if (!partials || partial_capacity_floats < required)
        throw std::runtime_error("ppisp staged: insufficient partial scratch");

    PpispStagedParams p{};
    p.image.in_image = (uint64_t)in_image.data_ptr();
    p.image.ppisp_params = std::get<0>(ppisp_params);
    p.image.intrins = std::get<0>(intrins);
    p.image.cam_indices = vkk::or_fallback(cam_ptr);
    p.image.out_image = (uint64_t)v_out_image.data_ptr();
    p.image.v_in_image = (uint64_t)v_in_image.data_ptr();
    p.image.v_ppisp_params = std::get<0>(v_ppisp_params);
    p.image.B = (int32_t)b;
    p.image.H = (int32_t)h;
    p.image.W = (int32_t)w;
    p.image.actual_image_width = actual_image_width;
    p.image.actual_image_height = actual_image_height;
    p.image.has_cam_indices = cam_ptr != 0 ? 1 : 0;
    image_rows(p.image, true);
    p.partials = (uint64_t)partials;
    p.groups_per_image = (uint32_t)((h * w + 63) / 64);
    p.num_cameras = (uint32_t)shape[0];
    const vkk::Fold f = vkk::fold_1d(h * w, 64);
    p.image.wgs_per_row = f.per_row;
    const auto sl = spec_list(spec, false);
    vkk::dispatch("ppisp_image.ppisp_image_bwd_staged", sl,
                  f.per_row, (uint32_t)b, f.rows, &p, sizeof(p));
    const vkk::Fold r = vkk::fold_1d(shape[0] * spec.num_params, 1);
    p.image.wgs_per_row = r.per_row;
    vkk::dispatch("ppisp_image.ppisp_image_bwd_staged_reduce", sl,
                  r.per_row, r.rows, 1, &p, sizeof(p));
}

void compute_ppsip_regularization_forward(
    TorchTensorView ppisp_params,
    const std::array<float, (int)PPISPRegLossIndex::length> loss_weights_0,
    std::string param_type, bool exposure_arithmetic_mean,
    TorchTensorView losses, TorchTensorView raw_losses
) {
    const PpispParamSpec spec = ppisp_param_spec(param_type);
    const int64_t B = std::get<2>(ppisp_params)[0];
    const int nr = spec.num_raw_losses;
    const backend::vk::SpecList sl = spec_list(spec, exposure_arithmetic_mean);

    PpispRegParams p{};
    p.ppisp_params = std::get<0>(ppisp_params);
    p.raw_losses = std::get<0>(raw_losses);
    p.losses = vkk::or_fallback(nullptr);
    p.v_ppisp_params = vkk::or_fallback(nullptr);
    for (int i = 0; i < (int)PPISPRegLossIndex::length; i++)
        p.loss_weights[i] = loss_weights_0[i];
    p.B = (int32_t)B;
    vkk::dispatch("ppisp_image.ppisp_reg_raw_fwd", sl,
                  (uint32_t)((B + 31) / 32), 1, 1, &p, sizeof(p));

    // Weighted final pass reads the summed tail row.
    PpispRegParams pf{};
    pf.ppisp_params = vkk::or_fallback(nullptr);
    pf.raw_losses =
        std::get<0>(raw_losses) + (uint64_t)B * nr * sizeof(float);
    pf.losses = std::get<0>(losses);
    pf.v_ppisp_params = vkk::or_fallback(nullptr);
    for (int i = 0; i < (int)PPISPRegLossIndex::length; i++)
        pf.loss_weights[i] = loss_weights_0[i];
    pf.B = (int32_t)B;
    vkk::dispatch("ppisp_image.ppisp_reg_final_fwd", sl, 1, 1, 1,
                  &pf, sizeof(pf));
}

void compute_ppsip_regularization_backward(
    TorchTensorView ppisp_params,
    const std::array<float, (int)PPISPRegLossIndex::length> loss_weights_0,
    TorchTensorView raw_losses, TorchTensorView v_losses,
    std::string param_type, bool exposure_arithmetic_mean,
    TorchTensorView v_ppisp_params
) {
    const PpispParamSpec spec = ppisp_param_spec(param_type);
    const int64_t B = std::get<2>(ppisp_params)[0];
    const int nr = spec.num_raw_losses;
    const backend::vk::SpecList sl = spec_list(spec, exposure_arithmetic_mean);

    float* v_raw_losses =
        DevicePool::global().acquire<float>(PoolSlot::PpispVRawLosses, nr);
    backend::memset_sync(v_raw_losses, 0, nr * sizeof(float));

    // v_losses -> v_raw_losses (single thread).
    PpispRegParams pf{};
    pf.ppisp_params = vkk::or_fallback(nullptr);
    pf.raw_losses =
        std::get<0>(raw_losses) + (uint64_t)B * nr * sizeof(float);
    pf.losses = std::get<0>(v_losses);
    pf.v_ppisp_params = (uint64_t)v_raw_losses;
    for (int i = 0; i < (int)PPISPRegLossIndex::length; i++)
        pf.loss_weights[i] = loss_weights_0[i];
    pf.B = (int32_t)B;
    vkk::dispatch("ppisp_image.ppisp_reg_final_bwd", sl, 1, 1, 1,
                  &pf, sizeof(pf));

    // v_raw_losses -> v_ppisp_params (per image).
    PpispRegParams p{};
    p.ppisp_params = std::get<0>(ppisp_params);
    p.raw_losses = (uint64_t)v_raw_losses;
    p.losses = vkk::or_fallback(nullptr);
    p.v_ppisp_params = std::get<0>(v_ppisp_params);
    for (int i = 0; i < (int)PPISPRegLossIndex::length; i++)
        p.loss_weights[i] = loss_weights_0[i];
    p.B = (int32_t)B;
    vkk::dispatch("ppisp_image.ppisp_reg_raw_bwd", sl,
                  (uint32_t)((B + 31) / 32), 1, 1, &p, sizeof(p));
}

void ppisp_original_default_init(
    float* params, int N, backend::Stream stream
) {
    (void)stream;
    if (N <= 0) return;
    PpispInitParams p{};
    p.params = (uint64_t)params;
    int64_t total = (int64_t)N * 36;
    p.total = (uint32_t)total;
    vkk::dispatch_flat("bilagrid_tv.ppisp_original_default_init",
                       backend::vk::SpecList{0u}, total, 256, &p, sizeof(p),
                       &p.wgs_per_row);
}

void ppisp_add_into_grad(
    const float* src, float* dst, size_t n, backend::Stream stream
) {
    (void)stream;
    if (n == 0) return;
    AddIntoGradParams p{};
    p.src = (uint64_t)src;
    p.dst = (uint64_t)dst;
    p.total = (uint32_t)n;
    vkk::dispatch_flat("bilagrid_tv.ppisp_add_into_grad",
                       backend::vk::SpecList{0u}, (int64_t)n, 256, &p,
                       sizeof(p), &p.wgs_per_row);
}
