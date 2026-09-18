#include "app/gui/BatchProcess.h"
#include "app/JobScheduler.h"
#include "app/gui/TrainPreset.h"
#include "i18n/catalog/Gui.h"
#include <algorithm>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        failures += !ok;
    };
    try {
        if (argc != 2 || !fs::is_regular_file(fs::u8path(argv[1])))
            throw std::runtime_error("batch_process_test needs the built spirula executable");
        const fs::path root = fs::current_path() / "batch-fixture";
        fs::create_directories(root / "dataset");
        std::ofstream(root / "dataset" / "transforms.json") << "{\"frames\":[]}";
        gui::BatchRow row;
        row.dataset = (root / "dataset").u8string();
        row.output_dir = (root / "output").u8string();
        row.device = "gpu-job";
        row.runs.push_back({});
        row.runs[0].iterations = "17";
        row.runs[0].cap_max = "1024";
        row.runs[0].sh_degree = "0";
        gui::TrainPreset saved;
        saved.cfg.num_iterations = 40;
        saved.cfg.means_lr = 0.004f;
        saved.touched = {"num_iterations", "means_lr"};
        row.runs[0].preset.path = (root / "preset.json").u8string();
        gui::save_preset(saved, row.runs[0].preset.path);
        TrainConfig cfg;
        std::string preset, error;
        check(gui::batch_build_train_config(row, 0, row.dataset, "images", "masks", true,
                                            cfg, preset, error), "resolves batch overrides");
        check(cfg.num_iterations == 17 && cfg.cap_max == 1024 && cfg.sh_degree == 0 &&
                  cfg.means_lr == 0.004f && cfg.disable_viewer && cfg.flip_mask,
              "explicit overrides and mask polarity survive preset resolution");
        const auto frozen = gui::batch_config_args(cfg);
        app::sched::JobScheduler scheduler((root / "queue").u8string(), argv[1]);
        scheduler.pause_dispatch(true);
        scheduler.set_foreground_device("gpu-desktop");
        app::sched::SubmitOpts opts;
        opts.phase = "train";
        opts.device = row.device;
        opts.work_dir = root.u8string();
        opts.args = frozen;
        const auto id = scheduler.submit(opts);
        check(!id.empty(), "submits resolved row without opening a GUI");
        row.runs[0].iterations = "99";
        row.device = "gpu-later";
        cfg.num_iterations = 99;
        const auto jobs = scheduler.list();
        auto value = [](const std::vector<std::string>& args, const std::string& flag) {
            const auto it = std::find(args.begin(), args.end(), flag);
            return it != args.end() && it + 1 != args.end() ? *(it + 1) : std::string();
        };
        check(jobs.size() == 1 && jobs[0].device == "gpu-job" &&
                  value(jobs[0].phases[0].args, "--num-iterations") == "17" &&
                  value(jobs[0].phases[0].args, "--means-lr") == value(frozen, "--means-lr"),
              "editing source row does not change submitted options or target");
        check(!scheduler.try_reserve_foreground_device("gpu-other"),
              "job submission preserves the independent foreground reservation");
        scheduler.cancel(id);
        const auto cancelled = scheduler.list();
        check(cancelled.size() == 1 && cancelled[0].state == app::sched::JobState::Stopped,
              "queued cancellation reaches a terminal stopped state");
        row.runs[0].iterations = "17oops";
        check(!gui::batch_build_train_config(row, 0, row.dataset, "", "", false,
                                             cfg, preset, error),
              "invalid typed override cannot become an executable configuration");
        gui::BatchCapabilities caps;
        caps.device = true;
        caps.builtin_sfm = true;
        row.stages[0] = false;
        row.stages[1] = false;
        row.stages[2] = true;
        check(gui::batch_has_error(gui::batch_check_row(row, {row}, 0, caps)),
              "unsupported scheduled meshing is rejected before submission");
        row.stages[2] = false;
        row.stages[0] = true;
        row.sources = {(root / "missing-input").u8string()};
        check(gui::batch_has_error(gui::batch_check_row(row, {row}, 0, caps)),
              "missing dataset source fails preflight");
        const auto conflicts = gui::batch_check_row(row, {row, row}, 0, caps);
        check(std::any_of(conflicts.begin(), conflicts.end(), [](const auto& issue) {
                  return issue.fatal && issue.text == &spirula::i18n::msg::gui::chk_dataset_collision;
              }), "competing dataset writers are rejected before launch");
        scheduler.shutdown();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL batch scenario: %s\n", e.what());
        ++failures;
    }
    return failures ? 1 : 0;
}
