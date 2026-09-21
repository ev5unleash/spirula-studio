// Numerical regression for the production uint8 image conversion path.
// Run once with native bytes and once with SS_VK_NATIVE_INT8=0.

#include "backend/api/BackendRuntime.h"
#include "engine/EngineInternal.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

int main() {
    constexpr int count = 3 * 256 + 13;
    std::vector<uint8_t> input(count);
    for (int i = 0; i < count; ++i) input[i] = static_cast<uint8_t>(i);

    auto* device_input = static_cast<uint8_t*>(backend::device_malloc(input.size()));
    auto* device_output = static_cast<float*>(
        backend::device_malloc(input.size() * sizeof(float)));
    backend::memcpy_sync(device_input, input.data(), input.size(),
                         backend::MemcpyKind::HostToDevice);
    uint8_image_to_float_raw(device_input, device_output, 1, 1, count, 1);

    std::vector<float> output(count);
    backend::memcpy_sync(output.data(), device_output,
                         output.size() * sizeof(float),
                         backend::MemcpyKind::DeviceToHost);
    backend::device_free(device_output);
    backend::device_free(device_input);

    for (int i = 0; i < count; ++i) {
        const float expected = static_cast<float>(input[i]) / 255.0f;
        if (std::fabs(output[i] - expected) > 1e-7f) {
            std::fprintf(stderr,
                         "byte %d at %d: got %.9g, expected %.9g\n",
                         static_cast<int>(input[i]), i, output[i], expected);
            return 1;
        }
    }
    const std::vector<uint16_t> depth = {0, 1, 1000, 65535};
    auto* device_depth = static_cast<uint16_t*>(
        backend::device_malloc(depth.size() * sizeof(uint16_t)));
    auto* device_depth_out = static_cast<float*>(
        backend::device_malloc(depth.size() * sizeof(float)));
    backend::memcpy_sync(device_depth, depth.data(),
                         depth.size() * sizeof(uint16_t),
                         backend::MemcpyKind::HostToDevice);
    uint16_depth_to_float_raw(device_depth, device_depth_out, 1, 1,
                              (int)depth.size(), 1, 0.001f);
    std::vector<float> scaled_depth(depth.size());
    backend::memcpy_sync(scaled_depth.data(), device_depth_out,
                         scaled_depth.size() * sizeof(float),
                         backend::MemcpyKind::DeviceToHost);
    backend::device_free(device_depth_out);
    backend::device_free(device_depth);
    for (size_t i = 0; i < depth.size(); ++i) {
        const float expected = (float)depth[i] * 0.001f;
        if (std::fabs(scaled_depth[i] - expected) > 1e-6f) {
            std::fprintf(stderr,
                         "depth %u: got %.9g, expected %.9g\n",
                         (unsigned)depth[i], scaled_depth[i], expected);
            return 1;
        }
    }
    std::printf("vk_byte_conversion: %d values passed on %s\n", count,
                backend::device_current_selector().c_str());
    return 0;
}
