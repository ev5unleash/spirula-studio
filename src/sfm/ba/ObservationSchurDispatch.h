#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

#include "sfm/core/Cancel.h"
#include "sfm/vk/VkContext.h"

namespace spirula {

inline void submitObservationSchur(VkContext& ctx, VkCommandBuffer& prefix, uint32_t numObs,
                                   float damping, const std::string& entry) {
    if (prefix != VK_NULL_HANDLE)
        ctx.submit(std::exchange(prefix, VK_NULL_HANDLE));
    Push push;
    push.u0 = numObs;
    push.f0 = damping;
    for (uint32_t offset = 0; offset < numObs;) {
        sfm::cancel::check();
        const uint32_t count = std::min(numObs - offset, 64u * 128u);
        push.u1 = offset;
        VkCommandBuffer cb = ctx.begin();
        ctx.barrier(cb);
        ctx.dispatch(cb, entry, (count + 127u) / 128u, push);
        ctx.barrier(cb);
        ctx.submit(std::exchange(cb, VK_NULL_HANDLE));
        offset += count;
    }
}

}  // namespace spirula
