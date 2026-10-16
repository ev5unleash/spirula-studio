// checkpoint_resolution_test -- archive integrity and latest-valid selection.

#include "checkpoint/Adapt.h"
#include "checkpoint/Resume.h"
#include "checkpoint/B1Contract.h"
#include "core/CheckpointIO.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

std::string state_json(int step, int non_sh_bits = 32) {
    return "{\"format_version\":1,\"full_resume\":1,\"step\":" +
           std::to_string(step) +
           ",\"primitive\":\"3dgs\",\"cur_num_splats\":1,"
           "\"max_num_splats\":1,\"num_sh\":0,\"sh_degree\":0,"
           "\"sh_optim_bits\":32,\"sh_value_bits\":32,"
           "\"non_sh_optim_bits\":" + std::to_string(non_sh_bits) +
           ",\"use_fused_proj_bwd_optim\":0}";
}

void write_npy(std::ostream& tar, const std::string& name,
               const char* descr, size_t numel) {
    std::string data = ckpt::npy_header(descr, numel);
    data.append(numel * (size_t)(descr[2] - '0'), '\0');
    ckpt::tar_write_bytes(tar, name, data.data(), data.size());
}

void write_f32(std::ostream& tar, const std::string& name, size_t numel) {
    write_npy(tar, name, "<f4", numel);
}

void write_core(std::ostream& tar) {
    write_f32(tar, "world.means.npy", 3);
    write_f32(tar, "world.quats.npy", 4);
    write_f32(tar, "world.scales.npy", 3);
    write_f32(tar, "world.opacities.npy", 1);
    write_f32(tar, "world.features_dc.npy", 3);
}
void write_fp32_optimizer(std::ostream& tar) {
    const std::pair<const char*, size_t> arrays[] = {
        {"means", 3}, {"quats", 4}, {"scales", 3},
        {"opacities", 1}, {"features_dc", 3},
    };
    for (const auto& [name, numel] : arrays) {
        write_f32(tar, "eng.g1_" + std::string(name) + ".npy", numel);
        write_f32(tar, "eng.g2_" + std::string(name) + ".npy", numel);
    }
}

void write_resume_aux(std::ostream& tar) {
    write_f32(tar, "eng.radii.npy", 1);
    write_f32(tar, "eng.accum_buffer.npy", 2);
}


void write_checkpoint(const fs::path& dir, int step) {
    fs::create_directories(dir);
    const std::string state = state_json(step);
    std::ofstream tar(dir / "state.tar", std::ios::binary);
    ckpt::tar_write_bytes(tar, "state.json", state.data(), state.size());
    write_core(tar);
    write_fp32_optimizer(tar);
    write_resume_aux(tar);
    ckpt::tar_finish(tar);
    tar.close();
    if (!tar) throw std::runtime_error("failed to write test checkpoint");
}

void write_interrupted_checkpoint(const fs::path& dir, int step) {
    fs::create_directories(dir);
    const std::string state = state_json(step);
    std::ofstream tar(dir / "state.tar", std::ios::binary);
    ckpt::tar_write_bytes(tar, "state.json", state.data(), state.size());
}

void write_invalid_npy_checkpoint(const fs::path& dir, int step) {
    fs::create_directories(dir);
    const std::string state = state_json(step);
    std::ofstream tar(dir / "state.tar", std::ios::binary);
    ckpt::tar_write_bytes(tar, "state.json", state.data(), state.size());
    ckpt::tar_write_bytes(tar, "world.means.npy", "", 0);
    ckpt::tar_finish(tar);
}
void write_missing_radii_checkpoint(const fs::path& dir, int step) {
    fs::create_directories(dir);
    const std::string state = state_json(step);
    std::ofstream tar(dir / "state.tar", std::ios::binary);
    ckpt::tar_write_bytes(tar, "state.json", state.data(), state.size());
    write_core(tar);
    write_fp32_optimizer(tar);
    write_f32(tar, "eng.accum_buffer.npy", 2);
    ckpt::tar_finish(tar);
}

void write_missing_manifest_member_checkpoint(const fs::path& dir, int step) {
    fs::create_directories(dir);
    const std::string state =
        "{\"format_version\":2,\"full_resume\":0,\"step\":" +
        std::to_string(step) +
        ",\"arrays\":[{\"name\":\"color_space.splat_matrix\","
        "\"descr\":\"<f4\",\"bytes\":36}]}";
    std::ofstream tar(dir / "state.tar", std::ios::binary);
    ckpt::tar_write_bytes(tar, "state.json", state.data(), state.size());
    ckpt::tar_finish(tar);
}


void write_short_quant_checkpoint(const fs::path& dir, int step) {
    fs::create_directories(dir);
    const std::string state = state_json(step, 16);
    std::ofstream tar(dir / "state.tar", std::ios::binary);
    ckpt::tar_write_bytes(tar, "state.json", state.data(), state.size());
    write_core(tar);
    write_resume_aux(tar);
    const std::pair<const char*, size_t> arrays[] = {
        {"means", 3}, {"quats", 4}, {"scales", 3},
        {"opacities", 1}, {"features_dc", 3},
    };
    for (const auto& [name, channels] : arrays) {
        const std::string base = "eng." + std::string(name) + "_qfpbo";
        write_npy(tar, base + ".q.npy", "|u1",
                  std::string(name) == "means" ? 1 : channels * 4);
        write_f32(tar, base + ".qb.npy", 4);
    }
    ckpt::tar_finish(tar);
}
void write_adapt_source(const fs::path& dir, int step) {
    std::vector<std::pair<std::string, size_t>> arrays = {
        {"world.means", 3}, {"world.quats", 4}, {"world.scales", 3},
        {"world.opacities", 1}, {"world.features_dc", 3},
        {"world.features_sh", 3}, {"eng.radii", 1},
        {"eng.accum_buffer", 2}, {"eng.g1_features_sh", 3},
        {"eng.g2_features_sh", 3},
    };
    for (const auto& [name, numel] :
         {std::pair{"means", 3u}, std::pair{"quats", 4u},
          std::pair{"scales", 3u}, std::pair{"opacities", 1u},
          std::pair{"features_dc", 3u}}) {
        arrays.emplace_back("eng.g1_" + std::string(name), numel);
        arrays.emplace_back("eng.g2_" + std::string(name), numel);
    }

    std::ostringstream manifest;
    for (size_t i = 0; i < arrays.size(); ++i) {
        if (i != 0) manifest << ',';
        manifest << "{\"name\":\"" << arrays[i].first
                 << "\",\"descr\":\"<f4\",\"bytes\":"
                 << arrays[i].second * sizeof(float) << '}';
    }
    const std::string state =
        "{\"format_version\":2,\"full_resume\":1,\"step\":" +
        std::to_string(step) +
        ",\"primitive\":\"3dgs\",\"cur_num_splats\":1,"
        "\"max_num_splats\":1,\"num_sh\":1,\"sh_degree\":1,"
        "\"sh_optim_bits\":32,\"sh_value_bits\":32,"
        "\"non_sh_optim_bits\":32,\"use_fused_proj_bwd_optim\":0,"
        "\"arrays\":[" + manifest.str() + "]}";

    fs::create_directories(dir);
    std::ofstream tar(dir / "state.tar", std::ios::binary);
    ckpt::tar_write_bytes(tar, "state.json", state.data(), state.size());
    for (const auto& [name, numel] : arrays)
        write_f32(tar, name + ".npy", numel);
    ckpt::tar_finish(tar);
}

bool adapted_checkpoint_is_valid(const fs::path& source,
                                 const fs::path& adapted) {
    ckpt::TargetLayout target;
    target.max_num_splats = 1;
    target.num_sh = 0;
    target.num_images = 1;
    if (!ckpt::adapt_checkpoint(source, target, adapted)) return false;
    try {
        ckpt::read_state_json(adapted);
        ckpt::check_resumable(adapted);
        std::ifstream tar(adapted / "state.tar", std::ios::binary);
        for (const auto& member : ckpt::tar_index(tar))
            if (member.name.find("features_sh") != std::string::npos)
                return false;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
void write_adapt_q4_source(const fs::path& dir, int step) {
    std::vector<std::pair<std::string, size_t>> arrays = {
        {"world.means", 3}, {"world.quats", 4}, {"world.scales", 3},
        {"world.opacities", 1}, {"world.features_dc", 3},
        {"world.features_sh", 3}, {"eng.radii", 1},
        {"eng.accum_buffer", 2},
    };
    for (const auto& [name, numel] :
         {std::pair{"means", 3u}, std::pair{"quats", 4u},
          std::pair{"scales", 3u}, std::pair{"opacities", 1u},
          std::pair{"features_dc", 3u}}) {
        arrays.emplace_back("eng.g1_" + std::string(name), numel);
        arrays.emplace_back("eng.g2_" + std::string(name), numel);
    }

    std::ostringstream manifest;
    for (const auto& [name, numel] : arrays) {
        if (manifest.tellp() != std::streampos(0)) manifest << ',';
        manifest << "{\"name\":\"" << name
                 << "\",\"descr\":\"<f4\",\"bytes\":"
                 << numel * sizeof(float) << '}';
    }
    manifest << ",{\"name\":\"eng.sh_quant.q\",\"descr\":\"|u1\","
                "\"bytes\":6},{\"name\":\"eng.sh_quant.qb\","
                "\"descr\":\"<f4\",\"bytes\":16}";
    const std::string state =
        "{\"format_version\":2,\"full_resume\":1,\"step\":" +
        std::to_string(step) +
        ",\"primitive\":\"3dgs\",\"cur_num_splats\":1,"
        "\"max_num_splats\":1,\"num_sh\":1,\"sh_degree\":1,"
        "\"sh_optim_bits\":4,\"sh_value_bits\":32,"
        "\"non_sh_optim_bits\":32,\"use_fused_proj_bwd_optim\":0,"
        "\"arrays\":[" + manifest.str() + "]}";

    fs::create_directories(dir);
    std::ofstream tar(dir / "state.tar", std::ios::binary);
    ckpt::tar_write_bytes(tar, "state.json", state.data(), state.size());
    for (const auto& [name, numel] : arrays)
        write_f32(tar, name + ".npy", numel);
    const uint8_t packed[6] = {0x00, 0xff, 0x78, 0, 0, 0};
    const float bounds[4] = {-1.0f, 1.0f, 0.0f, 30.0f};
    std::string q = ckpt::npy_header("|u1", 6);
    q.append(reinterpret_cast<const char*>(packed), sizeof packed);
    ckpt::tar_write_bytes(tar, "eng.sh_quant.q.npy", q.data(), q.size());
    std::string qb = ckpt::npy_header("<f4", 4);
    qb.append(reinterpret_cast<const char*>(bounds), sizeof bounds);
    ckpt::tar_write_bytes(tar, "eng.sh_quant.qb.npy", qb.data(), qb.size());
    ckpt::tar_finish(tar);
}

bool adapted_q4_is_valid(const fs::path& source, const fs::path& adapted) {
    ckpt::TargetLayout target;
    target.max_num_splats = 2;
    target.num_sh = 1;
    target.sh_optim_bits = 4;
    target.num_images = 1;
    try {
        ckpt::check_resumable(source);
        if (!ckpt::adapt_checkpoint(source, target, adapted)) return false;
        ckpt::read_state_json(adapted);
        ckpt::check_resumable(adapted);
        std::ifstream tar(adapted / "state.tar", std::ios::binary);
        std::vector<uint8_t> packed;
        float4 bounds{};
        for (const auto& member : ckpt::tar_index(tar)) {
            if (member.name != "eng.sh_quant.q.npy" &&
                member.name != "eng.sh_quant.qb.npy")
                continue;
            const ckpt::NpyInfo info =
                ckpt::npy_locate(tar, member.data_offset, member.size);
            tar.clear();
            tar.seekg((std::streamoff)info.data_offset, std::ios::beg);
            if (member.name == "eng.sh_quant.q.npy") {
                packed.resize((size_t)info.data_bytes);
                tar.read(reinterpret_cast<char*>(packed.data()),
                         (std::streamsize)packed.size());
            } else if (info.data_bytes == sizeof bounds) {
                tar.read(reinterpret_cast<char*>(&bounds), sizeof bounds);
            }
            if (!tar) return false;
        }
        if (packed.size() != 12) return false;
        const uint8_t source_packed[6] = {0x00, 0xff, 0x78, 0, 0, 0};
        const float4 source_bounds{-1.0f, 1.0f, 0.0f, 30.0f};
        for (int64_t i = 0; i < 3; ++i) {
            const float2 expected =
                QuantizedAdamState<4>::decode_g1g2(
                    source_packed, i, source_bounds);
            const float2 actual =
                QuantizedAdamState<4>::decode_g1g2(
                    packed.data(), i, bounds);
            const float tolerance =
                1e-5f * std::max(1.0f, std::abs(expected.x));
            if (std::abs(actual.x - expected.x) > tolerance ||
                std::abs(actual.y - expected.y) > tolerance)
                return false;
        }
        return true;
    } catch (const std::exception&) {
    }
    return false;
}
void write_invalid_grid_checkpoint(const fs::path& dir, int step) {
    const std::string state =
        "{\"format_version\":2,\"full_resume\":0,\"step\":" +
        std::to_string(step) +
        ",\"cur_num_splats\":1,\"max_num_splats\":1,\"num_sh\":0,"
        "\"bilagrid_rgb\":{\"enabled\":1,\"C\":1,\"L\":0,\"H\":1,"
        "\"W\":1,\"value_bits\":32},\"arrays\":["
        "{\"name\":\"world.opacities\",\"descr\":\"<f4\",\"bytes\":4},"
        "{\"name\":\"eng.bg.rgb.grids\",\"descr\":\"<f4\",\"bytes\":4}]}";
    fs::create_directories(dir);
    std::ofstream tar(dir / "state.tar", std::ios::binary);
    ckpt::tar_write_bytes(tar, "state.json", state.data(), state.size());
    write_f32(tar, "world.opacities.npy", 1);
    write_f32(tar, "eng.bg.rgb.grids.npy", 1);
    ckpt::tar_finish(tar);
}

bool invalid_grid_is_rejected(const fs::path& source,
                              const fs::path& adapted) {
    ckpt::TargetLayout target;
    target.max_num_splats = 1;
    target.num_images = 1;
    target.bilagrid_rgb = std::array<int, 3>{1, 1, 1};
    try {
        ckpt::adapt_checkpoint(source, target, adapted);
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

enum class AppearanceFixture {
    MissingBilagridOptimizer,
    MissingPpispOptimizer,
    BadBackgroundSize,
    Bilagrid,
    Ppisp,
};

void write_appearance_checkpoint(const fs::path& dir, int step,
                                 AppearanceFixture fixture) {
    std::vector<std::pair<std::string, size_t>> arrays = {
        {"world.means", 3}, {"world.quats", 4}, {"world.scales", 3},
        {"world.opacities", 1}, {"world.features_dc", 3},
        {"eng.radii", 1}, {"eng.accum_buffer", 2},
    };
    for (const auto& [name, numel] :
         {std::pair{"means", 3u}, std::pair{"quats", 4u},
          std::pair{"scales", 3u}, std::pair{"opacities", 1u},
          std::pair{"features_dc", 3u}}) {
        arrays.emplace_back("eng.g1_" + std::string(name), numel);
        arrays.emplace_back("eng.g2_" + std::string(name), numel);
    }

    std::string appearance;
    if (fixture == AppearanceFixture::MissingPpispOptimizer ||
        fixture == AppearanceFixture::Ppisp) {
        arrays.emplace_back("eng.ppisp.params", 9);
        appearance = "\"ppisp\":{\"enabled\":1,\"num_params\":9,"
                     "\"use_adagrad\":1},";
        if (fixture == AppearanceFixture::Ppisp)
            arrays.emplace_back("eng.ppisp.accum", 9);
    } else if (fixture == AppearanceFixture::MissingBilagridOptimizer ||
               fixture == AppearanceFixture::Bilagrid) {
        arrays.emplace_back("eng.bg.rgb.grids", 3);
        appearance = "\"bilagrid_rgb\":{\"enabled\":1,\"type\":\"affine\","
                     "\"C\":3,\"L\":1,\"H\":1,\"W\":1,\"optim_bits\":32,"
                     "\"value_bits\":32,\"use_adagrad\":1},";
        if (fixture == AppearanceFixture::Bilagrid)
            arrays.emplace_back("eng.bg.rgb.accum", 3);
    } else {
        arrays.emplace_back("eng.bg_sky.sh_coeffs", 4 * 3);
        arrays.emplace_back("eng.bg_sky.g1", 4 * 3);
        arrays.emplace_back("eng.bg_sky.g2", 4 * 3);
        appearance = "\"background_sh\":{\"enabled\":1,\"degree\":2},";
    }

    std::ostringstream manifest;
    for (size_t i = 0; i < arrays.size(); ++i) {
        if (i != 0) manifest << ',';
        manifest << "{\"name\":\"" << arrays[i].first
                 << "\",\"descr\":\"<f4\",\"bytes\":"
                 << arrays[i].second * sizeof(float) << '}';
    }
    const std::string state =
        "{\"format_version\":2,\"full_resume\":1,\"step\":" +
        std::to_string(step) +
        ",\"cur_num_splats\":1,\"max_num_splats\":1,\"num_sh\":0,"
        "\"sh_optim_bits\":32,\"sh_value_bits\":32,"
        "\"non_sh_optim_bits\":32,\"use_fused_proj_bwd_optim\":0," +
        appearance + "\"arrays\":[" + manifest.str() + "]}";

    fs::create_directories(dir);
    std::ofstream tar(dir / "state.tar", std::ios::binary);
    ckpt::tar_write_bytes(tar, "state.json", state.data(), state.size());
    for (const auto& [name, numel] : arrays)
        write_f32(tar, name + ".npy", numel);
    ckpt::tar_finish(tar);
}

bool adapted_bilagrid_is_valid(const fs::path& source,
                               const fs::path& adapted, bool drop) {
    ckpt::TargetLayout target;
    target.max_num_splats = 1;
    target.num_images = 1;
    target.bilagrid_use_adagrad = true;
    target.bilagrid_type = "affine";
    if (!drop) target.bilagrid_rgb = std::array<int, 3>{2, 1, 1};
    try {
        ckpt::check_resumable(source);
        if (!ckpt::adapt_checkpoint(source, target, adapted)) return false;
        ckpt::check_resumable(adapted);
        const JsonValue state = ckpt::read_state_json(adapted);
        const JsonValue* grid = state.find("bilagrid_rgb");
        return grid && grid->get_double("enabled", -1) == (drop ? 0 : 1) &&
               (drop || grid->get_double("L", 0) == 2);
    } catch (const std::exception&) {
        return false;
    }
}

bool adapted_ppisp_drop_is_valid(const fs::path& source,
                                 const fs::path& adapted) {
    ckpt::TargetLayout target;
    target.max_num_splats = 1;
    target.num_images = 1;
    try {
        ckpt::check_resumable(source);
        if (!ckpt::adapt_checkpoint(source, target, adapted)) return false;
        ckpt::check_resumable(adapted);
        const JsonValue state = ckpt::read_state_json(adapted);
        const JsonValue* ppisp = state.find("ppisp");
        return ppisp && ppisp->get_double("enabled", -1) == 0;
    } catch (const std::exception&) {
        return false;
    }
}

bool adapted_legacy_is_valid(const fs::path& source,
                             const fs::path& adapted) {
    ckpt::TargetLayout target;
    target.max_num_splats = 2;
    target.num_images = 1;
    try {
        if (!ckpt::adapt_checkpoint(source, target, adapted)) return false;
        ckpt::check_resumable(adapted);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool finish_rejects_failed_stream() {
    std::ostringstream tar;
    tar.setstate(std::ios::badbit);
    try {
        ckpt::tar_finish(tar);
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

bool resolve_fails(const fs::path& path) {
    try {
        ckpt::resolve_checkpoint(path);
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

bool resume_check_fails(const fs::path& path) {
    try {
        ckpt::check_resumable(path);
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

bool needs_adapt_fails(const std::string& state,
                       const ckpt::TargetLayout& target) {
    try {
        ckpt::needs_adapt(json_parse(state), target);
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

std::string layout_state(const std::string& feature) {
    return "{\"max_num_splats\":1,\"cur_num_splats\":1,\"num_sh\":0,"
           "\"sh_optim_bits\":32,\"sh_value_bits\":32,"
           "\"non_sh_optim_bits\":32,\"use_fused_proj_bwd_optim\":0," +
           feature + "}";
}

bool needs_adapt_changes(const std::string& state,
                         const ckpt::TargetLayout& target) {
    try {
        return ckpt::needs_adapt(json_parse(state), target);
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace

int main() {
    ckpt::B1Contract a;
    a.owner = 0; a.source_step = 10; a.completed_step = 12;
    a.horizon = 30; a.rows = 257; a.frozen_context = std::string(64, '1');
    const std::string digest(64, 'a');
    a.source_state_sha256 = a.config_sha256 = a.views_sha256 =
        a.train_indices_sha256 = a.full_train_indices_sha256 = a.mask_sha256 = a.frozen_shared_sha256 =
        a.blocks_sha256 = a.owned_ids_sha256 = a.excluded_ids_sha256 = a.ply_sha256 = digest;
    auto b = a; b.owner = 1;
    b.frozen_context = std::string(64, '2'); b.mask_sha256 = std::string(64, 'b');
    b.train_indices_sha256 = std::string(64, 'c');
    auto pair_rejected = [&](const ckpt::B1Contract& right) {
        try { ckpt::validate_b1_pair(a, right); return false; }
        catch (const std::exception&) { return true; }
    };
    check(!pair_rejected(b), "complementary owners allow owner-specific context and view indices");
    auto changed = b; changed.completed_step++;
    check(pair_rejected(changed), "rejects unequal completed owner schedule");
    changed = b; changed.source_state_sha256[0] = 'b';
    check(pair_rejected(changed), "rejects changed original source");
    changed = b; changed.config_sha256[0] = 'b';
    check(pair_rejected(changed), "rejects different numerical configuration");
    changed = b; changed.owner = 0;
    check(pair_rejected(changed), "rejects duplicate owner");
    changed = b; changed.sampler_version++;
    check(pair_rejected(changed), "rejects incompatible sampler");
    changed = b; changed.full_train_indices_sha256[0] = 'b';
    check(pair_rejected(changed), "rejects different canonical training identity");
    changed = b; changed.view_policy = "full";
    changed.train_indices_sha256 = changed.full_train_indices_sha256;
    check(pair_rejected(changed), "rejects mixed view policies");
    changed = b; changed.version = 1;
    check(pair_rejected(changed), "rejects mixed checkpoint schema versions");
    auto contract_rejected = [](const JsonValue& json) {
        try { ckpt::parse_b1_contract(json); return false; }
        catch (const std::exception&) { return true; }
    };
    for (const char* missing : {"view_policy", "full_train_indices_sha256"}) {
        auto json = ckpt::b1_contract_json(a);
        json.obj.erase(std::remove_if(json.obj.begin(), json.obj.end(),
            [&](const auto& field) { return field.first == missing; }), json.obj.end());
        check(contract_rejected(json), "rejects missing v2 view identity");
    }
    changed = a; changed.view_policy = "unknown";
    check(contract_rejected(ckpt::b1_contract_json(changed)), "rejects unknown view policy");
    changed = a; changed.full_train_indices_sha256 = "invalid";
    check(contract_rejected(ckpt::b1_contract_json(changed)), "rejects malformed canonical training digest");
    auto unknown_field = ckpt::b1_contract_json(a);
    for (auto& field : unknown_field.obj)
        if (field.first == "view_policy") field.first = "alternate_view_policy";
    check(contract_rejected(unknown_field), "rejects unknown v2 field replacing required policy");
    auto full_left = a; full_left.view_policy = "full";
    auto full_right = full_left; full_right.owner = 1;
    bool full_pair_valid = false;
    try { ckpt::validate_b1_pair(full_left, full_right); full_pair_valid = true; }
    catch (const std::exception&) {}
    check(full_pair_valid, "accepts full owners with identical canonical training identity");
    full_left.train_indices_sha256 = full_right.train_indices_sha256 = std::string(64, 'c');
    bool incomplete_full_rejected = false;
    try { ckpt::validate_b1_pair(full_left, full_right); }
    catch (const std::exception&) { incomplete_full_rejected = true; }
    check(incomplete_full_rejected, "rejects two matching full subsets that differ from canonical coverage");
    auto legacy_left = a; legacy_left.version = 1;
    auto legacy_right = b; legacy_right.version = 1;
    bool legacy_pair_valid = false;
    try { ckpt::validate_b1_pair(legacy_left, legacy_right); legacy_pair_valid = true; }
    catch (const std::exception&) {}
    check(legacy_pair_valid, "accepts historical partitioned pair");
    const auto legacy_json = ckpt::b1_contract_json(legacy_left);
    check(!legacy_json.find("view_policy") && !legacy_json.find("full_train_indices_sha256"),
          "legacy serialization preserves original schema");
    const auto legacy_parsed = ckpt::parse_b1_contract(legacy_json);
    check(legacy_parsed.view_policy == "partitioned" && legacy_parsed.full_train_indices_sha256.empty(),
          "historical owners retain partitioned semantics without inferred canonical coverage");
    legacy_left.owner = 2;
    const auto legacy_all = ckpt::parse_b1_contract(ckpt::b1_contract_json(legacy_left));
    check(legacy_all.view_policy == "full" && legacy_all.full_train_indices_sha256 == legacy_all.train_indices_sha256,
          "historical all-owned reference retains full native semantics");
    changed = a; changed.owner = 2;
    check(contract_rejected(ckpt::b1_contract_json(changed)), "rejects partitioned v2 all-owned reference");
    auto duplicate = ckpt::b1_contract_json(a);
    duplicate.obj.push_back(duplicate.obj.front());
    bool duplicate_rejected = false;
    try { ckpt::parse_b1_contract(duplicate); }
    catch (const std::exception&) { duplicate_rejected = true; }
    check(duplicate_rejected, "rejects duplicate contract field");
    auto fractional = ckpt::b1_contract_json(a);
    for (auto& field : fractional.obj)
        if (field.first == "completed_step") field.second.num = 12.5;
    bool fractional_rejected = false;
    try { ckpt::parse_b1_contract(fractional); }
    catch (const std::exception&) { fractional_rejected = true; }
    check(fractional_rejected, "rejects fractional schedule position");
    auto live_left = a;
    live_left.version = 3; live_left.view_policy = "full";
    live_left.refresh_cadence = 2;
    live_left.refresh_generation = live_left.refresh_parent = live_left.source_step;
    live_left.refresh_group_sha256 = live_left.refresh_commit_sha256 = digest;
    live_left.refresh_payload0_sha256 = live_left.refresh_payload1_sha256 = std::string(64, '0');
    auto live_right = live_left; live_right.owner = 1;
    live_right.frozen_context = std::string(64, '2');
    auto live_pair_rejected = [&](const ckpt::B1Contract& right) {
        try { ckpt::validate_b1_pair(live_left, right); return false; }
        catch (const std::exception&) { return true; }
    };
    check(!live_pair_rejected(live_right), "initial live context pair accepts receiver-specific frozen hashes");
    for (const char* field : {"refresh_cadence", "refresh_generation", "refresh_parent",
            "refresh_group_sha256", "refresh_payload0_sha256", "refresh_payload1_sha256",
            "refresh_commit_sha256"}) {
        auto json = ckpt::b1_contract_json(live_left);
        json.obj.erase(std::remove_if(json.obj.begin(), json.obj.end(),
            [&](const auto& entry) { return entry.first == field; }), json.obj.end());
        check(contract_rejected(json), "rejects missing live context binding");
    }
    changed = live_left; changed.refresh_cadence = 0;
    check(contract_rejected(ckpt::b1_contract_json(changed)), "rejects zero refresh cadence");
    changed = live_left; changed.refresh_parent = 9;
    check(contract_rejected(ckpt::b1_contract_json(changed)), "rejects wrong initial parent");
    changed = live_left; changed.refresh_payload0_sha256 = digest;
    check(contract_rejected(ckpt::b1_contract_json(changed)), "rejects partial initial payload");
    changed = live_left; changed.refresh_commit_sha256 = std::string(64, 'b');
    check(contract_rejected(ckpt::b1_contract_json(changed)), "rejects initial commit not bound to group");
    changed = live_left; changed.completed_step = 13;
    check(contract_rejected(ckpt::b1_contract_json(changed)), "rejects generation stale by more than cadence");
    changed = live_left; changed.view_policy = "partitioned";
    check(contract_rejected(ckpt::b1_contract_json(changed)), "rejects partitioned live context");
    changed = live_left; changed.owner = 2;
    check(contract_rejected(ckpt::b1_contract_json(changed)), "rejects all-owned live context");
    live_left.refresh_generation = 12;
    live_left.refresh_payload0_sha256 = std::string(64, 'b');
    live_left.refresh_payload1_sha256 = std::string(64, 'c');
    live_left.refresh_commit_sha256 = std::string(64, 'd');
    live_right = live_left; live_right.owner = 1;
    live_right.frozen_context = std::string(64, '2');
    check(!live_pair_rejected(live_right), "accepts installed matching live generation");
    for (auto member : {&ckpt::B1Contract::refresh_group_sha256,
            &ckpt::B1Contract::refresh_payload0_sha256, &ckpt::B1Contract::refresh_payload1_sha256,
            &ckpt::B1Contract::refresh_commit_sha256}) {
        changed = live_right; changed.*member = std::string(64, 'e');
        check(live_pair_rejected(changed), "rejects mixed live payload/group/commit identities");
        changed = live_left; changed.*member = "invalid";
        check(contract_rejected(ckpt::b1_contract_json(changed)), "rejects malformed live digest");
    }
    changed = live_right; changed.refresh_generation = changed.refresh_parent = 10;
    changed.refresh_payload0_sha256 = changed.refresh_payload1_sha256 = std::string(64, '0');
    changed.refresh_commit_sha256 = changed.refresh_group_sha256;
    check(live_pair_rejected(changed), "rejects individually valid owners across refresh barrier");
    changed = live_left; changed.refresh_parent = 11;
    check(contract_rejected(ckpt::b1_contract_json(changed)), "rejects skipped parent lineage");
    changed = live_left; changed.refresh_generation = 11;
    check(contract_rejected(ckpt::b1_contract_json(changed)), "rejects off-cadence generation");
    changed = live_left; changed.refresh_generation = 14;
    check(contract_rejected(ckpt::b1_contract_json(changed)), "rejects future generation");
    changed = live_left; changed.refresh_payload1_sha256 = std::string(64, '0');
    check(contract_rejected(ckpt::b1_contract_json(changed)), "rejects partial installed payload pair");
    changed = live_left; changed.refresh_commit_sha256 = changed.refresh_group_sha256;
    check(contract_rejected(ckpt::b1_contract_json(changed)), "rejects installed payload with initial commit");
    changed = live_right; changed.version = 2;
    check(live_pair_rejected(changed), "rejects mixed frozen/live schemas");
    auto live_policy = ckpt::b1_contract_json(live_left);
    for (auto& field : live_policy.obj)
        if (field.first == "policy") field.second.str = "native_fpbo_fixed_context_v1";
    check(contract_rejected(live_policy), "rejects live fields under historical frozen policy");
    auto historical_extra = ckpt::b1_contract_json(a);
    historical_extra.obj.push_back({"refresh_generation", json_parse("10")});
    check(contract_rejected(historical_extra), "rejects refresh fields in historical schema");
    changed = live_right; changed.refresh_cadence = 1; changed.refresh_parent = 11;
    check(live_pair_rejected(changed), "rejects different otherwise valid cadence and lineage");
    for (const char* name : {"refresh_cadence", "refresh_generation", "refresh_parent"}) {
        for (const char* value : {"-1", "1.5", "2147483648", "\"12\""}) {
            auto json = ckpt::b1_contract_json(live_left);
            for (auto& field : json.obj)
                if (field.first == name) field.second = json_parse(value);
            check(contract_rejected(json), "rejects invalid numeric refresh binding");
        }
    }
    TrainConfig numerical;
    const auto original_hash = ckpt::b1_config_hash(numerical);
    numerical.resume = "owner"; numerical.output_dir_name = "attempt";
    numerical.steps_per_save = 1; numerical.save_full_checkpoint = true;
    check(ckpt::b1_config_hash(numerical) == original_hash,
          "output and checkpoint logistics do not change numerical identity");
    numerical.eval_mode = "fraction";
    check(ckpt::b1_config_hash(numerical) != original_hash,
          "training split remains part of numerical identity");
    numerical.eval_mode = "all"; numerical.num_iterations++;
    check(ckpt::b1_config_hash(numerical) != original_hash,
          "original learning-rate horizon remains bound");
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root =
        fs::temp_directory_path() /
        ("spirula_checkpoint_resolution_" + std::to_string(nonce));
    std::error_code ec;
    fs::create_directories(root, ec);
    const auto marked = root / "marked.ckpt";
    fs::create_directories(marked);
    {
        std::ofstream tar(marked / "state.tar", std::ios::binary);
        const auto state = std::string("{\"format_version\":2,\"arrays\":[],\"full_resume\":1,\"step\":12,\"cur_num_splats\":257,\"b1\":") +
            ckpt::serialize_b1_contract(a) + "}";
        ckpt::tar_write_bytes(tar, "state.json", state.data(), state.size());
        ckpt::tar_finish(tar);
    }
    bool lost_sidecars_rejected = false;
    try { ckpt::read_b1_contract(marked); }
    catch (const std::exception&) { lost_sidecars_rejected = true; }
    check(lost_sidecars_rejected, "embedded owner marker rejects complete sidecar loss");
    ckpt::TargetLayout marked_target;
    marked_target.max_num_splats = 257;
    bool marked_adaptation_rejected = false;
    try { ckpt::adapt_checkpoint(marked, marked_target, root / "adapted-owner"); }
    catch (const std::exception&) { marked_adaptation_rejected = true; }
    check(marked_adaptation_rejected, "direct adaptation rejects marked owner checkpoint");
    auto write_marked_state = [&] {
        std::ofstream tar(marked / "state.tar", std::ios::binary);
        const auto state = std::string("{\"format_version\":2,\"arrays\":[],\"full_resume\":1,\"step\":12,\"cur_num_splats\":257,\"b1\":") +
            ckpt::serialize_b1_contract(a) + "}";
        ckpt::tar_write_bytes(tar, "state.json", state.data(), state.size());
        ckpt::tar_finish(tar);
    };
    auto put_le = [](std::ostream& out, uint64_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) out.put((char)(value >> (8 * i)));
    };
    {
        std::ofstream blocks(marked / "b1-owned-blocks.bin", std::ios::binary);
        put_le(blocks, 257, 8); put_le(blocks, 1, 8); put_le(blocks, 12, 8);
        put_le(blocks, 2, 4); put_le(blocks, 0, 4);
        put_le(blocks, 1, 4); put_le(blocks, 0, 4);
        std::ofstream ids(marked / "b1-owned-ids.bin", std::ios::binary);
        put_le(ids, 256, 8);
        for (uint32_t id = 0; id < 256; ++id) put_le(ids, id, 4);
        std::ofstream excluded(marked / "b1-excluded-ids.bin", std::ios::binary);
        put_le(excluded, 0, 8);
    }
    std::ofstream(marked / "config.json") << "{}";
    a.config_sha256 = ckpt::b1_config_hash(TrainConfig{});
    const std::string owner_ply =
        "ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\nend_header\n1 2 3\n";
    std::ofstream(marked / "splat.ply", std::ios::binary) << owner_ply;
    a.ply_sha256 = ckpt::b1_file_hash(marked / "splat.ply");
    a.frozen_shared_sha256 = ckpt::b1_shared_hash(marked);
    ckpt::bind_b1_sidecars(a, marked);
    write_marked_state();
    bool valid_marked = false;
    try { valid_marked = ckpt::read_b1_contract(marked).has_value(); }
    catch (const std::exception&) {}
    check(valid_marked, "accepts complete native owner artifact binding");
    auto mutated_ply = owner_ply;
    mutated_ply.replace(mutated_ply.size() - 6, 1, "9");
    std::ofstream(marked / "splat.ply", std::ios::binary) << mutated_ply;
    bool changed_ply_rejected = false;
    try { ckpt::read_b1_contract(marked); }
    catch (const std::exception&) { changed_ply_rejected = true; }
    check(changed_ply_rejected, "rejects same-count owner PLY coordinate mutation");
    std::ofstream(marked / "splat.ply", std::ios::binary) << owner_ply;
    const auto appearance_source = root / "appearance-source";
    fs::create_directories(appearance_source);
    {
        std::ofstream tar(appearance_source / "state.tar", std::ios::binary);
        const std::string state = "{\"format_version\":2,\"arrays\":[]}";
        ckpt::tar_write_bytes(tar, "state.json", state.data(), state.size());
        ckpt::tar_finish(tar);
    }
    auto appearance = a;
    appearance.source_state_sha256 = ckpt::b1_file_hash(appearance_source / "state.tar");
    appearance.frozen_shared_sha256 = ckpt::b1_shared_hash(appearance_source);
    const auto merged_ply = root / "merged.ply";
    std::ofstream(merged_ply, std::ios::binary) << owner_ply;
    const auto body_digest = ckpt::b1_ply_body_hash(merged_ply);
    const auto merged_bytes = std::string("ply\n") +
        ckpt::b1_appearance_comment(appearance, body_digest) + owner_ply.substr(4);
    std::ofstream(merged_ply, std::ios::binary) << merged_bytes;
    bool valid_appearance = false;
    try { ckpt::validate_b1_appearance(merged_ply, appearance_source); valid_appearance = true; }
    catch (const std::exception&) {}
    check(valid_appearance, "accepts merged PLY with embedded source and body binding");
    bool wrong_source_rejected = false;
    try { ckpt::validate_b1_appearance(merged_ply, marked); }
    catch (const std::exception&) { wrong_source_rejected = true; }
    check(wrong_source_rejected, "rejects merged appearance from a different source");
    auto changed_merged = merged_bytes;
    changed_merged.replace(changed_merged.size() - 6, 1, "9");
    std::ofstream(merged_ply, std::ios::binary) << changed_merged;
    bool merged_mutation_rejected = false;
    try { ckpt::validate_b1_appearance(merged_ply, appearance_source); }
    catch (const std::exception&) { merged_mutation_rejected = true; }
    check(merged_mutation_rejected, "rejects mutated merged PLY body");

    const fs::path first = root / "step-000001000.ckpt";
    write_checkpoint(first, 1000);
    check(ckpt::resolve_checkpoint(root).ckpt_dir == first,
          "selects the only valid checkpoint");

    const fs::path staging = root / ".staging-step-000002000.ckpt-42";
    write_checkpoint(staging, 2000);
    check(ckpt::resolve_checkpoint(root).ckpt_dir == first,
          "ignores a complete but unpublished staging directory");
    check(resolve_fails(staging),
          "rejects an explicitly selected unpublished staging directory");

    const fs::path second = root / "step-000002000.ckpt";
    write_checkpoint(second, 2000);
    check(ckpt::resolve_checkpoint(root).ckpt_dir == second,
          "selects the latest valid checkpoint");
    std::ofstream(root / "config.json") << "{\"cap_max\":2}";
    std::ofstream(second / "config.json") << "{\"cap_max\":1,\"sh_degree\":0}";
    TrainConfig cli;
    cli.resume = root.string();
    check(ckpt::build_resume_config(cli, "", {}).cap_max == 1,
          "uses the selected checkpoint's config");

    const fs::path corrupt = root / "step-000003000.ckpt";
    write_interrupted_checkpoint(corrupt, 3000);
    check(ckpt::resolve_checkpoint(root).ckpt_dir == second,
          "keeps the previous checkpoint when the latest is incomplete");
    check(resolve_fails(corrupt), "rejects an archive without its terminator");

    const fs::path bad_npy = root / "step-000004000.ckpt";
    write_invalid_npy_checkpoint(bad_npy, 4000);
    check(ckpt::resolve_checkpoint(root).ckpt_dir == second,
          "falls back when a NumPy member is invalid");
    check(resolve_fails(bad_npy), "rejects an invalid NumPy payload");

    const fs::path short_quant = root / "step-000005000.ckpt";
    write_short_quant_checkpoint(short_quant, 5000);
    check(ckpt::resolve_checkpoint(root).ckpt_dir == second,
          "falls back when quantized optimizer data is short");
    const fs::path missing_radii = root / "step-000006000.ckpt";
    write_missing_radii_checkpoint(missing_radii, 6000);
    check(ckpt::resolve_checkpoint(root).ckpt_dir == second,
          "falls back when densification state is missing");

    const fs::path missing_manifest = root / "step-000007000.ckpt";
    write_missing_manifest_member_checkpoint(missing_manifest, 7000);
    check(ckpt::resolve_checkpoint(root).ckpt_dir == second,
          "falls back when a manifested array is missing");
    check(resolve_fails(missing_manifest),
          "rejects a missing manifested array directly");
    const fs::path adapt_source = root / "step-000008000.ckpt";
    const fs::path adapted = root / ".adapted";
    write_adapt_source(adapt_source, 8000);
    check(adapted_checkpoint_is_valid(adapt_source, adapted),
          "rewrites adapted manifests and omits zero-sized SH arrays");
    const fs::path adapt_q4_source = root / "step-000009000.ckpt";
    const fs::path adapted_q4 = root / ".adapted-q4";
    write_adapt_q4_source(adapt_q4_source, 9000);
    check(adapted_q4_is_valid(adapt_q4_source, adapted_q4),
          "preserves four-bit SH optimizer storage during adaptation");
    const fs::path invalid_grid = root / "step-000010000.ckpt";
    write_invalid_grid_checkpoint(invalid_grid, 10000);
    check(invalid_grid_is_rejected(invalid_grid, root / ".adapted-grid"),
          "rejects invalid bilagrid dimensions before adaptation");
    const fs::path missing_ppisp = root / "step-000011000.ckpt";
    write_appearance_checkpoint(
        missing_ppisp, 11000, AppearanceFixture::MissingPpispOptimizer);
    check(resume_check_fails(missing_ppisp),
          "rejects missing PPISP optimizer state");
    const fs::path missing_bilagrid = root / "step-000012000.ckpt";
    write_appearance_checkpoint(
        missing_bilagrid, 12000,
        AppearanceFixture::MissingBilagridOptimizer);
    check(resume_check_fails(missing_bilagrid),
          "rejects missing bilagrid optimizer state");
    const fs::path bad_background = root / "step-000013000.ckpt";
    write_appearance_checkpoint(
        bad_background, 13000, AppearanceFixture::BadBackgroundSize);
    check(resume_check_fails(bad_background),
          "rejects background SH payload with the wrong degree size");
    const fs::path ppisp_source = root / "step-000015000.ckpt";
    write_appearance_checkpoint(
        ppisp_source, 15000, AppearanceFixture::Ppisp);
    check(adapted_ppisp_drop_is_valid(
              ppisp_source, root / ".adapted-ppisp-drop"),
          "disables dropped PPISP metadata");
    const fs::path bilagrid_source = root / "step-000014000.ckpt";
    write_appearance_checkpoint(
        bilagrid_source, 14000, AppearanceFixture::Bilagrid);
    check(adapted_bilagrid_is_valid(
              bilagrid_source, root / ".adapted-bilagrid", false),
          "rewrites bilagrid dimensions in adapted state");
    check(adapted_bilagrid_is_valid(
              bilagrid_source, root / ".adapted-bilagrid-drop", true),
          "disables dropped bilagrid metadata");
    ckpt::TargetLayout background_target;
    background_target.max_num_splats = 1;
    background_target.num_images = 1;
    check(needs_adapt_fails(
              layout_state(
                  "\"background_sh\":{\"enabled\":1,\"degree\":4}"),
              background_target),
          "rejects incompatible background SH layouts");
    ckpt::TargetLayout absent_background_target;
    absent_background_target.max_num_splats = 1;
    absent_background_target.num_images = 1;
    absent_background_target.background_sh = true;
    absent_background_target.background_sh_degree = 4;
    check(needs_adapt_fails(
              layout_state("\"ppisp\":{\"enabled\":0}"),
              absent_background_target),
          "rejects enabling background SH without saved metadata");
    ckpt::TargetLayout ppisp_target;
    ppisp_target.max_num_splats = 1;
    ppisp_target.num_images = 1;
    ppisp_target.ppisp = true;
    ppisp_target.ppisp_param_type = "no_crf_no_vig";
    check(needs_adapt_fails(
              layout_state(
                  "\"ppisp\":{\"enabled\":1,\"param_type\":\"original\","
                  "\"use_adagrad\":0}"),
              ppisp_target),
          "rejects incompatible PPISP parameter layouts");
    check(needs_adapt_fails(
              layout_state("\"ppisp\":{\"enabled\":0}"), ppisp_target),
          "rejects adding PPISP during resume");
    ckpt::TargetLayout bilagrid_target;
    bilagrid_target.max_num_splats = 1;
    bilagrid_target.num_images = 1;
    bilagrid_target.bilagrid_rgb = std::array<int, 3>{1, 1, 1};
    bilagrid_target.bilagrid_type = "affine";
    check(needs_adapt_fails(
              layout_state("\"bilagrid_rgb\":{\"enabled\":0}"),
              bilagrid_target),
          "rejects adding bilagrid during resume");
    ckpt::TargetLayout quant_target;
    quant_target.max_num_splats = 2;
    quant_target.num_images = 1;
    quant_target.sh_optim_bits = 8;
    quant_target.sh_value_bits = 16;
    quant_target.non_sh_optim_bits = 16;
    check(needs_adapt_fails(layout_state("\"ppisp\":{\"enabled\":0}"),
                            quant_target),
          "rejects combined cap and quantized layout changes");
    check(finish_rejects_failed_stream(), "rejects a failed archive stream");

    ckpt::TargetLayout legacy_target;
    legacy_target.max_num_splats = 2;
    check(adapted_legacy_is_valid(first, root / ".adapted-legacy"),
          "adapts legacy fp32 checkpoint archives");
    legacy_target.num_images = 1;
    check(needs_adapt_changes(
              "{\"format_version\":1,\"max_num_splats\":1,"
              "\"cur_num_splats\":1,\"num_sh\":0}",
              legacy_target),
          "adapts legacy checkpoints with implicit fp32 layout");
    fs::remove_all(root, ec);
    if (failures) {
        std::printf("FAILED with %d error(s)\n", failures);
        return 1;
    }
    std::printf("All checkpoint resolution tests passed.\n");
    return 0;
}
