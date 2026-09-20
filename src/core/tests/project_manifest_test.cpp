#include "data/ProjectManifest.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

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
        const fs::path exported_path = root / "exported.bin";
        write_bytes(exported_path, "export bytes");
        SourceRecord exported = project::make_source_record(exported_path);
        exported.relation = project::SourceRelation::Export;
        exported.original_source_id = source.source_id;
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
        revision.artifacts = {{"artifact-a", "image", "identity"}};
        revision.protected_regions = {"mask-a"};
        revision.operations = {"capture"};
        revision.validations = {"hash"};
        const std::string encoded = project::serialize_revision(revision);
        const ProjectRevision round_trip = project::parse_revision(encoded);
        check(project::serialize_revision(round_trip) == encoded,
              "revision JSON round-trip is stable");
        check(round_trip.raw_clocks[0].raw_fields == raw_clock.raw_fields &&
                  round_trip.raw_clocks[0].timezone == raw_clock.timezone &&
                  round_trip.source_export_mappings[0].anchors[0].exported.num ==
                      mapping.anchors[0].exported.num,
              "raw contradictions and rational anchors survive");
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
