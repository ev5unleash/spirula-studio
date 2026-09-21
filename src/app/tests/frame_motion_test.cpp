// frame_motion -- the adaptive frame plan and Pano360's source scoring
// regions. Motion errors are silent: the count has to land on the budget, the
// gaps have to stay inside the rate bounds, and a burst of motion must not
// swallow the budget it cannot spend.

#include "app/FrameMotion.h"
#include "app/Pano360.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "ok  " : "BAD ", what.c_str());
    if (!ok) g_failures++;
}

// One sample per source frame, so the plan is in the units the test states.
std::vector<int64_t> indices(size_t n) {
    std::vector<int64_t> v(n);
    for (size_t i = 0; i < n; i++) v[i] = (int64_t)i;
    return v;
}

void check_plan(const char* name, const std::vector<float>& cost, int skip,
                int window, float range, int64_t want) {
    const std::vector<int64_t> at = indices(cost.size());
    const std::vector<int64_t> plan = app::plan_by_motion(
        cost, at, (int64_t)cost.size(), skip, window, range, 0);
    const std::string tag(name);
    // Bisection lands on the budget or just under it; a tenth either way is
    // the granularity of a plan that can only place frames on samples.
    expect((int64_t)plan.size() <= want &&
               (int64_t)plan.size() >= want - want / 10 - 1,
           tag + ": " + std::to_string(plan.size()) + " frames for a budget of " +
               std::to_string(want));
    const int64_t min_gap = std::max<int64_t>(window, (int64_t)(skip / range));
    const int64_t max_gap = std::max<int64_t>(min_gap, (int64_t)(skip * range));
    int64_t tight = (int64_t)cost.size(), wide = 0;
    for (size_t i = 1; i < plan.size(); i++) {
        const int64_t gap = plan[i] - plan[i - 1];
        tight = std::min(tight, gap);
        wide = std::max(wide, gap);
    }
    expect(plan.size() < 2 || (tight >= min_gap && wide <= max_gap),
           tag + ": gaps " + std::to_string(tight) + ".." + std::to_string(wide) +
               " within " + std::to_string(min_gap) + ".." + std::to_string(max_gap));
}
void check_pano360_score_regions() {
    app::Pano360Layout eac;
    eac.packing = app::Pano360Packing::Eac;
    eac.track_w = 28;
    eac.track_h = 8;
    eac.face = 8;
    eac.strip = 2;
    const app::Pano360ScoreRegions eac0 = app::pano360_score_regions(eac, 0);
    const app::Pano360ScoreRegions eac1 = app::pano360_score_regions(eac, 1);
    expect(eac0.count == 3 && eac1.count == 3, "EAC: three regions per track");
    expect(eac0.packed_width == eac.canvasW() &&
               eac0.packed_height == eac.track_h,
           "EAC: packed dimensions equal one assembled canvas row");
    const int want_x[] = {0, 6, 24};
    const int want_w[] = {4, 16, 4};
    int covered[28] = {};
    int area = 0;
    for (int i = 0; i < eac0.count; i++) {
        const app::Pano360ScoreRegion& r = eac0.regions[i];
        expect(r.x == want_x[i] && r.y == 0 && r.width == want_w[i] &&
                   r.height == eac.track_h,
               "EAC: source slices match canvas assembly cuts");
        area += r.width * r.height;
        for (int x = r.x; x < r.x + r.width; x++) covered[x]++;
        expect(eac1.regions[i].x == r.x && eac1.regions[i].width == r.width &&
                   eac1.regions[i].height == r.height,
               "EAC: both tracks share the same valid slices");
    }
    expect(area == eac0.packed_width * eac0.packed_height,
           "EAC: packed area equals the valid source area");
    for (int x = 0; x < eac.track_w; x++)
        expect(covered[x] == ((x < 4 || (x >= 6 && x < 22) || x >= 24) ? 1 : 0),
               "EAC: overlap halves are excluded exactly");

    app::Pano360Layout sphere;
    sphere.packing = app::Pano360Packing::Sphere;
    sphere.track_w = 40;
    sphere.track_h = 12;
    sphere.face = 8;
    sphere.margin = 4;
    const app::Pano360ScoreRegions s0 = app::pano360_score_regions(sphere, 0);
    const app::Pano360ScoreRegions s1 = app::pano360_score_regions(sphere, 1);
    expect(s0.count == 1 && s1.count == 0,
           "Sphere: only track zero's central crop is valid");
    if (s0.count == 1) {
        const app::Pano360ScoreRegion& r = s0.regions[0];
        expect(r.x == sphere.margin && r.y == 0 && r.width == sphere.canvasW() &&
                   r.height == sphere.track_h,
               "Sphere: region is the central canvas crop");
        expect(r.x > 0 && r.x + r.width < sphere.track_w,
               "Sphere: side padding is excluded");
        expect(r.width * r.height == s0.packed_width * s0.packed_height,
               "Sphere: packed area equals the valid source area");
    }
}


}  // namespace

int main() {
    // Even motion: the plan should be the fixed schedule in all but name.
    {
        const std::vector<float> cost(600, 0.03f);
        check_plan("even", cost, 15, 3, 4.0f, 40);
        const std::vector<int64_t> plan = app::plan_by_motion(
            cost, indices(cost.size()), 600, 15, 3, 4.0f, 0);
        int64_t wide = 0, tight = 600;
        for (size_t i = 1; i < plan.size(); i++) {
            wide = std::max(wide, plan[i] - plan[i - 1]);
            tight = std::min(tight, plan[i] - plan[i - 1]);
        }
        expect(wide - tight <= 1, "even: the spacing stays even");
    }

    // Half the clip still, half moving: the moving half takes the frames.
    {
        std::vector<float> cost(600, 0.002f);
        for (size_t i = 300; i < 600; i++) cost[i] = 0.08f;
        check_plan("split", cost, 15, 3, 4.0f, 40);
        const std::vector<int64_t> plan = app::plan_by_motion(
            cost, indices(cost.size()), 600, 15, 3, 4.0f, 0);
        int first = 0;
        for (int64_t at : plan) first += at < 300 ? 1 : 0;
        expect(first * 2 < (int)plan.size() - first,
               "split: the moving half gets more than twice the frames");
    }

    // A burst one sample wide: its budget cannot be spent inside it, and the
    // rest of the clip must not be starved for it.
    {
        std::vector<float> cost(600, 0.01f);
        cost[200] = 40.0f;
        check_plan("burst", cost, 15, 3, 4.0f, 40);
    }

    // A tripod: nothing changes, so nothing justifies more than the slowest
    // rate the bounds allow.
    {
        const std::vector<float> cost(600, 0.0f);
        const std::vector<int64_t> plan = app::plan_by_motion(
            cost, indices(cost.size()), 600, 15, 3, 4.0f, 0);
        expect((int)plan.size() <= 600 / 60 + 1,
               "still: " + std::to_string(plan.size()) +
                   " frames, at most the slowest rate");
    }

    // The cap is a cap, not a target.
    {
        const std::vector<float> cost(600, 0.03f);
        const std::vector<int64_t> plan = app::plan_by_motion(
            cost, indices(cost.size()), 600, 15, 3, 4.0f, 12);
        expect((int)plan.size() <= 12, "cap: " + std::to_string(plan.size()) +
                                           " frames within a cap of 12");
        expect((int)plan.size() >= 10, "cap: the cap is nearly filled");
    }

    // Two clips on one rate: the budget is theirs together, so the one that
    // moves takes it off the one that does not.
    {
        std::vector<app::MotionPlanInput> in(2);
        for (int k = 0; k < 2; k++) {
            in[k].spans = {{0, 0, 599}};
            in[k].steps.reserve(600);
            for (int64_t i = 0; i < 600; i++)
                in[k].steps.push_back({{0, i}, k == 0 ? 0.002f : 0.06f});
            in[k].frames = 600;
            in[k].skip = 15;
            in[k].window = 3;
        }
        const std::vector<app::FramePlan> got =
            app::plan_by_motion(in, 4.0f);
        const int a = (int)got[0].size(), b = (int)got[1].size();
        expect(a + b <= 80 && a + b >= 70,
               "shared: " + std::to_string(a + b) + " frames for a budget of 80");
        expect(b > 2 * a, "shared: the clip that moves gets more than twice");
        // And a rate that is still each clip's own, whatever it spends.
        expect(a >= 600 / 60, "shared: the still clip keeps its slowest rate");
        expect(b <= 600 / 3 + 1, "shared: the moving clip keeps its fastest rate");
    }
    // Adjacent ordinals at a discontinuity stay separate identities. Each
    // segment has enough frames for its own sharpness window, but no window
    // may borrow the boundary frame from its neighbour.
    {
        app::MotionPlanInput segmented;
        segmented.spans = {{0, 0, 2}, {1, 3, 5}};
        segmented.steps = {};
        segmented.steps.push_back({{0, 2}, 0.9f});
        segmented.steps.push_back({{1, 5}, 0.9f});
        segmented.frames = 6;
        segmented.skip = 1;
        segmented.window = 3;
        const std::vector<app::FramePlan> plans =
            app::plan_by_motion({segmented}, 4.0f);
        expect(plans.size() == 1 && plans[0].size() == 2,
               "segments: one plan entry per usable segment");
        if (!plans.empty() && plans[0].size() == 2) {
            expect(plans[0][0] == app::FramePosition{0, 2} &&
                       plans[0][1] == app::FramePosition{1, 5},
                   "segments: selected identities retain segment and ordinal");
            expect(plans[0][0].segment != plans[0][1].segment,
                   "segments: boundary is never flattened into one timeline");
            expect(plans[0][0].ordinal <= segmented.spans[0].last_ordinal &&
                       plans[0][1].ordinal >= segmented.spans[1].first_ordinal +
                                                     segmented.window - 1,
                   "segments: windows stay inside their segment spans");
        }
    }

    // A segment shorter than the sharpness window cannot borrow frames from a
    // neighbouring segment just because its ordinals are adjacent.
    {
        app::MotionPlanInput segmented;
        segmented.spans = {{0, 0, 1}, {1, 2, 5}};
        segmented.steps = {};
        segmented.steps.push_back({{0, 1}, 0.9f});
        segmented.steps.push_back({{1, 4}, 0.9f});
        segmented.skip = 1;
        segmented.window = 3;
        const std::vector<app::FramePlan> plans =
            app::plan_by_motion({segmented}, 4.0f);
        expect(plans.size() == 1 && !plans[0].empty(),
               "short segment: longer neighbour remains plannable");
        for (const app::FramePosition& position : plans[0].frames)
            expect(position.segment == 1 && position.ordinal >= 4,
                   "short segment: no plan/window crosses its boundary");
    }

    // Nothing to plan from.
    expect(app::plan_by_motion({}, {}, 0, 15, 3, 4.0f, 0).empty(),
           "empty: no samples, no plan");
    check_pano360_score_regions();

    std::printf("%s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}
