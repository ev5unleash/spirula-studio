#include "backend/vulkan/kernels/ImageStrip.h"
#include "backend/api/BackendRuntime.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace backend::vk {
namespace {
thread_local ImageStripContext context;
}

void set_image_strip(ImageStripContext value) {
    if (value.full_height > uint32_t(std::numeric_limits<int32_t>::max()) ||
        value.owned_begin > value.owned_end ||
        value.owned_end > value.full_height)
        throw std::runtime_error("image strip: invalid full height or owned rows");
    context = std::move(value);
}

const ImageStripContext& image_strip() { return context; }

ImageStripRows image_strip_rows(uint32_t height) {
    if (!context.full_height) return {0, height, 0, height};
    if (!height) throw std::runtime_error("image strip: zero pyramid height");
    uint32_t h = context.full_height;
    uint64_t scale = 1;
    while (h != height && h > 1) {
        h = std::max(1u, h / 2);
        scale *= 2;
    }
    if (h != height)
        throw std::runtime_error("image strip: height is not in the native dyadic pyramid");
    auto endpoint = [&](uint32_t row) -> uint32_t {
        if (row == context.full_height) return height;
        if (row % scale)
            throw std::runtime_error("image strip: owned cut is not dyadically aligned");
        return uint32_t(std::min(uint64_t(height), row / scale));
    };
    const uint32_t begin = endpoint(context.owned_begin);
    const uint32_t end = endpoint(context.owned_end);
    if (begin == end) return {begin, end, begin, end};
    const uint64_t work_begin = context.owned_begin > context.halo
        ? context.owned_begin - context.halo : 0;
    const uint64_t work_end = std::min(uint64_t(context.full_height),
        uint64_t(context.owned_end) + context.halo);
    return {begin, end, uint32_t((work_begin + scale - 1) / scale),
        work_end == context.full_height ? height : uint32_t(work_end / scale)};
}

uint32_t image_strip_halo(uint32_t num_loss_scales, bool depth_normals) {
    if (!num_loss_scales || num_loss_scales > 4)
        throw std::runtime_error("image strip: loss scales must be in [1,4]");
    // SSIM input VJP reaches ten rows; a pooled row covers 2^level children.
    // Depth-normal's +/-1 forward stencil and backward scatter add two source rows.
    return (11u << (num_loss_scales - 1)) - 1u + (depth_normals ? 2u : 0u);
}

void image_strip_sum(float* device_values, size_t count) {
    if (!context.full_height) return;
    if (!device_values || !count || !context.sum)
        throw std::runtime_error("image strip: missing raw-statistic collective");
    context.sum(device_values, count);
}

void image_strip_zero_outside(float* values, int64_t B, int64_t H,
                             int64_t W, int64_t C, bool owned) {
    if (!context.full_height) return;
    const int64_t limit = std::numeric_limits<int32_t>::max();
    if (!values || B <= 0 || B > 65535 || H <= 0 || H > limit ||
        W <= 0 || C <= 0 || C > limit || W > limit / C / H / B)
        throw std::runtime_error("image strip: invalid pixel buffer or dispatch product");
    const auto rows = image_strip_rows(uint32_t(H));
    const uint32_t begin = owned ? rows.owned_begin : rows.work_begin;
    const uint32_t end = owned ? rows.owned_end : rows.work_end;
    const size_t row_bytes = size_t(W) * C * sizeof(float);
    auto* data = reinterpret_cast<char*>(values);
    for (int64_t b = 0; b < B; ++b) {
        char* plane = data + size_t(b) * H * row_bytes;
        if (begin)
            backend::memset_async(plane, 0, begin * row_bytes, backend::kDefaultStream);
        if (end < uint32_t(H))
            backend::memset_async(plane + end * row_bytes, 0,
                                  (H - end) * row_bytes, backend::kDefaultStream);
    }
}

}  // namespace backend::vk
