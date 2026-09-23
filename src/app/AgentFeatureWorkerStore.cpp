#include "app/AgentFeatureWorker.h"

#include "core/Sha256.h"
#include "data/Json.h"
#include "data/JsonWrite.h"

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
#endif

namespace app::agent::feature_worker_detail {
namespace {

namespace fs = std::filesystem;
constexpr std::uint64_t kMaxRecordBytes = 4ull << 20;
std::atomic<std::uint64_t> g_temporary_sequence{0};

bool same_offer(const wire::FeatureOffer& a, const wire::FeatureOffer& b) {
    if (a.job_id != b.job_id || a.attempt_id != b.attempt_id ||
        a.plan_digest != b.plan_digest ||
        a.request_digest != b.request_digest ||
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

bool same_result(const wire::FeatureResult& a, const wire::FeatureResult& b) {
    if (a.job_id != b.job_id || a.attempt_id != b.attempt_id ||
        a.outcome != b.outcome || a.error != b.error ||
        a.outputs.size() != b.outputs.size())
        return false;
    for (std::size_t i = 0; i < a.outputs.size(); ++i)
        if (a.outputs[i].path != b.outputs[i].path ||
            a.outputs[i].size != b.outputs[i].size ||
            a.outputs[i].sha256 != b.outputs[i].sha256)
            return false;
    return true;
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
    return true;
}

bool read_file(const fs::path& path, std::string& bytes, bool& exists,
               std::string& error) {
    if (!plain_file(path, exists)) {
        error = "feature record is not a plain file";
        return false;
    }
    if (!exists) return true;
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec || size > kMaxRecordBytes) {
        error = "feature record size is invalid";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "cannot read feature record";
        return false;
    }
    bytes.assign(static_cast<std::size_t>(size), '\0');
    if (size) {
        input.read(bytes.data(), static_cast<std::streamsize>(size));
        if (input.gcount() != static_cast<std::streamsize>(size)) {
            error = "feature record is truncated";
            return false;
        }
    }
    if (input.peek() != std::char_traits<char>::eof() || input.bad()) {
        error = "feature record changed while reading";
        return false;
    }
    return true;
}

std::string temp_suffix() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return ".tmp-" + std::to_string(now) + "-" +
           std::to_string(g_temporary_sequence.fetch_add(1,
                                                         std::memory_order_relaxed));
}

bool write_new_file(const fs::path& path, const std::string& bytes,
                    std::string& error) {
    std::error_code ec;
    const fs::path temporary = fs::path(path.u8string() + temp_suffix());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::out);
        if (!output) {
            error = "cannot create feature record";
            return false;
        }
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        output.close();
        if (output.fail()) {
            fs::remove(temporary, ec);
            error = "cannot flush feature record";
            return false;
        }
    }
    if (!plain_directory(path.parent_path())) {
        fs::remove(temporary, ec);
        error = "feature record directory is unsafe";
        return false;
    }
    bool exists = false;
    if (!plain_file(path, exists) || exists) {
        fs::remove(temporary, ec);
        error = "feature record already exists or is unsafe";
        return false;
    }
    fs::rename(temporary, path, ec);
    if (ec) {
        fs::remove(temporary, ec);
        error = "cannot publish feature record";
        return false;
    }
#ifdef _WIN32
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "cannot reopen feature record";
        return false;
    }
    const bool flushed = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    if (!flushed) {
        error = "cannot flush feature record";
        return false;
    }
#endif
    return true;
}

bool root_directory(const fs::path& root) {
    return root.is_absolute() && root != root.root_path() &&
           plain_directory(root);
}

fs::path attempt_directory(const fs::path& root, const std::string& job_id,
                           const std::string& attempt_id) {
    return root / FeatureAttemptKey(job_id, attempt_id);
}

bool ensure_attempt_directory(const fs::path& root,
                              const std::string& job_id,
                              const std::string& attempt_id,
                              fs::path& directory, bool& created,
                              std::string& error) {
    if (!root_directory(root)) {
        error = "feature attempt store root is unavailable";
        return false;
    }
    directory = attempt_directory(root, job_id, attempt_id);
    std::error_code ec;
    created = fs::create_directory(directory, ec);
    if (ec && ec != std::errc::file_exists) {
        error = "cannot create feature attempt directory";
        return false;
    }
    if (!plain_directory(directory)) {
        error = "feature attempt directory is unsafe";
        return false;
    }
    return true;
}

std::string offer_json(const wire::FeatureOffer& offer,
                       const std::string& leader_id,
                       std::uint64_t leader_epoch) {
    JsonWriter json;
    json.object().field("schema", 2).field("job_id", offer.job_id)
        .field("attempt_id", offer.attempt_id)
        .field("plan_digest", offer.plan_digest)
        .field("request_digest", offer.request_digest)
        .field("required_build", offer.required_build)
        .field("expires_at_ms", std::to_string(offer.expires_at_ms))
        .field("leader_id", leader_id)
        .field("leader_epoch", std::to_string(leader_epoch))
        .key("inputs").array();
    for (const TransferFile& file : offer.inputs)
        json.object().field("path", file.path)
            .field("size", std::to_string(file.size))
            .field("sha256", file.sha256).end();
    return json.end().end().str();
}

bool string_field(const JsonValue& object, const char* key,
                  std::string& result) {
    const JsonValue* value = object.find(key);
    if (!value || value->type != JsonValue::Type::String) return false;
    result = value->str;
    return true;
}

bool decimal_field(const JsonValue& object, const char* key,
                   std::uint64_t& result) {
    std::string text;
    if (!string_field(object, key, text) || text.empty()) return false;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, result);
    return parsed.ec == std::errc() && parsed.ptr == end;
}

bool parse_offer_record(const std::string& bytes,
                        FeatureOfferRecord& record, std::string& error) {
    try {
        const JsonValue root = json_parse(bytes);
        const JsonValue* schema = root.find("schema");
        const JsonValue* inputs = root.find("inputs");
        if (root.type != JsonValue::Type::Object || !schema ||
            schema->type != JsonValue::Type::Number || schema->num != 2 ||
            !inputs || !inputs->is_array() ||
            !string_field(root, "job_id", record.offer.job_id) ||
            !string_field(root, "attempt_id", record.offer.attempt_id) ||
            !string_field(root, "plan_digest", record.offer.plan_digest) ||
            !string_field(root, "request_digest", record.offer.request_digest) ||
            !string_field(root, "required_build", record.offer.required_build) ||
            !decimal_field(root, "expires_at_ms", record.offer.expires_at_ms) ||
            !string_field(root, "leader_id", record.leader_id) ||
            record.leader_id.empty() ||
            !decimal_field(root, "leader_epoch", record.leader_epoch) ||
            !record.leader_epoch) {
            error = "feature offer record is malformed";
            return false;
        }
        record.offer.inputs.clear();
        record.offer.inputs.reserve(inputs->arr.size());
        for (const JsonValue& value : inputs->arr) {
            TransferFile file;
            if (value.type != JsonValue::Type::Object ||
                !string_field(value, "path", file.path) ||
                !decimal_field(value, "size", file.size) ||
                !string_field(value, "sha256", file.sha256)) {
                error = "feature offer manifest record is malformed";
                return false;
            }
            record.offer.inputs.push_back(std::move(file));
        }
        return true;
    } catch (...) {
        error = "feature offer record is malformed";
        return false;
    }
}

const char* outcome_token(wire::FeatureResult::Outcome outcome) {
    switch (outcome) {
        case wire::FeatureResult::Outcome::Succeeded: return "succeeded";
        case wire::FeatureResult::Outcome::Failed: return "failed";
        case wire::FeatureResult::Outcome::Interrupted: return "interrupted";
    }
    return "invalid";
}

bool parse_outcome(const std::string& token,
                   wire::FeatureResult::Outcome& outcome) {
    if (token == "succeeded") outcome = wire::FeatureResult::Outcome::Succeeded;
    else if (token == "failed") outcome = wire::FeatureResult::Outcome::Failed;
    else if (token == "interrupted") outcome = wire::FeatureResult::Outcome::Interrupted;
    else return false;
    return true;
}

std::string result_json(const wire::FeatureResult& result) {
    JsonWriter json;
    json.object().field("schema", 1).field("job_id", result.job_id)
        .field("attempt_id", result.attempt_id)
        .field("outcome", outcome_token(result.outcome))
        .field("error", result.error).key("outputs").array();
    for (const TransferFile& file : result.outputs)
        json.object().field("path", file.path)
            .field("size", std::to_string(file.size))
            .field("sha256", file.sha256).end();
    return json.end().end().str();
}

bool parse_result(const std::string& bytes, wire::FeatureResult& result,
                  std::string& error) {
    try {
        const JsonValue root = json_parse(bytes);
        const JsonValue* schema = root.find("schema");
        const JsonValue* outputs = root.find("outputs");
        std::string outcome;
        if (root.type != JsonValue::Type::Object || !schema ||
            schema->type != JsonValue::Type::Number || schema->num != 1 ||
            !outputs || !outputs->is_array() ||
            !string_field(root, "job_id", result.job_id) ||
            !string_field(root, "attempt_id", result.attempt_id) ||
            !string_field(root, "outcome", outcome) ||
            !parse_outcome(outcome, result.outcome) ||
            !string_field(root, "error", result.error)) {
            error = "feature result record is malformed";
            return false;
        }
        result.outputs.clear();
        result.outputs.reserve(outputs->arr.size());
        for (const JsonValue& value : outputs->arr) {
            TransferFile file;
            if (value.type != JsonValue::Type::Object ||
                !string_field(value, "path", file.path) ||
                !decimal_field(value, "size", file.size) ||
                !string_field(value, "sha256", file.sha256)) {
                error = "feature result manifest record is malformed";
                return false;
            }
            result.outputs.push_back(std::move(file));
        }
        return true;
    } catch (...) {
        error = "feature result record is malformed";
        return false;
    }
}

bool read_offer_in_directory(const fs::path& directory,
                             FeatureOfferRecord& record,
                             std::string& error) {
    std::string bytes;
    bool exists = false;
    if (!read_file(directory / "offer.json", bytes, exists, error) || !exists) {
        if (error.empty()) error = "feature offer record is missing";
        return false;
    }
    return parse_offer_record(bytes, record, error);
}

}  // namespace

std::string FeatureAttemptKey(const std::string& job_id,
                              const std::string& attempt_id) {
    const std::string identity = job_id + "\n" + attempt_id;
    spirula::Sha256 hash;
    hash.update(reinterpret_cast<const std::uint8_t*>(identity.data()),
                identity.size());
    return hash.hex();
}

OfferRecordStatus SaveFeatureOffer(const fs::path& root,
                                   const wire::FeatureOffer& offer,
                                   const std::string& leader_id,
                                   std::uint64_t leader_epoch,
                                   std::string& error) {
    error.clear();
    if (leader_id.empty() || !leader_epoch) {
        error = "feature offer has no authenticated leader identity";
        return OfferRecordStatus::Error;
    }
    fs::path directory;
    bool created = false;
    if (!ensure_attempt_directory(root, offer.job_id, offer.attempt_id,
                                  directory, created, error))
        return OfferRecordStatus::Error;

    std::string bytes;
    bool exists = false;
    if (!read_file(directory / "offer.json", bytes, exists, error))
        return OfferRecordStatus::Error;
    if (exists) {
        FeatureOfferRecord previous;
        if (!parse_offer_record(bytes, previous, error))
            return OfferRecordStatus::Error;
        if (!same_offer(previous.offer, offer) ||
            previous.leader_id != leader_id ||
            previous.leader_epoch != leader_epoch) {
            error = "attempt identity already belongs to another offer or leader";
            return OfferRecordStatus::Conflict;
        }
        return OfferRecordStatus::Existing;
    }
    if (!created) {
        error = "attempt directory has no authoritative offer record";
        return OfferRecordStatus::Conflict;
    }
    if (!write_new_file(directory / "offer.json",
                        offer_json(offer, leader_id, leader_epoch), error)) {
        std::error_code ec;
        fs::remove_all(directory, ec);
        return OfferRecordStatus::Error;
    }
    return OfferRecordStatus::Created;
}

bool LoadFeatureOfferRecord(const fs::path& root, const std::string& job_id,
                            const std::string& attempt_id,
                            FeatureOfferRecord& record,
                            std::string& error) {
    error.clear();
    const fs::path directory = attempt_directory(root, job_id, attempt_id);
    if (!plain_directory(root) || !plain_directory(directory)) {
        error = "feature attempt record is unavailable";
        return false;
    }
    if (!read_offer_in_directory(directory, record, error)) return false;
    if (record.offer.job_id != job_id ||
        record.offer.attempt_id != attempt_id) {
        error = "feature attempt record identity differs";
        return false;
    }
    return true;
}

bool LoadFeatureOffer(const fs::path& root, const std::string& job_id,
                      const std::string& attempt_id, wire::FeatureOffer& offer,
                      std::string& error) {
    FeatureOfferRecord record;
    if (!LoadFeatureOfferRecord(root, job_id, attempt_id, record, error))
        return false;
    offer = std::move(record.offer);
    return true;
}

bool SaveFeatureResult(const fs::path& root, const wire::FeatureResult& result,
                       std::string& error) {
    error.clear();
    wire::FeatureOffer offer;
    if (!LoadFeatureOffer(root, result.job_id, result.attempt_id, offer, error))
        return false;
    const fs::path path = attempt_directory(root, result.job_id,
                                            result.attempt_id) /
                          "feature-result.json";
    std::string bytes;
    bool exists = false;
    if (!read_file(path, bytes, exists, error)) return false;
    if (exists) {
        wire::FeatureResult previous;
        if (!parse_result(bytes, previous, error)) return false;
        if (!same_result(previous, result)) {
            error = "feature result is immutable once persisted";
            return false;
        }
        return true;
    }
    return write_new_file(path, result_json(result), error);
}

bool LoadFeatureResult(const fs::path& root, const std::string& job_id,
                      const std::string& attempt_id,
                      wire::FeatureResult& result, std::string& error) {
    error.clear();
    const fs::path directory = attempt_directory(root, job_id, attempt_id);
    if (!plain_directory(root) || !plain_directory(directory)) {
        error = "feature attempt record is unavailable";
        return false;
    }
    std::string bytes;
    bool exists = false;
    if (!read_file(directory / "feature-result.json", bytes, exists, error) ||
        !exists) {
        if (error.empty()) error = "feature result record is missing";
        return false;
    }
    if (!parse_result(bytes, result, error)) return false;
    if (result.job_id != job_id || result.attempt_id != attempt_id) {
        error = "feature result identity differs";
        return false;
    }
    return true;
}
bool SaveFeatureInputRejection(const fs::path& root, const std::string& job_id,
                               const std::string& attempt_id,
                               const std::string& reason,
                               std::string& error) {
    error.clear();
    wire::FeatureOffer offer;
    if (!LoadFeatureOffer(root, job_id, attempt_id, offer, error)) return false;
    const fs::path path =
        attempt_directory(root, job_id, attempt_id) / "inputs-rejected.json";
    std::string bytes;
    bool exists = false;
    if (!read_file(path, bytes, exists, error)) return false;
    if (exists) {
        std::optional<std::string> previous;
        return LoadFeatureInputRejection(root, job_id, attempt_id, previous,
                                         error) &&
               previous.has_value();
    }
    JsonWriter json;
    json.object().field("schema", 1).field("job_id", job_id)
        .field("attempt_id", attempt_id).field("reason", reason).end();
    return write_new_file(path, json.str(), error);
}

bool LoadFeatureInputRejection(const fs::path& root,
                               const std::string& job_id,
                               const std::string& attempt_id,
                               std::optional<std::string>& reason,
                               std::string& error) {
    error.clear();
    reason.reset();
    const fs::path directory = attempt_directory(root, job_id, attempt_id);
    if (!plain_directory(root) || !plain_directory(directory)) {
        error = "feature attempt record is unavailable";
        return false;
    }
    std::string bytes;
    bool exists = false;
    if (!read_file(directory / "inputs-rejected.json", bytes, exists, error))
        return false;
    if (!exists) return true;
    try {
        const JsonValue value = json_parse(bytes);
        const JsonValue* schema = value.find("schema");
        std::string saved_job, saved_attempt, saved_reason;
        if (value.type != JsonValue::Type::Object || !schema ||
            schema->type != JsonValue::Type::Number || schema->num != 1 ||
            !string_field(value, "job_id", saved_job) ||
            !string_field(value, "attempt_id", saved_attempt) ||
            !string_field(value, "reason", saved_reason) ||
            saved_job != job_id || saved_attempt != attempt_id) {
            error = "feature input rejection record is malformed";
            return false;
        }
        reason = std::move(saved_reason);
        return true;
    } catch (...) {
        error = "feature input rejection record is malformed";
        return false;
    }
}


bool SaveFeatureAcknowledgment(const fs::path& root, const std::string& job_id,
                               const std::string& attempt_id,
                               const std::string& decision,
                               std::string& error) {
    error.clear();
    if (decision != "committed" && decision != "rejected") {
        error = "invalid feature output acknowledgment";
        return false;
    }
    const fs::path directory = attempt_directory(root, job_id, attempt_id);
    if (!plain_directory(root) || !plain_directory(directory)) {
        error = "feature attempt record is unavailable";
        return false;
    }
    std::string result_bytes;
    bool result_exists = false;
    if (!read_file(directory / "feature-result.json", result_bytes,
                   result_exists, error) || !result_exists) {
        if (error.empty()) error = "feature result must be persisted first";
        return false;
    }
    wire::FeatureResult result;
    if (!parse_result(result_bytes, result, error) || result.job_id != job_id ||
        result.attempt_id != attempt_id)
        return false;

    const fs::path path = directory / "output-ack.json";
    std::string bytes;
    bool exists = false;
    if (!read_file(path, bytes, exists, error)) return false;
    if (exists) {
        std::optional<std::string> previous;
        if (!LoadFeatureAcknowledgment(root, job_id, attempt_id, previous,
                                       error) ||
            !previous)
            return false;
        if (*previous != decision) {
            error = "feature output acknowledgment is immutable";
            return false;
        }
        return true;
    }
    JsonWriter json;
    json.object().field("schema", 1).field("job_id", job_id)
        .field("attempt_id", attempt_id).field("decision", decision).end();
    return write_new_file(path, json.str(), error);
}

bool LoadFeatureAcknowledgment(const fs::path& root, const std::string& job_id,
                               const std::string& attempt_id,
                               std::optional<std::string>& decision,
                               std::string& error) {
    error.clear();
    decision.reset();
    const fs::path directory = attempt_directory(root, job_id, attempt_id);
    if (!plain_directory(root) || !plain_directory(directory)) {
        error = "feature attempt record is unavailable";
        return false;
    }
    std::string bytes;
    bool exists = false;
    if (!read_file(directory / "output-ack.json", bytes, exists, error))
        return false;
    if (!exists) return true;
    try {
        const JsonValue root_value = json_parse(bytes);
        const JsonValue* schema = root_value.find("schema");
        std::string saved_job, saved_attempt, saved_decision;
        if (root_value.type != JsonValue::Type::Object || !schema ||
            schema->type != JsonValue::Type::Number || schema->num != 1 ||
            !string_field(root_value, "job_id", saved_job) ||
            !string_field(root_value, "attempt_id", saved_attempt) ||
            !string_field(root_value, "decision", saved_decision) ||
            saved_job != job_id || saved_attempt != attempt_id ||
            (saved_decision != "committed" && saved_decision != "rejected")) {
            error = "feature output acknowledgment is malformed";
            return false;
        }
        decision = std::move(saved_decision);
        return true;
    } catch (...) {
        error = "feature output acknowledgment is malformed";
        return false;
    }
}

}  // namespace app::agent::feature_worker_detail
