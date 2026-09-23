#include "app/AgentFeatureWorker.h"

#include "core/Sha256.h"
#include "data/Json.h"
#include "data/JsonWrite.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <fstream>
#include <limits>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace app::agent::portable_worker_detail {
namespace {
namespace fs = std::filesystem;
constexpr std::uint64_t kMaxRecordBytes = 4ull << 20;
constexpr std::size_t kMaxAuthorities = kMaxAcceptedAttempts;
std::atomic<std::uint64_t> g_temporary_sequence{0};

bool valid_identifier(const std::string& value) {
    return !value.empty() && value.size() <= wire::kMaxFeatureIdBytes &&
           value.find('\0') == std::string::npos;
}

bool valid_digest(const std::string& value) {
    if (value.size() != 64) return false;
    for (const char c : value)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

std::string sha256_hex(const std::string& value) {
    spirula::Sha256 hash;
    hash.update(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
    return hash.hex();
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

bool plain_file(const fs::path& path, bool& exists) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory) {
        exists = false;
        return true;
    }
    if (ec) return false;
    exists = status.type() != fs::file_type::not_found;
    if (!exists) return true;
    if (!fs::is_regular_file(status) || fs::is_symlink(status)) return false;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
        return false;
#endif
    std::error_code link_error;
    return fs::hard_link_count(path, link_error) == 1 && !link_error;
}

bool ensure_directory(const fs::path& path, std::string& error) {
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec || !plain_directory(path)) {
        error = "portable worker store directory is unavailable";
        return false;
    }
    return true;
}

bool ensure_layout(const fs::path& root, std::string& error) {
    if (!root.is_absolute() || root == root.root_path() ||
        !ensure_directory(root, error) ||
        !ensure_directory(root / "attempts", error) ||
        !ensure_directory(root / "jobs", error))
        return false;
    return true;
}

fs::path attempt_directory(const fs::path& root, const std::string& job_id,
                           const std::string& attempt_id) {
    return root / "attempts" / PortableAttemptKey(job_id, attempt_id);
}

fs::path job_record_path(const fs::path& root, const std::string& job_id) {
    return root / "jobs" /
           (sha256_hex(std::string("spirula-portable-job-v1\n") + job_id) +
            ".json");
}

fs::path binding_path(const fs::path& root) { return root / "leader.json"; }

bool read_file(const fs::path& path, std::string& bytes, bool& exists,
               std::string& error) {
    if (!plain_file(path, exists)) {
        error = "portable worker record is not a plain file";
        return false;
    }
    if (!exists) return true;
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec || size > kMaxRecordBytes) {
        error = "portable worker record size is invalid";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "cannot read portable worker record";
        return false;
    }
    bytes.assign(static_cast<std::size_t>(size), '\0');
    if (size && !input.read(bytes.data(), static_cast<std::streamsize>(size))) {
        error = "portable worker record is truncated";
        return false;
    }
    if (input.peek() != std::char_traits<char>::eof() || input.bad()) {
        error = "portable worker record changed while reading";
        return false;
    }
    return true;
}

bool flush_file(const fs::path& path) {
#ifdef _WIN32
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool flushed = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    return flushed;
#else
    const int file = ::open(path.c_str(), O_RDONLY);
    if (file < 0) return false;
    const bool flushed = ::fsync(file) == 0;
    ::close(file);
    return flushed;
#endif
}

void flush_directory(const fs::path& path) {
#ifdef _WIN32
    (void)path;
#else
    const int directory = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (directory >= 0) {
        (void)::fsync(directory);
        ::close(directory);
    }
#endif
}

bool write_atomic(const fs::path& path, const std::string& bytes,
                  bool replace, std::string& error) {
    if (!plain_directory(path.parent_path())) {
        error = "portable worker record directory is unsafe";
        return false;
    }
    bool exists = false;
    if (!plain_file(path, exists) || (exists && !replace)) {
        error = "portable worker record already exists or is unsafe";
        return false;
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path temporary = fs::path(
        path.u8string() + ".tmp-" + std::to_string(now) + "-" +
        std::to_string(g_temporary_sequence.fetch_add(1,
                                                      std::memory_order_relaxed)));
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::out);
        if (!output) {
            error = "cannot create portable worker record";
            return false;
        }
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        output.close();
        if (output.fail()) {
            std::error_code ec;
            fs::remove(temporary, ec);
            error = "cannot flush portable worker record";
            return false;
        }
    }
    if (!flush_file(temporary)) {
        std::error_code ec;
        fs::remove(temporary, ec);
        error = "cannot durably flush portable worker record";
        return false;
    }
#ifdef _WIN32
    const DWORD flags = MOVEFILE_WRITE_THROUGH |
                        (replace ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!MoveFileExW(temporary.c_str(), path.c_str(), flags)) {
        std::error_code ec;
        fs::remove(temporary, ec);
        error = "cannot publish portable worker record";
        return false;
    }
#else
    std::error_code ec;
    fs::rename(temporary, path, ec);
    if (ec) {
        fs::remove(temporary, ec);
        error = "cannot publish portable worker record";
        return false;
    }
    flush_directory(path.parent_path());
#endif
    return true;
}

bool string_field(const JsonValue& object, const char* key,
                  std::string& result) {
    const JsonValue* value = object.find(key);
    if (!value || value->type != JsonValue::Type::String ||
        value->str.find('\0') != std::string::npos)
        return false;
    result = value->str;
    return true;
}

bool decimal_field(const JsonValue& object, const char* key,
                   std::uint64_t& result) {
    std::string text;
    if (!string_field(object, key, text) || text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size();
}

bool same_offer(const wire::PortableOffer& a, const wire::PortableOffer& b) {
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

const char* workload_name(wire::PortableWorkload workload) {
    switch (workload) {
        case wire::PortableWorkload::Reconstruction: return "reconstruction";
        case wire::PortableWorkload::Training: return "training";
    }
    return nullptr;
}

bool parse_workload(const std::string& name, wire::PortableWorkload& workload) {
    if (name == "reconstruction")
        workload = wire::PortableWorkload::Reconstruction;
    else if (name == "training")
        workload = wire::PortableWorkload::Training;
    else
        return false;
    return true;
}

bool same_result(const wire::PortableResult& a, const wire::PortableResult& b) {
    if (a.workload != b.workload || a.job_id != b.job_id ||
        a.attempt_id != b.attempt_id || a.outcome != b.outcome ||
        a.output_metadata != b.output_metadata || a.error != b.error ||
        a.outputs.size() != b.outputs.size())
        return false;
    for (std::size_t i = 0; i < a.outputs.size(); ++i)
        if (a.outputs[i].path != b.outputs[i].path ||
            a.outputs[i].size != b.outputs[i].size ||
            a.outputs[i].sha256 != b.outputs[i].sha256)
            return false;
    return true;
}

std::string offer_json(const wire::PortableOffer& offer,
                       const std::string& leader_id,
                       std::uint64_t leader_epoch) {
    JsonWriter json;
    json.object().field("schema", 1)
        .field("workload", workload_name(offer.workload))
        .field("job_id", offer.job_id).field("attempt_id", offer.attempt_id)
        .field("input_identity_sha256", offer.input_identity_sha256)
        .field("required_build", offer.required_build)
        .field("expires_at_ms", std::to_string(offer.expires_at_ms))
        .field("leader_id", leader_id)
        .field("leader_epoch", std::to_string(leader_epoch)).key("inputs").array();
    for (const TransferFile& file : offer.inputs)
        json.object().field("path", file.path)
            .field("size", std::to_string(file.size))
            .field("sha256", file.sha256).end();
    return json.end().end().str();
}


bool parse_offer_record(const std::string& bytes, PortableOfferRecord& record,
                        std::string& error) {
    try {
        const JsonValue root = json_parse(bytes);
        const JsonValue* schema = root.find("schema");
        const JsonValue* workload = root.find("workload");
        const JsonValue* inputs = root.find("inputs");
        const bool legacy_reconstruction =
            root.type == JsonValue::Type::Object && root.obj.size() == 9 &&
            !workload;
        if (root.type != JsonValue::Type::Object ||
            (root.obj.size() != 10 && !legacy_reconstruction) ||
            !schema || schema->type != JsonValue::Type::Number ||
            schema->num != 1 || !inputs || !inputs->is_array() ||
            inputs->arr.size() > wire::kMaxManifestEntries ||
            !string_field(root, "job_id", record.offer.job_id) ||
            !string_field(root, "attempt_id", record.offer.attempt_id) ||
            !string_field(root, "input_identity_sha256",
                          record.offer.input_identity_sha256) ||
            !string_field(root, "required_build", record.offer.required_build) ||
            !decimal_field(root, "expires_at_ms", record.offer.expires_at_ms) ||
            !string_field(root, "leader_id", record.leader_id) ||
            !decimal_field(root, "leader_epoch", record.leader_epoch) ||
            (!legacy_reconstruction &&
             (!workload || workload->type != JsonValue::Type::String)) ||
            !valid_identifier(record.offer.job_id) ||
            !valid_identifier(record.offer.attempt_id) ||
            !valid_identifier(record.leader_id) || !record.leader_epoch ||
            !record.offer.expires_at_ms ||
            !valid_digest(record.offer.input_identity_sha256) ||
            record.offer.required_build.empty() ||
            record.offer.required_build.size() > wire::kMaxBuildBytes)
            throw std::runtime_error("malformed portable offer record");
        if (legacy_reconstruction)
            record.offer.workload = wire::PortableWorkload::Reconstruction;
        else if (!parse_workload(workload->str, record.offer.workload))
            throw std::runtime_error("unsupported portable worker workload");
        record.offer.inputs.clear();
        record.offer.inputs.reserve(inputs->arr.size());
        for (const JsonValue& value : inputs->arr) {
            TransferFile file;
            if (value.type != JsonValue::Type::Object || value.obj.size() != 3 ||
                !string_field(value, "path", file.path) ||
                !decimal_field(value, "size", file.size) ||
                !string_field(value, "sha256", file.sha256))
                throw std::runtime_error("malformed portable input manifest");
            record.offer.inputs.push_back(std::move(file));
        }
        std::string encoded;
        const std::uint64_t now = record.offer.expires_at_ms > 1
                                      ? record.offer.expires_at_ms - 1
                                      : 1;
        if (record.offer.expires_at_ms <= now ||
            wire::Encode(wire::Message{wire::kSchemaVersion, record.offer},
                         now, encoded) != wire::Error::None)
            throw std::runtime_error("invalid portable input manifest");
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    } catch (...) {
        error = "portable offer record is malformed";
        return false;
    }
}

bool read_offer_directory(const fs::path& directory,
                          PortableOfferRecord& record,
                          std::string& error) {
    std::string bytes;
    bool exists = false;
    if (!read_file(directory / "offer.json", bytes, exists, error) || !exists) {
        if (error.empty()) error = "portable offer record is missing";
        return false;
    }
    return parse_offer_record(bytes, record, error);
}

std::string authority_json(const wire::PortableOffer& offer,
                           const std::string& leader_id,
                           std::uint64_t leader_epoch) {
    return offer_json(offer, leader_id, leader_epoch);
}

bool parse_authority(const std::string& bytes, PortableOfferRecord& record,
                     std::string& error) {
    return parse_offer_record(bytes, record, error);
}

bool load_authority(const fs::path& root, const std::string& job_id,
                    PortableOfferRecord& record, bool& exists,
                    std::string& error) {
    std::string bytes;
    if (!read_file(job_record_path(root, job_id), bytes, exists, error) ||
        !exists)
        return error.empty();
    if (!parse_authority(bytes, record, error)) return false;
    if (record.offer.job_id != job_id) {
        error = "portable job authority identity is inconsistent";
        return false;
    }
    return true;
}

bool parse_binding(const std::string& bytes, std::string& leader_id,
                   std::uint64_t& leader_epoch, std::string& error) {
    try {
        const JsonValue root = json_parse(bytes);
        const JsonValue* schema = root.find("schema");
        if (root.type != JsonValue::Type::Object || root.obj.size() != 3 ||
            !schema || schema->type != JsonValue::Type::Number ||
            schema->num != 1 || !string_field(root, "leader_id", leader_id) ||
            !decimal_field(root, "leader_epoch", leader_epoch) ||
            !valid_identifier(leader_id) || !leader_epoch)
            throw std::runtime_error("portable leader binding is malformed");
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    } catch (...) {
        error = "portable leader binding is malformed";
        return false;
    }
}

bool load_binding(const fs::path& root, std::string& leader_id,
                  std::uint64_t& leader_epoch, bool& exists,
                  std::string& error) {
    std::string bytes;
    if (!read_file(binding_path(root), bytes, exists, error) || !exists)
        return error.empty();
    return parse_binding(bytes, leader_id, leader_epoch, error);
}

std::string binding_json(const std::string& leader_id,
                         std::uint64_t leader_epoch) {
    JsonWriter json;
    json.object().field("schema", 1).field("leader_id", leader_id)
        .field("leader_epoch", std::to_string(leader_epoch));
    return json.end().str();
}

const char* outcome_name(wire::PortableResult::Outcome outcome) {
    switch (outcome) {
        case wire::PortableResult::Outcome::Succeeded: return "succeeded";
        case wire::PortableResult::Outcome::Failed: return "failed";
        case wire::PortableResult::Outcome::Interrupted: return "interrupted";
    }
    return "invalid";
}

bool parse_outcome(const std::string& value,
                   wire::PortableResult::Outcome& outcome) {
    if (value == "succeeded") outcome = wire::PortableResult::Outcome::Succeeded;
    else if (value == "failed") outcome = wire::PortableResult::Outcome::Failed;
    else if (value == "interrupted") outcome = wire::PortableResult::Outcome::Interrupted;
    else return false;
    return true;
}

std::string result_json(const wire::PortableResult& result) {
    JsonWriter json;
    json.object().field("schema", 1)
        .field("workload", workload_name(result.workload))
        .field("job_id", result.job_id).field("attempt_id", result.attempt_id)
        .field("outcome", outcome_name(result.outcome))
        .field("output_metadata", result.output_metadata)
        .field("error", result.error).key("outputs").array();
    for (const TransferFile& file : result.outputs)
        json.object().field("path", file.path)
            .field("size", std::to_string(file.size))
            .field("sha256", file.sha256).end();
    return json.end().end().str();
}

bool parse_result(const std::string& bytes, wire::PortableResult& result,
                  std::string& error) {
    try {
        const JsonValue root = json_parse(bytes);
        const JsonValue* schema = root.find("schema");
        const JsonValue* workload = root.find("workload");
        const JsonValue* metadata = root.find("output_metadata");
        const JsonValue* outputs = root.find("outputs");
        const bool legacy_reconstruction =
            root.type == JsonValue::Type::Object && root.obj.size() == 6 &&
            !workload && !metadata;
        std::string outcome;
        if (root.type != JsonValue::Type::Object ||
            (root.obj.size() != 8 && !legacy_reconstruction) ||
            !schema || schema->type != JsonValue::Type::Number ||
            schema->num != 1 ||
            (!legacy_reconstruction &&
             (!workload || workload->type != JsonValue::Type::String ||
              !metadata)) ||
            !outputs || !outputs->is_array() ||
            outputs->arr.size() > wire::kMaxManifestEntries ||
            !string_field(root, "job_id", result.job_id) ||
            !string_field(root, "attempt_id", result.attempt_id) ||
            !string_field(root, "outcome", outcome) ||
            !parse_outcome(outcome, result.outcome) ||
            (!legacy_reconstruction &&
             !string_field(root, "output_metadata", result.output_metadata)) ||
            !string_field(root, "error", result.error) ||
            !valid_identifier(result.job_id) ||
            !valid_identifier(result.attempt_id))
            throw std::runtime_error("portable result record is malformed");
        if (legacy_reconstruction)
            result.workload = wire::PortableWorkload::Reconstruction;
        else if (!parse_workload(workload->str, result.workload))
            throw std::runtime_error("unsupported portable worker workload");
        if (legacy_reconstruction) result.output_metadata.clear();
        result.outputs.clear();
        result.outputs.reserve(outputs->arr.size());
        for (const JsonValue& value : outputs->arr) {
            TransferFile file;
            if (value.type != JsonValue::Type::Object || value.obj.size() != 3 ||
                !string_field(value, "path", file.path) ||
                !decimal_field(value, "size", file.size) ||
                !string_field(value, "sha256", file.sha256))
                throw std::runtime_error("portable result manifest is malformed");
            result.outputs.push_back(std::move(file));
        }
        if ((result.outcome == wire::PortableResult::Outcome::Succeeded) !=
            !result.outputs.empty())
            throw std::runtime_error("portable result outcome and artifacts disagree");
        std::string encoded;
        if (wire::Encode(wire::Message{wire::kSchemaVersion, result}, 1,
                         encoded) != wire::Error::None)
            throw std::runtime_error("portable result fields are invalid");
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    } catch (...) {
        error = "portable result record is malformed";
        return false;
    }
}

bool read_result(const fs::path& directory, wire::PortableResult& result,
                 std::string& error) {
    std::string bytes;
    bool exists = false;
    if (!read_file(directory / "result.json", bytes, exists, error) || !exists) {
        if (error.empty()) error = "portable result record is missing";
        return false;
    }
    return parse_result(bytes, result, error);
}

bool read_acknowledgment(const fs::path& directory,
                         std::optional<std::string>& decision,
                         std::string& error) {
    std::string bytes;
    bool exists = false;
    if (!read_file(directory / "ack.json", bytes, exists, error)) return false;
    if (!exists) {
        decision.reset();
        return true;
    }
    try {
        const JsonValue root = json_parse(bytes);
        const JsonValue* schema = root.find("schema");
        std::string value;
        if (root.type != JsonValue::Type::Object || root.obj.size() != 2 ||
            !schema || schema->type != JsonValue::Type::Number ||
            schema->num != 1 || !string_field(root, "decision", value) ||
            (value != "committed" && value != "rejected"))
            throw std::runtime_error("portable acknowledgment is malformed");
        decision = std::move(value);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    } catch (...) {
        error = "portable acknowledgment is malformed";
        return false;
    }
}

bool read_input_rejection(const fs::path& directory,
                          std::optional<std::string>& reason,
                          std::string& error) {
    std::string bytes;
    bool exists = false;
    if (!read_file(directory / "input-rejection.json", bytes, exists, error))
        return false;
    if (!exists) {
        reason.reset();
        return true;
    }
    try {
        const JsonValue root = json_parse(bytes);
        const JsonValue* schema = root.find("schema");
        std::string value;
        if (root.type != JsonValue::Type::Object || root.obj.size() != 2 ||
            !schema || schema->type != JsonValue::Type::Number ||
            schema->num != 1 || !string_field(root, "reason", value))
            throw std::runtime_error("portable input rejection is malformed");
        reason = std::move(value);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    } catch (...) {
        error = "portable input rejection is malformed";
        return false;
    }
}

bool load_attempt_record(const fs::path& root, const std::string& job_id,
                         const std::string& attempt_id,
                         PortableOfferRecord& record, fs::path& directory,
                         std::string& error) {
    directory = attempt_directory(root, job_id, attempt_id);
    if (!plain_directory(directory)) {
        error = "portable attempt record is unavailable";
        return false;
    }
    if (!read_offer_directory(directory, record, error)) return false;
    if (record.offer.job_id != job_id || record.offer.attempt_id != attempt_id) {
        error = "portable attempt record identity is inconsistent";
        return false;
    }
    return true;
}

bool attempt_offer_missing(const fs::path& root, const std::string& job_id,
                           const std::string& attempt_id) {
    const fs::path directory = attempt_directory(root, job_id, attempt_id);
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(directory, ec);
    if (ec == std::errc::no_such_file_or_directory ||
        (!ec && status.type() == fs::file_type::not_found))
        return true;
    if (ec || !fs::is_directory(status) || fs::is_symlink(status)) return false;
    bool exists = false;
    return plain_file(directory / "offer.json", exists) && !exists;
}

bool current_authority(const fs::path& root,
                       const PortableOfferRecord& expected,
                       std::string& error) {
    bool authoritative = false;
    if (!IsPortableAttemptAuthoritative(root, expected.offer.job_id,
                                        expected.offer.attempt_id,
                                        expected.leader_id,
                                        expected.leader_epoch,
                                        authoritative, error))
        return false;
    if (!authoritative) error = "portable attempt is no longer authoritative";
    return authoritative;
}

}  // namespace

std::string PortableAttemptKey(const std::string& job_id,
                               const std::string& attempt_id) {
    return sha256_hex(job_id + "\n" + attempt_id);
}

fs::path PortableAttemptDirectory(const fs::path& root,
                                  const std::string& job_id,
                                  const std::string& attempt_id) {
    return attempt_directory(root, job_id, attempt_id);
}

bool BindPortableLeader(const fs::path& root, const std::string& leader_id,
                        std::uint64_t leader_epoch, std::string& error) {
    error.clear();
    if (!valid_identifier(leader_id) || !leader_epoch ||
        !ensure_layout(root, error)) {
        if (error.empty()) error = "portable leader identity is invalid";
        return false;
    }
    std::string current_leader;
    std::uint64_t current_epoch = 0;
    bool exists = false;
    if (!load_binding(root, current_leader, current_epoch, exists, error))
        return false;
    if (exists && current_leader == leader_id && current_epoch > leader_epoch) {
        error = "portable leader epoch is stale";
        return false;
    }
    if (exists && current_leader == leader_id && current_epoch == leader_epoch)
        return true;
    return write_atomic(binding_path(root), binding_json(leader_id, leader_epoch),
                        exists, error);
}

OfferRecordStatus SavePortableOffer(const fs::path& root,
                                    const wire::PortableOffer& offer,
                                    const std::string& leader_id,
                                    std::uint64_t leader_epoch,
                                    std::string& error) {
    error.clear();
    std::string encoded;
    const std::uint64_t validation_now = offer.expires_at_ms > 1
                                             ? offer.expires_at_ms - 1
                                             : 1;
    if (!wire::IsValid(offer.workload) ||
        !valid_identifier(offer.job_id) || !valid_identifier(offer.attempt_id) ||
        !valid_identifier(leader_id) || !leader_epoch ||
        !offer.expires_at_ms ||
        wire::Encode(wire::Message{wire::kSchemaVersion, offer}, validation_now,
                     encoded) != wire::Error::None ||
        !ensure_layout(root, error)) {
        if (error.empty()) error = "portable offer is invalid";
        return OfferRecordStatus::Error;
    }

    std::string bound_leader;
    std::uint64_t bound_epoch = 0;
    bool has_binding = false;
    if (!load_binding(root, bound_leader, bound_epoch, has_binding, error) ||
        !has_binding) {
        if (error.empty()) error = "portable leader binding is missing";
        return OfferRecordStatus::Error;
    }
    if (bound_leader != leader_id || bound_epoch != leader_epoch) {
        error = "portable offer is not from the currently paired leader";
        return OfferRecordStatus::Conflict;
    }

    PortableOfferRecord owner;
    bool has_owner = false;
    if (!load_authority(root, offer.job_id, owner, has_owner, error))
        return OfferRecordStatus::Error;

    const fs::path directory = attempt_directory(root, offer.job_id,
                                                 offer.attempt_id);
    std::error_code ec;
    fs::create_directory(directory, ec);
    if (ec && ec != std::errc::file_exists) {
        error = "cannot create portable attempt directory";
        return OfferRecordStatus::Error;
    }
    if (!plain_directory(directory)) {
        error = "portable attempt directory is unsafe";
        return OfferRecordStatus::Error;
    }

    const std::string record = offer_json(offer, leader_id, leader_epoch);
    PortableOfferRecord prior;
    std::string prior_error;
    if (read_offer_directory(directory, prior, prior_error)) {
        if (!same_offer(prior.offer, offer) || prior.leader_id != leader_id ||
            prior.leader_epoch != leader_epoch) {
            error = "portable attempt identity conflicts with its durable offer";
            return OfferRecordStatus::Conflict;
        }
        if (!has_owner || owner.offer.attempt_id != offer.attempt_id ||
            !same_offer(owner.offer, offer) || owner.leader_id != leader_id ||
            owner.leader_epoch != leader_epoch) {
            error = "portable attempt has been superseded";
            return OfferRecordStatus::Conflict;
        }
        return OfferRecordStatus::Existing;
    }
    if (prior_error != "portable offer record is missing") {
        error = prior_error;
        return OfferRecordStatus::Error;
    }

    if (has_owner && owner.offer.attempt_id == offer.attempt_id) {
        if (!same_offer(owner.offer, offer) || owner.leader_id != leader_id ||
            owner.leader_epoch != leader_epoch) {
            error = "portable attempt conflicts with its durable authority";
            return OfferRecordStatus::Conflict;
        }
        if (!write_atomic(directory / "offer.json", record, false, error))
            return OfferRecordStatus::Error;
        return OfferRecordStatus::Existing;
    }

    if (!write_atomic(job_record_path(root, offer.job_id),
                      authority_json(offer, leader_id, leader_epoch),
                      has_owner, error))
        return OfferRecordStatus::Error;
    if (!write_atomic(directory / "offer.json", record, false, error))
        return OfferRecordStatus::Error;
    return has_owner ? OfferRecordStatus::Superseded
                     : OfferRecordStatus::Created;
}

bool LoadPortableOfferRecord(const fs::path& root, const std::string& job_id,
                             const std::string& attempt_id,
                             PortableOfferRecord& record,
                             std::string& error) {
    error.clear();
    fs::path directory;
    if (!load_attempt_record(root, job_id, attempt_id, record, directory, error))
        return false;
    return true;
}

bool LoadPortableOfferRecordFromAttempt(const fs::path& attempt_root,
                                        PortableOfferRecord& record,
                                        std::string& error) {
    error.clear();
    if (!plain_directory(attempt_root)) {
        error = "portable attempt directory is unavailable";
        return false;
    }
    if (!read_offer_directory(attempt_root, record, error)) return false;
    return true;
}

bool LoadPortableAuthorities(const fs::path& root,
                             std::vector<PortableOfferRecord>& records,
                             std::string& error) {
    error.clear();
    if (!ensure_layout(root, error)) return false;
    records.clear();
    std::error_code ec;
    fs::directory_iterator it(root / "jobs", fs::directory_options::none, ec), end;
    if (ec) {
        error = "cannot enumerate portable job authorities";
        return false;
    }
    for (; it != end; it.increment(ec)) {
        if (ec) {
            error = "cannot enumerate portable job authorities";
            return false;
        }
        bool exists = false;
        if (!plain_file(it->path(), exists) || !exists ||
            it->path().extension() != ".json" ||
            records.size() >= kMaxAuthorities) {
            error = "portable job authority directory is unsafe or full";
            return false;
        }
        std::string bytes;
        if (!read_file(it->path(), bytes, exists, error) || !exists) return false;
        PortableOfferRecord authority;
        if (!parse_authority(bytes, authority, error)) return false;
        PortableOfferRecord offer_record;
        if (!LoadPortableOfferRecord(root, authority.offer.job_id,
                                     authority.offer.attempt_id, offer_record,
                                     error)) {
            if (!attempt_offer_missing(root, authority.offer.job_id,
                                       authority.offer.attempt_id))
                return false;
            error.clear();
            offer_record = authority;
        }
        if (!same_offer(offer_record.offer, authority.offer) ||
            offer_record.leader_id != authority.leader_id ||
            offer_record.leader_epoch != authority.leader_epoch) {
            error = "portable job authority does not match its offer";
            return false;
        }
        records.push_back(std::move(offer_record));
    }
    return true;
}

bool IsPortableAttemptAuthoritative(const fs::path& root,
                                    const std::string& job_id,
                                    const std::string& attempt_id,
                                    const std::string& leader_id,
                                    std::uint64_t leader_epoch,
                                    bool& authoritative,
                                    std::string& error) {
    error.clear();
    authoritative = false;
    std::string bound_leader;
    std::uint64_t bound_epoch = 0;
    bool has_binding = false;
    if (!load_binding(root, bound_leader, bound_epoch, has_binding, error))
        return false;
    if (!has_binding || bound_leader != leader_id ||
        bound_epoch != leader_epoch)
        return true;
    PortableOfferRecord owner;
    bool has_owner = false;
    if (!load_authority(root, job_id, owner, has_owner, error)) return false;
    if (!has_owner || owner.offer.attempt_id != attempt_id ||
        owner.leader_id != leader_id || owner.leader_epoch != leader_epoch)
        return true;
    PortableOfferRecord record;
    fs::path directory;
    if (!load_attempt_record(root, job_id, attempt_id, record, directory,
                             error)) {
        if (!attempt_offer_missing(root, job_id, attempt_id)) return false;
        error.clear();
        authoritative = true;
        return true;
    }
    if (!same_offer(record.offer, owner.offer) ||
        record.leader_id != owner.leader_id ||
        record.leader_epoch != owner.leader_epoch) {
        error = "portable attempt does not match its current authority";
        return false;
    }
    authoritative = true;
    return true;
}

bool SavePortableResult(const fs::path& root,
                        const wire::PortableResult& result,
                        std::string& error) {
    error.clear();
    if (!wire::IsValid(result.workload) ||
        !valid_identifier(result.job_id) || !valid_identifier(result.attempt_id) ||
        ((result.outcome == wire::PortableResult::Outcome::Succeeded) !=
         !result.outputs.empty())) {
        error = "portable result is invalid";
        return false;
    }
    std::string encoded;
    if (wire::Encode(wire::Message{wire::kSchemaVersion, result}, 1, encoded) !=
        wire::Error::None) {
        error = "portable result is invalid";
        return false;
    }
    PortableOfferRecord record;
    fs::path directory;
    if (!load_attempt_record(root, result.job_id, result.attempt_id, record,
                             directory, error))
        return false;
    if (!current_authority(root, record, error)) return false;
    if (result.workload != record.offer.workload ||
        result.job_id != record.offer.job_id ||
        result.attempt_id != record.offer.attempt_id) {
        error = "portable result does not match its offer";
        return false;
    }
    wire::PortableResult prior;
    std::string prior_error;
    if (read_result(directory, prior, prior_error)) {
        if (same_result(prior, result)) return true;
        error = "portable result conflicts with its durable result";
        return false;
    }
    if (prior_error != "portable result record is missing") {
        error = prior_error;
        return false;
    }
    return write_atomic(directory / "result.json", result_json(result), false,
                        error);
}

bool LoadPortableResult(const fs::path& root, const std::string& job_id,
                        const std::string& attempt_id,
                        wire::PortableResult& result, std::string& error) {
    error.clear();
    fs::path directory;
    PortableOfferRecord record;
    if (!load_attempt_record(root, job_id, attempt_id, record, directory,
                             error)) {
        if (attempt_offer_missing(root, job_id, attempt_id)) {
            PortableOfferRecord owner;
            bool has_owner = false;
            if (!load_authority(root, job_id, owner, has_owner, error))
                return false;
            if (has_owner && owner.offer.attempt_id == attempt_id) {
                error = "portable result record is missing";
                return false;
            }
        }
        return false;
    }
    if (!read_result(directory, result, error)) return false;
    if (result.workload != record.offer.workload ||
        result.job_id != job_id || result.attempt_id != attempt_id) {
        error = "portable result identity is inconsistent";
        return false;
    }
    return true;
}

bool SavePortableInputRejection(const fs::path& root,
                                const std::string& job_id,
                                const std::string& attempt_id,
                                const std::string& reason,
                                std::string& error) {
    error.clear();
    PortableOfferRecord record;
    fs::path directory;
    if (!load_attempt_record(root, job_id, attempt_id, record, directory, error))
        return false;
    if (!current_authority(root, record, error)) return false;
    std::optional<std::string> prior;
    if (!read_input_rejection(directory, prior, error)) return false;
    if (prior) {
        if (*prior == reason) return true;
        error = "portable input rejection conflicts with its durable record";
        return false;
    }
    return write_atomic(directory / "input-rejection.json",
                        JsonWriter().object().field("schema", 1)
                            .field("reason", reason).end().str(),
                        false, error);
}

bool LoadPortableInputRejection(const fs::path& root,
                                const std::string& job_id,
                                const std::string& attempt_id,
                                std::optional<std::string>& reason,
                                std::string& error) {
    error.clear();
    PortableOfferRecord record;
    fs::path directory;
    if (!load_attempt_record(root, job_id, attempt_id, record, directory,
                             error)) {
        if (attempt_offer_missing(root, job_id, attempt_id)) {
            PortableOfferRecord owner;
            bool has_owner = false;
            if (!load_authority(root, job_id, owner, has_owner, error))
                return false;
            if (has_owner && owner.offer.attempt_id == attempt_id) {
                reason.reset();
                error.clear();
                return true;
            }
        }
        return false;
    }
    return read_input_rejection(directory, reason, error);
}

bool SavePortableAcknowledgment(const fs::path& root,
                                const std::string& job_id,
                                const std::string& attempt_id,
                                const std::string& decision,
                                std::string& error) {
    error.clear();
    if (decision != "committed" && decision != "rejected") {
        error = "portable output acknowledgment is invalid";
        return false;
    }
    PortableOfferRecord record;
    fs::path directory;
    if (!load_attempt_record(root, job_id, attempt_id, record, directory, error))
        return false;
    if (!current_authority(root, record, error)) return false;
    wire::PortableResult result;
    if (!read_result(directory, result, error)) return false;
    std::optional<std::string> prior;
    if (!read_acknowledgment(directory, prior, error)) return false;
    if (prior) {
        if (*prior == decision) return true;
        error = "portable output acknowledgment conflicts with its durable record";
        return false;
    }
    return write_atomic(directory / "ack.json",
                        JsonWriter().object().field("schema", 1)
                            .field("decision", decision).end().str(),
                        false, error);
}

bool LoadPortableAcknowledgment(const fs::path& root,
                                const std::string& job_id,
                                const std::string& attempt_id,
                                std::optional<std::string>& decision,
                                std::string& error) {
    error.clear();
    PortableOfferRecord record;
    fs::path directory;
    if (!load_attempt_record(root, job_id, attempt_id, record, directory,
                             error)) {
        if (attempt_offer_missing(root, job_id, attempt_id)) {
            PortableOfferRecord owner;
            bool has_owner = false;
            if (!load_authority(root, job_id, owner, has_owner, error))
                return false;
            if (has_owner && owner.offer.attempt_id == attempt_id) {
                decision.reset();
                error.clear();
                return true;
            }
        }
        return false;
    }
    return read_acknowledgment(directory, decision, error);
}

}  // namespace app::agent::portable_worker_detail
