#include <checkpoint/B1Contract.h>
#include <checkpoint/B1Merge.h>
#include <checkpoint/SplatPly.h>
#include <core/CheckpointIO.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static void le(std::ostream& out, uint64_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) out.put((char)(value >> (8 * i)));
}
static void ids(const fs::path& path, const std::vector<uint32_t>& values) {
    std::ofstream out(path, std::ios::binary);
    le(out, values.size(), 8);
    for (auto id : values) le(out, id, 4);
}
static void state(const fs::path& dir, const std::string& json,
                  const std::string& shared = "") {
    std::ofstream out(dir / "state.tar", std::ios::binary);
    ckpt::tar_write_bytes(out, "state.json", json.data(), json.size());
    if (!shared.empty())
        ckpt::tar_write_bytes(out, "eng.bg.color.npy", shared.data(), shared.size());
    ckpt::tar_finish(out);
}
static void ply(const fs::path& path, const std::string& appearance = "") {
    std::ofstream out(path, std::ios::binary);
    out << "ply\nformat binary_little_endian 1.0\n" << appearance << "element vertex 1\n";
    for (const char* name : {"x", "y", "z", "opacity", "scale_0", "scale_1",
            "scale_2", "rot_0", "rot_1", "rot_2", "rot_3", "f_dc_0", "f_dc_1", "f_dc_2"})
        out << "property float " << name << '\n';
    out << "end_header\n";
    for (int i = 0; i < 14; ++i) le(out, i == 7 ? 0x3f800000 : 0, 4);
}
static void save_contract(const fs::path& dir, ckpt::B1Contract contract) {
    ckpt::bind_b1_sidecars(contract, dir);
    state(dir, "{\"format_version\":2,\"full_resume\":1,\"step\":1,"
        "\"cur_num_splats\":257,\"arrays\":[],\"b1\":" +
        ckpt::serialize_b1_contract(contract) + "}");
}
static ckpt::B1Contract owner(const fs::path& dir, int index,
                               const std::string& source_hash) {
    fs::create_directory(dir);
    {
        std::ofstream blocks(dir / "b1-owned-blocks.bin", std::ios::binary);
        le(blocks, 257, 8); le(blocks, 1, 8); le(blocks, 1, 8);
        le(blocks, 2, 4); le(blocks, index, 4);
        le(blocks, index != 1, 4); le(blocks, index != 0, 4);
    }
    ids(dir / "b1-owned-ids.bin", {index == 1 ? 256u : 0u});
    std::vector<uint32_t> excluded;
    if (index != 1)
        for (uint32_t i = 1; i < (index == 2 ? 257u : 256u); ++i) excluded.push_back(i);
    ids(dir / "b1-excluded-ids.bin", excluded);
    ply(dir / "splat.ply");
    state(dir, "{\"arrays\":[]}");
    std::ofstream(dir / "config.json") << "{}";
    ckpt::B1Contract c;
    c.owner = index; c.rows = 257; c.completed_step = 1; c.horizon = 10;
    c.source_state_sha256 = source_hash;
    c.ply_sha256 = ckpt::b1_file_hash(dir / "splat.ply");
    c.config_sha256 = ckpt::b1_config_hash(TrainConfig{});
    c.views_sha256 = std::string(64, 'a');
    c.train_indices_sha256 = std::string(64, index ? 'b' : 'c');
    c.full_train_indices_sha256 = std::string(64, 'd');
    if (index == 2) {
        c.view_policy = "full";
        c.train_indices_sha256 = c.full_train_indices_sha256;
    }
    c.frozen_context = std::string(64, '1');
    c.frozen_shared_sha256 = ckpt::b1_shared_hash(dir);
    save_contract(dir, c);
    return c;
}
static void reject(const fs::path& left, const fs::path& right, const fs::path& out) {
    bool rejected = false;
    try { ckpt::merge_b1_ply(left, right, out); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "unsafe merge accepted");
}
static void reject_appearance(const fs::path& ply_path, const fs::path& source) {
    bool rejected = false;
    try { ckpt::validate_b1_appearance(ply_path, source); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "unsafe evaluation appearance accepted");
}

int main() {
    const fs::path root = fs::temp_directory_path() / ("ss-b1-merge-contract-" +
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    try {
        require(fs::create_directory(root), "cannot reserve test directory");
        fs::create_directory(root / "source");
        state(root / "source", "{\"arrays\":[]}");
        auto lc = owner(root / "left", 0, ckpt::b1_file_hash(root / "source" / "state.tar"));
        auto rc = owner(root / "right", 1, lc.source_state_sha256);
        const auto left = root / "left" / "splat.ply";
        const auto right = root / "right" / "splat.ply";
        const auto merged = root / "merged.ply";
        const auto rejected_output = root / "rejected.ply";
        ckpt::merge_b1_ply(left, right, merged);
        ckpt::validate_b1_appearance(merged, root / "source");
        require(spirula::read_splat_ply(merged.string(), false).num == 2,
                "merged PLY is not readable by mesh consumer");
        const auto source = root / "source";
        const auto all = root / "all";
        auto ac = owner(all, 2, lc.source_state_sha256);
        const auto all_ply = all / "splat.ply";
        ckpt::validate_b1_appearance(all_ply, source);
        require(spirula::read_splat_ply(all_ply.string(), false).num == 1,
                "allowned PLY is not readable by evaluation consumer");
        auto legacy_all = ac; legacy_all.version = 1;
        save_contract(all, legacy_all);
        const auto read_legacy = ckpt::read_b1_contract(all);
        require(read_legacy && read_legacy->version == 1 && read_legacy->view_policy == "full",
                "historical all-owned checkpoint lost native full-view semantics");
        ckpt::validate_b1_appearance(all_ply, source);
        save_contract(all, ac);
        reject_appearance(left, source);
        ply(source / "splat.ply");
        reject_appearance(source / "splat.ply", source);
        fs::copy_file(all_ply, root / "relocated.ply");
        reject_appearance(root / "relocated.ply", source);
        fs::copy_file(all_ply, all / "renamed.ply");
        reject_appearance(all / "renamed.ply", source);
        ply(root / "forged.ply", ckpt::b1_appearance_comment(ac, std::string(64, '0')));
        reject_appearance(root / "forged.ply", source);
        fs::create_directory(root / "foreign");
        state(root / "foreign", "{\"arrays\":[],\"foreign\":1}");
        reject_appearance(all_ply, root / "foreign");
        auto changed = ac;
        changed.source_state_sha256 = std::string(64, '0');
        save_contract(all, changed);
        reject_appearance(all_ply, source);
        changed = ac;
        changed.frozen_shared_sha256 = std::string(64, '0');
        save_contract(all, changed);
        reject_appearance(all_ply, source);
        save_contract(all, ac);
        state(source, "{\"arrays\":[]}", "changed appearance");
        changed.source_state_sha256 = ckpt::b1_file_hash(source / "state.tar");
        changed.frozen_shared_sha256 = ac.frozen_shared_sha256;
        save_contract(all, changed);
        reject_appearance(all_ply, source);
        state(source, "{\"arrays\":[]}");
        save_contract(all, ac);
        std::ofstream(all / "splat.ply", std::ios::binary | std::ios::app).put('\0');
        reject_appearance(all_ply, source);
        ply(all_ply);
        state(all, "{\"format_version\":2,\"full_resume\":1,\"step\":2,"
            "\"cur_num_splats\":257,\"arrays\":[],\"b1\":" +
            ckpt::serialize_b1_contract(ac) + "}");
        reject_appearance(all_ply, source);
        save_contract(all, ac);
        std::ofstream(all / "config.json") << "{\"num_iterations\":11}";
        reject_appearance(all_ply, source);
        std::ofstream(all / "config.json") << "{}";
        ids(all / "b1-excluded-ids.bin", {1});
        reject_appearance(all_ply, source);
        fs::remove(all / "b1-owned-blocks.bin");
        reject_appearance(all_ply, source);
        ckpt::validate_b1_appearance(merged, source);
        const auto original = ckpt::b1_file_hash(merged);
        reject(left, right, merged);
        require(ckpt::b1_file_hash(merged) == original, "merge replaced prior output");
        const auto left_root = root / "left";
        const auto right_root = root / "right";
        auto policy_changed = rc;
        policy_changed.view_policy = "full";
        policy_changed.train_indices_sha256 = policy_changed.full_train_indices_sha256;
        save_contract(right_root, policy_changed);
        reject(left, right, rejected_output);
        require(!fs::exists(rejected_output), "mixed view policies published output");
        policy_changed = rc; policy_changed.full_train_indices_sha256 = std::string(64, 'e');
        save_contract(right_root, policy_changed);
        reject(left, right, rejected_output);
        require(!fs::exists(rejected_output), "different canonical training sets published output");
        policy_changed = rc; policy_changed.sampler_version = 2;
        save_contract(right_root, policy_changed);
        reject(left, right, rejected_output);
        require(!fs::exists(rejected_output), "incompatible sampler published output");
        policy_changed = rc; policy_changed.view_policy = "unknown";
        save_contract(right_root, policy_changed);
        reject(left, right, rejected_output);
        require(!fs::exists(rejected_output), "invalid view policy published output");
        for (const char* missing : {"view_policy", "full_train_indices_sha256"}) {
            auto bound = rc;
            ckpt::bind_b1_sidecars(bound, right_root);
            auto json = ckpt::serialize_b1_contract(bound);
            const std::string field = "\"" + std::string(missing) + "\":";
            const auto start = json.find(field);
            require(start != std::string::npos, "fixture field missing");
            const auto end = json.find(',', start);
            require(end != std::string::npos, "fixture field delimiter missing");
            json.erase(start, end - start + 1);
            state(right_root, "{\"format_version\":2,\"full_resume\":1,\"step\":1,"
                "\"cur_num_splats\":257,\"arrays\":[],\"b1\":" + json + "}");
            reject(left, right, rejected_output);
            require(!fs::exists(rejected_output), "missing v2 view identity published output");
        }
        auto full_left = lc; full_left.view_policy = "full";
        auto full_right = rc; full_right.view_policy = "full";
        full_left.train_indices_sha256 = full_right.train_indices_sha256 = std::string(64, 'f');
        save_contract(left_root, full_left);
        save_contract(right_root, full_right);
        reject(left, right, rejected_output);
        require(!fs::exists(rejected_output), "matching incomplete full-view sets published output");
        full_left.train_indices_sha256 = full_left.full_train_indices_sha256;
        full_right.train_indices_sha256 = full_right.full_train_indices_sha256;
        save_contract(left_root, full_left);
        save_contract(right_root, full_right);
        const auto full_merged = root / "full-merged.ply";
        ckpt::merge_b1_ply(left, right, full_merged);
        ckpt::validate_b1_appearance(full_merged, source);
        require(spirula::read_splat_ply(full_merged.string(), false).num == 2,
                "full-view pair did not publish complete owned rows");
        auto live_left = full_left; live_left.version = 3;
        live_left.refresh_cadence = 1; live_left.refresh_generation = 1;
        live_left.refresh_parent = 0;
        live_left.refresh_group_sha256 = std::string(64, 'a');
        live_left.refresh_payload0_sha256 = std::string(64, 'b');
        live_left.refresh_payload1_sha256 = std::string(64, 'c');
        live_left.refresh_commit_sha256 = std::string(64, 'd');
        auto live_right = live_left; live_right.owner = 1;
        live_right.frozen_context = std::string(64, '2');
        save_contract(left_root, live_left);
        save_contract(right_root, live_right);
        const auto live_merged = root / "live-merged.ply";
        ckpt::merge_b1_ply(left, right, live_merged);
        ckpt::validate_b1_appearance(live_merged, source);
        require(spirula::read_splat_ply(live_merged.string(), false).num == 2,
                "live-context merge lost tail owned rows");
        const auto live_hash = ckpt::b1_file_hash(live_merged);
        auto mixed_live = live_right;
        mixed_live.refresh_payload1_sha256 = std::string(64, 'e');
        save_contract(right_root, mixed_live);
        reject(left, right, rejected_output);
        require(!fs::exists(rejected_output), "mixed live payloads published output");
        reject(left, right, live_merged);
        require(ckpt::b1_file_hash(live_merged) == live_hash, "mixed live pair replaced prior output");
        mixed_live = live_right;
        mixed_live.refresh_payload1_sha256 = std::string(64, '0');
        save_contract(right_root, mixed_live);
        reject(left, right, rejected_output);
        require(!fs::exists(rejected_output), "partial live installation published output");
        auto before_refresh = live_left;
        before_refresh.refresh_generation = before_refresh.refresh_parent = 0;
        before_refresh.refresh_payload0_sha256 = before_refresh.refresh_payload1_sha256 = std::string(64, '0');
        before_refresh.refresh_commit_sha256 = before_refresh.refresh_group_sha256;
        save_contract(left_root, before_refresh);
        save_contract(right_root, live_right);
        reject(left, right, rejected_output);
        require(!fs::exists(rejected_output), "owners across refresh barrier published output");
        before_refresh.owner = 1;
        save_contract(right_root, before_refresh);
        reject(left, right, rejected_output);
        require(!fs::exists(rejected_output), "stale matched live pair published output");
        auto legacy_left = lc; legacy_left.version = 1;
        auto legacy_right = rc; legacy_right.version = 1;
        save_contract(left_root, legacy_left);
        save_contract(right_root, rc);
        reject(left, right, rejected_output);
        require(!fs::exists(rejected_output), "mixed checkpoint versions published output");
        save_contract(right_root, legacy_right);
        const auto legacy_merged = root / "legacy-merged.ply";
        ckpt::merge_b1_ply(left, right, legacy_merged);
        ckpt::validate_b1_appearance(legacy_merged, source);
        require(spirula::read_splat_ply(legacy_merged.string(), false).num == 2,
                "historical partitioned pair did not publish complete owned rows");
        save_contract(left_root, lc);
        save_contract(right_root, rc);
        require(ckpt::b1_file_hash(merged) == original, "policy rejection changed prior merge");
        {
            std::fstream changed(left, std::ios::binary | std::ios::in | std::ios::out);
            std::string line;
            while (std::getline(changed, line) && line != "end_header") {}
            require(line == "end_header", "fixture header missing");
            changed.seekp(changed.tellg());
            le(changed, 0x3f800000, 4);
        }
        reject(left, right, rejected_output);
        require(!fs::exists(rejected_output), "mutated finite PLY published output");
        ply(left);
        ++rc.horizon;
        save_contract(root / "right", rc);
        reject(left, right, rejected_output);
        require(!fs::exists(rejected_output), "mismatched pair published output");
        --rc.horizon;
        save_contract(root / "right", rc);
        ids(root / "left" / "b1-excluded-ids.bin", {0});
        save_contract(root / "left", lc);
        reject(left, right, rejected_output);
        require(!fs::exists(rejected_output), "duplicate ownership published output");
        ids(root / "left" / "b1-excluded-ids.bin", {1});
        save_contract(root / "left", lc);
        reject(left, right, rejected_output);
        require(!fs::exists(rejected_output), "incomplete ownership published output");
        fs::remove(root / "left" / "b1-owned-blocks.bin");
        reject(left, right, rejected_output);
        require(!fs::exists(rejected_output), "missing sidecar published output");
        state(root / "right", "{\"arrays\":[]}");
        reject(right, right, rejected_output);
        require(!fs::exists(rejected_output), "unmarked owner published output");
        require(ckpt::b1_file_hash(merged) == original, "rejection changed prior merge");
        fs::remove_all(root);
        std::cout << "b1_merge_contract: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored;
        fs::remove_all(root, ignored);
        std::cerr << "b1_merge_contract: " << error.what() << '\n';
        return 1;
    }
}
