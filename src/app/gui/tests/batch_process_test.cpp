#include "app/gui/BatchProcess.h"
#include "app/DatasetPrep.h"
#include "app/AppPaths.h"
#include "app/JobScheduler.h"
#include "data/ProjectManifest.h"
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
        row.scheduler_ids = {"job-first", "job-second"};
        row.scheduler_active = true;
        fs::create_directories(fs::u8path(app::config_dir()));
        gui::save_batch_list({row});
        const auto persisted = gui::load_batch_list();
        check(persisted.size() == 1 &&
                  persisted[0].scheduler_ids == row.scheduler_ids &&
                  persisted[0].scheduler_active,
              "active scheduler ownership survives batch reload");
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
        const fs::path provenance_root = root / "provenance";
        fs::create_directories(provenance_root / "images" / "cam");
        const fs::path original = provenance_root / "capture.bin";
        const fs::path exact_output =
            provenance_root / "images" / "cam" / "exact.jpg";
        const fs::path fallback_output =
            provenance_root / "images" / "cam" / "fallback.jpg";
        std::ofstream(original, std::ios::binary) << "original";
        std::ofstream(exact_output, std::ios::binary) << "exact";
        std::ofstream(fallback_output, std::ios::binary) << "fallback";

        app::PrepCapture capture;
        capture.subdir = "cam";
        capture.path = original.u8string();
        capture.source_id =
            spirula::project::make_source_record(original).source_id;
        app::PrepFrameSource exact;
        exact.output_name = "cam/exact.jpg";
        exact.output_source_id =
            spirula::project::make_source_record(exact_output).source_id;
        exact.export_ordinal = 9007199254740993ULL;
        exact.timing.kind = video::TimingKind::SourceExact;
        exact.timing.presentation_ordinal = 9007199254740993ULL;
        exact.timing.decode_ordinal = 9007199254740994ULL;
        exact.timing.pts = 9007199254740993LL;
        exact.timing.has_pts = true;
        exact.timing.time_base_num = 1;
        exact.timing.time_base_den = 90000;
        app::PrepFrameSource fallback;
        fallback.output_name = "cam/fallback.jpg";
        fallback.output_source_id =
            spirula::project::make_source_record(fallback_output).source_id;
        fallback.export_ordinal = 7;
        fallback.timing.kind = video::TimingKind::ExportDerived;
        capture.frames = {exact, fallback};

        std::string provenance_error;
        check(app::write_prep_provenance(
                  provenance_root.u8string(), {capture}, provenance_error),
              "publishes exact and fallback frame provenance");
        std::vector<app::PrepCapture> loaded;
        check(app::read_prep_provenance(
                  app::prep_provenance_path(provenance_root.u8string()),
                  loaded, provenance_error) &&
                  loaded.size() == 1 && loaded[0].frames.size() == 2 &&
                  loaded[0].frames[0].timing.presentation_ordinal ==
                      9007199254740993ULL &&
                  loaded[0].frames[1].timing.kind ==
                      video::TimingKind::ExportDerived,
              "round-trips exact integers and unsupported timing");
        std::ofstream(exact_output, std::ios::binary | std::ios::trunc)
            << "changed";
        check(!app::read_prep_provenance(
                  app::prep_provenance_path(provenance_root.u8string()),
                  loaded, provenance_error),
              "refuses provenance after an output changes");
        const fs::path resume_root = root / "provenance-resume";
        std::error_code resume_cleanup;
        fs::remove_all(resume_root, resume_cleanup);
        const auto make_resumed_capture =
            [&](const char* name) {
                app::PrepCapture resumed;
                const fs::path source = resume_root / (std::string(name) + ".mp4");
                const fs::path image_dir = resume_root / "images" / name;
                fs::create_directories(image_dir);
                std::ofstream(source, std::ios::binary) << name;
                resumed.subdir = name;
                resumed.path = source.u8string();
                resumed.source_id =
                    spirula::project::make_source_record(source).source_id;
                for (int i = 0; i < 2; ++i) {
                    const fs::path output =
                        image_dir / (std::to_string(i) + ".jpg");
                    std::ofstream(output, std::ios::binary)
                        << name << i;
                    app::PrepFrameSource frame;
                    frame.output_name =
                        std::string(name) + "/" + std::to_string(i) + ".jpg";
                    frame.output_source_id =
                        spirula::project::make_source_record(output).source_id;
                    frame.export_ordinal = static_cast<std::uint64_t>(i + 1);
                    frame.timing.kind = video::TimingKind::ExportDerived;
                    resumed.frames.push_back(std::move(frame));
                }
                return resumed;
            };
        const app::PrepCapture resumed_one = make_resumed_capture("one");
        const app::PrepCapture resumed_two = make_resumed_capture("two");
        check(app::write_prep_provenance(
                  resume_root.u8string(), {resumed_one, resumed_two},
                  provenance_error),
              "writes a multi-video resume sidecar");
        app::PrepJob resume_job;
        resume_job.workspace = resume_root.u8string();
        resume_job.force_external_decode = true;
        app::PrepInput resume_input_one;
        resume_input_one.path = resumed_one.path;
        resume_input_one.is_video = true;
        resume_input_one.subdir = resumed_one.subdir;
        app::PrepInput resume_input_two;
        resume_input_two.path = resumed_two.path;
        resume_input_two.is_video = true;
        resume_input_two.subdir = resumed_two.subdir;
        resume_job.inputs = {resume_input_one, resume_input_two};
        std::atomic<bool> prep_cancel{false};
        app::DatasetPrep prep(prep_cancel, {});
        app::PrepResult resumed_result;
        std::string resumed_error;
        check(prep.run(resume_job, resumed_result, resumed_error),
              "resumes multiple videos from one verified sidecar");
        check(resumed_result.captures.size() == 2 &&
                  resumed_result.captures[0].path == resumed_one.path &&
                  resumed_result.captures[1].path == resumed_two.path,
              "multiple resumed videos reuse verified provenance");

        scheduler.shutdown();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL batch scenario: %s\n", e.what());
        ++failures;
    }
    return failures ? 1 : 0;
}
