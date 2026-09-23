#pragma once

// Stopping a run that is already going.
//
// The token is process-global, like the log sink and the progress directory:
// one SfM job per process, set by whoever drives it. A check throws, so a
// stage does not have to thread a status back through every call it makes --
// and the unwind runs ~VkContext, which is what frees the device memory the
// stage was holding (sfm/vk/VkContext.h).

#include <atomic>
#include <chrono>
#include <exception>
#include <thread>

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
using PauseAcknowledge = void (*)(void*, bool);
inline std::atomic<const std::atomic<bool>*> g_pause_flag{nullptr};
inline std::atomic<PauseAcknowledge> g_pause_acknowledge{nullptr};
inline std::atomic<void*> g_pause_context{nullptr};

inline void set_pause_token(const std::atomic<bool>* flag,
                            PauseAcknowledge acknowledge = nullptr,
                            void* context = nullptr) {
    g_pause_acknowledge.store(nullptr, std::memory_order_release);
    g_pause_context.store(context, std::memory_order_release);
    g_pause_acknowledge.store(acknowledge, std::memory_order_release);
    g_pause_flag.store(flag, std::memory_order_release);
}

// Pause only at a caller-chosen durable or algorithm boundary. Stop is checked
// before acknowledging and while waiting, so it always releases a paused run.
inline void pause_point() {
    check();
    const std::atomic<bool>* flag =
        g_pause_flag.load(std::memory_order_acquire);
    if (!flag || !flag->load(std::memory_order_acquire)) return;
    check();
    if (!flag->load(std::memory_order_acquire)) return;
    if (const PauseAcknowledge acknowledge =
            g_pause_acknowledge.load(std::memory_order_acquire))
        acknowledge(g_pause_context.load(std::memory_order_acquire), true);
    while (flag->load(std::memory_order_acquire)) {
        check();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    check();
    if (const PauseAcknowledge acknowledge =
            g_pause_acknowledge.load(std::memory_order_acquire))
        acknowledge(g_pause_context.load(std::memory_order_acquire), false);
}

}  // namespace cancel
}  // namespace sfm
