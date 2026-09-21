#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace spirula::project {

constexpr std::uint32_t kProjectSchemaVersion = 2;
constexpr std::uint32_t kDatasetPlanSchemaVersion = 1;
constexpr const char* kFrameKeyDomain = "spirula-frame-v1";

enum class SourceRelation { Original, Export };

enum class EvidenceClass {
    Unknown,
    MetadataOnly,
    Ambiguous,
    Independent,
    RawObservation,
};

enum class TimingAvailability {
    Unknown,
    Unavailable,
    MetadataOnly,
    Estimated,
    Supported,
};

enum class SourceExportStatus { Exact, Derived, Unsupported };

enum class SynchronizationPurpose {
    StaticOverlap,
    MovingRig,
    RgbdPose,
};

enum class DecisionStatus { Accepted, Review, Refused };

enum class DatasetRole { Pending, Train, Validation, Evaluation, Excluded };

struct RationalTime {
    std::int64_t num = 0;
    std::int64_t den = 1;
};

struct TimelineAnchor {
    RationalTime original;
    RationalTime exported;
};

struct SourceStream {
    std::string stream_id;
    std::string media_type;
    std::string codec;
    std::int64_t time_base_num = 0;
    std::int64_t time_base_den = 1;
};

struct OriginalMetadata {
    std::string media_type;
    std::string container;
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    std::uint64_t duration_num = 0;
    std::uint64_t duration_den = 1;
};

struct SourceRecord {
    std::string source_id;
    std::string digest_algorithm = "sha256";
    SourceRelation relation = SourceRelation::Original;
    std::string original_source_id;
    std::vector<std::filesystem::path> locators;
    std::uint64_t size_bytes = 0;
    std::vector<SourceStream> streams;
    std::vector<OriginalMetadata> original_metadata;
    std::vector<std::string> timing_features_supported;
    std::vector<std::string> timing_features_unsupported;
};

// Raw clock metadata is deliberately not a timing fit. It keeps the claims
// made by a source intact, including contradictory timezone/assumption fields.
struct RawClockRecord {
    std::string id;
    std::string source_id;
    std::string stream_id;
    std::string clock_domain;
    std::string timezone;
    std::vector<std::string> assumptions;
    std::vector<std::string> raw_fields;
    TimingAvailability timing_availability = TimingAvailability::Unknown;
    std::string revision;
};

struct SourceExportMapping {
    std::string id;
    std::string original_source_id;
    std::string export_source_id;
    SourceExportStatus status = SourceExportStatus::Unsupported;
    std::vector<TimelineAnchor> anchors;
    RationalTime support_start;
    RationalTime support_end;
    bool crosses_reset = false;
    std::string revision;
};

struct TimingEstimate {
    std::string id;
    std::vector<std::string> clock_refs;
    std::string mapping_id;
    std::string mapping_revision;
    double offset = 0;
    double rate = 1;
    bool rate_estimated = false;
    EvidenceClass evidence_class = EvidenceClass::Unknown;
    TimingAvailability timing_availability = TimingAvailability::Unknown;
    std::vector<std::string> evidence_refs;
    double residual = 0;
    double uncertainty = 0;
    double support_start = 0;
    double support_end = 0;
    double tolerance = 0;
    bool crosses_reset = false;
    std::string revision;
};

struct SynchronizationDecision {
    std::string id;
    SynchronizationPurpose purpose = SynchronizationPurpose::StaticOverlap;
    std::vector<std::string> members;
    std::string mapping_revision;
    double residual = 0;
    double uncertainty = 0;
    double support_start = 0;
    double support_end = 0;
    double tolerance = 0;
    EvidenceClass evidence_class = EvidenceClass::Unknown;
    TimingAvailability timing_availability = TimingAvailability::Unknown;
    std::vector<std::string> evidence_refs;
    DecisionStatus status = DecisionStatus::Refused;
    std::string reason;
    std::vector<std::string> consumers;
    bool crosses_reset = false;
    std::string revision;
};

SourceRecord make_source_record(const std::filesystem::path& locator);
void validate_source_record(const SourceRecord& source);
void verify_source_record(const SourceRecord& source);
SourceRecord relocate_source(SourceRecord source,
                             const std::filesystem::path& locator);
SourceRecord replace_source_locators(
    SourceRecord source, std::vector<std::filesystem::path> locators);

std::string stable_frame_id(const std::string& source_id,
                            const std::string& stream_id,
                            std::uint64_t original_presentation_ordinal);

void validate_raw_clock(const RawClockRecord& clock);
void validate_source_export_mapping(const SourceExportMapping& mapping);
void validate_timing_estimate(const TimingEstimate& estimate);
void validate_synchronization_decision(const SynchronizationDecision& decision);

bool decision_authorizes(const SynchronizationDecision& decision,
                         SynchronizationPurpose purpose,
                         const std::vector<std::string>& members,
                         double start,
                         double end,
                         std::string* reason = nullptr);
bool decision_authorizes(const SynchronizationDecision& decision,
                         const std::string& purpose,
                         const std::vector<std::string>& members,
                         double start,
                         double end,
                         std::string* reason = nullptr);

struct ArtifactReference {
    std::string artifact;
    std::string role;
    std::string transform;
};

struct DatasetPlanMember {
    std::string image;
    std::string source_id;
    std::string frame_id;
    std::string capture;
    std::string camera;
    std::string rig;
    std::string exclusion_group;
    DatasetRole role = DatasetRole::Pending;
};

struct DatasetPlan {
    std::uint32_t schema = kDatasetPlanSchemaVersion;
    std::vector<DatasetPlanMember> members;
};
struct DatasetRoleIndices {
    std::vector<std::int32_t> train;
    std::vector<std::int32_t> validation;
    std::vector<std::int32_t> evaluation;
};


struct DatasetPoseEvidence {
    std::string image;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

std::vector<std::size_t> select_training_by_coverage(
    const DatasetPlan& plan,
    const std::vector<DatasetPoseEvidence>& evidence,
    std::size_t max_members);
void freeze_exclusion_groups(DatasetPlan& plan);
DatasetRoleIndices resolve_dataset_roles(
    const DatasetPlan& plan,
    const std::filesystem::path& project_root,
    const std::vector<std::string>& image_files);



struct ProjectRevision {
    std::uint32_t schema = kProjectSchemaVersion;
    std::string name;
    std::vector<SourceRecord> sources;
    std::vector<RawClockRecord> raw_clocks;
    std::vector<SourceExportMapping> source_export_mappings;
    std::vector<TimingEstimate> timing_estimates;
    std::vector<SynchronizationDecision> synchronization_decisions;
    std::vector<std::string> parents;
    std::vector<std::string> captures;
    DatasetPlan dataset_plan;
    std::vector<ArtifactReference> artifacts;
    std::vector<std::string> protected_regions;
    std::vector<std::string> operations;
    std::vector<std::string> validations;
};
void validate_revision(const ProjectRevision& revision);
std::string serialize_revision(const ProjectRevision& revision);
ProjectRevision parse_revision(const std::string& text);

void publish_revision(const std::filesystem::path& project_dir,
                      const ProjectRevision& revision);
ProjectRevision read_revision(const std::filesystem::path& project_dir,
                              const std::string& name);
ProjectRevision read_current_revision(const std::filesystem::path& project_dir);
std::string revision_digest(const std::filesystem::path& project_dir,
                            const std::string& name);
ProjectRevision migrate_legacy_workspace(
    const std::filesystem::path& project_dir,
    const std::vector<std::filesystem::path>& source_roots,
    const std::vector<std::filesystem::path>& artifact_paths);

}  // namespace spirula::project
