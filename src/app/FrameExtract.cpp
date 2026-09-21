// FrameExtract.cpp -- see app/FrameExtract.h.
//
// Lifted out of the `spirula-sam extract` CLI when the GUI grew the same need:
// the decode/select/mask/write loop is the part worth having exactly once, and
// the two front ends differ only in how they report progress.

#include "app/FrameExtract.h"

#include "app/FrameMotion.h"
#include "app/FrameSelect.h"
#include "app/WriterPool.h"
#include "data/ProjectManifest.h"
#include "i18n/catalog/Data.h"
#include "i18n/catalog/Log.h"
#include "nn/core/Log.h"
#include "nn/Device.h"
#include "nn/io/Image.h"
#include "sam/Sam.h"
#include "video/Demuxer.h"
#include "video/VideoPipeline.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <deque>
#include <functional>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <limits>
#include <vector>

namespace fs = std::filesystem;

namespace app {

namespace {

void log_line(const FrameExtractSinks& sinks, const std::string& s) {
    if (sinks.log) sinks.log(s);
    else NN_LOG_ERROR("%s\n", s.c_str());
}

// The encoder thread pool this depends on is app/WriterPool.h: masking and
// plain extraction both need it, so it does not live here.
using app::WriteJob;
using app::WriterPool;

// Which decoded frames are candidates and when a window is written. One copy,
// because the paired path has to choose the same frames as the single-track
// one and as the ffmpeg fallback.
struct SelectClock {
    // A fixed rate is arithmetic on the ordinal relative to the current
    // segment; adaptive plans name the exact segment and ordinal.
    int keep = 0, skip = 1, rescue = 0;
    FramePlan plan;
    size_t next = 0;
    bool have_segment = false;
    uint32_t segment = 0;
    int64_t segment_first = 0;

    int primary_window() const { return std::max(keep, 1); }
    int buffer_limit() const {
        return primary_window() + std::max(rescue, 0);
    }

    void observe(const FramePosition& position) {
        if (!have_segment || segment != position.segment) {
            have_segment = true;
            segment = position.segment;
            segment_first = position.ordinal;
        }
    }

    bool candidate(const FramePosition& position) {
        observe(position);
        if (!plan.empty()) {
            while (next < plan.size() && plan[next] < position) ++next;
            if (next >= plan.size() || plan[next].segment != position.segment)
                return false;
            const int64_t window = (int64_t)buffer_limit();
            return position.ordinal > plan[next].ordinal - window &&
                   position.ordinal <= plan[next].ordinal &&
                   position.ordinal >= segment_first;
        }
        const int64_t phase = position.ordinal - segment_first;
        if (keep == 0) {
            if (rescue <= 0) return phase % skip == 0;
            const int64_t rem = phase % skip;
            const int64_t target = phase + (rem == 0 ? 0 : skip - rem);
            const int64_t first = target - (int64_t)buffer_limit() + 1;
            return phase >= first && phase <= target;
        }
        const int width = buffer_limit();
        return ((phase + width) % skip) < width;
    }

    bool step(const FramePosition& position) {
        observe(position);
        if (!plan.empty())
            return next < plan.size() && plan[next] == position;
        const int64_t phase = position.ordinal - segment_first;
        return keep == 0 ? (rescue > 0 ? phase % skip == 0 : true)
                         : ((phase + 1) % skip) == 0;
    }
};

// ---------------------------------------------------------------------------
// Adaptive selection
// ---------------------------------------------------------------------------

// Pass one of an adaptive run: ONE track, reduced on the GPU so a frame costs
// a few tens of kilobytes over the bus. Decoding twice is cheaper than holding
// a video's worth of pictures until the plan is known.
bool measure_motion(const FrameExtractJob& o, const FrameExtractSinks& sinks,
                    int track, int tracks, MotionPlanInput& out,
                    FrameExtractStats& t, std::string& error) {
    const double t_start = nn::now_ms();
    video::VideoPipeline pipe;
    if (!pipe.open(o.input, track, 1, error)) return false;
    const video::TrackInfo& info = pipe.track();

    MotionOptions mo;
    // A capture that sees the whole sphere: the 360 packing, or the two square
    // tracks every dual-fisheye camera writes.
    if (o.eac.valid()) mo.view = MotionView::Packed360;
    else if (tracks >= 2 && info.width == info.height)
        mo.view = MotionView::Fisheye;
    mo.eac = o.eac;
    mo.out_fov = mo.view == MotionView::Fisheye ? mo.circle_fov
                                                 : motion_out_fov(o.views);
    motion_frame_size(mo.view, info.width, info.height, mo.width, mo.height);

    MotionTracker tracker(mo);
    const double fps = info.fps > 1.0 ? info.fps : 30.0;
    // 15 samples a second is enough to follow a walk, but never coarser than a
    // third of the spacing being planned: a gap can only land on a sample.
    const int stride = std::max(
        1, std::min((int)std::lround(fps / 15.0), std::max(1, o.skip / 3)));

    // Tracking is CPU and decoding is GPU, so they run at the same time: the
    // decoder fills a short queue and one consumer thread owns the tracker.
    struct Sample {
        std::vector<uint8_t> gray;
        FramePosition position;
    };
    std::deque<Sample> queue;
    std::mutex mu;
    std::condition_variable room, work;
    bool feeding = true;
    size_t reported = 0;
    std::thread consumer([&] {
        for (;;) {
            Sample s;
            {
                std::unique_lock<std::mutex> lk(mu);
                work.wait(lk, [&] { return !queue.empty() || !feeding; });
                if (queue.empty()) return;
                s = std::move(queue.front());
                queue.pop_front();
            }
            room.notify_one();
            tracker.track(s.gray.data(), s.position);
            // Read on the thread that appended them, so the vectors need no
            // lock of their own; finish() may still revise what is already out.
            if (sinks.measured)
                for (; reported < tracker.steps().size(); reported++)
                    sinks.measured(tracker.steps()[reported].end,
                                   info.frame_count,
                                   tracker.steps()[reported].cost);
        }
    });
    auto stop = [&]() {
        {
            std::lock_guard<std::mutex> lk(mu);
            feeding = false;
        }
        work.notify_all();
        consumer.join();
    };

    std::vector<uint8_t> gray;
    std::vector<FrameSegmentSpan> spans;
    uint32_t sampled_segment = 0;
    int64_t sample_origin = 0;
    bool have_sample_origin = false;
    int64_t decoded = 0;
    for (;;) {
        if (sinks.cancel && sinks.cancel->load()) {
            error = "cancelled";
            stop();
            return false;
        }
        video::FrameHandle h;
        if (!pipe.next(h, error)) {
            if (!error.empty()) {
                stop();
                return false;
            }
            break;
        }
        if (h.timing.presentation_ordinal == video::kUnknownOrdinal ||
            h.timing.presentation_ordinal >
                (uint64_t)std::numeric_limits<int64_t>::max()) {
            pipe.release(h);
            continue;
        }
        const FramePosition position{
            h.timing.discontinuity_segment,
            (int64_t)h.timing.presentation_ordinal};
        ++decoded;
        if (spans.empty() || spans.back().segment != position.segment)
            spans.push_back({position.segment, position.ordinal, position.ordinal});
        else
            spans.back().last_ordinal = position.ordinal;
        if (!have_sample_origin || sampled_segment != position.segment) {
            sampled_segment = position.segment;
            sample_origin = position.ordinal;
            have_sample_origin = true;
        }
        if ((position.ordinal - sample_origin) % stride == 0) {
            if (!pipe.toGray(h, mo.width, mo.height, gray, error)) {
                pipe.release(h);
                stop();
                return false;
            }
            std::unique_lock<std::mutex> lk(mu);
            room.wait(lk, [&] { return queue.size() < 3; });
            queue.push_back({gray, position});
            lk.unlock();
            work.notify_one();
            ++t.analyzed;
            if (sinks.scanning)
                sinks.scanning(decoded, info.frame_count);
        }
        pipe.release(h);
    }
    stop();
    tracker.finish();
    out.spans = std::move(spans);
    out.steps = tracker.steps();
    if (sinks.spans) sinks.spans(out.spans);
    int64_t observed = 0;
    for (const FrameSegmentSpan& span : out.spans)
        observed += std::max<int64_t>(0, span.last_ordinal - span.first_ordinal + 1);
    out.frames = info.frame_count > 0 ? info.frame_count : observed;
    out.skip = o.skip;
    out.window = std::max(o.keep, 1);
    out.max_frames = o.max_frames;
    out.fps = fps;
    t.plan = nn::now_ms() - t_start;
    return true;
}

}  // namespace

// What the plan came out as, for the log and for whatever is drawing it.
void report_plan(const FrameExtractSinks& sinks, const FramePlan& plan,
                 int64_t frames, double fps) {
    if (plan.empty()) return;
    int64_t tightest = frames, widest = 0;
    for (size_t i = 1; i < plan.size(); i++) {
        if (plan[i - 1].segment != plan[i].segment) continue;
        const int64_t gap = plan[i].ordinal - plan[i - 1].ordinal;
        tightest = std::min(tightest, gap);
        widest = std::max(widest, gap);
    }
    auto rate = [&](int64_t gap) {
        return std::round((gap > 0 ? fps / (double)gap : fps) * 100.0) / 100.0;
    };
    namespace lmsg = spirula::i18n::msg::log;
    log_line(sinks,
             spirula::i18n::format(lmsg::motion_plan, {(long long)plan.size(),
                                                       rate(widest), rate(tightest)}));
    if (sinks.planned) sinks.planned(plan, frames);
}

bool scan_motion(const FrameExtractJob& job, const FrameExtractSinks& sinks,
                 MotionPlanInput& out, FrameExtractStats& stats,
                 std::string& error) {
    const int n = video_track_count(job.input, error);
    if (n <= 0) {
        if (error.empty()) error = "no video track in " + job.input;
        return false;
    }
    stats.tracks = n;
    return measure_motion(job, sinks, job.track >= 0 ? job.track : 0, n, out,
                          stats, error);
}

namespace {

// ---------------------------------------------------------------------------
// Extraction
// ---------------------------------------------------------------------------
bool extract_track(const FrameExtractJob& o, const FrameExtractSinks& sinks,
                   int track, const fs::path& image_dir,
                   const fs::path& mask_dir, sam::Masker* masker,
                   WriterPool& pool, const FramePlan& plan,
                   FrameExtractStats& t, std::string& error) {
    const int keep = o.keep;
    const int primary_window = std::max(keep, 1);
    const int rescue_frames = std::max(o.rescue_frames, 0);
    const int buffer_limit = primary_window + rescue_frames;
    video::VideoPipeline pipe;
    // The decoder pool must hold the primary window and its bounded rescue
    // candidates, never pictures from an earlier discontinuity segment.
    if (!pipe.open(o.input, track, buffer_limit, error)) return false;

    fs::create_directories(image_dir);
    if (masker) fs::create_directories(mask_dir);

    video::ConvertOpts conv;
    conv.scale = o.scale;
    conv.rotate = o.rotate;

    struct Buffered {
        video::FrameHandle h;
    };
    std::deque<Buffered> window;
    SelectClock clock{o.keep, o.skip, rescue_frames, plan, 0};
    const bool measure_scores =
        keep != 0 || o.minimum_sharpness > 0.0f || rescue_frames > 0;
    std::vector<double> scores;
    std::vector<uint8_t> excluded;
    scores.reserve((size_t)buffer_limit);
    excluded.reserve((size_t)buffer_limit);
    int64_t written = 0;
    bool measured_pending = false;

    auto release_window = [&]() {
        for (auto& b : window) pipe.release(b.h);
        window.clear();
    };
    auto discard_window = [&]() {
        if (window.empty()) return;
        if (measured_pending) {
            const double t0 = nn::now_ms();
            pipe.flushSharpness();
            t.sharpness += nn::now_ms() - t0;
            measured_pending = false;
        }
        release_window();
    };

    auto flush_window = [&](bool write, FramePosition planned) -> bool {
        if (window.empty()) return true;
        if (!write) {
            discard_window();
            return true;
        }
        if (measured_pending) {
            const double t0 = nn::now_ms();
            pipe.flushSharpness();
            t.sharpness += nn::now_ms() - t0;
            measured_pending = false;
        }
        scores.resize(window.size());
        excluded.resize(window.size());
        std::fill(excluded.begin(), excluded.end(), uint8_t(0));
        for (size_t i = 0; i < window.size(); ++i)
            scores[i] = measure_scores
                            ? (double)pipe.sharpness(window[i].h)
                            : 0.0;

        // The newest primary_window candidates are preferred. The chooser
        // scans the bounded prefix immediately before them only when every
        // primary score is rejected by the floor or is not finite.
        const size_t primary_begin =
            window.size() > (size_t)primary_window
                ? window.size() - (size_t)primary_window : 0;
        for (;;) {
            const FrameSelectChoice choice = choose_frame_candidate(
                scores.data(), scores.size(), primary_begin, scores.size(),
                rescue_frames, excluded.data(), o.minimum_sharpness);
            if (choice.index < 0) {
                ++t.rejected;
                FrameSelectionDecision decision;
                decision.planned = planned;
                decision.score = choice.score;
                decision.kind = FrameSelectionKind::Rejected;
                if (sinks.decision) sinks.decision(decision);
                release_window();
                return true;
            }
            const size_t selected = (size_t)choice.index;
            excluded[selected] = 1;
            const video::FrameHandle& chosen = window[selected].h;
            const uint64_t ordinal = chosen.timing.presentation_ordinal;
            if (ordinal == video::kUnknownOrdinal ||
                ordinal > (uint64_t)std::numeric_limits<int64_t>::max()) {
                error = "native decoder cannot prove the selected frame's source ordinal";
                release_window();
                return false;
            }
            const int64_t index = (int64_t)ordinal;
            const FramePosition selected_position{
                chosen.timing.discontinuity_segment, index};
            if (selected_position.segment != planned.segment) {
                ++t.rejected;
                FrameSelectionDecision decision;
                decision.planned = planned;
                decision.score = choice.score;
                decision.kind = FrameSelectionKind::Rejected;
                if (sinks.decision) sinks.decision(decision);
                release_window();
                return true;
            }

            nn::Image image;
            const double t0 = nn::now_ms();
            std::string err;
            if (!pipe.toImage(chosen, conv, image, err)) {
                t.convert += nn::now_ms() - t0;
                log_line(sinks, "frame " + std::to_string(index) + ": " + err);
                // A failed conversion consumes only this candidate. The next
                // eligible score in the same segment may still satisfy the
                // interval, including a bounded rescue candidate.
                continue;
            }
            t.convert += nn::now_ms() - t0;

            char stem[64];
            std::snprintf(stem, sizeof(stem), "%05llu",
                          (unsigned long long)ordinal);
            if (masker) {
                const double mask_start = nn::now_ms();
                sam::Mask mask;
                sam::Result overlay;
                // The decoded index, not the written one: a click was drawn
                // on a frame of the video, and only one frame per sharpness
                // window survives to be written.
                if (masker->run(image, mask, o.write_overlay ? &overlay : nullptr,
                                index)) {
                    t.mask += nn::now_ms() - mask_start;
                    WriteJob mj;
                    mj.mask = std::move(mask);
                    mj.path = (mask_dir / (std::string(stem) + ".png")).string();
                    pool.submit(std::move(mj));
                    if (o.write_overlay) {
                        sam::save_overlay_png(
                            image, overlay,
                            (mask_dir / (std::string(stem) + "_overlay.png")).string());
                    }
                } else {
                    log_line(sinks, "frame " + std::to_string(index) +
                                            ": masking failed: " + masker->lastError());
                }
            }

            const bool jpeg = o.quality >= 0 && o.quality <= 100;
            WriteJob job;
            job.path = (image_dir / (std::string(stem) + (jpeg ? ".jpg" : ".png"))).string();
            if (sinks.preview && image.channels == 3)
                sinks.preview(image.data.data(), image.width, image.height,
                              job.path, chosen.timing);
            if (sinks.written) sinks.written(job.path, chosen.timing);
            job.quality = o.quality;
            job.image = std::move(image);
            const double submit_start = nn::now_ms();
            pool.submit(std::move(job));
            t.submit += nn::now_ms() - submit_start;
            ++written;
            ++t.written;
            if (choice.kind == FrameSelectionKind::Rescued)
                ++t.rescued;
            else
                ++t.accepted;
            FrameSelectionDecision decision;
            decision.planned = planned;
            decision.selected = selected_position;
            decision.has_selected = true;
            decision.score = choice.score;
            decision.kind = choice.kind;
            if (sinks.decision) sinks.decision(decision);
            if (sinks.progress) sinks.progress(t.written, t.decoded);
            release_window();
            return true;
        }
    };

    while (o.max_frames <= 0 || written < o.max_frames) {
        if (sinks.cancel && sinks.cancel->load()) {
            error = "cancelled";
            release_window();
            return false;
        }
        video::FrameHandle h;
        const double t0 = nn::now_ms();
        if (!pipe.next(h, error)) {
            t.decode += nn::now_ms() - t0;
            if (!error.empty()) {
                release_window();
                return false;
            }
            break;  // end of stream
        }
        t.decode += nn::now_ms() - t0;
        ++t.decoded;

        if (h.timing.presentation_ordinal == video::kUnknownOrdinal ||
            h.timing.presentation_ordinal >
                (uint64_t)std::numeric_limits<int64_t>::max()) {
            pipe.release(h);
            continue;
        }
        const FramePosition position{
            h.timing.discontinuity_segment,
            (int64_t)h.timing.presentation_ordinal};
        if (!window.empty() &&
            window.front().h.timing.discontinuity_segment != position.segment)
            discard_window();
        // A frame matters only if some write window can select it. Everything
        // else is decoded (inter prediction needs it) but never touched again.
        if (!clock.candidate(position)) {
            pipe.release(h);
            continue;
        }

        if (measure_scores) {
            const double t1 = nn::now_ms();
            pipe.queueSharpness(h);
            t.sharpness += nn::now_ms() - t1;
            measured_pending = true;
            ++t.measured;
        }
        window.push_back({h});
        if ((int)window.size() > buffer_limit) {
            pipe.release(window.front().h);
            window.pop_front();
        }

        if (clock.step(position) && !flush_window(true, position))
            return false;
    }
    discard_window();
    return error.empty();
}

// Several tracks decoded in lockstep under one sharpness window (the score is
// the worst finite track score); `on_frame` gets the chosen instant's
// pictures, one per track, and writes whatever the caller wants out of them.
using LockstepSink =
    std::function<bool(std::vector<nn::Image>& imgs,
                       const std::vector<video::FrameTiming>& timing,
                       std::string& err)>;

video::VideoPipeline::SharpnessRegions score_regions(const Pano360Layout& layout,
                                                     int track) {
    const Pano360ScoreRegions source = pano360_score_regions(layout, track);
    video::VideoPipeline::SharpnessRegions out{};
    out.count = source.count;
    out.packed_width = source.packed_width;
    out.packed_height = source.packed_height;
    for (int i = 0; i < source.count && i < video::VideoPipeline::kMaxSharpnessRegions;
         i++) {
        out.regions[i] = {source.regions[i].x, source.regions[i].y,
                          source.regions[i].width, source.regions[i].height};
    }
    return out;
}

bool extract_lockstep(const FrameExtractJob& o, const FrameExtractSinks& sinks,
                      const std::vector<int>& tracks, const video::ConvertOpts& conv,
                      const FramePlan& plan,
                      const video::VideoPipeline::SharpnessRegions* score_regions,
                      FrameExtractStats& t, std::string& error,
                      const LockstepSink& on_frame) {

    const size_t n = tracks.size();
    const int keep = o.keep;
    const int primary_window = std::max(keep, 1);
    const int rescue_frames = std::max(o.rescue_frames, 0);
    const int buffer_limit = primary_window + rescue_frames;
    std::vector<std::unique_ptr<video::VideoPipeline>> pipe;
    for (size_t k = 0; k < n; k++) {
        pipe.push_back(std::make_unique<video::VideoPipeline>());
        if (!pipe[k]->open(o.input, tracks[k], buffer_limit, error)) return false;
    }
    struct Buffered {
        std::vector<video::FrameHandle> h;
        FramePosition position;
    };
    std::deque<Buffered> window;
    SelectClock clock{o.keep, o.skip, rescue_frames, plan, 0};
    // Lockstep policy always evaluates all tracks: a non-finite or
    // below-floor track must reject the synchronized instant even when the
    // sharpness window itself is one frame.
    const bool measure_scores = true;
    std::vector<double> scores;
    std::vector<uint8_t> excluded;
    std::vector<video::FrameTiming> timing;
    std::vector<nn::Image> imgs(n);
    scores.reserve((size_t)buffer_limit);
    excluded.reserve((size_t)buffer_limit);
    timing.reserve(n);
    int64_t written = 0;
    bool measured_pending = false;

    auto release = [&](Buffered& b) {
        for (size_t k = 0; k < n; k++) pipe[k]->release(b.h[k]);
    };
    auto drain = [&]() {
        for (auto& b : window) release(b);
        window.clear();
    };

    auto flush_window = [&](bool write, FramePosition planned) -> bool {
        if (window.empty()) return true;
        if (!write) {
            if (measured_pending) {
                const double t0 = nn::now_ms();
                for (size_t k = 0; k < n; k++) pipe[k]->flushSharpness();
                t.sharpness += nn::now_ms() - t0;
                measured_pending = false;
            }
            drain();
            return true;
        }
        if (measured_pending) {
            const double t0 = nn::now_ms();
            for (size_t k = 0; k < n; k++) pipe[k]->flushSharpness();
            t.sharpness += nn::now_ms() - t0;
            measured_pending = false;
        }

        scores.resize(window.size());
        excluded.resize(window.size());
        std::fill(excluded.begin(), excluded.end(), uint8_t(0));
        for (size_t i = 0; i < window.size(); ++i) {
            if (!measure_scores) {
                scores[i] = 0.0;
                continue;
            }
            double worst = std::numeric_limits<double>::infinity();
            bool eligible = true;
            for (size_t k = 0; k < n; k++) {
                const double score = (double)pipe[k]->sharpness(window[i].h[k]);
                // A synchronized instant is valid only when every track has
                // a finite, non-negative score. The chooser then applies the
                // requested floor to this worst-track score.
                if (!std::isfinite(score) || score < 0.0) {
                    eligible = false;
                    break;
                }
                worst = std::min(worst, score);
            }
            scores[i] = eligible ? worst
                                 : std::numeric_limits<double>::quiet_NaN();
        }
        const size_t primary_begin =
            window.size() > (size_t)primary_window
                ? window.size() - (size_t)primary_window : 0;
        for (;;) {
            const FrameSelectChoice choice = choose_frame_candidate(
                scores.data(), scores.size(), primary_begin, scores.size(),
                rescue_frames, excluded.data(), o.minimum_sharpness);
            if (choice.index < 0) {
                ++t.rejected;
                FrameSelectionDecision decision;
                decision.planned = planned;
                decision.score = choice.score;
                decision.kind = FrameSelectionKind::Rejected;
                if (sinks.decision) sinks.decision(decision);
                drain();
                return true;
            }
            const size_t selected = (size_t)choice.index;
            excluded[selected] = 1;
            const Buffered& chosen = window[selected];
            timing.clear();
            for (const video::FrameHandle& h : chosen.h) timing.push_back(h.timing);

            const double t0 = nn::now_ms();
            std::string err;
            bool converted = true;
            for (size_t k = 0; k < n && converted; k++)
                converted = pipe[k]->toImage(chosen.h[k], conv, imgs[k], err);
            t.convert += nn::now_ms() - t0;
            if (!converted) {
                log_line(sinks, "frame " + std::to_string(chosen.position.ordinal) +
                                     ": " + err);
                // Keep the interval alive and try the next eligible instant.
                continue;
            }

            if (!on_frame(imgs, timing, err)) {
                ++t.rejected;
                FrameSelectionDecision decision;
                decision.planned = planned;
                decision.selected = chosen.position;
                decision.has_selected = true;
                decision.score = choice.score;
                decision.kind = FrameSelectionKind::Rejected;
                if (sinks.decision) sinks.decision(decision);
                log_line(sinks, "frame " + std::to_string(chosen.position.ordinal) +
                                     ": " + err);
                drain();
                return true;
            }
            ++written;
            if (choice.kind == FrameSelectionKind::Rescued)
                ++t.rescued;
            else
                ++t.accepted;
            FrameSelectionDecision decision;
            decision.planned = planned;
            decision.selected = chosen.position;
            decision.has_selected = true;
            decision.score = choice.score;
            decision.kind = choice.kind;
            if (sinks.decision) sinks.decision(decision);
            if (sinks.progress) sinks.progress(t.written, t.decoded);
            drain();
            return true;
        }
    };
    while (o.max_frames <= 0 || written < o.max_frames) {
        if (sinks.cancel && sinks.cancel->load()) {
            error = "cancelled";
            drain();
            return false;
        }
        Buffered b;
        b.h.resize(n);
        const double t0 = nn::now_ms();
        bool got = true;
        for (size_t k = 0; k < n && got; k++) got = pipe[k]->next(b.h[k], error);
        t.decode += nn::now_ms() - t0;
        if (!got) {
            // One track ending first leaves the others' pictures held; release
            // them so a future caller can reopen without a full pool.
            for (size_t k = 0; k < n; k++)
                if (b.h[k].valid()) pipe[k]->release(b.h[k]);
            drain();
            return error.empty();   // end of stream
        }
        ++t.decoded;

        const video::FrameTiming& reference = b.h[0].timing;
        if (reference.presentation_ordinal == video::kUnknownOrdinal ||
            reference.presentation_ordinal >
                (uint64_t)std::numeric_limits<int64_t>::max()) {
            error = "native decoder cannot prove synchronized frame identity";
            release(b);
            drain();
            return false;
        }
        for (size_t k = 1; k < n; k++) {
            const video::FrameTiming& other = b.h[k].timing;
            if (other.presentation_ordinal != reference.presentation_ordinal ||
                other.discontinuity_segment != reference.discontinuity_segment) {
                error = "native decoder tracks disagree on frame identity";
                release(b);
                drain();
                return false;
            }
        }
        b.position = FramePosition{
            reference.discontinuity_segment,
            (int64_t)reference.presentation_ordinal};
        const FramePosition position = b.position;
        if (!window.empty() &&
            window.front().h[0].timing.discontinuity_segment != position.segment)
            flush_window(false, {});
        if (!clock.candidate(position)) {
            release(b);
            continue;
        }
        if (measure_scores) {
            const double t1 = nn::now_ms();
            for (size_t k = 0; k < n; k++) {
                if (score_regions)
                    pipe[k]->queueSharpness(b.h[k], score_regions[k]);
                else
                    pipe[k]->queueSharpness(b.h[k]);
            }
            t.sharpness += nn::now_ms() - t1;
            measured_pending = true;
            ++t.measured;
        }
        window.push_back(std::move(b));
        if ((int)window.size() > buffer_limit) {
            release(window.front());
            window.pop_front();
        }
        if (clock.step(position) && !flush_window(true, position))
            return false;
    }
    flush_window(false, {});
    return true;
}

// A 360 file's tracks, laid out as one canvas and resampled into every view.
bool extract_pair(const FrameExtractJob& o, const FrameExtractSinks& sinks,
                  const fs::path& image_dir, WriterPool& pool,
                  const FramePlan& plan, FrameExtractStats& t,
                  std::string& error) {
    const std::vector<int> want = pano360_needs_track1(o.eac)
                                      ? std::vector<int>{0, 1}
                                      : std::vector<int>{0};
    {
        std::vector<std::pair<int, int>> sizes = video_track_sizes(o.input, error);
        for (size_t k = 0; k < want.size(); k++) {
            if (k < sizes.size() && sizes[k].first == o.eac.track_w &&
                sizes[k].second == o.eac.track_h)
                continue;
            error = "track " + std::to_string(k) + " is not the size the 360 layout"
                    " was detected at";
            return false;
        }
    }
    video::VideoPipeline::SharpnessRegions sharp_regions[2]{};
    for (size_t k = 0; k < want.size(); k++)
        sharp_regions[k] = score_regions(o.eac, (int)k);
    std::vector<Pano360Remap> maps(o.views.size());
    for (size_t i = 0; i < o.views.size(); i++) {
        pano360_remap(o.eac, o.views[i], maps[i]);
        fs::create_directories(image_dir / o.views[i].dir);
    }
    std::vector<uint8_t> canvas((size_t)o.eac.canvasW() * o.eac.canvasH() * 3);
    const bool jpeg = o.quality >= 0 && o.quality <= 100;
    auto on_frame = [&](std::vector<nn::Image>& track,
                        const std::vector<video::FrameTiming>& timing,
                        std::string&) {
        double t0 = nn::now_ms();
        pano360_canvas(o.eac, track[0].data.data(),
                       track.size() > 1 ? track[1].data.data() : nullptr,
                       canvas.data());
        t.convert += nn::now_ms() - t0;
        if (timing.empty() || timing[0].presentation_ordinal == video::kUnknownOrdinal)
            return false;
        const uint64_t ordinal = timing[0].presentation_ordinal;
        char stem[64];
        std::snprintf(stem, sizeof(stem), "%05llu", (unsigned long long)ordinal);
        for (size_t i = 0; i < o.views.size(); i++) {
            t0 = nn::now_ms();
            WriteJob job;
            job.image.width = maps[i].width;
            job.image.height = maps[i].height;
            job.image.channels = 3;
            job.image.data.resize((size_t)maps[i].width * maps[i].height * 3);
            pano360_apply(maps[i], canvas.data(), o.eac.canvasW(), o.eac.canvasH(), 0,
                          job.image.data.data());
            t.convert += nn::now_ms() - t0;
            job.path = (image_dir / o.views[i].dir /
                        (std::string(stem) + (jpeg ? ".jpg" : ".png"))).string();
            job.quality = o.quality;
            // The reel shows the first view; the others are the same instant
            // seen the other way, and six thumbnails a frame is not what the
            // slider is for.
            if (i == 0 && sinks.preview)
                sinks.preview(job.image.data.data(), job.image.width, job.image.height,
                              job.path, timing[0]);
            if (sinks.written) sinks.written(job.path, timing[0]);
            t0 = nn::now_ms();
            pool.submit(std::move(job));
            t.submit += nn::now_ms() - t0;
            // Per view, not per frame: the count is what the progress bar is
            // scaled against, and a plan writes six files here.
            ++t.written;
        }
        return true;
    };
    return extract_lockstep(o, sinks, want, video::ConvertOpts{}, plan, sharp_regions, t,
                            error, on_frame);
}

// Every track of a multi-lens file at the same instants, one folder per
// track: the rig the frames of one stem form is then a rig in fact.
bool extract_synced(const FrameExtractJob& o, const FrameExtractSinks& sinks,
                    const std::vector<int>& tracks, const fs::path& base, WriterPool& pool,
                    const FramePlan& plan, FrameExtractStats& t,
                    std::string& error) {
    std::vector<fs::path> dirs;
    for (size_t k = 0; k < tracks.size(); k++) {
        dirs.push_back(base / ("cam" + std::to_string(k)));
        fs::create_directories(dirs.back());
        log_line(sinks, "track " + std::to_string(tracks[k]) + " -> " + dirs.back().string());
    }
    video::ConvertOpts conv;
    conv.scale = o.scale;
    conv.rotate = o.rotate;
    const bool jpeg = o.quality >= 0 && o.quality <= 100;
    auto on_frame = [&](std::vector<nn::Image>& imgs,
                        const std::vector<video::FrameTiming>& timing,
                        std::string&) {
        if (timing.empty() || timing[0].presentation_ordinal == video::kUnknownOrdinal)
            return false;
        const uint64_t ordinal = timing[0].presentation_ordinal;
        char stem[64];
        std::snprintf(stem, sizeof(stem), "%05llu", (unsigned long long)ordinal);
        for (size_t k = 0; k < imgs.size(); k++) {
            WriteJob job;
            job.path = (dirs[k] / (std::string(stem) + (jpeg ? ".jpg" : ".png"))).string();
            if (k == 0 && sinks.preview && imgs[k].channels == 3)
                sinks.preview(imgs[k].data.data(), imgs[k].width, imgs[k].height,
                              job.path, timing[k]);
            if (sinks.written) sinks.written(job.path, timing[k]);
            job.quality = o.quality;
            job.image = std::move(imgs[k]);
            const double t0 = nn::now_ms();
            pool.submit(std::move(job));
            t.submit += nn::now_ms() - t0;
            ++t.written;
        }
        return true;
    };
    return extract_lockstep(o, sinks, tracks, conv, plan, nullptr, t, error, on_frame);
}

}  // namespace

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

std::string video_decode_availability() {
    return video::VideoPipeline::availability();
}

int video_track_count(const std::string& path, std::string& error) {
    // The demuxer alone: creating a video session per track just to count them
    // would be several hundred megabytes of DPB for nothing.
    auto demux = video::open_demuxer(path, error);
    if (!demux) return 0;
    return (int)demux->tracks().size();
}

std::vector<std::pair<int, int>> video_track_sizes(const std::string& path,
                                                   std::string& error) {
    std::vector<std::pair<int, int>> out;
    auto demux = video::open_demuxer(path, error);
    if (!demux) return out;
    for (const video::TrackInfo& t : demux->tracks())
        out.emplace_back(t.width, t.height);
    return out;
}

std::vector<sfm::ExifTransform> video_track_turns(const std::string& path,
                                                  std::string& error) {
    std::vector<sfm::ExifTransform> out;
    auto demux = video::open_demuxer(path, error);
    if (!demux) return out;
    for (const video::TrackInfo& t : demux->tracks())
        out.push_back({((t.rotate % 360) + 360) % 360 / 90, t.mirror});
    return out;
}

sfm::ExifTransform fold_auto_rotate(const std::string& path,
                                    const std::vector<int>& tracks,
                                    FrameLook& look, bool& mixed) {
    mixed = false;
    if (!look.auto_rotate || look.pano()) return {};
    std::string probe_error;
    const sfm::ExifTransform t =
        merge_turns(video_track_turns(path, probe_error), tracks, mixed);
    look.rotate = (look.rotate + 90 * t.turns_cw) % 360;
    return t;
}

bool extract_frames(const FrameExtractJob& job_in, const FrameExtractSinks& sinks,
                    FrameExtractStats& stats, std::string& error) {
    FrameExtractJob job = job_in;
    if (job.skip < 1) job.skip = 1;
    if (job.keep < 0) job.keep = (int)(0.5 * job.skip + 0.5);
    if (!std::isfinite(job.minimum_sharpness) || job.minimum_sharpness < 0.0f)
        job.minimum_sharpness = 0.0f;
    job.rescue_frames = std::clamp(job.rescue_frames, 0,
                                   FrameSelectionSettings::kMaxRescueFrames);
    if (job.rotate % 90 != 0) {
        error = "rotation must be a multiple of 90 degrees";
        return false;
    }

    // Before the probe below, which creates the device and reports the video
    // queue family this job then decodes on. Everything after -- decoder,
    // Masker, Tracker -- inherits that identity.
    if (!sam::freeze_device(job.device.empty() ? job.mask.device : job.device,
                            error))
        return false;
    // Past this point the canonical UUID is what crosses into the decoder and
    // the masker; an ordinal only ever meant something in the resolution that
    // consumed it.
    job.device = nn::configured_device_selector();
    job.mask.device = job.device;

    if (job.source_id.empty()) {
        try {
            job.source_id = spirula::project::make_source_record(job.input).source_id;
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
    }
    stats.source_id = job.source_id;
    std::unique_ptr<video::Demuxer> metadata_demux =
        video::open_demuxer(job.input, error);
    if (!metadata_demux) return false;
    stats.streams = metadata_demux->tracks();

    const bool pano = job.eac.valid() && !job.views.empty();
    if (pano) {
        // The layout is measured in source pixels and the sphere is turned by
        // the plan's own yaw/pitch/roll, so neither of these can be honoured
        // quietly.
        if (job.scale != 1.0f || job.rotate != 0) {
            error = "--scale and --rotate do not apply to a 360 capture";
            return false;
        }
        // One masker cannot track six views at once: its memory bank is keyed
        // by frame, and six directions per frame are six videos to it.
        if (!job.mask.model.empty()) {
            error = "masking during extraction does not support a 360 capture;"
                    " mask the extracted frames instead";
            return false;
        }
    }
    if (job.sync_tracks && !job.mask.model.empty()) {
        error = "masking during extraction does not support synchronized tracks;"
                " mask the extracted frames instead";
        return false;
    }

    std::vector<int> tracks;
    if (job.track >= 0) {
        tracks.push_back(job.track);
    } else {
        const int n = video_track_count(job.input, error);
        if (n <= 0) {
            if (error.empty()) error = "no video track in " + job.input;
            return false;
        }
        for (int i = 0; i < n; ++i) tracks.push_back(i);
    }
    stats.tracks = (int)tracks.size();

    {
        namespace lmsg = spirula::i18n::msg::log;
        bool mixed = false;
        const sfm::ExifTransform turn =
            fold_auto_rotate(job.input, tracks, job, mixed);
        if (turn.turns_cw > 0) {
            log_line(sinks, spirula::i18n::format(
                                lmsg::video_autorotate,
                                {(long long)(90 * turn.turns_cw)}));
            if (mixed) log_line(sinks, lmsg::video_autorotate_mixed.get());
        }
        if (turn.mirror) log_line(sinks, lmsg::video_autorotate_mirror.get());
    }

    std::unique_ptr<sam::Masker> masker;
    if (!job.mask.model.empty()) {
        masker = std::make_unique<sam::Masker>();
        if (!masker->init(job.mask, error)) return false;
    }

    stats.encoder_threads =
        job.threads > 0 ? job.threads
                        : std::max(1, (int)std::thread::hardware_concurrency() - 1);
    WriterPool pool(stats.encoder_threads);

    const fs::path base(job.image_dir);
    const fs::path mask_base(job.mask_dir.empty() ? (base.parent_path() / "masks")
                                                  : fs::path(job.mask_dir));

    const double t_start = nn::now_ms();
    bool ok = true;
    const bool synced = !pano && job.sync_tracks && tracks.size() > 1;
    // One plan for the whole file, measured on its first track: the tracks of
    // a rig see the same motion, and the ones that do not are still one camera
    // moving through one scene.
    FramePlan plan = job.plan;
    if (job.adaptive && plan.empty()) {
        MotionPlanInput mi;
        if (!measure_motion(job, sinks, tracks[0], (int)tracks.size(), mi, stats,
                            error))
            return false;
        const std::vector<FramePlan> got =
            plan_by_motion({mi}, job.adaptive_range);
        if (!got.empty()) plan = got[0];
        if (plan.empty()) {
            error = "the capture is too short to space frames by motion";
            return false;
        }
        report_plan(sinks, plan, mi.frames, mi.fps);
    }
    if (pano) {
        stats.tracks = 2;
        ok = extract_pair(job, sinks, base, pool, plan, stats, error);
    } else if (synced) {
        ok = extract_synced(job, sinks, tracks, base, pool, plan, stats, error);
    }
    for (size_t ti = 0; ti < tracks.size() && ok && !pano && !synced; ++ti) {
        // A multi-track file (an Insta360 .insv carries two fisheye streams)
        // becomes cam0/, cam1/, ... -- one camera per folder downstream.
        const bool multi = tracks.size() > 1;
        const fs::path image_dir = multi ? base / ("cam" + std::to_string(ti)) : base;
        const fs::path mask_dir =
            multi ? mask_base / ("cam" + std::to_string(ti)) : mask_base;
        if (multi)
            log_line(sinks, "track " + std::to_string(tracks[ti]) + " -> " +
                                image_dir.string());
        ok = extract_track(job, sinks, tracks[ti], image_dir, mask_dir, masker.get(),
                           pool, plan, stats, error);
    }

    const double t_drain = nn::now_ms();
    pool.finish();
    stats.drain = nn::now_ms() - t_drain;
    stats.total = nn::now_ms() - t_start;
    stats.encode_cpu = pool.busyMs();
    stats.write_failures = pool.failures();
    return ok;
}

bool extract_frames_at(const std::string& input, const FrameLook& look_in,
                       const std::vector<int64_t>& indices, int folder,
                       const FrameAtSink& on_frame,
                       const std::atomic<bool>* cancel, std::string& error,
                       const std::string& device) {
    // A preview decodes on the run's device, so a click drawn here names a
    // pixel the run will read.
    if (!sam::freeze_device(device, error)) return false;

    error = video_decode_availability();
    if (!error.empty()) return false;

    const int n = video_track_count(input, error);
    if (n <= 0) {
        if (error.empty()) error = "no video track in " + input;
        return false;
    }
    std::vector<int64_t> want;
    for (int64_t i : indices) want.push_back(std::max<int64_t>(i, 0));
    std::sort(want.begin(), want.end());
    want.erase(std::unique(want.begin(), want.end()), want.end());
    if (want.empty()) return true;

    std::vector<int> tracks;
    for (int i = 0; i < n; i++) tracks.push_back(i);
    FrameLook look = look_in;
    bool mixed = false;
    fold_auto_rotate(input, tracks, look, mixed);
    if (look.rotate % 90 != 0) {
        error = "rotation must be a multiple of 90 degrees";
        return false;
    }
    const bool pano = look.pano();
    if (pano && n < 2) {
        error = "a 360 capture needs two video tracks";
        return false;
    }

    video::ConvertOpts conv;
    if (!pano) {
        conv.scale = look.scale;
        conv.rotate = look.rotate;
    }
    const size_t np = pano && pano360_needs_track1(look.eac) ? 2 : 1;
    std::vector<std::unique_ptr<video::VideoPipeline>> pipe(np);
    for (size_t k = 0; k < np; k++) {
        pipe[k] = std::make_unique<video::VideoPipeline>();
        const int t = pano ? (int)k : std::min(std::max(folder, 0), n - 1);
        // Two: the frame being looked at, and the one after it, so the last
        // picture decoded is still held when the stream ends.
        if (!pipe[k]->open(input, t, 2, error)) return false;
    }

    Pano360Remap map;
    std::vector<uint8_t> canvas;
    if (pano) {
        const size_t view =
            std::min((size_t)std::max(folder, 0), look.views.size() - 1);
        pano360_remap(look.eac, look.views[view], map);
        canvas.resize((size_t)look.eac.canvasW() * look.eac.canvasH() * 3);
    }

    // Straight to the keyframe that covers the first index wanted. Without it
    // the last frame of a fifteen-minute capture is a fifteen-minute decode,
    // and twice that for the two tracks of a 360 file.
    int64_t floor_index = 0;
    for (size_t k = 0; k < np; k++) {
        int64_t landed = 0;
        std::string seek_error;
        if (pipe[k]->seek(want.front(), landed, seek_error))
            floor_index = std::max(floor_index, landed);
    }

    std::vector<video::FrameHandle> held(np);
    auto release = [&](std::vector<video::FrameHandle>& h) {
        for (size_t k = 0; k < np; k++)
            if (h[k].valid()) pipe[k]->release(h[k]);
    };
    auto emit = [&](int64_t index) {
        std::vector<nn::Image> img(np);
        for (size_t k = 0; k < np; k++)
            if (!pipe[k]->toImage(held[k], conv, img[k], error)) return false;
        if (!pano) {
            on_frame(img[0], index);
            return true;
        }
        for (size_t k = 0; k < np; k++)
            if (img[k].width != look.eac.track_w ||
                img[k].height != look.eac.track_h) {
                error = "track " + std::to_string(k) + " is not the size the"
                        " 360 layout was detected at";
                return false;
            }
        pano360_canvas(look.eac, img[0].data.data(),
                       np > 1 ? img[1].data.data() : nullptr, canvas.data());
        nn::Image out;
        out.width = map.width;
        out.height = map.height;
        out.channels = 3;
        out.data.resize((size_t)map.width * map.height * 3);
        pano360_apply(map, canvas.data(), look.eac.canvasW(),
                      look.eac.canvasH(), 0, out.data.data());
        on_frame(out, index);
        return true;
    };

    size_t next = 0;
    bool ended = false;
    while (next < want.size() && !ended) {
        if (cancel && cancel->load()) {
            release(held);
            error = "cancelled";
            return false;
        }
        // Every track brought up to `floor_index` -- and no further, or a
        // track that landed on a later keyframe than another could never be
        // caught up with. A frame already there is left where it is.
        for (size_t k = 0; k < np && !ended; k++) {
            while (!held[k].valid() ||
                   (held[k].timing.presentation_ordinal != video::kUnknownOrdinal &&
                    held[k].timing.presentation_ordinal < (uint64_t)floor_index)) {
                video::FrameHandle h;
                if (!pipe[k]->next(h, error)) {
                    ended = true;
                    break;
                }
                if (h.timing.presentation_ordinal == video::kUnknownOrdinal) {
                    pipe[k]->release(h);
                    continue;
                }
                if (held[k].valid()) pipe[k]->release(held[k]);
                held[k] = h;
            }
        }
        if (ended) break;
        // Presentation order from the decoder, which is what extraction names
        // its files after -- not a count of how many have gone by.
        int64_t index = (int64_t)held[0].timing.presentation_ordinal;
        bool aligned = true;
        for (size_t k = 1; k < np; k++) {
            const int64_t other = (int64_t)held[k].timing.presentation_ordinal;
            index = std::max(index, other);
            aligned = aligned &&
                      held[k].timing.presentation_ordinal ==
                          held[0].timing.presentation_ordinal;
        }
        floor_index = index;
        if (!aligned) continue;   // a track is behind; pull it up next round
        ++floor_index;
        while (next < want.size() && want[next] < index) ++next;
        if (next < want.size() && want[next] == index) {
            if (!emit(index)) {
                release(held);
                return false;
            }
            ++next;
        }
    }
    bool ok = error.empty();
    if (ok && next < want.size() && held[0].valid()) ok = emit(want[next]);
    release(held);
    return ok;
}

bool extract_one_frame(const std::string& input, const FrameLook& look,
                       int64_t index, int folder, nn::Image& out,
                       const std::atomic<bool>* cancel, std::string& error,
                       const std::string& device) {
    bool got = false;
    const bool ok = extract_frames_at(
        input, look, {index}, folder,
        [&](nn::Image& img, int64_t) {
            out = std::move(img);
            got = true;
        },
        cancel, error, device);
    if (ok && !got && error.empty()) error = "no frame could be decoded";
    return ok && got;
}

std::string format_extract_stats(const FrameExtractStats& s,
                                 const std::string& out_dir, bool masked) {
    namespace dmsg = spirula::i18n::msg::data;
    using spirula::i18n::format;
    using spirula::i18n::display_width;
    using spirula::i18n::pad_to;

    auto ms = [](double v) {
        char b[32];
        std::snprintf(b, sizeof b, "%.0f", v);
        return std::string(b);
    };
    auto rate = [](double per_ms_count, double ms_total) {
        char b[32];
        std::snprintf(b, sizeof b, "%.1f",
                      per_ms_count > 0 ? 1000.0 * per_ms_count /
                                             std::max(ms_total, 1e-6)
                                       : 0.0);
        return std::string(b);
    };

    // A label column and a value column. The labels are the translated ones,
    // so the column is as wide as the longest of THEM -- measured in terminal
    // columns, since a CJK label is half the characters and twice the width.
    struct Row { std::string label, value; bool gap_before; };
    std::vector<Row> rows;
    rows.push_back({dmsg::xs_decoded.get(),
                    std::to_string((long long)s.decoded), false});
    rows.push_back({dmsg::xs_measured.get(),
                    std::to_string((long long)s.measured), false});
    rows.push_back({dmsg::xs_written.get(),
                    format(dmsg::xs_written_to,
                           {(long long)s.written, out_dir}), false});
    rows.push_back({dmsg::xs_accepted.get(),
                    std::to_string((long long)s.accepted), false});
    rows.push_back({dmsg::xs_rescued.get(),
                    std::to_string((long long)s.rescued), false});
    rows.push_back({dmsg::xs_rejected.get(),
                    std::to_string((long long)s.rejected), false});
    if (s.analyzed > 0)
        rows.push_back({dmsg::xs_analyzed.get(),
                        std::to_string((long long)s.analyzed), false});
    rows.push_back({dmsg::xs_decode.get(),
                    format(dmsg::xs_ms_fps,
                           {ms(s.decode), rate((double)s.decoded, s.decode)}),
                    true});
    if (s.plan > 0)
        rows.push_back({dmsg::xs_motion.get(),
                        format(dmsg::xs_ms, {ms(s.plan)}), false});
    rows.push_back({dmsg::xs_sharpness.get(),
                    format(dmsg::xs_ms, {ms(s.sharpness)}), false});
    rows.push_back({dmsg::xs_convert.get(),
                    format(dmsg::xs_ms, {ms(s.convert)}), false});
    if (masked)
        rows.push_back({dmsg::xs_segmentation.get(),
                        format(dmsg::xs_ms, {ms(s.mask)}), false});
    rows.push_back({format(dmsg::xs_encode, {s.encoder_threads}),
                    format(dmsg::xs_ms_cpu, {ms(s.encode_cpu)}), false});
    rows.push_back({dmsg::xs_stalled.get(),
                    format(dmsg::xs_ms, {ms(s.submit)}), false});
    rows.push_back({dmsg::xs_drain.get(),
                    format(dmsg::xs_ms, {ms(s.drain)}), false});
    rows.push_back({dmsg::xs_total.get(),
                    format(dmsg::xs_ms_written_rate,
                           {ms(s.total), rate((double)s.written, s.total)}),
                    false});

    int width = 0;
    for (const Row& r : rows) width = std::max(width, display_width(r.label));

    std::string out = "\n";
    for (const Row& r : rows) {
        if (r.gap_before) out += "\n";
        out += pad_to(r.label, width + 2);
        out += r.value;
        out += "\n";
    }
    return out;
}

}  // namespace app
