#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string>
#include <vector>
#include <string_view>

struct EngineDiagnosticField {
    std::string name;
    uint8_t dtype = 0;
    bool serialized = true;
    std::vector<uint8_t> bytes;
};

struct EngineDiagnosticSnapshot {
    int64_t rows = 0, max_rows = 0;
    int num_sh = 0, owner = -1;
    uint64_t source_id = 0;
    std::vector<uint32_t> owned_blocks;
    std::vector<EngineDiagnosticField> fields;
    std::string host_signature;
    std::string common_host_signature;
    std::string role_host_signature;
    std::string host_state;
    std::function<void()> restore_host;
};

EngineDiagnosticSnapshot engine_diagnostic_capture(bool include_reset);
void engine_diagnostic_restore(const EngineDiagnosticSnapshot&);
void engine_diagnostic_verify_start(const EngineDiagnosticSnapshot&);
std::string engine_diagnostic_host_signature(bool common = false);
std::string engine_diagnostic_host_state();
void engine_diagnostic_self_check(const EngineDiagnosticSnapshot&);
bool engine_diagnostic_compare(const EngineDiagnosticSnapshot& baseline,
    const EngineDiagnosticSnapshot& reference, const EngineDiagnosticSnapshot& actual,
    std::ostream&, const std::string& arm, int updates,
    const std::filesystem::path& detail_root, bool descriptive = false,
    bool reference_all_owned = false);

struct TrainStep;
void engine_diagnostic_begin_arm(const std::string& arm);
void engine_diagnostic_end_arm();
void engine_diagnostic_timing(double& seconds);
void engine_diagnostic_input(const TrainStep&, int step);
// Private provider runs before managed input fetch; returned inputs remain borrowed.
void engine_diagnostic_replay_step(std::function<const TrainStep*()>);
const TrainStep* engine_diagnostic_replay_step();
void engine_diagnostic_step_state(int step, bool common = false,
                                 const EngineDiagnosticSnapshot* role = nullptr);

struct EngineStepConfig;
using EngineDiagnosticStage = std::function<void(const char*, int)>;
void engine_diagnostic_mask_probe(EngineDiagnosticStage, bool all_owned,
                                  bool capture_data_gradients);
void engine_diagnostic_stage(const char*, int);
bool engine_diagnostic_probe_optim(int, const EngineStepConfig&);

// Private synchronous borrowed launch inputs, valid only during the callback.
using EngineDiagnosticPpisp = std::function<void(uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, int, int, int, int, int, float, float, std::string_view)>;
void engine_diagnostic_ppisp_probe(EngineDiagnosticPpisp);
const EngineDiagnosticPpisp& engine_diagnostic_ppisp_probe();
const TrainStep* engine_diagnostic_ppisp_input();
