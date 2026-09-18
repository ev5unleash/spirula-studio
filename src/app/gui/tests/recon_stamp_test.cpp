// recon_stamp -- the workspace stamp round-trip (app/ReconStamp.h), which
// is what decides whether a finished reconstruction is kept or built again.
// A value holding newlines -- the manifest, which every rig or telemetry
// capture has -- used to be dropped, so such a stamp never matched its own run.

#include "app/ReconStamp.h"

#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void expect(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "BAD ", what);
    if (!ok) g_failures++;
}

}  // namespace

int main() {
    const fs::path ws = fs::temp_directory_path() / "spirula_recon_stamp_test";
    std::error_code ec;
    fs::remove_all(ws, ec);
    fs::create_directories(ws, ec);

    app::ReconStamp now;
    now.present = true;
    now.engine = "builtin";
    now.args = {"--quality", "high", "--data-type", "video",
                "--manifest",
                "image_dir: /x/images\ncaptures:\n- prefix: \"\"\n"
                "  telemetry: /x/a.mp4\n",
                "--metric-gps", "horizontal", "--no-masks"};
    app::write_recon_stamp(ws.string(), now);

    const app::ReconStamp back = app::read_recon_stamp(ws.string());
    expect(back.present, "a written stamp reads back as present");
    expect(back.engine == now.engine, "the engine round-trips");
    expect(back.args == now.args, "a value holding newlines round-trips");
    expect(app::recon_stamp_change(back, now).empty(),
           "an unchanged run matches its own stamp");

    app::ReconStamp other = now;
    other.args[3] = "individual";
    expect(app::recon_stamp_change(back, other) == "--data-type",
           "a moved value names the flag it belongs to");
    expect(app::recon_stamp_change(app::ReconStamp{}, now).empty(),
           "a workspace with no stamp reports no change");

    fs::remove_all(ws, ec);
    std::printf(g_failures ? "\nFAILED: %d\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
