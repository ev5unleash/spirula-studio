#pragma once

// WorkerRequest -- one scheduled phase, frozen at submission. Schema and
// result contract in docs/notes/device-job-scheduling-plan.md §3.5.

#include <string>
#include <vector>

namespace app::worker {

struct Request {
    int schema_version = 0;
    std::string request_path; // source JSON path; not part of the schema
    std::string job_id;
    std::string attempt_id;
    std::string phase;        // "train" | "sfm" | "geometry"
    std::string device;       // resolved device identity
    std::string work_dir;     // cwd for the phase
    std::string result_path;  // where to leave the result JSON
    std::vector<std::string> args;  // argv tokens past the tool name
};

struct Result {
    std::string outcome;      // success | failed | stopped | spawn_failed
    int exit_code = -1;
    std::string message;
};

// Throws std::runtime_error on any structural problem.
Request parse_request(const std::string& path);

void validate_request(const Request& r);

// tmp-write + rename in the same directory. False on I/O failure.
bool publish_result(const Request& r, const Result& res);

}  // namespace app::worker
