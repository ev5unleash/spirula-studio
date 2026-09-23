#include "app/AgentFeatureWorker.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>

int main() {
    namespace fs = std::filesystem;
    namespace detail = app::agent::feature_worker_detail;

    const fs::path root = fs::temp_directory_path() /
        ("spirula-feature-worker-store-" +
         std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch().count()));
    fs::create_directories(root);

    app::agent::wire::FeatureOffer original;
    original.job_id = "job-store-test";
    original.attempt_id = "attempt-store-test";
    original.plan_digest = std::string(64, 'a');
    original.request_digest = std::string(64, 'b');
    original.required_build = "test-build";
    original.expires_at_ms = 123456789;
    original.inputs.push_back({"plan.json", 7, std::string(64, 'c')});

    std::string error;
    assert(detail::SaveFeatureOffer(root, original, "leader-store-test", 1,
                                    error) ==
           detail::OfferRecordStatus::Created);

    // A new store invocation models reconnect/restart: exact metadata is
    // recovered and cannot silently be replaced by a conflicting offer.
    app::agent::wire::FeatureOffer reopened;
    assert(detail::LoadFeatureOffer(root, original.job_id, original.attempt_id,
                                    reopened, error));
    assert(reopened.job_id == original.job_id);
    assert(reopened.attempt_id == original.attempt_id);
    assert(reopened.request_digest == original.request_digest);
    assert(reopened.inputs.size() == 1);
    assert(reopened.inputs.front().path == "plan.json");
    assert(detail::SaveFeatureOffer(root, original, "leader-store-test", 1,
                                    error) ==
           detail::OfferRecordStatus::Existing);
    assert(detail::SaveFeatureOffer(root, original, "other-leader", 1,
                                    error) ==
           detail::OfferRecordStatus::Conflict);
    detail::FeatureOfferRecord authority;
    assert(detail::LoadFeatureOfferRecord(root, original.job_id,
                                          original.attempt_id, authority,
                                          error));
    assert(authority.leader_id == "leader-store-test");
    assert(authority.leader_epoch == 1);

    app::agent::wire::FeatureOffer conflicting = original;
    conflicting.request_digest = std::string(64, 'd');
    assert(detail::SaveFeatureOffer(root, conflicting, "leader-store-test", 1,
                                    error) ==
           detail::OfferRecordStatus::Conflict);
    assert(detail::LoadFeatureOffer(root, original.job_id, original.attempt_id,
                                    reopened, error));
    assert(reopened.request_digest == original.request_digest);

    app::agent::wire::FeatureResult result;
    result.job_id = original.job_id;
    result.attempt_id = original.attempt_id;
    result.outcome = app::agent::wire::FeatureResult::Outcome::Failed;
    result.error = "scheduler refused the verified attempt";
    assert(detail::SaveFeatureResult(root, result, error));
    app::agent::wire::FeatureResult recovered_result;
    assert(detail::LoadFeatureResult(root, result.job_id, result.attempt_id,
                                     recovered_result, error));
    assert(recovered_result.outcome == result.outcome);
    assert(recovered_result.outputs.empty());
    assert(detail::SaveFeatureAcknowledgment(root, result.job_id,
                                             result.attempt_id, "rejected",
                                             error));
    std::optional<std::string> acknowledgment;
    assert(detail::LoadFeatureAcknowledgment(root, result.job_id,
                                             result.attempt_id,
                                             acknowledgment, error));
    assert(acknowledgment && *acknowledgment == "rejected");
    assert(!detail::SaveFeatureAcknowledgment(root, result.job_id,
                                              result.attempt_id, "committed",
                                              error));
    fs::remove_all(root);
}
