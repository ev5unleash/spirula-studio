// FrameSelect.cpp -- see FrameSelect.h.

#include "app/gui/FrameSelect.h"

#include "core/ExrImage.h"

#include "external/stb_image.h"
#include "i18n/catalog/Log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <thread>
#include <charconv>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace lmsg = spirula::i18n::msg::log;

namespace gui {

namespace {

// Candidates loaded ahead of the motion tracker, which has to walk them in
// order. 64 grey frames is a few tens of megabytes whatever the source is.
constexpr size_t kChunk = 64;

// Box-average an interleaved RGB image into a grey rectangle. `rows` is how
// much of the source to read, so an EAC canvas can be measured on its top row.
void box_grey(const unsigned char* img, int W, int rows, int ow, int oh,
              float* out) {
    for (int y = 0; y < oh; y++) {
        int y0 = (int)((int64_t)y * rows / oh), y1 = (int)((int64_t)(y + 1) * rows / oh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < ow; x++) {
            int x0 = (int)((int64_t)x * W / ow), x1 = (int)((int64_t)(x + 1) * W / ow);
            if (x1 <= x0) x1 = x0 + 1;
            double acc = 0.0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    const unsigned char* p = img + ((size_t)yy * W + xx) * 3;
                    // BT.601 luma like cv2.cvtColor BGR2GRAY (RGB order here).
                    acc += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
                }
            out[(size_t)y * ow + x] = (float)(acc / ((y1 - y0) * (x1 - x0)));
        }
    }
}

// Sharpness, matching extract_frames.py add_frame(): 512x512 grey, mean
// subtracted, variance of the 3x3 Laplacian.
double laplacian_variance(std::vector<float>& gray, int S) {
    double mean = 0.0;
    for (float v : gray) mean += v;
    mean /= (double)gray.size();
    for (float& v : gray) v -= (float)mean;
    double sum = 0.0, sum2 = 0.0;
    int64_t n = 0;
    for (int y = 1; y < S - 1; y++)
        for (int x = 1; x < S - 1; x++) {
            const float* r = &gray[(size_t)y * S + x];
            const double lap = (double)r[-S] + r[S] + r[-1] + r[1] - 4.0 * r[0];
            sum += lap;
            sum2 += lap * lap;
            n++;
        }
    const double mu = sum / n;
    return sum2 / n - mu * mu;
}

struct Analysis {
    double score = -1.0;
    std::vector<uint8_t> grey;   // empty unless the motion pass wants it
};

// `mw`/`mh` <= 0 asks for the sharpness only. `top_rows` limits what the grey
// covers, for an EAC canvas whose bottom row is a different half of the sphere.
void analyze(const std::string& path, int mw, int mh, bool top_rows,
             Analysis& out) {
    int W = 0, H = 0, C = 0;
    std::vector<uint8_t> exr_rgb;
    unsigned char* img = nullptr;
    if (exr::is_exr(path)) {
        exr::Info info;
        if (!exr::decode_srgb8(path, exr::Options(), info, exr_rgb).empty()) return;
        W = info.width;
        H = info.height;
        img = exr_rgb.data();
    } else {
        img = stbi_load(path.c_str(), &W, &H, &C, 3);
        if (!img) return;
    }

    constexpr int S = 512;
    std::vector<float> gray((size_t)S * S);
    box_grey(img, W, H, S, S, gray.data());
    out.score = laplacian_variance(gray, S);

    if (mw > 0 && mh > 0) {
        std::vector<float> f((size_t)mw * mh);
        box_grey(img, W, top_rows ? H / 2 : H, mw, mh, f.data());
        out.grey.resize(f.size());
        for (size_t i = 0; i < f.size(); i++)
            out.grey[i] = (uint8_t)std::min(255.0f, std::max(0.0f, f[i] + 0.5f));
    }
    if (exr_rgb.empty()) stbi_image_free(img);
}

bool same_bytes(const fs::path& a, const fs::path& b) {
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    return std::equal(std::istreambuf_iterator<char>(fa), std::istreambuf_iterator<char>(),
                      std::istreambuf_iterator<char>(fb), std::istreambuf_iterator<char>());
}

// ffmpeg's fps filter repeats a source frame to reach a rate above the
// source's, and the repeats encode to the same bytes. Each candidate's first
// copy, so a frame is kept once however many groups choose it.
std::vector<size_t> first_copies(const std::vector<fs::path>& files) {
    std::vector<size_t> first(files.size());
    uintmax_t prev = (uintmax_t)-1;
    for (size_t i = 0; i < files.size(); i++) {
        std::error_code ec;
        const uintmax_t size = fs::file_size(files[i], ec);
        first[i] = i;
        if (i > 0 && size != (uintmax_t)-1 && size == prev &&
            same_bytes(files[i - 1], files[i]))
            first[i] = first[i - 1];
        prev = size;
    }
    return first;
}

}  // namespace

int select_sharpest_frames(const std::string& cand_dir,
                           const std::string& out_dir,
                           const std::string& prefix,
                           const FrameSelectOptions& options,
                           const std::function<void(const std::string&)>& log,
                           const std::atomic<bool>& cancel) {
    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(cand_dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) files.push_back(it->path());
    std::sort(files.begin(), files.end());
    if (files.empty()) return -1;

    int group = std::max(options.group, 1);
    if (!options.adaptive && options.max_frames > 0)
        group = std::max<int>(
            group, (int)((files.size() + options.max_frames - 1) / options.max_frames));

    // The motion tracker's frames. ffmpeg has already cut the canvas, so what
    // is left of an EAC packing is its top row and of a panorama the whole of it.
    app::MotionOptions mo;
    std::unique_ptr<app::MotionTracker> tracker;
    if (options.adaptive) {
        mo.view = options.view;
        mo.out_fov = options.out_fov;
        int src_w = 0, src_h = 0;
        if (options.eac.valid()) {
            mo.eac = options.eac;
            mo.eac.margin = 0;
            if (options.eac.sphere()) {
                mo.eac.track_w = options.eac.canvasW();
            } else {
                mo.eac.strip = 0;
                mo.eac.track_w = 3 * options.eac.face;
                mo.eac.track_h = options.eac.face;
            }
            src_w = mo.eac.track_w;
            src_h = mo.eac.track_h;
        } else {
            int w = 0, h = 0, c = 0;
            if (stbi_info(files[0].string().c_str(), &w, &h, &c)) {
                src_w = w;
                src_h = h;
            }
        }
        app::motion_frame_size(mo.view, src_w, src_h, mo.width, mo.height);
        if (mo.width > 0) tracker = std::make_unique<app::MotionTracker>(mo);
    }

    // Score, and measure, in chunks: the tracker has to see the frames in
    // order, and everything before it parallelizes.
    std::vector<double> scores(files.size(), -1.0);
    size_t reported = 0;
    const bool want_scores = group > 1 || options.adaptive;
    if (want_scores) {
        const unsigned n_threads = std::max(1u, std::thread::hardware_concurrency());
        auto last_log = std::chrono::steady_clock::now();
        for (size_t base = 0; base < files.size(); base += kChunk) {
            if (cancel.load()) return -1;
            const size_t end = std::min(base + kChunk, files.size());
            std::vector<Analysis> got(end - base);
            std::atomic<size_t> next{base};
            std::vector<std::thread> pool;
            for (unsigned t = 0; t < n_threads; t++)
                pool.emplace_back([&] {
                    for (;;) {
                        const size_t i = next.fetch_add(1);
                        if (i >= end || cancel.load()) return;
                        // A decode that throws would be std::terminate off a
                        // worker thread. An unscored frame loses its group.
                        try {
                            analyze(files[i].string(), tracker ? mo.width : 0,
                                    tracker ? mo.height : 0,
                                    options.eac.valid() && !options.eac.sphere(),
                                    got[i - base]);
                        } catch (...) {}
                    }
                });
            for (auto& th : pool) th.join();
            if (cancel.load()) return -1;
            for (size_t i = base; i < end; i++) {
                scores[i] = got[i - base].score;
                if (!tracker || got[i - base].grey.empty()) continue;
                tracker->track(got[i - base].grey.data(), (int64_t)i);
                if (options.measured)
                    for (; reported < tracker->costs().size(); reported++)
                        options.measured(tracker->ends()[reported],
                                         (int64_t)files.size(),
                                         tracker->costs()[reported]);
            }
            if (options.scanning)
                options.scanning((int64_t)end, (int64_t)files.size());
            const auto now = std::chrono::steady_clock::now();
            if (log && (end == files.size() ||
                        now - last_log > std::chrono::seconds(1))) {
                last_log = now;
                log(spirula::i18n::format(
                    lmsg::frame_candidates_scored,
                    {(long long)end, (long long)files.size()}));
            }
        }
    }

    // Which candidates survive: one per group, or one per equal share of view
    // change with the sharpest of the window around it. What an earlier pick
    // already took, itself or a repeat of it, is not a candidate again.
    const std::vector<size_t> first = first_copies(files);
    std::vector<bool> taken(files.size(), false);
    std::vector<size_t> keep;
    auto take = [&](size_t g0, size_t g1) {
        size_t best = g1;
        for (size_t i = g0; i < g1; i++)
            if (!taken[first[i]] && (best == g1 || scores[i] > scores[best])) best = i;
        if (best == g1) return;
        taken[first[best]] = true;
        keep.push_back(best);
    };
    if (tracker) {
        tracker->finish();
        const int window = std::max(options.window, 1);
        app::MotionPlanInput mi;
        mi.cost = tracker->costs();
        mi.step = tracker->steps();
        mi.ends = tracker->ends();
        mi.frames = (int64_t)files.size();
        mi.skip = group;
        mi.window = window;
        mi.max_frames = options.max_frames;
        const std::vector<std::vector<int64_t>> planned =
            app::plan_by_motion({mi}, options.range);
        const std::vector<int64_t>& plan = planned[0];
        for (int64_t at : plan) {
            const size_t g1 = (size_t)at + 1;
            take(g1 > (size_t)window ? g1 - (size_t)window : 0, g1);
        }
        if (options.planned) options.planned(plan, (int64_t)files.size());
    }
    if (keep.empty())
        for (size_t g0 = 0; g0 < files.size(); g0 += (size_t)group)
            take(g0, std::min(g0 + (size_t)group, files.size()));

    fs::create_directories(out_dir, ec);
    std::vector<bool> kept_flag(files.size(), false);
    int kept = 0;
    for (size_t best : keep) {
        const std::string ext = files[best].extension().string();
        char name[64];
        // Numbered by the candidate it is, not by how many were kept: the stem
        // is what times a frame against the video's IMU, and an adaptive plan
        // leaves nothing evenly spaced for a rate to recover it from.
        std::snprintf(name, sizeof name, "%s%05d%s", prefix.c_str(), (int)best,
                      ext.empty() ? ".jpg" : ext.c_str());
        fs::rename(files[best], fs::path(out_dir) / name, ec);
        if (ec) {   // cross-device fallback
            fs::copy_file(files[best], fs::path(out_dir) / name,
                          fs::copy_options::overwrite_existing, ec);
            fs::remove(files[best], ec);
        }
        kept_flag[best] = true;
        kept++;
        if (cancel.load()) return -1;
    }
    for (size_t i = 0; i < files.size(); i++)
        if (!kept_flag[i]) fs::remove(files[i], ec);
    return kept;
}

bool select_sharpest_frame_groups(
        std::vector<FrameSelectGroup>& groups,
        const std::function<void(const std::string&)>& log,
        const std::atomic<bool>& cancel, std::string& error) {
    struct Track {
        std::vector<fs::path> files;
        std::vector<int64_t> times;
        std::vector<double> scores;
        std::vector<size_t> first, keep;
    };
    struct Work {
        FrameSelectGroup* group = nullptr;
        std::vector<Track> tracks;
        std::unique_ptr<app::MotionTracker> tracker;
        app::MotionOptions motion;
        app::MotionPlanInput plan_input;
        size_t plan_index = 0;
        bool planned = false;
    };
    auto timestamp = [](const fs::path& path, int64_t& value) {
        const std::string stem = path.stem().string();
        const size_t sep = stem.find_last_of('_');
        if (sep == std::string::npos || sep + 1 == stem.size()) return false;
        const char* first = stem.data() + sep + 1;
        const char* last = stem.data() + stem.size();
        const auto parsed = std::from_chars(first, last, value);
        return parsed.ec == std::errc{} && parsed.ptr == last;
    };

    error.clear();
    if (cancel.load()) {
        error = lmsg::err_cancelled.get();
        return false;
    }
    float range = groups.empty() ? 1.0f : groups[0].options.range;
    std::vector<Work> work(groups.size());
    std::vector<app::MotionPlanInput> shared_plans;

    for (size_t gi = 0; gi < groups.size(); gi++) {
        FrameSelectGroup& group = groups[gi];
        if (group.candidate_dirs.empty() ||
            group.candidate_dirs.size() != group.output_dirs.size()) {
            error = lmsg::err_frame_candidate_tracks.get();
            return false;
        }
        if (group.options.adaptive &&
            std::fabs(group.options.range - range) > 1e-6f) {
            error = lmsg::err_frame_candidate_range.get();
            return false;
        }

        Work& w = work[gi];
        w.group = &group;
        w.tracks.resize(group.candidate_dirs.size());
        for (size_t ti = 0; ti < w.tracks.size(); ti++) {
            Track& track = w.tracks[ti];
            std::vector<std::pair<int64_t, fs::path>> timed;
            std::error_code ec;
            for (fs::directory_iterator it(group.candidate_dirs[ti], ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                int64_t pts = 0;
                if (!timestamp(it->path(), pts)) {
                    error = lmsg::err_frame_candidate_timestamp.get();
                    return false;
                }
                timed.emplace_back(pts, it->path());
            }
            if (ec) {
                error = lmsg::err_frame_candidate_read.get();
                return false;
            }
            std::sort(timed.begin(), timed.end(),
                      [](const auto& a, const auto& b) {
                          return a.first < b.first ||
                                 (a.first == b.first && a.second < b.second);
                      });
            for (const auto& item : timed) {
                if (!track.times.empty() && track.times.back() == item.first &&
                    group.sync_tracks) {
                    error = lmsg::err_frame_duplicate_timestamp.get();
                    return false;
                }
                track.times.push_back(item.first);
                track.files.push_back(item.second);
            }
            if (track.files.empty()) {
                error = lmsg::err_frame_candidate_read.get();
                return false;
            }
        }

        if (group.sync_tracks) {
            const Track& first = w.tracks[0];
            for (size_t ti = 1; ti < w.tracks.size(); ti++) {
                const Track& other = w.tracks[ti];
                if (other.times != first.times) {
                    error = lmsg::err_frame_tracks_timestamp_mismatch.get();
                    return false;
                }
            }
        }
        for (Track& track : w.tracks)
            track.scores.assign(track.files.size(), -1.0);
        if (group.sync_tracks)
            std::fill(w.tracks[0].scores.begin(), w.tracks[0].scores.end(), 0.0);

        size_t candidates = 0;
        for (const Track& track : w.tracks)
            candidates = std::max(candidates, track.files.size());
        int selection_group = std::max(group.options.group, 1);
        if (!group.options.adaptive && group.options.max_frames > 0)
            selection_group = std::max<int>(
                selection_group,
                (int)((candidates + (size_t)group.options.max_frames - 1) /
                      (size_t)group.options.max_frames));
        const bool want_scores =
            group.options.adaptive || selection_group > 1;
        int mw = 0, mh = 0;
        if (group.options.adaptive) {
            w.motion.view = group.options.view;
            w.motion.out_fov = group.options.out_fov;
            int src_w = 0, src_h = 0, channels = 0;
            if (group.options.eac.valid()) {
                w.motion.eac = group.options.eac;
                w.motion.eac.margin = 0;
                if (group.options.eac.sphere()) {
                    w.motion.eac.track_w = group.options.eac.canvasW();
                } else {
                    w.motion.eac.strip = 0;
                    w.motion.eac.track_w = 3 * group.options.eac.face;
                    w.motion.eac.track_h = group.options.eac.face;
                }
                src_w = w.motion.eac.track_w;
                src_h = w.motion.eac.track_h;
            } else if (stbi_info(w.tracks[0].files[0].string().c_str(),
                                 &src_w, &src_h, &channels)) {
                // Dimensions only; the candidate is decoded once below.
            }
            app::motion_frame_size(w.motion.view, src_w, src_h, mw, mh);
            w.motion.width = mw;
            w.motion.height = mh;
            if (mw <= 0 || mh <= 0) {
                error = lmsg::err_frame_motion_measure.get();
                return false;
            }
            w.tracker = std::make_unique<app::MotionTracker>(w.motion);
        }

        if (want_scores) {
            size_t n = w.tracks[0].files.size();
            if (!w.tracker)
                for (const Track& track : w.tracks)
                    n = std::max(n, track.files.size());
            size_t reported = 0;
            const unsigned n_threads = std::min(
                std::max(1u, std::thread::hardware_concurrency()),
                (unsigned)n);
            auto last_log = std::chrono::steady_clock::now();
            for (size_t base = 0; base < n; base += kChunk) {
                if (cancel.load()) { error = lmsg::err_cancelled.get(); return false; }
                const size_t end = std::min(base + kChunk, n);
                std::vector<std::vector<Analysis>> got(w.tracks.size());
                for (size_t ti = 0; ti < w.tracks.size(); ti++) {
                    Track& track = w.tracks[ti];
                    const size_t stop = std::min(end, track.files.size());
                    if (stop <= base) continue;
                    got[ti].resize(stop - base);
                    std::atomic<size_t> next{base};
                    std::vector<std::thread> pool;
                    pool.reserve(n_threads);
                    for (unsigned t = 0; t < n_threads; t++)
                        pool.emplace_back([&, ti, base, stop] {
                            for (;;) {
                                const size_t i = next.fetch_add(1);
                                if (i >= stop || cancel.load()) return;
                                try {
                                    const bool measure = w.tracker && ti == 0;
                                    analyze(track.files[i].string(),
                                            measure ? mw : 0,
                                            measure ? mh : 0,
                                            group.options.eac.valid() &&
                                                !group.options.eac.sphere(),
                                            got[ti][i - base]);
                                } catch (...) {}
                            }
                        });
                    for (std::thread& thread : pool) thread.join();
                }
                if (cancel.load()) { error = lmsg::err_cancelled.get(); return false; }
                for (size_t i = base; i < end; i++) {
                    for (size_t ti = 0; ti < w.tracks.size(); ti++) {
                        if (i >= w.tracks[ti].files.size()) continue;
                        const Analysis& a = got[ti][i - base];
                        if (a.score < 0.0) {
                            error = lmsg::err_frame_candidate_score.get();
                            return false;
                        }
                        if (group.sync_tracks)
                            w.tracks[0].scores[i] += a.score;
                        else
                            w.tracks[ti].scores[i] = a.score;
                    }
                    if (w.tracker && i < w.tracks[0].files.size()) {
                        const Analysis& a = got[0][i - base];
                        if (a.grey.empty()) {
                            error = lmsg::err_frame_motion_measure.get();
                            return false;
                        }
                        w.tracker->track(a.grey.data(), (int64_t)i);
                        if (group.options.measured)
                            for (; reported < w.tracker->costs().size(); reported++)
                                group.options.measured(
                                    w.tracker->ends()[reported], (int64_t)n,
                                    w.tracker->costs()[reported]);
                    }
                }
                if (group.options.scanning)
                    group.options.scanning((int64_t)end, (int64_t)n);
                const auto now = std::chrono::steady_clock::now();
                if (log && (end == n ||
                            now - last_log > std::chrono::seconds(1))) {
                    last_log = now;
                    log(spirula::i18n::format(
                        lmsg::frame_candidates_scored,
                        {(long long)end, (long long)n}));
                }
            }
        }

        if (w.tracker) {
            w.tracker->finish();
            w.plan_input.cost = w.tracker->costs();
            w.plan_input.view = w.motion.view;
            w.plan_input.out_fov = w.motion.out_fov;
            w.plan_input.step = w.tracker->steps();
            w.plan_input.ends = w.tracker->ends();
            w.plan_input.frames = (int64_t)w.tracks[0].files.size();
            w.plan_input.skip = std::max(group.options.group, 1);
            w.plan_input.window = std::max(group.options.window, 1);
            w.plan_input.max_frames = group.options.max_frames;
            w.plan_index = shared_plans.size();
            shared_plans.push_back(std::move(w.plan_input));
            w.planned = true;
        }
    }

    std::vector<std::vector<int64_t>> plans;
    if (!shared_plans.empty()) {
        plans = app::plan_by_motion(shared_plans, range);
        for (Work& w : work) {
            if (!w.planned) continue;
            const std::vector<int64_t>& plan = plans[w.plan_index];
            if (w.group->options.planned)
                w.group->options.planned(
                    plan, (int64_t)w.tracks[0].files.size());
            if (plan.empty()) {
                error = lmsg::err_frame_motion_too_short.get();
                return false;
            }
        }
    }

    auto choose = [](Track& track, const std::vector<int64_t>& plan,
                     const FrameSelectOptions& options, bool joined) {
        if (!joined) track.first = first_copies(track.files);
        std::vector<bool> taken(track.files.size(), false);
        auto take = [&](size_t begin, size_t end) {
            size_t best = end;
            for (size_t i = begin; i < end; i++)
                if (!taken[track.first[i]] &&
                    (best == end || track.scores[i] > track.scores[best]))
                    best = i;
            if (best == end) return;
            taken[track.first[best]] = true;
            track.keep.push_back(best);
        };
        if (!plan.empty()) {
            const size_t window = (size_t)std::max(options.window, 1);
            for (int64_t at : plan) {
                if (at < 0) continue;
                const size_t end = std::min((size_t)at + 1, track.files.size());
                if (!end) continue;
                take(end > window ? end - window : 0, end);
            }
        } else {
            int group = std::max(options.group, 1);
            if (!options.adaptive && options.max_frames > 0)
                group = std::max<int>(
                    group, (int)((track.files.size() + options.max_frames - 1) /
                                 options.max_frames));
            for (size_t begin = 0; begin < track.files.size();
                 begin += (size_t)group)
                take(begin, std::min(begin + (size_t)group,
                                     track.files.size()));
        }
    };

    const std::vector<int64_t> no_plan;
    for (size_t gi = 0; gi < groups.size(); gi++) {
        Work& w = work[gi];
        const std::vector<int64_t>& plan =
            w.planned ? plans[w.plan_index] : no_plan;
        if (w.group->sync_tracks) {
            Track& first = w.tracks[0];
            first.first.resize(first.files.size());
            for (size_t i = 0; i < first.files.size(); i++)
                first.first[i] = i;
            for (size_t i = 1; i < first.files.size(); i++) {
                bool repeat = true;
                for (size_t ti = 0; ti < w.tracks.size() && repeat; ti++)
                    repeat = same_bytes(w.tracks[ti].files[i - 1],
                                        w.tracks[ti].files[i]);
                if (repeat) first.first[i] = first.first[i - 1];
            }
            choose(first, plan, w.group->options, /*joined=*/true);
            for (size_t ti = 1; ti < w.tracks.size(); ti++)
                w.tracks[ti].keep = first.keep;
            w.group->kept = (int)first.keep.size();
        } else {
            for (Track& track : w.tracks)
                choose(track, plan, w.group->options, /*joined=*/false);
            w.group->kept = (int)w.tracks[0].keep.size();
        }
    }

    // All analysis and plans succeeded before the first final image is moved.
    for (size_t gi = 0; gi < groups.size(); gi++) {
        Work& w = work[gi];
        for (size_t ti = 0; ti < w.tracks.size(); ti++) {
            std::error_code ec;
            fs::create_directories(w.group->output_dirs[ti], ec);
            if (ec) {
                error = lmsg::err_frame_output_directory.get();
                return false;
            }
        }
        for (size_t ti = 0; ti < w.tracks.size(); ti++) {
            Track& track = w.tracks[ti];
            for (size_t best : track.keep) {
                const std::string ext = track.files[best].extension().string();
                char name[64];
                std::snprintf(name, sizeof name, "%05lld%s",
                              (long long)best,
                              ext.empty() ? ".jpg" : ext.c_str());
                const fs::path to =
                    fs::path(w.group->output_dirs[ti]) / name;
                std::error_code ec;
                fs::rename(track.files[best], to, ec);
                if (ec) {
                    ec.clear();
                    fs::copy_file(track.files[best], to,
                                  fs::copy_options::overwrite_existing, ec);
                    if (ec) {
                        error = lmsg::err_frame_candidate_publish.get();
                        return false;
                    }
                    fs::remove(track.files[best], ec);
                }
                if (cancel.load()) { error = lmsg::err_cancelled.get(); return false; }
            }
        }
    }
    for (Work& w : work)
        for (Track& track : w.tracks) {
            std::error_code ec;
            for (const fs::path& file : track.files) fs::remove(file, ec);
        }
    return true;
}

}  // namespace gui
