#include "data/ProjectManifest.h"

#include "core/Sha256.h"
#include "data/Json.h"
#include "data/JsonWrite.h"

#include <atomic>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace spirula::project {
namespace {

constexpr std::uintmax_t kMaxRevisionBytes = (std::uintmax_t)64 << 20;
constexpr std::uintmax_t kMaxPointerBytes = 1024;

struct FileSnapshot {
    std::uintmax_t size = 0;
    fs::file_time_type write_time{};
};

FileSnapshot snapshot(const fs::path& path) {
    std::error_code ec;
    const fs::file_status status = fs::status(path, ec);
    if (ec) throw std::runtime_error("cannot stat " + path.u8string() + ": " + ec.message());
    if (!fs::is_regular_file(status))
        throw std::runtime_error(path.u8string() + " is not a regular file");

    FileSnapshot out;
    out.size = fs::file_size(path, ec);
    if (ec) throw std::runtime_error("cannot read size of " + path.u8string() + ": " + ec.message());
    out.write_time = fs::last_write_time(path, ec);
    if (ec) throw std::runtime_error("cannot read timestamp of " + path.u8string() + ": " + ec.message());
    return out;
}

bool same_snapshot(const FileSnapshot& a, const FileSnapshot& b) {
    return a.size == b.size && a.write_time == b.write_time;
}

struct HashedFile {
    std::string digest;
    std::uint64_t size = 0;
};

HashedFile hash_file(const fs::path& path) {
    const FileSnapshot before = snapshot(path);
    if (before.size > std::numeric_limits<std::uint64_t>::max())
        throw std::runtime_error(path.u8string() + " is too large");

    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.u8string());

    spirula::Sha256 sha;
    std::vector<char> chunk(1 << 20);
    std::uint64_t count = 0;
    for (;;) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) {
            const std::uint64_t n = static_cast<std::uint64_t>(got);
            if (count > std::numeric_limits<std::uint64_t>::max() - n)
                throw std::runtime_error(path.u8string() + " is too large");
            count += n;
            sha.update(reinterpret_cast<const std::uint8_t*>(chunk.data()),
                       static_cast<std::size_t>(got));
        }
        if (in.bad() || (!in.eof() && in.fail()))
            throw std::runtime_error("failed while reading " + path.u8string());
        if (in.eof()) break;
        if (got == 0)
            throw std::runtime_error("failed while reading " + path.u8string());
    }

    const FileSnapshot after = snapshot(path);
    if (count != before.size || !same_snapshot(before, after))
        throw std::runtime_error(path.u8string() + " changed while hashing");

    return {sha.hex(), count};
}

std::string digest_bytes(const std::string& bytes) {
    spirula::Sha256 sha;
    sha.update(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    return "sha256:" + sha.hex();
}

bool valid_utf8(const std::string& text) {
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c <= 0x7f) {
            ++i;
            continue;
        }
        std::size_t n = 0;
        std::uint32_t code = 0;
        std::uint32_t min_code = 0;
        if (c >= 0xc2 && c <= 0xdf) {
            n = 2; code = c & 0x1f; min_code = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            n = 3; code = c & 0x0f; min_code = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            n = 4; code = c & 0x07; min_code = 0x10000;
        } else {
            return false;
        }
        if (i + n > text.size()) return false;
        for (std::size_t j = 1; j < n; ++j) {
            const unsigned char part = static_cast<unsigned char>(text[i + j]);
            if ((part & 0xc0) != 0x80) return false;
            code = (code << 6) | (part & 0x3f);
        }
        if (code < min_code || code > 0x10ffff ||
            (code >= 0xd800 && code <= 0xdfff))
            return false;
        i += n;
    }
    return true;
}

bool valid_text(const std::string& text, bool allow_empty = true) {
    return (allow_empty || !text.empty()) && text.find('\0') == std::string::npos &&
           valid_utf8(text);
}

bool valid_source_id(const std::string& id) {
    if (id.size() != 71 || id.compare(0, 7, "sha256:") != 0) return false;
    for (std::size_t i = 7; i < id.size(); ++i) {
        const char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool valid_revision_name(const std::string& name) {
    if (name.empty() || name.size() > 128 || name == "." || name == "..") return false;
    const unsigned char first = static_cast<unsigned char>(name.front());
    if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
          (first >= '0' && first <= '9')))
        return false;
    for (unsigned char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            return false;
    }
    return true;
}

void require_text(const std::string& value, const char* field, bool allow_empty = false) {
    if (!valid_text(value, allow_empty))
        throw std::runtime_error(std::string("invalid ") + field);
}

void append_u64_be(std::string& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back(static_cast<char>((value >> shift) & 0xff));
}

void append_string_field(std::string& out, const std::string& value) {
    out.push_back(static_cast<char>(1));
    append_u64_be(out, static_cast<std::uint64_t>(value.size()));
    out.append(value);
}

void append_array(std::string& out, const std::vector<std::string>& values) {
    out.push_back('[');
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out.push_back(',');
        out += json_quote(values[i]);
    }
    out.push_back(']');
}

std::string source_json(const SourceRecord& source) {
    JsonWriter w;
    w.object();
    w.field("source_id", source.source_id);
    w.field("digest_algorithm", source.digest_algorithm);
    w.field("relation", source.relation == SourceRelation::Original ? "original" : "export");
    w.field("original_source_id", source.original_source_id);
    std::string locators;
    locators.push_back('[');
    for (std::size_t i = 0; i < source.locators.size(); ++i) {
        if (i) locators.push_back(',');
        locators += json_quote(source.locators[i].u8string());
    }
    locators.push_back(']');
    w.field_raw("locators", locators);
    w.field("size_bytes", std::to_string(source.size_bytes));

    std::string streams;
    streams.push_back('[');
    for (std::size_t i = 0; i < source.streams.size(); ++i) {
        if (i) streams.push_back(',');
        JsonWriter s;
        s.object();
        s.field("stream_id", source.streams[i].stream_id);
        s.field("media_type", source.streams[i].media_type);
        s.field("codec", source.streams[i].codec);
        s.field("time_base_num", std::to_string(source.streams[i].time_base_num));
        s.field("time_base_den", std::to_string(source.streams[i].time_base_den));
        streams += s.end().str();
    }
    streams.push_back(']');
    w.field_raw("streams", streams);

    std::string metadata;
    metadata.push_back('[');
    for (std::size_t i = 0; i < source.original_metadata.size(); ++i) {
        if (i) metadata.push_back(',');
        JsonWriter m;
        m.object();
        m.field("media_type", source.original_metadata[i].media_type);
        m.field("container", source.original_metadata[i].container);
        m.field("width", std::to_string(source.original_metadata[i].width));
        m.field("height", std::to_string(source.original_metadata[i].height));
        m.field("duration_num",
                std::to_string(source.original_metadata[i].duration_num));
        m.field("duration_den",
                std::to_string(source.original_metadata[i].duration_den));
        metadata += m.end().str();
    }
    metadata.push_back(']');
    w.field_raw("original_metadata", metadata);

    std::string supported;
    append_array(supported, source.timing_features_supported);
    w.field_raw("timing_features_supported", supported);
    std::string unsupported;
    append_array(unsupported, source.timing_features_unsupported);
    w.field_raw("timing_features_unsupported", unsupported);
    return w.end().str();
}

const char* evidence_name(EvidenceClass value) {
    switch (value) {
        case EvidenceClass::Unknown: return "unknown";
        case EvidenceClass::MetadataOnly: return "metadata_only";
        case EvidenceClass::Ambiguous: return "ambiguous";
        case EvidenceClass::Independent: return "independent";
        case EvidenceClass::RawObservation: return "raw_observation";
    }
    return "unknown";
}

const char* availability_name(TimingAvailability value) {
    switch (value) {
        case TimingAvailability::Unknown: return "unknown";
        case TimingAvailability::Unavailable: return "unavailable";
        case TimingAvailability::MetadataOnly: return "metadata_only";
        case TimingAvailability::Estimated: return "estimated";
        case TimingAvailability::Supported: return "supported";
    }
    return "unknown";
}

const char* mapping_status_name(SourceExportStatus value) {
    switch (value) {
        case SourceExportStatus::Exact: return "exact";
        case SourceExportStatus::Derived: return "derived";
        case SourceExportStatus::Unsupported: return "unsupported";
    }
    return "unsupported";
}

const char* purpose_name(SynchronizationPurpose value) {
    switch (value) {
        case SynchronizationPurpose::StaticOverlap: return "static_overlap";
        case SynchronizationPurpose::MovingRig: return "moving_rig";
        case SynchronizationPurpose::RgbdPose: return "rgbd_pose";
    }
    return "static_overlap";
}

const char* decision_status_name(DecisionStatus value) {
    switch (value) {
        case DecisionStatus::Accepted: return "accepted";
        case DecisionStatus::Review: return "review";
        case DecisionStatus::Refused: return "refused";
    }
    return "refused";
}

std::string rational_json(const RationalTime& value) {
    JsonWriter w;
    w.object().field("num", std::to_string(value.num)).field("den", std::to_string(value.den));
    return w.end().str();
}

std::string anchor_json(const TimelineAnchor& anchor) {
    JsonWriter w;
    w.object().field_raw("original", rational_json(anchor.original))
        .field_raw("exported", rational_json(anchor.exported));
    return w.end().str();
}

std::string raw_clock_json(const RawClockRecord& clock) {
    JsonWriter w;
    w.object()
        .field("id", clock.id)
        .field("source_id", clock.source_id)
        .field("stream_id", clock.stream_id)
        .field("clock_domain", clock.clock_domain)
        .field("timezone", clock.timezone)
        .field_raw("assumptions", [&] {
            std::string out;
            append_array(out, clock.assumptions);
            return out;
        }())
        .field_raw("raw_fields", [&] {
            std::string out;
            append_array(out, clock.raw_fields);
            return out;
        }())
        .field("timing_availability", availability_name(clock.timing_availability))
        .field("revision", clock.revision);
    return w.end().str();
}

std::string mapping_json(const SourceExportMapping& mapping) {
    std::string anchors = "[";
    for (std::size_t i = 0; i < mapping.anchors.size(); ++i) {
        if (i) anchors += ',';
        anchors += anchor_json(mapping.anchors[i]);
    }
    anchors += ']';
    JsonWriter w;
    w.object()
        .field("id", mapping.id)
        .field("original_source_id", mapping.original_source_id)
        .field("export_source_id", mapping.export_source_id)
        .field("status", mapping_status_name(mapping.status))
        .field_raw("anchors", anchors)
        .field_raw("support_start", rational_json(mapping.support_start))
        .field_raw("support_end", rational_json(mapping.support_end))
        .field("crosses_reset", mapping.crosses_reset)
        .field("revision", mapping.revision);
    return w.end().str();
}

std::string timing_estimate_json(const TimingEstimate& estimate) {
    std::string refs;
    append_array(refs, estimate.clock_refs);
    std::string evidence;
    append_array(evidence, estimate.evidence_refs);
    JsonWriter w;
    w.object()
        .field("id", estimate.id)
        .field_raw("clock_refs", refs)
        .field("mapping_id", estimate.mapping_id)
        .field("mapping_revision", estimate.mapping_revision)
        .field("offset", estimate.offset)
        .field("rate", estimate.rate)
        .field("rate_estimated", estimate.rate_estimated)
        .field("evidence_class", evidence_name(estimate.evidence_class))
        .field("timing_availability", availability_name(estimate.timing_availability))
        .field_raw("evidence_refs", evidence)
        .field("residual", estimate.residual)
        .field("uncertainty", estimate.uncertainty)
        .field("support_start", estimate.support_start)
        .field("support_end", estimate.support_end)
        .field("tolerance", estimate.tolerance)
        .field("crosses_reset", estimate.crosses_reset)
        .field("revision", estimate.revision);
    return w.end().str();
}

std::string decision_json(const SynchronizationDecision& decision) {
    std::string members;
    append_array(members, decision.members);
    std::string evidence;
    append_array(evidence, decision.evidence_refs);
    std::string consumers;
    append_array(consumers, decision.consumers);
    JsonWriter w;
    w.object()
        .field("id", decision.id)
        .field("purpose", purpose_name(decision.purpose))
        .field_raw("members", members)
        .field("mapping_revision", decision.mapping_revision)
        .field("residual", decision.residual)
        .field("uncertainty", decision.uncertainty)
        .field("support_start", decision.support_start)
        .field("support_end", decision.support_end)
        .field("tolerance", decision.tolerance)
        .field("evidence_class", evidence_name(decision.evidence_class))
        .field("timing_availability", availability_name(decision.timing_availability))
        .field_raw("evidence_refs", evidence)
        .field("status", decision_status_name(decision.status))
        .field("reason", decision.reason)
        .field_raw("consumers", consumers)
        .field("crosses_reset", decision.crosses_reset)
        .field("revision", decision.revision);
    return w.end().str();
}

std::string artifact_json(const ArtifactReference& artifact) {
    JsonWriter w;
    w.object();
    w.field("artifact", artifact.artifact);
    w.field("role", artifact.role);
    w.field("transform", artifact.transform);
    return w.end().str();
}

std::uint64_t integer_value(const JsonValue& value, const char* field) {
    if (value.type == JsonValue::Type::String && !value.str.empty()) {
        std::uint64_t parsed = 0;
        const char* begin = value.str.data();
        const char* end = begin + value.str.size();
        const auto result = std::from_chars(begin, end, parsed);
        if (result.ec == std::errc{} && result.ptr == end) return parsed;
    } else if (value.type == JsonValue::Type::Number &&
               std::isfinite(value.num) && value.num >= 0.0 &&
               std::floor(value.num) == value.num &&
               value.num <= 9007199254740992.0) {
        return static_cast<std::uint64_t>(value.num);
    }
    throw std::runtime_error(std::string("invalid integer field ") + field);
}

std::int64_t signed_integer_value(const JsonValue& value, const char* field) {
    if (value.type != JsonValue::Type::Number || !std::isfinite(value.num) ||
        std::floor(value.num) != value.num ||
        value.num < -9007199254740992.0 || value.num > 9007199254740992.0)
        throw std::runtime_error(std::string("invalid integer field ") + field);
    return static_cast<std::int64_t>(value.num);
}

const JsonValue& required_field(const JsonValue& object, const char* key) {
    const JsonValue* value = object.find(key);
    if (!value) throw std::runtime_error(std::string("missing field ") + key);
    std::size_t count = 0;
    for (const auto& entry : object.obj)
        if (entry.first == key) ++count;
    if (count != 1) throw std::runtime_error(std::string("duplicate field ") + key);
    return *value;
}

void reject_unknown_fields(const JsonValue& object,
                           std::initializer_list<const char*> allowed) {
    std::set<std::string> names;
    for (const char* name : allowed) names.emplace(name);
    for (const auto& entry : object.obj)
        if (!names.count(entry.first))
            throw std::runtime_error("unknown field " + entry.first);
}

std::string string_field(const JsonValue& object, const char* key,
                         bool allow_empty = true) {
    const JsonValue& value = required_field(object, key);
    if (value.type != JsonValue::Type::String || !valid_text(value.str, allow_empty))
        throw std::runtime_error(std::string("invalid string field ") + key);
    return value.str;
}

std::vector<std::string> string_array_field(const JsonValue& object, const char* key) {
    const JsonValue& value = required_field(object, key);
    if (!value.is_array()) throw std::runtime_error(std::string("invalid array field ") + key);
    std::vector<std::string> out;
    out.reserve(value.arr.size());
    for (const JsonValue& item : value.arr) {
        if (item.type != JsonValue::Type::String || !valid_text(item.str, false))
            throw std::runtime_error(std::string("invalid entry in ") + key);
        out.push_back(item.str);
    }
    return out;
}

std::int64_t signed_integer_text(const JsonValue& value, const char* field) {
    if (value.type != JsonValue::Type::String || !valid_text(value.str, false))
        throw std::runtime_error(std::string("invalid integer field ") + field);
    std::int64_t out = 0;
    const char* begin = value.str.data();
    const char* end = begin + value.str.size();
    const auto result = std::from_chars(begin, end, out);
    if (result.ec != std::errc() || result.ptr != end)
        throw std::runtime_error(std::string("invalid integer field ") + field);
    return out;
}

std::int64_t signed_integer_field(const JsonValue& value, const char* field) {
    if (value.type == JsonValue::Type::String)
        return signed_integer_text(value, field);
    return signed_integer_value(value, field);
}

double real_field(const JsonValue& object, const char* key) {
    const JsonValue& value = required_field(object, key);
    if (value.type != JsonValue::Type::Number || !std::isfinite(value.num))
        throw std::runtime_error(std::string("invalid real field ") + key);
    return value.num;
}

bool bool_field(const JsonValue& object, const char* key) {
    const JsonValue& value = required_field(object, key);
    if (value.type != JsonValue::Type::Bool)
        throw std::runtime_error(std::string("invalid boolean field ") + key);
    return value.b;
}

RationalTime parse_rational(const JsonValue& object, const char* field) {
    if (!object.is_object()) throw std::runtime_error(std::string(field) + " is not an object");
    reject_unknown_fields(object, {"num", "den"});
    RationalTime out;
    out.num = signed_integer_text(required_field(object, "num"), "rational num");
    out.den = signed_integer_text(required_field(object, "den"), "rational den");
    if (out.den <= 0) throw std::runtime_error(std::string(field) + " denominator is not positive");
    return out;
}

TimelineAnchor parse_anchor(const JsonValue& object) {
    if (!object.is_object()) throw std::runtime_error("timeline anchor is not an object");
    reject_unknown_fields(object, {"original", "exported"});
    TimelineAnchor out;
    out.original = parse_rational(required_field(object, "original"), "anchor original");
    out.exported = parse_rational(required_field(object, "exported"), "anchor exported");
    return out;
}

EvidenceClass parse_evidence(const JsonValue& object, const char* key) {
    const std::string value = string_field(object, key, false);
    if (value == "unknown") return EvidenceClass::Unknown;
    if (value == "metadata_only") return EvidenceClass::MetadataOnly;
    if (value == "ambiguous") return EvidenceClass::Ambiguous;
    if (value == "independent") return EvidenceClass::Independent;
    if (value == "raw_observation") return EvidenceClass::RawObservation;
    throw std::runtime_error(std::string("invalid evidence class ") + value);
}

TimingAvailability parse_availability(const JsonValue& object, const char* key) {
    const std::string value = string_field(object, key, false);
    if (value == "unknown") return TimingAvailability::Unknown;
    if (value == "unavailable") return TimingAvailability::Unavailable;
    if (value == "metadata_only") return TimingAvailability::MetadataOnly;
    if (value == "estimated") return TimingAvailability::Estimated;
    if (value == "supported") return TimingAvailability::Supported;
    throw std::runtime_error(std::string("invalid timing availability ") + value);
}

SourceExportStatus parse_mapping_status(const JsonValue& object) {
    const std::string value = string_field(object, "status", false);
    if (value == "exact") return SourceExportStatus::Exact;
    if (value == "derived") return SourceExportStatus::Derived;
    if (value == "unsupported") return SourceExportStatus::Unsupported;
    throw std::runtime_error(std::string("invalid source-export mapping status ") + value);
}

SynchronizationPurpose parse_purpose(const JsonValue& object) {
    const std::string value = string_field(object, "purpose", false);
    if (value == "static_overlap") return SynchronizationPurpose::StaticOverlap;
    if (value == "moving_rig") return SynchronizationPurpose::MovingRig;
    if (value == "rgbd_pose") return SynchronizationPurpose::RgbdPose;
    throw std::runtime_error(std::string("invalid synchronization purpose ") + value);
}

DecisionStatus parse_decision_status(const JsonValue& object) {
    const std::string value = string_field(object, "status", false);
    if (value == "accepted") return DecisionStatus::Accepted;
    if (value == "review") return DecisionStatus::Review;
    if (value == "refused") return DecisionStatus::Refused;
    throw std::runtime_error(std::string("invalid synchronization decision status ") + value);
}

RawClockRecord parse_raw_clock(const JsonValue& object) {
    if (!object.is_object()) throw std::runtime_error("raw clock is not an object");
    reject_unknown_fields(object, {"id", "source_id", "stream_id", "clock_domain",
                                   "timezone", "assumptions", "raw_fields",
                                   "timing_availability", "revision"});
    RawClockRecord out;
    out.id = string_field(object, "id", false);
    out.source_id = string_field(object, "source_id", false);
    out.stream_id = string_field(object, "stream_id", false);
    out.clock_domain = string_field(object, "clock_domain", false);
    out.timezone = string_field(object, "timezone");
    out.assumptions = string_array_field(object, "assumptions");
    out.raw_fields = string_array_field(object, "raw_fields");
    out.timing_availability = parse_availability(object, "timing_availability");
    out.revision = string_field(object, "revision", false);
    return out;
}

SourceExportMapping parse_mapping(const JsonValue& object) {
    if (!object.is_object()) throw std::runtime_error("source-export mapping is not an object");
    reject_unknown_fields(object, {"id", "original_source_id", "export_source_id",
                                   "status", "anchors", "support_start", "support_end",
                                   "crosses_reset", "revision"});
    SourceExportMapping out;
    out.id = string_field(object, "id", false);
    out.original_source_id = string_field(object, "original_source_id", false);
    out.export_source_id = string_field(object, "export_source_id", false);
    out.status = parse_mapping_status(object);
    const JsonValue& anchors = required_field(object, "anchors");
    if (!anchors.is_array()) throw std::runtime_error("mapping anchors is not an array");
    out.anchors.reserve(anchors.arr.size());
    for (const JsonValue& anchor : anchors.arr) out.anchors.push_back(parse_anchor(anchor));
    out.support_start = parse_rational(required_field(object, "support_start"), "mapping support");
    out.support_end = parse_rational(required_field(object, "support_end"), "mapping support");
    out.crosses_reset = bool_field(object, "crosses_reset");
    out.revision = string_field(object, "revision", false);
    return out;
}

TimingEstimate parse_timing_estimate(const JsonValue& object) {
    if (!object.is_object()) throw std::runtime_error("timing estimate is not an object");
    reject_unknown_fields(object, {"id", "clock_refs", "mapping_id", "mapping_revision",
                                   "offset", "rate", "rate_estimated", "evidence_class",
                                   "timing_availability", "evidence_refs", "residual",
                                   "uncertainty", "support_start", "support_end", "tolerance",
                                   "crosses_reset", "revision"});
    TimingEstimate out;
    out.id = string_field(object, "id", false);
    out.clock_refs = string_array_field(object, "clock_refs");
    out.mapping_id = string_field(object, "mapping_id");
    out.mapping_revision = string_field(object, "mapping_revision");
    out.offset = real_field(object, "offset");
    out.rate = real_field(object, "rate");
    out.rate_estimated = bool_field(object, "rate_estimated");
    out.evidence_class = parse_evidence(object, "evidence_class");
    out.timing_availability = parse_availability(object, "timing_availability");
    out.evidence_refs = string_array_field(object, "evidence_refs");
    out.residual = real_field(object, "residual");
    out.uncertainty = real_field(object, "uncertainty");
    out.support_start = real_field(object, "support_start");
    out.support_end = real_field(object, "support_end");
    out.tolerance = real_field(object, "tolerance");
    out.crosses_reset = bool_field(object, "crosses_reset");
    out.revision = string_field(object, "revision", false);
    return out;
}

SynchronizationDecision parse_decision(const JsonValue& object) {
    if (!object.is_object()) throw std::runtime_error("synchronization decision is not an object");
    reject_unknown_fields(object, {"id", "purpose", "members", "mapping_revision",
                                   "residual", "uncertainty", "support_start", "support_end",
                                   "tolerance", "evidence_class", "timing_availability",
                                   "evidence_refs", "status", "reason", "consumers",
                                   "crosses_reset", "revision"});
    SynchronizationDecision out;
    out.id = string_field(object, "id", false);
    out.purpose = parse_purpose(object);
    out.members = string_array_field(object, "members");
    out.mapping_revision = string_field(object, "mapping_revision");
    out.residual = real_field(object, "residual");
    out.uncertainty = real_field(object, "uncertainty");
    out.support_start = real_field(object, "support_start");
    out.support_end = real_field(object, "support_end");
    out.tolerance = real_field(object, "tolerance");
    out.evidence_class = parse_evidence(object, "evidence_class");
    out.timing_availability = parse_availability(object, "timing_availability");
    out.evidence_refs = string_array_field(object, "evidence_refs");
    out.status = parse_decision_status(object);
    out.reason = string_field(object, "reason");
    out.consumers = string_array_field(object, "consumers");
    out.crosses_reset = bool_field(object, "crosses_reset");
    out.revision = string_field(object, "revision", false);
    return out;
}

SourceStream parse_stream(const JsonValue& object) {
    if (!object.is_object()) throw std::runtime_error("stream is not an object");
    reject_unknown_fields(object, {"stream_id", "media_type", "codec",
                                   "time_base_num", "time_base_den"});
    SourceStream stream;
    stream.stream_id = string_field(object, "stream_id", false);
    stream.media_type = string_field(object, "media_type", false);
    stream.codec = string_field(object, "codec");
    stream.time_base_num = signed_integer_field(
        required_field(object, "time_base_num"), "time_base_num");
    stream.time_base_den = signed_integer_field(
        required_field(object, "time_base_den"), "time_base_den");
    return stream;
}

OriginalMetadata parse_original_metadata(const JsonValue& object) {
    if (!object.is_object()) throw std::runtime_error("original metadata is not an object");
    reject_unknown_fields(object, {"media_type", "container", "width", "height",
                                   "duration_num", "duration_den"});
    OriginalMetadata metadata;
    metadata.media_type = string_field(object, "media_type", false);
    metadata.container = string_field(object, "container");
    metadata.width = integer_value(required_field(object, "width"), "width");
    metadata.height = integer_value(required_field(object, "height"), "height");
    metadata.duration_num =
        integer_value(required_field(object, "duration_num"), "duration_num");
    metadata.duration_den =
        integer_value(required_field(object, "duration_den"), "duration_den");
    return metadata;
}

SourceRecord parse_source(const JsonValue& object) {
    if (!object.is_object()) throw std::runtime_error("source is not an object");
    reject_unknown_fields(object, {"source_id", "digest_algorithm", "relation",
                                   "original_source_id", "locators", "size_bytes",
                                   "streams", "original_metadata",
                                   "timing_features_supported",
                                   "timing_features_unsupported"});
    SourceRecord source;
    source.source_id = string_field(object, "source_id", false);
    source.digest_algorithm = string_field(object, "digest_algorithm", false);
    const std::string relation = string_field(object, "relation", false);
    if (relation == "original") {
        source.relation = SourceRelation::Original;
    } else if (relation == "export") {
        source.relation = SourceRelation::Export;
    } else {
        throw std::runtime_error("invalid source relation");
    }
    source.original_source_id = string_field(object, "original_source_id");

    const JsonValue& locators = required_field(object, "locators");
    if (!locators.is_array() || locators.arr.empty())
        throw std::runtime_error("source locators must be a non-empty array");
    for (const JsonValue& locator : locators.arr) {
        if (locator.type != JsonValue::Type::String || locator.str.empty() ||
            locator.str.find('\0') != std::string::npos)
            throw std::runtime_error("invalid source locator");
        source.locators.emplace_back(fs::u8path(locator.str));
    }
    source.size_bytes = integer_value(required_field(object, "size_bytes"), "size_bytes");

    const JsonValue& streams = required_field(object, "streams");
    if (!streams.is_array()) throw std::runtime_error("source streams is not an array");
    source.streams.reserve(streams.arr.size());
    for (const JsonValue& stream : streams.arr) source.streams.push_back(parse_stream(stream));

    const JsonValue& metadata = required_field(object, "original_metadata");
    if (!metadata.is_array()) throw std::runtime_error("original metadata is not an array");
    source.original_metadata.reserve(metadata.arr.size());
    for (const JsonValue& value : metadata.arr)
        source.original_metadata.push_back(parse_original_metadata(value));
    source.timing_features_supported =
        string_array_field(object, "timing_features_supported");
    source.timing_features_unsupported =
        string_array_field(object, "timing_features_unsupported");
    validate_source_record(source);
    return source;
}

std::string read_exact(const fs::path& path, std::uintmax_t max_bytes) {
    const FileSnapshot before = snapshot(path);
    if (before.size > max_bytes)
        throw std::runtime_error(path.u8string() + " is too large");
    std::string text(static_cast<std::size_t>(before.size), '\0');
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.u8string());
    if (!text.empty()) {
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (in.gcount() != static_cast<std::streamsize>(text.size()) || in.bad())
            throw std::runtime_error("failed while reading " + path.u8string());
    }
    const FileSnapshot after = snapshot(path);
    if (!same_snapshot(before, after))
        throw std::runtime_error(path.u8string() + " changed while reading");
    return text;
}

void sync_file(const fs::path& path) {
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw std::runtime_error("cannot open " + path.u8string() + " for sync");
    const BOOL ok = FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!ok) throw std::runtime_error("cannot flush " + path.u8string());
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) throw std::runtime_error("cannot open " + path.u8string() + " for sync");
    const int result = ::fsync(fd);
    ::close(fd);
    if (result != 0) throw std::runtime_error("cannot flush " + path.u8string());
#endif
}

void sync_directory(const fs::path& path) {
#ifndef _WIN32
    const fs::path dir = path.parent_path().empty() ? fs::path(".") : path.parent_path();
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) throw std::runtime_error("cannot open " + dir.u8string() + " for sync");
    const int result = ::fsync(fd);
    ::close(fd);
    if (result != 0) throw std::runtime_error("cannot flush " + dir.u8string());
#else
    (void)path;
#endif
}

void write_synced(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write " + path.u8string());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    if (!out) throw std::runtime_error("failed while writing " + path.u8string());
    out.close();
    if (out.fail()) throw std::runtime_error("failed while closing " + path.u8string());
    sync_file(path);
}

std::atomic<std::uint64_t> g_temp_counter{0};

fs::path temporary_path(const fs::path& target) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto n = g_temp_counter.fetch_add(1, std::memory_order_relaxed);
    return target.parent_path() /
           ("." + target.filename().u8string() + ".tmp-" +
            std::to_string(static_cast<unsigned long long>(now)) + "-" +
            std::to_string(static_cast<unsigned long long>(n)));
}

void remove_quietly(const fs::path& path) {
    std::error_code ec;
    fs::remove(path, ec);
}

bool exists_file(const fs::path& path) {
    std::error_code ec;
    const bool result = fs::exists(path, ec);
    if (ec) throw std::runtime_error("cannot inspect " + path.u8string() + ": " + ec.message());
    return result;
}

void install_new_file(const fs::path& temporary, const fs::path& target) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.wstring().c_str(), target.wstring().c_str(),
                     MOVEFILE_WRITE_THROUGH)) {
        if (!exists_file(target))
            throw std::runtime_error("cannot publish " + target.u8string());
        throw std::runtime_error("revision already exists: " + target.u8string());
    }
#else
    if (::link(temporary.c_str(), target.c_str()) == 0) {
        if (::unlink(temporary.c_str()) != 0)
            throw std::runtime_error("cannot finish publishing " + target.u8string());
    } else if (errno == EEXIST) {
        throw std::runtime_error("revision already exists: " + target.u8string());
    } else {
        throw std::runtime_error("cannot publish " + target.u8string() +
                                 " without no-replace support");
    }
#endif
    sync_directory(target);
}


void replace_file(const fs::path& temporary, const fs::path& target) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.wstring().c_str(), target.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("cannot replace " + target.u8string());
#else
    std::error_code ec;
    fs::rename(temporary, target, ec);
    if (ec) throw std::runtime_error("cannot replace " + target.u8string() + ": " + ec.message());
#endif
    sync_directory(target);
}

void ensure_directory(const fs::path& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec) throw std::runtime_error("cannot create " + path.u8string() + ": " + ec.message());
}

fs::path canonical_directory(const fs::path& path, const char* field) {
    std::error_code ec;
    const fs::path result = fs::canonical(path, ec);
    if (ec) {
        throw std::runtime_error(std::string("cannot canonicalize ") + field + ": " +
                                 ec.message());
    }
    if (!fs::is_directory(result, ec) || ec) {
        throw std::runtime_error(std::string(field) + " is not a directory");
    }
    return result;
}

std::string path_key(const fs::path& path) {
    std::string key = path.generic_u8string();
#ifdef _WIN32
    for (char& c : key)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
#endif
    return key;
}

bool path_within(const fs::path& root, const fs::path& child) {
    auto root_it = root.begin();
    auto child_it = child.begin();
    for (; root_it != root.end(); ++root_it, ++child_it) {
        if (child_it == child.end() || path_key(*root_it) != path_key(*child_it))
            return false;
    }
    return true;
}

fs::path canonical_existing(const fs::path& path, const char* field) {
    std::error_code ec;
    const fs::path result = fs::canonical(path, ec);
    if (ec) {
        throw std::runtime_error(std::string("cannot read ") + field + ": " + ec.message());
    }
    return result;
}

std::string normalized_artifact_name(const std::string& value) {
    if (!valid_text(value, false))
        throw std::runtime_error("invalid artifact reference");
    const fs::path path = fs::u8path(value);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
        throw std::runtime_error("artifact reference must be project-relative");
    for (const fs::path& component : path) {
        if (component == fs::path(".."))
            throw std::runtime_error("artifact reference escapes project root");
    }
    const fs::path normalized = path.lexically_normal();
    if (normalized.empty() || normalized == fs::path(".") ||
        normalized == fs::path("..") || normalized.is_absolute())
        throw std::runtime_error("invalid artifact reference");
    const std::string result = normalized.generic_u8string();
    if (result != value)
        throw std::runtime_error("artifact reference is not normalized");
    return result;
}

bool reserved_project_path(const fs::path& root, const fs::path& path) {
    const fs::path relative = path.lexically_relative(root);
    if (relative.empty() || relative.is_absolute()) return false;
    const auto first = relative.begin();
    if (first == relative.end()) return false;
    const std::string key = path_key(*first);
    return key == "current" || key == "revisions";
}
bool is_current_pointer_target(const fs::path& root, const fs::path& target) {
    const fs::path current = root / "current";
    if (path_key(target) == path_key(current)) return true;
    std::error_code ec;
    return fs::equivalent(target, current, ec) && !ec;
}


void validate_publish_artifacts(const fs::path& project_dir,
                                const ProjectRevision& revision) {
    if (revision.artifacts.empty()) return;
    const fs::path root = canonical_directory(project_dir, "project root");
    std::set<std::string> names;
    std::set<std::string> targets;
    for (const ArtifactReference& artifact : revision.artifacts) {
        const std::string name = normalized_artifact_name(artifact.artifact);
        if (!names.emplace(path_key(fs::u8path(name))).second)
            throw std::runtime_error("duplicate normalized artifact reference");
        const fs::path target = canonical_existing(root / fs::u8path(name), "artifact");
        if (!path_within(root, target))
            throw std::runtime_error("artifact reference escapes project root");
        if (is_current_pointer_target(root, target))
            throw std::runtime_error("current pointer cannot be an artifact");
        if (!targets.emplace(path_key(target)).second)
            throw std::runtime_error("duplicate normalized artifact reference");
    }
}


}  // namespace

SourceRecord make_source_record(const fs::path& locator) {
    if (locator.empty()) throw std::runtime_error("source locator is empty");
    const HashedFile hashed = hash_file(locator);
    SourceRecord source;
    source.source_id = "sha256:" + hashed.digest;
    source.locators.push_back(locator);
    source.size_bytes = hashed.size;
    validate_source_record(source);
    return source;
}

void validate_source_record(const SourceRecord& source) {
    if (source.digest_algorithm != "sha256")
        throw std::runtime_error("unsupported source digest algorithm");
    if (!valid_source_id(source.source_id))
        throw std::runtime_error("malformed source digest");
    if (source.relation == SourceRelation::Original) {
        if (!source.original_source_id.empty())
            throw std::runtime_error("original source cannot name an original source");
    } else if (source.relation == SourceRelation::Export) {
        if (!valid_source_id(source.original_source_id))
            throw std::runtime_error("export source has no valid original source");
    } else {
        throw std::runtime_error("invalid source relation");
    }
    if (source.locators.empty()) throw std::runtime_error("source has no locators");
    std::set<std::string> locators;
    for (const fs::path& locator : source.locators) {
        if (locator.empty() || locator.u8string().find('\0') != std::string::npos)
            throw std::runtime_error("invalid source locator");
        if (!locators.emplace(locator.lexically_normal().u8string()).second)
            throw std::runtime_error("duplicate source locator");
    }
    std::set<std::string> streams;
    for (const SourceStream& stream : source.streams) {
        require_text(stream.stream_id, "stream id", false);
        require_text(stream.media_type, "stream media type", false);
        require_text(stream.codec, "stream codec");
        if (stream.time_base_den <= 0)
            throw std::runtime_error("stream time base denominator must be positive");
        if (!streams.emplace(stream.stream_id).second)
            throw std::runtime_error("duplicate source stream");
    }
    for (const OriginalMetadata& metadata : source.original_metadata) {
        require_text(metadata.media_type, "original media type", false);
        require_text(metadata.container, "original container");
        if (metadata.duration_den == 0)
            throw std::runtime_error("original duration denominator must be positive");
    }
    std::set<std::string> supported;
    for (const std::string& feature : source.timing_features_supported) {
        require_text(feature, "supported timing feature", false);
        if (!supported.emplace(feature).second)
            throw std::runtime_error("duplicate supported timing feature");
    }
    for (const std::string& feature : source.timing_features_unsupported) {
        require_text(feature, "unsupported timing feature", false);
        if (supported.count(feature))
            throw std::runtime_error("timing feature is both supported and unsupported");
    }
}

namespace {

bool valid_enum(EvidenceClass value) {
    return value == EvidenceClass::Unknown || value == EvidenceClass::MetadataOnly ||
           value == EvidenceClass::Ambiguous || value == EvidenceClass::Independent ||
           value == EvidenceClass::RawObservation;
}

bool valid_enum(TimingAvailability value) {
    return value == TimingAvailability::Unknown || value == TimingAvailability::Unavailable ||
           value == TimingAvailability::MetadataOnly || value == TimingAvailability::Estimated ||
           value == TimingAvailability::Supported;
}

bool valid_enum(SourceExportStatus value) {
    return value == SourceExportStatus::Exact || value == SourceExportStatus::Derived ||
           value == SourceExportStatus::Unsupported;
}

bool valid_enum(SynchronizationPurpose value) {
    return value == SynchronizationPurpose::StaticOverlap ||
           value == SynchronizationPurpose::MovingRig ||
           value == SynchronizationPurpose::RgbdPose;
}

bool valid_enum(DecisionStatus value) {
    return value == DecisionStatus::Accepted || value == DecisionStatus::Review ||
           value == DecisionStatus::Refused;
}

void validate_rational(const RationalTime& value, const char* field) {
    if (value.den <= 0) throw std::runtime_error(std::string(field) + " denominator is not positive");
}

struct U128 {
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
};

U128 multiply_u64(std::uint64_t a, std::uint64_t b) {
    constexpr std::uint64_t kMask = 0xffffffffu;
    const std::uint64_t a0 = a & kMask;
    const std::uint64_t a1 = a >> 32;
    const std::uint64_t b0 = b & kMask;
    const std::uint64_t b1 = b >> 32;
    const std::uint64_t p0 = a0 * b0;
    const std::uint64_t p1 = a0 * b1;
    const std::uint64_t p2 = a1 * b0;
    const std::uint64_t p3 = a1 * b1;
    const std::uint64_t middle = p1 + p2;
    const std::uint64_t middle_carry = middle < p1 ? 1 : 0;
    const std::uint64_t lo_add = middle << 32;
    const std::uint64_t lo = p0 + lo_add;
    const std::uint64_t carry = lo < p0 ? 1 : 0;
    return {p3 + (middle >> 32) + (middle_carry << 32) + carry, lo};
}

std::uint64_t magnitude(std::int64_t value) {
    return value >= 0 ? static_cast<std::uint64_t>(value)
                      : static_cast<std::uint64_t>(-(value + 1)) + 1;
}
int compare_positive_rationals(std::uint64_t an, std::uint64_t ad,
                               std::uint64_t bn, std::uint64_t bd) {
    const U128 left = multiply_u64(an, bd);
    const U128 right = multiply_u64(bn, ad);
    if (left.hi != right.hi) return left.hi < right.hi ? -1 : 1;
    if (left.lo != right.lo) return left.lo < right.lo ? -1 : 1;
    return 0;
}

int compare_rational(const RationalTime& a, const RationalTime& b) {
    const bool a_negative = a.num < 0;
    const bool b_negative = b.num < 0;
    if (a_negative != b_negative) return a_negative ? -1 : 1;
    const int magnitude_order =
        compare_positive_rationals(magnitude(a.num), static_cast<std::uint64_t>(a.den),
                                   magnitude(b.num), static_cast<std::uint64_t>(b.den));
    return a_negative ? -magnitude_order : magnitude_order;
}

void validate_interval(double start, double end, const char* field) {
    if (!std::isfinite(start) || !std::isfinite(end) || start > end)
        throw std::runtime_error(std::string("invalid ") + field + " interval");
}

void validate_unique_refs(const std::vector<std::string>& refs, const char* field,
                          bool require_nonempty) {
    if (require_nonempty && refs.empty())
        throw std::runtime_error(std::string(field) + " is empty");
    std::set<std::string> seen;
    for (const std::string& ref : refs) {
        require_text(ref, field, false);
        if (!seen.emplace(ref).second)
            throw std::runtime_error(std::string("duplicate ") + field);
    }
}

}  // namespace

void validate_raw_clock(const RawClockRecord& clock) {
    require_text(clock.id, "raw clock id", false);
    require_text(clock.source_id, "raw clock source", false);
    require_text(clock.stream_id, "raw clock stream", false);
    require_text(clock.clock_domain, "raw clock domain", false);
    require_text(clock.timezone, "raw clock timezone", true);
    require_text(clock.revision, "raw clock revision", false);
    validate_unique_refs(clock.assumptions, "raw clock assumption", false);
    validate_unique_refs(clock.raw_fields, "raw clock field", false);
    if (!valid_enum(clock.timing_availability))
        throw std::runtime_error("invalid raw clock timing availability");
}

void validate_source_export_mapping(const SourceExportMapping& mapping) {
    require_text(mapping.id, "source-export mapping id", false);
    require_text(mapping.original_source_id, "mapping original source", false);
    require_text(mapping.export_source_id, "mapping export source", false);
    require_text(mapping.revision, "source-export mapping revision", false);
    if (!valid_source_id(mapping.original_source_id) ||
        !valid_source_id(mapping.export_source_id))
        throw std::runtime_error("source-export mapping has malformed source identity");
    if (!valid_enum(mapping.status)) throw std::runtime_error("invalid source-export mapping status");
    validate_rational(mapping.support_start, "mapping support start");
    validate_rational(mapping.support_end, "mapping support end");
    if (compare_rational(mapping.support_start, mapping.support_end) > 0)
        throw std::runtime_error("mapping support interval is reversed");
    if (mapping.status != SourceExportStatus::Unsupported && mapping.anchors.size() < 2)
        throw std::runtime_error("supported source-export mapping needs two anchors");
    bool first = true;
    RationalTime previous_original;
    RationalTime previous_exported;
    for (const TimelineAnchor& anchor : mapping.anchors) {
        validate_rational(anchor.original, "anchor original");
        validate_rational(anchor.exported, "anchor exported");
        if (!first &&
            (compare_rational(anchor.original, previous_original) <= 0 ||
             compare_rational(anchor.exported, previous_exported) <= 0))
            throw std::runtime_error("source-export anchors are not strictly ordered");
        previous_original = anchor.original;
        previous_exported = anchor.exported;
        first = false;
    }
}

void validate_timing_estimate(const TimingEstimate& estimate) {
    require_text(estimate.id, "timing estimate id", false);
    require_text(estimate.mapping_id, "timing estimate mapping id", true);
    require_text(estimate.mapping_revision, "timing estimate mapping revision", true);
    if (estimate.mapping_id.empty() != estimate.mapping_revision.empty())
        throw std::runtime_error("timing estimate mapping link is incomplete");
    require_text(estimate.revision, "timing estimate revision", false);
    validate_unique_refs(estimate.clock_refs, "timing clock reference", true);
    validate_unique_refs(estimate.evidence_refs, "timing evidence reference",
                         estimate.evidence_class == EvidenceClass::Independent ||
                             estimate.evidence_class == EvidenceClass::RawObservation);
    if (!std::isfinite(estimate.offset) || !std::isfinite(estimate.rate) ||
        !(estimate.rate > 0) || estimate.rate > 100.0)
        throw std::runtime_error("timing estimate has invalid offset or rate");
    if (!estimate.rate_estimated && estimate.rate != 1.0)
        throw std::runtime_error("unestimated timing rate must be exactly one");
    if (!std::isfinite(estimate.residual) || !std::isfinite(estimate.uncertainty) ||
        !std::isfinite(estimate.tolerance) || estimate.residual < 0 ||
        estimate.uncertainty < 0 || estimate.tolerance < 0)
        throw std::runtime_error("timing estimate has invalid error metrics");
    validate_interval(estimate.support_start, estimate.support_end, "timing estimate support");
    if (!valid_enum(estimate.evidence_class))
        throw std::runtime_error("invalid timing estimate evidence class");
    if (!valid_enum(estimate.timing_availability))
        throw std::runtime_error("invalid timing estimate timing availability");
    if (estimate.rate_estimated &&
        estimate.evidence_class != EvidenceClass::Independent &&
        estimate.evidence_class != EvidenceClass::RawObservation)
        throw std::runtime_error("estimated rate needs independent evidence");
}

void validate_synchronization_decision(const SynchronizationDecision& decision) {
    require_text(decision.id, "synchronization decision id", false);
    require_text(decision.mapping_revision, "synchronization mapping revision", true);
    require_text(decision.revision, "synchronization decision revision", false);
    validate_unique_refs(decision.members, "synchronization member", true);
    validate_unique_refs(decision.evidence_refs, "synchronization evidence reference",
                         decision.status == DecisionStatus::Accepted);
    validate_unique_refs(decision.consumers, "synchronization consumer", false);
    if (!valid_enum(decision.purpose)) throw std::runtime_error("invalid synchronization purpose");
    if (!valid_enum(decision.evidence_class))
        throw std::runtime_error("invalid synchronization evidence class");
    if (!valid_enum(decision.timing_availability))
        throw std::runtime_error("invalid synchronization timing availability");
    if (!valid_enum(decision.status)) throw std::runtime_error("invalid synchronization status");
    if (!std::isfinite(decision.residual) || !std::isfinite(decision.uncertainty) ||
        !std::isfinite(decision.tolerance) || decision.residual < 0 ||
        decision.uncertainty < 0 || decision.tolerance < 0)
        throw std::runtime_error("synchronization decision has invalid error metrics");
    validate_interval(decision.support_start, decision.support_end, "synchronization support");
    if (decision.status == DecisionStatus::Accepted) {
        if (decision.tolerance <= 0 || decision.residual > decision.tolerance ||
            decision.uncertainty > decision.tolerance)
            throw std::runtime_error("accepted synchronization exceeds tolerance");
        if (decision.evidence_class != EvidenceClass::Independent &&
            decision.evidence_class != EvidenceClass::RawObservation)
            throw std::runtime_error("accepted synchronization lacks independent evidence");
        if (decision.purpose == SynchronizationPurpose::MovingRig ||
            decision.purpose == SynchronizationPurpose::RgbdPose) {
            if (decision.timing_availability == TimingAvailability::Unknown ||
                decision.timing_availability == TimingAvailability::Unavailable ||
                decision.timing_availability == TimingAvailability::MetadataOnly)
                throw std::runtime_error("accepted synchronization lacks timing availability");
            if (decision.mapping_revision.empty())
                throw std::runtime_error("rig synchronization has no mapping revision");
        }
        if (decision.crosses_reset)
            throw std::runtime_error("accepted synchronization crosses a clock reset");
    }
}

bool decision_authorizes(const SynchronizationDecision& decision,
                         SynchronizationPurpose purpose,
                         const std::vector<std::string>& members,
                         double start,
                         double end,
                         std::string* reason) {
    auto refuse = [&](const char* why) {
        if (reason) *reason = why;
        return false;
    };
    try {
        validate_synchronization_decision(decision);
    } catch (const std::exception& error) {
        return refuse(error.what());
    }
    if (decision.status != DecisionStatus::Accepted) return refuse("decision is not accepted");
    if (decision.purpose != purpose) return refuse("decision purpose does not match");
    if (members.empty()) return refuse("no members requested");
    for (const std::string& member : members) {
        if (std::find(decision.members.begin(), decision.members.end(), member) ==
            decision.members.end())
            return refuse("requested member is not covered");
    }
    if (!std::isfinite(start) || !std::isfinite(end) || start > end)
        return refuse("requested interval is invalid");
    if (start < decision.support_start || end > decision.support_end)
        return refuse("requested interval is outside support");
    if (decision.residual > decision.tolerance || decision.uncertainty > decision.tolerance)
        return refuse("decision residual or uncertainty exceeds tolerance");
    if (decision.crosses_reset) return refuse("decision crosses a clock reset");
    if (decision.evidence_class == EvidenceClass::MetadataOnly ||
        decision.evidence_class == EvidenceClass::Ambiguous ||
        decision.evidence_class == EvidenceClass::Unknown)
        return refuse("decision evidence is not independent");
    if ((purpose == SynchronizationPurpose::MovingRig ||
         purpose == SynchronizationPurpose::RgbdPose) &&
        (decision.timing_availability == TimingAvailability::MetadataOnly ||
         decision.timing_availability == TimingAvailability::Unknown ||
         decision.timing_availability == TimingAvailability::Unavailable))
        return refuse("timing evidence is unavailable");
    if ((purpose == SynchronizationPurpose::MovingRig ||
         purpose == SynchronizationPurpose::RgbdPose) &&
        decision.mapping_revision.empty())
        return refuse("rig synchronization has no mapping revision");
    return true;
}

bool decision_authorizes(const SynchronizationDecision& decision,
                         const std::string& purpose,
                         const std::vector<std::string>& members,
                         double start,
                         double end,
                         std::string* reason) {
    if (purpose == "static_overlap")
        return decision_authorizes(decision, SynchronizationPurpose::StaticOverlap, members,
                                   start, end, reason);
    if (purpose == "moving_rig")
        return decision_authorizes(decision, SynchronizationPurpose::MovingRig, members,
                                   start, end, reason);
    if (purpose == "rgbd_pose")
        return decision_authorizes(decision, SynchronizationPurpose::RgbdPose, members,
                                   start, end, reason);
    if (reason) *reason = "unknown synchronization purpose";
    return false;
}

void verify_source_record(const SourceRecord& source) {
    validate_source_record(source);
    std::string last_error;
    for (const fs::path& locator : source.locators) {
        try {
            const HashedFile hashed = hash_file(locator);
            if (hashed.size == source.size_bytes &&
                "sha256:" + hashed.digest == source.source_id)
                return;
            last_error = locator.u8string() + " does not match source identity";
        } catch (const std::exception& error) {
            last_error = error.what();
        }
    }
    throw std::runtime_error(last_error.empty() ? "source does not match identity" : last_error);
}

SourceRecord relocate_source(SourceRecord source, const fs::path& locator) {
    return replace_source_locators(std::move(source), {locator});
}

SourceRecord replace_source_locators(SourceRecord source,
                                     std::vector<fs::path> locators) {
    source.locators = std::move(locators);
    validate_source_record(source);
    return source;
}

std::string stable_frame_id(const std::string& source_id,
                            const std::string& stream_id,
                            std::uint64_t original_presentation_ordinal) {
    if (!valid_source_id(source_id)) throw std::runtime_error("malformed source digest");
    require_text(stream_id, "stream id");
    std::string encoded;
    append_string_field(encoded, kFrameKeyDomain);
    append_string_field(encoded, source_id);
    append_string_field(encoded, stream_id);
    encoded.push_back(static_cast<char>(2));
    append_u64_be(encoded, original_presentation_ordinal);
    spirula::Sha256 sha;
    sha.update(reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size());
    return "sha256:" + sha.hex();
}

void validate_revision(const ProjectRevision& revision) {
    if (revision.schema != kProjectSchemaVersion)
        throw std::runtime_error("unsupported project revision schema");
    if (!valid_revision_name(revision.name))
        throw std::runtime_error("revision name is outside the portable scope");
    for (const SourceRecord& source : revision.sources) validate_source_record(source);

    std::set<std::string> source_ids;
    for (const SourceRecord& source : revision.sources) {
        if (!source_ids.emplace(source.source_id).second)
            throw std::runtime_error("duplicate source identity");
    }

    for (const SourceRecord& source : revision.sources) {
        if (source.relation != SourceRelation::Export) continue;
        const auto original = std::find_if(
            revision.sources.begin(), revision.sources.end(),
            [&](const SourceRecord& candidate) {
                return candidate.source_id == source.original_source_id;
            });
        if (original == revision.sources.end() ||
            original->relation != SourceRelation::Original)
            throw std::runtime_error("export source references a missing original source");
    }

    std::set<std::string> clock_ids;
    for (const RawClockRecord& clock : revision.raw_clocks) {
        validate_raw_clock(clock);
        if (!source_ids.count(clock.source_id))
            throw std::runtime_error("raw clock references an unknown source");
        if (!clock_ids.emplace(clock.id).second)
            throw std::runtime_error("duplicate raw clock identity");
    }

    std::set<std::string> mapping_ids;
    for (const SourceExportMapping& mapping : revision.source_export_mappings) {
        validate_source_export_mapping(mapping);
        const auto original = std::find_if(
            revision.sources.begin(), revision.sources.end(),
            [&](const SourceRecord& candidate) {
                return candidate.source_id == mapping.original_source_id;
            });
        const auto exported = std::find_if(
            revision.sources.begin(), revision.sources.end(),
            [&](const SourceRecord& candidate) {
                return candidate.source_id == mapping.export_source_id;
            });
        if (original == revision.sources.end() ||
            original->relation != SourceRelation::Original)
            throw std::runtime_error("mapping original source is not an original");
        if (exported == revision.sources.end() ||
            exported->relation != SourceRelation::Export)
            throw std::runtime_error("mapping export source is not an export");
        if (exported->original_source_id != mapping.original_source_id)
            throw std::runtime_error("mapping export lineage does not match original");

        if (!source_ids.count(mapping.original_source_id) ||
            !source_ids.count(mapping.export_source_id))
            throw std::runtime_error("source-export mapping references an unknown source");
        if (!mapping_ids.emplace(mapping.id).second)
            throw std::runtime_error("duplicate source-export mapping identity");
    }

    std::set<std::string> estimate_ids;
    for (const TimingEstimate& estimate : revision.timing_estimates) {
        validate_timing_estimate(estimate);
        for (const std::string& clock_ref : estimate.clock_refs)
            if (!clock_ids.count(clock_ref))
                throw std::runtime_error("timing estimate references an unknown clock");
        if (!estimate.mapping_id.empty()) {
            const auto mapping = std::find_if(
                revision.source_export_mappings.begin(), revision.source_export_mappings.end(),
                [&](const SourceExportMapping& candidate) {
                    return candidate.id == estimate.mapping_id;
                });
            if (mapping == revision.source_export_mappings.end())
                throw std::runtime_error("timing estimate references an unknown mapping");
            if (mapping->revision != estimate.mapping_revision)
                throw std::runtime_error("timing estimate mapping revision mismatch");
        }
        if (!estimate_ids.emplace(estimate.id).second)
            throw std::runtime_error("duplicate timing estimate identity");
    }

    std::set<std::string> decision_ids;
    for (const SynchronizationDecision& decision : revision.synchronization_decisions) {
        validate_synchronization_decision(decision);
        if (!decision_ids.emplace(decision.id).second)
            throw std::runtime_error("duplicate synchronization decision identity");
    }

    auto validate_refs = [](const std::vector<std::string>& refs, const char* field) {
        for (const std::string& ref : refs) require_text(ref, field, false);
    };
    validate_refs(revision.parents, "parent reference");
    validate_refs(revision.captures, "capture reference");
    validate_refs(revision.protected_regions, "protected-region reference");
    validate_refs(revision.operations, "operation reference");
    validate_refs(revision.validations, "validation reference");

    std::set<std::string> artifact_ids;
    for (const ArtifactReference& artifact : revision.artifacts) {
        require_text(artifact.artifact, "artifact reference", false);
        require_text(artifact.role, "artifact role", false);
        require_text(artifact.transform, "artifact transform");
        if (!artifact_ids.emplace(artifact.artifact).second)
            throw std::runtime_error("duplicate artifact reference");
    }
}

std::string serialize_revision(const ProjectRevision& revision) {
    validate_revision(revision);
    JsonWriter w;
    w.object();
    w.field("schema", static_cast<int>(revision.schema));
    w.field("name", revision.name);

    std::string sources;
    sources.push_back('[');
    for (std::size_t i = 0; i < revision.sources.size(); ++i) {
        if (i) sources.push_back(',');
        sources += source_json(revision.sources[i]);
    }
    sources.push_back(']');
    w.field_raw("sources", sources);
    std::string raw_clocks;
    raw_clocks.push_back('[');
    for (std::size_t i = 0; i < revision.raw_clocks.size(); ++i) {
        if (i) raw_clocks.push_back(',');
        raw_clocks += raw_clock_json(revision.raw_clocks[i]);
    }
    raw_clocks.push_back(']');
    w.field_raw("raw_clocks", raw_clocks);

    std::string mappings;
    mappings.push_back('[');
    for (std::size_t i = 0; i < revision.source_export_mappings.size(); ++i) {
        if (i) mappings.push_back(',');
        mappings += mapping_json(revision.source_export_mappings[i]);
    }
    mappings.push_back(']');
    w.field_raw("source_export_mappings", mappings);

    std::string estimates;
    estimates.push_back('[');
    for (std::size_t i = 0; i < revision.timing_estimates.size(); ++i) {
        if (i) estimates.push_back(',');
        estimates += timing_estimate_json(revision.timing_estimates[i]);
    }
    estimates.push_back(']');
    w.field_raw("timing_estimates", estimates);

    std::string decisions;
    decisions.push_back('[');
    for (std::size_t i = 0; i < revision.synchronization_decisions.size(); ++i) {
        if (i) decisions.push_back(',');
        decisions += decision_json(revision.synchronization_decisions[i]);
    }
    decisions.push_back(']');
    w.field_raw("synchronization_decisions", decisions);

    std::string parents;
    append_array(parents, revision.parents);
    w.field_raw("parents", parents);
    std::string captures;
    append_array(captures, revision.captures);
    w.field_raw("captures", captures);

    std::string artifacts;
    artifacts.push_back('[');
    for (std::size_t i = 0; i < revision.artifacts.size(); ++i) {
        if (i) artifacts.push_back(',');
        artifacts += artifact_json(revision.artifacts[i]);
    }
    artifacts.push_back(']');
    w.field_raw("artifacts", artifacts);

    std::string protected_regions;
    append_array(protected_regions, revision.protected_regions);
    w.field_raw("protected_regions", protected_regions);
    std::string operations;
    append_array(operations, revision.operations);
    w.field_raw("operations", operations);
    std::string validations;
    append_array(validations, revision.validations);
    w.field_raw("validations", validations);
    const std::string text = w.end().str();
    if (text.size() > kMaxRevisionBytes)
        throw std::runtime_error("project revision is too large");
    return text;

}

ProjectRevision parse_revision(const std::string& text) {
    if (text.size() > kMaxRevisionBytes)
        throw std::runtime_error("project revision is too large");
    const JsonValue root = json_parse(text);

    if (!root.is_object()) throw std::runtime_error("project revision is not an object");
    reject_unknown_fields(
        root, {"schema", "name", "sources", "raw_clocks", "source_export_mappings",
               "timing_estimates", "synchronization_decisions", "parents", "captures",
               "artifacts", "protected_regions", "operations", "validations"});
    ProjectRevision revision;
    const std::uint64_t schema = integer_value(required_field(root, "schema"), "schema");
    if (schema != kProjectSchemaVersion)
        throw std::runtime_error("unsupported project revision schema");
    revision.schema = static_cast<std::uint32_t>(schema);
    revision.name = string_field(root, "name", false);

    const JsonValue& sources = required_field(root, "sources");
    if (!sources.is_array()) throw std::runtime_error("sources is not an array");
    revision.sources.reserve(sources.arr.size());
    const JsonValue& raw_clocks = required_field(root, "raw_clocks");
    if (!raw_clocks.is_array()) throw std::runtime_error("raw_clocks is not an array");
    revision.raw_clocks.reserve(raw_clocks.arr.size());
    for (const JsonValue& value : raw_clocks.arr)
        revision.raw_clocks.push_back(parse_raw_clock(value));

    const JsonValue& mappings = required_field(root, "source_export_mappings");
    if (!mappings.is_array()) throw std::runtime_error("source_export_mappings is not an array");
    revision.source_export_mappings.reserve(mappings.arr.size());
    for (const JsonValue& value : mappings.arr)
        revision.source_export_mappings.push_back(parse_mapping(value));

    const JsonValue& estimates = required_field(root, "timing_estimates");
    if (!estimates.is_array()) throw std::runtime_error("timing_estimates is not an array");
    revision.timing_estimates.reserve(estimates.arr.size());
    for (const JsonValue& value : estimates.arr)
        revision.timing_estimates.push_back(parse_timing_estimate(value));

    const JsonValue& decisions = required_field(root, "synchronization_decisions");
    if (!decisions.is_array())
        throw std::runtime_error("synchronization_decisions is not an array");
    revision.synchronization_decisions.reserve(decisions.arr.size());
    for (const JsonValue& value : decisions.arr)
        revision.synchronization_decisions.push_back(parse_decision(value));
    for (const JsonValue& source : sources.arr) revision.sources.push_back(parse_source(source));

    revision.parents = string_array_field(root, "parents");
    revision.captures = string_array_field(root, "captures");

    const JsonValue& artifacts = required_field(root, "artifacts");
    if (!artifacts.is_array()) throw std::runtime_error("artifacts is not an array");
    revision.artifacts.reserve(artifacts.arr.size());
    for (const JsonValue& value : artifacts.arr) {
        if (!value.is_object()) throw std::runtime_error("artifact is not an object");
        reject_unknown_fields(value, {"artifact", "role", "transform"});
        ArtifactReference artifact;
        artifact.artifact = string_field(value, "artifact", false);
        artifact.role = string_field(value, "role", false);
        artifact.transform = string_field(value, "transform");
        revision.artifacts.push_back(std::move(artifact));
    }

    revision.protected_regions = string_array_field(root, "protected_regions");
    revision.operations = string_array_field(root, "operations");
    revision.validations = string_array_field(root, "validations");
    validate_revision(revision);
    return revision;
}

void publish_revision(const fs::path& project_dir, const ProjectRevision& revision) {
    validate_revision(revision);
    validate_publish_artifacts(project_dir, revision);
    for (const SourceRecord& source : revision.sources) verify_source_record(source);
    const std::string text = serialize_revision(revision);
    const fs::path revisions_dir = project_dir / "revisions";
    ensure_directory(revisions_dir);
    const fs::path target = revisions_dir / (revision.name + ".json");

    if (exists_file(target)) {
        if (read_exact(target, kMaxRevisionBytes) != text)
            throw std::runtime_error("conflicting immutable revision bytes: " + target.u8string());
    } else {
        const fs::path temporary = temporary_path(target);
        try {
            write_synced(temporary, text);
            if (exists_file(target)) {
                if (read_exact(target, kMaxRevisionBytes) != text)
                    throw std::runtime_error("conflicting immutable revision bytes: " + target.u8string());
                remove_quietly(temporary);
            } else {
                install_new_file(temporary, target);
            }
        } catch (...) {
            remove_quietly(temporary);
            throw;
        }
    }

    const fs::path current = project_dir / "current";
    const fs::path temporary = temporary_path(current);
    try {
        write_synced(temporary, revision.name + "\n");
        replace_file(temporary, current);
    } catch (...) {
        remove_quietly(temporary);
        throw;
    }
}

ProjectRevision read_revision(const fs::path& project_dir, const std::string& name) {
    if (!valid_revision_name(name))
        throw std::runtime_error("revision name is outside the portable scope");
    const fs::path path = project_dir / "revisions" / (name + ".json");
    ProjectRevision revision = parse_revision(read_exact(path, kMaxRevisionBytes));
    if (revision.name != name)
        throw std::runtime_error("revision file name does not match its contents");
    return revision;
}

ProjectRevision read_current_revision(const fs::path& project_dir) {
    std::string pointer = read_exact(project_dir / "current", kMaxPointerBytes);
    if (pointer.empty() || pointer.back() != '\n')
        throw std::runtime_error("invalid current revision pointer");
    pointer.pop_back();
    if (pointer.find('\n') != std::string::npos || pointer.find('\r') != std::string::npos ||
        !valid_revision_name(pointer))
        throw std::runtime_error("invalid current revision pointer");
    return read_revision(project_dir, pointer);
}

std::string revision_digest(const fs::path& project_dir, const std::string& name) {
    if (!valid_revision_name(name))
        throw std::runtime_error("revision name is outside the portable scope");
    const fs::path path = project_dir / "revisions" / (name + ".json");
    const std::string bytes = read_exact(path, kMaxRevisionBytes);
    const ProjectRevision revision = parse_revision(bytes);
    if (revision.name != name)
        throw std::runtime_error("revision file name does not match its contents");
    return digest_bytes(bytes);
}

ProjectRevision migrate_legacy_workspace(
    const fs::path& project_dir,
    const std::vector<fs::path>& source_roots,
    const std::vector<fs::path>& artifact_paths) {
    std::error_code ec;
    const bool has_current = fs::exists(project_dir / "current", ec);
    if (ec) throw std::runtime_error("cannot inspect project pointer: " + ec.message());
    const bool has_revisions = fs::exists(project_dir / "revisions", ec);
    if (ec) throw std::runtime_error("cannot inspect project revisions: " + ec.message());
    if (has_revisions && !has_current)
        throw std::runtime_error("legacy project has revisions but no current pointer");

    if (!has_current) ensure_directory(project_dir);
    const fs::path root = canonical_directory(project_dir, "project root");
    ProjectRevision current;
    if (has_current) current = read_current_revision(root);
    const std::vector<SourceRecord> previous_sources = current.sources;


    bool unknown_source = false;
    std::vector<fs::path> files;
    std::set<std::string> file_keys;
    std::set<std::string> visited_directories;
    std::set<std::string> active_directories;

    auto add_file = [&](const fs::path& path) {
        const std::string key = path_key(path);
        if (file_keys.emplace(key).second) files.push_back(path);
    };
    auto scan_source = [&](auto&& self, const fs::path& input) -> void {
        if (reserved_project_path(root, input)) return;
        std::error_code status_ec;
        const fs::file_status link_status = fs::symlink_status(input, status_ec);
        if (status_ec || !fs::exists(link_status)) {
            unknown_source = true;
            return;
        }
        const fs::path canonical = fs::canonical(input, status_ec);
        if (status_ec) {
            unknown_source = true;
            return;
        }
        if (reserved_project_path(root, canonical)) return;

        const fs::file_status status = fs::status(canonical, status_ec);
        if (status_ec) {
            unknown_source = true;
            return;
        }
        if (fs::is_regular_file(status)) {
            add_file(canonical);
            return;
        }
        if (!fs::is_directory(status)) {
            unknown_source = true;
            return;
        }

        const std::string directory_key = path_key(canonical);
        if (active_directories.count(directory_key)) {
            unknown_source = true;
            return;
        }
        if (!visited_directories.emplace(directory_key).second) return;
        active_directories.emplace(directory_key);

        fs::directory_iterator it(canonical, fs::directory_options::none, status_ec);
        if (status_ec) {
            active_directories.erase(directory_key);
            unknown_source = true;
            return;
        }
        const fs::directory_iterator end;
        for (; it != end;) {
            const fs::path child = it->path();
            self(self, child);
            it.increment(status_ec);
            if (status_ec) {
                unknown_source = true;
                break;
            }
        }
        active_directories.erase(directory_key);
    };

    for (fs::path source : source_roots) {
        if (source.is_relative()) source = root / source;
        scan_source(scan_source, source);
    }
    std::sort(files.begin(), files.end(),
              [](const fs::path& a, const fs::path& b) {
                  return path_key(a) < path_key(b);
              });

    ProjectRevision revision = has_current ? current : ProjectRevision{};
    bool dropped_source = false;
    if (has_current) {
        std::vector<SourceRecord> retained;
        retained.reserve(revision.sources.size());
        for (SourceRecord source : revision.sources) {
            std::vector<fs::path> locators;
            for (const fs::path& locator : source.locators) {
                try {
                    fs::path resolved = locator;
                    if (resolved.is_relative()) resolved = root / resolved;
                    const fs::path canonical = canonical_existing(resolved, "source locator");
                    const HashedFile hashed = hash_file(canonical);
                    if (hashed.size == source.size_bytes &&
                        "sha256:" + hashed.digest == source.source_id) {
                        locators.push_back(canonical);
                    } else {
                        dropped_source = true;
                    }
                } catch (const std::exception&) {
                    dropped_source = true;
                }
            }
            std::sort(locators.begin(), locators.end(),
                      [](const fs::path& a, const fs::path& b) {
                          return path_key(a) < path_key(b);
                      });
            locators.erase(
                std::unique(locators.begin(), locators.end(),
                            [](const fs::path& a, const fs::path& b) {
                                return path_key(a) == path_key(b);
                            }),
                locators.end());
            if (locators.empty()) {
                dropped_source = true;
                continue;
            }
            source.locators = std::move(locators);
            retained.push_back(std::move(source));
        }
        revision.sources = std::move(retained);
    }

    for (const fs::path& file : files) {
        try {
            SourceRecord source = make_source_record(file);
            const auto same = std::find_if(
                revision.sources.begin(), revision.sources.end(),
                [&](const SourceRecord& candidate) {
                    return candidate.source_id == source.source_id;
                });
            if (same == revision.sources.end()) {
                const auto previous = std::find_if(
                    previous_sources.begin(), previous_sources.end(),
                    [&](const SourceRecord& candidate) {
                        return candidate.source_id == source.source_id;
                    });
                if (previous != previous_sources.end()) {
                    source.relation = previous->relation;
                    source.original_source_id = previous->original_source_id;
                    source.streams = previous->streams;
                    source.original_metadata = previous->original_metadata;
                    source.timing_features_supported =
                        previous->timing_features_supported;
                    source.timing_features_unsupported =
                        previous->timing_features_unsupported;
                }
                revision.sources.push_back(std::move(source));
            } else {
                same->locators.push_back(file);
            }

        } catch (const std::exception&) {
            unknown_source = true;
        }
    }

    for (SourceRecord& source : revision.sources) {
        std::sort(source.locators.begin(), source.locators.end(),
                  [](const fs::path& a, const fs::path& b) {
                      return path_key(a) < path_key(b);
                  });
        source.locators.erase(
            std::unique(source.locators.begin(), source.locators.end(),
                        [](const fs::path& a, const fs::path& b) {
                            return path_key(a) == path_key(b);
                        }),
            source.locators.end());
    }

    std::set<std::string> source_ids;
    for (const SourceRecord& source : revision.sources) source_ids.emplace(source.source_id);
    std::vector<SourceRecord> valid_sources;
    valid_sources.reserve(revision.sources.size());
    for (SourceRecord source : revision.sources) {
        if (source.relation == SourceRelation::Export) {
            const auto original = std::find_if(
                revision.sources.begin(), revision.sources.end(),
                [&](const SourceRecord& candidate) {
                    return candidate.source_id == source.original_source_id &&
                           candidate.relation == SourceRelation::Original;
                });
            if (original == revision.sources.end()) {
                dropped_source = true;
                continue;
            }
        }
        valid_sources.push_back(std::move(source));
    }
    revision.sources = std::move(valid_sources);
    source_ids.clear();
    for (const SourceRecord& source : revision.sources) source_ids.emplace(source.source_id);
    std::sort(revision.sources.begin(), revision.sources.end(),
              [](const SourceRecord& a, const SourceRecord& b) {
                  return a.source_id < b.source_id;
              });

    {
        std::vector<RawClockRecord> clocks;
        std::set<std::string> clock_ids;
        for (const RawClockRecord& clock : revision.raw_clocks) {
            if (source_ids.count(clock.source_id) && clock_ids.emplace(clock.id).second)
                clocks.push_back(clock);
        }
        revision.raw_clocks = std::move(clocks);

        std::vector<SourceExportMapping> mappings;
        std::set<std::string> mapping_ids;
        for (const SourceExportMapping& mapping : revision.source_export_mappings) {
            if (source_ids.count(mapping.original_source_id) &&
                source_ids.count(mapping.export_source_id) &&
                mapping_ids.emplace(mapping.id).second)
                mappings.push_back(mapping);
        }
        revision.source_export_mappings = std::move(mappings);

        std::set<std::string> kept_clock_ids;
        for (const RawClockRecord& clock : revision.raw_clocks)
            kept_clock_ids.emplace(clock.id);
        std::set<std::string> kept_mapping_ids;
        for (const SourceExportMapping& mapping : revision.source_export_mappings)
            kept_mapping_ids.emplace(mapping.id);
        std::vector<TimingEstimate> estimates;
        for (const TimingEstimate& estimate : revision.timing_estimates) {
            bool keep = true;
            for (const std::string& clock : estimate.clock_refs)
                if (!kept_clock_ids.count(clock)) keep = false;
            if (!estimate.mapping_id.empty() &&
                (!kept_mapping_ids.count(estimate.mapping_id) ||
                 std::find_if(revision.source_export_mappings.begin(),
                              revision.source_export_mappings.end(),
                              [&](const SourceExportMapping& mapping) {
                                  return mapping.id == estimate.mapping_id &&
                                         mapping.revision == estimate.mapping_revision;
                              }) == revision.source_export_mappings.end()))
                keep = false;
            if (keep) estimates.push_back(estimate);
        }
        revision.timing_estimates = std::move(estimates);
    }

    bool dropped_artifact = false;
    std::vector<ArtifactReference> retained_artifacts;
    std::set<std::string> artifact_keys;
    if (has_current) {
        for (ArtifactReference artifact : revision.artifacts) {
            try {
                const std::string name = normalized_artifact_name(artifact.artifact);
                const fs::path target =
                    canonical_existing(root / fs::u8path(name), "artifact");
                if (!path_within(root, target) ||
                    is_current_pointer_target(root, target))
                    throw std::runtime_error("artifact escapes reserved project paths");
                if (!artifact_keys.emplace(path_key(target)).second) {
                    dropped_artifact = true;
                    continue;
                }
                artifact.artifact = name;
                retained_artifacts.push_back(std::move(artifact));
            } catch (const std::exception&) {
                dropped_artifact = true;
            }
        }
    }
    for (const fs::path& supplied : artifact_paths) {
        fs::path artifact = supplied;
        if (artifact.is_relative()) artifact = root / artifact;
        const fs::path target = canonical_existing(artifact, "legacy artifact");
        if (!path_within(root, target) ||
            is_current_pointer_target(root, target))
            throw std::runtime_error("legacy artifact is outside project paths");
        const fs::path relative = target.lexically_relative(root);
        if (relative.empty() || relative.is_absolute())
            throw std::runtime_error("legacy artifact is project root");
        const std::string name = normalized_artifact_name(relative.generic_u8string());
        if (artifact_keys.emplace(path_key(target)).second)
            retained_artifacts.push_back({name, "legacy-artifact", "unknown"});
    }
    revision.artifacts = std::move(retained_artifacts);

    std::sort(revision.artifacts.begin(), revision.artifacts.end(),
              [](const ArtifactReference& a, const ArtifactReference& b) {
                  if (a.artifact != b.artifact) return a.artifact < b.artifact;
                  if (a.role != b.role) return a.role < b.role;
                  return a.transform < b.transform;
              });

    auto add_operation = [&](const char* operation) {
        if (std::find(revision.operations.begin(), revision.operations.end(), operation) ==
            revision.operations.end())
            revision.operations.emplace_back(operation);
    };
    if (!has_current) add_operation("legacy-import");
    if (unknown_source || dropped_source)
        add_operation("legacy-source-provenance-unknown");
    if (dropped_artifact || (!has_current && !revision.artifacts.empty()))
        add_operation("legacy-artifact-provenance-unknown");

    if (has_current) {
        revision.name = current.name;
        if (serialize_revision(revision) == serialize_revision(current)) return current;
        revision.parents = {current.name};
    }


    revision.name = "legacy";
    const std::string identity = digest_bytes(serialize_revision(revision));
    revision.name = "legacy-" + identity.substr(7, 16);
    publish_revision(root, revision);
    return revision;
}

}  // namespace spirula::project
