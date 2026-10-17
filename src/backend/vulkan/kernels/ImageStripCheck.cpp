#include "backend/vulkan/kernels/ImageStrip.h"
#include "backend/vulkan/kernels/KernelCommon.h"

#include <limits>
#include <stdexcept>

namespace backend::vk {

void image_strip_check_finite(const float* values, size_t count,
                              uint32_t* invalid_flag) {
    if (!count) return;
    if (!values || !invalid_flag || count > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("image strip: invalid finite-check extent");
    struct Params {
        uint64_t values, invalid_flag;
        uint32_t count, wgs_per_row;
    };
    static_assert(sizeof(Params) == 24, "image strip finite-check ABI");
    const auto grid = vkk::fold_1d(count, 256);
    const Params params{reinterpret_cast<uint64_t>(values),
        reinterpret_cast<uint64_t>(invalid_flag), static_cast<uint32_t>(count),
        grid.per_row};
    vkk::dispatch("image_strip_check.check_finite", {}, grid.per_row, grid.rows,
                  1, &params, sizeof(params));
}

}  // namespace backend::vk
