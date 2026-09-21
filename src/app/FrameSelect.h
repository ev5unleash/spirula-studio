#pragma once

// FrameSelect -- which of the candidate frames ffmpeg extracted are kept.
//
// The sharpness metric is the variance of the 3x3 Laplacian of the
// mean-subtracted 512x512 grayscale image. Evenly, one per group of `group`
// candidates; or spaced by how much the view changes (app/FrameMotion.h),
// which is what the built-in decoder's adaptive mode does from the source
// frames themselves.

#include "app/FrameMotion.h"

#include <cstddef>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace app {

struct FrameSelectOutput {
    std::string path;
    uint64_t export_ordinal = 0;
};


struct FrameSelectChoice {
    int64_t index = -1;
    double score = -1.0;
    FrameSelectionKind kind = FrameSelectionKind::Rejected;
};

// Selects from [primary_begin, primary_end), then at most rescue_frames
// candidates immediately before it. `excluded` has `score_count` entries.
FrameSelectChoice choose_frame_candidate(
    const double* scores, size_t score_count, size_t primary_begin,
    size_t primary_end, int rescue_frames, const uint8_t* excluded,
    float minimum_sharpness);


struct FrameSelectOptions {
    int group = 1;          // candidates per kept frame
    int max_frames = 0;     // 0 = no cap
    float minimum_sharpness = 0.0f; // 0 disables a positive floor
    int rescue_frames = 0;          // earlier candidates to try
    // Adaptive: the rate stays within `range` either side of the average, and
    // the sharpest of `window` candidates around each chosen one is kept.
    bool adaptive = false;
    float range = 4.0f;
    int   window = 3;
    app::MotionView view = app::MotionView::Planar;
    // Set when the candidates are 360 canvases. An EAC one is measured on its
    // top row alone, which is half the sphere and all a rotation needs.
    app::Pano360Layout eac;
    float out_fov = 1.5708f;
    // (candidates scored, candidates in all), and the spacing settled on: the
    // candidate each kept frame ends at, out of that many. Both optional.
    std::function<void(int64_t, int64_t)> scanning;
    // ffmpeg exports cannot prove source discontinuities; the fallback reports
    // one explicit segment-0 span.
    std::function<void(const std::vector<FrameSegmentSpan>&)> spans;
    std::function<void(const FramePlan&, int64_t)> planned;
    // Each candidate's view change as it is measured, for a live curve:
    // the candidate it ends at, how many there are, and the change.
    std::function<void(const FramePosition&, int64_t, float)> measured;
    // Called for every planned candidate interval.
    std::function<void(const FrameSelectionDecision&)> decision;
    // Called after a selected candidate has reached its final path.
    std::function<void(const FrameSelectOutput&)> selected;
    // Synchronized selection callback with the output track identity.
    std::function<void(size_t, const FrameSelectOutput&)> selected_track;
};

// Picks the sharpest common ordinal across all tracks. Every score vector must
// describe the same candidate grid; a mismatch rejects the choice.
FrameSelectChoice choose_synchronized_candidate(
    const std::vector<const std::vector<double>*>& scores,
    size_t primary_begin, size_t primary_end, int rescue_frames,
    const uint8_t* excluded, float minimum_sharpness);

// Returns false instead of wrapping when a bounded candidate count overflows.
// A zero max_frames means no cap and yields cap == 0.
bool checked_candidate_cap(uint64_t max_frames, uint64_t candidate_group,
                           uint64_t& cap);

// Validates the adaptive range before FrameSelectionSettings::candidate_group()
// converts it to an int.
bool checked_candidate_group(const FrameSelectionSettings& settings, int& group);

// Reduces a known sampling rate when a known duration would exceed cap.
// A zero cap leaves the rate unchanged.
bool checked_candidate_rate(double base_rate, double duration, uint64_t cap,
                            double& rate);

struct FrameSelectTrack {
    std::string cand_dir;
    std::string out_dir;
    std::string prefix;
};

int select_synchronized_frames(
    const std::vector<FrameSelectTrack>& tracks,
    const FrameSelectOptions& options,
    const std::function<void(const std::string&)>& log,
    const std::atomic<bool>& cancel);



// Scores every image in cand_dir (sorted by filename) across all hardware
// threads and MOVES the keepers to out_dir as <prefix>NNNNN.<ext>, numbered by
// their place in cand_dir; the losers are deleted. -1 on error or cancellation.
int select_sharpest_frames(const std::string& cand_dir,
                           const std::string& out_dir,
                           const std::string& prefix,
                           const FrameSelectOptions& options,
                           const std::function<void(const std::string&)>& log,
                           const std::atomic<bool>& cancel);

}  // namespace app
