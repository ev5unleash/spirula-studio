#pragma once

// FrameSelect -- motion-blur-aware frame selection for video datasets.
// C++ multithreaded port of reference/scripts/extract_frames.py's
// FrameSelector: the sharpness metric is the variance of the 3x3 Laplacian
// of the mean-subtracted 512x512 grayscale image; within each group of
// `group` consecutive candidate frames the sharpest one is kept.
//
// Callers extract candidates at (target fps x group), then keep the best per
// group; this matches plain fps output while choosing the least blurry frame.

#include <atomic>
#include <functional>
#include <string>

namespace app {

// Score sorted candidates in groups, move each sharpest frame to contiguous
// output names, and delete losers. Return kept count or -1 on error/cancel.
int select_sharpest_frames(const std::string& cand_dir,
                           const std::string& out_dir,
                           const std::string& prefix,
                           int group, int max_frames,
                           const std::function<void(const std::string&)>& log,
                           const std::atomic<bool>& cancel);

}  // namespace app
