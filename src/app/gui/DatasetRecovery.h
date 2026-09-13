#pragma once

// DatasetRecovery -- the dataset job the screen was running when the process
// died, published as one record under the config directory.
//
// Only stage granularity is recovered: the record names the running stage and
// a resumed run restarts it; nothing inside a GPU kernel is checkpointed. The
// record is written by temp file + atomic replace, so a failed write leaves
// the previous valid record in place.

#include "app/gui/ColmapRunner.h"
#include "app/gui/PrepProgress.h"
#include "app/gui/SfmRunner.h"

#include <array>
#include <optional>
#include <string>

namespace gui::dataset_recovery {

// Everything a run needs to be reproduced except the engine's own artifacts,
// which `resume` reuses from the workspace.
struct State {
    int engine = 0;              // 0 built-in (SfmJob), 1 COLMAP (ColmapJob)
    Stage current = Stage::Frames;
    std::array<bool, kNumStages> completed{};
    // Both are kept so the GUI can populate either panel; save() serializes
    // only the selected engine's job.
    SfmJob sfm;
    ColmapJob colmap;
};

// The published record, or nullopt when there is none, it is malformed, or it
// was written by a version this build does not understand.
std::optional<State> load();

// Publish `state` atomically. False with `error` set when the record could not
// be written; the previously published record is then still on disk.
bool save(const State& state, std::string& error);

// Forget the record. Never throws.
void clear() noexcept;

}  // namespace gui::dataset_recovery
