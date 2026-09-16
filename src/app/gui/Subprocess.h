#pragma once

#include "app/Subprocess.h"

namespace gui {

constexpr int kSpawnFailed = -1;
constexpr int kCancelled   = -2;

inline int run_process(const std::vector<std::string>& argv,
                       const std::string& cwd,
                       const std::function<void(const std::string&)>& on_line,
                       const std::atomic<bool>& cancel) {
    app::proc::ProcessOptions opts;
    opts.argv = argv;
    opts.cwd = cwd;
    opts.on_line = on_line;
    opts.cancel = &cancel;
    app::proc::ProcessResult res = app::proc::run_process(opts);
    if (res.outcome == app::proc::ProcessOutcome::Cancelled) return kCancelled;
    if (res.outcome == app::proc::ProcessOutcome::SpawnFailed) return kSpawnFailed;
    return res.exit_code;
}

inline bool command_exists(const std::string& exe) {
    return app::proc::command_exists(exe);
}

inline std::vector<std::string> split_args(const std::string& s) {
    return app::proc::split_args(s);
}

inline bool open_url(const std::string& url) {
    return app::proc::open_url(url);
}

}  // namespace gui
