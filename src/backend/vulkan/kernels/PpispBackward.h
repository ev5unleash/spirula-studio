#pragma once

#include "core/Tensor.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace backend::vk {

// Experimental global reduction order, not native parameter-gradient byte parity.
// Scratch: B * ceil(H*W/64) * P float32 values; caller owns/reuses device storage.
size_t ppisp_backward_staged_scratch_floats(
    int64_t B, int64_t H, int64_t W, const std::string& param_type);

// Same additive/pre-zeroed parameter-gradient contract and valid camera indices
// as ppisp_backward. Null parameter gradients require no scratch.
void ppisp_backward_staged(
    DeviceTensor3D<float3> in_image, TorchTensorView ppisp_params,
    TorchTensorView intrins, float actual_image_width,
    float actual_image_height, DeviceTensor3D<float3> v_out_image,
    std::string param_type, TorchTensorView cam_indices,
    DeviceTensor3D<float3> v_in_image, TorchTensorView v_ppisp_params,
    float* partials, size_t partial_capacity_floats);

}  // namespace backend::vk
