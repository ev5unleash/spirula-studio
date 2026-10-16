#include "app/B1Group.h"
#include "app/JobScheduler.h"
#include "app/OutputLease.h"
#include "app/TextFile.h"
#include "checkpoint/B1Contract.h"
#include "checkpoint/B1Merge.h"
#include "checkpoint/Resume.h"
#include "core/Env.h"
#include "i18n/catalog/Cli.h"
#include <array>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <thread>
#include <memory>
#include <map>
#include <set>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }
int read_control() {
    static std::string pending;
    char buffer[64];
    size_t count = 0;
#ifdef _WIN32
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD available = 0, received = 0;
    if (input && PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr) && available &&
        ReadFile(input, buffer, sizeof buffer, &received, nullptr))
        count = received;
#else
    pollfd input{STDIN_FILENO, POLLIN, 0};
    if (::poll(&input, 1, 0) > 0 && (input.revents & POLLIN)) {
        const auto received = ::read(STDIN_FILENO, buffer, sizeof buffer);
        if (received > 0) count = static_cast<size_t>(received);
    }
#endif
    pending.append(buffer, count);
    int action = 0;
    for (size_t end; (end = pending.find('\n')) != std::string::npos;) {
        const auto command = pending.substr(0, end);
        if (command == "STOP" || command == "STOP\r") action |= 1;
        if (command == "CANCEL" || command == "CANCEL\r") action |= 2;
        if (command == "STOP 0" || command == "STOP 0\r") action |= 4;
        if (command == "STOP 1" || command == "STOP 1\r") action |= 8;
        pending.erase(0, end + 1);
    }
    if (pending.size() > 64) pending.clear();
    return action;
}
void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
    if (_putenv_s(name, value.c_str())) throw std::runtime_error("B1 environment update failed");
#else
    if (setenv(name, value.c_str(), 1)) throw std::runtime_error("B1 environment update failed");
#endif
}
std::string required_string(const JsonValue& json, const char* key) {
    const auto value = json.find(key);
    if (!value || value->type != JsonValue::Type::String || value->str.empty() ||
        value->str.find('\0') != std::string::npos)
        throw std::runtime_error(std::string("B1 group field invalid: ") + key);
    return value->str;
}
using app::b1_publish_record;
struct Group {
    fs::path source, views;
    std::array<std::string, 2> devices;
    std::string source_hash, views_hash, config_hash, shared_hash;
    std::string view_policy = "partitioned";
    int version = 2;
    int64_t source_step = 0, target = 0;
    int64_t refresh_cadence = 0;
};
std::string manifest(const Group& g) {
    JsonWriter j;
    j.object().field("version", g.version).field("source", g.source.u8string())
        .field("views", g.views.u8string()).field("source_sha256", g.source_hash)
        .field("views_sha256", g.views_hash).field("config_sha256", g.config_hash)
        .field("shared_sha256", g.shared_hash)
        .field("source_step", static_cast<long long>(g.source_step))
        .field("target", static_cast<long long>(g.target))
        .field("device0", g.devices[0]).field("device1", g.devices[1]);
    if (g.version >= 2) j.field("view_policy", g.view_policy);
    if (g.version == 3) j.field("refresh_cadence", static_cast<long long>(g.refresh_cadence));
    j.end();
    return j.str();
}
Group read_group(const fs::path& root) {
    const auto j = json_parse_file((root / "group.json").u8string());
    Group g;
    g.view_policy = app::b1_group_view_policy(j);
    g.version = j.find("version")->as_int();
    g.source = fs::u8path(required_string(j, "source"));
    g.views = fs::u8path(required_string(j, "views"));
    g.devices = {required_string(j, "device0"), required_string(j, "device1")};
    g.source_hash = required_string(j, "source_sha256");
    g.views_hash = required_string(j, "views_sha256");
    g.config_hash = required_string(j, "config_sha256");
    g.shared_hash = required_string(j, "shared_sha256");
    for (const auto* path : {&g.source, &g.views})
        if (!path->is_absolute()) throw std::runtime_error("B1 group input path invalid");
    for (const auto* hash : {&g.source_hash, &g.views_hash, &g.config_hash, &g.shared_hash})
        if (hash->size() != 64 || hash->find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error("B1 group digest invalid");
    for (const char* key : {"source_step", "target"}) {
        const auto n = j.find(key);
        if (!n || n->type != JsonValue::Type::Number || !std::isfinite(n->num) ||
            n->num < 0 || n->num > 2147483647 || std::floor(n->num) != n->num)
            throw std::runtime_error("B1 group schedule invalid");
    }
    g.source_step = j.find("source_step")->as_int();
    g.target = j.find("target")->as_int();
    if (g.target <= g.source_step) throw std::runtime_error("B1 group target invalid");
    if (g.version == 3) {
        g.refresh_cadence = j.find("refresh_cadence")->as_int();
        if ((g.target - g.source_step) % g.refresh_cadence)
            throw std::runtime_error("B1 refresh target is not cadence aligned");
    }
    return g;
}
void validate_inputs(const Group& g) {
    ckpt::check_resumable(g.source);
    if (ckpt::read_b1_contract(g.source)) throw std::runtime_error("B1 group source is already an owner");
    const auto resolved = ckpt::resolve_checkpoint(g.source);
    const auto config = ckpt::config_from_json(resolved.run_dir / "config.json");
    if (ckpt::b1_file_hash(g.source / "state.tar") != g.source_hash ||
        ckpt::b1_file_hash(g.views) != g.views_hash ||
        ckpt::b1_config_hash(config) != g.config_hash ||
        ckpt::b1_shared_hash(g.source) != g.shared_hash ||
        ckpt::read_state_json(g.source).get_double("step", -1) != g.source_step ||
        g.target > config.num_iterations)
        throw std::runtime_error("B1 group immutable inputs changed");
    auto devices = g.devices;
    for (auto& device : devices) {
        std::transform(device.begin(), device.end(), device.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (device.rfind("uuid:", 0) != 0 || device.size() != 37 ||
            device.find_first_not_of("0123456789abcdef", 5) != std::string::npos)
            throw std::runtime_error("B1 group device must be an explicit Vulkan UUID");
    }
    if (devices[0] == devices[1]) throw std::runtime_error("B1 group needs distinct devices");
}
struct Owner {
    fs::path checkpoint;
    std::optional<ckpt::B1Contract> contract;
};
std::array<Owner, 2> collect_refresh(const fs::path& root, const Group& g);
std::string read_permission(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::string hash, extra;
    if (!(input >> hash) || (input >> extra)) throw std::runtime_error("B1 refresh permission invalid");
    return hash;
}
std::string digest(const JsonValue& j, const char* key) {
    const auto value = required_string(j, key);
    if (value.size() != 64 || value.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::runtime_error("B1 refresh digest invalid");
    return value;
}
void refresh_identity(const JsonValue& j, const Group& g, const std::string& hash, int64_t boundary) {
    if (!j.is_object() || j.get_double("generation", -1) != boundary ||
        j.get_double("parent", -1) != boundary - g.refresh_cadence ||
        required_string(j, "group_sha256") != hash ||
        required_string(j, "source_sha256") != g.source_hash ||
        required_string(j, "view_policy") != g.view_policy)
        throw std::runtime_error("B1 refresh generation identity invalid");
    for (size_t i = 0; i < j.obj.size(); ++i)
        for (size_t k = 0; k < i; ++k)
            if (j.obj[i].first == j.obj[k].first)
                throw std::runtime_error("B1 refresh duplicate field");
}
JsonWriter refresh_record(const Group& g, const std::string& hash, int64_t boundary, const std::string& parent_commit) {
    JsonWriter j;
    j.object().field("generation", static_cast<long long>(boundary))
        .field("parent", static_cast<long long>(boundary - g.refresh_cadence))
        .field("parent_commit_sha256", parent_commit)
        .field("group_sha256", hash).field("source_sha256", g.source_hash)
        .field("view_policy", g.view_policy);
    return j;
}
std::string previous_commit(const fs::path& root, const Group& g, int64_t boundary) {
    if (boundary - g.refresh_cadence == g.source_step) return ckpt::b1_file_hash(root / "group.json");
    return digest(json_parse_file((root / "refresh-commits" /
        (std::to_string(boundary - g.refresh_cadence) + ".json")).u8string()), "commit_sha256");
}
std::array<Owner, 2> committed_pair(const fs::path& root, const Group& g,
                                  const JsonValue& record, int64_t boundary) {
    const auto hash = ckpt::b1_file_hash(root / "group.json");
    refresh_identity(record, g, hash, boundary);
    const auto install_path = fs::u8path(required_string(record, "install"));
    if (!install_path.is_absolute() ||
        !app::sched::path_claims_conflict({root.u8string(), true}, {install_path.u8string(), false}))
        throw std::runtime_error("B1 refresh install outside group");
    const auto install = json_parse_file(install_path.u8string());
    refresh_identity(install, g, hash, boundary);
    const auto parent_commit = previous_commit(root, g, boundary);
    app::validate_b1_refresh_parent(digest(record, "parent_commit_sha256"),
        digest(install, "parent_commit_sha256"), parent_commit);
    const auto commit_hash = ckpt::b1_file_hash(install_path);
    if (digest(record, "commit_sha256") != commit_hash)
        throw std::runtime_error("B1 refresh commit identity changed");
    std::array<Owner, 2> owners;
    for (int n = 0; n < 2; ++n) {
        const auto suffix = std::to_string(n);
        const auto path = fs::u8path(required_string(record, ("owner" + suffix).c_str()));
        if (!path.is_absolute() ||
            !app::sched::path_claims_conflict({root.u8string(), true}, {path.u8string(), false}) ||
            path.parent_path().filename() != "owner-" + suffix)
            throw std::runtime_error("B1 refresh checkpoint outside owner run");
        ckpt::check_resumable(path);
        auto c = ckpt::read_b1_contract(path);
        if (!c || c->version != 3 || c->owner != n || c->completed_step != boundary ||
            c->source_step != g.source_step || c->source_state_sha256 != g.source_hash ||
            c->views_sha256 != g.views_hash || c->config_sha256 != g.config_hash ||
            c->frozen_shared_sha256 != g.shared_hash || c->view_policy != g.view_policy ||
            c->refresh_cadence != g.refresh_cadence || c->refresh_generation != boundary ||
            c->refresh_parent != boundary - g.refresh_cadence || c->refresh_group_sha256 != hash ||
            c->refresh_commit_sha256 != commit_hash ||
            c->refresh_payload0_sha256 != digest(install, "payload0_sha256") ||
            c->refresh_payload1_sha256 != digest(install, "payload1_sha256") ||
            c->mask_sha256 != digest(install, ("mask" + suffix + "_sha256").c_str()) ||
            ckpt::b1_file_hash(path / "state.tar") != digest(record, ("state" + suffix + "_sha256").c_str()))
            throw std::runtime_error("B1 refresh checkpoint pair identity invalid");
        owners[n] = {path, std::move(c)};
    }
    ckpt::validate_b1_pair(*owners[0].contract, *owners[1].contract);
    return owners;
}
std::array<Owner, 2> collect_refresh(const fs::path& root, const Group& g) {
    std::array<Owner, 2> owners;
    const auto commits = root / "refresh-commits";
    if (!fs::exists(commits)) return owners;
    std::map<int64_t, fs::path> ordered;
    for (const auto& entry : fs::directory_iterator(commits)) {
        if (entry.path().extension() == ".pending") continue; // Unpublished records confer no recovery authority.
        if (entry.path().extension() != ".json") throw std::runtime_error("B1 refresh commit filename invalid");
        const auto j = json_parse_file(entry.path().u8string());
        const auto generation = j.get_double("generation", -1);
        if (!std::isfinite(generation) || std::floor(generation) != generation ||
            generation <= g.source_step || generation > g.target ||
            entry.path().filename() != std::to_string(static_cast<int64_t>(generation)) + ".json" ||
            !ordered.emplace(static_cast<int64_t>(generation), entry.path()).second)
            throw std::runtime_error("B1 refresh stale or duplicate commit");
    }
    int64_t expected = g.source_step + g.refresh_cadence;
    for (const auto& item : ordered) {
        if (item.first != expected) throw std::runtime_error("B1 refresh partial generation lineage");
        const auto record = json_parse_file(item.second.u8string());
        const auto hash = ckpt::b1_file_hash(root / "group.json");
        refresh_identity(record, g, hash, item.first);
        const auto install_path = fs::u8path(required_string(record, "install"));
        if (!install_path.is_absolute() ||
            !app::sched::path_claims_conflict({root.u8string(), true}, {install_path.u8string(), false}) ||
            ckpt::b1_file_hash(install_path) != digest(record, "commit_sha256"))
            throw std::runtime_error("B1 refresh compact lineage identity invalid");
        const auto install = json_parse_file(install_path.u8string());
        refresh_identity(install, g, hash, item.first);
        const auto parent_commit = previous_commit(root, g, item.first);
        app::validate_b1_refresh_parent(digest(record, "parent_commit_sha256"),
            digest(install, "parent_commit_sha256"), parent_commit);
        for (const char* key : {"payload0_sha256", "payload1_sha256", "mask0_sha256", "mask1_sha256"})
            digest(install, key);
        if (item.first == ordered.rbegin()->first) owners = committed_pair(root, g, record, item.first);
        expected += g.refresh_cadence;
    }
    return owners;
}
void remove_owned_refresh_output(const fs::path& root, const fs::path& path) {
    const auto relative = path.lexically_relative(root);
    if (relative.empty() || relative.is_absolute() || *relative.begin() != "attempts")
        throw std::runtime_error("B1 refresh cleanup outside owned attempts");
    auto check = [](const fs::path& entry) {
        if (fs::is_symlink(fs::symlink_status(entry)))
            throw std::runtime_error("B1 refresh cleanup refuses symlink");
#ifdef _WIN32
        const DWORD attributes = GetFileAttributesW(entry.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("B1 refresh cleanup refuses reparse path");
#endif
        if (fs::is_regular_file(entry) && fs::hard_link_count(entry) != 1)
            throw std::runtime_error("B1 refresh cleanup refuses foreign hardlink");
        if (!fs::is_regular_file(entry) && !fs::is_directory(entry))
            throw std::runtime_error("B1 refresh cleanup refuses special file");
    };
    fs::path ancestor = root;
    check(ancestor);
    for (const auto& part : relative) {
        if (part == "..") throw std::runtime_error("B1 refresh cleanup traversal refused");
        ancestor /= part;
        if (!fs::exists(ancestor)) return;
        check(ancestor);
    }
    std::vector<fs::path> entries;
    if (fs::is_directory(path))
        for (const auto& entry : fs::recursive_directory_iterator(path)) {
            check(entry.path());
            entries.push_back(entry.path());
        }
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) fs::remove(*it);
    fs::remove(path);
}
void prune_refresh_outputs(const fs::path& root, const Group& g, int64_t boundary) {
    // Peak before pruning: old+new checkpoint pairs, old+new payload pairs, and
    // native checkpoint staging. Admission must budget coexistence, not steady state.
    const auto latest = root / "refresh-commits" / (std::to_string(boundary) + ".json");
    const auto retained = committed_pair(root, g, json_parse_file(latest.u8string()), boundary);
    const auto generation = boundary - g.refresh_cadence;
    if (generation == g.source_step) return;
    {
        const auto record = json_parse_file((root / "refresh-commits" / (std::to_string(generation) + ".json")).u8string());
        refresh_identity(record, g, ckpt::b1_file_hash(root / "group.json"), generation);
        const auto install_path = fs::u8path(required_string(record, "install"));
        if (ckpt::b1_file_hash(install_path) != digest(record, "commit_sha256"))
            throw std::runtime_error("B1 refresh cleanup lineage changed");
        for (int n = 0; n < 2; ++n) {
            const auto checkpoint = fs::u8path(required_string(record, ("owner" + std::to_string(n)).c_str()));
            if (checkpoint.extension() != ".ckpt" || checkpoint.parent_path().filename() != "owner-" + std::to_string(n))
                throw std::runtime_error("B1 refresh cleanup checkpoint ownership invalid");
            if (checkpoint == retained[0].checkpoint || checkpoint == retained[1].checkpoint)
                throw std::runtime_error("B1 refresh cleanup refuses retained pair");
            if (fs::exists(checkpoint / "state.tar") &&
                ckpt::b1_file_hash(checkpoint / "state.tar") != digest(record, ("state" + std::to_string(n) + "_sha256").c_str()))
                throw std::runtime_error("B1 refresh cleanup checkpoint identity changed");
            remove_owned_refresh_output(root, checkpoint);
            remove_owned_refresh_output(root, install_path.parent_path() / ("owner-" + std::to_string(n) + ".payload"));
        }
    }
}
void prune_abandoned_refresh_attempts(const fs::path& root, const std::array<Owner, 2>& retained) {
    if (!fs::exists(root / "attempts")) return;
    for (const auto& attempt : fs::directory_iterator(root / "attempts")) {
        if (!fs::is_regular_file(attempt.path() / "scheduler" / "job-state.json")) continue;
        for (int n = 0; n < 2; ++n) {
            const auto run = attempt.path() / ("owner-" + std::to_string(n));
            if (!fs::is_directory(run)) continue;
            std::vector<fs::path> obsolete;
            for (const auto& entry : fs::directory_iterator(run)) {
                const auto path = entry.path();
                if (path == retained[0].checkpoint || path == retained[1].checkpoint) continue;
                const auto name = path.filename().u8string();
                const bool checkpoint = name.size() == 19 && name.rfind("step-", 0) == 0 &&
                    name.substr(5, 9).find_first_not_of("0123456789") == std::string::npos &&
                    name.substr(14) == ".ckpt";
                const bool staging = name.rfind(".staging-step-", 0) == 0 &&
                    name.find(".ckpt-", 14) != std::string::npos;
                if (checkpoint || staging) obsolete.push_back(path);
            }
            for (const auto& path : obsolete) remove_owned_refresh_output(root, path);
        }
        const auto refresh = attempt.path() / "refresh";
        if (!fs::is_directory(refresh)) continue;
        for (const auto& generation : fs::directory_iterator(refresh))
            for (int n = 0; n < 2; ++n)
                remove_owned_refresh_output(root, generation.path() / ("owner-" + std::to_string(n) + ".payload"));
    }
}
bool coordinate_refresh(const fs::path& root, const fs::path& attempt, const Group& g,
                        const std::string& hash, int64_t boundary) {
    const auto parent_commit = previous_commit(root, g, boundary);
    const auto dir = attempt / "refresh" / std::to_string(boundary);
    if (!fs::exists(dir / "owner-0.ready.json") || !fs::exists(dir / "owner-1.ready.json")) return false;
    if (!fs::exists(dir / "install.json")) {
        auto install = refresh_record(g, hash, boundary, parent_commit);
        for (int n = 0; n < 2; ++n) {
            const auto suffix = std::to_string(n);
            const auto ready = json_parse_file((dir / ("owner-" + suffix + ".ready.json")).u8string());
            refresh_identity(ready, g, hash, boundary);
            if (digest(ready, "parent_commit_sha256") != parent_commit)
                throw std::runtime_error("B1 refresh owner parent lineage changed");
            if (ready.get_double("owner", -1) != n ||
                ckpt::b1_file_hash(dir / ("owner-" + suffix + ".payload")) != digest(ready, "payload_sha256"))
                throw std::runtime_error("B1 refresh payload identity invalid");
            install.field(("payload" + suffix + "_sha256").c_str(), digest(ready, "payload_sha256"))
                .field(("mask" + suffix + "_sha256").c_str(), digest(ready, "mask_sha256"));
        }
        install.end();
        b1_publish_record(dir / "install.json", install.str());
    }
    const auto commit_hash = ckpt::b1_file_hash(dir / "install.json");
    for (int n = 0; n < 2; ++n) {
        const auto path = dir / ("owner-" + std::to_string(n) + ".installed.json");
        if (!fs::exists(path)) return false;
        const auto ack = json_parse_file(path.u8string());
        refresh_identity(ack, g, hash, boundary);
        if (ack.get_double("owner", -1) != n || digest(ack, "commit_sha256") != commit_hash)
            throw std::runtime_error("B1 refresh install acknowledgement invalid");
    }
    if (!fs::exists(dir / "save.json")) b1_publish_record(dir / "save.json", commit_hash);
    auto record = refresh_record(g, hash, boundary, parent_commit);
    record.field("install", (dir / "install.json").u8string()).field("commit_sha256", commit_hash);
    for (int n = 0; n < 2; ++n) {
        const auto suffix = std::to_string(n);
        const auto marker = dir / ("owner-" + suffix + ".checkpoint.json");
        if (!fs::exists(marker)) return false;
        const auto checkpoint = json_parse_file(marker.u8string());
        refresh_identity(checkpoint, g, hash, boundary);
        if (checkpoint.get_double("owner", -1) != n || digest(checkpoint, "commit_sha256") != commit_hash)
            throw std::runtime_error("B1 refresh checkpoint acknowledgement invalid");
        record.field(("owner" + suffix).c_str(), required_string(checkpoint, "checkpoint"))
            .field(("state" + suffix + "_sha256").c_str(), digest(checkpoint, "state_sha256"));
    }
    record.end();
    committed_pair(root, g, json_parse(record.str()), boundary);
    fs::create_directories(root / "refresh-commits");
    b1_publish_record(root / "refresh-commits" / (std::to_string(boundary) + ".json"), record.str(), dir / "commit.pending");
    b1_publish_record(dir / "release.json", commit_hash);
    prune_refresh_outputs(root, g, boundary);
    return true;
}
std::array<Owner, 2> collect(const fs::path& root, const Group& g) {
    std::array<Owner, 2> owners;
    if (g.version == 3) return collect_refresh(root, g);
    const auto attempts = root / "attempts";
    if (!fs::exists(attempts)) return owners;
    for (const auto& entry : fs::recursive_directory_iterator(attempts)) {
        if (!entry.is_directory() || entry.path().extension() != ".ckpt") continue;
        ckpt::check_resumable(entry.path());
        auto c = ckpt::read_b1_contract(entry.path());
        if (!c || (c->owner != 0 && c->owner != 1) ||
            c->source_state_sha256 != g.source_hash || c->views_sha256 != g.views_hash ||
            c->config_sha256 != g.config_hash || c->frozen_shared_sha256 != g.shared_hash ||
            c->source_step != g.source_step || c->completed_step < g.source_step ||
            c->completed_step > g.target || c->view_policy != g.view_policy ||
            c->version != g.version)
            throw std::runtime_error("B1 group checkpoint is incompatible or stale");
        if (entry.path().parent_path().filename() != "owner-" + std::to_string(c->owner))
            throw std::runtime_error("B1 checkpoint is in the wrong owner output");
        auto& owner = owners[c->owner];
        if (owner.contract) {
            const auto& previous = *owner.contract;
            if (previous.train_indices_sha256 != c->train_indices_sha256 ||
                previous.mask_sha256 != c->mask_sha256 || previous.frozen_context != c->frozen_context ||
                previous.rows != c->rows || previous.horizon != c->horizon ||
                previous.sampler_version != c->sampler_version ||
                previous.view_policy != c->view_policy ||
                previous.full_train_indices_sha256 != c->full_train_indices_sha256 ||
                previous.version != c->version || previous.completed_step == c->completed_step)
                throw std::runtime_error("B1 group duplicate or changed owner checkpoint");
            if (previous.completed_step > c->completed_step) continue;
        }
        owner = {entry.path(), std::move(c)};
    }
    return owners;
}
bool terminal(app::sched::JobState state) {
    using S = app::sched::JobState;
    return state == S::Succeeded || state == S::Stopped || state == S::Failed ||
           state == S::Interrupted || state == S::Blocked;
}
void validate_attempt_bindings(const fs::path& root, const Group& g,
                               const fs::path& group_path, const std::string& group_hash) {
    if (!fs::exists(root / "attempts")) return;
    for (const auto& entry : fs::directory_iterator(root / "attempts")) {
        if (!entry.is_directory()) continue;
        const auto state = entry.path() / "scheduler" / "job-state.json";
        if (!fs::exists(state)) continue;
        const auto json = json_parse_file(state.u8string());
        const auto jobs = json.find("jobs");
        if (!jobs || jobs->type != JsonValue::Type::Array)
            throw std::runtime_error("B1 group prior scheduler identity invalid");
        for (const auto& job : jobs->arr) {
            const auto phases = job.find("phases");
            if (!phases || phases->type != JsonValue::Type::Array)
                throw std::runtime_error("B1 group prior worker identity invalid");
            for (const auto& phase : phases->arr) {
                const auto payload = app::parse_b1_worker_payload(required_string(phase, "payload"));
                if (fs::u8path(payload.source) != g.source || fs::u8path(payload.views) != g.views ||
                    payload.target != g.target ||
                    ((g.version == 3) != !payload.refresh_attempt.empty()) ||
                    (g.version == 3 && fs::u8path(payload.refresh_attempt) != entry.path()) ||
                    (payload.group.empty() ? g.version != 1 :
                     (fs::u8path(payload.group) != group_path || payload.group_sha256 != group_hash)))
                    throw std::runtime_error("B1 group identity differs from prior worker binding");
            }
        }
    }
}
}

std::string app::b1_worker_view_policy(int owner, const std::string& source,
                                      const std::string& views, int64_t target) {
    if (std::getenv("SS_B1_VIEW_POLICY") || std::getenv("SSPLAT_B1_VIEW_POLICY"))
        throw std::runtime_error("B1 view policy environment overrides are not allowed");
    const char* path = spirula::env("B1_GROUP");
    const char* hash = spirula::env("B1_GROUP_SHA256");
    if (!path && !hash) return owner == 2 ? "full" : "partitioned";
    if (!path || !hash || owner < 0 || owner > 1 ||
        !fs::u8path(path).is_absolute() || fs::u8path(path).filename() != "group.json" ||
        ckpt::b1_file_hash(fs::u8path(path)) != hash)
        throw std::runtime_error("B1 worker group binding changed");
    const auto g = read_group(fs::u8path(path).parent_path());
    validate_inputs(g);
    const auto resolved = ckpt::resolve_checkpoint(g.source);
    const auto horizon = ckpt::config_from_json(resolved.run_dir / "config.json").num_iterations;
    if (fs::u8path(source) != g.source || fs::u8path(views) != g.views ||
        (target ? target != g.target : g.target != horizon))
        throw std::runtime_error("B1 worker inputs differ from immutable group");
    return g.view_policy;
}

int app::b1_worker_contract_version() {
    const char* path = spirula::env("B1_GROUP");
    const char* hash = spirula::env("B1_GROUP_SHA256");
    if (!path && !hash) return 2;
    if (!path || !hash || !fs::u8path(path).is_absolute() ||
        fs::u8path(path).filename() != "group.json" || ckpt::b1_file_hash(fs::u8path(path)) != hash)
        throw std::runtime_error("B1 worker group binding changed");
    return read_group(fs::u8path(path).parent_path()).version;
}

int64_t app::b1_refresh_cadence() {
    if (!spirula::env("B1_REFRESH_ATTEMPT")) return 0;
    const auto group = spirula::env("B1_GROUP");
    if (!group || !spirula::env("B1_GROUP_SHA256"))
        throw std::runtime_error("B1 refresh group binding missing");
    if (ckpt::b1_file_hash(fs::u8path(group)) != b1_refresh_group_hash())
        throw std::runtime_error("B1 refresh group binding changed");
    return read_group(fs::u8path(group).parent_path()).refresh_cadence;
}
fs::path app::b1_refresh_root() {
    const auto path = spirula::env("B1_REFRESH_ATTEMPT");
    if (!path) throw std::runtime_error("B1 refresh attempt missing");
    return fs::u8path(path);
}
std::string app::b1_refresh_group_hash() {
    const auto hash = spirula::env("B1_GROUP_SHA256");
    if (!hash) throw std::runtime_error("B1 refresh group hash missing");
    return hash;
}
bool app::b1_refresh_boundary(int64_t boundary, int64_t parent, const std::string& mask_sha256,
    std::string parent_commit,
    const std::function<std::string(const fs::path&, int64_t)>& export_payload,
    const std::function<void(const fs::path&, int64_t, const std::string&)>& install,
    const std::function<void(int64_t, const std::string&, const std::string&, const std::string&)>& commit,
    const std::function<fs::path(int64_t)>& save,
    const std::function<bool()>& stop_requested) {
    const auto attempt = b1_refresh_root();
    if (!spirula::env("B1_GROUP") || !spirula::env("B1_OWNER"))
        throw std::runtime_error("B1 refresh worker identity missing");
    const auto root = fs::u8path(spirula::env("B1_GROUP")).parent_path();
    const auto g = read_group(root);
    if (previous_commit(root, g, boundary) != parent_commit)
        throw std::runtime_error("B1 refresh engine parent lineage differs from committed pair");
    const auto hash = b1_refresh_group_hash();
    const int owner = std::stoi(spirula::env("B1_OWNER"));
    if (g.version != 3 || owner < 0 || owner > 1 || boundary != parent + g.refresh_cadence ||
        boundary > g.target || boundary <= g.source_step ||
        (boundary - g.source_step) % g.refresh_cadence || ckpt::b1_file_hash(root / "group.json") != hash)
        throw std::runtime_error("B1 refresh boundary invalid");
    const auto dir = attempt / "refresh" / std::to_string(boundary);
    fs::create_directories(dir);
    const auto prefix = "owner-" + std::to_string(owner);
    const auto started = std::chrono::steady_clock::now();
    const auto payload = dir / (prefix + ".payload");
    const auto payload_hash = export_payload(payload, boundary);
    const auto exported = std::chrono::steady_clock::now();
    if (ckpt::b1_file_hash(payload) != payload_hash) throw std::runtime_error("B1 refresh export digest mismatch");
    auto ready = refresh_record(g, hash, boundary, parent_commit);
    ready.field("owner", owner).field("payload_sha256", payload_hash).field("mask_sha256", mask_sha256).end();
    b1_publish_record(dir / (prefix + ".ready.json"), ready.str());
    auto wait_for = [&](const fs::path& path) {
        while (!fs::exists(path)) {
            if (stop_requested() || fs::exists(attempt / "refresh.abort")) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return !stop_requested() && !fs::exists(attempt / "refresh.abort");
    };
    if (!wait_for(dir / "install.json")) return false;
    const auto identity = json_parse_file((dir / "install.json").u8string());
    refresh_identity(identity, g, hash, boundary);
    if (digest(identity, "parent_commit_sha256") != parent_commit)
        throw std::runtime_error("B1 refresh install parent lineage changed");
    const auto hash0 = digest(identity, "payload0_sha256"), hash1 = digest(identity, "payload1_sha256");
    const auto own_hash = owner == 0 ? hash0 : hash1;
    if (own_hash != payload_hash || digest(identity, ("mask" + std::to_string(owner) + "_sha256").c_str()) != mask_sha256)
        throw std::runtime_error("B1 refresh owner identity changed");
    const auto peer = dir / ("owner-" + std::to_string(1 - owner) + ".payload");
    const auto peer_hash = owner == 0 ? hash1 : hash0;
    if (ckpt::b1_file_hash(peer) != peer_hash) throw std::runtime_error("B1 refresh peer payload changed");
    const auto install_started = std::chrono::steady_clock::now();
    install(peer, boundary, peer_hash);
    const auto installed = std::chrono::steady_clock::now();
    const auto commit_hash = ckpt::b1_file_hash(dir / "install.json");
    commit(boundary, hash0, hash1, commit_hash);
    auto ack = refresh_record(g, hash, boundary, parent_commit);
    ack.field("owner", owner).field("commit_sha256", commit_hash).end();
    b1_publish_record(dir / (prefix + ".installed.json"), ack.str());
    if (!wait_for(dir / "save.json")) return false;
    if (read_permission(dir / "save.json") != commit_hash)
        throw std::runtime_error("B1 refresh save permission changed");
    const auto save_started = std::chrono::steady_clock::now();
    const auto checkpoint = save(boundary);
    const auto saved_at = std::chrono::steady_clock::now();
    auto saved = refresh_record(g, hash, boundary, parent_commit);
    saved.field("owner", owner).field("commit_sha256", commit_hash)
        .field("checkpoint", checkpoint.u8string())
        .field("state_sha256", ckpt::b1_file_hash(checkpoint / "state.tar")).end();
    b1_publish_record(dir / (prefix + ".checkpoint.json"), saved.str());
    if (!wait_for(dir / "release.json")) return false;
    if (read_permission(dir / "release.json") != commit_hash)
        throw std::runtime_error("B1 refresh release identity changed");
    const auto committed = json_parse_file((root / "refresh-commits" / (std::to_string(boundary) + ".json")).u8string());
    committed_pair(root, g, committed, boundary);
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::fprintf(stderr, "B1_REFRESH owner=%d generation=%lld export_bytes=%llu install_bytes=%llu export_seconds=%.9f install_seconds=%.9f checkpoint_seconds=%.9f wall_seconds=%.9f commit=%s\n",
        owner, static_cast<long long>(boundary), static_cast<unsigned long long>(fs::file_size(payload)),
        static_cast<unsigned long long>(fs::file_size(peer)),
        std::chrono::duration<double>(exported - started).count(),
        std::chrono::duration<double>(installed - install_started).count(),
        std::chrono::duration<double>(saved_at - save_started).count(), seconds, commit_hash.c_str());
    return true;
}

void app::configure_b1_worker(const B1WorkerPayload& p) {
    if (std::getenv("SS_B1_VIEW_POLICY") || std::getenv("SSPLAT_B1_VIEW_POLICY") ||
        spirula::env("B1_REFRESH_ATTEMPT") ||
        spirula::env("B1_REFRESH_CADENCE") ||
        spirula::env("B1_GROUP") || spirula::env("B1_GROUP_SHA256"))
        throw std::runtime_error("B1 inherited group/policy overrides are not allowed");
    if (!p.group.empty()) {
        if (fs::u8path(p.group).filename() != "group.json" ||
            ckpt::b1_file_hash(fs::u8path(p.group)) != p.group_sha256)
            throw std::runtime_error("B1 worker group binding changed");
        const auto g = read_group(fs::u8path(p.group).parent_path());
        validate_inputs(g);
        if (p.owner < 0 || p.owner > 1 || fs::u8path(p.source) != g.source ||
            fs::u8path(p.views) != g.views || p.target != g.target)
            throw std::runtime_error("B1 worker inputs differ from immutable group");
        if ((g.version == 3) != !p.refresh_attempt.empty() ||
            (!p.refresh_attempt.empty() &&
             (fs::u8path(p.refresh_attempt).parent_path().parent_path() != fs::u8path(p.group).parent_path() ||
              fs::u8path(p.refresh_attempt).parent_path().filename() != "attempts" ||
              !fs::is_directory(fs::u8path(p.refresh_attempt)))))
            throw std::runtime_error("B1 refresh worker attempt binding invalid");
        if (!p.refresh_attempt.empty()) set_env("SS_B1_REFRESH_ATTEMPT", p.refresh_attempt);
        set_env("SS_B1_GROUP", p.group);
        set_env("SS_B1_GROUP_SHA256", p.group_sha256);
    }
    constexpr uint64_t default_limit = uint64_t{24} << 30;
    uint64_t limit = default_limit;
    if (const char* inherited = spirula::env("WORKER_DEVICE_LIMIT_BYTES")) {
        const std::string text(inherited);
        if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
            throw std::runtime_error("SS_WORKER_DEVICE_LIMIT_BYTES must be positive bytes");
        const uint64_t parsed = std::stoull(text);
        if (!parsed) throw std::runtime_error("SS_WORKER_DEVICE_LIMIT_BYTES must be positive bytes");
        limit = std::min(default_limit, parsed);
    }
    set_env("SS_WORKER_DEVICE_LIMIT_BYTES", std::to_string(limit));
    std::fprintf(stderr, "[worker-backing] B1 owner=%d bound_limit=%llu default_limit=%llu\n",
                 p.owner, (unsigned long long)limit, (unsigned long long)default_limit);
    ckpt::check_resumable(fs::u8path(p.source));
    if (!fs::is_regular_file(fs::u8path(p.views))) throw std::runtime_error("B1 views missing");
    set_env("SS_B1_OWNER", std::to_string(p.owner));
    set_env("SS_B1_SOURCE", p.source);
    set_env("SS_B1_VIEWS", p.views);
    const auto resolved = ckpt::resolve_checkpoint(fs::u8path(p.source));
    const auto config = ckpt::config_from_json(resolved.run_dir / "config.json");
    if (p.target > config.num_iterations) throw std::runtime_error("B1 target exceeds original horizon");
    if (p.target < config.num_iterations) {
        set_env("SS_B1_STOP_AT_STEP", std::to_string(p.target));
    } else {
#ifdef _WIN32
        _putenv_s("SS_B1_STOP_AT_STEP", "");
        _putenv_s("SSPLAT_B1_STOP_AT_STEP", "");
#else
        unsetenv("SS_B1_STOP_AT_STEP");
        unsetenv("SSPLAT_B1_STOP_AT_STEP");
#endif
    }
#ifdef _WIN32
    _putenv_s("SS_B1_EVAL_PLY", "");
    _putenv_s("SSPLAT_B1_EVAL_PLY", "");
#else
    unsetenv("SS_B1_EVAL_PLY");
    unsetenv("SSPLAT_B1_EVAL_PLY");
#endif
}

int app::run_b1_group(int argc, char** argv, const std::string& executable,
                      const std::string& admission_dir) {
    try {
        if (std::getenv("SS_B1_VIEW_POLICY") || std::getenv("SSPLAT_B1_VIEW_POLICY") ||
            spirula::env("B1_GROUP") || spirula::env("B1_GROUP_SHA256") ||
            spirula::env("B1_REFRESH_CADENCE") || spirula::env("B1_REFRESH_ATTEMPT"))
            throw std::runtime_error("B1 inherited group/policy overrides are not allowed");
#if !defined(SS_BACKEND_VULKAN)
        throw std::runtime_error("B1 group requires Vulkan");
#endif
        fs::path source, views, output, resume;
        std::string devices, view_policy = "partitioned";
        bool policy_given = false;
        bool cadence_given = false;
        int64_t refresh_cadence = 0;
        int64_t updates = 0;
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            if (key == "--help") {
                std::printf("spirula train-blocks --resume SOURCE --views MANIFEST --devices uuid:A,uuid:B --output NEW_GROUP --updates N [--view-policy partitioned|full] [--refresh-cadence N]\nspirula train-blocks --resume-group GROUP\n");
                std::printf("--view-policy partitioned|full  %s\n",
                            spirula::i18n::msg::cli::b1_view_policy_help.get());
                return 0;
            }
            if (++i >= argc) throw std::runtime_error("B1 group option value missing");
            const std::string value = argv[i];
            if (key == "--resume") source = fs::u8path(value);
            else if (key == "--views") views = fs::u8path(value);
            else if (key == "--devices") devices = value;
            else if (key == "--output") output = fs::u8path(value);
            else if (key == "--resume-group") resume = fs::u8path(value);
            else if (key == "--view-policy") {
                if (policy_given || (value != "partitioned" && value != "full"))
                    throw std::runtime_error("B1 view policy must be partitioned or full");
                view_policy = value;
                policy_given = true;
            }
            else if (key == "--refresh-cadence") {
                size_t consumed = 0;
                if (cadence_given) throw std::runtime_error("B1 refresh cadence repeated");
                refresh_cadence = std::stoll(value, &consumed);
                if (consumed != value.size() || refresh_cadence < 1 || refresh_cadence > 2147483647)
                    throw std::runtime_error("B1 refresh cadence invalid");
                cadence_given = true;
            }
            else if (key == "--updates") {
                size_t consumed = 0;
                updates = std::stoll(value, &consumed);
                if (consumed != value.size() || updates <= 0 || updates > 2147483647)
                    throw std::runtime_error("B1 updates invalid");
            } else throw std::runtime_error("B1 group unknown option: " + key);
        }
        Group g;
        const bool restarting = !resume.empty();
        if (restarting) {
            if (!source.empty() || !views.empty() || !output.empty() || !devices.empty() || updates || policy_given || cadence_given)
                throw std::runtime_error("B1 resume-group accepts no input overrides");
            output = fs::absolute(resume);
            g = read_group(output);
        } else {
            if (source.empty() || views.empty() || output.empty() || devices.empty() || updates <= 0)
                throw std::runtime_error("B1 group required options missing");
            g.view_policy = view_policy;
            if (cadence_given) {
                if (view_policy != "full" || updates % refresh_cadence)
                    throw std::runtime_error("B1 refresh requires full views and cadence-aligned updates");
                g.version = 3;
                g.refresh_cadence = refresh_cadence;
            }
            g.source = ckpt::resolve_checkpoint(fs::absolute(source)).ckpt_dir;
            g.views = fs::absolute(views);
            const auto comma = devices.find(',');
            if (comma == std::string::npos) throw std::runtime_error("B1 group needs two devices");
            g.devices = {devices.substr(0, comma), devices.substr(comma + 1)};
            g.source_step = static_cast<int64_t>(ckpt::read_state_json(g.source).get_double("step", -1));
            g.target = g.source_step + updates;
            g.source_hash = ckpt::b1_file_hash(g.source / "state.tar");
            g.views_hash = ckpt::b1_file_hash(g.views);
            g.shared_hash = ckpt::b1_shared_hash(g.source);
            const auto resolved = ckpt::resolve_checkpoint(g.source);
            g.config_hash = ckpt::b1_config_hash(ckpt::config_from_json(resolved.run_dir / "config.json"));
            output = fs::absolute(output);
            if (app::sched::path_claims_conflict({resolved.run_dir.u8string(), false}, {output.u8string(), true}) ||
                app::sched::path_claims_conflict({g.views.u8string(), false}, {output.u8string(), true}))
                throw std::runtime_error("B1 group output overlaps inputs");
            validate_inputs(g);
            if (!fs::create_directory(output)) throw std::runtime_error("B1 group output already exists");
        }
        app::OutputLease group_lease, source_lease;
        std::string error;
        if (!group_lease.acquire(output, error) ||
            !source_lease.acquire(ckpt::resolve_checkpoint(g.source).run_dir, error))
            throw std::runtime_error(error);
        validate_inputs(g);
        if (fs::exists(output / "complete.json")) throw std::runtime_error("B1 group already complete");
        if (!restarting) b1_publish_record(output / "group.json", manifest(g));
        const auto group_path = output / "group.json";
        const auto group_hash = ckpt::b1_file_hash(group_path);
        validate_attempt_bindings(output, g, group_path, group_hash);
        std::vector<std::unique_ptr<app::OutputLease>> owner_leases;
        std::set<fs::path> leased_runs;
        auto lease_owner_runs = [&](const fs::path& attempts) {
            if (!fs::exists(attempts)) return;
            for (const auto& attempt_entry : fs::directory_iterator(attempts))
                if (attempt_entry.is_directory())
                    for (int owner = 0; owner < 2; ++owner) {
                        const auto run = attempt_entry.path() / ("owner-" + std::to_string(owner));
                        if (!fs::is_directory(run) || leased_runs.count(run)) continue;
                        auto lease = std::make_unique<app::OutputLease>();
                        if (!lease->acquire(run, error)) throw std::runtime_error(error);
                        owner_leases.push_back(std::move(lease));
                        leased_runs.insert(run);
                    }
        };
        lease_owner_runs(output / "attempts");
        auto owners = collect(output, g);
        if (g.version == 3) prune_abandoned_refresh_attempts(output, owners);
        fs::create_directories(output / "attempts");
        fs::path attempt;
        for (uint64_t n = 1; ; ++n) {
            attempt = output / "attempts" / std::to_string(n);
            if (fs::create_directory(attempt)) break;
        }
        app::sched::JobScheduler scheduler((attempt / "scheduler").u8string(), executable,
                                           admission_dir);
        if (!scheduler.state_error().empty()) throw std::runtime_error(scheduler.state_error());
        scheduler.pause_dispatch(true);
        scheduler.set_event_callback([](const app::sched::Event& event) {
            std::printf("B1 job=%s state=%s %s %s\n", event.job_id.c_str(),
                        app::sched::to_string(event.state), event.line.c_str(), event.error.c_str());
        });
        std::array<std::string, 2> owner_jobs;
        std::array<bool, 2> owner_stop_requested{};
        for (int owner = 0; owner < 2; ++owner) {
            if (owners[owner].contract && owners[owner].contract->completed_step == g.target) continue;
            const auto run = attempt / ("owner-" + std::to_string(owner));
            app::sched::SubmitOpts opts;
            opts.phase = "train"; opts.device = g.devices[owner];
            opts.work_dir = fs::current_path().u8string(); opts.output_dir = run.u8string();
            opts.args = {"--resume", (owners[owner].contract ? owners[owner].checkpoint : g.source).u8string(),
                         "--output-dir-prefix", attempt.u8string(), "--output-dir-name", run.filename().u8string(),
                         "--disable-viewer", "true", "--save-full-checkpoint", "true",
                         "--save-only-latest-checkpoint", g.version == 3 ? "false" : "true"};
            opts.payload = app::b1_worker_payload({owner, g.target, g.source.u8string(),
                                                  g.views.u8string(), group_path.u8string(), group_hash,
                                                  g.version == 3 ? attempt.u8string() : std::string()});
            opts.path_claims = {{g.source.u8string(), false}, {g.views.u8string(), false},
                               {group_path.u8string(), false}};
            if (owners[owner].contract) opts.path_claims.push_back({owners[owner].checkpoint.u8string(), false});
            owner_jobs[owner] = scheduler.submit(opts);
            if (owner_jobs[owner].empty()) throw std::runtime_error("B1 owner admission failed: " + scheduler.state_error());
        }
        const auto previous_signal = std::signal(SIGINT, interrupt);
        const auto previous_term = std::signal(SIGTERM, interrupt);
        interrupted = 0;
        scheduler.pause_dispatch(false);
        bool stopping = false, failed = false;
        int64_t refresh_boundary = (owners[0].contract ? owners[0].contract->completed_step : g.source_step) + g.refresh_cadence;
        for (;;) {
            const int control = read_control();
            if (control & 2) interrupted = 2;
            else if (control & 1) interrupted = 1;
            if (interrupted) stopping = true;
            scheduler.drain_events();
            if (!scheduler.state_error().empty()) {
                failed = true;
                std::fprintf(stderr, "B1 scheduler: %s\n", scheduler.state_error().c_str());
            }
            const auto jobs = scheduler.list();
            for (int owner = 0; owner < 2; ++owner)
                if (control & (4 << owner))
                    for (const auto& job : jobs)
                        if (job.job_id == owner_jobs[owner] &&
                            job.state != app::sched::JobState::Queued && !terminal(job.state)) {
                            scheduler.stop_and_save(job.job_id);
                            owner_stop_requested[owner] = true;
                        }
            if (g.version == 3 && !stopping && !failed && refresh_boundary <= g.target) {
                try {
                    if (coordinate_refresh(output, attempt, g, group_hash, refresh_boundary))
                        refresh_boundary += g.refresh_cadence;
                } catch (const std::exception& e) {
                    failed = true;
                    std::fprintf(stderr, "B1 refresh: %s\n", e.what());
                }
            }
            bool done = true;
            for (const auto& job : jobs) {
                done = done && terminal(job.state);
                if (job.state == app::sched::JobState::Stopped)
                    for (int owner = 0; owner < 2; ++owner)
                        if (owner_stop_requested[owner] && job.job_id == owner_jobs[owner])
                            stopping = true;
                if (g.version == 3 && terminal(job.state) && refresh_boundary <= g.target)
                    stopping = true;
                failed = failed || job.state == app::sched::JobState::Failed ||
                    job.state == app::sched::JobState::Interrupted || job.state == app::sched::JobState::Blocked;
            }
            if (interrupted || failed || stopping) {
                stopping = true;
                if (g.version == 3 && !fs::exists(attempt / "refresh.abort"))
                    b1_publish_record(attempt / "refresh.abort", "abort");
                scheduler.pause_dispatch(true);
                for (const auto& job : scheduler.list()) {
                    if (job.state == app::sched::JobState::Queued) scheduler.cancel(job.job_id);
                    if (!terminal(job.state)) {
                        if (interrupted == 2) scheduler.force_stop(job.job_id);
                        else scheduler.stop_and_save(job.job_id);
                    }
                }
            }
            if (done) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        scheduler.shutdown();
        std::signal(SIGINT, previous_signal); std::signal(SIGTERM, previous_term);
        lease_owner_runs(attempt.parent_path());
        owners = collect(output, g);
        if (stopping || !owners[0].contract || !owners[1].contract ||
            owners[0].contract->completed_step != g.target || owners[1].contract->completed_step != g.target) {
            std::printf("B1_GROUP outcome=incomplete resume_group=%s target=%lld\n",
                        output.u8string().c_str(), static_cast<long long>(g.target));
            return failed ? 2 : 42;
        }
        validate_inputs(g);
        if (ckpt::b1_file_hash(group_path) != group_hash)
            throw std::runtime_error("B1 group identity changed during execution");
        app::validate_b1_group_completion(*owners[0].contract, *owners[1].contract, g.target);
        ckpt::merge_b1_ply(owners[0].checkpoint / "splat.ply", owners[1].checkpoint / "splat.ply", attempt / "merged.ply");
        ckpt::validate_b1_appearance(attempt / "merged.ply", g.source);
        JsonWriter completion;
        completion.object().field("group_sha256", ckpt::b1_file_hash(output / "group.json"))
            .field("owner0", owners[0].checkpoint.u8string()).field("owner1", owners[1].checkpoint.u8string())
            .field("merged", (attempt / "merged.ply").u8string())
            .field("merged_sha256", ckpt::b1_file_hash(attempt / "merged.ply"))
            .field("target", static_cast<long long>(g.target)).end();
        b1_publish_record(output / "complete.json", completion.str(), attempt / "complete.pending");
        std::printf("B1_GROUP outcome=complete completion=%s\n", (output / "complete.json").u8string().c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", spirula::i18n::format(
            spirula::i18n::msg::cli::error_line, {e.what()}).c_str());
        return 2;
    }
}
