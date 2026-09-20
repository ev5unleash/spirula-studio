#pragma once

// WorkerRequest -- one scheduled phase, JSON codec.
// Validation stops malformed requests before the worker changes state.

#include "app/DatasetPrep.h"

#include <cstdint>
#include <string>
#include <vector>

namespace app::worker {

// Non-empty references freeze one immutable revision and its exact file digest;
// metadata and artifact paths are project-relative, while revision locators
// remain in the revision file.
struct ProjectReference {
    std::string project_root;
    std::string project_revision;
    std::string project_revision_digest;
    std::vector<std::string> metadata_paths;
    std::vector<std::string> artifact_paths;
};

// Throws std::runtime_error unless the reference is empty or points at one
// complete, immutable manifest revision and its declared artifact set.
void validate_project_reference(const ProjectReference& reference);
ProjectReference make_project_reference(const std::string& project_root,
                                        const std::string& project_revision);

// A request is immutable once written by the scheduler.  `payload` is one
// phase-specific JSON object encoded as text; keeping it opaque here avoids a
// second phase registry in the worker boundary.
struct Request : ProjectReference {
    int schema_version = 2;
    std::string request_path; // source JSON path; not part of the schema
    std::string job_id;
    std::string attempt_id;
    std::string phase;        // prep | train | sfm-extract | sfm | geometry
    std::string device;       // resolved device identity
    std::string device_name;  // captured display name
    std::string work_dir;     // cwd for the phase
    std::string workspace;    // frozen mutable workspace, when applicable
    std::string result_path;  // where to leave the result JSON
    std::vector<std::string> args;  // argv tokens past the tool name
    std::string payload = "{}";     // phase-specific JSON object
};

struct Result {
    int schema_version = 2;
    std::string job_id;
    std::string attempt_id;
    std::string phase;
    std::string outcome;      // success | partial | nonmetric | failed | stopped | spawn_failed
    int exit_code = -1;
    std::string message;
    std::vector<std::string> outputs;  // validated/published artifacts
    bool partial = false;
    bool metric = true;
    int64_t registered = 0, images = 0, points = 0, models = 0;
    double mean_reprojection = 0.0;
    double median_reprojection = 0.0;
    std::string sparse_path;
};

// Throws std::runtime_error on any structural problem.
Request parse_request(const std::string& path);

void validate_request(const Request& r);

// Parse the worker's atomic result.  The request identity and phase are
// checked by the scheduler, not inferred from a human log.
Result parse_result(const std::string& path);

// tmp-write + rename in the same directory. False on I/O failure.
bool publish_result(const Request& r, const Result& res);

// Prep is the only phase whose options are not already native argv.  These
// functions are the one wire codec at the worker boundary; they intentionally
// carry every PrepJob input and option rather than falling back to ambient GUI
// state.
std::string serialize_prep_job(const app::PrepJob& job);
app::PrepJob deserialize_prep_job(const std::string& payload);

// Resolve all path-bearing fields exactly once at submit time. Relative
// inputs are resolved against `base_dir`; NULs are rejected and all emitted
// paths become absolute before the worker sees them.
bool freeze_prep_job(app::PrepJob& job, const std::string& base_dir,
                     std::string& error);

// Scheduled prep must never silently invoke reference/scripts/mask.py.
bool reject_external_masking(const app::PrepJob& job, std::string& error);

}  // namespace app::worker
