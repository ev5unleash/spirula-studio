#pragma once

#include "config/TrainConfig.h"

#include <atomic>
#include <memory>
#include <string>

namespace app {

class ImageStripExecutor {
public:
    static std::string requested_device();
    static void check_config(const TrainConfig& cfg);
    ImageStripExecutor(const TrainConfig& cfg,
                       const std::atomic<bool>& controller_disconnected);
    ~ImageStripExecutor();
    ImageStripExecutor(const ImageStripExecutor&) = delete;
    ImageStripExecutor& operator=(const ImageStripExecutor&) = delete;

    void begin_step(int step);
    // Unwind only a decode failure before any prepared frame was dispatched.
    void abandon_undispatched_step();
    void finish_step();
    void require_checkpointable() const;
    void poison() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

int image_strip_worker_main(int argc, char** argv);

}  // namespace app
