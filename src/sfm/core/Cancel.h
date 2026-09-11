#pragma once

// Stopping a run that is already going.
//
// The token is process-global, like the log sink and the progress directory:
// one SfM job per process, set by whoever drives it. A check throws, so a
// stage does not have to thread a status back through every call it makes --
// and the unwind runs ~VkContext, which is what frees the device memory the
// stage was holding (sfm/vk/VkContext.h).

#include <atomic>
#include <exception>

namespace sfm {

// Not a failure: the caller asked for it. Caught at the job boundary.
struct Cancelled : std::exception {
    const char* what() const noexcept override { return "sfm: cancelled"; }
};

namespace cancel {

// Null (the default) means nothing can stop the run, which is a plain CLI
// invocation. The flag outlives the run.
inline std::atomic<const std::atomic<bool>*> g_flag{nullptr};

inline void set_token(const std::atomic<bool>* flag) {
    g_flag.store(flag, std::memory_order_relaxed);
}

inline bool requested() {
    const std::atomic<bool>* f = g_flag.load(std::memory_order_relaxed);
    return f && f->load(std::memory_order_relaxed);
}

// Call at stage boundaries and once per image / pair / registration / solver
// iteration -- often enough that a cancel lands in under a second, rarely
// enough that the relaxed load never shows up in a profile.
inline void check() {
    if (requested()) throw Cancelled();
}

}  // namespace cancel
}  // namespace sfm
