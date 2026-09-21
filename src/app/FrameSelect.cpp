// FrameSelect.cpp -- see FrameSelect.h.

#include "app/FrameSelect.h"

#include "core/ExrImage.h"

#include "external/stb_image.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <utility>
#include <thread>
#include <limits>
#include <vector>

namespace fs = std::filesystem;

namespace app {

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

struct Candidate {
    fs::path path;
    uint64_t export_ordinal = 0;
    FramePosition position;
};

struct Analysis {
    double score = std::numeric_limits<double>::quiet_NaN();
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

}  // namespace

FrameSelectChoice choose_frame_candidate(const double* scores, size_t score_count,
                                         size_t primary_begin, size_t primary_end,
                                         int rescue_frames,
                                         const uint8_t* excluded,
                                         float minimum_sharpness) {
    FrameSelectChoice choice;
    if (!scores || !score_count) return choice;
    primary_begin = std::min(primary_begin, score_count);
    primary_end = std::min(primary_end, score_count);
    if (primary_end < primary_begin) primary_end = primary_begin;

    auto scan = [&](size_t begin, size_t end,
                    FrameSelectionKind kind) {
        for (size_t i = begin; i < end; i++) {
            if (excluded && excluded[i]) continue;
            const double score = scores[i];
            if (!std::isfinite(score) || score < 0.0 ||
                (minimum_sharpness > 0.0f &&
                 score < (double)minimum_sharpness))
                continue;
            if (choice.index < 0 || score > choice.score) {
                choice.index = (int64_t)i;
                choice.score = score;
                choice.kind = kind;
            }
        }
    };
    scan(primary_begin, primary_end, FrameSelectionKind::Accepted);
    if (choice.index >= 0) return choice;
    if (rescue_frames > 0) {
        const size_t rescue = std::min(primary_begin, (size_t)rescue_frames);
        scan(primary_begin - rescue, primary_begin,
             FrameSelectionKind::Rescued);
    }
    return choice;
}

bool checked_candidate_cap(uint64_t max_frames, uint64_t candidate_group,
                           uint64_t& cap) {
    cap = 0;
    if (candidate_group == 0) return false;
    if (max_frames == 0) return true;
    if (max_frames > std::numeric_limits<uint64_t>::max() / candidate_group)
        return false;
    cap = max_frames * candidate_group;
    return cap != 0;
}

bool checked_candidate_group(const FrameSelectionSettings& settings, int& group) {
    const int window = std::max(settings.sharp_window, 1);
    if (!settings.adaptive) {
        group = window;
        return true;
    }
    if (!std::isfinite((double)settings.adaptive_range)) return false;
    const double range = std::ceil((double)settings.adaptive_range);
    if (range < (double)std::numeric_limits<int>::min() ||
        range > (double)std::numeric_limits<int>::max())
        return false;
    group = std::max(window, (int)range);
    return true;
}

bool checked_candidate_rate(double base_rate, double duration, uint64_t cap,
                            double& rate) {
    if (!std::isfinite(base_rate) || base_rate <= 0.0 ||
        !std::isfinite(duration) || duration < 0.0)
        return false;
    rate = base_rate;
    if (cap != 0 && duration > 0.0) {
        const double limited = (double)cap / duration;
        if (!std::isfinite(limited) || limited <= 0.0) return false;
        rate = std::min(rate, limited);
    }
    return std::isfinite(rate) && rate > 0.0;
}

FrameSelectChoice choose_synchronized_candidate(
    const std::vector<const std::vector<double>*>& scores,
    size_t primary_begin, size_t primary_end, int rescue_frames,
    const uint8_t* excluded, float minimum_sharpness) {
    FrameSelectChoice choice;
    if (scores.empty() || !scores.front()) return choice;
    const size_t score_count = scores.front()->size();
    for (const std::vector<double>* track : scores)
        if (!track || track->size() != score_count) return choice;
    primary_begin = std::min(primary_begin, score_count);
    primary_end = std::min(primary_end, score_count);
    if (primary_end < primary_begin) primary_end = primary_begin;

    auto scan = [&](size_t begin, size_t end,
                    FrameSelectionKind kind) {
        for (size_t i = begin; i < end; i++) {
            if (excluded && excluded[i]) continue;
            double weakest = std::numeric_limits<double>::infinity();
            bool eligible = true;
            for (const std::vector<double>* track : scores) {
                const double score = (*track)[i];
                if (!std::isfinite(score) || score < 0.0 ||
                    (minimum_sharpness > 0.0f &&
                     score < (double)minimum_sharpness)) {
                    eligible = false;
                    break;
                }
                weakest = std::min(weakest, score);
            }
            if (eligible && (choice.index < 0 || weakest > choice.score)) {
                choice.index = (int64_t)i;
                choice.score = weakest;
                choice.kind = kind;
            }
        }
    };
    scan(primary_begin, primary_end, FrameSelectionKind::Accepted);
    if (choice.index >= 0) return choice;
    if (rescue_frames > 0) {
        const size_t rescue = std::min(primary_begin, (size_t)rescue_frames);
        scan(primary_begin - rescue, primary_begin,
             FrameSelectionKind::Rescued);
    }
    return choice;
}

namespace {

struct SelectionTrack {
    FrameSelectTrack spec;
    std::vector<Candidate> files;
    std::vector<double> scores;
};

bool enumerate_candidates(const std::string& dir, std::vector<Candidate>& files) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec))
        if (it->is_regular_file(ec)) files.push_back({it->path(), 0, {}});
    if (ec) return false;
    std::sort(files.begin(), files.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.path < b.path;
              });
    for (size_t i = 0; i < files.size(); i++) {
        files[i].export_ordinal = (uint64_t)i;
        // ffmpeg's numbered export is not enough to prove a source
        // discontinuity, so the fallback deliberately exposes one segment.
        files[i].position = FramePosition{0, (int64_t)i};
    }
    return !files.empty();
}

bool move_candidate(const Candidate& candidate, const std::string& out_dir,
                    const std::string& prefix, FrameSelectOutput& output,
                    std::error_code& ec) {
    fs::create_directories(out_dir, ec);
    if (ec) return false;
    const std::string ext = candidate.path.extension().string();
    char name[64];
    // Numbered by the candidate locator, never by an original source-frame
    // identity; the fallback cannot prove one from ffmpeg's output.
    std::snprintf(name, sizeof name, "%s%05llu%s", prefix.c_str(),
                  (unsigned long long)candidate.export_ordinal,
                  ext.empty() ? ".jpg" : ext.c_str());
    output.path = (fs::path(out_dir) / name).string();
    output.export_ordinal = candidate.export_ordinal;
    fs::rename(candidate.path, output.path, ec);
    if (!ec) return true;
    ec.clear();
    fs::copy_file(candidate.path, output.path,
                  fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    fs::remove(candidate.path, ec);
    return !ec;
}

int select_frames(const std::vector<FrameSelectTrack>& specs,
                  bool synchronized, const FrameSelectOptions& options,
                  const std::function<void(const std::string&)>& log,
                  const std::atomic<bool>& cancel) {
    if (specs.empty()) return -1;
    std::vector<SelectionTrack> tracks(specs.size());
    for (size_t t = 0; t < specs.size(); t++) {
        tracks[t].spec = specs[t];
        if (!enumerate_candidates(tracks[t].spec.cand_dir, tracks[t].files))
            return -1;
    }
    const size_t count = tracks.front().files.size();
    if (options.spans) {
        std::vector<FrameSegmentSpan> spans;
        spans.push_back({0, 0, (int64_t)count - 1});
        options.spans(spans);
    }
    if (synchronized) {
        for (size_t t = 1; t < tracks.size(); t++) {
            if (tracks[t].files.size() != count ||
                tracks[t].spec.prefix != tracks.front().spec.prefix)
                return -1;
            for (size_t i = 0; i < count; i++)
                if (tracks[t].files[i].path.filename() !=
                    tracks.front().files[i].path.filename())
                    return -1;
        }
    }

    const int group = std::max(options.group, 1);
    int fixed_group = group;
    if (options.max_frames > 0) {
        const size_t max_frames = (size_t)options.max_frames;
        const size_t by_cap = count / max_frames +
                              (count % max_frames != 0 ? 1 : 0);
        if (by_cap > (size_t)std::numeric_limits<int>::max()) return -1;
        fixed_group = std::max<int>(fixed_group, (int)by_cap);
    }

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
            if (stbi_info(tracks.front().files[0].path.string().c_str(),
                          &w, &h, &c)) {
                src_w = w;
                src_h = h;
            }
        }
        app::motion_frame_size(mo.view, src_w, src_h, mo.width, mo.height);
        if (mo.width > 0) tracker = std::make_unique<app::MotionTracker>(mo);
    }

    for (SelectionTrack& track : tracks)
        track.scores.assign(count, std::numeric_limits<double>::quiet_NaN());
    size_t reported = 0;
    const unsigned n_threads = std::max(1u, std::thread::hardware_concurrency());
    auto last_log = std::chrono::steady_clock::now();
    for (size_t base = 0; base < count; base += kChunk) {
        if (cancel.load()) return -1;
        const size_t end = std::min(base + kChunk, count);
        const size_t chunk = end - base;
        std::vector<Analysis> got(tracks.size() * chunk);
        std::atomic<size_t> next{0};
        const size_t work = got.size();
        std::vector<std::thread> pool;
        pool.reserve(n_threads);
        for (unsigned t = 0; t < n_threads; t++)
            pool.emplace_back([&] {
                for (;;) {
                    const size_t at = next.fetch_add(1);
                    if (at >= work || cancel.load()) return;
                    const size_t track = at / chunk;
                    const size_t i = base + at % chunk;
                    try {
                        analyze(
                            tracks[track].files[i].path.string(),
                            tracker && track == 0 ? mo.width : 0,
                            tracker && track == 0 ? mo.height : 0,
                            options.eac.valid() && !options.eac.sphere(),
                            got[at]);
                    } catch (...) {}
                }
            });
        for (std::thread& th : pool) th.join();
        if (cancel.load()) return -1;
        for (size_t track = 0; track < tracks.size(); track++) {
            for (size_t i = base; i < end; i++)
                tracks[track].scores[i] = got[track * chunk + i - base].score;
            if (track == 0 && tracker) {
                for (size_t i = base; i < end; i++) {
                    Analysis& result = got[i - base];
                    if (result.grey.empty()) continue;
                    tracker->track(result.grey.data(),
                                   app::FramePosition{0, (int64_t)i});
                    if (options.measured) {
                        const auto& steps = tracker->steps();
                        for (; reported < steps.size(); reported++)
                            options.measured(
                                steps[reported].end,
                                (int64_t)count, steps[reported].cost);
                    }
                }
            }
        }
        if (options.scanning)
            options.scanning((int64_t)end, (int64_t)count);
        const auto now = std::chrono::steady_clock::now();
        if (log && (end == count ||
                    now - last_log > std::chrono::seconds(1))) {
            last_log = now;
            log("scored " + std::to_string(end) + "/" +
                std::to_string(count) + " candidate frames");
        }
    }

    std::vector<size_t> keep;
    std::vector<const std::vector<double>*> synchronized_scores;
    if (synchronized) {
        synchronized_scores.reserve(tracks.size());
        for (const SelectionTrack& track : tracks)
            synchronized_scores.push_back(&track.scores);
    }
    std::vector<uint8_t> excluded(count, 0);
    auto plan_one = [&](size_t primary_begin, size_t primary_end,
                        size_t planned) {
        const FrameSelectChoice choice =
            synchronized
                ? choose_synchronized_candidate(
                      synchronized_scores, primary_begin, primary_end,
                      options.rescue_frames, excluded.data(),
                      options.minimum_sharpness)
                : choose_frame_candidate(
                      tracks.front().scores.data(), count, primary_begin,
                      primary_end, options.rescue_frames, excluded.data(),
                      options.minimum_sharpness);
        FrameSelectionDecision d;
        d.planned = tracks.front().files[planned].position;
        d.has_selected = choice.index >= 0;
        if (d.has_selected)
            d.selected = tracks.front().files[(size_t)choice.index].position;
        d.score = choice.score;
        d.kind = choice.kind;
        if (options.decision) options.decision(d);
        if (choice.index >= 0) {
            const size_t selected = (size_t)choice.index;
            excluded[selected] = 1;
            keep.push_back(selected);
        }
    };

    bool adaptive_plan = false;
    std::vector<std::pair<size_t, size_t>> intervals;
    if (tracker) {
        tracker->finish();
        const int window = std::max(options.window, 1);
        std::vector<float> motion_cost;
        std::vector<int64_t> motion_ends;
        const auto& steps = tracker->steps();
        motion_cost.reserve(steps.size());
        motion_ends.reserve(steps.size());
        for (const auto& step : steps) {
            if (step.end.segment != 0) continue;
            motion_cost.push_back(step.cost);
            motion_ends.push_back(step.end.ordinal);
        }
        const std::vector<int64_t> plan = app::plan_by_motion(
            motion_cost, motion_ends, (int64_t)count, group, window,
            options.range, options.max_frames);
        for (int64_t at : plan) {
            if (at < 0 || (size_t)at >= count) continue;
            const size_t g1 = (size_t)at + 1;
            const size_t g0 = g1 > (size_t)window ? g1 - (size_t)window : 0;
            intervals.push_back({g0, g1});
        }
        adaptive_plan = !intervals.empty();
    }
    if (!adaptive_plan)
        for (size_t g0 = 0; g0 < count; g0 += (size_t)fixed_group) {
            const size_t g1 = std::min(g0 + (size_t)fixed_group, count);
            intervals.push_back({g0, g1});
        }

    FramePlan plan;
    plan.frames.reserve(intervals.size());
    for (const auto& interval : intervals)
        plan.frames.push_back(tracks.front().files[interval.second - 1].position);
    if (options.planned && !plan.empty())
        options.planned(plan, (int64_t)count);
    for (const auto& interval : intervals)
        plan_one(interval.first, interval.second, interval.second - 1);

    std::error_code ec;
    for (const SelectionTrack& track : tracks) {
        fs::create_directories(track.spec.out_dir, ec);
        if (ec) return -1;
    }
    std::vector<std::vector<bool>> kept_flag(
        tracks.size(), std::vector<bool>(count, false));
    int kept = 0;
    for (size_t best : keep) {
        std::vector<FrameSelectOutput> outputs(tracks.size());
        std::vector<fs::path> moved;
        for (size_t track = 0; track < tracks.size(); track++) {
            if (!move_candidate(tracks[track].files[best],
                                tracks[track].spec.out_dir,
                                tracks[track].spec.prefix, outputs[track],
                                ec)) {
                for (const fs::path& path : moved) fs::remove(path, ec);
                return -1;
            }
            moved.push_back(outputs[track].path);
        }
        for (size_t track = 0; track < tracks.size(); track++) {
            if (options.selected_track)
                options.selected_track(track, outputs[track]);
            else if (options.selected)
                options.selected(outputs[track]);
            kept_flag[track][best] = true;
        }
        kept++;
        if (cancel.load()) return -1;
    }
    for (size_t track = 0; track < tracks.size(); track++)
        for (size_t i = 0; i < count; i++)
            if (!kept_flag[track][i]) fs::remove(tracks[track].files[i].path, ec);
    return kept;
}

}  // namespace

int select_sharpest_frames(const std::string& cand_dir,
                           const std::string& out_dir,
                           const std::string& prefix,
                           const FrameSelectOptions& options,
                           const std::function<void(const std::string&)>& log,
                           const std::atomic<bool>& cancel) {
    return select_frames({FrameSelectTrack{cand_dir, out_dir, prefix}}, false,
                         options, log, cancel);
}

int select_synchronized_frames(
    const std::vector<FrameSelectTrack>& tracks,
    const FrameSelectOptions& options,
    const std::function<void(const std::string&)>& log,
    const std::atomic<bool>& cancel) {
    return select_frames(tracks, true, options, log, cancel);
}

}  // namespace app
