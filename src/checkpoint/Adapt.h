#pragma once

// Host checkpoint layout adaptation writes an ordinary state.tar for the
// exact-match engine loader. It supports cap/SH changes, bilagrid resize, and
// dropping bilagrid/PPISP; additions and storage changes are rejected.
//
// Buffers are rewritten one at a time because the motivating case has no VRAM
// left. Quantized buffers use the engine's host-callable codecs in Tensor.h.
//
// Splat reduction drops unsaturated newest slots first, then lowest opacity.

#include "data/Json.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace ckpt {

// The layout the engine skeleton actually holds. Read off the live engine
// rather than re-derived from the config: whether a depth/normal grid exists
// also depends on the dataset carrying those maps, which only setup knows.
struct TargetLayout {
    int64_t max_num_splats = 0;
    int     num_sh         = 0;    // SH coefficients per colour channel
    int     sh_optim_bits = 32;
    int     sh_value_bits = 32;
    int     non_sh_optim_bits = 32;
    bool    use_fused_proj_bwd_optim = false;
    bool    use_per_splat_bias_correction = false;
    int     num_images     = 0;    // POST-split camera count
    // Grid extents as (L, H, W); unset means the target has no such channel.
    std::optional<std::array<int, 3>> bilagrid_rgb;
    std::optional<std::array<int, 3>> bilagrid_depth;
    std::optional<std::array<int, 3>> bilagrid_normal;
    bool    background_sh = false;
    int     background_sh_degree = 0;
    int     bilagrid_optim_bits = 32;
    int     bilagrid_value_bits = 32;
    bool    bilagrid_use_adagrad = false;
    std::string bilagrid_type;
    bool    ppisp = false;
    std::string ppisp_param_type;
    bool    ppisp_use_adagrad = false;
};

// True when `state` (a checkpoint's state.json) disagrees with `t` in any way
// that changes a buffer's shape.
bool needs_adapt(const JsonValue& state, const TargetLayout& t);

// Write an adapted `out_dir/state.tar`. Returns false and writes nothing when
// needs_adapt() is false -- the caller then loads the original directly.
bool adapt_checkpoint(const std::filesystem::path& ckpt_dir,
                      const TargetLayout& t,
                      const std::filesystem::path& out_dir);

}  // namespace ckpt
