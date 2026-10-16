#include "engine/EngineDiagnostic.h"
#include "engine/Engine.h"
#include "engine/EngineInternal.h"
#include "engine/EngineState.h"
#include "data/DataManager.h"
#include "core/Sha256.h"
#if defined(SS_BACKEND_VULKAN)
#include "backend/vulkan/kernels/B1Ownership.h"
#endif

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <cstdio>

namespace {

std::string diagnostic_arm;
std::vector<double> diagnostic_times;
std::vector<std::string> diagnostic_inputs;
std::vector<std::string> diagnostic_step_states;
size_t diagnostic_time_cursor = 0, diagnostic_input_cursor = 0;
EngineDiagnosticStage mask_probe;
bool mask_probe_all_owned = false;
bool mask_probe_capture_data = false;
EngineDiagnosticPpisp ppisp_probe;
const TrainStep* ppisp_input = nullptr;

std::string host_digest(const std::string& text, int scope) {
    spirula::Sha256 sha;
    const size_t stop = text.find("---publication---\n");
    if (scope == 0) sha.update(reinterpret_cast<const uint8_t*>(text.data()), stop);
    else for (size_t begin = 0; begin < stop;) {
        const size_t end = text.find('\n', begin);
        const std::string line = text.substr(begin, end - begin);
        const bool role = line.rfind("s.b1.owner=", 0) == 0 ||
            line.rfind("s.b1.frozen_hash=", 0) == 0 ||
            line.rfind("c.owner=", 0) == 0 || line.rfind("c.frozen_context=", 0) == 0;
        if (role == (scope == 2))
            sha.update(reinterpret_cast<const uint8_t*>(text.data() + begin), end + 1 - begin);
        begin = end + 1;
    }
    return sha.hex();
}

void checked_backend() {
    if (const char* error = backend::last_error())
        throw std::runtime_error(std::string("B2 causal device copy: ") + error);
}

bool persistent_derived(const std::string& name) {
    return name == "eng.v_losses" || name == "eng.color_shift_reg.batch_sum" ||
           name == "eng.densify.oversize" || name == "eng.densify.sample_score";
}

std::vector<DevicePool::SavedSlot> diagnostic_slots() {
    auto slots = DevicePool::global().saved(SaveClass::Never);
    slots.erase(std::remove_if(slots.begin(), slots.end(), [](const auto& slot) {
        return slot.save == SaveClass::Never && !persistent_derived(slot.name);
    }), slots.end());
    return slots;
}

void validate_format() {
    const auto& s = engine();
    if (!s.dm || s.b1.owner < 0 || s.b1.owner > 2 || !s.b1.contract ||
        s.primitive != "3dgs" || !s.packed || s.world.sh_value_bits != 16 ||
        s.optim.sh_optim_bits != 8 || s.optim.non_sh_optim_bits != 16 ||
        !s.optim.use_fused_proj_bwd_optim || !s.optim.fused_state_active ||
        !s.optim.use_per_splat_bias_correction || s.cur_num_splats != s.max_num_splats)
        throw std::runtime_error("B2 causal capture requires original marked FPBO 16/8/16 layout");
}

} // namespace

void engine_diagnostic_ppisp_probe(EngineDiagnosticPpisp probe) {
    ppisp_probe = std::move(probe);
    ppisp_input = nullptr;
}
const EngineDiagnosticPpisp& engine_diagnostic_ppisp_probe() { return ppisp_probe; }
const TrainStep* engine_diagnostic_ppisp_input() { return ppisp_input; }

std::string engine_diagnostic_host_state() {
    const auto& s = engine();
    std::ostringstream out;
    out << std::hexfloat;
#define D2_SCALAR(value) out << #value << '=' << (value) << '\n'
    D2_SCALAR(s.primitive); D2_SCALAR(s.cur_num_splats); D2_SCALAR(s.max_num_splats);
    D2_SCALAR(s.num_sh); D2_SCALAR(s.sh_degree); D2_SCALAR(s.packed);
    D2_SCALAR(s.bin_tile_request); D2_SCALAR(s.v_losses_uploaded);
    D2_SCALAR(s.world.initialized); D2_SCALAR(s.world.sh_value_bits);
    D2_SCALAR(s.optim.initialized); D2_SCALAR(s.optim.sh_optim_bits);
    D2_SCALAR(s.optim.non_sh_optim_bits); D2_SCALAR(s.optim.use_per_splat_bias_correction);
    D2_SCALAR(s.optim.use_fused_proj_bwd_optim); D2_SCALAR(s.optim.fused_state_active);
    D2_SCALAR(s.optim.skip_grad_zero); D2_SCALAR(s.optim.grad_scale);
    D2_SCALAR(s.optim.zero_grad_in_optim); D2_SCALAR(s.b1.owner); D2_SCALAR(s.b1.source_id);
    D2_SCALAR(s.b1.frozen_hash);
    if (s.b1.contract) {
        const auto& c = *s.b1.contract;
        D2_SCALAR(c.version); D2_SCALAR(c.owner); D2_SCALAR(c.sampler_version);
        D2_SCALAR(c.source_step); D2_SCALAR(c.horizon); D2_SCALAR(c.rows);
        D2_SCALAR(c.source_state_sha256); D2_SCALAR(c.config_sha256);
        D2_SCALAR(c.views_sha256); D2_SCALAR(c.train_indices_sha256);
        D2_SCALAR(c.frozen_context); D2_SCALAR(c.frozen_shared_sha256);
    }
#define D2_GRID(grid) \
    D2_SCALAR(grid.optim_bits); D2_SCALAR(grid.value_bits); D2_SCALAR(grid.use_adagrad); \
    D2_SCALAR(grid.enabled); D2_SCALAR(grid.optim_initialized)
    D2_GRID(s.bilagrid_rgb); D2_GRID(s.bilagrid_depth); D2_GRID(s.bilagrid_normal);
    D2_SCALAR(s.bilagrid_rgb.type); D2_SCALAR(s.bilagrid_rgb.C);
    D2_SCALAR(s.ppisp.param_type); D2_SCALAR(s.ppisp.num_params);
    D2_SCALAR(s.ppisp.enabled); D2_SCALAR(s.ppisp.optim_initialized);
    D2_SCALAR(s.ppisp.use_adagrad); D2_SCALAR(s.ppisp.exposure_arithmetic_mean);
    D2_SCALAR(s.ppisp.cur_run_before_bilagrid); D2_SCALAR(s.ppisp.cur_run_before_color_space);
    D2_SCALAR(s.ppisp.forward_pending);
    D2_SCALAR(s.color_shift_reg.initialized); D2_SCALAR(s.color_shift_reg.steps);
    D2_SCALAR(s.color_shift_reg.cur_weight); D2_SCALAR(s.color_shift_reg.cur_beta);
    D2_SCALAR(s.color_space.splat_enabled); D2_SCALAR(s.color_space.splat_transfer);
    D2_SCALAR(s.color_space.splat_is_linear); D2_SCALAR(s.color_space.image_enabled);
    D2_SCALAR(s.color_space.image_transfer); D2_SCALAR(s.color_space.image_is_linear);
    D2_SCALAR(static_cast<int>(s.background.mode)); D2_SCALAR(s.background.enabled);
    D2_SCALAR(s.background.splat_transfer); D2_SCALAR(s.background.splat_is_linear);
    D2_SCALAR(s.background.color.x); D2_SCALAR(s.background.color.y); D2_SCALAR(s.background.color.z);
    D2_SCALAR(s.background.color_display.x); D2_SCALAR(s.background.color_display.y);
    D2_SCALAR(s.background.color_display.z); D2_SCALAR(s.background.sh_degree);
    D2_SCALAR(s.background.sh_optim_initialized); D2_SCALAR(s.background.cur_seed);
    D2_SCALAR(s.background.cur_randomize_weight); D2_SCALAR(s.background.cur_num_loss_scales);
    D2_SCALAR(s.background.cur_loss_scale_min_pixels); D2_SCALAR(s.background.cur_block_px);
    D2_SCALAR(s.background.match_luma_pending); D2_SCALAR(s.background.cur_match_luma);
    for (float value : s.background.luma_by_cam_host) D2_SCALAR(value);
    for (float value : s.background.exponent_by_cam_host) D2_SCALAR(value);
    for (float value : s.color_space.image_color_matrix_host) D2_SCALAR(value);
    for (float value : s.gt_mean_luma) {
        uint32_t bits; std::memcpy(&bits, &value, sizeof bits); D2_SCALAR(bits);
    }
    for (const auto& [key, c] : s.bin_tile_auto) {
        D2_SCALAR(key); D2_SCALAR(c.macro_log2); D2_SCALAR(c.floor); D2_SCALAR(c.from);
        D2_SCALAR(c.probing); D2_SCALAR(c.dir); D2_SCALAR(c.warmup); D2_SCALAR(c.have);
        D2_SCALAR(c.wait);
        D2_SCALAR(c.sum); D2_SCALAR(c.base); D2_SCALAR(c.trial);
    }
#undef D2_GRID
    out << s.dm->diagnostic_signature();
    out << "---publication---\n";
    if (s.b1.contract) {
        const auto& c = *s.b1.contract;
        D2_SCALAR(c.completed_step); D2_SCALAR(c.mask_sha256);
        D2_SCALAR(c.blocks_sha256); D2_SCALAR(c.owned_ids_sha256);
        D2_SCALAR(c.excluded_ids_sha256); D2_SCALAR(c.ply_sha256);
    }
#undef D2_SCALAR
    return out.str();
}

std::string engine_diagnostic_host_signature(bool common) {
    return host_digest(engine_diagnostic_host_state(), common ? 1 : 0);
}

EngineDiagnosticSnapshot engine_diagnostic_capture(bool include_reset) {
    validate_format();
    backend::device_synchronize();
    checked_backend();
    const auto& s = engine();
    EngineDiagnosticSnapshot snapshot;
    snapshot.rows = s.cur_num_splats;
    snapshot.max_rows = s.max_num_splats;
    snapshot.num_sh = s.num_sh;
    snapshot.owner = s.b1.owner;
    snapshot.source_id = s.b1.source_id;
    snapshot.host_state = engine_diagnostic_host_state();
    snapshot.host_signature = host_digest(snapshot.host_state, 0);
    snapshot.common_host_signature = host_digest(snapshot.host_state, 1);
    snapshot.role_host_signature = host_digest(snapshot.host_state, 2);
    snapshot.owned_blocks.resize(static_cast<size_t>(s.b1.owned_blocks.size()));
    backend::memcpy_sync(snapshot.owned_blocks.data(), s.b1.owned_blocks.data_ptr(),
                         snapshot.owned_blocks.size() * sizeof(uint32_t), backend::MemcpyKind::DeviceToHost);
    size_t bytes = snapshot.owned_blocks.size() * sizeof(uint32_t);
    for (const auto& slot : diagnostic_slots()) {
        bytes += slot.nbytes;
        if (bytes > (size_t(2) << 30))
            throw std::runtime_error("B2 causal snapshot exceeds 2 GiB host admission");
        EngineDiagnosticField field;
        field.name = slot.name;
        field.dtype = slot.dtype_tag;
        field.serialized = slot.save != SaveClass::Never;
        field.bytes.resize(slot.nbytes);
        backend::memcpy_sync(field.bytes.data(), slot.ptr, slot.nbytes, backend::MemcpyKind::DeviceToHost);
        snapshot.fields.push_back(std::move(field));
    }
    checked_backend();
    if (include_reset) {
        snapshot.restore_host = [world = s.world, optim = s.optim, b1 = s.b1,
            rgb = s.bilagrid_rgb, depth = s.bilagrid_depth, normal = s.bilagrid_normal,
            background = s.background, ppisp = s.ppisp, color = s.color_space,
            shift = s.color_shift_reg, luma = s.gt_mean_luma, bin = s.bin_tile_auto,
            primitive = s.primitive, rows = s.cur_num_splats, max_rows = s.max_num_splats,
            num_sh = s.num_sh, sh_degree = s.sh_degree, packed = s.packed,
            bin_request = s.bin_tile_request, uploaded = s.v_losses_uploaded,
            reset_sampler = s.dm->diagnostic_snapshot()] {
            auto& current = engine();
            current.world = world; current.optim = optim;
            const auto mask = current.b1.owned_blocks;
            current.b1 = b1; current.b1.owned_blocks = mask;
            current.bilagrid_rgb = rgb; current.bilagrid_depth = depth;
            current.bilagrid_normal = normal; current.background = background;
            current.ppisp = ppisp; current.color_space = color; current.color_shift_reg = shift;
            current.gt_mean_luma = luma; current.bin_tile_auto = bin;
            current.primitive = primitive; current.cur_num_splats = rows;
            current.max_num_splats = max_rows; current.num_sh = num_sh;
            current.sh_degree = sh_degree; current.packed = packed;
            current.bin_tile_request = bin_request; current.v_losses_uploaded = uploaded;
            reset_sampler();
        };
    }
    return snapshot;
}

void engine_diagnostic_verify_start(const EngineDiagnosticSnapshot& snapshot) {
    const auto actual = engine_diagnostic_capture(false);
    if (actual.rows != snapshot.rows || actual.max_rows != snapshot.max_rows ||
        actual.num_sh != snapshot.num_sh || actual.owner != snapshot.owner ||
        actual.source_id != snapshot.source_id || actual.owned_blocks != snapshot.owned_blocks ||
        actual.host_state != snapshot.host_state ||
        actual.host_signature != snapshot.host_signature || actual.fields.size() != snapshot.fields.size())
        throw std::runtime_error("B2 causal independent starting metadata inequality");
    for (size_t i = 0; i < actual.fields.size(); ++i) {
        const auto& a = actual.fields[i];
        const auto& b = snapshot.fields[i];
        if (a.name != b.name || a.dtype != b.dtype || a.serialized != b.serialized || a.bytes != b.bytes)
            throw std::runtime_error("B2 causal independent starting field inequality: " + b.name);
    }
    engine_b1_verify_context();
}

void engine_diagnostic_restore(const EngineDiagnosticSnapshot& snapshot) {
    if (!snapshot.restore_host)
        throw std::runtime_error("B2 causal snapshot has no independent reset");
    backend::device_synchronize();
    const auto slots = diagnostic_slots();
    if (slots.size() != snapshot.fields.size())
        throw std::runtime_error("B2 causal reset changed persistent allocation schema");
    for (size_t i = 0; i < slots.size(); ++i) {
        const auto& field = snapshot.fields[i];
        const auto& slot = slots[i];
        if (field.name != slot.name || field.dtype != slot.dtype_tag || field.bytes.size() != slot.nbytes)
            throw std::runtime_error("B2 causal reset changed layout: " + field.name);
        backend::memcpy_sync(const_cast<void*>(slot.ptr), field.bytes.data(), slot.nbytes,
                             backend::MemcpyKind::HostToDevice);
    }
    auto& mask = engine().b1.owned_blocks;
    if (mask.size() != static_cast<int64_t>(snapshot.owned_blocks.size()))
        throw std::runtime_error("B2 causal reset changed ownership shape");
    backend::memcpy_sync(mask.data_ptr(), snapshot.owned_blocks.data(),
                         snapshot.owned_blocks.size() * sizeof(uint32_t), backend::MemcpyKind::HostToDevice);
    snapshot.restore_host();
    backend::device_synchronize();
    checked_backend();
    engine_diagnostic_verify_start(snapshot);
}

void engine_diagnostic_self_check(const EngineDiagnosticSnapshot& snapshot) {
    engine_diagnostic_verify_start(snapshot);
    auto& s = engine();
    ++s.color_shift_reg.steps;
    bool rejected_host = false;
    try { engine_diagnostic_verify_start(snapshot); }
    catch (const std::runtime_error& error) {
        rejected_host = std::string(error.what()) == "B2 causal independent starting metadata inequality";
    }
    engine_diagnostic_restore(snapshot);
    const auto block = std::find(snapshot.owned_blocks.begin(), snapshot.owned_blocks.end(), 1u);
    if (block == snapshot.owned_blocks.end())
        throw std::runtime_error("B2 causal snapshot check has no owned row");
    const size_t row = static_cast<size_t>(block - snapshot.owned_blocks.begin()) * 256;
    uint32_t bits = 0;
    backend::memcpy_sync(&bits, s.world.means.data_ptr() + row, sizeof bits,
                         backend::MemcpyKind::DeviceToHost);
    bits ^= 1u;
    backend::memcpy_sync(s.world.means.data_ptr() + row, &bits, sizeof bits,
                         backend::MemcpyKind::HostToDevice);
    checked_backend();
    bool rejected_device = false;
    try { engine_diagnostic_verify_start(snapshot); }
    catch (const std::runtime_error& error) {
        rejected_device = std::string(error.what()) ==
                          "B2 causal independent starting field inequality: world.means";
    }
    engine_diagnostic_restore(snapshot);
    if (!rejected_host || !rejected_device)
        throw std::runtime_error("B2 causal independent snapshot regression failed");
}

void engine_diagnostic_begin_arm(const std::string& arm) {
    if (arm == "A") {
        diagnostic_times.clear();
        diagnostic_inputs.clear();
        diagnostic_step_states.clear();
    }
    diagnostic_arm = arm;
    diagnostic_time_cursor = diagnostic_input_cursor = 0;
}

void engine_diagnostic_end_arm() {
    if (diagnostic_arm != "A" &&
        (diagnostic_time_cursor != diagnostic_times.size() ||
         diagnostic_input_cursor != diagnostic_inputs.size()))
        throw std::runtime_error("B2 causal replay did not consume the native A sequence");
    diagnostic_arm.clear();
}

void engine_diagnostic_timing(double& seconds) {
    if (diagnostic_arm.empty()) return;
    if (diagnostic_arm == "A") diagnostic_times.push_back(seconds);
    else {
        if (diagnostic_time_cursor >= diagnostic_times.size())
            throw std::runtime_error("B2 causal native timing replay exhausted");
        seconds = diagnostic_times[diagnostic_time_cursor];
    }
    ++diagnostic_time_cursor;
}

void engine_diagnostic_input(const TrainStep& input, int step) {
    if (ppisp_probe) ppisp_input = &input;
    if (diagnostic_arm.empty()) return;
    spirula::Sha256 sha;
    auto scalar = [&](const auto& value) {
        sha.update(reinterpret_cast<const uint8_t*>(&value), sizeof value);
    };
    auto vector = [&](const auto& values) {
        const uint64_t count = values.size();
        scalar(count);
        if (count) sha.update(reinterpret_cast<const uint8_t*>(values.data()),
                              values.size() * sizeof(values[0]));
    };
    scalar(step);
    for (const auto& sub : input.subs) {
        const auto& b = *sub;
        scalar(b.width); scalar(b.height); scalar(b.num); scalar(b.model); scalar(b.distortion);
        scalar(b.input_width); scalar(b.input_height); scalar(b.input_num); scalar(b.K);
        scalar(b.input_model); scalar(b.input_distortion);
        scalar(b.rgb_dtype); scalar(b.depth_dtype); scalar(b.normal_dtype);
        scalar(b.mask_width); scalar(b.mask_height); scalar(b.depth_width); scalar(b.depth_height);
        scalar(b.normal_width); scalar(b.normal_height);
        vector(b.indices); vector(b.post_offsets); vector(b.viewmats); vector(b.intrins);
        vector(b.dist_coeffs); vector(b.face_axes); vector(b.input_intrins);
        vector(b.input_dist_coeffs); vector(b.input_source_models); vector(b.input_source_params);
        for (const auto& pass : b.face_passes) {
            scalar(pass.k0); scalar(pass.k1); scalar(pass.width); scalar(pass.height);
        }
        vector(b.rgb_buffer); vector(b.mask_buffer); vector(b.depth_buffer); vector(b.normal_buffer);
    }
    const auto digest = sha.hex();
    if (diagnostic_arm == "A") diagnostic_inputs.push_back(digest);
    else if (diagnostic_input_cursor >= diagnostic_inputs.size() ||
             diagnostic_inputs[diagnostic_input_cursor] != digest)
        throw std::runtime_error("B2 causal next native input differs from A at step " + std::to_string(step));
    ++diagnostic_input_cursor;
    std::fprintf(stderr, "D2_INPUT arm=%s step=%d sha256=%s\n",
                 diagnostic_arm.c_str(), step, digest.c_str());
}

void engine_diagnostic_step_state(int step, bool common, const EngineDiagnosticSnapshot* role) {
    const auto host = engine_diagnostic_host_state();
    const auto signature = host_digest(host, common ? 1 : 0);
    if (role && host_digest(host, 2) != role->role_host_signature)
        throw std::runtime_error("native diagnostic arm role/context identity changed");
    if (diagnostic_arm == "A") diagnostic_step_states.push_back(signature);
    else if (diagnostic_input_cursor == 0 ||
             diagnostic_input_cursor > diagnostic_step_states.size() ||
             signature != diagnostic_step_states[diagnostic_input_cursor - 1])
        throw std::runtime_error("B2 causal native settings or sampler differs from A after step " +
                                 std::to_string(step));
}

void engine_diagnostic_mask_probe(EngineDiagnosticStage observer, bool all_owned,
                                  bool capture_data_gradients) {
    if (observer) validate_format();
    mask_probe = std::move(observer);
    mask_probe_all_owned = all_owned;
    mask_probe_capture_data = capture_data_gradients;
}

void engine_diagnostic_stage(const char* stage, int step) {
    if (mask_probe) mask_probe(stage, step);
}

bool engine_diagnostic_probe_optim(int step, const EngineStepConfig& cfg) {
    if (!mask_probe) return false;
#if defined(SS_BACKEND_VULKAN)
    validate_format();
    auto& s = engine();
    const int owner = s.b1.owner;
    if (mask_probe_all_owned && owner != 2)
        throw std::runtime_error("unmasked native probe requires genuine all-owned contract");
    const auto contract_owner = s.b1.contract->owner;
    const auto source_id = s.b1.source_id;
    const auto frozen_hash = s.b1.frozen_hash;
    // Only the Gaussian launch is unmasked; the real contract still freezes
    // shared optimizers and topology in the surrounding production step.
    try {
        if (mask_probe_capture_data) {
            engine_fpbo_capture_data_gradients(step, cfg.optim);
            engine_diagnostic_stage("data_gradient", step);
        }
        if (mask_probe_all_owned) s.b1.owner = -1;
        engine_optim_step(step, cfg.optim);
        s.b1.owner = owner;
        vkk::b1_set_owned_blocks(s.b1.owned_blocks.data_ptr());
    } catch (...) {
        s.b1.owner = owner;
        vkk::b1_set_owned_blocks(s.b1.owned_blocks.data_ptr());
        throw;
    }
    if (s.b1.owner != owner || !s.b1.contract || s.b1.contract->owner != contract_owner ||
        s.b1.source_id != source_id || s.b1.frozen_hash != frozen_hash ||
        vkk::b1_owned_blocks() != s.b1.owned_blocks.data_ptr())
        throw std::runtime_error("native mask probe failed to restore ownership role");
    return true;
#else
    throw std::runtime_error("native mask probe requires Vulkan");
#endif
}
