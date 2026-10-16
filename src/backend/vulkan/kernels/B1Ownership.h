#pragma once

#include <cstdint>

namespace vkk {

// Null disables filtering; otherwise one nonzero entry owns each 256-row block.
void b1_set_owned_blocks(const uint32_t* device_ptr);
const uint32_t* b1_owned_blocks();

// Synchronous, thread-local override for the next native FPBO launch.
// The six pointers are row-major fp32 gradients, including SH [N, 3*K].
enum class FpboGradientMode { Internal, Capture, External };
struct FpboGradientLaunch {
    FpboGradientMode mode = FpboGradientMode::Internal;
    const float* means = nullptr;
    const float* quats = nullptr;
    const float* scales = nullptr;
    const float* opacities = nullptr;
    const float* dc = nullptr;
    const float* sh = nullptr;
};
void fpbo_set_gradient_launch(FpboGradientLaunch launch);
FpboGradientLaunch fpbo_gradient_launch();

// Bounded single-image 3dgs proof; thread-local, off by default.
void g1_set_ordered_raster(bool enabled);

}  // namespace vkk
