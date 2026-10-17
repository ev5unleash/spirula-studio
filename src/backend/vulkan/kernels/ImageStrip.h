#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace backend::vk {

struct ImageStripRows {
    uint32_t owned_begin, owned_end, work_begin, work_end;
};

struct ImageStripContext {
    uint32_t full_height = 0, owned_begin = 0, owned_end = 0, halo = 0;
    std::function<void(float* device_values, size_t count)> sum;
};

void set_image_strip(ImageStripContext context);
const ImageStripContext& image_strip();
// Matches the native max(1, H / 2) pyramid; interior cuts must remain aligned.
ImageStripRows image_strip_rows(uint32_t height);
uint32_t image_strip_halo(uint32_t num_loss_scales, bool depth_normals = false);
// Replaces corresponding raw statistics on both roles before normalization/VJP.
void image_strip_sum(float* device_values, size_t count);
void image_strip_zero_outside(float* device_values, int64_t B, int64_t H,
                             int64_t W, int64_t C, bool owned);
void image_strip_check_finite(const float* values, size_t count, uint32_t* invalid_flag);

}  // namespace backend::vk
