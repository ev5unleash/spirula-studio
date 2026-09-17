// WorkerRequest -- one scheduled phase, JSON codec.
// Validation stops malformed requests before the worker changes state.

#include "app/WorkerRequest.h"

#include "data/Json.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace app::worker {

namespace {

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + path);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

std::string need_string(const JsonValue& v, const char* key) {
    const JsonValue* p = v.find(key);
    if (!p)
        throw std::runtime_error(std::string("request: missing \"") + key + "\"");
    if (p->type != JsonValue::Type::String)
        throw std::runtime_error(std::string("request: \"") + key +
                                 "\" must be a string");
    return p->str;
}

int need_int(const JsonValue& v, const char* key) {
    const JsonValue* p = v.find(key);
    if (!p)
        throw std::runtime_error(std::string("request: missing \"") + key + "\"");
    if (p->type != JsonValue::Type::Number || !std::isfinite(p->num) ||
        std::trunc(p->num) != p->num ||
        p->num < (double)std::numeric_limits<int>::min() ||
        p->num >= (double)std::numeric_limits<int>::max() + 1.0)
        throw std::runtime_error(std::string("request: \"") + key +
                                 "\" must be an integer");
    return (int)p->num;
}

bool has_nul(const std::string& s) {
    return s.find('\0') != std::string::npos;
}

void reject_nul(const std::string& value, const char* key) {
    if (has_nul(value))
        throw std::runtime_error(std::string("request: \"") + key +
                                 "\" contains an embedded NUL");
}

bool generated_id(const std::string& value, const char* prefix) {
    constexpr size_t digits = 16;
    const size_t prefix_size = std::char_traits<char>::length(prefix);
    if (value.size() != prefix_size + digits ||
        value.compare(0, prefix_size, prefix) != 0)
        return false;
    for (size_t i = prefix_size; i < value.size(); ++i) {
        const char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

fs::path canonical_parent(const fs::path& path, const char* key) {
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(path, ec);
    if (ec)
        throw std::runtime_error(std::string("request: cannot canonicalize ") +
                                 key + ": " + ec.message());
    return canonical.parent_path();
}

bool is_auto_device(const std::string& device) {
    size_t begin = 0;
    size_t end = device.size();
    while (begin < end && std::isspace((unsigned char)device[begin])) ++begin;
    while (end > begin && std::isspace((unsigned char)device[end - 1])) --end;
    std::string lower = device.substr(begin, end - begin);
    for (char& c : lower)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return lower.empty() || lower == "auto" || lower == "-1";
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", (unsigned)(unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

}  // namespace

Request parse_request(const std::string& path) {
    if (has_nul(path))
        throw std::runtime_error("request: request path contains an embedded NUL");
    const std::string text = read_file(path);
    const JsonValue root = json_parse(text);
    if (root.type != JsonValue::Type::Object)
        throw std::runtime_error("request: root must be an object");

    Request r;
    r.request_path = path;
    r.schema_version = need_int(root, "schema_version");
    r.job_id      = need_string(root, "job_id");
    r.attempt_id  = need_string(root, "attempt_id");
    r.phase       = need_string(root, "phase");
    r.device      = need_string(root, "device");
    r.work_dir    = need_string(root, "work_dir");
    r.result_path = need_string(root, "result_path");
    const JsonValue* args = root.find("args");
    if (!args || args->type != JsonValue::Type::Array)
        throw std::runtime_error("request: \"args\" must be an array of strings");
    for (const JsonValue& a : args->arr) {
        if (a.type != JsonValue::Type::String)
            throw std::runtime_error("request: \"args\" element not a string");
        r.args.push_back(a.str);
    }
    return r;
}

void validate_request(const Request& r) {
    reject_nul(r.request_path, "request_path");
    reject_nul(r.job_id, "job_id");
    reject_nul(r.attempt_id, "attempt_id");
    reject_nul(r.phase, "phase");
    reject_nul(r.device, "device");
    reject_nul(r.work_dir, "work_dir");
    reject_nul(r.result_path, "result_path");
    for (const std::string& arg : r.args) reject_nul(arg, "args");

    if (r.schema_version != 1)
        throw std::runtime_error("request: unsupported schema_version " +
                                 std::to_string(r.schema_version));
    if (!generated_id(r.job_id, "job-"))
        throw std::runtime_error("request: invalid job_id");
    if (!generated_id(r.attempt_id, "att-"))
        throw std::runtime_error("request: invalid attempt_id");
    if (r.phase != "train" && r.phase != "sfm" && r.phase != "geometry")
        throw std::runtime_error("request: phase \"" + r.phase +
                                 "\" is not one of train/sfm/geometry");
    if (r.device.empty() || is_auto_device(r.device))
        throw std::runtime_error("request: device must be resolved");
    if (r.result_path.empty())
        throw std::runtime_error("request: result_path is empty");
    if (r.request_path.empty())
        throw std::runtime_error("request: request path is empty");

    const fs::path request_path = fs::u8path(r.request_path);
    const fs::path result_path = fs::u8path(r.result_path);
    const std::string expected_name = "result-" + r.attempt_id + ".json";
    if (result_path.filename().u8string() != expected_name)
        throw std::runtime_error("request: result filename does not match attempt_id");
    if (!result_path.is_absolute())
        throw std::runtime_error("request: result_path must be absolute");
    if (canonical_parent(request_path, "request_path") !=
        canonical_parent(result_path, "result_path"))
        throw std::runtime_error("request: result directory differs from request directory");
}

bool publish_result(const Request& r, const Result& res) {
    const fs::path target = fs::u8path(r.result_path);
    const fs::path tmp = target.parent_path() /
        (target.filename().u8string() + ".tmp-" + r.attempt_id);

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << "{\n"
          << "  \"schema_version\": 1,\n"
          << "  \"job_id\": \"" << json_escape(r.job_id) << "\",\n"
          << "  \"attempt_id\": \"" << json_escape(r.attempt_id) << "\",\n"
          << "  \"phase\": \"" << json_escape(r.phase) << "\",\n"
          << "  \"outcome\": \"" << json_escape(res.outcome) << "\",\n"
          << "  \"exit_code\": " << res.exit_code << ",\n"
          << "  \"message\": \"" << json_escape(res.message) << "\"\n"
          << "}\n";
        f.flush();
        if (!f) {
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        std::error_code ec2;
        fs::remove(tmp, ec2);
        return false;
    }
    return true;
}

}  // namespace app::worker
