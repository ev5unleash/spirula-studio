#pragma once

// Resume support: everything needed to continue training from a saved run,
// short of the engine-state restore itself (`engine_load_checkpoint()` in
// engine/EngineCheckpoint.cpp does that, and is already native).
//
// A run directory holds `config.json` plus one `step-%09d.ckpt/` per save; a
// checkpoint directory holds `state.tar`, whose `state.json` member is the
// runtime manifest. Resolution accepts either, so `--resume <run_dir>` picks
// the latest checkpoint and `--resume <run_dir>/step-000030000.ckpt` pins one.

#include "config/TrainConfig.h"
#include "data/Json.h"

#include <filesystem>
#include <set>
#include <string>

namespace ckpt {

struct ResolvedCheckpoint {
    std::filesystem::path run_dir;    // holds config.json
    std::filesystem::path ckpt_dir;   // holds state.tar
    std::filesystem::path config_path; // paired snapshot, or legacy run config
};

// Numeric step parsed from the exact step-%09d.ckpt spelling, or -1.
int checkpoint_step(const std::filesystem::path& path);

// Accepts a run directory or a step-*.ckpt directory. Run directories choose
// the newest valid full-state checkpoint; explicit checkpoint paths are pinned.
ResolvedCheckpoint resolve_checkpoint(const std::filesystem::path& path);

// The state.json member of ckpt_dir/state.tar.
JsonValue read_state_json(const std::filesystem::path& ckpt_dir);

// Validate structural resumability and return the parsed state manifest.
JsonValue validate_checkpoint(const std::filesystem::path& ckpt_dir,
                              bool require_full);

// A run's config.json -> TrainConfig. Keys absent from the file keep the
// field's default, so a config.json written by an older build still loads.
TrainConfig config_from_json(const std::filesystem::path& config_json);

// The effective config for a resumed run:
//   checkpoint's config.json
//     + the resume path itself
//     + output dir defaulting back to the checkpoint's own run folder
//     + `preset`'s overrides, if a preset was named on the command line
//     + every flag the user passed explicitly (those always win)
//
// `explicit_flags` holds CLI keys as spelled in the field table; the CLI
// parser records them. Comparing against defaults instead -- what the Python
// implementation had to do, because tyro does not report which flags were
// seen -- silently ignores a flag whose value happens to equal the default.
TrainConfig build_resume_config(const TrainConfig& cli,
                                 const std::string& preset,
                                 const std::set<std::string>& explicit_flags);

}  // namespace ckpt
