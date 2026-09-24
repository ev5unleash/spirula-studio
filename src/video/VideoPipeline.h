#pragma once
// GPU video decoding. Frames stay in device memory until a caller requests pixels.

#include "nn/io/Image.h"
#include "video/CodecDecoder.h"
#include "video/Demuxer.h"

#include <memory>
#include <string>
#include <vector>

namespace nn {
namespace vk {
struct ContextOptions;
}  // namespace vk
}  // namespace nn

namespace video {

struct FrameHandle {
    int64_t index = -1;      // presentation order, from zero
    double  pts = 0.0;       // seconds
    int     slot = -1;       // picture-pool slot; < 0 when invalid
    bool valid() const { return slot >= 0; }
};

struct ConvertOpts {
    float scale = 1.0f;   // < 1 downscales; 1.0 is a straight copy
    int   rotate = 0;     // 0, 90, 180 or 270, clockwise
};

struct PreflightResult {
    enum class Status {
        Compatible, UnsupportedStream, InvalidDevice, InvalidInput, RuntimeFailure
    };
    Status status = Status::InvalidInput;
    std::string reason;
    std::string device_id;
    std::string device_name;
    int required_level = 0;   // HEVC level × 10, e.g. 60 == 6.0; 0 when inapplicable
    int supported_level = 0;
};

class VideoPipeline {
public:
    VideoPipeline();
    ~VideoPipeline();
    VideoPipeline(const VideoPipeline&) = delete;
    VideoPipeline& operator=(const VideoPipeline&) = delete;

    // Why Vulkan video decoding is unavailable here, or "" when it works.
    static std::string availability();

    // Preflight the selected track/device without creating video session resources.
    static PreflightResult preflight(const std::string& path, int track,
                                     const nn::vk::ContextOptions& selected_device);
    // `lookahead` sizes the picture pool. `admission` receives typed failures.
    bool open(const std::string& path, int track, int lookahead, std::string& error,
              PreflightResult* admission = nullptr);

    const std::vector<TrackInfo>& tracks() const;
    const TrackInfo&              track() const;
    const StreamFormat&           format() const;

    // `admission` receives typed late failures; an empty error still means end of stream.
    bool next(FrameHandle& out, std::string& error, PreflightResult* admission = nullptr);
    void release(FrameHandle& h);

    // To the sync sample at or before `index`, reporting the frame the next
    // next() hands back; false where the container carries no index, leaving
    // the pipeline as it was. Release every FrameHandle first.
    bool seek(int64_t index, int64_t& landed, std::string& error);

    // Records the sharpness reduction for a live frame. Values become readable
    // after flushSharpness(), which costs one queue sync for the whole batch.
    void  queueSharpness(const FrameHandle& h);
    void  flushSharpness();
    float sharpness(const FrameHandle& h) const;

    // A box-filtered grey copy of the frame at `w` x `h`, on the host. What
    // motion analysis reads: the GPU does the reduction, so a frame costs a
    // dispatch and a few tens of kilobytes over the bus rather than a download.
    bool toGray(const FrameHandle& h, int w, int h_out, std::vector<uint8_t>& out,
                std::string& error);

    // Converts to host RGB. Blocks until the frame is ready.
    bool toImage(const FrameHandle& h, const ConvertOpts& opts, nn::Image& out,
                 std::string& error);
    // Output geometry for `opts`, so a caller can size buffers up front.
    void outputSize(const ConvertOpts& opts, int& w, int& h) const;

    // Frames decoded so far, for the timing report.
    int64_t decodedCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace video
