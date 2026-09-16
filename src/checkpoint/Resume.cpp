// Resume.cpp -- see Resume.h.

#include "checkpoint/Resume.h"

#include "config/TrainConfigJson.h"
#include "core/CheckpointIO.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

namespace ckpt {

namespace {

// Read state.tar once and hand back both its member index and the stream.
// Callers want either state.json or the member name list, usually both.
struct TarView {
    std::ifstream in;
    std::vector<TarMember> members;
    std::map<std::string, NpyInfo> arrays;
};

TarView open_state_tar(const fs::path& ckpt_dir) {
    fs::path tarpath = ckpt_dir / "state.tar";
    TarView v;
    v.in.open(tarpath.string(), std::ios::binary);
    if (!v.in)
        throw std::runtime_error("cannot open " + tarpath.string() +
                                 " (not a checkpoint directory)");
    v.members = tar_index(v.in);
    for (const auto& m : v.members) {
        if (m.name.size() < 4 ||
            m.name.compare(m.name.size() - 4, 4, ".npy") != 0)
            continue;
        const std::string base = m.name.substr(0, m.name.size() - 4);
        PoolSlot slot;
        uint32_t sub;
        if (!parse_saved_name(base, slot, sub))
            throw std::runtime_error("unknown checkpoint slot " + base);
        const NpyInfo info = npy_locate(v.in, m.data_offset, m.size);
        if (!v.arrays.emplace(base, info).second)
            throw std::runtime_error("duplicate checkpoint slot " + base);
    }
    return v;
}

std::string read_member(TarView& v, const std::string& name) {
    for (const auto& m : v.members)
        if (m.name == name) return tar_read_member(v.in, m);
    return {};
}

constexpr double kMaxExactJsonInteger = 9007199254740991.0;

bool exact_integer(double value, double minimum, double maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum &&
           std::trunc(value) == value;
}

bool has_complete_manifest(const TarView& v, const JsonValue& state) {
    const double format = state.get_double("format_version", 1);
    if (format == 1) return true;
    if (format != 2) return false;

    const JsonValue* manifest = state.find("arrays");
    if (!manifest || !manifest->is_array()) return false;
    std::set<std::string> seen;
    for (const JsonValue& item : manifest->arr) {
        if (!item.is_object()) return false;
        const JsonValue* name_value = item.find("name");
        const JsonValue* descr_value = item.find("descr");
        const JsonValue* bytes_value = item.find("bytes");
        if (!name_value || !descr_value || !bytes_value ||
            name_value->type != JsonValue::Type::String ||
            descr_value->type != JsonValue::Type::String ||
            bytes_value->type != JsonValue::Type::Number)
            return false;
        const std::string& name = name_value->as_string();
        const double raw_bytes = bytes_value->as_double(-1);
        if (name.empty() ||
            !exact_integer(raw_bytes, 0, kMaxExactJsonInteger))
            return false;
        const uint64_t bytes = (uint64_t)raw_bytes;
        const auto actual = v.arrays.find(name);
        if ((double)bytes != raw_bytes || !seen.insert(name).second ||
            actual == v.arrays.end() ||
            actual->second.descr != descr_value->as_string() ||
            actual->second.data_bytes != bytes)
            return false;
    }
    return seen.size() == v.arrays.size();
}

bool has_resume_payload(const TarView& v, const JsonValue& state) {
    const JsonValue* full = state.find("full_resume");
    const double raw_max = state.get_double("max_num_splats", 0);
    const double raw_cur = state.get_double("cur_num_splats", -1);
    const double raw_num_sh = state.get_double("num_sh", -1);
    if (!full || full->as_double(0) != 1 ||
        !exact_integer(raw_max, 1, std::numeric_limits<int32_t>::max()) ||
        !exact_integer(raw_cur, 0, raw_max) ||
        !exact_integer(raw_num_sh, 0, 1024))
        return false;
    const int64_t max_splats = (int64_t)raw_max;
    const int num_sh = (int)raw_num_sh;

    auto f32 = [&](const std::string& name, uint64_t channels) {
        const auto it = v.arrays.find(name);
        const uint64_t splats = (uint64_t)max_splats;
        if (it == v.arrays.end() || it->second.descr != "<f4" ||
            splats > std::numeric_limits<uint64_t>::max() / sizeof(float))
            return false;
        const uint64_t bytes_per_channel = splats * sizeof(float);
        if (channels > std::numeric_limits<uint64_t>::max() / bytes_per_channel)
            return false;
        return it->second.data_bytes == channels * bytes_per_channel;
    };
    auto exact = [&](const std::string& name, const char* descr,
                     uint64_t bytes) {
        const auto it = v.arrays.find(name);
        return it != v.arrays.end() && it->second.descr == descr &&
               it->second.data_bytes == bytes;
    };
    auto pair = [&](const std::string& name, uint64_t packed_bytes,
                    uint64_t bounds_bytes) {
        return exact(name + ".q", "|u1", packed_bytes) &&
               exact(name + ".qb", "<f4", bounds_bytes);
    };
    auto array = [&](const std::string& name, const char* descr) {
        const auto it = v.arrays.find(name);
        return it != v.arrays.end() && it->second.descr == descr
            ? &it->second : nullptr;
    };
    auto product = [](std::initializer_list<uint64_t> factors,
                      uint64_t& result) {
        result = 1;
        for (uint64_t factor : factors) {
            if (factor == 0 ||
                result > std::numeric_limits<uint64_t>::max() / factor)
                return false;
            result *= factor;
        }
        return true;
    };
    auto bilagrid = [&](const char* state_key, const std::string& prefix,
                        uint64_t fixed_channels, bool depth) {
        const JsonValue* bg = state.find(state_key);
        if (!bg) return true;
        if (!bg->is_object()) return false;
        const JsonValue* enabled_value = bg->find("enabled");
        const double enabled = enabled_value
            ? enabled_value->as_double(-1) : -1;
        if (enabled == 0) return true;
        if (enabled != 1) return false;

        const double raw_l = bg->get_double("L", 0);
        const double raw_h = bg->get_double("H", 0);
        const double raw_w = bg->get_double("W", 0);
        const double raw_c = fixed_channels
            ? (double)fixed_channels : bg->get_double("C", 0);
        const double value_bits = bg->get_double("value_bits", 0);
        const double optim_bits = bg->get_double("optim_bits", 0);
        const double use_adagrad = bg->get_double("use_adagrad", -1);
        if (!exact_integer(raw_l, 1, UINT32_MAX) ||
            !exact_integer(raw_h, 1, UINT32_MAX) ||
            !exact_integer(raw_w, 1, UINT32_MAX) ||
            !exact_integer(raw_c, 1, UINT32_MAX) ||
            (value_bits != 16 && value_bits != 32) ||
            (optim_bits != 4 && optim_bits != 8 && optim_bits != 32) ||
            (use_adagrad != 0 && use_adagrad != 1))
            return false;

        uint64_t cells_per_grid;
        if (!product({(uint64_t)raw_l, (uint64_t)raw_h,
                      (uint64_t)raw_w, (uint64_t)raw_c},
                     cells_per_grid))
            return false;
        uint64_t cells;
        if (value_bits == 32) {
            const NpyInfo* grids = array(prefix + ".grids", "<f4");
            if (!grids || grids->data_bytes == 0 ||
                grids->data_bytes % sizeof(float) != 0)
                return false;
            cells = grids->data_bytes / sizeof(float);
        } else {
            const NpyInfo* grids = array(prefix + ".grids_q.q", "|u1");
            if (!grids || grids->data_bytes == 0 ||
                grids->data_bytes % 2 != 0)
                return false;
            cells = grids->data_bytes / 2;
            if (!pair(prefix + ".grids_q", grids->data_bytes,
                      ((cells + 255) / 256) * sizeof(float) * 2))
                return false;
        }
        if (cells % cells_per_grid != 0) return false;
        const uint64_t num_grids = cells / cells_per_grid;
        if (depth &&
            !exact(prefix + ".scalars", "<f4",
                   num_grids * sizeof(float)))
            return false;

        const uint64_t blocks = (cells + 255) / 256;
        if (use_adagrad == 1) {
            if (optim_bits == 32)
                return exact(prefix + ".accum", "<f4",
                             cells * sizeof(float));
            return pair(prefix + ".bg_ag", cells,
                        blocks * sizeof(float) * 2);
        }
        if (optim_bits == 32)
            return exact(prefix + ".g1", "<f4", cells * sizeof(float)) &&
                   exact(prefix + ".g2", "<f4", cells * sizeof(float));
        return pair(prefix + ".bg_quant", cells * 2,
                    blocks * sizeof(float) * 4);
    };

    if (!f32("world.means", 3) || !f32("world.quats", 4) ||
        !f32("world.scales", 3) || !f32("world.opacities", 1) ||
        !f32("world.features_dc", 3))
        return false;
    if (!f32("eng.radii", 1) || !f32("eng.accum_buffer", 2))
        return false;
    if (state.get_double("use_per_splat_bias_correction", 0) != 0 &&
        !exact("eng.bias_correction_steps", "<i4",
               (uint64_t)max_splats * sizeof(int32_t)))
        return false;

    const uint64_t N = (uint64_t)max_splats;
    const uint64_t K = (uint64_t)num_sh;
    const uint64_t splat_blocks = (N + 255) / 256;
    const bool fused =
        state.get_double("use_fused_proj_bwd_optim", 0) != 0;
    const uint64_t sh_cells = N * K * 3;
    const uint64_t sh_cells_fpbo =
        (uint64_t)sh_fpbo_cells(max_splats, (uint32_t)num_sh);

    const double value_bits = state.get_double("sh_value_bits", 32);
    if (num_sh > 0) {
        if (value_bits == 32) {
            if (!f32("world.features_sh", K * 3)) return false;
        } else if (value_bits == 8 || value_bits == 16) {
            const std::string name = value_bits == 8
                ? (fused ? "eng.world.sh_vq8_fpbo" : "eng.world.sh_vq8")
                : (fused ? "eng.world.sh_vq16_fpbo" : "eng.world.sh_vq16");
            const uint64_t cells = fused ? sh_cells_fpbo : sh_cells;
            const uint64_t bounds = fused ? splat_blocks : (cells + 255) / 256;
            if (!pair(name, cells * (uint64_t)(value_bits / 8),
                      bounds * sizeof(float) * 2))
                return false;
        } else {
            return false;
        }
    }

    const double non_sh_bits = state.get_double("non_sh_optim_bits", 32);
    if (non_sh_bits == 32) {
        for (const auto& [name, channels] :
             {std::pair{"means", 3u}, std::pair{"quats", 4u},
              std::pair{"scales", 3u}, std::pair{"opacities", 1u},
              std::pair{"features_dc", 3u}})
            if (!f32("eng.g1_" + std::string(name), channels) ||
                !f32("eng.g2_" + std::string(name), channels))
                return false;
    } else if (non_sh_bits == 16) {
        for (const auto& [name, channels] :
             {std::pair{"means", 3u}, std::pair{"quats", 4u},
              std::pair{"scales", 3u}, std::pair{"opacities", 1u},
              std::pair{"features_dc", 3u}})
            if (!pair("eng." + std::string(name) + "_qfpbo",
                      N * channels * 4, splat_blocks * sizeof(float) * 4))
                return false;
    } else {
        return false;
    }

    const double sh_optim_bits = state.get_double("sh_optim_bits", 32);
    if (num_sh > 0) {
        if (sh_optim_bits == 32) {
            if (!f32("eng.g1_features_sh", K * 3) ||
                !f32("eng.g2_features_sh", K * 3))
                return false;
        } else if (sh_optim_bits == 4 || sh_optim_bits == 8) {
            const uint64_t cells = fused ? sh_cells_fpbo : sh_cells;
            const uint64_t bounds =
                fused ? splat_blocks : (sh_cells + 255) / 256;
            if (!pair(fused ? "eng.sh_quant_fpbo" : "eng.sh_quant",
                      cells * 2, bounds * sizeof(float) * 4))
                return false;
        } else {
            return false;
        }
    }
    if (!bilagrid("bilagrid_rgb", "eng.bg.rgb", 0, false) ||
        !bilagrid("bilagrid_depth", "eng.bg.depth", 2, true) ||
        !bilagrid("bilagrid_normal", "eng.bg.normal", 3, false))
        return false;

    const JsonValue* ppisp = state.find("ppisp");
    if (ppisp) {
        if (!ppisp->is_object()) return false;
        const double enabled = ppisp->get_double("enabled", -1);
        if (enabled != 0 && enabled != 1) return false;
        if (enabled == 1) {
            const double raw_params = ppisp->get_double("num_params", 0);
            const double use_adagrad = ppisp->get_double("use_adagrad", -1);
            if (!exact_integer(raw_params, 1, UINT32_MAX) ||
                (use_adagrad != 0 && use_adagrad != 1))
                return false;
            const NpyInfo* params = array("eng.ppisp.params", "<f4");
            uint64_t row_bytes;
            if (!product({(uint64_t)raw_params, sizeof(float)}, row_bytes) ||
                !params || params->data_bytes == 0 ||
                params->data_bytes % row_bytes != 0)
                return false;
            if (use_adagrad == 1) {
                if (!exact("eng.ppisp.accum", "<f4", params->data_bytes))
                    return false;
            } else if (!exact("eng.ppisp.g1", "<f4", params->data_bytes) ||
                       !exact("eng.ppisp.g2", "<f4", params->data_bytes)) {
                return false;
            }
        }
    }

    const JsonValue* background = state.find("background_sh");
    const NpyInfo* background_coeffs =
        array("eng.bg_sky.sh_coeffs", "<f4");
    const bool any_background_array =
        v.arrays.count("eng.bg_sky.sh_coeffs") != 0 ||
        v.arrays.count("eng.bg_sky.g1") != 0 ||
        v.arrays.count("eng.bg_sky.g2") != 0;
    if (background) {
        if (!background->is_object()) return false;
        const double enabled = background->get_double("enabled", -1);
        if (enabled != 0 && enabled != 1) return false;
        if (enabled == 0) return !any_background_array;
        const double degree = background->get_double("degree", -1);
        if (!exact_integer(degree, 0, 4)) return false;
        uint64_t bytes;
        const uint64_t side = (uint64_t)degree + 1;
        if (!product({side, side, 3, sizeof(float)}, bytes))
            return false;
        return exact("eng.bg_sky.sh_coeffs", "<f4", bytes) &&
               exact("eng.bg_sky.g1", "<f4", bytes) &&
               exact("eng.bg_sky.g2", "<f4", bytes);
    }
    if (!any_background_array) return true;
    if (!background_coeffs) return false;
    for (uint64_t degree = 0; degree <= 4; ++degree) {
        uint64_t bytes;
        const uint64_t side = degree + 1;
        if (product({side, side, 3, sizeof(float)}, bytes) &&
            background_coeffs->data_bytes == bytes)
            return exact("eng.bg_sky.g1", "<f4", bytes) &&
                   exact("eng.bg_sky.g2", "<f4", bytes);
    }
    return false;
}

bool valid_checkpoint(const fs::path& ckpt_dir, bool require_resume = false) {
    try {
        TarView v = open_state_tar(ckpt_dir);
        const std::string state = read_member(v, "state.json");
        if (state.empty()) return false;
        const JsonValue json = json_parse(state);
        const JsonValue* step = json.find("step");
        const double raw_step = step ? step->as_double(-1) : -1;
        return exact_integer(raw_step, 0, kMaxExactJsonInteger) &&
               has_complete_manifest(v, json) &&
               (!require_resume || has_resume_payload(v, json));
    } catch (...) {
        return false;
    }
}

}  // namespace


ResolvedCheckpoint resolve_checkpoint(const fs::path& path) {
    const std::string name = path.filename().string();
    const bool unpublished = name.rfind(".staging-", 0) == 0 ||
                             name.rfind(".replaced-", 0) == 0;
    if (!unpublished && valid_checkpoint(path))
        return {path.parent_path(), path};

    std::vector<fs::path> ckpts;
    if (fs::is_directory(path)) {
        for (const auto& e : fs::directory_iterator(path)) {
            const std::string b = e.path().filename().string();
            if (b.rfind("step-", 0) == 0 && b.size() > 5 &&
                b.compare(b.size() - 5, 5, ".ckpt") == 0)
                ckpts.push_back(e.path());
        }
    }
    std::sort(ckpts.begin(), ckpts.end());
    while (!ckpts.empty()) {
        const fs::path c = ckpts.back();
        ckpts.pop_back();
        if (valid_checkpoint(c, true)) return {path, c};
    }
    throw std::runtime_error("no valid state.tar or step-*.ckpt found under " +
                             path.string());
}


JsonValue read_state_json(const fs::path& ckpt_dir) {
    TarView v = open_state_tar(ckpt_dir);
    std::string sj = read_member(v, "state.json");
    if (sj.empty())
        throw std::runtime_error("state.json missing in " +
                                 (ckpt_dir / "state.tar").string());
    JsonValue state = json_parse(sj);
    if (!has_complete_manifest(v, state))
        throw std::runtime_error("checkpoint array manifest mismatch in " +
                                 (ckpt_dir / "state.tar").string());
    return state;
}


void check_resumable(const fs::path& ckpt_dir) {
    TarView v = open_state_tar(ckpt_dir);
    const std::string sj = read_member(v, "state.json");
    const JsonValue state = sj.empty() ? JsonValue() : json_parse(sj);
    if (!has_complete_manifest(v, state))
        throw std::runtime_error("checkpoint array manifest mismatch in " +
                                 (ckpt_dir / "state.tar").string());
    if (!has_resume_payload(v, state))
        throw std::runtime_error(
            "checkpoint '" + ckpt_dir.string() + "' is NOT resumable: it was "
            "saved without save_full_checkpoint, so it holds only the "
            "inference/appearance params and splat.ply -- not the world "
            "parameters and optimizer state needed to continue training. "
            "Re-run the source training with --save-full-checkpoint 1 so its "
            "checkpoints are resumable; this one is usable for inference and "
            "meshing (splat.ply) only.");
}


TrainConfig config_from_json(const fs::path& config_json) {
    TrainConfig c;
    train_config_from_json(json_parse_file(config_json.string()), c);
    return c;
}


TrainConfig build_resume_config(const TrainConfig& cli,
                                 const std::string& preset,
                                 const std::set<std::string>& explicit_flags) {
    ResolvedCheckpoint r = resolve_checkpoint(cli.resume);
    check_resumable(r.ckpt_dir);

    const fs::path run_dir = fs::absolute(r.run_dir);
    fs::path cfg_path = r.ckpt_dir / "config.json";
    if (!fs::is_regular_file(cfg_path)) cfg_path = run_dir / "config.json";
    if (!fs::is_regular_file(cfg_path))
        throw std::runtime_error("no config.json in " + r.ckpt_dir.string() +
                                 " or " + run_dir.string() +
                                 " (needed to reconstruct the run's config)");

    TrainConfig base = config_from_json(cfg_path);
    base.resume = cli.resume;

    // Continue writing into the checkpoint's own run folder, so new
    // checkpoints, eval images and logs land beside the old ones. An explicit
    // --output-dir-* below overrides this.
    base.output_dir_prefix = run_dir.parent_path().string();
    base.output_dir_name   = run_dir.filename().string();

    // A preset named on the resume command line re-imposes its deviations on
    // top of the checkpoint (e.g. resuming a 3dgs run as `synthetic` turns
    // bilagrid/PPISP off). Applying nothing for a same-preset resume is
    // automatic: the preset assigns exactly the fields it overrides.
    if (!preset.empty() && !train_apply_preset(base, preset))
        throw std::runtime_error("unknown preset: " + preset);

    // Explicit flags win over both.
#define SS_APPLY_EXPLICIT(type, member, default_, section, tier, choices)     \
    if (explicit_flags.count(#member)) base.member = cli.member;
    SS_CONFIG_FIELDS(SS_APPLY_EXPLICIT)
#undef SS_APPLY_EXPLICIT

    return base;
}

}  // namespace ckpt
