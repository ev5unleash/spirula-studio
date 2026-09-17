// worker_request_test -- schema and output-boundary checks for worker requests.

#include "app/WorkerRequest.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition) {
        std::printf("ok   %s\n", message);
    } else {
        std::printf("FAIL %s\n", message);
        ++failures;
    }
}

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write worker request fixture");
    out << text;
}

std::string replace_once(std::string text, const std::string& from,
                         const std::string& to) {
    const size_t pos = text.find(from);
    if (pos == std::string::npos) throw std::runtime_error("fixture replacement failed");
    return text.replace(pos, from.size(), to);
}

bool accepts(const fs::path& path) {
    try {
        const app::worker::Request request =
            app::worker::parse_request(path.u8string());
        app::worker::validate_request(request);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool rejects(const fs::path& path, const std::string& text) {
    write_text(path, text);
    return !accepts(path);
}

}  // namespace

int main() {
    const std::string job_id = "job-0123456789abcdef";
    const std::string attempt_id = "att-0123456789abcdef";
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("spirula_worker_request_" + std::to_string(nonce));
    const fs::path request_path = root / ("request-" + attempt_id + ".json");
    const fs::path result_path = root / ("result-" + attempt_id + ".json");
    const std::string result_json_path = result_path.generic_u8string();
    const std::string valid =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"job_id\": \"" + job_id + "\",\n"
        "  \"attempt_id\": \"" + attempt_id + "\",\n"
        "  \"phase\": \"geometry\",\n"
        "  \"device\": \"uuid:0123456789abcdef0123456789abcdef\",\n"
        "  \"work_dir\": \"\",\n"
        "  \"result_path\": \"" + result_json_path + "\",\n"
        "  \"args\": [\"--check\", \"quoted value\"]\n"
        "}\n";

    try {
        fs::create_directories(root);
        write_text(request_path, valid);
        const app::worker::Request request =
            app::worker::parse_request(request_path.u8string());
        bool valid_request = true;
        try {
            app::worker::validate_request(request);
        } catch (const std::exception&) {
            valid_request = false;
        }
        check(valid_request, "accepts a valid schema-1 request");
        check(request.request_path == request_path.u8string(),
              "retains the request source path");

        check(rejects(request_path,
                       replace_once(valid, "\"schema_version\": 1",
                                    "\"schema_version\": \"1\"")),
              "rejects a non-numeric schema_version");
        check(rejects(request_path,
                       replace_once(valid,
                                    "\"device\": \"uuid:0123456789abcdef0123456789abcdef\"",
                                    "\"device\": 7")),
              "rejects a non-string device");
        check(rejects(request_path,
                       replace_once(valid, "\"work_dir\": \"\"",
                                    "\"work_dir\": false")),
              "rejects a non-string work_dir");
        check(rejects(request_path,
                       replace_once(valid, "  \"work_dir\": \"\",\n", "")),
              "rejects a missing work_dir");
        check(rejects(request_path,
                       replace_once(valid, "\"work_dir\": \"\"",
                                    "\"work_dir\": \"\\u0000\"")),
              "rejects an embedded NUL in a path");
        check(rejects(request_path,
                       replace_once(valid, "\"args\": [\"--check\", \"quoted value\"]",
                                    "\"args\": [3]")),
              "rejects a non-string args element");
        check(rejects(request_path,
                       replace_once(valid, "\"phase\": \"geometry\"",
                                    "\"phase\": \"shell\"")),
              "rejects an unsupported phase");
        check(rejects(request_path,
                       replace_once(valid, "\"job_id\": \"" + job_id + "\"",
                                    "\"job_id\": \"job-bad\"")),
              "rejects an unsafe job id");
        check(rejects(request_path,
                       replace_once(valid,
                                    "\"result_path\": \"" + result_json_path + "\"",
                                    "\"result_path\": \"" +
                                        (root / "result-other.json").generic_u8string() + "\"")),
              "rejects a result filename that does not match the attempt");
        check(rejects(request_path,
                       replace_once(valid,
                                    "\"result_path\": \"" + result_json_path + "\"",
                                    "\"result_path\": \"" +
                                        (root / ".." / "outside" /
                                         ("result-" + attempt_id + ".json")).generic_u8string() + "\"")),
              "rejects a result directory outside the request directory");
        check(rejects(request_path,
                       replace_once(valid,
                                    "\"device\": \"uuid:0123456789abcdef0123456789abcdef\"",
                                    "\"device\": \"uuid:0123456789abcdef0123456789abc\\u0000\"")),
              "rejects an embedded NUL in the device");
        check(rejects(request_path,
                       replace_once(valid, "\"quoted value\"",
                                    "\"bad\\u0000arg\"")),
              "rejects an embedded NUL in args");
    } catch (const std::exception& e) {
        std::printf("FAIL worker request test setup: %s\n", e.what());
        ++failures;
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    if (failures) {
        std::printf("FAILED with %d error(s)\n", failures);
        return 1;
    }
    std::printf("All worker request tests passed.\n");
    return 0;
}
