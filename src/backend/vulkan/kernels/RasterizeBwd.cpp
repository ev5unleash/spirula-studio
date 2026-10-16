// Vulkan implementation of the rasterization-backward launch APIs
// (kernels/raster/RasterizationBwd.cuh + RasterizationEval3DBwd.cuh). Mirrors the
// rasterize_to_pixels_*_bwd_tensor host wrappers (v_splat zeros_pool /
// accum-weight / v_viewmats allocation) and dispatches
// shaders/rasterize_bwd.slang: one 32-thread workgroup per 8x8
// sub-tile, grid (I, tile_h*2, tile_w*2). Template axes become
// specialization constants (camera model, dist type, median, accum weight,
// viewmat grad, packed).

#include <kernels/raster/RasterizationBwd.cuh>
#include <kernels/raster/RasterizationEval3DBwd.cuh>

#include "backend/vulkan/kernels/KernelCommon.h"
#include "backend/vulkan/kernels/B1Ownership.h"
#include <shaders/screen_layout.h>

#include <optional>

namespace {

thread_local bool g1_ordered_raster = false;

// Mirrors Raster2dBwdParams in shaders/rasterize_bwd.slang.
struct Raster2dBwdParams {
    uint64_t s_screen;
    uint64_t gaussian_ids, tile_offsets, flatten_ids;
    uint64_t render_Ts, last_ids;
    uint64_t out_rgb, out_depth, dist_rgb, dist_depth, awmap;
    uint64_t v_out_rgb, v_out_depth, v_render_Ts, v_median, v_dist_rgb,
        v_dist_depth;
    uint64_t v_s_screen;
    uint64_t o_accum_weight, o_accum_weight_den;
    uint64_t ordered_partials;
    uint32_t I, N, n_isects, width, height, tile_width, tile_height, macro_log2;
    uint32_t ordered_stride;
};
static_assert(sizeof(Raster2dBwdParams) == 21 * 8 + 10 * 4,
              "params layout must match the slang struct");


struct RasterOrderedParams {
    uint64_t flatten_ids, keys, perm, partials, v_screen;
    uint64_t gaussian_ids, accum_weight, accum_weight_den;
    uint32_t n_isects, subtiles, wgs_per_row;
    uint32_t N, stride, accum_mode;
};
static_assert(sizeof(RasterOrderedParams) == 8 * 8 + 6 * 4,
              "params layout must match the slang struct");
// Mirrors Raster3dgutBwdParams.
struct Raster3dgutBwdParams {
    uint64_t means, quats, scales, s_screen, gaussian_ids;
    uint64_t viewmats, intrins, dist_coeffs, aabb;
    uint64_t tile_offsets, flatten_ids, render_Ts, last_ids;
    uint64_t out_rgb, out_depth, dist_rgb, dist_depth, awmap;
    uint64_t v_out_rgb, v_out_depth, v_render_Ts, v_median, v_dist_rgb,
        v_dist_depth;
    uint64_t v_means, v_quats, v_scales, v_s_screen;
    uint64_t o_accum_weight, o_accum_weight_den, v_viewmats;
    uint32_t I, N, n_isects, width, height, tile_width, tile_height, macro_log2;
};
static_assert(sizeof(Raster3dgutBwdParams) == 31 * 8 + 8 * 4,
              "params layout must match the slang struct");

uint32_t dist_spec_bwd(DistortionType dist_type) {
    switch (dist_type) {
        case DistortionType::None: return 0;
        case DistortionType::D: return 1;
        case DistortionType::RGB_D: return 2;
        default:
            throw std::runtime_error(
                "rasterize_to_pixels_bwd: distortion type not instantiated "
                "(normal needs a normal-rendering primitive)");
    }
}

}  // namespace

void vkk::g1_set_ordered_raster(bool enabled) {
    g1_ordered_raster = enabled;
}

/* API definition matching kernels/raster/RasterizationBwd.cuh */

std::tuple<
    std::vector<DeviceTensorFloatND>, std::vector<DeviceTensorFloatND>,
    DeviceVector<float>
> rasterize_to_pixels_3dgs_bwd(
    int64_t num_splats,
    std::vector<DeviceTensorFloatND> splats_w,
    std::vector<DeviceTensorFloatND> splats_s,
    DeviceVector<int32_t> gaussian_ids,
    const uint32_t image_width,
    const uint32_t image_height,
    const DeviceTensor3D<int32_t> tile_offsets,
    const DeviceVector<int32_t> flatten_ids,
    int macro_log2,
    const DeviceTensor3D<float> render_Ts,
    const DeviceTensor3D<int32_t> last_ids,
    RenderOutput::TensorTuple render_outputs_tuple,
    std::optional<RenderOutput::TensorTuple> distortion_fwd_outputs,
    DistortionType dist_type,
    DeviceTensor3D<float> accum_weight_map,
    DensifyAccumMode accum_mode,
    RenderOutput::TensorTuple v_render_outputs,
    const DeviceTensor3D<float> v_render_Ts,
    const DeviceTensor3D<float> v_median,
    std::optional<RenderOutput::TensorTuple> v_distortion_outputs,
    std::optional<std::vector<DeviceTensorFloatND>> v_splats_w,
    std::optional<std::vector<DeviceTensorFloatND>> v_splats_s
) {
    const uint32_t spec_dist = dist_spec_bwd(dist_type);
    if (accum_weight_map.data_ptr() == nullptr)
        accum_mode = DensifyAccumMode::None;
    const bool aw = accum_any(accum_mode);
    const bool md = v_median.data_ptr() != nullptr;
    const bool packed = gaussian_ids.data_ptr() != nullptr;

    // Opens the raster-backward phase: the screen-gradient buffer below shares
    // the tile intersector's arena (POOL_ALIAS_TABLE, core/PoolSlots.h).
    pool_begin_phase(PoolPhase::RasterBwd);
    if (!v_splats_w.has_value())
        v_splats_w = Vanilla3DGS<0>::WorldBuffer::zeros_pool(
            splats_w, PoolSlot::RasterBwdVWorld);
    if (!v_splats_s.has_value())
        v_splats_s = Vanilla3DGS<0>::ScreenBuffer::zeros_pool(
            splats_s, PoolSlot::RasterBwdVScreen);
    DeviceVector<float> o_accum_weight;
    if (aw) {
        o_accum_weight.resize(PoolSlot::RasterBwdAccumWeight,
                              num_splats * accum_lanes(accum_mode));
        o_accum_weight.zero();
    }

    const uint32_t I = (uint32_t)render_Ts.size<0>();
    const uint32_t tile_height = (uint32_t)tile_offsets.size<1>();
    const uint32_t tile_width = (uint32_t)tile_offsets.size<2>();
    const uint32_t n_isects = (uint32_t)flatten_ids.size();

    const bool ordered = g1_ordered_raster;
    uint32_t subtiles = 0;
    const uint32_t ordered_stride = SCR2_STRIDE +
        (aw ? accum_lanes(accum_mode) : 0);
    if (ordered) {
        if (I != 1 || num_splats < 0 ||
            (packed ? gaussian_ids.size() : num_splats) >
                0x7fffffffLL / SCR2_STRIDE ||
            macro_log2 < kMacroLog2Min || macro_log2 > kMacroLog2Max)
            throw std::runtime_error(
                "G1 ordered raster requires single-image 3dgs "
                "and supported binning/row sizes");
        subtiles = 1u << (2 * macro_log2);
        const uint64_t scalars = uint64_t(flatten_ids.size()) *
                                 subtiles * ordered_stride;
        if (scalars > 0x7fffffffULL ||
            (uint64_t(tile_height) << macro_log2) > 65535 ||
            (uint64_t(tile_width) << macro_log2) > 65535)
            throw std::runtime_error(
                "G1 ordered raster scratch/index or dispatch limit exceeded");
    }

    if (n_isects > 0) {
        Vanilla3DGS<0>::ScreenBuffer sb(splats_s);
        Vanilla3DGS<0>::ScreenBuffer vsb(v_splats_s.value());


        DeviceVector<float> partials;
        DeviceVector<int32_t> keys_a, keys_b, perm_a, perm_b;
        RasterOrderedParams op{};
        if (ordered) {
            partials.resize(PoolSlot::RasterBwdOrderedPartials,
                            int64_t(n_isects) * subtiles * ordered_stride);
            // Culled, untouched and outside-image subtiles remain explicit zero.
            partials.zero();
            keys_a.resize(PoolSlot::RasterBwdOrderedKeysA, n_isects);
            keys_b.resize(PoolSlot::RasterBwdOrderedKeysB, n_isects);
            perm_a.resize(PoolSlot::RasterBwdOrderedPermA, n_isects);
            perm_b.resize(PoolSlot::RasterBwdOrderedPermB, n_isects);
            op.flatten_ids = (uint64_t)flatten_ids.data_ptr();
            op.keys = (uint64_t)keys_a.data_ptr();
            op.perm = (uint64_t)perm_a.data_ptr();
            op.partials = (uint64_t)partials.data_ptr();
            op.v_screen = (uint64_t)vsb.raw_data();
            op.n_isects = n_isects;
            op.subtiles = subtiles;
            op.gaussian_ids = vkk::or_fallback(gaussian_ids.data_ptr());
            op.accum_weight = vkk::or_fallback(o_accum_weight.data_ptr());
            op.accum_weight_den = vkk::or_fallback(
                accum_mode == DensifyAccumMode::Avg
                    ? o_accum_weight.data_ptr() + num_splats : nullptr);
            op.N = packed ? 0u : (uint32_t)num_splats;
            op.stride = ordered_stride;
            op.accum_mode = (uint32_t)accum_mode;
            const auto g = vkk::fold_1d(n_isects, 256);
            op.wgs_per_row = g.per_row;
            vkk::dispatch_ring("rasterize_bwd.raster_ordered_keys", {},
                               g.per_row, g.rows, 1, &op, sizeof(op));
        }
        Raster2dBwdParams p{};
        p.s_screen = (uint64_t)sb.raw_data();
        p.gaussian_ids = vkk::or_fallback(gaussian_ids.data_ptr());
        p.tile_offsets = (uint64_t)tile_offsets.data_ptr();
        p.flatten_ids = (uint64_t)flatten_ids.data_ptr();
        p.render_Ts = (uint64_t)render_Ts.data_ptr();
        p.last_ids = (uint64_t)last_ids.data_ptr();
        p.out_rgb = (uint64_t)std::get<0>(render_outputs_tuple).data_ptr();
        p.out_depth = (uint64_t)std::get<1>(render_outputs_tuple).data_ptr();
        if (distortion_fwd_outputs.has_value()) {
            p.dist_rgb = vkk::or_fallback(
                std::get<0>(distortion_fwd_outputs.value()).data_ptr());
            p.dist_depth = vkk::or_fallback(
                std::get<1>(distortion_fwd_outputs.value()).data_ptr());
        } else {
            p.dist_rgb = vkk::null_fallback();
            p.dist_depth = vkk::null_fallback();
        }
        p.awmap = vkk::or_fallback(accum_weight_map.data_ptr());
        p.v_out_rgb = (uint64_t)std::get<0>(v_render_outputs).data_ptr();
        p.v_out_depth = (uint64_t)std::get<1>(v_render_outputs).data_ptr();
        p.v_render_Ts = (uint64_t)v_render_Ts.data_ptr();
        p.v_median = vkk::or_fallback(v_median.data_ptr());
        if (v_distortion_outputs.has_value()) {
            p.v_dist_rgb = vkk::or_fallback(
                std::get<0>(v_distortion_outputs.value()).data_ptr());
            p.v_dist_depth = vkk::or_fallback(
                std::get<1>(v_distortion_outputs.value()).data_ptr());
        } else {
            p.v_dist_rgb = vkk::null_fallback();
            p.v_dist_depth = vkk::null_fallback();
        }
        p.v_s_screen = (uint64_t)vsb.raw_data();
        p.o_accum_weight = vkk::or_fallback(o_accum_weight.data_ptr());
        p.o_accum_weight_den = vkk::or_fallback(
            accum_mode == DensifyAccumMode::Avg
                ? o_accum_weight.data_ptr() + num_splats : nullptr);
        p.ordered_partials = vkk::or_fallback(partials.data_ptr());
        p.I = I;
        p.N = packed ? 0u : (uint32_t)num_splats;
        p.n_isects = n_isects;
        p.width = image_width;
        p.height = image_height;
        p.tile_width = tile_width;
        p.tile_height = tile_height;
        p.macro_log2 = (uint32_t)macro_log2;
        p.ordered_stride = ordered_stride;

        // Spec IDs: cam(0, unused), dist(1), median(2), accum(3),
        // viewmat(4, unused), packed(5), distortion(6, unused), ordered(7).
        backend::vk::SpecList spec{0u,           spec_dist,
                                   md ? 1u : 0u, (uint32_t)accum_mode,
                                   0u,           packed ? 1u : 0u,
                                   0u, ordered ? 1u : 0u};
        vkk::dispatch_ring("rasterize_bwd.rasterize_bwd_2d", spec, I,
                           tile_height << macro_log2,
                           tile_width << macro_log2, &p, sizeof(p));
        if (ordered) {
            backend::DoubleBuffer<int32_t> keys(keys_a.data_ptr(),
                                                keys_b.data_ptr());
            backend::DoubleBuffer<int32_t> perm(perm_a.data_ptr(),
                                                perm_b.data_ptr());
            backend::sort_pairs(keys, perm, n_isects, 0, 32);
            op.keys = (uint64_t)keys.current();
            op.perm = (uint64_t)perm.current();
            const auto g = vkk::fold_1d(n_isects, 1);
            op.wgs_per_row = g.per_row;
            vkk::dispatch_ring("rasterize_bwd.raster_ordered_reduce", {},
                               g.per_row, g.rows, 1, &op, sizeof(op));
        }
    }

    return std::make_tuple(v_splats_w.value(), v_splats_s.value(),
                           o_accum_weight);
}

/* API definition matching kernels/raster/RasterizationEval3DBwd.cuh */

std::tuple<
    std::vector<DeviceTensorFloatND>, std::vector<DeviceTensorFloatND>,
    DeviceTensor2D<float>,
    DeviceVector<float>
> rasterize_to_pixels_3dgut_bwd(
    int64_t num_splats,
    std::vector<DeviceTensorFloatND> splats_w,
    std::vector<DeviceTensorFloatND> splats_s,
    DeviceVector<int32_t> gaussian_ids,
    TorchTensorView viewmats,
    TorchTensorView intrins,
    const std::string camera_model,
    const std::string distortion,
    const TorchTensorView dist_coeffs,
    DeviceTensor2D<uint2> aabb,
    const uint32_t image_width,
    const uint32_t image_height,
    const DeviceTensor3D<int32_t> tile_offsets,
    const DeviceVector<int32_t> flatten_ids,
    int macro_log2,
    const DeviceTensor3D<float> render_Ts,
    const DeviceTensor3D<int32_t> last_ids,
    RenderOutput::TensorTuple render_outputs,
    std::optional<RenderOutput::TensorTuple> distortion_fwd_outputs,
    DistortionType dist_type,
    DeviceTensor3D<float> loss_map,
    DeviceTensor3D<float> accum_weight_map,
    DensifyAccumMode accum_mode,
    RenderOutput::TensorTuple v_render_outputs,
    const DeviceTensor3D<float> v_render_Ts,
    const DeviceTensor3D<float> v_median,
    std::optional<RenderOutput::TensorTuple> v_distortion_outputs,
    std::optional<std::vector<DeviceTensorFloatND>> v_splats_w,
    std::optional<std::vector<DeviceTensorFloatND>> v_splats_s,
    bool need_viewmat_grad
) {
    if (g1_ordered_raster)
        throw std::runtime_error("G1 ordered raster does not support 3dgut");
    (void)loss_map;  // declared but unused by the CUDA kernel as well
    const vkk::CamDistSpec cd = vkk::cam_dist_spec(camera_model, distortion);
    const uint32_t spec_dist = dist_spec_bwd(dist_type);
    if (accum_weight_map.data_ptr() == nullptr)
        accum_mode = DensifyAccumMode::None;
    const bool aw = accum_any(accum_mode);
    const bool md = v_median.data_ptr() != nullptr;
    const bool packed = gaussian_ids.data_ptr() != nullptr;

    // Opens the raster-backward phase: the screen-gradient buffer below shares
    // the tile intersector's arena (POOL_ALIAS_TABLE, core/PoolSlots.h).
    pool_begin_phase(PoolPhase::RasterBwd);
    if (!v_splats_w.has_value())
        v_splats_w = Vanilla3DGUT<0>::WorldBuffer::zeros_pool(
            splats_w, PoolSlot::RasterBwdVWorld);
    if (!v_splats_s.has_value())
        v_splats_s = Vanilla3DGUT<0>::ScreenBuffer::zeros_pool(
            splats_s, PoolSlot::RasterBwdVScreen);
    DeviceTensor2D<float> v_viewmats_buf;
    if (need_viewmat_grad) {
        auto& shape = std::get<2>(viewmats);
        int64_t total = 1;
        for (auto s : shape) total *= s;
        v_viewmats_buf.resize(PoolSlot::RasterBwdVViewmats, total, 1);
        v_viewmats_buf.zero();
    }
    DeviceVector<float> o_accum_weight;
    if (aw) {
        o_accum_weight.resize(PoolSlot::RasterBwdAccumWeight,
                              num_splats * accum_lanes(accum_mode));
        o_accum_weight.zero();
    }

    const uint32_t I = (uint32_t)render_Ts.size<0>();
    const uint32_t tile_height = (uint32_t)tile_offsets.size<1>();
    const uint32_t tile_width = (uint32_t)tile_offsets.size<2>();
    const uint32_t n_isects = (uint32_t)flatten_ids.size();

    if (n_isects > 0) {
        Vanilla3DGUT<0>::WorldBuffer wb(splats_w);
        Vanilla3DGUT<0>::ScreenBuffer sb(splats_s);
        Vanilla3DGUT<0>::WorldBuffer vwb(v_splats_w.value());
        Vanilla3DGUT<0>::ScreenBuffer vsb(v_splats_s.value());

        Raster3dgutBwdParams p{};
        p.means = (uint64_t)wb.raw_data(0);
        p.quats = (uint64_t)wb.raw_data(1);
        p.scales = (uint64_t)wb.raw_data(2);
        p.s_screen = (uint64_t)sb.raw_data();
        p.gaussian_ids = vkk::or_fallback(gaussian_ids.data_ptr());
        p.viewmats = std::get<0>(viewmats);
        p.intrins = std::get<0>(intrins);
        p.dist_coeffs = vkk::or_fallback(std::get<0>(dist_coeffs));
        p.aabb = (uint64_t)aabb.data_ptr();
        p.tile_offsets = (uint64_t)tile_offsets.data_ptr();
        p.flatten_ids = (uint64_t)flatten_ids.data_ptr();
        p.render_Ts = (uint64_t)render_Ts.data_ptr();
        p.last_ids = (uint64_t)last_ids.data_ptr();
        p.out_rgb = (uint64_t)std::get<0>(render_outputs).data_ptr();
        p.out_depth = (uint64_t)std::get<1>(render_outputs).data_ptr();
        if (distortion_fwd_outputs.has_value()) {
            p.dist_rgb = vkk::or_fallback(
                std::get<0>(distortion_fwd_outputs.value()).data_ptr());
            p.dist_depth = vkk::or_fallback(
                std::get<1>(distortion_fwd_outputs.value()).data_ptr());
        } else {
            p.dist_rgb = vkk::null_fallback();
            p.dist_depth = vkk::null_fallback();
        }
        p.awmap = vkk::or_fallback(accum_weight_map.data_ptr());
        p.v_out_rgb = (uint64_t)std::get<0>(v_render_outputs).data_ptr();
        p.v_out_depth = (uint64_t)std::get<1>(v_render_outputs).data_ptr();
        p.v_render_Ts = (uint64_t)v_render_Ts.data_ptr();
        p.v_median = vkk::or_fallback(v_median.data_ptr());
        if (v_distortion_outputs.has_value()) {
            p.v_dist_rgb = vkk::or_fallback(
                std::get<0>(v_distortion_outputs.value()).data_ptr());
            p.v_dist_depth = vkk::or_fallback(
                std::get<1>(v_distortion_outputs.value()).data_ptr());
        } else {
            p.v_dist_rgb = vkk::null_fallback();
            p.v_dist_depth = vkk::null_fallback();
        }
        p.v_means = (uint64_t)vwb.raw_data(0);
        p.v_quats = (uint64_t)vwb.raw_data(1);
        p.v_scales = (uint64_t)vwb.raw_data(2);
        p.v_s_screen = (uint64_t)vsb.raw_data();
        p.o_accum_weight = vkk::or_fallback(o_accum_weight.data_ptr());
        p.o_accum_weight_den = vkk::or_fallback(
            accum_mode == DensifyAccumMode::Avg
                ? o_accum_weight.data_ptr() + num_splats : nullptr);
        p.v_viewmats = vkk::or_fallback(v_viewmats_buf.data_ptr());
        p.I = I;
        p.N = packed ? 0u : (uint32_t)num_splats;
        p.n_isects = n_isects;
        p.width = image_width;
        p.height = image_height;
        p.tile_width = tile_width;
        p.tile_height = tile_height;
        p.macro_log2 = (uint32_t)macro_log2;

        // Spec IDs: cam(0), dist(1), median(2), accum(3), viewmat(4),
        // packed(5), distortion tier(6).
        backend::vk::SpecList spec{
            cd.cam,         spec_dist,
            md ? 1u : 0u,   (uint32_t)accum_mode,
            need_viewmat_grad ? 1u : 0u, packed ? 1u : 0u,
            cd.dist};
        vkk::dispatch_ring("rasterize_bwd.rasterize_bwd_3dgut", spec, I,
                           tile_height << macro_log2,
                           tile_width << macro_log2, &p, sizeof(p));
    }

    return std::make_tuple(v_splats_w.value(), v_splats_s.value(),
                           v_viewmats_buf, o_accum_weight);
}
