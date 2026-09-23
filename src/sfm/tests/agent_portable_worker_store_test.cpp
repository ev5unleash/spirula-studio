#include "app/AgentFeatureWorker.h"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {
void check(bool condition) {
    if (!condition)
        throw std::runtime_error("portable worker store contract failed");
}
}  // namespace
int main() {
    namespace fs = std::filesystem;
    namespace detail = app::agent::portable_worker_detail;
    namespace wire = app::agent::wire;

    // CTest gives each run a private working directory; keep nested hashed
    // store paths below Windows' legacy path limit instead of nesting under TEMP.
    const fs::path root = fs::current_path() /
        ("p" + std::to_string(std::chrono::steady_clock::now()
                                  .time_since_epoch().count()));
    fs::create_directories(root);

    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    wire::PortableOffer offer;
    offer.workload = wire::PortableWorkload::Reconstruction;
    offer.job_id = "job-store-test";
    offer.attempt_id = "attempt-store-test";
    offer.input_identity_sha256 = std::string(64, 'a');
    offer.required_build = "test-build";
    offer.expires_at_ms = static_cast<std::uint64_t>(now + 60'000);
    offer.inputs.push_back({"request.json", 7, std::string(64, 'b')});

    std::string error;
    check(detail::BindPortableLeader(root, "leader-old", 1, error));
    check(detail::SavePortableOffer(root, offer, "leader-old", 1, error) ==
          detail::OfferRecordStatus::Created);
    check(detail::SavePortableOffer(root, offer, "leader-old", 1, error) ==
          detail::OfferRecordStatus::Existing);

    wire::PortableOffer conflict = offer;
    conflict.input_identity_sha256 = std::string(64, 'c');
    check(detail::SavePortableOffer(root, conflict, "leader-old", 1, error) ==
          detail::OfferRecordStatus::Conflict);

    const fs::path attempt_directory =
        detail::PortableAttemptDirectory(root, offer.job_id, offer.attempt_id);
    fs::remove(attempt_directory / "offer.json");
    std::vector<detail::PortableOfferRecord> authorities;
    check(detail::LoadPortableAuthorities(root, authorities, error));
    check(authorities.size() == 1);
    check(authorities.front().offer.attempt_id == offer.attempt_id);
    check(detail::SavePortableOffer(root, offer, "leader-old", 1, error) ==
          detail::OfferRecordStatus::Existing);
    detail::PortableOfferRecord recovered;
    check(detail::LoadPortableOfferRecord(root, offer.job_id, offer.attempt_id,
                                          recovered, error));
    check(recovered.offer.input_identity_sha256 ==
          offer.input_identity_sha256);
    check(recovered.leader_id == "leader-old");
    check(recovered.leader_epoch == 1);

    wire::PortableResult result;
    result.workload = wire::PortableWorkload::Reconstruction;
    result.job_id = offer.job_id;
    result.attempt_id = offer.attempt_id;
    result.outcome = wire::PortableResult::Outcome::Failed;
    result.error = "SfM scheduler refused the attempt";
    check(detail::SavePortableResult(root, result, error));
    check(detail::SavePortableResult(root, result, error));
    wire::PortableResult changed = result;
    changed.outcome = wire::PortableResult::Outcome::Interrupted;
    changed.error = "different terminal outcome";
    check(!detail::SavePortableResult(root, changed, error));

    wire::PortableResult reopened;
    check(detail::LoadPortableResult(root, offer.job_id, offer.attempt_id,
                                     reopened, error));
    check(reopened.outcome == result.outcome);
    check(reopened.output_metadata.empty());
    check(reopened.outputs.empty());
    check(detail::SavePortableAcknowledgment(root, offer.job_id,
                                             offer.attempt_id, "committed",
                                             error));
    check(detail::SavePortableAcknowledgment(root, offer.job_id,
                                             offer.attempt_id, "committed",
                                             error));
    check(!detail::SavePortableAcknowledgment(root, offer.job_id,
                                              offer.attempt_id, "rejected",
                                              error));

    check(detail::BindPortableLeader(root, "leader-current", 2, error));
    wire::PortableOffer replacement = offer;
    replacement.attempt_id = "attempt-current";
    replacement.input_identity_sha256 = std::string(64, 'd');
    check(detail::SavePortableOffer(root, replacement, "leader-current", 2,
                                    error) ==
          detail::OfferRecordStatus::Superseded);

    bool authoritative = true;
    check(detail::IsPortableAttemptAuthoritative(
        root, offer.job_id, offer.attempt_id, "leader-old", 1,
        authoritative, error));
    check(!authoritative);
    check(detail::IsPortableAttemptAuthoritative(
        root, replacement.job_id, replacement.attempt_id, "leader-current", 2,
        authoritative, error));
    check(authoritative);
    check(!detail::SavePortableResult(root, result, error));
    check(detail::SavePortableOffer(root, offer, "leader-old", 1, error) ==
          detail::OfferRecordStatus::Conflict);

    wire::PortableOffer training_offer;
    training_offer.workload = wire::PortableWorkload::Training;
    training_offer.job_id = "job-training-store-test";
    training_offer.attempt_id = "attempt-training-store-test";
    training_offer.input_identity_sha256 = std::string(64, 'e');
    training_offer.required_build = "test-build";
    training_offer.expires_at_ms = static_cast<std::uint64_t>(now + 60'000);
    training_offer.inputs.push_back({"config.json", 7, std::string(64, 'f')});
    check(detail::SavePortableOffer(root, training_offer, "leader-current", 2,
                                    error) ==
          detail::OfferRecordStatus::Created);

    wire::PortableResult training_result;
    training_result.workload = wire::PortableWorkload::Training;
    training_result.job_id = training_offer.job_id;
    training_result.attempt_id = training_offer.attempt_id;
    training_result.outcome = wire::PortableResult::Outcome::Succeeded;
    training_result.outputs.push_back(
        {"step-000000007.ckpt/state.tar", 11, std::string(64, 'a')});
    training_result.output_metadata = "step-000000007.ckpt";
    check(detail::SavePortableResult(root, training_result, error));
    check(detail::SavePortableResult(root, training_result, error));
    wire::PortableResult replayed;
    check(detail::LoadPortableResult(root, training_offer.job_id,
                                     training_offer.attempt_id, replayed,
                                     error));
    check(replayed.workload == training_result.workload &&
          replayed.output_metadata == training_result.output_metadata &&
          replayed.outputs.size() == 1 &&
          replayed.outputs.front().path ==
              training_result.outputs.front().path);
    wire::PortableResult wrong_workload = training_result;
    wrong_workload.workload = wire::PortableWorkload::Reconstruction;
    wrong_workload.output_metadata.clear();
    check(!detail::SavePortableResult(root, wrong_workload, error));
    wire::PortableResult malformed_checkpoint = training_result;
    malformed_checkpoint.output_metadata = "step-7.ckpt";
    check(!detail::SavePortableResult(root, malformed_checkpoint, error));
    check(detail::IsPortableAttemptAuthoritative(
        root, training_offer.job_id, training_offer.attempt_id,
        "leader-current", 1, authoritative, error));
    check(!authoritative);

    check(detail::BindPortableLeader(root, "leader-current", 3, error));
    check(detail::IsPortableAttemptAuthoritative(
        root, training_offer.job_id, training_offer.attempt_id,
        "leader-current", 2, authoritative, error));
    check(!authoritative);
    check(!detail::SavePortableResult(root, training_result, error));
    wire::PortableOffer training_replacement = training_offer;
    training_replacement.attempt_id = "attempt-training-current";
    training_replacement.input_identity_sha256 = std::string(64, '1');
    training_replacement.expires_at_ms =
        static_cast<std::uint64_t>(now + 60'000);
    check(detail::SavePortableOffer(root, training_replacement,
                                    "leader-current", 3, error) ==
          detail::OfferRecordStatus::Superseded);
    check(detail::IsPortableAttemptAuthoritative(
        root, training_replacement.job_id, training_replacement.attempt_id,
        "leader-current", 3, authoritative, error));
    check(authoritative);

    fs::remove_all(root);
}
