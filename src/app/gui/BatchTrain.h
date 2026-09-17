#pragma once

// Batch editor data and its pre-flight/config resolver. Execution belongs to
// app::sched::JobScheduler; rows retain only editable inputs and issues.
//
// A row that fails pre-flight is reported before anything is submitted. The
// resolver still owns preset loading, macro expansion and row overrides.

#include "config/TrainConfig.h"
#include "i18n/Message.h"

#include <string>
#include <vector>

namespace gui {

// One thing a pre-flight found. `text` is interface copy with an optional
// {0}; `raw` is engine text (a flag name, a parser message) that stays in
// English -- exactly the ui:: / ui::*Raw split. Exactly one of them is set.
struct BatchIssue {
    const spirula::i18n::Msg* text = nullptr;
    std::string arg;      // fills {0} in `text`
    std::string raw;      // the whole line, untranslated
    bool fatal = false;   // blocks the start; otherwise a warning
};

// The line to draw. Already translated and formatted, so it goes on screen
// through a ui::*Raw call.
std::string batch_issue_line(const BatchIssue& issue);

struct BatchJob {
    std::string dataset;
    // "" means the built-in preset `preset_name`; otherwise the preset file at
    // this path, and `preset_name` is what it calls itself (for display).
    std::string preset_path;
    std::string preset_name = "3dgs";
    // Where runs go. "" = <dataset>/outputs, the same default the trainer
    // screen picks when a dataset is opened. Each run still lands in its own
    // timestamped subfolder of this, so two rows sharing one folder is fine.
    std::string output_dir;
    std::string device;          // canonical target; empty = inherit, "auto" = row Auto

    // Per-row overrides of the three flags that get changed often enough that
    // making a whole preset for each combination is the wrong shape of work.
    // "" means "whatever the preset says".
    //
    // Held as TEXT, not as ints: "unset" and "0" are different answers and an
    // integer box cannot show the difference -- 0 is a legal --sh-degree. The
    // pre-flight reports anything that is not a usable number, so a typo is
    // caught before the queue starts rather than silently rounded to a
    // default.
    std::string cap_max_override;
    std::string sh_degree_override;
    std::string iterations_override;
    // Runtime association; scheduler state remains authoritative.
    std::string scheduler_id;

    // From the last batch_check(); empty until one has run.
    std::vector<BatchIssue> issues;
};

// True if any issue found on this row blocks the start.
bool batch_has_error(const BatchJob& job);

// Check one row. `all`/`index` are for the checks that are about the list
// rather than the row -- another row that would do exactly the same work.
// Does no GPU work and touches nothing on disk.
std::vector<BatchIssue> batch_check(const BatchJob& job,
                                    const std::vector<BatchJob>& all,
                                    int index);

// The config this row would train with: the preset, then the row's dataset and
// output folder, then the macro options resolved the way the trainer screen
// resolves them. `preset_base` comes back as the built-in preset name to
// record in the run's config.json. Returns false and fills `error` (English)
// when the preset cannot be read -- the same condition batch_check() reports,
// re-checked here because the file can go away in between.
bool batch_build_config(const BatchJob& job, TrainConfig& cfg,
                        std::string& preset_base, std::string& error);

// Serialize a resolved config for `spirula worker`; every field is explicit so
// worker parsing cannot reapply a macro or inherit GUI state.
std::vector<std::string> batch_config_args(const TrainConfig& cfg);

// The list, kept across sessions in <config_dir>/batch.json. A queue that is
// worth setting up is worth surviving a restart, and losing it to a crash
// three hours into a five-dataset run is the one failure the user cannot
// recover from by trying again. Neither call throws; a list that cannot be
// read comes back empty.
std::vector<BatchJob> load_batch_list();
void save_batch_list(const std::vector<BatchJob>& jobs);

}  // namespace gui
