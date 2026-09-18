// frames_stamp -- what a workspace records about the pictures in its images/
// (app/DatasetPrep.h). Changing how a 360 capture is unwrapped used to
// leave the old frames in place, so the run picked up at feature matching and
// reconstructed views nobody had asked for.

#include "app/DatasetPrep.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void expect(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "ok  " : "BAD ", what.c_str());
    if (!ok) g_failures++;
}

app::PrepJob two_clips() {
    app::PrepJob job;
    job.workspace = "/tmp/x";
    job.video_fps = 2.0f;
    app::PrepInput a, b;
    a.path = "/a.360";
    a.is_video = true;
    b.path = "/b.mp4";
    b.is_video = true;
    job.inputs = {a, b};
    return job;
}

void moves(const char* what, void (*edit)(app::PrepJob&)) {
    app::PrepJob before = two_clips();
    app::PrepJob after = two_clips();
    edit(after);
    expect(!app::recon_stamp_change(app::frames_stamp(before),
                                    app::frames_stamp(after)).empty(),
           std::string("a moved ") + what + " makes the frames stale");
}

}  // namespace

int main() {
    const app::PrepJob job = two_clips();
    expect(app::recon_stamp_change(app::frames_stamp(job),
                                   app::frames_stamp(job)).empty(),
           "an unchanged job matches its own stamp");

    moves("unwrap mode", [](app::PrepJob& j) {
        j.pano.mode = app::Pano360Mode::Equirect;
    });
    moves("orientation", [](app::PrepJob& j) { j.pano.roll = 180.0f; });
    moves("rate", [](app::PrepJob& j) { j.video_fps = 4.0f; });
    moves("per-video rate", [](app::PrepJob& j) { j.inputs[1].fps = 6.0f; });
    moves("adaptive switch", [](app::PrepJob& j) { j.adaptive_fps = true; });
    moves("sharpness window", [](app::PrepJob& j) { j.sharp_window = 5; });
    moves("input list", [](app::PrepJob& j) { j.inputs.pop_back(); });

    // And what does NOT: masking and the reconstruction stamp their own
    // settings, so a prompt must not cost the extraction as well.
    {
        app::PrepJob after = two_clips();
        after.mask_enable = true;
        after.mask_prompt = "people";
        expect(app::recon_stamp_change(app::frames_stamp(job),
                                       app::frames_stamp(after)).empty(),
               "a masking prompt leaves the frames alone");
    }

    // "The same as above" chains down the list; the first row takes the job's.
    {
        app::PrepJob j = two_clips();
        j.inputs[0].fps = 6.0f;
        expect(app::input_fps(j.inputs, j.video_fps, 0) == 6.0f,
               "a row that states a rate uses it");
        expect(app::input_fps(j.inputs, j.video_fps, 1) == 6.0f,
               "the row below follows it");
        expect(app::fps_group(j.inputs, 1) == 0,
               "and is in its group");
        j.inputs[1].fps = 1.0f;
        expect(app::input_fps(j.inputs, j.video_fps, 1) == 1.0f,
               "a row that states its own wins");
        expect(app::fps_group(j.inputs, 1) == 1, "and opens a group");
        app::PrepJob plain = two_clips();
        expect(app::input_fps(plain.inputs, plain.video_fps, 1) == 2.0f,
               "a list that states nothing is all on the dataset's rate");
    }

    std::printf(g_failures ? "\nFAILED: %d\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
