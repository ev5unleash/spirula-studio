// WorkerRequest -- one scheduled phase, JSON codec.
//
// Deliberately thin: the parser is data/Json.h, the writer is fprintf. Nothing
// here knows what the phases mean.

#include "app/WorkerRequest.h"

#include "data/Json.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
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
    if (!p || p->type != JsonValue::Type::String)
        throw std::runtime_error(std::string("request: missing \"") + key + "\"");
    return p->str;
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
    const std::string text = read_file(path);
    const JsonValue root = json_parse(text);
    if (root.type != JsonValue::Type::Object)
        throw std::runtime_error("request: root must be an object");

    Request r;
    const JsonValue* sv = root.find("schema_version");
    if (!sv) throw std::runtime_error("request: missing \"schema_version\"");
    r.schema_version = (int)sv->as_int(0);
    r.job_id      = need_string(root, "job_id");
    r.attempt_id  = need_string(root, "attempt_id");
    r.phase       = need_string(root, "phase");
    if (const JsonValue* d = root.find("device"))        r.device      = d->str;
    if (const JsonValue* w = root.find("work_dir"))      r.work_dir    = w->str;
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
    if (r.schema_version != 1)
        throw std::runtime_error("request: unsupported schema_version " +
                                 std::to_string(r.schema_version));
    if (r.job_id.empty() || r.attempt_id.empty())
        throw std::runtime_error("request: job_id and attempt_id must be non-empty");
    if (r.phase != "train" && r.phase != "sfm" && r.phase != "geometry")
        throw std::runtime_error("request: phase \"" + r.phase +
                                 "\" is not one of train/sfm/geometry");
    if (r.device.empty() || r.device == "auto")
        throw std::runtime_error("request: device must be resolved");
    if (r.result_path.empty())
        throw std::runtime_error("request: result_path is empty");
    const fs::path result_dir = fs::u8path(r.result_path).parent_path();
    if (!result_dir.empty()) {
        std::error_code ec;
        if (!fs::exists(result_dir, ec))
            throw std::runtime_error("request: result directory does not exist: " +
                                     result_dir.u8string());
    }
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
