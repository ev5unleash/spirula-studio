#include "data/ProjectManifest.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>


#include <stdexcept>

namespace fs = std::filesystem;
namespace project = spirula::project;
using project::ProjectRevision;
using project::SourceRecord;

namespace {

void write_bytes(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot create test file");
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("cannot write test file");
}
template <class F>
bool throws(F&& fn) {
    try {
        fn();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}
}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "spirula-project-manifest-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    if (ec) return 1;

    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        if (!ok) {
            std::fprintf(stderr, "FAIL %s\n", name);
            ++failures;
        }
    };

    try {
        const fs::path original = root / "original.bin";
        const fs::path moved = root / "moved.bin";
        write_bytes(original, "source bytes");
        SourceRecord source = project::make_source_record(original);
        fs::rename(original, moved, ec);
        if (ec) throw std::runtime_error("cannot move test source");
        source = project::relocate_source(source, moved);
        project::verify_source_record(source);
        check(source.locators.size() == 1 && source.locators.front() == moved,
              "locator replacement preserves source record");
        source.streams.push_back({"video", "video", "raw", 1, 30});
        source.original_metadata.push_back({"video", "container", 640, 480, 1, 30});
        source.timing_features_supported = {"pts"};
        source.timing_features_unsupported = {"wallclock"};

        write_bytes(moved, "rewritten bytes");
        const SourceRecord rewritten = project::make_source_record(moved);
        check(source.source_id != rewritten.source_id,
              "rewritten source bytes change identity");
        check(throws([&] { project::verify_source_record(source); }),
              "rewritten locator fails source verification");
        write_bytes(moved, "source bytes");
        project::verify_source_record(source);

        const std::string frame_a = project::stable_frame_id(source.source_id, "video", 7);
        const std::string frame_b = project::stable_frame_id(source.source_id, "video", 7);
        check(frame_a == frame_b, "frame key is deterministic");
        check(frame_a != project::stable_frame_id(source.source_id, "video", 8),
              "presentation ordinal participates in frame key");
        check(frame_a != project::stable_frame_id(source.source_id, "audio", 7) &&
                  frame_a != project::stable_frame_id(rewritten.source_id, "video", 7),
              "source and stream identities participate in frame key");
        check(frame_a != project::stable_frame_id(
                             source.source_id, "video",
                             (std::uint64_t{1} << 32) + 7),
              "frame key preserves the full presentation ordinal range");
        project::DatasetPlan groups;
        groups.members = {
            {"capture-a/left/frame-0001.jpg", source.source_id, "", "capture-a",
             "left", "rig-a", "", project::DatasetRole::Train},
            {"capture-a/right/frame-0001.jpg", source.source_id, "", "capture-a",
             "right", "rig-a", "", project::DatasetRole::Validation},
            {"capture-b/left/frame-0001.jpg", source.source_id, "", "capture-b",
             "left", "rig-a", "", project::DatasetRole::Evaluation},
            {"capture-b/right/frame-0001.jpg", source.source_id, "", "capture-b",
             "right", "rig-a", "", project::DatasetRole::Excluded},
            {"capture-a/derived/view-a.jpg", source.source_id, frame_a, "capture-a",
             "view-a", "", "", project::DatasetRole::Pending},
            {"capture-a/derived/view-b.jpg", source.source_id, frame_a, "capture-a",
             "view-b", "", "", project::DatasetRole::Train},
            {"capture-a/singleton.jpg", source.source_id, "", "capture-a",
             "singleton", "", "", project::DatasetRole::Pending},
        };
        std::vector<std::string> images;
        std::vector<project::DatasetRole> roles;
        for (const auto& member : groups.members) {
            images.push_back(member.image);
            roles.push_back(member.role);
        }
        project::freeze_exclusion_groups(groups);
        const project::DatasetPlan frozen_groups = groups;
        project::freeze_exclusion_groups(groups);
        bool repeated = groups.members.size() == frozen_groups.members.size();
        bool order_roles = groups.members.size() == images.size();
        for (std::size_t i = 0; i < groups.members.size(); ++i) {
            repeated = repeated &&
                       groups.members[i].exclusion_group ==
                           frozen_groups.members[i].exclusion_group;
            order_roles = order_roles && groups.members[i].image == images[i] &&
                          groups.members[i].role == roles[i] &&
                          frozen_groups.members[i].image == images[i] &&
                          frozen_groups.members[i].role == roles[i];
        }
        const auto image_group = [](const project::DatasetPlan& plan,
                                    const std::string& image) {
            const auto found = std::find_if(
                plan.members.begin(), plan.members.end(),
                [&](const project::DatasetPlanMember& member) {
                    return member.image == image;
                });
            return found == plan.members.end() ? std::string{} : found->exclusion_group;
        };
        const auto group_a_left = image_group(groups, images[0]);
        const auto group_a_right = image_group(groups, images[1]);
        const auto group_b_left = image_group(groups, images[2]);
        const auto frame_view_a = image_group(groups, images[4]);
        const auto frame_view_b = image_group(groups, images[5]);
        const auto singleton = image_group(groups, images[6]);
        const bool hash_ids = std::all_of(
            groups.members.begin(), groups.members.end(),
            [](const project::DatasetPlanMember& member) {
                return member.exclusion_group.size() == 71 &&
                       member.exclusion_group.compare(0, 7, "sha256:") == 0;
            });
        check(hash_ids && repeated && order_roles,
              "freezing groups preserves order and roles deterministically");
        check(group_a_left == group_a_right && group_a_left != group_b_left &&
                  frame_view_a == frame_view_b && singleton != group_a_left &&
                  singleton != frame_view_a,
              "group precedence separates captures and shares frames");
        project::DatasetPlan permuted = frozen_groups;
        std::reverse(permuted.members.begin(), permuted.members.end());
        project::freeze_exclusion_groups(permuted);
        bool permutation_stable = true;
        for (const auto& member : frozen_groups.members)
            permutation_stable =
                permutation_stable &&
                image_group(permuted, member.image) == member.exclusion_group;
        check(permutation_stable, "group IDs are independent of member order");

        const fs::path exported_path = root / "exported.bin";
        write_bytes(exported_path, "export bytes");
        SourceRecord exported = project::make_source_record(exported_path);
        exported.relation = project::SourceRelation::Export;
        exported.original_source_id = source.source_id;
        exported.streams.push_back({"export-video", "video", "raw", 1, 30});
        project::validate_source_record(exported);

        project::RawClockRecord raw_clock;
        raw_clock.id = "clock-video";
        raw_clock.source_id = source.source_id;
        raw_clock.stream_id = "video";
        raw_clock.clock_domain = "capture";
        raw_clock.timezone = "UTC+02:00";
        raw_clock.assumptions = {"wallclock", "reset observed"};
        raw_clock.raw_fields = {"pts=9007199254740993", "pts=0"};
        raw_clock.timing_availability = project::TimingAvailability::Supported;
        raw_clock.revision = "revision-a";

        project::SourceExportMapping mapping;
        mapping.id = "mapping-a";
        mapping.original_source_id = source.source_id;
        mapping.export_source_id = exported.source_id;
        mapping.status = project::SourceExportStatus::Derived;
        mapping.anchors = {{{1, 1}, {10000000000000001LL, 1000}},
                           {{2, 1}, {10000000000000002LL, 1000}}};
        mapping.support_start = {1, 1};
        mapping.support_end = {10, 1};
        mapping.revision = "revision-a";

        project::TimingEstimate estimate;
        estimate.id = "estimate-a";
        estimate.clock_refs = {raw_clock.id};
        estimate.mapping_id = mapping.id;
        estimate.mapping_revision = mapping.revision;
        estimate.offset = 0.037;
        estimate.evidence_class = project::EvidenceClass::Independent;
        estimate.timing_availability = project::TimingAvailability::Estimated;
        estimate.evidence_refs = {"fit-a"};
        estimate.residual = 0.01;
        estimate.uncertainty = 0.02;
        estimate.support_start = 1;
        estimate.support_end = 10;
        estimate.tolerance = 0.1;
        estimate.revision = "revision-a";

        project::SynchronizationDecision decision;
        decision.id = "decision-a";
        decision.purpose = project::SynchronizationPurpose::MovingRig;
        decision.members = {"cam0", "imu0"};
        decision.mapping_revision = mapping.revision;
        decision.residual = 0.01;
        decision.uncertainty = 0.02;
        decision.support_start = 1;
        decision.support_end = 10;
        decision.tolerance = 0.1;
        decision.evidence_class = project::EvidenceClass::Independent;
        decision.timing_availability = project::TimingAvailability::Estimated;
        decision.evidence_refs = {estimate.id};
        decision.status = project::DecisionStatus::Accepted;
        decision.reason = "independently fitted";
        decision.consumers = {"rig-solver"};
        decision.revision = "revision-a";


        write_bytes(root / "artifact-a", "artifact bytes");
        ProjectRevision revision;
        revision.name = "revision-a";
        revision.sources = {source, exported};
        revision.raw_clocks = {raw_clock};
        revision.source_export_mappings = {mapping};
        revision.timing_estimates = {estimate};
        revision.synchronization_decisions = {decision};
        revision.parents = {"root"};
        revision.captures = {frame_a};
        revision.dataset_plan.members = {
            {"images/original.jpg", source.source_id, frame_a, "capture-original",
             "camera-original", "rig-a", "group-a", project::DatasetRole::Train},
            {"images/exported.jpg", exported.source_id, "", "capture-exported",
             "camera-exported", "", "group-exported", project::DatasetRole::Evaluation},
        };
        revision.artifacts = {{"artifact-a", "image", "identity"}};
        revision.protected_regions = {"mask-a"};
        revision.operations = {"capture"};
        revision.validations = {"hash"};
        const std::string encoded = project::serialize_revision(revision);
        const ProjectRevision round_trip = project::parse_revision(encoded);
        check(project::serialize_revision(round_trip) == encoded,
              "revision JSON round-trip is stable");
        check(round_trip.schema == project::kProjectSchemaVersion &&
                  round_trip.dataset_plan.schema == project::kDatasetPlanSchemaVersion &&
                  round_trip.dataset_plan.members.size() == 2 &&
                  round_trip.dataset_plan.members[0].image == "images/original.jpg" &&
                  round_trip.dataset_plan.members[0].source_id == source.source_id &&
                  round_trip.dataset_plan.members[0].role == project::DatasetRole::Train &&
                  round_trip.dataset_plan.members[1].source_id == exported.source_id &&
                  round_trip.dataset_plan.members[1].role ==
                      project::DatasetRole::Evaluation &&
                  encoded.find("\"role\": \"train\"") != std::string::npos &&
                  encoded.find("\"role\": \"evaluation\"") != std::string::npos,
              "dataset plan preserves member order, sources, and roles");
        check(round_trip.raw_clocks[0].raw_fields == raw_clock.raw_fields &&
                  round_trip.raw_clocks[0].timezone == raw_clock.timezone &&
                  round_trip.source_export_mappings[0].anchors[0].exported.num ==
                      mapping.anchors[0].exported.num,
              "raw contradictions and rational anchors survive");
        ProjectRevision missing_clock_stream = revision;
        missing_clock_stream.raw_clocks[0].stream_id = "export-video";
        check(throws([&] { project::validate_revision(missing_clock_stream); }),
              "raw clock stream must be included in its source");

        const std::string plan_marker = "\"dataset_plan\": {\n    \"schema\"";
        std::string unknown_plan_field = encoded;
        const std::size_t plan_marker_pos = unknown_plan_field.find(plan_marker);
        if (plan_marker_pos == std::string::npos)
            throw std::runtime_error("dataset plan marker missing from test JSON");
        unknown_plan_field.replace(
            plan_marker_pos, plan_marker.size(),
            "\"dataset_plan\": {\n    \"unexpected\": true,\n    \"schema\"");
        check(throws([&] { project::parse_revision(unknown_plan_field); }),
              "unknown dataset plan field is rejected");

        const std::string member_marker = "\"image\": \"images/original.jpg\"";
        std::string unknown_member_field = encoded;
        const std::size_t member_marker_pos = unknown_member_field.find(member_marker);
        if (member_marker_pos == std::string::npos)
            throw std::runtime_error("dataset plan member marker missing from test JSON");
        unknown_member_field.replace(
            member_marker_pos, member_marker.size(),
            member_marker + ",\n            \"unexpected\": true");
        check(throws([&] { project::parse_revision(unknown_member_field); }),
              "unknown dataset plan member field is rejected");

        ProjectRevision duplicate_image = revision;
        duplicate_image.dataset_plan.members[1].image =
            duplicate_image.dataset_plan.members[0].image;
        check(throws([&] { project::validate_revision(duplicate_image); }),
              "duplicate dataset plan image is rejected");
        ProjectRevision missing_plan_source = revision;
        missing_plan_source.dataset_plan.members[0].source_id =
            "sha256:" + std::string(64, '0');
        check(throws([&] { project::validate_revision(missing_plan_source); }),
              "dataset plan source must be included in revision");
        ProjectRevision missing_plan_frame = revision;
        missing_plan_frame.dataset_plan.members[0].frame_id =
            "sha256:" + std::string(64, 'f');
        check(throws([&] { project::validate_revision(missing_plan_frame); }),
              "dataset plan frame must be listed in revision captures");
        ProjectRevision unfrozen_plan = revision;
        unfrozen_plan.dataset_plan.members[0].exclusion_group.clear();
        check(throws([&] { project::validate_revision(unfrozen_plan); }),
              "assigned dataset role requires a frozen exclusion group");
        ProjectRevision pending_plan = revision;
        pending_plan.dataset_plan.members[0].role = project::DatasetRole::Pending;
        pending_plan.dataset_plan.members[0].exclusion_group.clear();
        check(!throws([&] { project::validate_revision(pending_plan); }),
              "pending dataset member may omit an exclusion group");
        const fs::path role_root = root / "role-root";
        const auto role_image = [&](const char* name) {
            return (role_root / name).u8string();
        };
        project::DatasetPlan role_plan;
        role_plan.members = {
            {"train-a.jpg", "", "", "", "", "", "train-a", project::DatasetRole::Train},
            {"train-b.jpg", "", "", "", "", "", "train-b", project::DatasetRole::Train},
            {"validation.jpg", "", "", "", "", "", "validation",
             project::DatasetRole::Validation},
            {"evaluation.jpg", "", "", "", "", "", "evaluation",
             project::DatasetRole::Evaluation},
            {"excluded.jpg", "", "", "", "", "", "excluded",
             project::DatasetRole::Excluded},
        };
        const std::vector<std::string> role_images = {
            role_image("evaluation.jpg"), role_image("train-b.jpg"),
            role_image("excluded.jpg"), role_image("validation.jpg"),
            role_image("train-a.jpg"),
        };
        const project::DatasetRoleIndices role_indices =
            project::resolve_dataset_roles(role_plan, role_root, role_images);
        check(role_indices.train == std::vector<std::int32_t>{1, 4} &&
                  role_indices.validation == std::vector<std::int32_t>{3} &&
                  role_indices.evaluation == std::vector<std::int32_t>{0},
              "dataset roles map exactly and preserve image order");
        const std::vector<std::string> relative_role_images = {
            "evaluation.jpg", "train-b.jpg", "excluded.jpg",
            "validation.jpg", "train-a.jpg",
        };
        const project::DatasetRoleIndices relative_role_indices =
            project::resolve_dataset_roles(
                role_plan, role_root, relative_role_images);
        check(relative_role_indices.train == role_indices.train &&
                  relative_role_indices.validation == role_indices.validation &&
                  relative_role_indices.evaluation == role_indices.evaluation,
              "relative dataset images resolve against the project root");
        project::DatasetPlan pending_roles = role_plan;
        pending_roles.members.front().role = project::DatasetRole::Pending;
        check(throws([&] {
                  project::resolve_dataset_roles(pending_roles, role_root, role_images);
              }),
              "pending dataset role is rejected");
        project::DatasetPlan empty_plan;
        check(throws([&] {
                  project::resolve_dataset_roles(empty_plan, role_root, role_images);
              }),
              "empty dataset plan is rejected");
        std::vector<std::string> unmatched_image = role_images;
        unmatched_image.push_back(role_image("not-planned.jpg"));
        check(throws([&] {
                  project::resolve_dataset_roles(role_plan, role_root, unmatched_image);
              }),
              "unmatched dataset image is rejected");
        std::vector<std::string> duplicate_role_images = role_images;
        duplicate_role_images.push_back(role_images.front());
        check(throws([&] {
                  project::resolve_dataset_roles(
                      role_plan, role_root, duplicate_role_images);
              }),
              "duplicate dataset image is rejected");
        project::DatasetPlan unmatched_member = role_plan;
        unmatched_member.members.front().image = "not-parsed.jpg";
        check(throws([&] {
                  project::resolve_dataset_roles(unmatched_member, role_root, role_images);
              }),
              "unmatched plan member is rejected");
        ProjectRevision mixed_roles = revision;
        mixed_roles.dataset_plan.members[1].exclusion_group =
            mixed_roles.dataset_plan.members[0].exclusion_group;
        check(throws([&] { project::validate_revision(mixed_roles); }),
              "exclusion group cannot mix assigned roles");
        ProjectRevision pending_assigned_group = revision;
        pending_assigned_group.dataset_plan.members[1].exclusion_group =
            pending_assigned_group.dataset_plan.members[0].exclusion_group;
        pending_assigned_group.dataset_plan.members[1].role =
            project::DatasetRole::Pending;
        check(throws([&] { project::validate_revision(pending_assigned_group); }),
              "exclusion group cannot mix pending and assigned roles");

        project::DatasetPlan coverage_plan;
        coverage_plan.members = {
            {"scene/left-a.jpg", "", "", "", "", "", "group-left",
             project::DatasetRole::Pending},
            {"scene/left-b.jpg", "", "", "", "", "", "group-left",
             project::DatasetRole::Pending},
            {"scene/right.jpg", "", "", "", "", "", "group-right",
             project::DatasetRole::Pending},
            {"scene/assigned.jpg", "", "", "", "", "", "group-right",
             project::DatasetRole::Train},
            {"scene/center.jpg", "", "", "", "", "", "group-center",
             project::DatasetRole::Pending},
            {"scene/missing-a.jpg", "", "", "", "", "", "group-missing-a",
             project::DatasetRole::Pending},
            {"scene/missing-b.jpg", "", "", "", "", "", "group-missing-b",
             project::DatasetRole::Pending},
        };
        const std::vector<project::DatasetPoseEvidence> coverage_evidence = {
            {"scene/left-a.jpg", -10.0, 0.0, 0.0},
            {"scene/./left-b.jpg", -8.0, 0.0, 0.0},
            {"scene/./right.jpg", 10.0, 0.0, 0.0},
            {"scene/center.jpg", 1.0, 0.0, 0.0},
            {"scene/missing-b.jpg", std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0},
        };
        const project::DatasetPlan coverage_before = coverage_plan;
        const std::vector<std::size_t> covered =
            project::select_training_by_coverage(coverage_plan, coverage_evidence, 7);
        const std::vector<std::size_t> covered_repeat =
            project::select_training_by_coverage(coverage_plan, coverage_evidence, 7);
        const std::vector<std::size_t> expected_covered = {2, 0, 1, 4, 5, 6};
        bool coverage_unchanged = coverage_plan.members.size() ==
                                   coverage_before.members.size();
        for (std::size_t i = 0; coverage_unchanged && i < coverage_plan.members.size(); ++i) {
            const auto& got = coverage_plan.members[i];
            const auto& before = coverage_before.members[i];
            coverage_unchanged = got.image == before.image && got.exclusion_group == before.exclusion_group &&
                                 got.role == before.role;
        }
        check(covered == expected_covered && covered_repeat == covered &&
                  coverage_unchanged && std::find(covered.begin(), covered.end(), 3) ==
                                             covered.end(),
              "coverage selection prefers spread, is deterministic, and preserves roles");
        check(project::select_training_by_coverage(coverage_plan, coverage_evidence, 3) ==
                  std::vector<std::size_t>{2, 0, 1},
              "coverage selection includes whole exclusion groups within budget");
        check(project::select_training_by_coverage(coverage_plan, coverage_evidence, 0).empty(),
              "coverage selection honors zero budget");

        project::DatasetPlan oversized_coverage;
        oversized_coverage.members = {
            {"wide-a.jpg", "", "", "", "", "", "wide", project::DatasetRole::Pending},
            {"wide-b.jpg", "", "", "", "", "", "wide", project::DatasetRole::Pending},
            {"near.jpg", "", "", "", "", "", "near", project::DatasetRole::Pending},
        };
        const std::vector<project::DatasetPoseEvidence> oversized_evidence = {
            {"wide-a.jpg", -1.0, 0.0, 0.0},
            {"wide-b.jpg", 1.0, 0.0, 0.0},
            {"near.jpg", 0.0, 0.0, 0.0},
        };
        check(project::select_training_by_coverage(oversized_coverage, oversized_evidence, 1) ==
                  std::vector<std::size_t>{0, 1},
              "coverage selection allows the first oversized group and tie-breaks by ordinal");


        ProjectRevision default_empty;
        default_empty.name = "default-empty";
        const ProjectRevision default_round_trip =
            project::parse_revision(project::serialize_revision(default_empty));
        check(default_round_trip.dataset_plan.schema == project::kDatasetPlanSchemaVersion &&
                  default_round_trip.dataset_plan.members.empty(),
              "default-empty revisions carry an empty dataset plan");
        ProjectRevision missing_origin = revision;
        missing_origin.sources[1].original_source_id =
            "sha256:" + std::string(64, '0');
        check(throws([&] { project::validate_revision(missing_origin); }),
              "export source must reference an included original");
        ProjectRevision wrong_mapping_role = revision;
        wrong_mapping_role.source_export_mappings[0].original_source_id =
            exported.source_id;
        wrong_mapping_role.source_export_mappings[0].export_source_id = source.source_id;
        check(throws([&] { project::validate_revision(wrong_mapping_role); }),
              "source-export mapping enforces endpoint roles");
        SourceRecord other_original = source;
        other_original.source_id = "sha256:" + std::string(64, '1');
        ProjectRevision wrong_mapping_lineage = revision;
        wrong_mapping_lineage.sources.push_back(other_original);
        wrong_mapping_lineage.source_export_mappings[0].original_source_id =
            other_original.source_id;
        check(throws([&] { project::validate_revision(wrong_mapping_lineage); }),
              "source-export mapping enforces export lineage");

        ProjectRevision large_integers = revision;
        large_integers.sources[0].size_bytes =
            std::numeric_limits<std::uint64_t>::max();
        large_integers.sources[0].original_metadata[0].duration_num =
            9007199254740993ULL;
        large_integers.sources[0].streams[0].time_base_num = 9007199254740993LL;
        large_integers.sources[0].streams[0].time_base_den = 9007199254740995LL;
        const ProjectRevision large_round_trip =
            project::parse_revision(project::serialize_revision(large_integers));
        check(large_round_trip.sources[0].size_bytes ==
                  std::numeric_limits<std::uint64_t>::max() &&
                  large_round_trip.sources[0].original_metadata[0].duration_num ==
                      9007199254740993ULL &&
                  large_round_trip.sources[0].streams[0].time_base_num ==
                      9007199254740993LL &&
                  large_round_trip.sources[0].streams[0].time_base_den ==
                      9007199254740995LL,
              "large source integers and stream time bases survive JSON exactly");
        ProjectRevision oversized = revision;
        oversized.name = "oversized";
        oversized.operations = {std::string((std::size_t{64} << 20), 'x')};
        check(throws([&] { project::serialize_revision(oversized); }),
              "serialized revision size is bounded");

        check(project::decision_authorizes(round_trip.synchronization_decisions[0],
                                           project::SynchronizationPurpose::MovingRig,
                                           {"cam0"}, 2, 9),
              "supported independent fit authorizes its saved interval");
        project::SynchronizationDecision metadata_only = decision;
        metadata_only.evidence_class = project::EvidenceClass::MetadataOnly;
        check(!project::decision_authorizes(metadata_only,
                                            project::SynchronizationPurpose::MovingRig,
                                            {"cam0"}, 2, 9),
              "metadata-only moving rig is refused");
        project::SynchronizationDecision ambiguous = decision;
        ambiguous.evidence_class = project::EvidenceClass::Ambiguous;
        check(!project::decision_authorizes(ambiguous,
                                            project::SynchronizationPurpose::RgbdPose,
                                            {"cam0"}, 2, 9),
              "ambiguous RGB-D evidence is refused");
        project::SynchronizationDecision high_uncertainty = decision;
        high_uncertainty.uncertainty = 0.2;
        check(!project::decision_authorizes(high_uncertainty,
                                            project::SynchronizationPurpose::MovingRig,
                                            {"cam0"}, 2, 9),
              "high-uncertainty moving rig is refused");
        project::SynchronizationDecision reset = decision;
        reset.crosses_reset = true;
        check(!project::decision_authorizes(reset, project::SynchronizationPurpose::MovingRig,
                                            {"cam0"}, 2, 9),
              "reset-crossing synchronization is refused");
        check(!project::decision_authorizes(decision, project::SynchronizationPurpose::MovingRig,
                                            {"cam0"}, 0, 11),
              "extrapolated synchronization is refused");
        project::SynchronizationDecision static_pass = decision;
        static_pass.purpose = project::SynchronizationPurpose::StaticOverlap;
        static_pass.mapping_revision.clear();
        static_pass.timing_availability = project::TimingAvailability::Unavailable;
        check(project::decision_authorizes(static_pass,
                                           project::SynchronizationPurpose::StaticOverlap,
                                           {"cam0"}, 2, 9),
              "static independent pass needs no clock mapping");

        project::publish_revision(root, revision);
        check(project::read_current_revision(root).name == "revision-a",
              "first revision becomes current");

        ProjectRevision next = revision;
        next.name = "revision-b";
        next.parents = {"revision-a"};
        project::publish_revision(root, next);
        check(project::read_current_revision(root).name == "revision-b",
              "publishing a later revision advances current");
        const fs::path artifact_project = root / "artifact-project";
        ProjectRevision artifact_base = revision;
        artifact_base.name = "artifact-base";
        artifact_base.artifacts.clear();
        project::publish_revision(artifact_project, artifact_base);
        ProjectRevision current_artifact = artifact_base;
        current_artifact.name = "artifact-current";
        current_artifact.artifacts = {{"current", "test", "identity"}};
        check(throws([&] { project::publish_revision(artifact_project, current_artifact); }),
              "current pointer is rejected as an artifact");
        const fs::path current_alias = artifact_project / "current-alias";
        ec.clear();
        fs::create_symlink(artifact_project / "current", current_alias, ec);
        if (!ec) {
            ProjectRevision current_alias_artifact = artifact_base;
            current_alias_artifact.name = "artifact-current-alias";
            current_alias_artifact.artifacts = {{"current-alias", "test", "identity"}};
            check(throws([&] {
                      project::publish_revision(artifact_project, current_alias_artifact);
                  }),
                  "alias of current pointer is rejected as an artifact");
        }
        const fs::path artifact_target = artifact_project / "artifact-target";
        fs::create_directories(artifact_target, ec);
        const fs::path artifact_alias = artifact_project / "artifact-alias";
        ec.clear();
        fs::create_directory_symlink(artifact_target, artifact_alias, ec);
        if (!ec) {
            ProjectRevision artifact_inventory = artifact_base;
            artifact_inventory.name = "artifact-alias-base";
            artifact_inventory.artifacts = {{"artifact-alias", "test", "identity"}};
            project::publish_revision(artifact_project, artifact_inventory);
            const ProjectRevision artifact_reused =
                project::migrate_legacy_workspace(artifact_project, {}, {artifact_target});
            check(artifact_reused.name == artifact_inventory.name &&
                      artifact_reused.artifacts.size() == 1 &&
                      artifact_reused.artifacts[0].artifact == "artifact-alias",
                  "legacy artifacts deduplicate by canonical target");
        }


        const std::string digest_before = project::revision_digest(root, "revision-a");
        ProjectRevision modified_revision = revision;
        modified_revision.operations.push_back("same-name-rewrite");
        write_bytes(root / "revisions" / "revision-a.json",
                    project::serialize_revision(modified_revision));
        check(project::revision_digest(root, "revision-a") != digest_before,
              "same-name valid rewrite changes revision digest");
        check(throws([&] { project::publish_revision(root, revision); }),
              "immutable revision rejects conflicting bytes");

        ProjectRevision traversal = revision;
        traversal.name = "artifact-traversal";
        traversal.artifacts = {{"../artifact-a", "test", "identity"}};
        check(throws([&] { project::publish_revision(root, traversal); }),
              "artifact traversal is rejected before publication");
        ProjectRevision missing_artifact = revision;
        missing_artifact.name = "artifact-missing";
        missing_artifact.artifacts = {{"missing.bin", "test", "identity"}};
        check(throws([&] { project::publish_revision(root, missing_artifact); }),
              "missing artifact is rejected before publication");
        ProjectRevision duplicate_artifact = revision;
        duplicate_artifact.name = "artifact-duplicate";
        duplicate_artifact.artifacts = {{"artifact-a", "test", "identity"},
                                        {"./artifact-a", "test", "identity"}};
        check(throws([&] { project::publish_revision(root, duplicate_artifact); }),
              "normalized duplicate artifact is rejected");

        const fs::path escape_target = root.parent_path() / "spirula-manifest-escape";
        fs::create_directories(escape_target, ec);
        write_bytes(escape_target / "outside.bin", "outside");
        const fs::path escape_link = root / "escape-link";
        ec.clear();
        fs::create_directory_symlink(escape_target, escape_link, ec);
        if (!ec) {
            ProjectRevision symlink_escape = revision;
            symlink_escape.name = "artifact-symlink-escape";
            symlink_escape.artifacts = {{"escape-link", "test", "identity"}};
            check(throws([&] { project::publish_revision(root, symlink_escape); }),
                  "artifact symlink escape is rejected");
        }
        fs::remove_all(escape_target, ec);
        fs::remove(escape_link, ec);
        write_bytes(root / ".current.tmp-interrupted", "truncated");
        check(project::read_current_revision(root).name == "revision-b",
              "interrupted temporary pointer leaves current usable");
        check(throws([&] { project::parse_revision(encoded.substr(0, encoded.size() - 2)); }),
              "truncated revision JSON is rejected");

        const fs::path legacy_sources = root / "legacy-sources";
        const fs::path legacy_project = root / "legacy-project";
        fs::create_directories(legacy_sources, ec);
        fs::create_directories(legacy_project / "images", ec);
        write_bytes(legacy_sources / "a.bin", "same legacy source");
        write_bytes(legacy_sources / "b.bin", "same legacy source");
        const fs::path linked_target = root / "linked-source-target";
        const fs::path linked_file = linked_target / "linked.bin";
        fs::create_directories(linked_target, ec);
        write_bytes(linked_file, "linked source");
        const fs::path linked_root = legacy_sources / "linked";
        ec.clear();
        fs::create_directory_symlink(linked_target, linked_root, ec);
        const bool linked_supported = !ec;
        write_bytes(legacy_project / "images" / "frame.jpg", "prepared pixels");
        const ProjectRevision migrated = project::migrate_legacy_workspace(
            legacy_project, {legacy_sources, root / "missing-source"},
            {legacy_project / "images"});
        const auto duplicate_source = std::find_if(
            migrated.sources.begin(), migrated.sources.end(),
            [](const SourceRecord& candidate) { return candidate.locators.size() == 2; });
        check(duplicate_source != migrated.sources.end(),
              "legacy import keeps content identities and all locators");
        if (linked_supported) {
            const std::string linked_id = project::make_source_record(linked_file).source_id;
            check(std::find_if(
                      migrated.sources.begin(), migrated.sources.end(),
                      [&](const SourceRecord& candidate) {
                          return candidate.source_id == linked_id;
                      }) != migrated.sources.end(),
                  "legacy import follows linked source directories");
        }
        check(migrated.artifacts.size() == 1 &&
                  migrated.artifacts[0].artifact == "images" &&
                  std::find(migrated.operations.begin(), migrated.operations.end(),
                            "legacy-source-provenance-unknown") !=
                      migrated.operations.end() &&
                  std::find(migrated.operations.begin(), migrated.operations.end(),
                            "legacy-artifact-provenance-unknown") !=
                      migrated.operations.end(),
              "legacy import preserves unknown provenance explicitly");
        const ProjectRevision reused = project::migrate_legacy_workspace(
            legacy_project, {legacy_sources, root / "missing-source"},
            {legacy_project / "images"});
        check(reused.name == migrated.name,
              "legacy import reuses an equivalent current inventory");
        write_bytes(legacy_sources / "c.bin", "new legacy source");
        const ProjectRevision child = project::migrate_legacy_workspace(
            legacy_project, {legacy_sources}, {legacy_project / "images"});
        check(child.name != migrated.name &&
                  project::read_revision(legacy_project, migrated.name).name == migrated.name &&
                  child.parents.size() == 1 && child.parents.front() == migrated.name &&
                  child.sources.size() > migrated.sources.size() &&
                  child.captures == migrated.captures &&
                  child.protected_regions == migrated.protected_regions,
              "legacy inventory change publishes an immutable child with metadata");

        const fs::path metadata_project = root / "metadata-project";
        const fs::path metadata_sources = metadata_project / "sources";
        fs::create_directories(metadata_sources, ec);
        const fs::path metadata_original_path = metadata_sources / "original.bin";
        const fs::path metadata_export_path = metadata_sources / "export.bin";
        const fs::path metadata_export_moved = metadata_sources / "export-moved.bin";
        write_bytes(metadata_original_path, "metadata original bytes");
        write_bytes(metadata_export_path, "metadata export bytes");
        SourceRecord metadata_original = project::make_source_record(metadata_original_path);
        SourceRecord metadata_export = project::make_source_record(metadata_export_path);
        metadata_export.relation = project::SourceRelation::Export;
        metadata_export.original_source_id = metadata_original.source_id;
        metadata_export.streams.push_back({"export-stream", "video", "raw", 1, 30});
        metadata_export.original_metadata.push_back(
            {"video", "container", 1920, 1080, 30, 1});
        metadata_export.timing_features_supported = {"pts"};
        metadata_export.timing_features_unsupported = {"wallclock"};
        ProjectRevision metadata_revision;
        metadata_revision.name = "metadata-base";
        metadata_revision.sources = {metadata_original, metadata_export};
        project::publish_revision(metadata_project, metadata_revision);
        fs::rename(metadata_export_path, metadata_export_moved, ec);
        if (ec) throw std::runtime_error("cannot move metadata test source");
        const ProjectRevision metadata_migrated = project::migrate_legacy_workspace(
            metadata_project, {metadata_sources}, {});
        const auto moved_export = std::find_if(
            metadata_migrated.sources.begin(), metadata_migrated.sources.end(),
            [&](const SourceRecord& candidate) {
                return candidate.source_id == metadata_export.source_id;
            });
        check(moved_export != metadata_migrated.sources.end() &&
                  moved_export->relation == project::SourceRelation::Export &&
                  moved_export->original_source_id == metadata_original.source_id &&
                  moved_export->locators.size() == 1 &&
                  moved_export->locators.front().filename() == metadata_export_moved.filename() &&
                  moved_export->streams.size() == 1 &&
                  moved_export->streams.front().stream_id == "export-stream" &&
                  moved_export->original_metadata.size() == 1 &&
                  moved_export->original_metadata.front().width == 1920 &&
                  moved_export->timing_features_supported == std::vector<std::string>{"pts"} &&
                  moved_export->timing_features_unsupported ==
                      std::vector<std::string>{"wallclock"},
              "moved source retains authoritative metadata and relation");

        const fs::path partial_project = root / "partial-project";
        fs::create_directories(partial_project / "revisions", ec);
        check(throws([&] {
                  project::migrate_legacy_workspace(partial_project, {}, {});
              }),
              "legacy import refuses partial project publication");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL ProjectManifest: %s\n", error.what());
        ++failures;
    }

    fs::remove_all(root, ec);
    return failures ? 1 : 0;
}
