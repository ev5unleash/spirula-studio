// The quality policy is small enough to test without decoding images. These
// cases pin its range, threshold, exclusion, rescue, and tie rules.

#include "app/FrameSelect.h"

#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "ok  " : "BAD ", what.c_str());
    if (!ok) g_failures++;
}

void check(const char* name, const std::vector<double>& scores, size_t begin,
           size_t end, int rescue, const std::vector<uint8_t>& excluded,
           float floor, int64_t index, app::FrameSelectionKind kind) {
    const app::FrameSelectChoice got = app::choose_frame_candidate(
        scores.data(), scores.size(), begin, end, rescue, excluded.data(), floor);
    const std::string tag(name);
    expect(got.index == index, tag + ": index " + std::to_string(got.index));
    expect(got.kind == kind, tag + ": outcome");
}


void check_synchronized(const char* name,
                        const std::vector<double>& first,
                        const std::vector<double>& second, size_t begin,
                        size_t end, int rescue, float floor, int64_t index,
                        app::FrameSelectionKind kind) {
    const std::vector<const std::vector<double>*> scores{&first, &second};
    const app::FrameSelectChoice got = app::choose_synchronized_candidate(
        scores, begin, end, rescue, nullptr, floor);
    const std::string tag(name);
    expect(got.index == index, tag + ": index " + std::to_string(got.index));
    expect(got.kind == kind, tag + ": outcome");
}

}  // namespace

int main() {

    // The best qualifying candidate in the primary half-open range wins.
    check("primary", {1.0, 8.0, 5.0, 7.0}, 1, 4, 0,
          std::vector<uint8_t>(4, 0), 6.0f, 1,
          app::FrameSelectionKind::Accepted);

    // A failed primary may rescue only the bounded earlier candidates.
    check("rescue", {4.0, 8.0, 1.0, 2.0}, 2, 4, 2,
          std::vector<uint8_t>(4, 0), 3.0f, 1,
          app::FrameSelectionKind::Rescued);
    check("rescue-bound", {9.0, 1.0, 1.0, 1.0}, 2, 4, 1,
          std::vector<uint8_t>(4, 0), 5.0f, -1,
          app::FrameSelectionKind::Rejected);

    // Non-finite and below-floor candidates leave the interval rejected.
    check("reject", {std::numeric_limits<double>::quiet_NaN(), 2.0, 3.0}, 0, 3,
          0, std::vector<uint8_t>(3, 0), 4.0f, -1,
          app::FrameSelectionKind::Rejected);

    // An excluded high scorer is not reused when retrying the same interval.
    std::vector<uint8_t> excluded(3, 0);
    excluded[0] = 1;
    check("excluded", {9.0, 7.0, 1.0}, 0, 3, 0, excluded, 0.0f, 1,
          app::FrameSelectionKind::Accepted);

    // Equal scores keep the earliest candidate.
    check("tie", {5.0, 5.0, 4.0}, 0, 3, 0,
          std::vector<uint8_t>(3, 0), 0.0f, 0,
          app::FrameSelectionKind::Accepted);

    // Synchronized selection ranks by the weakest track while retaining one
    // common ordinal.
    check_synchronized(
        "sync-common", {2.0, 9.0, 8.0, 7.0}, {3.0, 4.0, 8.0, 6.0}, 0, 4, 0,
        0.0f, 2, app::FrameSelectionKind::Accepted);
    check_synchronized(
        "sync-floor", {8.0, 8.0}, {4.0, 4.0}, 0, 2, 0, 5.0f, -1,
        app::FrameSelectionKind::Rejected);
    check_synchronized(
        "sync-rescue", {1.0, 7.0, 2.0}, {1.0, 8.0, 2.0}, 2, 3, 2, 5.0f, 1,
        app::FrameSelectionKind::Rescued);
    const std::vector<double> mismatch{1.0};
    const std::vector<double> mismatch_other{1.0, 2.0};
    const std::vector<const std::vector<double>*> mismatched{
        &mismatch, &mismatch_other};
    expect(app::choose_synchronized_candidate(mismatched, 0, 2, 0, nullptr,
                                              0.0f)
                   .index < 0,
           "sync-mismatch: candidate counts reject");

    uint64_t cap = 0;
    expect(app::checked_candidate_cap(12, 3, cap) && cap == 36,
           "cap: checked product");
    expect(app::checked_candidate_cap(0, 3, cap) && cap == 0,
           "cap: zero max is unlimited");
    expect(!app::checked_candidate_cap(std::numeric_limits<uint64_t>::max(), 2,
                                       cap),
           "cap: overflow rejects");
    double rate = 0.0;
    expect(app::checked_candidate_rate(30.0, 10.0, 100, rate) &&
               rate == 10.0,
           "rate: duration bounds nominal output");
    app::FrameSelectionSettings settings;
    settings.adaptive = true;
    settings.adaptive_range =
        std::numeric_limits<float>::infinity();
    int group = 0;
    expect(!app::checked_candidate_group(settings, group),
           "group: non-finite adaptive range rejects");

    std::printf("%s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}
