#include "app/AgentFeatureWorker.h"
#include "app/AgentFeatureJob.h"

#include "app/AgentReconstructionJob.h"
#ifdef SS_TOOL_TRAIN
#include "app/AgentTrainingJob.h"
#include "checkpoint/Resume.h"
#endif
#include "app/JobScheduler.h"
#include "core/VulkanDeviceSelection.h"
#include "sfm/Pipeline.h"
#include "sfm/SfmConfig.h"
#include "sfm/vk/VkContext.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#ifndef SS_VERSION
#define SS_VERSION "unknown"
#endif

namespace app::agent {
namespace {
namespace fs = std::filesystem;
namespace fw = sfm::feature_work;
namespace sched = app::sched;
bool agent_owned_job(const sched::Job& job) noexcept {
    return job.options_payload.compare(0, 14, "agent-feature:") == 0 ||
           job.options_payload.compare(0, 21, "agent-reconstruction:") == 0 ||
           job.options_payload.compare(0, 15, "agent-training:") == 0;
}

bool active_job(const sched::Job& job) noexcept {
    return job.state == sched::JobState::Starting ||
           job.state == sched::JobState::Running ||
           job.state == sched::JobState::Stopping;
}
namespace portable = portable_worker_detail;
using namespace feature_worker_detail;

std::uint64_t unix_millis() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return value < 0 ? 0 : static_cast<std::uint64_t>(value);
}

bool plain_directory(const fs::path& path) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (ec || !fs::is_directory(status) || fs::is_symlink(status)) return false;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) !=
            FILE_ATTRIBUTE_DIRECTORY)
        return false;
#endif
    return true;
}

bool ensure_directory(const fs::path& path, std::string& error) {
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec || !plain_directory(path)) {
        error = "feature worker storage directory is unavailable";
        return false;
    }
    return true;
}

std::uint64_t tree_size(const fs::path& root,
                        const fs::path& excluded = {}) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(root, ec);
    if (ec == std::errc::no_such_file_or_directory ||
        status.type() == fs::file_type::not_found)
        return 0;
    if (ec || !fs::is_directory(status) || fs::is_symlink(status))
        throw std::runtime_error("feature storage contains an unsafe path");
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(root.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) !=
            FILE_ATTRIBUTE_DIRECTORY)
        throw std::runtime_error("feature storage contains a reparse point");
#endif
    const fs::path skip = excluded.empty() ? fs::path() : fs::absolute(excluded).lexically_normal();
    std::uint64_t total = 0;
    fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end;
    if (ec) throw std::runtime_error("cannot scan feature storage: " + ec.message());
    for (; it != end; it.increment(ec)) {
        if (ec) throw std::runtime_error("cannot scan feature storage: " + ec.message());
        const fs::path path = fs::absolute(it->path()).lexically_normal();
        if (!skip.empty() && path == skip) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        const fs::file_status item = it->symlink_status(ec);
        if (ec || fs::is_symlink(item))
            throw std::runtime_error("feature storage contains an unsafe path");
        if (fs::is_regular_file(item)) {
            const std::uintmax_t size = fs::file_size(it->path(), ec);
            if (ec || size > std::numeric_limits<std::uint64_t>::max() - total)
                throw std::runtime_error("feature storage size is invalid");
            total += static_cast<std::uint64_t>(size);
        } else if (!fs::is_directory(item)) {
            throw std::runtime_error("feature storage contains a non-file entry");
        }
    }
    return total;
}

std::uint64_t manifest_size(const std::vector<TransferFile>& files) {
    std::uint64_t total = 0;
    for (const TransferFile& file : files) {
        if (file.size > std::numeric_limits<std::uint64_t>::max() - total)
            throw std::runtime_error("feature manifest size overflow");
        total += file.size;
    }
    return total;
}

bool terminal(AttemptState state) noexcept {
    return state == AttemptState::Succeeded || state == AttemptState::Failed ||
           state == AttemptState::Interrupted;
}

std::string clipped(std::string value, std::size_t limit = 1024) {
    if (value.size() <= limit) return value;
    value.resize(limit);
    while (!value.empty() &&
           (static_cast<unsigned char>(value.back()) & 0xc0u) == 0x80u)
        value.pop_back();
    if (!value.empty()) {
        const unsigned char lead = static_cast<unsigned char>(value.back());
        const std::size_t width = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        if (value.size() - 1 + width > limit) value.pop_back();
    }
    return value;
}

wire::FeatureDecision decision_for(const wire::FeatureOffer& offer,
                                  wire::FeatureDecision::Step step,
                                  wire::FeatureDecision::Decision decision,
                                  std::string reason = {}) {
    wire::FeatureDecision out;
    out.job_id = offer.job_id;
    out.attempt_id = offer.attempt_id;
    out.step = step;
    out.decision = decision;
    out.reason = clipped(std::move(reason), 512);
    return out;
}

wire::PortableDecision portable_decision_for(
    const wire::PortableOffer& offer, wire::PortableDecision::Step step,
    wire::PortableDecision::Decision decision, std::string reason = {}) {
    wire::PortableDecision out;
    out.workload = offer.workload;
    out.job_id = offer.job_id;
    out.attempt_id = offer.attempt_id;
    out.step = step;
    out.decision = decision;
    out.reason = clipped(std::move(reason), 512);
    return out;
}

std::string portable_scheduler_prefix(wire::PortableWorkload workload,
                                      const std::string& job_id) {
    const char* prefix =
        workload == wire::PortableWorkload::Training
            ? "agent-training:"
            : "agent-reconstruction:";
    return std::string(prefix) + std::to_string(job_id.size()) + ":" +
           job_id + ":";
}

std::string portable_scheduler_marker(const wire::PortableOffer& offer) {
    return portable_scheduler_prefix(offer.workload, offer.job_id) +
           portable::PortableAttemptKey(offer.job_id, offer.attempt_id);
}

bool exact_offer(const wire::FeatureOffer& a, const wire::FeatureOffer& b) {
    if (a.job_id != b.job_id || a.attempt_id != b.attempt_id ||
        a.plan_digest != b.plan_digest || a.request_digest != b.request_digest ||
        a.required_build != b.required_build ||
        a.expires_at_ms != b.expires_at_ms || a.inputs.size() != b.inputs.size())
        return false;
    for (std::size_t i = 0; i < a.inputs.size(); ++i)
        if (a.inputs[i].path != b.inputs[i].path ||
            a.inputs[i].size != b.inputs[i].size ||
            a.inputs[i].sha256 != b.inputs[i].sha256)
            return false;
    return true;
}

bool exact_portable_offer(const wire::PortableOffer& a,
                          const wire::PortableOffer& b) {
    if (a.workload != b.workload || a.job_id != b.job_id ||
        a.attempt_id != b.attempt_id ||
        a.input_identity_sha256 != b.input_identity_sha256 ||
        a.required_build != b.required_build ||
        a.expires_at_ms != b.expires_at_ms || a.inputs.size() != b.inputs.size())
        return false;
    for (std::size_t i = 0; i < a.inputs.size(); ++i)
        if (a.inputs[i].path != b.inputs[i].path ||
            a.inputs[i].size != b.inputs[i].size ||
            a.inputs[i].sha256 != b.inputs[i].sha256)
            return false;
    return true;
}

bool reconstruction_build_available() noexcept {
#if defined(SS_TOOL_SFM) && defined(SS_BACKEND_VULKAN)
    return !ReconstructionBuildIdentity().empty() &&
           ReconstructionBuildIdentity() != "unknown";
#else
    return false;
#endif
}

bool training_build_available() noexcept {
#if defined(SS_TOOL_TRAIN) && defined(SS_BACKEND_VULKAN)
    return SS_VERSION[0] != '\0' && std::string(SS_VERSION) != "unknown";
#else
    return false;
#endif
}

struct LocalDevice {
    std::string selector;
    std::string name;
};

std::vector<LocalDevice> approved_devices(const Config& config,
                                          std::string& reason) {
#if (defined(SS_TOOL_SFM) || defined(SS_TOOL_TRAIN)) && defined(SS_BACKEND_VULKAN)
    std::vector<VkDeviceRecord> devices;
    try {
        devices = VkContext::listDevices();
    } catch (const std::exception& e) {
        reason = clipped(e.what());
        return {};
    }
    std::vector<LocalDevice> approved;
    for (const std::string& allowed : config.allowed_vulkan_uuids) {
        for (const VkDeviceRecord& device : devices) {
            if (!device.usable || device.props.vendorID == 0x10de ||
                device.type == "cpu" || device.type == "other")
                continue;
            const std::string selector =
                spirula::vkselect::selectorFor(device);
            if (selector == allowed &&
                std::none_of(approved.begin(), approved.end(),
                             [&](const LocalDevice& item) {
                                 return item.selector == selector;
                             })) {
                approved.push_back({selector, device.name});
                break;
            }
        }
    }
    if (!approved.empty()) return approved;
    reason = devices.empty()
                 ? "no Vulkan compute device is available"
                 : "no usable non-NVIDIA Vulkan device matches worker policy";
    return {};
#else
    (void)config;
    reason = "this build has no Vulkan training or reconstruction capability";
    return {};
#endif
}

std::optional<LocalDevice> approved_device(const Config& config,
                                           std::string& reason) {
    std::vector<LocalDevice> devices = approved_devices(config, reason);
    return devices.empty() ? std::nullopt
                           : std::optional<LocalDevice>(std::move(devices.front()));
}

bool device_still_usable(const Config& config, const std::string& selector,
                         std::string& error) {
    std::string reason;
    const std::vector<LocalDevice> devices = approved_devices(config, reason);
    const auto selected = std::find_if(
        devices.begin(), devices.end(), [&](const LocalDevice& device) {
            return device.selector == selector;
        });
    if (selected == devices.end()) {
        error = reason.empty() ? "the approved Vulkan device changed" : reason;
        return false;
    }
    return true;
}


bool build_recipe_args(const fw::FeaturePlan& plan,
                       std::vector<std::string>& args,
                       std::string& error) {
    if (plan.extraction.frontend != "sift") {
        error = "only the local SIFT feature recipe is currently supported";
        return false;
    }
    const auto signature = plan.extraction.settings.find("extraction_signature");
    if (signature == plan.extraction.settings.end()) {
        error = "feature recipe has no extraction signature";
        return false;
    }

    sfm::SfmConfig config;
    std::set<std::string> seen;
    const std::string& text = signature->second;
    for (std::size_t begin = 0; begin < text.size();) {
        const std::size_t end = text.find('\n', begin);
        const std::string line = text.substr(
            begin, end == std::string::npos ? end : end - begin);
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos || equals == 0) {
            error = "feature recipe has an invalid extraction setting";
            return false;
        }
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        if (!seen.insert(key).second) {
            error = "feature recipe repeats extraction setting " + key;
            return false;
        }
        // These values belong to the worker, not the leader's filesystem.
        if (key == "masks" || key == "aliked-model" ||
            key == "loma-detector-model" || key == "loma-descriptor-model") {
            if (key == "masks" && !value.empty()) {
                error = "feature recipe contains a remote mask path";
                return false;
            }
        } else {
            std::string flag = "--" + key;
            std::string argument = value;
            int argc = 3;
            if (value == "on") {
                argument.clear();
                argc = 2;
            } else if (value == "off") {
                flag = "--no-" + key;
                argument.clear();
                argc = 2;
            }
            char program[] = "feature-recipe";
            char* argv[] = {program, flag.data(), argument.data()};
            int index = 1;
            std::set<std::string> parsed;
            const sfm::FieldResult result = sfm::setConfigField(
                config, sfm::CMD_EXTRACT, flag, argc, argv, index, parsed,
                error);
            if (result != sfm::FieldResult::Ok) {
                if (error.empty()) error = "unsupported feature recipe setting " + key;
                return false;
            }
            args.push_back(std::move(flag));
            if (argc == 3) args.push_back(value);
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    if (config.features != "sift") {
        error = "feature recipe does not select SIFT";
        return false;
    }
    if (const std::string finalized = config.finalize(sfm::CMD_EXTRACT);
        !finalized.empty()) {
        error = finalized;
        return false;
    }
    config.mask_dir.clear();
    config.sift.spv_path.clear();
    try {
        const fw::Recipe local = sfm::makeFeatureRecipe(config);
        if (local.digest != plan.extraction.digest) {
            error = "feature recipe differs from the local SIFT implementation";
            return false;
        }
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

std::string output_path_for(const fs::path& root, const std::string& key) {
    return (root / fs::u8path(key)).u8string();
}

bool safe_remove_tree(const fs::path& path, bool preserve_output_lock,
                      std::string& error) {
    std::error_code ec;
    const fs::file_status root_status = fs::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory ||
        root_status.type() == fs::file_type::not_found)
        return true;
    if (ec || !fs::is_directory(root_status) || fs::is_symlink(root_status)) {
        error = "feature cleanup root is unsafe";
        return false;
    }
    std::vector<fs::path> entries;
    fs::recursive_directory_iterator it(path, fs::directory_options::none, ec), end;
    if (ec) {
        error = "cannot enumerate feature cleanup root";
        return false;
    }
    for (; it != end; it.increment(ec)) {
        if (ec) {
            error = "cannot enumerate feature cleanup root";
            return false;
        }
        const fs::file_status item = it->symlink_status(ec);
        if (ec || fs::is_symlink(item)) {
            error = "feature cleanup encountered an unsafe path";
            return false;
        }
        const fs::path relative = it->path().lexically_relative(path);
        if (preserve_output_lock && relative == fs::path(".spirula-output.lock")) {
            if (!fs::is_regular_file(item) || fs::file_size(it->path(), ec) != 0 || ec) {
                error = "feature output lease marker is invalid";
                return false;
            }
            continue;
        }
        entries.push_back(it->path());
    }
    std::sort(entries.begin(), entries.end(), [](const fs::path& a, const fs::path& b) {
        return std::distance(a.begin(), a.end()) > std::distance(b.begin(), b.end());
    });
    for (const fs::path& entry : entries) {
        if (preserve_output_lock && entry == path / ".spirula-output.lock") continue;
        fs::remove(entry, ec);
        if (ec) {
            error = "cannot remove accepted feature data";
            return false;
        }
    }
    return true;
}

void persist_state(const fs::path& state_root, const State& state) {
    save_state(state_root, state);
}

AttemptState state_for(const wire::FeatureResult& result) {
    if (result.outcome == wire::FeatureResult::Outcome::Succeeded)
        return AttemptState::Succeeded;
    if (result.outcome == wire::FeatureResult::Outcome::Interrupted)
        return AttemptState::Interrupted;
    return AttemptState::Failed;
}

}  // namespace

struct FeatureWorker::Impl {
    struct Prepared {
        FeatureInputBundle input;
        std::vector<std::string> args;
    };
    struct PreparedPortable {
        wire::PortableOffer offer;
    };

    Config config;
    fs::path state_root;
    fs::path storage_root;
    fs::path shard_root;
    fs::path result_root;
    fs::path portable_root;
    fs::path scheduler_root;
    std::string executable_path;
    std::unique_ptr<sched::JobScheduler> scheduler;
    std::optional<LocalDevice> device;
    std::string device_error;
    std::string storage_error;
    std::optional<Prepared> prepared;
    std::optional<PreparedPortable> prepared_portable;
    Impl(const Config& cfg, const fs::path& state, const fs::path& storage,
         std::string executable, const State& current)
        : config(cfg), state_root(state), storage_root(storage),
          shard_root(storage / "feature-shards"),
          result_root(storage / "feature-results"),
          portable_root(storage / "reconstruction-attempts"),
          scheduler_root(storage / "feature-scheduler"),
          executable_path(std::move(executable)) {
        if (!storage_root.is_absolute() || !plain_directory(storage_root))
            throw std::invalid_argument("feature storage root is unavailable");
        std::string error;
        if (!ensure_directory(shard_root, error) ||
            !ensure_directory(result_root, error) ||
            !ensure_directory(portable_root, error) ||
            !ensure_directory(scheduler_root, error))
            throw std::runtime_error(error);
        device = approved_device(config, device_error);

        scheduler = std::make_unique<sched::JobScheduler>(
            scheduler_root.u8string(), executable_path);
        scheduler->pause_dispatch(true);
        scheduler->set_device_validator([this](const std::string& selector,
                                               std::string& message) {
            return device_still_usable(config, selector, message);
        });
        scheduler->set_feature_result_validator(
            [this](const sched::Job& job, const sched::Phase& phase,
                   const std::string& output, std::string& message) {
                return validate_scheduled_output(job, phase, output, message);
            });
        scheduler->load();
        storage_error = scheduler->state_error();
        if (storage_error.empty()) {
            for (const sched::Job& job : scheduler->list())
                if (job.state == sched::JobState::Queued &&
                    job.options_payload.compare(
                        0, std::string("agent-feature:").size(),
                        "agent-feature:") == 0)
                    scheduler->cancel(job.job_id);
        }
        sync_controls(current);
    }

    bool validate_scheduled_output(const sched::Job& job,
                                   const sched::Phase& phase,
                                   const std::string& output,
                                   std::string& error) {
        const std::string reconstruction_prefix = "agent-reconstruction:";
        const std::string training_prefix = "agent-training:";
        if ((phase.phase == "sfm" &&
             job.options_payload.compare(0, reconstruction_prefix.size(),
                                         reconstruction_prefix) == 0) ||
            (phase.phase == "train" &&
             job.options_payload.compare(0, training_prefix.size(),
                                         training_prefix) == 0)) {
            std::vector<portable::PortableOfferRecord> records;
            if (!portable::LoadPortableAuthorities(portable_root, records,
                                                   error))
                return false;
            for (const portable::PortableOfferRecord& record : records) {
                if (job.options_payload !=
                    portable_scheduler_marker(record.offer))
                    continue;
                bool authoritative = false;
                if (!portable::IsPortableAttemptAuthoritative(
                        portable_root, record.offer.job_id,
                        record.offer.attempt_id, record.leader_id,
                        record.leader_epoch, authoritative, error) ||
                    !authoritative) {
                    if (error.empty())
                        error = "scheduler portable attempt was superseded";
                    return false;
                }
                try {
                    if (record.offer.workload ==
                        wire::PortableWorkload::Reconstruction) {
                        const fs::path sparse = portable_sparse_root(record.offer);
                        if (output != sparse.u8string()) {
                            error = "scheduler reconstruction output path changed";
                            return false;
                        }
                        const ReconstructionInputBundle inputs =
                            VerifyReconstructionInputs(
                                portable_input_directory(record.offer),
                                record.offer.inputs,
                                record.offer.input_identity_sha256,
                                config.disk_budget_bytes);
                        const std::uint64_t quota =
                            budget_for_portable_output(record.offer);
                        const std::vector<TransferFile> manifest =
                            ReconstructionOutputManifest(inputs, sparse, quota);
                        VerifyReconstructionOutputs(inputs, sparse, manifest,
                                                    quota);
                        return true;
                    }
#ifdef SS_TOOL_TRAIN
                    if (record.offer.workload ==
                        wire::PortableWorkload::Training) {
                        const fs::path training = portable_training_root(record.offer);
                        if (output != training.u8string()) {
                            error = "scheduler training output path changed";
                            return false;
                        }
                        const TrainingInputBundle inputs = VerifyTrainingInputs(
                            portable_input_directory(record.offer),
                            record.offer.inputs,
                            record.offer.input_identity_sha256,
                            config.disk_budget_bytes);
                        const std::string checkpoint =
                            ckpt::resolve_checkpoint(training)
                                .ckpt_dir.filename().u8string();
                        (void)TrainingOutputManifest(
                            inputs, training, checkpoint,
                            budget_for_portable_output(record.offer));
                        return true;
                    }
#endif
                    error = "scheduler portable workload is unavailable";
                    return false;
                } catch (const std::exception& e) {
                    error = e.what();
                    return false;
                }
            }
            error = "scheduler portable attempt has no durable authority";
            return false;
        }
        const std::string prefix = "agent-feature:";
        if (phase.phase != "sfm-extract" ||
            job.options_payload.compare(0, prefix.size(), prefix) != 0) {
            error = "scheduler feature attempt marker is invalid";
            return false;
        }
        const std::string key = job.options_payload.substr(prefix.size());
        if (key.size() != 64 || output != output_path_for(result_root, key)) {
            error = "scheduler feature result path differs from its attempt";
            return false;
        }
        bool safe = true;
        for (char c : key)
            safe = safe && ((c >= '0' && c <= '9') ||
                            (c >= 'a' && c <= 'f'));
        if (!safe) {
            error = "scheduler feature attempt marker is malformed";
            return false;
        }
        std::error_code ec;
        const fs::path request_path = shard_root / fs::u8path(key) /
                                      "inputs" / "request.json";
        if (!fs::is_regular_file(request_path, ec) || ec) {
            error = "verified feature request is unavailable";
            return false;
        }
        try {
            sfm::validateFeatureResult(request_path.u8string(),
                                      fs::u8path(output));
            return true;
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
    }

    bool sync_controls(const State& state) {
        if (!scheduler) return false;
        const bool healthy =
            storage_error.empty() && scheduler->state_error().empty();
        scheduler->pause_dispatch(
            state.restart_intent || restart_pending(state) ||
            administrative_pending(state) || state.paused ||
            state.maintenance || !healthy || stop_all_pending(state));
        for (const sched::Job& job : scheduler->list()) {
            if (!agent_owned_job(job) || !active_job(job)) continue;
            const sched::JobPauseStatus pause = scheduler->pause_status(job.job_id);
            if (state.paused) {
                if (pause == sched::JobPauseStatus::Running)
                    scheduler->request_pause(job.job_id);
            } else if (pause != sched::JobPauseStatus::Running) {
                scheduler->request_resume(job.job_id);
            }
        }
        return healthy;
    }

    bool stop_all_pending(const State& state) const {
        return std::any_of(
            state.recent_commands.begin(), state.recent_commands.end(),
            [](const CommandRecord& record) {
                return record.outcome == CommandOutcome::Accepted &&
                       record.command_action == "stop" &&
                       record.target_job_id.empty();
            });
    }
    bool restart_pending(const State& state) const {
        return std::any_of(
            state.recent_commands.begin(), state.recent_commands.end(),
            [](const CommandRecord& record) {
                return record.outcome == CommandOutcome::Pending &&
                       (record.command_action == "restart_service" ||
                        record.command_action == "force_restart_service");
            });
    }
    bool administrative_pending(const State& state) const {
        return std::any_of(
            state.recent_commands.begin(), state.recent_commands.end(),
            [](const CommandRecord& record) {
                return (record.outcome == CommandOutcome::Pending ||
                        record.outcome == CommandOutcome::Accepted) &&
                       (record.command_action == "reboot_machine" ||
                        record.command_action == "activate_update");
            });
    }


    bool pause_acknowledged() const {
        if (!scheduler || !storage_error.empty() ||
            !scheduler->state_error().empty())
            return false;
        for (const sched::Job& job : scheduler->list())
            if (agent_owned_job(job) && active_job(job) &&
                scheduler->pause_status(job.job_id) !=
                    sched::JobPauseStatus::Paused)
                return false;
        return true;
    }

    bool resume_acknowledged() const {
        if (!scheduler || !storage_error.empty() ||
            !scheduler->state_error().empty())
            return false;
        for (const sched::Job& job : scheduler->list())
            if (agent_owned_job(job) && active_job(job) &&
                scheduler->pause_status(job.job_id) !=
                    sched::JobPauseStatus::Running)
                return false;
        return true;
    }

    bool stop_complete(const std::string& target_job_id) const {
        if (!scheduler || !storage_error.empty() ||
            !scheduler->state_error().empty())
            return false;
        for (const sched::Job& job : scheduler->list()) {
            if (!agent_owned_job(job) ||
                (!target_job_id.empty() && job.job_id != target_job_id))
                continue;
            if (scheduled(job)) return false;
        }
        return true;
    }

    CommandOutcome stop_jobs(const std::string& target_job_id) {
        if (!scheduler || !storage_error.empty() ||
            !scheduler->state_error().empty())
            return CommandOutcome::Failed;
        bool found_target = false;
        for (const sched::Job& job : scheduler->list()) {
            if (!agent_owned_job(job) ||
                (!target_job_id.empty() && job.job_id != target_job_id))
                continue;
            found_target = true;
            if (job.state == sched::JobState::Queued)
                scheduler->cancel(job.job_id);
            else if (active_job(job))
                scheduler->force_stop(job.job_id);
        }
        if (!target_job_id.empty() && !found_target)
            return CommandOutcome::Rejected;
        return stop_complete(target_job_id) ? CommandOutcome::Applied
                                            : CommandOutcome::Accepted;
    }

    fs::path attempt_directory(const wire::FeatureOffer& offer) const {
        return shard_root / fs::u8path(FeatureAttemptKey(offer.job_id,
                                                         offer.attempt_id));
    }

    fs::path input_directory(const wire::FeatureOffer& offer) const {
        return attempt_directory(offer) / "inputs";
    }

    fs::path result_directory(const wire::FeatureOffer& offer) const {
        return result_root / fs::u8path(FeatureAttemptKey(offer.job_id,
                                                         offer.attempt_id));
    }

    std::uint64_t budget_for_input(const wire::FeatureOffer& offer) const {
        const std::uint64_t used =
            tree_size(shard_root, input_directory(offer)) +
            tree_size(result_root);
        if (used > config.disk_budget_bytes)
            throw std::runtime_error("feature storage exceeds worker disk budget");
        return config.disk_budget_bytes - used;
    }

    std::uint64_t budget_for_output(const wire::FeatureOffer& offer) const {
        const std::uint64_t used =
            tree_size(shard_root) + tree_size(result_root, result_directory(offer));
        if (used > config.disk_budget_bytes)
            throw std::runtime_error("feature storage exceeds worker disk budget");
        return config.disk_budget_bytes - used;
    }

    bool has_scheduler_job(const std::string& key) const {
        if (!scheduler) return false;
        for (const sched::Job& job : scheduler->list())
            if (job.options_payload == "agent-feature:" + key) return true;
        return false;
    }

    fs::path portable_attempt_directory(
        const wire::PortableOffer& offer) const {
        return portable::PortableAttemptDirectory(
            portable_root, offer.job_id, offer.attempt_id);
    }

    fs::path portable_input_directory(
        const wire::PortableOffer& offer) const {
        return offer.workload == wire::PortableWorkload::Training
                   ? portable_workspace(offer) / "input"
                   : portable_attempt_directory(offer) / "inputs";
    }

    fs::path portable_output_lease_root(
        const wire::PortableOffer& offer) const {
        return portable_attempt_directory(offer) / "output";
    }

    fs::path portable_workspace(const wire::PortableOffer& offer) const {
        return portable_output_lease_root(offer) / "workspace";
    }

    fs::path portable_sparse_root(const wire::PortableOffer& offer) const {
        return portable_workspace(offer) / "sparse";
    }
    fs::path portable_training_root(const wire::PortableOffer& offer) const {
        return portable_workspace(offer) / "output" / "run";
    }

    fs::path portable_output_root(const wire::PortableOffer& offer) const {
        return offer.workload == wire::PortableWorkload::Training
                   ? portable_training_root(offer)
                   : portable_sparse_root(offer);
    }

    std::uint64_t budget_for_portable_input(
        const wire::PortableOffer& offer) const {
        const std::uint64_t used =
            tree_size(shard_root) + tree_size(result_root) +
            tree_size(portable_root, portable_input_directory(offer));
        if (used > config.disk_budget_bytes)
            throw std::runtime_error(
                "worker storage exceeds its configured disk budget");
        return config.disk_budget_bytes - used;
    }

    std::uint64_t budget_for_portable_output(
        const wire::PortableOffer& offer) const {
        const std::uint64_t used =
            tree_size(shard_root) + tree_size(result_root) +
            tree_size(portable_root, portable_output_root(offer));
        if (used > config.disk_budget_bytes)
            throw std::runtime_error(
                "worker storage exceeds its configured disk budget");
        return config.disk_budget_bytes - used;
    }
    bool scheduled(const sched::Job& job) const noexcept {
        return job.state == sched::JobState::Queued ||
               job.state == sched::JobState::Starting ||
               job.state == sched::JobState::Running ||
               job.state == sched::JobState::Stopping;
    }

    std::size_t scheduled_job_count() const {
        if (!scheduler) return 0;
        std::size_t count = 0;
        for (const sched::Job& job : scheduler->list())
            if (scheduled(job)) ++count;
        return count;
    }

    std::optional<LocalDevice> available_device() const {
        if (!scheduler || !config.max_concurrent_jobs ||
            scheduled_job_count() >= config.max_concurrent_jobs)
            return std::nullopt;
        std::set<std::string> occupied;
        for (const sched::Job& job : scheduler->list()) {
            if (!scheduled(job)) continue;
            if (!job.device.empty()) occupied.insert(job.device);
            if (job.current_phase < job.phases.size() &&
                !job.phases[job.current_phase].planned_device.empty())
                occupied.insert(job.phases[job.current_phase].planned_device);
        }
        std::string reason;
        for (const LocalDevice& candidate : approved_devices(config, reason))
            if (!occupied.count(candidate.selector)) return candidate;
        return std::nullopt;
    }

    bool has_scheduler_marker(const std::string& marker) const {
        if (!scheduler) return false;
        for (const sched::Job& job : scheduler->list())
            if (job.options_payload == marker) return true;
        return false;
    }

    bool has_active_feature_job() const {
        if (!scheduler) return false;
        for (const sched::Job& job : scheduler->list())
            if (job.options_payload.compare(
                    0, std::string("agent-feature:").size(),
                    "agent-feature:") == 0 &&
                (job.state == sched::JobState::Queued ||
                 job.state == sched::JobState::Starting ||
                 job.state == sched::JobState::Running ||
                 job.state == sched::JobState::Stopping))
                return true;
        return false;
    }


    bool has_active_portable_attempt(wire::PortableWorkload workload) {
        std::vector<portable::PortableOfferRecord> records;
        std::string error;
        if (!portable::LoadPortableAuthorities(portable_root, records, error)) {
            storage_error = error;
            return true;
        }
        for (const portable::PortableOfferRecord& record : records) {
            if (record.offer.workload != workload) continue;
            bool authoritative = false;
            if (!portable::IsPortableAttemptAuthoritative(
                    portable_root, record.offer.job_id, record.offer.attempt_id,
                    record.leader_id, record.leader_epoch, authoritative, error)) {
                storage_error = error;
                return true;
            }
            if (!authoritative) continue;
            if (record.offer.expires_at_ms <= unix_millis() &&
                !has_scheduler_marker(portable_scheduler_marker(record.offer)))
                continue;
            std::optional<std::string> acknowledgment;
            std::optional<std::string> rejection;
            if (!portable::LoadPortableAcknowledgment(
                    portable_root, record.offer.job_id, record.offer.attempt_id,
                    acknowledgment, error) ||
                !portable::LoadPortableInputRejection(
                    portable_root, record.offer.job_id, record.offer.attempt_id,
                    rejection, error)) {
                storage_error = error;
                return true;
            }
            if (acknowledgment || rejection) continue;
            wire::PortableResult result;
            if (portable::LoadPortableResult(
                    portable_root, record.offer.job_id, record.offer.attempt_id,
                    result, error))
                continue;
            if (error == "portable result record is missing") return true;
            storage_error = error;
            return true;
        }
        return false;
    }
    bool has_unacknowledged_portable_result() {
        std::vector<portable::PortableOfferRecord> records;
        std::string error;
        if (!portable::LoadPortableAuthorities(portable_root, records, error)) {
            storage_error = error;
            return true;
        }
        for (const portable::PortableOfferRecord& record : records) {
            bool authoritative = false;
            if (!portable::IsPortableAttemptAuthoritative(
                    portable_root, record.offer.job_id, record.offer.attempt_id,
                    record.leader_id, record.leader_epoch, authoritative, error)) {
                storage_error = error;
                return true;
            }
            if (!authoritative) continue;
            std::optional<std::string> acknowledgment;
            if (!portable::LoadPortableAcknowledgment(
                    portable_root, record.offer.job_id, record.offer.attempt_id,
                    acknowledgment, error)) {
                storage_error = error;
                return true;
            }
            if (acknowledgment) continue;
            wire::PortableResult result;
            if (portable::LoadPortableResult(
                    portable_root, record.offer.job_id, record.offer.attempt_id,
                    result, error))
                return true;
            if (error != "portable result record is missing") {
                storage_error = error;
                return true;
            }
        }
        return false;
    }

    bool set_portable_result(const portable::PortableOfferRecord& record,
                             const wire::PortableResult& result) {
        bool authoritative = false;
        std::string error;
        if (!portable::IsPortableAttemptAuthoritative(
                portable_root, record.offer.job_id, record.offer.attempt_id,
                record.leader_id, record.leader_epoch, authoritative, error)) {
            storage_error = error;
            return false;
        }
        if (!authoritative) return false;
        if (!portable::SavePortableResult(portable_root, result, error)) {
            if (error != "portable attempt is no longer authoritative")
                storage_error = error;
            return false;
        }
        storage_error.clear();
        return true;
    }

    const AcceptedAssignment* assignment(const State& state,
                                         const std::string& attempt) const {
        for (const AcceptedAssignment& item : state.accepted)
            if (item.attempt_id == attempt) return &item;
        return nullptr;
    }

    bool has_unacknowledged_result(const State& state) const {
        for (const AcceptedAssignment& item : state.accepted) {
            if (!terminal(item.state)) continue;
            std::optional<std::string> ack;
            std::string error;
            if (!LoadFeatureAcknowledgment(shard_root, item.job_id,
                                           item.attempt_id, ack, error) ||
                !ack)
                return true;
        }
        return false;
    }

    bool erase_acked_terminal(State& state) {
        for (auto it = state.accepted.begin(); it != state.accepted.end(); ++it) {
            if (!terminal(it->state)) continue;
            std::optional<std::string> ack;
            std::string error;
            if (LoadFeatureAcknowledgment(shard_root, it->job_id,
                                          it->attempt_id, ack, error) && ack) {
                state.accepted.erase(it);
                return true;
            }
        }
        return false;
    }

    bool set_result(const wire::FeatureOffer& offer,
                    const wire::FeatureResult& result, State& state) {
        std::string error;
        if (!SaveFeatureResult(shard_root, result, error)) {
            storage_error = error;
            return false;
        }
        for (AcceptedAssignment& item : state.accepted) {
            if (item.attempt_id != offer.attempt_id ||
                item.job_id != offer.job_id)
                continue;
            if (item.state == AttemptState::InProgress) {
                try {
                    transition_assignment(state, offer.attempt_id,
                                          state_for(result));
                } catch (const std::exception& e) {
                    storage_error = e.what();
                    return false;
                }
            }
            try {
                persist_state(state_root, state);
            } catch (const std::exception& e) {
                storage_error = e.what();
                return false;
            }
            storage_error.clear();
            return true;
        }
        return true;
    }
};

FeatureWorker::FeatureWorker(const Config& config,
                             const fs::path& state_root,
                             const fs::path& storage_root,
                             const std::string& executable_path,
                             const State& state)
    : _impl(std::make_unique<Impl>(config, state_root, storage_root,
                                   executable_path, state)) {}

FeatureWorker::~FeatureWorker() = default;

wire::Status FeatureWorker::status(const State& state) {
    Impl& impl = *_impl;
    impl.device = approved_device(impl.config, impl.device_error);
    wire::Status status;
    status.connection = wire::ConnectionState::Connected;
    status.build = SS_VERSION;
#ifdef _WIN32
    status.platform = "windows";
#elif defined(__APPLE__)
    status.platform = "macos";
#elif defined(__linux__)
    status.platform = "linux";
#else
    status.platform = "unknown";
#endif
    if (impl.device)
        status.gpu = clipped(impl.device->name, wire::kMaxGpuBytes);
    status.compatibility =
        !status.build.empty() && status.build != "unknown" &&
                status.platform != "unknown"
            ? wire::CompatibilityState::Compatible
            : wire::CompatibilityState::Incompatible;
    status.maintenance = state.maintenance;
    status.online = !state.maintenance;
    const bool disk_ready = [&] {
        if (!impl.config.disk_budget_bytes) return false;
        std::error_code ec;
        const fs::space_info available = fs::space(impl.storage_root, ec);
        return !ec && available.available > 0;
    }();
    const bool worker_ready =
        impl.device && disk_ready &&
        status.compatibility == wire::CompatibilityState::Compatible &&
        impl.storage_error.empty() && impl.scheduler &&
        impl.scheduler->state_error().empty();
    if (worker_ready) {
        status.capabilities.push_back(wire::Capability::Feature);
        if (reconstruction_build_available())
            status.capabilities.push_back(wire::Capability::Reconstruction);
        if (training_build_available())
            status.capabilities.push_back(wire::Capability::Training);
    }
    bool queued_feature = false;
    bool queued_reconstruction = false;
    bool queued_training = false;
    if (impl.scheduler) {
        for (const sched::Job& job : impl.scheduler->list()) {
            if (!impl.scheduled(job)) continue;
            const bool feature =
                job.options_payload.compare(0, 14, "agent-feature:") == 0;
            const bool reconstruction =
                job.options_payload.compare(0, 21,
                                            "agent-reconstruction:") == 0;
            const bool training =
                job.options_payload.compare(0, 15, "agent-training:") == 0;
            if (!feature && !reconstruction && !training) continue;
            const wire::ActivityState activity =
                training ? wire::ActivityState::Training
                : reconstruction ? wire::ActivityState::Reconstruction
                                 : wire::ActivityState::Feature;
            if (job.state == sched::JobState::Starting ||
                job.state == sched::JobState::Running ||
                job.state == sched::JobState::Stopping) {
                status.activity = activity;
                queued_feature = queued_reconstruction = queued_training = false;
                break;
            }
            queued_reconstruction = queued_reconstruction || reconstruction;
            queued_training = queued_training || training;
            queued_feature = queued_feature || feature;
        }
    }
    if (status.activity == wire::ActivityState::Idle)
        status.activity = queued_training
                              ? wire::ActivityState::Training
                          : queued_reconstruction
                              ? wire::ActivityState::Reconstruction
                          : queued_feature ? wire::ActivityState::Feature
                                           : wire::ActivityState::Idle;
    const bool result_pending =
        impl.has_unacknowledged_result(state) ||
        impl.has_unacknowledged_portable_result();
    if (!impl.storage_error.empty() ||
        (impl.scheduler && !impl.scheduler->state_error().empty()))
        status.health = wire::HealthState::Unhealthy;
    else if (!impl.device || !disk_ready ||
             status.compatibility != wire::CompatibilityState::Compatible)
        status.health = wire::HealthState::Degraded;
    else
        status.health = wire::HealthState::Healthy;

    if (state.restart_intent || impl.restart_pending(state))
        status.scheduling = wire::SchedulingState::Stopped;
    else if (state.paused)
        status.scheduling = impl.pause_acknowledged()
                                ? wire::SchedulingState::Paused
                                : wire::SchedulingState::Pausing;
    else if (state.maintenance || !impl.device || !disk_ready ||
             !impl.storage_error.empty() || status.capabilities.empty() ||
             result_pending || impl.stop_all_pending(state) ||
             !impl.available_device())
        status.scheduling = wire::SchedulingState::Stopped;
    else
        status.scheduling = wire::SchedulingState::Accepting;
    return status;
}

wire::FeatureDecision FeatureWorker::accept_offer(
    const wire::FeatureOffer& offer, State& state,
    const std::string& leader_id, std::uint64_t leader_epoch) {
    Impl& impl = *_impl;
    const auto reject = [&](const std::string& reason) {
        return decision_for(offer, wire::FeatureDecision::Step::Offer,
                            wire::FeatureDecision::Decision::Rejected, reason);
    };
    if (!impl.storage_error.empty() || !impl.scheduler ||
        !impl.scheduler->state_error().empty())
        return reject("worker storage or scheduler is unavailable");
    if (leader_id.empty() || !leader_epoch)
        return reject("feature offer has no authenticated leader identity");
    if (offer.job_id.empty() || offer.attempt_id.empty() ||
        SS_VERSION[0] == '\0' || std::string(SS_VERSION) == "unknown" ||
        offer.required_build != SS_VERSION)
        return reject("offer build or identity is incompatible");
    if (offer.expires_at_ms <= unix_millis())
        return reject("feature offer expired");
    if (state.restart_intent || impl.restart_pending(state))
        return reject("worker is restarting");
    if (state.maintenance || state.paused)
        return reject("worker is paused or in maintenance");
    if (impl.stop_all_pending(state))
        return reject("worker is stopping all agent jobs");
    if (impl.has_unacknowledged_result(state))
        return reject("a previous feature result awaits confirmation");
    if (impl.has_unacknowledged_portable_result() ||
        impl.has_active_portable_attempt(
            wire::PortableWorkload::Reconstruction))
        return reject("a reconstruction result awaits confirmation or is active");
    impl.device = approved_device(impl.config, impl.device_error);
    if (!impl.device || !impl.available_device())
        return reject("no free policy-approved Vulkan GPU or worker slot is available");

    std::error_code disk_error;
    const fs::space_info available = fs::space(impl.storage_root, disk_error);
    if (!impl.config.disk_budget_bytes || disk_error || !available.available)
        return reject("worker storage has no usable disk capacity");
    std::string error;
    try {
        const std::uint64_t budget = impl.budget_for_input(offer);
        const std::uint64_t declared = manifest_size(offer.inputs);
        if (declared > budget || declared > available.available)
            return reject("feature input bundle exceeds worker disk capacity");
    } catch (const std::exception& e) {
        return reject(e.what());
    }

    const AcceptedAssignment* known = impl.assignment(state, offer.attempt_id);
    if (known) {
        feature_worker_detail::FeatureOfferRecord stored;
        if (known->job_id != offer.job_id ||
            !LoadFeatureOfferRecord(impl.shard_root, offer.job_id,
                                    offer.attempt_id, stored, error)) {
            impl.storage_error = error.empty()
                                     ? "persisted feature offer is unavailable"
                                     : error;
            return reject("attempt identity conflicts with persisted state");
        }
        if (!exact_offer(stored.offer, offer) ||
            stored.leader_id != leader_id ||
            stored.leader_epoch != leader_epoch)
            return reject("attempt identity belongs to a different offer or leader");
        std::optional<std::string> rejected_inputs;
        if (!LoadFeatureInputRejection(impl.shard_root, offer.job_id,
                                       offer.attempt_id, rejected_inputs,
                                       error)) {
            impl.storage_error = error;
            return reject("persisted input rejection record is unavailable");
        }
        if (rejected_inputs || terminal(known->state))
            return reject("feature attempt has already been finalized");
        std::optional<std::string> ack;
        if (!LoadFeatureAcknowledgment(impl.shard_root, offer.job_id,
                                       offer.attempt_id, ack, error) || ack) {
            impl.storage_error = error;
            return reject("feature attempt has already been finalized");
        }
        return decision_for(offer, wire::FeatureDecision::Step::Offer,
                            wire::FeatureDecision::Decision::Accepted);
    }

    feature_worker_detail::FeatureOfferRecord prior_offer;
    const bool prior_exists =
        LoadFeatureOfferRecord(impl.shard_root, offer.job_id,
                               offer.attempt_id, prior_offer, error);
    if (!prior_exists && error != "feature attempt record is unavailable") {
        impl.storage_error = error;
        return reject("persisted feature offer is malformed");
    }
    if (prior_exists &&
        (!exact_offer(prior_offer.offer, offer) ||
         prior_offer.leader_id != leader_id ||
         prior_offer.leader_epoch != leader_epoch))
        return reject("attempt identity already belongs to another offer or leader");
    if (prior_exists) {
        std::optional<std::string> ack;
        std::optional<std::string> rejected_inputs;
        wire::FeatureResult old_result;
        if (!LoadFeatureAcknowledgment(impl.shard_root, offer.job_id,
                                       offer.attempt_id, ack, error) ||
            !LoadFeatureInputRejection(impl.shard_root, offer.job_id,
                                       offer.attempt_id, rejected_inputs,
                                       error)) {
            impl.storage_error = error;
            return reject("persisted feature attempt metadata is unavailable");
        }
        if (ack || rejected_inputs)
            return reject("feature attempt has already been finalized");
        if (LoadFeatureResult(impl.shard_root, offer.job_id, offer.attempt_id,
                              old_result, error))
            return reject("feature attempt already has a durable result");
        if (error != "feature result record is missing") {
            impl.storage_error = error;
            return reject("persisted feature result is malformed");
        }
    }

    if (impl.has_scheduler_job(FeatureAttemptKey(offer.job_id,
                                                 offer.attempt_id)))
        return reject("feature attempt already exists in the scheduler");
    for (const AcceptedAssignment& item : state.accepted)
        if (item.state == AttemptState::Accepted ||
            item.state == AttemptState::InProgress)
            return reject("another feature attempt is already active");

    std::uint64_t incoming = 0;
    try {
        incoming = manifest_size(offer.inputs);
    } catch (const std::exception&) {
        return reject("feature input size overflows worker limits");
    }
    if (!impl.config.disk_budget_bytes || incoming > impl.config.disk_budget_bytes)
        return reject("feature input exceeds worker disk budget");
    try {
        const std::uint64_t used = tree_size(impl.shard_root) +
                                   tree_size(impl.result_root);
        if (used > impl.config.disk_budget_bytes ||
            incoming > impl.config.disk_budget_bytes - used)
            return reject("worker feature storage budget is full");
        std::error_code ec;
        const fs::space_info available = fs::space(impl.storage_root, ec);
        if (ec || available.available < incoming)
            return reject("worker has insufficient free storage for inputs");
    } catch (const std::exception&) {
        return reject("worker feature storage is unsafe");
    }

    const OfferRecordStatus stored = SaveFeatureOffer(
        impl.shard_root, offer, leader_id, leader_epoch, error);
    if (stored == OfferRecordStatus::Conflict)
        return reject("attempt identity already belongs to another offer");
    if (stored == OfferRecordStatus::Error)
        return reject("cannot persist the feature offer");
    State updated = state;
    if (updated.accepted.size() >= kMaxAcceptedAttempts &&
        !impl.erase_acked_terminal(updated))
        return reject("worker accepted-attempt history is full");
    try {
        accept_assignment(updated, offer.job_id, offer.attempt_id);
        persist_state(impl.state_root, updated);
    } catch (const std::exception&) {
        return reject("cannot persist worker assignment state");
    }
    state = std::move(updated);
    return decision_for(offer, wire::FeatureDecision::Step::Offer,
                        wire::FeatureDecision::Decision::Accepted);
}

TransferResult FeatureWorker::receive_inputs(TlsChannel& channel,
                                             const wire::FeatureOffer& offer) {
    Impl& impl = *_impl;
    std::string error;
    wire::FeatureOffer stored_offer;
    if (!LoadFeatureOffer(impl.shard_root, offer.job_id, offer.attempt_id,
                          stored_offer, error) ||
        !exact_offer(stored_offer, offer))
        return {TransferError::InvalidManifest,
                error.empty() ? "persisted feature offer changed" : error};
    const fs::path root = impl.input_directory(offer);
    if (!ensure_directory(root, error))
        return {TransferError::Filesystem, error};
    std::uint64_t budget = 0;
    try {
        budget = impl.budget_for_input(offer);
    } catch (const std::exception& e) {
        return {TransferError::QuotaExceeded, e.what()};
    }
    return ReceiveArtifacts(channel, root, offer.inputs, budget);
}

bool FeatureWorker::verify_inputs(const wire::FeatureOffer& offer,
                                  std::string& error) {
    Impl& impl = *_impl;
    impl.prepared.reset();
    try {
        if (offer.expires_at_ms <= unix_millis())
            throw std::runtime_error("feature offer expired during input transfer");
        wire::FeatureOffer stored;
        if (!LoadFeatureOffer(impl.shard_root, offer.job_id, offer.attempt_id,
                              stored, error) || !exact_offer(stored, offer)) {
            if (error.empty()) error = "feature offer changed during transfer";
            return false;
        }
        const fs::path root = impl.input_directory(offer);
        const fs::path request_path = root / "request.json";
        const fw::FeatureRequest request = fw::readRequestFile(request_path.u8string());
        if (fw::normalizeRelativePath(request.plan_path) != request.plan_path ||
            fs::u8path(request.plan_path).is_absolute())
            throw std::runtime_error("feature request plan path is not relative");
        const fs::path plan_path = request_path.parent_path() /
                                   fs::u8path(request.plan_path);
        const fw::FeaturePlan plan = fw::readPlanFile(plan_path.u8string());
        if (request.digest != offer.request_digest ||
            request.plan_digest != offer.plan_digest ||
            plan.digest != offer.plan_digest ||
            request.attempt_id != offer.attempt_id)
            throw std::runtime_error("feature request does not match the offer");
        FeatureInputBundle verified = VerifyFeatureInputs(
            root, plan, request, offer.inputs, impl.config.disk_budget_bytes);
        std::vector<std::string> recipe_args;
        if (!build_recipe_args(verified.plan, recipe_args, error)) return false;
        Impl::Prepared prepared;
        prepared.input = std::move(verified);
        prepared.args.push_back(prepared.input.image_root.u8string());
        prepared.args.push_back("-o");
        prepared.args.push_back(impl.result_directory(offer).u8string());
        prepared.args.push_back("--feature-request");
        prepared.args.push_back(prepared.input.request_path.u8string());
        if (!prepared.input.mask_root.empty()) {
            prepared.args.push_back("--masks");
            prepared.args.push_back(prepared.input.mask_root.u8string());
        }
        prepared.args.insert(prepared.args.end(), recipe_args.begin(),
                             recipe_args.end());
        impl.prepared = std::move(prepared);
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

wire::FeatureDecision FeatureWorker::reject_inputs(
    const wire::FeatureOffer& offer, State& state, const std::string& reason) {
    Impl& impl = *_impl;
    std::string error;
    if (!SaveFeatureInputRejection(impl.shard_root, offer.job_id,
                                   offer.attempt_id, reason, error)) {
        impl.storage_error = error;
        return decision_for(offer, wire::FeatureDecision::Step::Inputs,
                            wire::FeatureDecision::Decision::Rejected,
                            "cannot persist input rejection");
    }
    (void)state;
    impl.prepared.reset();
    return decision_for(offer, wire::FeatureDecision::Step::Inputs,
                        wire::FeatureDecision::Decision::Rejected,
                        reason.empty() ? "feature inputs were rejected" : reason);
}


wire::FeatureDecision FeatureWorker::accept_inputs(
    const wire::FeatureOffer& offer, State& state) {
    Impl& impl = *_impl;
    const std::string key = FeatureAttemptKey(offer.job_id, offer.attempt_id);
    const AcceptedAssignment* current = impl.assignment(state, offer.attempt_id);
    if (!current || current->job_id != offer.job_id || !impl.prepared ||
        impl.prepared->input.request.attempt_id != offer.attempt_id)
        return decision_for(offer, wire::FeatureDecision::Step::Inputs,
                            wire::FeatureDecision::Decision::Rejected,
                            "feature inputs were not verified");

    if (current->state == AttemptState::Accepted) {
        State updated = state;
        try {
            transition_assignment(updated, offer.attempt_id,
                                  AttemptState::InProgress);
            persist_state(impl.state_root, updated);
            state = std::move(updated);
        } catch (const std::exception&) {
            return decision_for(offer, wire::FeatureDecision::Step::Inputs,
                                wire::FeatureDecision::Decision::Rejected,
                                "cannot persist accepted feature inputs");
        }
    } else if (terminal(current->state)) {
        impl.prepared.reset();
        return decision_for(offer, wire::FeatureDecision::Step::Inputs,
                            wire::FeatureDecision::Decision::Accepted);
    }

    const std::optional<LocalDevice> device = impl.available_device();
    if (!impl.scheduler || !impl.storage_error.empty() || !device) {
        wire::FeatureResult result;
        result.job_id = offer.job_id;
        result.attempt_id = offer.attempt_id;
        result.outcome = wire::FeatureResult::Outcome::Failed;
        result.error = "feature scheduler, worker slot, or approved GPU is unavailable";
        impl.set_result(offer, result, state);
        impl.prepared.reset();
        return decision_for(offer, wire::FeatureDecision::Step::Inputs,
                            wire::FeatureDecision::Decision::Rejected,
                            result.error);
    }
    if (!impl.has_scheduler_job(key)) {
        sched::WorkflowSubmitOpts options;
        options.work_dir = impl.prepared->input.image_root.u8string();
        options.options_payload = "agent-feature:" + key;
        options.add_scheduler_publish = false;
        options.path_claims.push_back(
            {impl.prepared->input.bundle_root.u8string(), false});
        const fs::path output = impl.result_directory(offer);
        options.path_claims.push_back({output.u8string(), true});
        sched::Phase phase;
        phase.phase = "sfm-extract";
        phase.planned_device = device->selector;
        phase.planned_device_name = device->name;
        phase.output = output.u8string();
        phase.args = impl.prepared->args;
        options.phases.push_back(std::move(phase));
        std::error_code ec;
        fs::create_directories(output.parent_path(), ec);
        if (ec || !plain_directory(output.parent_path())) {
            wire::FeatureResult result;
            result.job_id = offer.job_id;
            result.attempt_id = offer.attempt_id;
            result.outcome = wire::FeatureResult::Outcome::Failed;
            result.error = "cannot create the worker result root";
            impl.set_result(offer, result, state);
            impl.prepared.reset();
            return decision_for(offer, wire::FeatureDecision::Step::Inputs,
                                wire::FeatureDecision::Decision::Rejected,
                                result.error);
        }
        const std::string scheduled = impl.scheduler->submit(options);
        if (scheduled.empty()) {
            wire::FeatureResult result;
            result.job_id = offer.job_id;
            result.attempt_id = offer.attempt_id;
            result.outcome = wire::FeatureResult::Outcome::Failed;
            result.error = "local feature scheduler refused the verified attempt";
            impl.set_result(offer, result, state);
            impl.prepared.reset();
            return decision_for(offer, wire::FeatureDecision::Step::Inputs,
                                wire::FeatureDecision::Decision::Rejected,
                                result.error);
        }
    }
    impl.prepared.reset();
    impl.sync_controls(state);
    return decision_for(offer, wire::FeatureDecision::Step::Inputs,
                        wire::FeatureDecision::Decision::Accepted);
}


bool FeatureWorker::bind_portable_leader(const std::string& leader_id,
                                         std::uint64_t leader_epoch) {
    Impl& impl = *_impl;
    std::string error;
    if (!portable::BindPortableLeader(impl.portable_root, leader_id,
                                      leader_epoch, error)) {
        impl.storage_error = error;
        impl.sync_controls(State{});
        return false;
    }
    impl.prepared_portable.reset();
    return true;
}

wire::PortableDecision FeatureWorker::accept_portable_offer(
    const wire::PortableOffer& offer, const State& state,
    const std::string& leader_id, std::uint64_t leader_epoch) {
    Impl& impl = *_impl;
    const auto reject = [&](const std::string& reason) {
        return portable_decision_for(
            offer, wire::PortableDecision::Step::Offer,
            wire::PortableDecision::Decision::Rejected, reason);
    };
    if (offer.workload == wire::PortableWorkload::Reconstruction) {
        if (!reconstruction_build_available() ||
            offer.required_build != ReconstructionBuildIdentity() ||
            offer.required_build != SS_VERSION)
            return reject("reconstruction build is incompatible");
    } else if (offer.workload == wire::PortableWorkload::Training) {
        if (!training_build_available() || offer.required_build != SS_VERSION)
            return reject("training build is incompatible");
    } else {
        return reject("worker does not support this portable workload");
    }
    if (offer.job_id.empty() || offer.attempt_id.empty() ||
        leader_id.empty() || !leader_epoch)
        return reject("portable offer or paired leader identity is incomplete");
    if (offer.expires_at_ms <= unix_millis())
        return reject("portable offer expired");
    if (!impl.storage_error.empty() || !impl.scheduler ||
        !impl.scheduler->state_error().empty())
        return reject("worker storage or scheduler is unavailable");
    if (state.restart_intent || impl.restart_pending(state))
        return reject("worker is restarting");
    if (state.maintenance || state.paused)
        return reject("worker is paused or in maintenance");
    if (impl.stop_all_pending(state))
        return reject("worker is stopping all agent jobs");
    if (impl.has_unacknowledged_result(state) ||
        impl.has_unacknowledged_portable_result())
        return reject("a previous worker result awaits confirmation");
    if (offer.workload == wire::PortableWorkload::Reconstruction) {
        for (const AcceptedAssignment& assignment : state.accepted)
            if (assignment.state == AttemptState::Accepted ||
                assignment.state == AttemptState::InProgress)
                return reject("a feature attempt is already active");
        if (impl.has_active_feature_job())
            return reject("the shared worker scheduler is executing feature work");
    }

    std::string error;
    portable::PortableOfferRecord known;
    if (portable::LoadPortableOfferRecord(
            impl.portable_root, offer.job_id, offer.attempt_id, known, error)) {
        bool authoritative = false;
        if (!exact_portable_offer(known.offer, offer) ||
            known.leader_id != leader_id ||
            known.leader_epoch != leader_epoch ||
            !portable::IsPortableAttemptAuthoritative(
                impl.portable_root, offer.job_id, offer.attempt_id, leader_id,
                leader_epoch, authoritative, error) ||
            !authoritative)
            return reject(error.empty()
                              ? "portable attempt belongs to a different offer or leader"
                              : error);
        std::optional<std::string> rejected_inputs;
        std::optional<std::string> acknowledgment;
        if (!portable::LoadPortableInputRejection(
                impl.portable_root, offer.job_id, offer.attempt_id,
                rejected_inputs, error) ||
            !portable::LoadPortableAcknowledgment(
                impl.portable_root, offer.job_id, offer.attempt_id,
                acknowledgment, error)) {
            impl.storage_error = error;
            return reject("portable attempt metadata is unavailable");
        }
        if (rejected_inputs || acknowledgment)
            return reject("portable attempt has already been finalized");
        return portable_decision_for(
            offer, wire::PortableDecision::Step::Offer,
            wire::PortableDecision::Decision::Accepted);
    }
    if (error != "portable attempt record is unavailable" &&
        error != "portable offer record is missing") {
        impl.storage_error = error;
        return reject("persisted portable offer is malformed");
    }

    std::vector<portable::PortableOfferRecord> authorities;
    if (!portable::LoadPortableAuthorities(impl.portable_root, authorities,
                                           error)) {
        impl.storage_error = error;
        return reject("persisted portable authority is malformed");
    }
    const std::string same_reconstruction_job =
        portable_scheduler_prefix(wire::PortableWorkload::Reconstruction,
                                  offer.job_id);
    const std::string same_training_job =
        portable_scheduler_prefix(wire::PortableWorkload::Training,
                                  offer.job_id);
    for (const sched::Job& job : impl.scheduler->list()) {
        if (!impl.scheduled(job)) continue;
        const bool reconstruction =
            job.options_payload.compare(0, 21, "agent-reconstruction:") == 0;
        const bool training =
            job.options_payload.compare(0, 15, "agent-training:") == 0;
        if (!reconstruction && !training) continue;
        const bool has_authority = std::any_of(
            authorities.begin(), authorities.end(),
            [&](const portable::PortableOfferRecord& record) {
                return job.options_payload ==
                       portable_scheduler_marker(record.offer);
            });
        if (!has_authority) {
            impl.storage_error =
                "scheduled portable job has no durable attempt authority";
            return reject(impl.storage_error);
        }
        if (job.options_payload.compare(0, same_reconstruction_job.size(),
                                        same_reconstruction_job) == 0 ||
            job.options_payload.compare(0, same_training_job.size(),
                                        same_training_job) == 0)
            return reject("another attempt for this portable job is active");
    }
    if (!impl.available_device())
        return reject("no free policy-approved Vulkan GPU or worker slot is available");
    std::error_code disk_error;
    const fs::space_info available = fs::space(impl.storage_root, disk_error);
    if (!impl.config.disk_budget_bytes || disk_error || !available.available)
        return reject("worker storage has no usable disk capacity");
    try {
        const std::uint64_t declared = manifest_size(offer.inputs);
        const std::uint64_t budget = impl.budget_for_portable_input(offer);
        if (declared > budget || declared > available.available)
            return reject("portable input bundle exceeds worker disk capacity");
    } catch (const std::exception& e) {
        return reject(e.what());

    }
    const portable::OfferRecordStatus stored = portable::SavePortableOffer(
        impl.portable_root, offer, leader_id, leader_epoch, error);
    if (stored == portable::OfferRecordStatus::Conflict)
        return reject("portable attempt conflicts with durable worker authority");
    if (stored == portable::OfferRecordStatus::Error) {
        impl.storage_error = error;
        return reject(error.empty() ? "cannot persist portable offer" : error);
    }
    impl.prepared_portable.reset();
    return portable_decision_for(
        offer, wire::PortableDecision::Step::Offer,
        wire::PortableDecision::Decision::Accepted);
}

TransferResult FeatureWorker::receive_portable_inputs(
    TlsChannel& channel, const wire::PortableOffer& offer) {
    Impl& impl = *_impl;
    std::string error;
    portable::PortableOfferRecord record;
    if (!portable::LoadPortableOfferRecord(
            impl.portable_root, offer.job_id, offer.attempt_id, record, error) ||
        !exact_portable_offer(record.offer, offer))
        return {TransferError::InvalidManifest,
                error.empty() ? "persisted portable offer changed" : error};
    bool authoritative = false;
    if (!portable::IsPortableAttemptAuthoritative(
            impl.portable_root, offer.job_id, offer.attempt_id, record.leader_id,
            record.leader_epoch, authoritative, error) ||
        !authoritative)
        return {TransferError::InvalidManifest,
                error.empty() ? "portable offer is no longer authoritative" : error};
    const fs::path root = impl.portable_input_directory(offer);
    if (!ensure_directory(root, error))
        return {TransferError::Filesystem, error};
    std::uint64_t budget = 0;
    try {
        budget = impl.budget_for_portable_input(offer);
    } catch (const std::exception& e) {
        return {TransferError::QuotaExceeded, e.what()};
    }
    return ReceiveArtifacts(channel, root, offer.inputs, budget);
}

bool FeatureWorker::verify_portable_inputs(const wire::PortableOffer& offer,
                                           std::string& error) {
    Impl& impl = *_impl;
    impl.prepared_portable.reset();
    try {
        if (offer.expires_at_ms <= unix_millis())
            throw std::runtime_error(
                "portable offer expired during input transfer");
        portable::PortableOfferRecord record;
        if (!portable::LoadPortableOfferRecord(
                impl.portable_root, offer.job_id, offer.attempt_id, record,
                error) ||
            !exact_portable_offer(record.offer, offer)) {
            if (error.empty()) error = "portable offer changed during transfer";
            throw std::runtime_error(error);
        }
        bool authoritative = false;
        if (!portable::IsPortableAttemptAuthoritative(
                impl.portable_root, offer.job_id, offer.attempt_id,
                record.leader_id, record.leader_epoch, authoritative, error) ||
            !authoritative)
            throw std::runtime_error(
                error.empty() ? "portable attempt was superseded" : error);
        if (offer.workload == wire::PortableWorkload::Reconstruction) {
            if (record.offer.required_build != ReconstructionBuildIdentity())
                throw std::runtime_error(
                    "reconstruction input build is incompatible");
            const ReconstructionInputBundle inputs = VerifyReconstructionInputs(
                impl.portable_input_directory(offer), offer.inputs,
                offer.input_identity_sha256, impl.config.disk_budget_bytes);
            const fs::path lease_root = impl.portable_output_lease_root(offer);
            const fs::path workspace = impl.portable_workspace(offer);
            if (!ensure_directory(lease_root, error) ||
                !ensure_directory(workspace, error))
                throw std::runtime_error(error);
            (void)DecodeReconstructionRequest(inputs, workspace,
                                              impl.config.disk_budget_bytes);
        } else if (offer.workload == wire::PortableWorkload::Training) {
#ifdef SS_TOOL_TRAIN
            if (!training_build_available() ||
                record.offer.required_build != SS_VERSION)
                throw std::runtime_error("training input build is incompatible");
            const TrainingInputBundle inputs = VerifyTrainingInputs(
                impl.portable_input_directory(offer), offer.inputs,
                offer.input_identity_sha256, impl.config.disk_budget_bytes);
            const TrainingInvocationOptions invocation =
                MakeTrainingInvocation(inputs);
            const fs::path lease_root = impl.portable_output_lease_root(offer);
            const fs::path workspace = impl.portable_workspace(offer);
            if (!ensure_directory(lease_root, error) ||
                !ensure_directory(workspace, error))
                throw std::runtime_error(error);
            (void)invocation;
#else
            throw std::runtime_error("training is unavailable in this build");
#endif
        } else {
            throw std::runtime_error("unsupported portable workload");
        }
        impl.prepared_portable = Impl::PreparedPortable{offer};
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

wire::PortableDecision FeatureWorker::reject_portable_inputs(
    const wire::PortableOffer& offer, const std::string& reason) {
    Impl& impl = *_impl;
    std::string error;
    if (!portable::SavePortableInputRejection(
            impl.portable_root, offer.job_id, offer.attempt_id, reason, error)) {
        impl.storage_error = error;
        return portable_decision_for(
            offer, wire::PortableDecision::Step::Inputs,
            wire::PortableDecision::Decision::Rejected,
            "cannot persist portable input rejection");
    }
    impl.prepared_portable.reset();
    return portable_decision_for(
        offer, wire::PortableDecision::Step::Inputs,
        wire::PortableDecision::Decision::Rejected,
        reason.empty() ? "reconstruction inputs were rejected" : reason);
}

wire::PortableDecision FeatureWorker::accept_portable_inputs(
    const wire::PortableOffer& offer, const State& state) {
    Impl& impl = *_impl;
    const auto reject = [&](const std::string& reason) {
        return portable_decision_for(
            offer, wire::PortableDecision::Step::Inputs,
            wire::PortableDecision::Decision::Rejected, reason);
    };
    if (!impl.prepared_portable ||
        !exact_portable_offer(impl.prepared_portable->offer, offer))
        return reject("portable inputs were not verified");
    std::string error;
    portable::PortableOfferRecord record;
    if (!portable::LoadPortableOfferRecord(
            impl.portable_root, offer.job_id, offer.attempt_id, record, error))
        return reject(error);
    bool authoritative = false;
    if (!portable::IsPortableAttemptAuthoritative(
            impl.portable_root, offer.job_id, offer.attempt_id, record.leader_id,
            record.leader_epoch, authoritative, error) ||
        !authoritative)
        return reject(error.empty() ? "portable attempt was superseded" : error);

    wire::PortableResult old_result;
    if (portable::LoadPortableResult(impl.portable_root, offer.job_id,
                                     offer.attempt_id, old_result, error)) {
        impl.prepared_portable.reset();
        return portable_decision_for(
            offer, wire::PortableDecision::Step::Inputs,
            wire::PortableDecision::Decision::Accepted);
    }
    if (error != "portable result record is missing") {
        impl.storage_error = error;
        return reject("persisted portable result is malformed");
    }
    const std::string marker = portable_scheduler_marker(offer);
    const bool already_scheduled = impl.has_scheduler_marker(marker);
    if (!already_scheduled) {
        if (state.restart_intent || impl.restart_pending(state) ||
            state.maintenance || state.paused ||
            impl.stop_all_pending(state) || !impl.scheduler ||
            !impl.storage_error.empty() ||
            !impl.scheduler->state_error().empty())
            return reject("worker scheduler is unavailable or paused");
        if (offer.workload == wire::PortableWorkload::Reconstruction) {
            for (const AcceptedAssignment& assignment : state.accepted)
                if (assignment.state == AttemptState::Accepted ||
                    assignment.state == AttemptState::InProgress)
                    return reject("a feature attempt is already active");
            if (impl.has_active_feature_job())
                return reject(
                    "the shared worker scheduler is executing feature work");
        }
        const std::optional<LocalDevice> device = impl.available_device();
        if (!device) {
            wire::PortableResult failed;
            failed.workload = offer.workload;
            failed.job_id = offer.job_id;
            failed.attempt_id = offer.attempt_id;
            failed.outcome = wire::PortableResult::Outcome::Failed;
            failed.error =
                "no free policy-approved Vulkan GPU or worker slot is available";
            (void)impl.set_portable_result(record, failed);
            impl.prepared_portable.reset();
            return portable_decision_for(
                offer, wire::PortableDecision::Step::Inputs,
                wire::PortableDecision::Decision::Accepted);
        }

        const fs::path input_root = impl.portable_input_directory(offer);
        const fs::path lease_root = impl.portable_output_lease_root(offer);
        const fs::path workspace = impl.portable_workspace(offer);
        std::string directory_error;
        if (!ensure_directory(lease_root, directory_error) ||
            !ensure_directory(workspace, directory_error)) {
            wire::PortableResult failed;
            failed.workload = offer.workload;
            failed.job_id = offer.job_id;
            failed.attempt_id = offer.attempt_id;
            failed.outcome = wire::PortableResult::Outcome::Failed;
            failed.error = directory_error.empty()
                               ? "cannot prepare the worker-owned portable workspace"
                               : directory_error;
            (void)impl.set_portable_result(record, failed);
            impl.prepared_portable.reset();
            return portable_decision_for(
                offer, wire::PortableDecision::Step::Inputs,
                wire::PortableDecision::Decision::Accepted);
        }

        sched::WorkflowSubmitOpts options;
        options.work_dir = impl.storage_root.u8string();
        options.options_payload = marker;
        options.add_scheduler_publish = false;
        options.path_claims.push_back({input_root.u8string(), false});
        options.path_claims.push_back({lease_root.u8string(), true});
        sched::Phase phase;
        const bool training =
            offer.workload == wire::PortableWorkload::Training;
        phase.phase = training ? "train" : "sfm";
        phase.planned_device = device->selector;
        phase.planned_device_name = device->name;
        phase.output = impl.portable_output_root(offer).u8string();
        phase.args = {training ? "--agent-portable-training"
                               : "--agent-portable-reconstruction",
                      offer.job_id, offer.attempt_id,
                      std::to_string(impl.config.disk_budget_bytes)};
        options.phases.push_back(std::move(phase));
        if (impl.scheduler->submit(options).empty()) {
            wire::PortableResult failed;
            failed.workload = offer.workload;
            failed.job_id = offer.job_id;
            failed.attempt_id = offer.attempt_id;
            failed.outcome = wire::PortableResult::Outcome::Failed;
            failed.error = "local JobScheduler refused the verified portable workload";
            (void)impl.set_portable_result(record, failed);
        }
    }
    impl.prepared_portable.reset();
    impl.sync_controls(state);
    return portable_decision_for(
        offer, wire::PortableDecision::Step::Inputs,
        wire::PortableDecision::Decision::Accepted);
}
void FeatureWorker::refresh(State& state) {
    Impl& impl = *_impl;
    if (impl.scheduler) impl.scheduler->drain_events();
    const std::vector<sched::Job> jobs =
        impl.scheduler ? impl.scheduler->list() : std::vector<sched::Job>{};
    const std::vector<AcceptedAssignment> attempts = state.accepted;
    for (const AcceptedAssignment& item : attempts) {
        wire::FeatureOffer offer;
        std::string error;
        if (!LoadFeatureOffer(impl.shard_root, item.job_id, item.attempt_id,
                              offer, error)) {
            impl.storage_error = error;
            continue;
        }
        std::optional<std::string> acknowledgment;
        if (!LoadFeatureAcknowledgment(impl.shard_root, item.job_id,
                                       item.attempt_id, acknowledgment, error)) {
            impl.storage_error = error;
            continue;
        }
        if (acknowledgment) {
            if (!safe_remove_tree(impl.input_directory(offer), false, error) ||
                !safe_remove_tree(impl.result_directory(offer), true, error))
                impl.storage_error = error;
            continue;
        }
        std::optional<std::string> input_rejection;
        if (!LoadFeatureInputRejection(impl.shard_root, item.job_id,
                                       item.attempt_id, input_rejection,
                                       error)) {
            impl.storage_error = error;
            continue;
        }
        if (input_rejection) {
            if (item.state != AttemptState::Accepted) {
                impl.storage_error = "started feature attempt has rejected inputs";
                continue;
            }
            state.accepted.erase(
                std::remove_if(state.accepted.begin(), state.accepted.end(),
                               [&](const AcceptedAssignment& candidate) {
                                   return candidate.job_id == item.job_id &&
                                          candidate.attempt_id == item.attempt_id &&
                                          candidate.state == AttemptState::Accepted;
                               }),
                state.accepted.end());
            try {
                persist_state(impl.state_root, state);
                if (!safe_remove_tree(impl.input_directory(offer), false, error))
                    impl.storage_error = error;
            } catch (const std::exception& e) {
                impl.storage_error = e.what();
            }
            continue;
        }
        wire::FeatureResult persisted;
        if (LoadFeatureResult(impl.shard_root, item.job_id, item.attempt_id,
                              persisted, error)) {
            if (item.state == AttemptState::InProgress)
                impl.set_result(offer, persisted, state);
            continue;
        }
        if (error != "feature result record is missing") {
            impl.storage_error = error;
            continue;
        }
        if (item.state == AttemptState::Accepted) {
            if (offer.expires_at_ms <= unix_millis()) {
                if (SaveFeatureInputRejection(impl.shard_root, item.job_id,
                                              item.attempt_id,
                                              "accepted offer expired before inputs completed",
                                              error)) {
                    state.accepted.erase(
                        std::remove_if(state.accepted.begin(), state.accepted.end(),
                                       [&](const AcceptedAssignment& candidate) {
                                           return candidate.job_id == item.job_id &&
                                                  candidate.attempt_id == item.attempt_id &&
                                                  candidate.state == AttemptState::Accepted;
                                       }),
                        state.accepted.end());
                    try {
                        persist_state(impl.state_root, state);
                        if (!safe_remove_tree(impl.input_directory(offer), false,
                                              error))
                            impl.storage_error = error;
                    } catch (const std::exception& e) {
                        impl.storage_error = e.what();
                    }
                } else {
                    impl.storage_error = error;
                }
            }
            continue;
        }

        const std::string key = FeatureAttemptKey(item.job_id, item.attempt_id);
        const auto found = std::find_if(jobs.begin(), jobs.end(),
                                        [&](const sched::Job& job) {
                                            return job.options_payload ==
                                                   "agent-feature:" + key;
                                        });
        wire::FeatureResult result;
        result.job_id = item.job_id;
        result.attempt_id = item.attempt_id;
        if (found == jobs.end()) {
            result.outcome = wire::FeatureResult::Outcome::Interrupted;
            result.error = "feature attempt was not restarted after worker recovery";
        } else if (found->state == sched::JobState::Succeeded) {
            try {
                const std::uint64_t quota = impl.budget_for_output(offer);
                const fs::path scratch = impl.attempt_directory(offer) /
                                         "output-request.json";
                const fs::path source = impl.input_directory(offer) / "request.json";
                std::error_code ec;
                const fs::file_status scratch_status = fs::symlink_status(scratch, ec);
                if (ec && ec != std::errc::no_such_file_or_directory)
                    throw std::runtime_error("cannot inspect feature output request");
                if (!ec && fs::exists(scratch_status) &&
                    (!fs::is_regular_file(scratch_status) ||
                     fs::is_symlink(scratch_status)))
                    throw std::runtime_error("feature output request copy is unsafe");
                fs::copy_file(source, scratch,
                              fs::copy_options::overwrite_existing, ec);
                if (ec) throw std::runtime_error("cannot prepare feature output request");
                result.outputs = FeatureOutputManifest(scratch,
                                                       impl.result_directory(offer),
                                                       quota);
                if (result.outputs.empty())
                    throw std::runtime_error("feature output manifest is empty");
                result.outcome = wire::FeatureResult::Outcome::Succeeded;
            } catch (const std::exception& e) {
                result.outcome = wire::FeatureResult::Outcome::Failed;
                result.error = clipped(e.what());
                result.outputs.clear();
            }
        } else if (found->state == sched::JobState::Interrupted ||
                   found->state == sched::JobState::Stopped) {
            result.outcome = wire::FeatureResult::Outcome::Interrupted;
            result.error = clipped(found->error.empty()
                                       ? "feature extraction was interrupted"
                                       : found->error);
        } else if (found->state == sched::JobState::Failed ||
                   found->state == sched::JobState::Blocked) {
            result.outcome = wire::FeatureResult::Outcome::Failed;
            result.error = clipped(found->error.empty()
                                       ? "feature extraction failed"
                                       : found->error);
        } else {
            continue;
        }
        if (!impl.set_result(offer, result, state)) continue;
    }
    std::vector<portable::PortableOfferRecord> portable_attempts;
    std::string portable_error;
    if (!portable::LoadPortableAuthorities(impl.portable_root,
                                           portable_attempts,
                                           portable_error)) {
        impl.storage_error = portable_error;
        return;
    }
    for (const portable::PortableOfferRecord& record : portable_attempts) {
        const wire::PortableOffer& offer = record.offer;
        bool authoritative = false;
        if (!portable::IsPortableAttemptAuthoritative(
                impl.portable_root, offer.job_id, offer.attempt_id,
                record.leader_id, record.leader_epoch, authoritative,
                portable_error)) {
            impl.storage_error = portable_error;
            continue;
        }
        if (!authoritative) continue;

        std::optional<std::string> acknowledgment;
        if (!portable::LoadPortableAcknowledgment(
                impl.portable_root, offer.job_id, offer.attempt_id,
                acknowledgment, portable_error)) {
            impl.storage_error = portable_error;
            continue;
        }
        if (acknowledgment) {
            if (!safe_remove_tree(impl.portable_input_directory(offer), false,
                                  portable_error) ||
                !safe_remove_tree(impl.portable_output_lease_root(offer), true,
                                  portable_error))
                impl.storage_error = portable_error;
            continue;
        }

        std::optional<std::string> input_rejection;
        if (!portable::LoadPortableInputRejection(
                impl.portable_root, offer.job_id, offer.attempt_id,
                input_rejection, portable_error)) {
            impl.storage_error = portable_error;
            continue;
        }
        if (input_rejection) {
            if (!safe_remove_tree(impl.portable_input_directory(offer), false,
                                  portable_error) ||
                !safe_remove_tree(impl.portable_output_lease_root(offer), true,
                                  portable_error))
                impl.storage_error = portable_error;
            continue;
        }

        wire::PortableResult persisted;
        if (portable::LoadPortableResult(impl.portable_root, offer.job_id,
                                         offer.attempt_id, persisted,
                                         portable_error))
            continue;
        if (portable_error != "portable result record is missing") {
            impl.storage_error = portable_error;
            continue;
        }

        const std::string marker = portable_scheduler_marker(offer);
        const auto found = std::find_if(
            jobs.begin(), jobs.end(), [&](const sched::Job& job) {
                return job.options_payload == marker;
            });
        if (found == jobs.end()) continue;
        wire::PortableResult result;
        result.workload = offer.workload;
        result.job_id = offer.job_id;
        result.attempt_id = offer.attempt_id;
        if (found->state == sched::JobState::Succeeded) {
            try {
                const std::uint64_t quota =
                    impl.budget_for_portable_output(offer);
                if (offer.workload ==
                    wire::PortableWorkload::Reconstruction) {
                    const ReconstructionInputBundle inputs =
                        VerifyReconstructionInputs(
                            impl.portable_input_directory(offer),
                            offer.inputs, offer.input_identity_sha256,
                            impl.config.disk_budget_bytes);
                    result.outputs = ReconstructionOutputManifest(
                        inputs, impl.portable_sparse_root(offer), quota);
                    if (result.outputs.empty())
                        throw std::runtime_error(
                            "reconstruction produced no verified sparse model");
                } else if (offer.workload ==
                           wire::PortableWorkload::Training) {
#ifdef SS_TOOL_TRAIN
                    const TrainingInputBundle inputs = VerifyTrainingInputs(
                        impl.portable_input_directory(offer), offer.inputs,
                        offer.input_identity_sha256,
                        impl.config.disk_budget_bytes);
                    const fs::path output = impl.portable_training_root(offer);
                    const std::string checkpoint =
                        ckpt::resolve_checkpoint(output)
                            .ckpt_dir.filename().u8string();
                    const TrainingOutputPackage package =
                        TrainingOutputManifest(
                            inputs, output, checkpoint, quota);
                    if (package.files.empty() ||
                        package.input_identity_sha256 !=
                            offer.input_identity_sha256)
                        throw std::runtime_error(
                            "training produced no input-bound checkpoint package");
                    result.outputs = package.files;
                    result.output_metadata = package.returned_checkpoint;
#else
                    throw std::runtime_error("training is unavailable in this build");
#endif
                } else {
                    throw std::runtime_error("unsupported portable workload");
                }
                result.outcome = wire::PortableResult::Outcome::Succeeded;
            } catch (const std::exception& e) {
                result.outcome = wire::PortableResult::Outcome::Failed;
                result.error = clipped(e.what());
                result.outputs.clear();
                result.output_metadata.clear();
            }
        } else if (found->state == sched::JobState::Interrupted ||
                   found->state == sched::JobState::Stopped) {
            result.outcome = wire::PortableResult::Outcome::Interrupted;
            result.error = clipped(found->error.empty()
                                       ? "portable work was interrupted"
                                       : found->error);
        } else if (found->state == sched::JobState::Failed ||
                   found->state == sched::JobState::Blocked) {
            result.outcome = wire::PortableResult::Outcome::Failed;
            result.error = clipped(found->error.empty()
                                       ? "portable work failed"
                                       : found->error);
        } else {
            continue;
        }
        (void)impl.set_portable_result(record, result);
    }
}

std::optional<wire::FeatureResult> FeatureWorker::pending_result(
    const State& state) {
    Impl& impl = *_impl;
    for (const AcceptedAssignment& item : state.accepted) {
        if (!terminal(item.state)) continue;
        std::optional<std::string> ack;
        std::string error;
        if (!LoadFeatureAcknowledgment(impl.shard_root, item.job_id,
                                       item.attempt_id, ack, error)) {
            impl.storage_error = error;
            return std::nullopt;
        }
        if (ack) continue;
        wire::FeatureResult result;
        if (LoadFeatureResult(impl.shard_root, item.job_id, item.attempt_id,
                              result, error))
            return result;
        impl.storage_error = error.empty()
                                 ? "terminal feature result is missing"
                                 : error;
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<wire::PortableResult> FeatureWorker::pending_portable_result(
    const std::string& leader_id, std::uint64_t leader_epoch) {
    Impl& impl = *_impl;
    std::vector<portable::PortableOfferRecord> records;
    std::string error;
    if (!portable::LoadPortableAuthorities(impl.portable_root, records, error)) {
        impl.storage_error = error;
        return std::nullopt;
    }
    for (const portable::PortableOfferRecord& record : records) {
        if (record.leader_id != leader_id ||
            record.leader_epoch != leader_epoch)
            continue;
        bool authoritative = false;
        if (!portable::IsPortableAttemptAuthoritative(
                impl.portable_root, record.offer.job_id,
                record.offer.attempt_id, leader_id, leader_epoch,
                authoritative, error)) {
            impl.storage_error = error;
            return std::nullopt;
        }
        if (!authoritative) continue;
        std::optional<std::string> acknowledgment;
        if (!portable::LoadPortableAcknowledgment(
                impl.portable_root, record.offer.job_id,
                record.offer.attempt_id, acknowledgment, error)) {
            impl.storage_error = error;
            return std::nullopt;
        }
        if (acknowledgment) continue;
        wire::PortableResult result;
        if (portable::LoadPortableResult(
                impl.portable_root, record.offer.job_id,
                record.offer.attempt_id, result, error))
            return result;
        if (error != "portable result record is missing") {
            impl.storage_error = error;
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<wire::PortableOffer> FeatureWorker::portable_offer_for_result(
    const wire::PortableResult& result, const std::string& leader_id,
    std::uint64_t leader_epoch) {
    Impl& impl = *_impl;
    if (result.workload != wire::PortableWorkload::Reconstruction &&
        result.workload != wire::PortableWorkload::Training)
        return std::nullopt;
    std::string error;
    portable::PortableOfferRecord record;
    if (!portable::LoadPortableOfferRecord(
            impl.portable_root, result.job_id, result.attempt_id, record,
            error))
        return std::nullopt;
    bool authoritative = false;
    if (record.leader_id != leader_id || record.leader_epoch != leader_epoch ||
        !portable::IsPortableAttemptAuthoritative(
            impl.portable_root, result.job_id, result.attempt_id, leader_id,
            leader_epoch, authoritative, error) ||
        !authoritative)
        return std::nullopt;
    wire::PortableResult persisted;
    std::string expected_bytes;
    std::string persisted_bytes;
    if (!portable::LoadPortableResult(impl.portable_root, result.job_id,
                                      result.attempt_id, persisted, error) ||
        wire::Encode(wire::Message{wire::kSchemaVersion, result}, 1,
                     expected_bytes) != wire::Error::None ||
        wire::Encode(wire::Message{wire::kSchemaVersion, persisted}, 1,
                     persisted_bytes) != wire::Error::None ||
        expected_bytes != persisted_bytes) {
        impl.storage_error =
            error.empty() ? "portable result changed before transfer" : error;
        return std::nullopt;
    }
    return record.offer;
}

TransferResult FeatureWorker::send_outputs(TlsChannel& channel,
                                           const wire::FeatureOffer& offer) {
    Impl& impl = *_impl;
    wire::FeatureResult result;
    std::string error;
    if (!LoadFeatureResult(impl.shard_root, offer.job_id, offer.attempt_id,
                           result, error) ||
        result.outcome != wire::FeatureResult::Outcome::Succeeded ||
        result.outputs.empty())
        return {TransferError::InvalidArgument,
                error.empty() ? "feature result has no transferable outputs" : error};
    return SendArtifacts(channel, impl.result_directory(offer), result.outputs,
                         impl.config.disk_budget_bytes);
}

void FeatureWorker::confirm_output(const wire::FeatureOffer& offer,
                                   const wire::FeatureDecision& decision) {
    Impl& impl = *_impl;
    if (decision.step != wire::FeatureDecision::Step::Output ||
        decision.job_id != offer.job_id ||
        decision.attempt_id != offer.attempt_id ||
        (decision.decision != wire::FeatureDecision::Decision::Committed &&
         decision.decision != wire::FeatureDecision::Decision::Rejected))
        throw std::invalid_argument("invalid feature output confirmation");
    wire::FeatureResult result;
    std::string error;
    if (!LoadFeatureResult(impl.shard_root, offer.job_id, offer.attempt_id,
                           result, error)) {
        impl.storage_error = error;
        throw std::runtime_error(error);
    }
    const std::string token =
        decision.decision == wire::FeatureDecision::Decision::Committed
            ? "committed"
            : "rejected";
    if (!SaveFeatureAcknowledgment(impl.shard_root, offer.job_id,
                                   offer.attempt_id, token, error)) {
        impl.storage_error = error;
        throw std::runtime_error(error);
    }
}

void FeatureWorker::cleanup_confirmed_output(
    const wire::FeatureOffer& offer) {
    Impl& impl = *_impl;
    std::optional<std::string> acknowledgment;
    std::string error;
    if (!LoadFeatureAcknowledgment(impl.shard_root, offer.job_id,
                                   offer.attempt_id, acknowledgment, error) ||
        !acknowledgment) {
        impl.storage_error =
            error.empty() ? "feature output is not durably acknowledged" : error;
        throw std::runtime_error(impl.storage_error);
    }
    if (!safe_remove_tree(impl.input_directory(offer), false, error) ||
        !safe_remove_tree(impl.result_directory(offer), true, error)) {
        impl.storage_error = error;
        throw std::runtime_error(error);
    }
    if (impl.prepared &&
        impl.prepared->input.request.attempt_id == offer.attempt_id)
        impl.prepared.reset();
}

TransferResult FeatureWorker::send_portable_outputs(
    TlsChannel& channel, const wire::PortableOffer& offer,
    const std::string& leader_id, std::uint64_t leader_epoch) {
    Impl& impl = *_impl;
    std::string error;
    portable::PortableOfferRecord record;
    if (!portable::LoadPortableOfferRecord(
            impl.portable_root, offer.job_id, offer.attempt_id, record, error) ||
        !exact_portable_offer(record.offer, offer))
        return {TransferError::InvalidManifest,
                error.empty() ? "persisted portable offer changed" : error};
    bool authoritative = false;
    if (record.leader_id != leader_id || record.leader_epoch != leader_epoch ||
        !portable::IsPortableAttemptAuthoritative(
            impl.portable_root, offer.job_id, offer.attempt_id, leader_id,
            leader_epoch, authoritative, error) ||
        !authoritative)
        return {TransferError::InvalidArgument,
                error.empty() ? "portable attempt is no longer authoritative" : error};
    wire::PortableResult result;
    if (!portable::LoadPortableResult(impl.portable_root, offer.job_id,
                                      offer.attempt_id, result, error) ||
        result.workload != offer.workload ||
        result.outcome != wire::PortableResult::Outcome::Succeeded ||
        result.outputs.empty())
        return {TransferError::InvalidArgument,
                error.empty() ? "portable result has no verified outputs"
                              : error};
    try {
        const std::uint64_t quota = impl.budget_for_portable_output(offer);
        if (offer.workload == wire::PortableWorkload::Reconstruction) {
            if (!result.output_metadata.empty())
                throw std::runtime_error(
                    "reconstruction result has unexpected metadata");
            const ReconstructionInputBundle inputs = VerifyReconstructionInputs(
                impl.portable_input_directory(offer), offer.inputs,
                offer.input_identity_sha256, impl.config.disk_budget_bytes);
            VerifyReconstructionOutputs(
                inputs, impl.portable_sparse_root(offer), result.outputs, quota);
        } else if (offer.workload == wire::PortableWorkload::Training) {
#ifdef SS_TOOL_TRAIN
            const TrainingInputBundle inputs = VerifyTrainingInputs(
                impl.portable_input_directory(offer), offer.inputs,
                offer.input_identity_sha256, impl.config.disk_budget_bytes);
            const TrainingOutputPackage manifest = TrainingOutputManifest(
                inputs, impl.portable_training_root(offer),
                result.output_metadata, quota);
            const auto same_files = [](const std::vector<TransferFile>& a,
                                       const std::vector<TransferFile>& b) {
                if (a.size() != b.size()) return false;
                for (std::size_t i = 0; i < a.size(); ++i)
                    if (a[i].path != b[i].path || a[i].size != b[i].size ||
                        a[i].sha256 != b[i].sha256)
                        return false;
                return true;
            };
            if (manifest.input_identity_sha256 !=
                    offer.input_identity_sha256 ||
                manifest.returned_checkpoint != result.output_metadata ||
                !same_files(manifest.files, result.outputs))
                throw std::runtime_error(
                    "training output changed after its durable manifest");
#else
            throw std::runtime_error("training is unavailable in this build");
#endif
        } else {
            throw std::runtime_error("unsupported portable workload");
        }
        if (!portable::IsPortableAttemptAuthoritative(
                impl.portable_root, offer.job_id, offer.attempt_id, leader_id,
                leader_epoch, authoritative, error) ||
            !authoritative)
            return {TransferError::InvalidArgument,
                    error.empty() ? "portable attempt was superseded" : error};
        return SendArtifacts(channel, impl.portable_output_root(offer),
                             result.outputs, quota);
    } catch (const std::exception& e) {
        return {TransferError::Integrity, e.what()};
    }
}

void FeatureWorker::confirm_portable_output(
    const wire::PortableOffer& offer,
    const wire::PortableDecision& decision, const std::string& leader_id,
    std::uint64_t leader_epoch) {
    Impl& impl = *_impl;
    if (decision.workload != offer.workload ||
        (offer.workload != wire::PortableWorkload::Reconstruction &&
         offer.workload != wire::PortableWorkload::Training) ||
        decision.step != wire::PortableDecision::Step::Output ||
        decision.job_id != offer.job_id ||
        decision.attempt_id != offer.attempt_id ||
        (decision.decision != wire::PortableDecision::Decision::Committed &&
         decision.decision != wire::PortableDecision::Decision::Rejected))
        throw std::invalid_argument("invalid portable output confirmation");
    std::string error;
    portable::PortableOfferRecord record;
    if (!portable::LoadPortableOfferRecord(
            impl.portable_root, offer.job_id, offer.attempt_id, record, error) ||
        !exact_portable_offer(record.offer, offer) ||
        record.leader_id != leader_id || record.leader_epoch != leader_epoch) {
        impl.storage_error =
            error.empty() ? "portable output authority changed" : error;
        throw std::runtime_error(impl.storage_error);
    }
    bool authoritative = false;
    if (!portable::IsPortableAttemptAuthoritative(
            impl.portable_root, offer.job_id, offer.attempt_id, leader_id,
            leader_epoch, authoritative, error) ||
        !authoritative) {
        impl.storage_error =
            error.empty() ? "portable output attempt was superseded" : error;
        throw std::runtime_error(impl.storage_error);
    }
    wire::PortableResult result;
    if (!portable::LoadPortableResult(impl.portable_root, offer.job_id,
                                      offer.attempt_id, result, error)) {
        impl.storage_error = error;
        throw std::runtime_error(error);
    }
    if (result.outcome != wire::PortableResult::Outcome::Succeeded &&
        decision.decision != wire::PortableDecision::Decision::Committed) {
        impl.storage_error = "failed portable attempts require terminal commit";
        throw std::runtime_error(impl.storage_error);
    }
    const std::string token =
        decision.decision == wire::PortableDecision::Decision::Committed
            ? "committed"
            : "rejected";
    if (!portable::SavePortableAcknowledgment(
            impl.portable_root, offer.job_id, offer.attempt_id, token, error)) {
        impl.storage_error = error;
        throw std::runtime_error(error);
    }
}

void FeatureWorker::cleanup_confirmed_portable_output(
    const wire::PortableOffer& offer) {
    Impl& impl = *_impl;
    std::optional<std::string> acknowledgment;
    std::string error;
    if (!portable::LoadPortableAcknowledgment(
            impl.portable_root, offer.job_id, offer.attempt_id,
            acknowledgment, error) ||
        !acknowledgment) {
        impl.storage_error =
            error.empty() ? "portable output is not durably acknowledged" : error;
        throw std::runtime_error(impl.storage_error);
    }
    if (!safe_remove_tree(impl.portable_input_directory(offer), false, error) ||
        !safe_remove_tree(impl.portable_output_lease_root(offer), true, error)) {
        impl.storage_error = error;
        throw std::runtime_error(error);
    }
    if (impl.prepared_portable &&
        impl.prepared_portable->offer.job_id == offer.job_id &&
        impl.prepared_portable->offer.attempt_id == offer.attempt_id)
        impl.prepared_portable.reset();
}

bool FeatureWorker::sync_controls(const State& state) {
    return _impl->sync_controls(state);
}

bool FeatureWorker::pause_acknowledged() const {
    return _impl->pause_acknowledged();
}

bool FeatureWorker::resume_acknowledged() const {
    return _impl->resume_acknowledged();
}

CommandOutcome FeatureWorker::stop_jobs(const std::string& target_job_id) {
    return _impl->stop_jobs(target_job_id);
}

bool FeatureWorker::stop_complete(const std::string& target_job_id) const {
    return _impl->stop_complete(target_job_id);
}
CommandOutcome FeatureWorker::restart_jobs(bool force, State& state) {
    Impl& impl = *_impl;
    if (!impl.scheduler || !impl.storage_error.empty() ||
        !impl.scheduler->state_error().empty())
        return CommandOutcome::Failed;

    impl.scheduler->pause_dispatch(true);
    const auto fail = [&] {
        try {
            (void)impl.sync_controls(state);
        } catch (...) {
        }
        return CommandOutcome::Failed;
    };
    try {
        std::vector<std::string> active;
        for (const sched::Job& job : impl.scheduler->list()) {
            if (!active_job(job)) continue;
            if (!agent_owned_job(job)) return fail();
            if (!force) {
                const bool train =
                    job.options_payload.compare(0, 15, "agent-training:") == 0 &&
                    job.phase == "train";
                const bool extraction =
                    job.options_payload.compare(0, 14, "agent-feature:") == 0 &&
                    job.phase == "sfm-extract";
                if (!train && !extraction) return fail();
            }
            active.push_back(job.job_id);
        }

        for (const std::string& job_id : active) {
            if (force) impl.scheduler->force_stop(job_id);
            else impl.scheduler->stop_and_save(job_id);
        }

        const auto deadline = std::chrono::steady_clock::now() +
            (force ? std::chrono::seconds(30) : std::chrono::minutes(5));
        for (;;) {
            bool running = false;
            for (const sched::Job& job : impl.scheduler->list())
                running = running || active_job(job);
            if (!running) break;
            if (!impl.scheduler->state_error().empty() ||
                std::chrono::steady_clock::now() >= deadline)
                return fail();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!force) {
            const std::vector<sched::Job> stopped_jobs =
                impl.scheduler->list();
            for (const std::string& job_id : active) {
                const auto found = std::find_if(
                    stopped_jobs.begin(), stopped_jobs.end(),
                    [&](const sched::Job& job) { return job.job_id == job_id; });
                if (found == stopped_jobs.end() ||
                    (found->state != sched::JobState::Stopped &&
                     found->state != sched::JobState::Succeeded))
                    return fail();
                if (found->state != sched::JobState::Stopped) continue;
                std::string error;
                if (found->phase == "train") {
                    const auto phase = std::find_if(
                        found->phases.begin(), found->phases.end(),
                        [&](const sched::Phase& item) {
                            return item.phase == found->phase;
                        });
                    if (phase == found->phases.end() ||
                        !impl.validate_scheduled_output(
                            *found, *phase, phase->output, error))
                        return fail();
                    continue;
                }
                const std::string prefix = "agent-feature:";
                const std::string marker = found->options_payload.substr(
                    prefix.size());
                const auto assignment = std::find_if(
                    state.accepted.begin(), state.accepted.end(),
                    [&](const AcceptedAssignment& item) {
                        return FeatureAttemptKey(item.job_id, item.attempt_id) ==
                               marker;
                    });
                wire::FeatureOffer offer;
                if (assignment == state.accepted.end() ||
                    !LoadFeatureOffer(impl.shard_root, assignment->job_id,
                                      assignment->attempt_id, offer, error))
                    return fail();
                const fs::path input = impl.input_directory(offer);
                const fw::FeatureRequest request =
                    fw::readRequestFile((input / "request.json").u8string());
                if (fw::normalizeRelativePath(request.plan_path) !=
                        request.plan_path ||
                    fs::u8path(request.plan_path).is_absolute() ||
                    request.digest != offer.request_digest ||
                    request.attempt_id != offer.attempt_id ||
                    request.plan_digest != offer.plan_digest)
                    return fail();
                const fw::FeaturePlan plan = fw::readPlanFile(
                    (input / fs::u8path(request.plan_path)).u8string());
                if (plan.digest != offer.plan_digest) return fail();
                const fs::path output = impl.result_directory(offer);
                const fs::path receipts = output / "receipts";
                std::error_code ec;
                const fs::file_status directory_status =
                    fs::symlink_status(receipts, ec);
                if (ec == std::errc::no_such_file_or_directory ||
                    directory_status.type() == fs::file_type::not_found)
                    continue;
                if (ec || !plain_directory(receipts)) return fail();
                for (fs::directory_iterator it(receipts, ec), end;
                     !ec && it != end; it.increment(ec)) {
                    const fs::path receipt_path = it->path();
                    const fs::file_status receipt_status =
                        it->symlink_status(ec);
                    if (ec || fs::is_symlink(receipt_status) ||
                        !fs::is_regular_file(receipt_status))
                        return fail();
#ifdef _WIN32
                    const DWORD attributes =
                        GetFileAttributesW(receipt_path.c_str());
                    if (attributes == INVALID_FILE_ATTRIBUTES ||
                        (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                        return fail();
#endif
                    const std::uintmax_t size =
                        fs::file_size(receipt_path, ec);
                    if (ec || size > (64ull << 20)) return fail();
                    const fw::ImageReceipt receipt =
                        fw::readReceiptFile(receipt_path.u8string());
                    std::string digits =
                        std::to_string(receipt.global_index);
                    if (digits.size() < 8)
                        digits.insert(0, 8 - digits.size(), '0');
                    if (receipt_path.filename().u8string() !=
                        digits + ".json")
                        return fail();
                    fw::validateReceipt(
                        plan, request, receipt,
                        (output / "payload").u8string());
                }
                if (ec) return fail();
            }
        }
        if (!impl.scheduler->save() ||
            !impl.scheduler->state_error().empty())
            return fail();
        refresh(state);
        if (std::any_of(state.accepted.begin(), state.accepted.end(),
                        [](const AcceptedAssignment& item) {
                            return item.state == AttemptState::InProgress;
                        }))
            return fail();
        if (!impl.storage_error.empty() ||
            !impl.scheduler->state_error().empty() ||
            !impl.scheduler->save())
            return fail();
        return CommandOutcome::Applied;
    } catch (...) {
        return fail();
    }
}

}  // namespace app::agent
