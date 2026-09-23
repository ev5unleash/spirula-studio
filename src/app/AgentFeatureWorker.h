#pragma once

#include "app/AgentConfig.h"
#include "app/AgentState.h"
#include "app/AgentTls.h"
#include "app/AgentTransfer.h"
#include "app/AgentWire.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace app::agent {

namespace feature_worker_detail {

enum class OfferRecordStatus { Created, Existing, Conflict, Error };

struct FeatureOfferRecord {
    wire::FeatureOffer offer;
    std::string leader_id;
    std::uint64_t leader_epoch = 0;
};

std::string FeatureAttemptKey(const std::string& job_id,
                              const std::string& attempt_id);
OfferRecordStatus SaveFeatureOffer(const std::filesystem::path& root,
                                   const wire::FeatureOffer& offer,
                                   const std::string& leader_id,
                                   std::uint64_t leader_epoch,
                                   std::string& error);
bool LoadFeatureOfferRecord(const std::filesystem::path& root,
                            const std::string& job_id,
                            const std::string& attempt_id,
                            FeatureOfferRecord& record,
                            std::string& error);
bool LoadFeatureOffer(const std::filesystem::path& root,
                      const std::string& job_id,
                      const std::string& attempt_id,
                      wire::FeatureOffer& offer, std::string& error);
bool SaveFeatureResult(const std::filesystem::path& root,
                       const wire::FeatureResult& result,
                       std::string& error);
bool LoadFeatureResult(const std::filesystem::path& root,
                      const std::string& job_id,
                      const std::string& attempt_id,
                      wire::FeatureResult& result, std::string& error);
bool SaveFeatureInputRejection(const std::filesystem::path& root,
                               const std::string& job_id,
                               const std::string& attempt_id,
                               const std::string& reason,
                               std::string& error);
bool LoadFeatureInputRejection(const std::filesystem::path& root,
                               const std::string& job_id,
                               const std::string& attempt_id,
                               std::optional<std::string>& reason,
                               std::string& error);
bool SaveFeatureAcknowledgment(const std::filesystem::path& root,
                               const std::string& job_id,
                               const std::string& attempt_id,
                               const std::string& decision,
                               std::string& error);
bool LoadFeatureAcknowledgment(
    const std::filesystem::path& root, const std::string& job_id,
    const std::string& attempt_id, std::optional<std::string>& decision,
    std::string& error);

}  // namespace feature_worker_detail


namespace portable_worker_detail {

enum class OfferRecordStatus { Created, Existing, Superseded, Conflict, Error };

struct PortableOfferRecord {
    wire::PortableOffer offer;
    std::string leader_id;
    std::uint64_t leader_epoch = 0;
};

std::string PortableAttemptKey(const std::string& job_id,
                               const std::string& attempt_id);
std::filesystem::path PortableAttemptDirectory(
    const std::filesystem::path& root, const std::string& job_id,
    const std::string& attempt_id);
bool BindPortableLeader(const std::filesystem::path& root,
                        const std::string& leader_id,
                        std::uint64_t leader_epoch, std::string& error);
OfferRecordStatus SavePortableOffer(const std::filesystem::path& root,
                                    const wire::PortableOffer& offer,
                                    const std::string& leader_id,
                                    std::uint64_t leader_epoch,
                                    std::string& error);
bool LoadPortableOfferRecord(const std::filesystem::path& root,
                             const std::string& job_id,
                             const std::string& attempt_id,
                             PortableOfferRecord& record,
                             std::string& error);
bool LoadPortableOfferRecordFromAttempt(
    const std::filesystem::path& attempt_root, PortableOfferRecord& record,
    std::string& error);
bool LoadPortableAuthorities(const std::filesystem::path& root,
                             std::vector<PortableOfferRecord>& records,
                             std::string& error);
bool IsPortableAttemptAuthoritative(
    const std::filesystem::path& root, const std::string& job_id,
    const std::string& attempt_id, const std::string& leader_id,
    std::uint64_t leader_epoch, bool& authoritative, std::string& error);
bool SavePortableResult(const std::filesystem::path& root,
                        const wire::PortableResult& result,
                        std::string& error);
bool LoadPortableResult(const std::filesystem::path& root,
                        const std::string& job_id,
                        const std::string& attempt_id,
                        wire::PortableResult& result, std::string& error);
bool SavePortableInputRejection(const std::filesystem::path& root,
                                const std::string& job_id,
                                const std::string& attempt_id,
                                const std::string& reason,
                                std::string& error);
bool LoadPortableInputRejection(const std::filesystem::path& root,
                                const std::string& job_id,
                                const std::string& attempt_id,
                                std::optional<std::string>& reason,
                                std::string& error);
bool SavePortableAcknowledgment(const std::filesystem::path& root,
                                const std::string& job_id,
                                const std::string& attempt_id,
                                const std::string& decision,
                                std::string& error);
bool LoadPortableAcknowledgment(
    const std::filesystem::path& root, const std::string& job_id,
    const std::string& attempt_id, std::optional<std::string>& decision,
    std::string& error);

}  // namespace portable_worker_detail

// Owns the worker-local feature shard store and its one-GPU scheduler. Network
// protocol decisions stay in AgentClient so pairing locks never span transfers.
class FeatureWorker final {
public:
    FeatureWorker(const Config& config,
                  const std::filesystem::path& state_root,
                  const std::filesystem::path& storage_root,
                  const std::string& executable_path, const State& state);
    ~FeatureWorker();

    FeatureWorker(const FeatureWorker&) = delete;
    FeatureWorker& operator=(const FeatureWorker&) = delete;

    wire::Status status(const State& state);
    wire::FeatureDecision accept_offer(const wire::FeatureOffer& offer,
                                       State& state,
                                       const std::string& leader_id,
                                       std::uint64_t leader_epoch);
    bool verify_inputs(const wire::FeatureOffer& offer, std::string& error);
    wire::FeatureDecision reject_inputs(const wire::FeatureOffer& offer,
                                        State& state,
                                        const std::string& reason);
    TransferResult receive_inputs(TlsChannel& channel,
                                  const wire::FeatureOffer& offer);
    wire::FeatureDecision accept_inputs(const wire::FeatureOffer& offer,
                                        State& state);

    bool bind_portable_leader(const std::string& leader_id,
                              std::uint64_t leader_epoch);
    wire::PortableDecision accept_portable_offer(
        const wire::PortableOffer& offer, const State& state,
        const std::string& leader_id, std::uint64_t leader_epoch);
    TransferResult receive_portable_inputs(TlsChannel& channel,
                                           const wire::PortableOffer& offer);
    bool verify_portable_inputs(const wire::PortableOffer& offer,
                                std::string& error);
    wire::PortableDecision reject_portable_inputs(
        const wire::PortableOffer& offer, const std::string& reason);
    wire::PortableDecision accept_portable_inputs(
        const wire::PortableOffer& offer, const State& state);

    void refresh(State& state);
    std::optional<wire::FeatureResult> pending_result(const State& state);
    std::optional<wire::PortableResult> pending_portable_result(
        const std::string& leader_id, std::uint64_t leader_epoch);
    std::optional<wire::PortableOffer> portable_offer_for_result(
        const wire::PortableResult& result, const std::string& leader_id,
        std::uint64_t leader_epoch);

    void cleanup_confirmed_output(const wire::FeatureOffer& offer);
    TransferResult send_outputs(TlsChannel& channel,
                                const wire::FeatureOffer& offer);
    void confirm_output(const wire::FeatureOffer& offer,
                       const wire::FeatureDecision& decision);
    TransferResult send_portable_outputs(TlsChannel& channel,
                                         const wire::PortableOffer& offer,
                                         const std::string& leader_id,
                                         std::uint64_t leader_epoch);
    void confirm_portable_output(const wire::PortableOffer& offer,
                                 const wire::PortableDecision& decision,
                                 const std::string& leader_id,
                                 std::uint64_t leader_epoch);
    void cleanup_confirmed_portable_output(const wire::PortableOffer& offer);

    bool sync_controls(const State& state);
    bool pause_acknowledged() const;
    bool resume_acknowledged() const;
    CommandOutcome stop_jobs(const std::string& target_job_id);
    CommandOutcome restart_jobs(bool force, State& state);
    bool stop_complete(const std::string& target_job_id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace app::agent
