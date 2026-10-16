#pragma once

#include "data/Json.h"
#include "data/JsonWrite.h"
#include "checkpoint/B1Contract.h"
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <functional>

namespace app {
inline void b1_publish_record(const std::filesystem::path& path, const std::string& text,
                              std::filesystem::path temp = {}) {
    if (temp.empty()) temp = std::filesystem::path(path.u8string() + ".pending");
    if (std::filesystem::exists(temp)) throw std::runtime_error("B1 pending publication already exists");
    std::ofstream output(temp, std::ios::binary);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    if (!output) throw std::runtime_error("B1 pending publication write failed: " + temp.u8string());
    std::filesystem::create_hard_link(temp, path);
    std::error_code cleanup_error;
    std::filesystem::remove(temp, cleanup_error);
}
inline void validate_b1_refresh_parent(const std::string& record_parent,
                                       const std::string& install_parent, const std::string& expected) {
    if (record_parent != expected || install_parent != expected)
        throw std::runtime_error("B1 refresh parent commit identity invalid");
}
inline std::string b1_group_view_policy(const JsonValue& json) {
    const auto version = json.find("version");
    if (!json.is_object() || !version || version->type != JsonValue::Type::Number ||
        (version->num != 1 && version->num != 2 && version->num != 3) ||
        json.obj.size() != (version->num == 1 ? 11 : version->num == 2 ? 12 : 13))
        throw std::runtime_error("B1 group version/fields invalid");
    for (size_t i = 0; i < json.obj.size(); ++i)
        for (size_t k = 0; k < i; ++k)
            if (json.obj[i].first == json.obj[k].first)
                throw std::runtime_error("B1 group duplicate field");
    for (const auto& field : json.obj)
        if (field.first != "version" && field.first != "source" && field.first != "views" &&
            field.first != "source_sha256" && field.first != "views_sha256" &&
            field.first != "config_sha256" && field.first != "shared_sha256" &&
            field.first != "source_step" && field.first != "target" &&
            field.first != "device0" && field.first != "device1" &&
            (version->num < 2 || field.first != "view_policy") &&
            (version->num != 3 || field.first != "refresh_cadence"))
            throw std::runtime_error("B1 group field not allowed");
    if (version->num == 1) return "partitioned";
    const auto policy = json.find("view_policy");
    if (!policy || policy->type != JsonValue::Type::String ||
        (policy->str != "partitioned" && policy->str != "full"))
        throw std::runtime_error("B1 group view policy invalid");
    if (version->num == 3) {
        const auto cadence = json.find("refresh_cadence");
        if (policy->str != "full" || !cadence || cadence->type != JsonValue::Type::Number ||
            !std::isfinite(cadence->num) || cadence->num < 1 || cadence->num > 2147483647 ||
            std::floor(cadence->num) != cadence->num)
            throw std::runtime_error("B1 refresh cadence/policy invalid");
    }
    return policy->str;
}
struct B1WorkerPayload {
    int owner = -1;
    int64_t target = 0;
    std::string source, views;
    std::string group, group_sha256;
    std::string refresh_attempt;
};
inline B1WorkerPayload parse_b1_worker_payload(const std::string& text) {
    const auto json = json_parse(text);
    const bool bound = json.find("version") != nullptr;
    const bool refresh = bound && json.find("version")->num == 3;
    if (!json.is_object() || json.obj.size() != (refresh ? 9 : bound ? 8 : 5) ||
        (bound && (json.find("version")->type != JsonValue::Type::Number ||
                   (json.find("version")->num != 2 && !refresh))))
        throw std::runtime_error("B1 worker payload fields invalid");
    for (size_t i = 0; i < json.obj.size(); ++i)
        for (size_t k = 0; k < i; ++k)
            if (json.obj[i].first == json.obj[k].first)
                throw std::runtime_error("B1 worker duplicate field");
    for (const auto& field : json.obj)
        if (field.first != "kind" && field.first != "owner" && field.first != "target" &&
            field.first != "source" && field.first != "views" &&
            (!bound || (field.first != "version" && field.first != "group" &&
                        field.first != "group_sha256" && (!refresh || field.first != "refresh_attempt"))))
            throw std::runtime_error("B1 worker payload field not allowed");
    const auto kind = json.find("kind"), owner = json.find("owner"), target = json.find("target");
    const auto source = json.find("source"), views = json.find("views");
    if (!kind || kind->as_string() != "b1" || !owner || !target || !source || !views ||
        owner->type != JsonValue::Type::Number || (owner->num != 0 && owner->num != 1) ||
        target->type != JsonValue::Type::Number || !std::isfinite(target->num) ||
        target->num < 1 || target->num > 2147483647 || std::floor(target->num) != target->num)
        throw std::runtime_error("B1 worker owner/target invalid");
    B1WorkerPayload out{static_cast<int>(owner->num), static_cast<int64_t>(target->num),
                        source->as_string(), views->as_string()};
    if (bound) {
        const auto group = json.find("group"), hash = json.find("group_sha256");
        if (!group || group->type != JsonValue::Type::String ||
            !hash || hash->type != JsonValue::Type::String || hash->str.size() != 64 ||
            hash->str.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error("B1 worker group binding invalid");
        out.group = group->str;
        out.group_sha256 = hash->str;
        if (refresh) {
            const auto attempt = json.find("refresh_attempt");
            if (!attempt || attempt->type != JsonValue::Type::String || attempt->str.empty() ||
                attempt->str.find('\0') != std::string::npos ||
                !std::filesystem::u8path(attempt->str).is_absolute())
                throw std::runtime_error("B1 refresh attempt invalid");
            out.refresh_attempt = attempt->str;
        }
    }
    for (const auto* path : {&out.source, &out.views})
        if (path->empty() || path->find('\0') != std::string::npos ||
            !std::filesystem::u8path(*path).is_absolute())
            throw std::runtime_error("B1 worker input path invalid");
    if (bound && (out.group.empty() || out.group.find('\0') != std::string::npos ||
                  !std::filesystem::u8path(out.group).is_absolute()))
        throw std::runtime_error("B1 worker group path invalid");
    return out;
}
inline std::string b1_worker_payload(const B1WorkerPayload& p) {
    JsonWriter json;
    json.object().field("kind", "b1").field("owner", p.owner)
        .field("target", static_cast<long long>(p.target))
        .field("source", p.source).field("views", p.views);
    if (!p.group.empty())
        json.field("version", p.refresh_attempt.empty() ? 2 : 3).field("group", p.group).field("group_sha256", p.group_sha256);
    if (!p.refresh_attempt.empty()) json.field("refresh_attempt", p.refresh_attempt);
    json.end();
    return json.str();
}
inline void validate_b1_group_completion(const ckpt::B1Contract& left,
                                         const ckpt::B1Contract& right, int64_t target) {
    ckpt::validate_b1_pair(left, right);
    if (left.owner != 0 || right.owner != 1 ||
        left.completed_step != target || right.completed_step != target)
        throw std::runtime_error("B1 group completion is not at the declared target");
}
int64_t b1_refresh_cadence();
std::filesystem::path b1_refresh_root();
std::string b1_refresh_group_hash();
bool b1_refresh_boundary(int64_t boundary, int64_t parent, const std::string& mask_sha256,
    std::string parent_commit_sha256,
    const std::function<std::string(const std::filesystem::path&, int64_t)>& export_payload,
    const std::function<void(const std::filesystem::path&, int64_t, const std::string&)>& install,
    const std::function<void(int64_t, const std::string&, const std::string&, const std::string&)>& commit,
    const std::function<std::filesystem::path(int64_t)>& save,
    const std::function<bool()>& stop_requested);
void configure_b1_worker(const B1WorkerPayload& payload);
int b1_worker_contract_version();
std::string b1_worker_view_policy(int owner, const std::string& source,
                                  const std::string& views, int64_t target);
int run_b1_group(int argc, char** argv, const std::string& executable,
                 const std::string& admission_dir);
}
