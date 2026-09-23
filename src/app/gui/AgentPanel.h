#pragma once

#include "config/TrainConfig.h"

#include "app/AgentLeader.h"

#ifdef SS_TOOL_SFM
#include "sfm/Pipeline.h"
#endif

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace gui {

class AgentPanel {
public:
    struct FeatureShardSubmission {
        std::string worker_id;
        std::string job_id;
        std::filesystem::path plan_path;
        std::filesystem::path request_path;
        std::filesystem::path image_root;
        std::filesystem::path mask_root;
        std::uint64_t disk_budget_bytes = 0;
    };

    struct TrainingSubmission {
        std::string worker_id;
        std::string job_id;
        TrainConfig config;
        std::string preset;
        std::filesystem::path resume_checkpoint;
        std::uint64_t disk_budget_bytes = 0;
    };


#ifdef SS_TOOL_SFM
    struct ReconstructionSubmission {
        std::string worker_id;
        std::string job_id;
        sfm::AutoRequest request;
        std::filesystem::path source_manifest;
        std::uint64_t disk_budget_bytes = 0;
    };
#endif

    AgentPanel();
    ~AgentPanel();
    AgentPanel(const AgentPanel&) = delete;
    AgentPanel& operator=(const AgentPanel&) = delete;

    void open();
    void draw();
    void draw_status_strip();
    void shutdown();

    bool leader_running() const;
    std::vector<app::agent::LeaderServer::WorkerSnapshot> workers() const;
    std::vector<app::agent::LeaderServer::FeatureJobSnapshot> feature_jobs() const;
    std::string feature_error(const std::string& job_id) const;
    bool submit_feature_shard(FeatureShardSubmission submission);
    bool supersede_feature_shard(const std::string& job_id);
    std::vector<app::agent::LeaderServer::TrainingJobSnapshot>
    training_jobs() const;
    std::string training_error(const std::string& job_id) const;
    bool submit_training(TrainingSubmission submission);
    bool supersede_training(const std::string& job_id);
#ifdef SS_TOOL_SFM
    std::vector<app::agent::LeaderServer::ReconstructionJobSnapshot>
    reconstruction_jobs() const;
    std::string reconstruction_error(const std::string& job_id) const;
    bool submit_reconstruction(ReconstructionSubmission submission);
    bool supersede_reconstruction(const std::string& job_id);
#endif

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace gui
