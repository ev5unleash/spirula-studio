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

// Splits a flag or command string the way a shell would for the simple cases:
// whitespace separates (a line break included), "quoted runs" stay together,
// and a backslash before a break continues the line instead of being an arg.
std::vector<std::string> split_args(const std::string& s);

// A MESSAGE made safe to hand to another program: no backslash, no straight
// quote, no control character, so it needs no escaping in a JSON payload or a
// command line. Quotes curl (” ’), a backslash becomes '/', spaces collapse.
std::string safe_arg(const std::string& text);

// split_args(command) with every `token` inside an argument replaced by
// safe_arg(value). The value lands in ONE argument whether or not the token
// was quoted, so nothing in it can be read as syntax.
std::vector<std::string> command_argv(const std::string& command,
                                      const std::string& token,
                                      const std::string& value);

inline bool open_url(const std::string& url) {
    return app::proc::open_url(url);
}

}  // namespace gui
