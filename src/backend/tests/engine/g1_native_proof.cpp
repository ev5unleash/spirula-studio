#include "app/TrainerCore.h"
#include "engine/Engine.h"
#include "engine/EngineInternal.h"
#include "engine/EngineState.h"
#include "engine/EngineDiagnostic.h"
#include "core/Sha256.h"
#include <map>
#include "checkpoint/Resume.h"
#include "backend/vulkan/kernels/B1Ownership.h"
#include "core/Env.h"
#include "kernels/pixelwise/PixelWise.cuh"
#include "data/DataManager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <limits>
#include <set>
#include <numeric>
#include <string>
#include <vector>
#ifdef _WIN32
#include <process.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <bcrypt.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
void save_canonical_snapshot(const fs::path& path, int step) {
    const int owner = engine().b1.owner;
    engine().b1.owner = -1;
    try {
        engine_save_checkpoint(path.string(), true, step);
    } catch (...) {
        engine().b1.owner = owner;
        throw;
    }
    engine().b1.owner = owner;
}
constexpr int N = 512, K = 15, Views = 3, Steps = 3;
constexpr uint64_t Magic = 0x47314e4154495645ULL;
constexpr uint64_t Source = 0x3d65000020260929ULL;
uint64_t packet_source = Source;
uint64_t packet_attempt = 1;
uint64_t packet_topology = 0;
uint64_t packet_layout = 0;
uint64_t packet_first = 0;
uint64_t packet_rows = 0;
using DevicePair = std::array<std::string, 2>;
using Bytes = std::vector<uint8_t>;
using Fields = std::vector<Bytes>;
fs::path proof_root;
uint64_t proof_step = 0;
void (*source_phase_observer)(const char*) = nullptr;

void need(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
double packet_hash_seconds = 0, packet_copy_seconds = 0;
// Process-local: packet serialization is single-threaded in each process.
uint64_t packet_read_payload_hash = 0, packet_write_payload_hash = 0;
std::string state_transport = "full", packet_hash_algorithm = "fnv";
bool resident_owned() { return state_transport == "resident-owned"; }
void select_packet_hash(const std::string& value) {
    need(value == "fnv" || value == "sha256", "packet hash must be fnv or sha256");
#ifndef _WIN32
    need(value == "fnv", "sha256 packet hashing requires Windows CNG");
#endif
    packet_hash_algorithm = value;
}
uint64_t wire_kind(uint64_t kind) {
    return kind | (resident_owned() ? uint64_t{1} << 32 : 0) |
        (packet_hash_algorithm == "sha256" ? uint64_t{1} << 33 : 0);
}
struct PacketDigest {
    uint64_t fnv = 14695981039346656037ULL;
#ifdef _WIN32
    struct Cng {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        Bytes object;
        bool active = false;
        Cng() {
            try {
            need(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
                 "CNG SHA256 provider failed");
            ULONG size = 0, received = 0, digest_size = 0;
            need(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, (PUCHAR)&size,
                 sizeof(size), &received, 0) >= 0 && received == sizeof(size), "CNG object length failed");
            need(BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, (PUCHAR)&digest_size,
                 sizeof(digest_size), &received, 0) >= 0 && received == sizeof(digest_size) && digest_size == 32,
                 "CNG SHA256 length failed");
            object.resize(size);
            need(BCryptCreateHash(algorithm, &hash, object.data(), size, nullptr, 0,
                 BCRYPT_HASH_REUSABLE_FLAG) >= 0, "CNG reusable hash failed");
            } catch (...) { release(); throw; }
        }
        void release() noexcept {
            if (hash && BCryptDestroyHash(hash) < 0) std::terminate();
            if (algorithm && BCryptCloseAlgorithmProvider(algorithm, 0) < 0) std::terminate();
            hash = nullptr; algorithm = nullptr;
        }
        ~Cng() { release(); }
        std::array<uint8_t, 32> finish() {
            std::array<uint8_t, 32> digest{};
            need(BCryptFinishHash(hash, digest.data(), (ULONG)digest.size(), 0) >= 0,
                 "CNG packet digest failed");
            active = false;
            return digest;
        }
    };
    Cng* cng = nullptr;
#endif
    PacketDigest() {
#ifdef _WIN32
        if (packet_hash_algorithm == "sha256") {
            static Cng context;
            cng = &context;
            if (cng->active) (void)cng->finish(); // Discard an interrupted receive before reuse.
            cng->active = true;
        }
#endif
    }
    void update(const void* pointer, size_t size) {
        if (!size) return;
#ifdef _WIN32
        if (cng) {
            need(size <= std::numeric_limits<ULONG>::max() &&
                 BCryptHashData(cng->hash, (PUCHAR)pointer, (ULONG)size, 0) >= 0,
                 "CNG packet hash update failed");
            return;
        }
#endif
        const auto* bytes = (const uint8_t*)pointer;
        for (size_t i = 0; i < size; ++i) fnv = (fnv ^ bytes[i]) * 1099511628211ULL;
    }
    void field(const Bytes& bytes) {
        uint8_t length[8];
        for (int i = 0; i < 8; ++i) length[i] = (uint8_t)((uint64_t)bytes.size() >> (i * 8));
        update(length, sizeof(length));
        update(bytes.data(), bytes.size());
    }
    uint64_t finish() {
#ifdef _WIN32
        if (cng) {
            const auto digest = cng->finish();
            uint64_t result = 0;
            for (int i = 0; i < 8; ++i) result |= (uint64_t)digest[i] << (i * 8);
            return result;
        }
#endif
        return fnv;
    }
};
TorchTensorView tv(const void* p, uint32_t size, std::vector<int64_t> shape) {
    return {(uint64_t)p, size, std::move(shape)};
}
TorchTensorView nil() { return {0, 4, {0}}; }
Bytes copy_device(const void* p, size_t bytes) {
    need(p != nullptr || bytes == 0, "missing canonical device buffer");
    Bytes out(bytes);
    if (bytes) backend::memcpy_sync(out.data(), p, bytes, backend::MemcpyKind::DeviceToHost);
    return out;
}
void upload(void* p, const Bytes& b) {
    need(p != nullptr || b.empty(), "missing canonical destination");
    if (!b.empty()) backend::memcpy_sync(p, b.data(), b.size(), backend::MemcpyKind::HostToDevice);
}
template<class T> void take(Fields& out, const DeviceVector<T>& v) {
    out.push_back(copy_device(v.data_ptr(), (size_t)v.size() * sizeof(T)));
}
template<class T> void take(Fields& out, const DeviceTensor2D<T>& v) {
    out.push_back(copy_device(v.data_ptr(), (size_t)v.numel() * sizeof(T)));
}
template<class T> void put(const Fields& in, size_t& i, const DeviceVector<T>& v) {
    need(i < in.size() && in[i].size() == (size_t)v.size() * sizeof(T), "state vector shape mismatch");
    upload(v.data_ptr(), in[i++]);
}
template<class T> void put(const Fields& in, size_t& i, const DeviceTensor2D<T>& v) {
    need(i < in.size() && in[i].size() == (size_t)v.numel() * sizeof(T), "state tensor shape mismatch");
    upload(v.data_ptr(), in[i++]);
}
template<class T> void take(Fields& out, const DeviceTensor5D<T>& v) {
    // Quantized bilagrids retain float tensor shapes without float storage.
    out.push_back(copy_device(v.data_ptr(), v.data_ptr() ? (size_t)v.numel() * sizeof(T) : 0));
}
template<class T> void put(const Fields& in, size_t& i, const DeviceTensor5D<T>& v) {
    const size_t bytes = v.data_ptr() ? (size_t)v.numel() * sizeof(T) : 0;
    need(i < in.size() && in[i].size() == bytes,
         "shared tensor shape mismatch");
    upload(v.data_ptr(), in[i++]);
}
template<class Q> void take_quant(Fields& out, const Q& q) {
    need(q.initialized(), "quantized state not initialized");
    out.push_back(copy_device(q.packed_ptr(), (size_t)q.packed_bytes()));
    out.push_back(copy_device(q.bounds_ptr(), (size_t)q.n_bounds * sizeof(*q.bounds_ptr())));
}
template<class Q> void put_quant(const Fields& in, size_t& i, const Q& q) {
    need(i + 1 < in.size() && in[i].size() == (size_t)q.packed_bytes() &&
         in[i + 1].size() == (size_t)q.n_bounds * sizeof(*q.bounds_ptr()), "codec shape mismatch");
    upload(q.packed_ptr(), in[i++]);
    upload(q.bounds_ptr(), in[i++]);
}
constexpr size_t WorldFields = 7;
Fields world_state() {
    const auto& w = engine().world;
    Fields b;
    take(b, w.means); take(b, w.quats); take(b, w.scales);
    take(b, w.opacities); take(b, w.features_dc);
    take_quant(b, w.features_sh_quant16);
    return b;
}
void restore_world(const Fields& b) {
    auto& w = engine().world;
    size_t i = 0;
    put(b, i, w.means); put(b, i, w.quats); put(b, i, w.scales);
    put(b, i, w.opacities); put(b, i, w.features_dc);
    put_quant(b, i, w.features_sh_quant16);
}
Fields state() {
    const auto& o = engine().optim;
    Fields b = world_state();
    take_quant(b, o.means_quant_state_fpbo);
    take_quant(b, o.quats_quant_state_fpbo);
    take_quant(b, o.scales_quant_state_fpbo);
    take_quant(b, o.opacities_quant_state_fpbo);
    take_quant(b, o.features_dc_quant_state_fpbo);
    take_quant(b, o.sh_quant_state);
    take(b, o.bias_correction_steps);
    return b;
}
// Radii and densification statistics are frozen for the proof: restore them
// before each alternative update, and never run the densifier.
Fields statistics() {
    Fields b;
    take(b, engine().optim.radii);
    take(b, engine().optim.accum_buffer);
    take(b, engine().optim.densify_oversize);
    take(b, engine().fwd.accum_weight);
    return b;
}
void restore_statistics(const Fields& b) {
    size_t i = 0;
    put(b, i, engine().optim.radii);
    put(b, i, engine().optim.accum_buffer);
    put(b, i, engine().optim.densify_oversize);
    if (b[i].empty() && engine().fwd.accum_weight.size()) {
        engine().fwd.accum_weight.zero();
        ++i;
    } else put(b, i, engine().fwd.accum_weight);
}
Fields shared_state() {
    const auto& s = engine();
    Fields b;
    take(b, s.ppisp.params); take(b, s.ppisp.g1);
    take(b, s.ppisp.g2); take(b, s.ppisp.accum_f);
    take(b, s.background.sh_coeffs); take(b, s.background.sh_g1);
    take(b, s.background.sh_g2);
    take(b, s.color_shift_reg.ema); take(b, s.color_shift_reg.batch_sum);
    Bytes steps(sizeof(s.color_shift_reg.steps));
    std::memcpy(steps.data(), &s.color_shift_reg.steps, steps.size());
    b.push_back(std::move(steps));
    if (s.bilagrid_rgb.enabled) {
        take(b, s.bilagrid_rgb.grids); take(b, s.bilagrid_rgb.g1);
        take(b, s.bilagrid_rgb.g2); take(b, s.bilagrid_rgb.accum_f);
        if (s.bilagrid_rgb.grids_quant.initialized())
            take_quant(b, s.bilagrid_rgb.grids_quant);
        if (s.bilagrid_rgb.quant_state.initialized())
            take_quant(b, s.bilagrid_rgb.quant_state);
        if (s.bilagrid_rgb.adagrad_quant.initialized())
            take_quant(b, s.bilagrid_rgb.adagrad_quant);
    }
    if (s.bilagrid_depth.enabled) {
        take(b, s.bilagrid_depth.grids); take(b, s.bilagrid_depth.g1);
        take(b, s.bilagrid_depth.g2); take(b, s.bilagrid_depth.accum_f);
        take(b, s.bilagrid_depth.scalars);
        if (s.bilagrid_depth.grids_quant.initialized())
            take_quant(b, s.bilagrid_depth.grids_quant);
        if (s.bilagrid_depth.quant_state.initialized())
            take_quant(b, s.bilagrid_depth.quant_state);
        if (s.bilagrid_depth.adagrad_quant.initialized())
            take_quant(b, s.bilagrid_depth.adagrad_quant);
    }
    if (s.bilagrid_normal.enabled) {
        take(b, s.bilagrid_normal.grids); take(b, s.bilagrid_normal.g1);
        take(b, s.bilagrid_normal.g2); take(b, s.bilagrid_normal.accum_f);
        if (s.bilagrid_normal.grids_quant.initialized())
            take_quant(b, s.bilagrid_normal.grids_quant);
        if (s.bilagrid_normal.quant_state.initialized())
            take_quant(b, s.bilagrid_normal.quant_state);
        if (s.bilagrid_normal.adagrad_quant.initialized())
            take_quant(b, s.bilagrid_normal.adagrad_quant);
    }
    return b;
}
void restore_shared(const Fields& b) {
    auto& s = engine();
    size_t i = 0;
    put(b, i, s.ppisp.params); put(b, i, s.ppisp.g1);
    put(b, i, s.ppisp.g2); put(b, i, s.ppisp.accum_f);
    put(b, i, s.background.sh_coeffs); put(b, i, s.background.sh_g1);
    put(b, i, s.background.sh_g2);
    put(b, i, s.color_shift_reg.ema); put(b, i, s.color_shift_reg.batch_sum);
    need(i < b.size() && b[i].size() == sizeof(s.color_shift_reg.steps), "color shift steps");
    std::memcpy(&s.color_shift_reg.steps, b[i++].data(), sizeof(s.color_shift_reg.steps));
    if (s.bilagrid_rgb.enabled) {
        put(b, i, s.bilagrid_rgb.grids); put(b, i, s.bilagrid_rgb.g1);
        put(b, i, s.bilagrid_rgb.g2); put(b, i, s.bilagrid_rgb.accum_f);
        if (s.bilagrid_rgb.grids_quant.initialized())
            put_quant(b, i, s.bilagrid_rgb.grids_quant);
        if (s.bilagrid_rgb.quant_state.initialized())
            put_quant(b, i, s.bilagrid_rgb.quant_state);
        if (s.bilagrid_rgb.adagrad_quant.initialized())
            put_quant(b, i, s.bilagrid_rgb.adagrad_quant);
    }
    if (s.bilagrid_depth.enabled) {
        put(b, i, s.bilagrid_depth.grids); put(b, i, s.bilagrid_depth.g1);
        put(b, i, s.bilagrid_depth.g2); put(b, i, s.bilagrid_depth.accum_f);
        put(b, i, s.bilagrid_depth.scalars);
        if (s.bilagrid_depth.grids_quant.initialized())
            put_quant(b, i, s.bilagrid_depth.grids_quant);
        if (s.bilagrid_depth.quant_state.initialized())
            put_quant(b, i, s.bilagrid_depth.quant_state);
        if (s.bilagrid_depth.adagrad_quant.initialized())
            put_quant(b, i, s.bilagrid_depth.adagrad_quant);
    }
    if (s.bilagrid_normal.enabled) {
        put(b, i, s.bilagrid_normal.grids); put(b, i, s.bilagrid_normal.g1);
        put(b, i, s.bilagrid_normal.g2); put(b, i, s.bilagrid_normal.accum_f);
        if (s.bilagrid_normal.grids_quant.initialized())
            put_quant(b, i, s.bilagrid_normal.grids_quant);
        if (s.bilagrid_normal.quant_state.initialized())
            put_quant(b, i, s.bilagrid_normal.quant_state);
        if (s.bilagrid_normal.adagrad_quant.initialized())
            put_quant(b, i, s.bilagrid_normal.adagrad_quant);
    }
    need(i == b.size(), "shared field count mismatch");
}
Fields source_state() {
    const auto& w = engine().world;
    const auto& o = engine().optim;
    Fields b;
    take(b, w.means); take(b, w.quats); take(b, w.scales);
    take(b, w.opacities); take(b, w.features_dc);
    take_quant(b, w.features_sh_quant16_fpbo);
    take_quant(b, o.means_quant_state_fpbo);
    take_quant(b, o.quats_quant_state_fpbo);
    take_quant(b, o.scales_quant_state_fpbo);
    take_quant(b, o.opacities_quant_state_fpbo);
    take_quant(b, o.features_dc_quant_state_fpbo);
    take_quant(b, o.sh_quant_state_fpbo);
    take(b, o.bias_correction_steps);
    const Fields stats = statistics();
    b.insert(b.end(), stats.begin(), stats.end());
    const Fields shared = shared_state();
    b.insert(b.end(), shared.begin(), shared.end());
    return b;
}
void restore_source(const Fields& b) {
    auto& w = engine().world;
    auto& o = engine().optim;
    size_t i = 0;
    put(b, i, w.means); put(b, i, w.quats); put(b, i, w.scales);
    put(b, i, w.opacities); put(b, i, w.features_dc);
    put_quant(b, i, w.features_sh_quant16_fpbo);
    put_quant(b, i, o.means_quant_state_fpbo);
    put_quant(b, i, o.quats_quant_state_fpbo);
    put_quant(b, i, o.scales_quant_state_fpbo);
    put_quant(b, i, o.opacities_quant_state_fpbo);
    put_quant(b, i, o.features_dc_quant_state_fpbo);
    put_quant(b, i, o.sh_quant_state_fpbo);
    put(b, i, o.bias_correction_steps);
    need(i + 4 <= b.size(), "source statistics field count");
    restore_statistics(Fields(b.begin() + i, b.begin() + i + 4));
    restore_shared(Fields(b.begin() + i + 4, b.end()));
}
void restore(const Fields& b) {
    auto& o = engine().optim;
    restore_world(b);
    size_t i = WorldFields;
    put_quant(b, i, o.means_quant_state_fpbo);
    put_quant(b, i, o.quats_quant_state_fpbo);
    put_quant(b, i, o.scales_quant_state_fpbo);
    put_quant(b, i, o.opacities_quant_state_fpbo);
    put_quant(b, i, o.features_dc_quant_state_fpbo);
    put_quant(b, i, o.sh_quant_state);
    put(b, i, o.bias_correction_steps);
    need(i == b.size(), "unexpected canonical state fields");
}
void initialize(int first = 0, int count = N, bool fpbo = false) {
    std::vector<float> means(count * 3), quats(count * 4, 0), scales(count * 3, -2.4f),
        opacity(count, -0.3f), dc(count * 3, 0.45f), sh(count * K * 3, 0);
    for (int local = 0; local < count; ++local) {
        const int row = first + local;
        means[3 * local] = (row % 16 - 7.5f) * 0.095f;
        means[3 * local + 1] = ((row / 16) % 16 - 7.5f) * 0.095f;
        means[3 * local + 2] = row == N - 1 ? -100.f : (row % 2 ? 0.05f : 0.f);
        quats[4 * local] = 1;
        if (row == 1) means[3 * local] = means[0];
        if (row == 1) means[3 * local + 1] = means[1];
        if (row == 1) means[3 * local + 2] = means[2];
    }
    set_data_3dgs(count, tv(means.data(), 4, {count, 3}), tv(quats.data(), 4, {count, 4}),
                  tv(scales.data(), 4, {count, 3}), tv(opacity.data(), 4, {count, 1}),
                  tv(dc.data(), 4, {count, 3}), tv(sh.data(), 4, {count, K, 3}));
    engine().optim.use_fused_proj_bwd_optim = fpbo;
    engine_ensure_optim_state(8, 16, 16, true);
    engine().optim.radii.zero();
    engine_set_bin_tile_size(16);
}
OptimConfig optim() {
    OptimConfig c{};
    c.use_fused_proj_bwd_optim = false;
    c.quantization_level = 0;
    c.sh_optim_bits = 8; c.sh_value_bits = 16; c.non_sh_optim_bits = 16;
    c.use_per_splat_bias_correction = true;
    c.lr_means = 0.0002f; c.lr_quats = 0.001f; c.lr_scales = 0.001f;
    c.lr_opacities = 0.003f; c.lr_features_dc = 0.001f; c.lr_features_sh = 0.0002f;
    return c;
}
struct View {
    int w, h;
    std::array<float, 16> matrix;
    std::array<float, 4> intr;
    std::array<float, 8> distortion{};
    std::vector<uint8_t> gt;
};
View view(int id) {
    View v{};
    v.w = id == 0 ? 53 : id == 1 ? 64 : 71;
    v.h = id == 0 ? 47 : id == 1 ? 56 : 43;
    v.matrix = {1,0,0,(id - 1)*0.12f, 0,1,0,0, 0,0,1,id == 1 ? -1000.f : 3.0f, 0,0,0,1};
    v.intr = {56, 57, v.w * .5f, v.h * .5f};
    v.gt.resize((size_t)v.w * v.h * 3);
    for (size_t p = 0; p < v.gt.size(); ++p) v.gt[p] = (uint8_t)(23 + (p * 13 + id * 29) % 170);
    return v;
}
Fields gradient() {
    const auto& g = engine().grad;
    Fields b;
    take(b, g.means); take(b, g.quats); take(b, g.scales);
    take(b, g.opacities); take(b, g.features_dc); take(b, g.features_sh);
    return b;
}
void load_gradient(const Fields& b) {
    auto& g = engine().grad;
    const int64_t count = engine().max_num_splats;
    g.means.resize(PoolSlot::EngVMeans, count); g.quats.resize(PoolSlot::EngVQuats, count);
    g.scales.resize(PoolSlot::EngVScales, count); g.opacities.resize(PoolSlot::EngVOpacities, count);
    g.features_dc.resize(PoolSlot::EngVFeaturesDc, count);
    g.features_sh.resize(PoolSlot::EngVFeaturesSh, count, K);
    size_t i = 0;
    put(b, i, g.means); put(b, i, g.quats); put(b, i, g.scales);
    put(b, i, g.opacities); put(b, i, g.features_dc); put(b, i, g.features_sh);
    need(i == b.size(), "gradient field count mismatch");
    g.quantize_grad = false;
    engine().optim.grad_scale = 1.f / Views;
    engine().optim.zero_grad_in_optim = false;
}
struct ImageResult {
    Fields grad;
    Bytes render;
    std::vector<float> loss;
    int image = -1, faces = 0, width = 0, height = 0;
};
ImageResult render_backward(int id, int step) {
    const View v = view(id);
    set_camera_params(v.w, v.h, "PINHOLE", "NONE",
                      tv(v.matrix.data(), 4, {1,4,4}), tv(v.intr.data(), 4, {1,4}),
                      tv(v.distortion.data(), 4, {1,8}));
    set_training_data(tv(v.gt.data(), 1, {1,v.h,v.w,3}), nil(), nil(), nil());
    engine().optim.use_fused_proj_bwd_optim = false;
    engine().optim.skip_grad_zero = false;
    engine().grad.quantize_grad = false;
    forward_3dgs("3dgs", 3, false);
    std::vector<float> rgb((size_t)v.h * v.w * 3);
    engine_copy_render_to_host(tv(rgb.data(), 4, {1,v.h,v.w,3}), nil(), nil(), nil(), nil());
    std::array<float, (int)LossWeightIndex::length> weights{};
    weights[(int)LossWeightIndex::RgbSupL1] = 1.f;
    engine_compute_loss_backward(step, weights, 0.f, 1, 0,
        false, 0, 0.f, 0.f, false, 0.f, 1.f, 0, 0.f);
    const auto losses = engine_compute_loss_backward(step, weights, 0.f, 1, 0,
        false, 0, 0.f, 0.f, false, 0.f, 1.f, 0, 0.f);
    ImageResult result;
    result.grad = gradient();
    result.render.resize(rgb.size() * sizeof(float));
    std::memcpy(result.render.data(), rgb.data(), result.render.size());
    for (const auto& [key, value] : losses) result.loss.push_back(value);
    return result;
}
std::array<int, 2> images_for(int owner, int step) {
    if (step == 0) return owner == 0 ? std::array<int, 2>{0, 2} : std::array<int, 2>{1, -1};
    return owner == 0 ? std::array<int, 2>{0, 1} : std::array<int, 2>{2, -1};
}
bool row_nonzero(const Fields& grad, int row) {
    for (const auto& field : grad) {
        need(field.size() % (N * sizeof(float)) == 0, "gradient row layout");
        const size_t width = field.size() / N;
        for (size_t p = row * width; p < (row + 1) * width; p += sizeof(float)) {
            float value;
            std::memcpy(&value, field.data() + p, sizeof(value));
            if (value != 0.f) return true;
        }
    }
    return false;
}
uint64_t hash_bytes(const Bytes& b, uint64_t h = 14695981039346656037ULL) {
    for (uint8_t c : b) h = (h ^ c) * 1099511628211ULL;
    return h;
}
uint64_t hash_file(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    need((bool)f, "missing identity file");
    uint64_t h = 14695981039346656037ULL;
    std::vector<char> buf(1 << 20);
    while (f) {
        f.read(buf.data(), buf.size());
        for (std::streamsize i = 0; i < f.gcount(); ++i)
            h = (h ^ (uint8_t)buf[(size_t)i]) * 1099511628211ULL;
    }
    need(f.eof(), "identity file read failed");
    return h;
}
struct Header {
    uint64_t magic, source, attempt, topology, layout, first, rows;
    uint64_t version, step, kind, owner, count, bytes, hash;
};
template<class Write>
void packet_write(Write write, uint64_t version, uint64_t step,
                  uint64_t kind, uint64_t owner, const Fields& b, const Fields* tail = nullptr) {
    Header h{Magic, packet_source, packet_attempt, packet_topology,
             packet_layout, packet_first, packet_rows, wire_kind(version), step, wire_kind(kind),
             owner, b.size() + (tail ? tail->size() : 0), 0, 0};
    const auto hash_started = Clock::now();
    PacketDigest digest;
    for (const Fields* group : {&b, tail}) if (group) for (const Bytes& field : *group) {
        need(field.size() <= 256ULL * 1024 * 1024, "oversized packet field");
        h.bytes += sizeof(uint64_t) + field.size();
        digest.field(field);
    }
    h.hash = digest.finish();
    packet_hash_seconds += std::chrono::duration<double>(Clock::now() - hash_started).count();
    need(h.count <= 96 && h.bytes + sizeof(Header) <= 1024ULL * 1024 * 1024,
         "packet exceeds bounded staging");
    write(&h, sizeof(h));
    for (const Fields* group : {&b, tail}) if (group) for (const Bytes& field : *group) {
        const uint64_t size = field.size();
        write(&size, sizeof(size));
        write(field.data(), (size_t)size);
    }
    packet_write_payload_hash = h.hash;
}
void packet(const fs::path& path, uint64_t version, uint64_t step,
            uint64_t kind, uint64_t owner, const Fields& b, const Fields* tail = nullptr) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    need((bool)f, "cannot create packet");
    packet_write([&](const void* p, size_t n) {
        f.write((const char*)p, (std::streamsize)n);
        need((bool)f, "cannot write complete packet");
    }, version, step, kind, owner, b, tail);
    f.flush();
    f.close();
    need((bool)f, "cannot write complete packet");
}
template<class Read>
Fields receive_read(Read read, uint64_t length, uint64_t version, uint64_t step,
                    uint64_t kind, uint64_t owner, size_t count) {
    need(length >= sizeof(Header), "partial packet header");
    need(length <= 1024ULL * 1024 * 1024, "packet exceeds bounded staging");
    Header h{};
    read(&h, sizeof(h));
    need(h.magic == Magic && h.source == packet_source &&
         h.attempt == packet_attempt && h.topology == packet_topology &&
         h.layout == packet_layout && h.first == packet_first &&
         h.rows == packet_rows && h.version == wire_kind(version) &&
         h.step == step && h.kind == wire_kind(kind) && h.owner == owner && h.count == count &&
         h.bytes == (uint64_t)length - sizeof(Header), "stale or mismatched packet");
    need(h.bytes <= 1024ULL * 1024 * 1024 && h.count <= 96, "packet exceeds bounded staging");
    Fields fields;
    uint64_t remain = h.bytes;
    PacketDigest hasher;
    for (uint64_t i = 0; i < h.count; ++i) {
        uint64_t size = 0;
        need(remain >= sizeof(size), "partial field length");
        read(&size, sizeof(size));
        remain -= sizeof(size);
        need(size <= remain && size <= 256ULL * 1024 * 1024, "partial or oversized field");
        Bytes b((size_t)size);
        read(b.data(), (size_t)size);
        remain -= size;
        const auto hash_started = Clock::now();
        hasher.field(b);
        packet_hash_seconds += std::chrono::duration<double>(Clock::now() - hash_started).count();
        fields.push_back(std::move(b));
    }
    const auto finish_started = Clock::now();
    const uint64_t digest = hasher.finish();
    packet_hash_seconds += std::chrono::duration<double>(Clock::now() - finish_started).count();
    need(remain == 0 && digest == h.hash, "packet content mismatch");
    packet_read_payload_hash = digest;
    return fields;
}
Fields receive(const fs::path& path, uint64_t version, uint64_t step,
               uint64_t kind, uint64_t owner, size_t count) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    need((bool)f, "missing packet");
    const auto length = f.tellg();
    need(length >= 0, "packet length unavailable");
    f.seekg(0);
    return receive_read([&](void* p, size_t n) {
        f.read((char*)p, (std::streamsize)n);
        need((bool)f, "partial field payload");
    }, (uint64_t)length, version, step, kind, owner, count);
}
#ifdef _WIN32
struct TimingMapping {
    struct Control { uint64_t capacity; volatile LONG64 length; };
    HANDLE handle = nullptr;
    Control* view = nullptr;
    uint64_t capacity;
    TimingMapping(uint64_t attempt, int owner, uint64_t bytes, bool create) : capacity(bytes) {
        need(bytes >= sizeof(Header) && bytes <= 1024ULL * 1024 * 1024,
             "mapping exceeds bounded packet");
        const std::string name = "Local\\SpirulaG1Timing-" + std::to_string(attempt) +
            "-" + std::to_string(owner) + "-packet";
        const uint64_t total = bytes + sizeof(Control);
        handle = create ? CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            (DWORD)(total >> 32), (DWORD)total, name.c_str()) :
            OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
        const bool collision = create && GetLastError() == ERROR_ALREADY_EXISTS;
        if (handle && !collision)
            view = (Control*)MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)total);
        if (!view) {
            if (handle) CloseHandle(handle);
            throw std::runtime_error("mapping creation/open/view failed or attempt collision");
        }
        if (create) { view->capacity = bytes; view->length = 0; }
        if (view->capacity != bytes) {
            UnmapViewOfFile(view); CloseHandle(handle);
            throw std::runtime_error("mapping capacity mismatch");
        }
    }
    ~TimingMapping() { UnmapViewOfFile(view); CloseHandle(handle); }
    TimingMapping(const TimingMapping&) = delete;
    TimingMapping& operator=(const TimingMapping&) = delete;
    uint64_t length() const {
        need(view->capacity == capacity, "mapping capacity changed");
        const LONG64 n = InterlockedCompareExchange64(&view->length, 0, 0);
        need(n > 0 && (uint64_t)n <= capacity, "missing or oversized mapped packet");
        return (uint64_t)n;
    }
    uint64_t write(uint64_t version, uint64_t step, uint64_t kind, uint64_t owner,
                   const Fields& fields, const Fields* tail = nullptr) {
        need(view->capacity == capacity &&
             InterlockedCompareExchange64(&view->length, 0, 0) == 0,
             "mapped slot reused before acknowledgement");
        size_t offset = 0;
        packet_write([&](const void* p, size_t n) {
            need(n <= capacity - offset, "mapped packet exceeds capacity");
            if (n) {
                const auto started = Clock::now();
                std::memcpy((uint8_t*)(view + 1) + offset, p, n);
                packet_copy_seconds += std::chrono::duration<double>(Clock::now() - started).count();
            }
            offset += n;
        }, version, step, kind, owner, fields, tail);
        InterlockedExchange64(&view->length, (LONG64)offset);
        return offset;
    }
    Fields read(uint64_t version, uint64_t step, uint64_t kind, uint64_t owner, size_t count) {
        const uint64_t n = length();
        size_t offset = 0;
        auto fields = receive_read([&](void* p, size_t bytes) {
            need(bytes <= n - offset, "partial mapped payload");
            if (bytes) {
                const auto started = Clock::now();
                std::memcpy(p, (uint8_t*)(view + 1) + offset, bytes);
                packet_copy_seconds += std::chrono::duration<double>(Clock::now() - started).count();
            }
            offset += bytes;
        }, n, version, step, kind, owner, count);
        need(offset == n, "mapped packet trailing bytes");
        InterlockedExchange64(&view->length, 0); // event protocol acknowledges before next write
        return fields;
    }
};
TimingMapping* timing_slots[2] = {nullptr, nullptr};
#endif
Fields receive_once(const fs::path& path, uint64_t version, uint64_t step,
                    uint64_t kind, uint64_t owner, size_t count,
                    std::set<std::string>& accepted, int image = -1) {
    const auto fields = receive(path, version, step, kind, owner, count);
    const std::string key = std::to_string(version) + ":" + std::to_string(step) +
        ":" + std::to_string(kind) + ":" + std::to_string(owner) + ":" + std::to_string(image);
    need(accepted.insert(key).second, "duplicate contribution rejected");
    return fields;
}
uint64_t digest_word(uint64_t digest, uint64_t value) {
    for (int j = 0; j < 8; ++j)
        digest = (digest ^ (uint8_t)(value >> (j * 8))) * 1099511628211ULL;
    return digest;
}
uint64_t frozen_state_digest(const Fields& state) {
    need(state.size() >= 24, "frozen native state schema");
    uint64_t digest = digest_word(14695981039346656037ULL, state.size() - 20);
    for (size_t i = 20; i < state.size(); ++i)
        digest = hash_bytes(state[i], digest_word(digest, state[i].size()));
    return digest;
}
std::array<uint8_t, sizeof(uint64_t)> frozen_owner_attestation(
    uint64_t frozen, uint64_t step, uint64_t owner) {
    uint64_t digest = 14695981039346656037ULL;
    for (uint64_t value : {Magic, packet_source, packet_attempt, packet_topology,
         packet_layout, packet_first, packet_rows, step + 1, step,
         uint64_t{12}, owner, frozen})
        digest = digest_word(digest, value);
    std::array<uint8_t, sizeof(uint64_t)> out{};
    std::memcpy(out.data(), &digest, sizeof(digest));
    return out;
}
void validate_frozen_owner(const Fields& reply, uint64_t frozen,
                           uint64_t step, uint64_t owner) {
    const auto expected = frozen_owner_attestation(frozen, step, owner);
    need(reply.size() == 21 && reply.back().size() == expected.size() &&
         std::equal(expected.begin(), expected.end(), reply.back().begin()),
         "stale or changed frozen owner context");
}
int measured_exposure_count(const std::string& token) {
    need(!token.empty() && token.find_first_not_of("0123456789") == std::string::npos,
         "measured exposures must be a positive even integer");
    size_t used = 0;
    const uint64_t count = std::stoull(token, &used);
    need(used == token.size() && count > 0 && count % 2 == 0 &&
         count <= (uint64_t)std::numeric_limits<int>::max(),
         "measured exposures must be a positive even integer");
    return (int)count;
}
struct NumericalError {
    uint64_t values = 0, outside = 0;
    double max_abs = 0, squared_error = 0, squared_ref = 0;
    void add(const Bytes& a, const Bytes& b, float atol) {
        need(a.size() == b.size() && a.size() % sizeof(float) == 0, "float field shape parity");
        values += a.size() / sizeof(float);
        for (size_t j = 0; j < a.size(); j += sizeof(float)) {
            float x, y;
            std::memcpy(&x, a.data() + j, 4);
            std::memcpy(&y, b.data() + j, 4);
            if (!std::isfinite(x) || !std::isfinite(y)) { ++outside; continue; }
            const double error = std::abs((double)x - y);
            max_abs = std::max(max_abs, error);
            squared_error += error * error;
            squared_ref += (double)x * x;
            outside += error > atol + 0.003f * std::max(std::abs(x), std::abs(y));
        }
    }
    void judge(const char* what, float atol) const {
        std::printf("G1 numerical label=%s values=%llu maxabs=%.9g relRMS=%.9g out_of_tolerance=%llu atol=%.9g rtol=0.003\n",
            what, (unsigned long long)values, max_abs,
            std::sqrt(squared_error / std::max(squared_ref, 1e-30)),
            (unsigned long long)outside, atol);
        need(outside == 0, what);
    }
};
void compare(const Bytes& a, const Bytes& b, const char* what, float atol = 0) {
    if (atol == 0) { need(a == b, what); return; }
    NumericalError error;
    error.add(a, b, atol);
    error.judge(what, atol);
}
void compare(const Fields& a, const Fields& b, const char* what, float atol = 0) {
    need(a.size() == b.size(), "field count parity");
    if (atol == 0) { need(a == b, what); return; }
    NumericalError error;
    for (size_t i = 0; i < a.size(); ++i) error.add(a[i], b[i], atol);
    error.judge(what, atol);
}
void add(Fields& sum, const Fields& next) {
    if (sum.empty()) { sum = next; return; }
    need(sum.size() == next.size(), "gradient count mismatch");
    for (size_t i = 0; i < sum.size(); ++i) {
        need(sum[i].size() == next[i].size() && sum[i].size() % 4 == 0, "gradient shape mismatch");
        for (size_t p = 0; p < sum[i].size(); p += 4) {
            float a, b;
            std::memcpy(&a, sum[i].data() + p, 4);
            std::memcpy(&b, next[i].data() + p, 4);
            a += b;
            std::memcpy(sum[i].data() + p, &a, 4);
        }
    }
}
Fields half(const Fields& full, int owner) {
    Fields result;
    for (const auto& b : full) {
        need(b.size() % 2 == 0, "unaligned owner codec block");
        const size_t n = b.size() / 2;
        result.emplace_back(b.begin() + owner * n, b.begin() + (owner + 1) * n);
    }
    return result;
}
void merge(Fields& full, const Fields& a, const Fields& b) {
    need(full.size() == a.size() && a.size() == b.size(), "owner state fields");
    for (size_t i = 0; i < full.size(); ++i) {
        need(full[i].size() == a[i].size() + b[i].size(), "owner state field shape");
        std::copy(a[i].begin(), a[i].end(), full[i].begin());
        std::copy(b[i].begin(), b[i].end(), full[i].begin() + a[i].size());
    }
}
#ifdef _WIN32
using Child = intptr_t;
Child launch(const char* exe, const char* mode, const char* device,
             const fs::path& in, const fs::path& out, int step, int owner) {
    const std::string s = std::to_string(step), o = std::to_string(owner);
    Child pid = _spawnl(_P_NOWAIT, exe, exe, mode, device, in.string().c_str(),
                        out.string().c_str(), s.c_str(), o.c_str(), nullptr);
    need(pid != -1, "worker spawn failed");
    return pid;
}
bool join(Child child) { int status = 0; return _cwait(&status, child, 0) != -1 && status == 0; }
#else
using Child = pid_t;
Child launch(const char* exe, const char* mode, const char* device,
             const fs::path& in, const fs::path& out, int step, int owner) {
    Child pid = fork();
    need(pid >= 0, "worker fork failed");
    if (pid == 0) {
        std::string s = std::to_string(step), o = std::to_string(owner);
        execl(exe, exe, mode, device, in.string().c_str(), out.string().c_str(),
              s.c_str(), o.c_str(), (char*)nullptr);
        _exit(127);
    }
    return pid;
}
bool join(Child child) {
    int status = 0;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif
void run_pair(const char* exe, const char* mode, const DevicePair& devices,
              const fs::path (&in)[2], const fs::path (&out)[2], int step) {
    const Child first = launch(exe, mode, devices[0].c_str(), in[0], out[0], step, 0);
    Child second;
    try {
        second = launch(exe, mode, devices[1].c_str(), in[1], out[1], step, 1);
    } catch (...) {
        join(first);
        throw;
    }
    const bool first_ok = join(first);
    const bool second_ok = join(second);
    need(first_ok && second_ok, "worker failed");
}

void worker(int argc, char** argv) {
    need(argc == 7, "worker arguments");
    const std::string mode = argv[1];
    const int step = std::stoi(argv[5]), owner = std::stoi(argv[6]);
    need(step >= 0 && step < Steps && owner >= 0 && owner < 2, "worker identity");
    need(mode == "grad" || mode == "owner", "worker mode");
    need(std::string(argv[2]).rfind("uuid:", 0) == 0, "explicit device UUID required");
    need(backend::device_select_identity(argv[2]), "device UUID unavailable");
    initialize(mode == "owner" ? owner * (N / 2) : 0, mode == "owner" ? N / 2 : N);
    const uint64_t version = (uint64_t)step + 1;
    const size_t current_count = mode == "grad" ? WorldFields : state().size();
    const auto fields = receive(argv[3], version, step, mode == "grad" ? 1 : 3,
                                owner, mode == "grad" ? current_count : current_count + 6);
    Fields input(fields.begin(), fields.begin() + current_count);
    if (mode == "grad") restore_world(input);
    else restore(input);
    if (mode == "grad") {
        const auto ids = images_for(owner, step);
        Fields result;
        for (int id : ids) {
            if (id < 0) continue;
            const auto image = render_backward(id, step);
            Fields row = image.grad;
            row.push_back(image.render);
            Bytes loss(image.loss.size() * sizeof(float));
            std::memcpy(loss.data(), image.loss.data(), loss.size());
            row.push_back(std::move(loss));
            Bytes identity((N + 1) * sizeof(uint32_t));
            const uint32_t image_id = (uint32_t)id;
            std::memcpy(identity.data(), &image_id, 4);
            for (uint32_t source_row = 0; source_row < N; ++source_row)
                std::memcpy(identity.data() + (source_row + 1) * 4, &source_row, 4);
            row.push_back(std::move(identity));
            for (auto& field : row) result.push_back(std::move(field));
        }
        packet(argv[4], version, step, 2, owner, result);
    } else {
        Fields grad(fields.begin() + current_count, fields.end());
        load_gradient(grad);
        engine_optim_step(step, optim());
        packet(argv[4], version, step, 4, owner, state());
    }
}

template<class T> uint64_t bytes(const DeviceVector<T>& v) { return (uint64_t)v.size() * sizeof(T); }
template<class T> uint64_t bytes(const DeviceTensor2D<T>& v) {
    return v.data_ptr() ? (uint64_t)v.numel() * sizeof(T) : 0;
}
template<class Q> uint64_t codec_bytes(const Q& q) {
    return q.initialized() ? (uint64_t)q.packed_bytes() +
        (uint64_t)q.n_bounds * sizeof(*q.bounds_ptr()) : 0;
}
uint64_t world_bytes() {
    const auto& w = engine().world;
    return bytes(w.means) + bytes(w.quats) + bytes(w.scales) +
        bytes(w.opacities) + bytes(w.features_dc) + bytes(w.features_sh) +
        codec_bytes(w.features_sh_quant8) + codec_bytes(w.features_sh_quant16) +
        codec_bytes(w.features_sh_quant8_fpbo) + codec_bytes(w.features_sh_quant16_fpbo);
}
uint64_t owner_state_bytes() {
    const auto& o = engine().optim;
    return bytes(o.g1_means) + bytes(o.g2_means) + bytes(o.g1_quats) + bytes(o.g2_quats) +
        bytes(o.g1_scales) + bytes(o.g2_scales) + bytes(o.g1_opacities) + bytes(o.g2_opacities) +
        bytes(o.g1_features_dc) + bytes(o.g2_features_dc) +
        bytes(o.g1_features_sh) + bytes(o.g2_features_sh) +
        codec_bytes(o.sh_quant_state) + codec_bytes(o.sh_quant_state_fpbo) +
        codec_bytes(o.means_quant_state_fpbo) + codec_bytes(o.quats_quant_state_fpbo) +
        codec_bytes(o.scales_quant_state_fpbo) + codec_bytes(o.opacities_quant_state_fpbo) +
        codec_bytes(o.features_dc_quant_state_fpbo) + bytes(o.bias_correction_steps);
}
template<class T> void scan_gradient(const T& v, size_t rows, size_t row_bytes,
                                     std::vector<uint8_t>& dirty) {
    if (!row_bytes || !v.data_ptr()) return;
    need((uint64_t)rows * row_bytes == bytes(v), "gradient layout mismatch");
    constexpr size_t ChunkBytes = 2 * 1024 * 1024;
    const size_t chunk_rows = std::max<size_t>(1, ChunkBytes / row_bytes);
    std::vector<float> host((std::min(rows, chunk_rows) * row_bytes + 3) / 4);
    for (size_t first = 0; first < rows; first += chunk_rows) {
        const size_t count = std::min(chunk_rows, rows - first);
        backend::memcpy_sync(host.data(), (const uint8_t*)v.data_ptr() + first * row_bytes,
                             count * row_bytes, backend::MemcpyKind::DeviceToHost);
        for (size_t r = 0; r < count; ++r) {
            const size_t off = r * row_bytes / sizeof(float);
            for (size_t j = 0; j < row_bytes / sizeof(float); ++j) {
                need(std::isfinite(host[off + j]), "non-finite native gradient");
                dirty[first + r] |= host[off + j] != 0.f;
            }
        }
    }
}
std::vector<uint8_t> dirty_gradient_rows() {
    const auto& g = engine().grad;
    const size_t n = (size_t)engine().max_num_splats;
    std::vector<uint8_t> dirty(n, 0);
    scan_gradient(g.means, n, sizeof(float3), dirty);
    scan_gradient(g.quats, n, sizeof(float4), dirty);
    scan_gradient(g.scales, n, sizeof(float3), dirty);
    scan_gradient(g.opacities, n, sizeof(float), dirty);
    scan_gradient(g.features_dc, n, sizeof(float3), dirty);
    scan_gradient(g.features_sh, n, (size_t)engine().num_sh * sizeof(float3), dirty);
    return dirty;
}
void packet_projection_check();
void packet_check() {
    const fs::path path = fs::temp_directory_path() /
        ("g1-packet-" + std::to_string(Clock::now().time_since_epoch().count()));
    const Fields payload{{1, 2, 3, 4}};
    packet_source = 39; packet_attempt = 7; packet_topology = 512;
    packet_layout = 16; packet_first = 256; packet_rows = 256;
    auto rejects = [&] {
        bool rejected = false;
        try { (void)receive(path, 2, 10, 3, 1, 1); }
        catch (const std::runtime_error&) { rejected = true; }
        need(rejected, "invalid packet accepted");
    };
    auto corrupt = [&](size_t offset, uint64_t value) {
        packet(path, 2, 10, 3, 1, payload);
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp((std::streamoff)offset);
        f.write((const char*)&value, sizeof(value));
        need((bool)f, "cannot corrupt packet");
        f.close();
        rejects();
    };
    try {
        packet_projection_check();
        packet(path, 2, 10, 3, 1, payload);
        state_transport = "resident-owned";
        rejects();
        state_transport = "full";
        compare(receive(path, 2, 10, 3, 1, 1), payload, "mode refusal preserves valid packet");
        const Fields two_fields{{1, 2, 3, 4}, {5, 6}};
        packet(path, 2, 10, 3, 1, two_fields);
        {
            std::fstream partial(path, std::ios::binary | std::ios::in | std::ios::out);
            const uint64_t invalid_length = 1024;
            partial.seekp(sizeof(Header) + sizeof(uint64_t) + two_fields[0].size());
            partial.write((const char*)&invalid_length, sizeof(invalid_length));
            need((bool)partial, "partial digest corruption write failed");
        }
        bool partial_rejected = false;
        try { (void)receive(path, 2, 10, 3, 1, 2); }
        catch (const std::runtime_error&) { partial_rejected = true; }
        need(partial_rejected, "partially hashed malformed packet accepted");
        packet(path, 2, 10, 3, 1, payload);
        compare(receive(path, 2, 10, 3, 1, 1), payload, "digest context recovered after partial receive");
#ifdef _WIN32
        const uint64_t attempt = (uint64_t)Clock::now().time_since_epoch().count();
        TimingMapping slot(attempt, 0, sizeof(Header) + sizeof(uint64_t) + 4, true);
        TimingMapping child(attempt, 0, slot.capacity, false);
        slot.write(2, 10, 3, 1, payload);
        compare(child.read(2, 10, 3, 1, 1), payload, "mapped packet roundtrip");
        auto mapping_rejects = [&](auto action) {
            bool rejected = false;
            try { action(); } catch (const std::runtime_error&) { rejected = true; }
            need(rejected, "invalid mapped slot accepted");
        };
        mapping_rejects([&] { (void)child.read(2, 10, 3, 1, 1); });
        slot.write(2, 10, 3, 1, payload);
        mapping_rejects([&] { slot.write(2, 10, 3, 1, payload); });
        mapping_rejects([&] { (void)child.read(3, 10, 3, 1, 1); });
        mapping_rejects([&] { (void)child.read(2, 11, 3, 1, 1); });
        ((uint8_t*)(slot.view + 1))[slot.capacity - 1] ^= 1;
        mapping_rejects([&] { (void)child.read(2, 10, 3, 1, 1); });
        InterlockedExchange64(&slot.view->length, slot.capacity + 1);
        mapping_rejects([&] { (void)child.read(2, 10, 3, 1, 1); });
        InterlockedExchange64(&slot.view->length, sizeof(Header) - 1);
        mapping_rejects([&] { (void)child.read(2, 10, 3, 1, 1); });
        InterlockedExchange64(&slot.view->length, 0);
        mapping_rejects([&] { slot.write(2, 10, 3, 1, Fields{{1, 2, 3, 4, 5}}); });
        mapping_rejects([&] { TimingMapping oversized(attempt, 1, (1ULL << 30) + 1, true); });
        mapping_rejects([&] { TimingMapping collision(attempt, 0, slot.capacity, true); });
#endif
        packet(path, 2, 10, 3, 1, payload);
        compare(receive(path, 2, 10, 3, 1, 1), payload, "packet roundtrip");
        std::set<std::string> accepted;
        compare(receive_once(path, 2, 10, 3, 1, 1, accepted),
                payload, "accepted contribution");
        bool duplicate_rejected = false;
        try { (void)receive_once(path, 2, 10, 3, 1, 1, accepted); }
        catch (const std::runtime_error&) { duplicate_rejected = true; }
        need(duplicate_rejected && accepted.size() == 1, "duplicate accepted");
        bool incomplete_rejected = false;
        try { need(accepted.size() == 2, "incomplete version publication refused"); }
        catch (const std::runtime_error&) { incomplete_rejected = true; }
        need(incomplete_rejected, "incomplete version published");
        corrupt(offsetof(Header, source), 40);
        corrupt(offsetof(Header, attempt), 8);
        corrupt(offsetof(Header, topology), 513);
        corrupt(offsetof(Header, layout), 17);
        corrupt(offsetof(Header, first), 0);
        corrupt(offsetof(Header, rows), 255);
        corrupt(offsetof(Header, version), 1);
        corrupt(offsetof(Header, step), 9);
        corrupt(offsetof(Header, owner), 0);
        corrupt(offsetof(Header, bytes), 5);
        packet(path, 2, 10, 3, 1, payload);
        fs::resize_file(path, sizeof(Header) + 1);
        rejects();
        Fields frozen(24, Bytes{1, 2, 3, 4});
        const uint64_t frozen_identity = frozen_state_digest(frozen);
        Fields owner_reply(frozen.begin(), frozen.begin() + 20);
        const auto attestation = frozen_owner_attestation(frozen_identity, 10, 1);
        owner_reply.emplace_back(attestation.begin(), attestation.end());
        packet(path, 11, 10, 12, 1, owner_reply);
        validate_frozen_owner(receive(path, 11, 10, 12, 1, 21), frozen_identity, 10, 1);
        auto refuses_frozen = [&](const Fields& reply, uint64_t identity,
                                  uint64_t step, uint64_t owner) {
            bool rejected = false;
            try { validate_frozen_owner(reply, identity, step, owner); }
            catch (const std::runtime_error&) { rejected = true; }
            need(rejected, "invalid frozen owner context accepted");
        };
        frozen[20][0] ^= 1;
        refuses_frozen(owner_reply, frozen_state_digest(frozen), 10, 1);
        refuses_frozen(owner_reply, frozen_identity, 11, 1);
        refuses_frozen(owner_reply, frozen_identity, 10, 0);
        ++packet_attempt;
        refuses_frozen(owner_reply, frozen_identity, 10, 1);
        --packet_attempt;
        ++packet_layout;
        refuses_frozen(owner_reply, frozen_identity, 10, 1);
        --packet_layout;
        owner_reply.back()[0] ^= 1;
        // A valid packet checksum does not make a stale attestation acceptable.
        packet(path, 11, 10, 12, 1, owner_reply);
        refuses_frozen(receive(path, 11, 10, 12, 1, 21), frozen_identity, 10, 1);
        owner_reply.pop_back();
        refuses_frozen(owner_reply, frozen_identity, 10, 1);
        fs::remove(path);
        bool missing_owner = false;
        try { (void)receive(path, 11, 10, 12, 1, 21); }
        catch (const std::runtime_error&) { missing_owner = true; }
        need(missing_owner, "missing owner accepted");
        need(measured_exposure_count("120") == 120, "measured exposure parser");
        for (const char* token : {"", "0", "1", "119", "-2", "+2", "2x", " 2",
                                  "2147483648", "18446744073709551616"}) {
            bool rejected = false;
            try { (void)measured_exposure_count(token); }
            catch (const std::exception&) { rejected = true; }
            need(rejected, "invalid measured exposure count accepted");
        }
        fs::remove(path);
    } catch (...) { fs::remove(path); throw; }
    std::printf("G1 packet-check source/config/attempt/topology/layout/range/version/step/owner/length/truncation/duplicate/incomplete_publication/frozen_attestation/missing_owner/measured_exposure_boundaries=pass\n");
}
void kitchen(int argc, char** argv) {
    need(argc == 8 && std::strcmp(argv[2], "--resume") == 0 &&
         std::strcmp(argv[4], "--device") == 0 &&
         std::strcmp(argv[6], "--images") == 0, "kitchen arguments");
    need(std::string(argv[5]).rfind("uuid:", 0) == 0 &&
         backend::device_select_identity(argv[5]), "explicit kitchen device UUID unavailable");
    TrainConfig cli;
    cli.resume = argv[3];
    cli.cache_images = "disk";
    spirula::TrainerSession session;
    session.cfg = ckpt::build_resume_config(cli, "", {"cache_images"});
    session.write_config_json = false;
    session.out_dir_override = (fs::temp_directory_path() /
        ("g1-kitchen-" + std::to_string(Clock::now().time_since_epoch().count()))).string();
    session.check_config();
    session.load_dataset();
    std::vector<int32_t> image_ids;
    const std::string list = argv[7];
    for (size_t start = 0; start < list.size();) {
        size_t end = list.find(',', start);
        if (end == std::string::npos) end = list.size();
        need(end > start, "empty image ID");
        const std::string part = list.substr(start, end - start);
        size_t consumed = 0;
        const long long id = std::stoll(part, &consumed);
        need(consumed == part.size() && id >= 0 && id < session.ds.num_cameras &&
             id <= std::numeric_limits<int32_t>::max(), "invalid input image ID");
        need(std::find(session.ds.train_indices.begin(), session.ds.train_indices.end(), id)
             != session.ds.train_indices.end(), "image ID must belong to training split");
        image_ids.push_back((int32_t)id);
        start = end + 1;
    }
    need(!image_ids.empty() && list.back() != ',', "at least one image ID required");
    need(session.cfg.packed || session.cfg.use_bvh,
         "kitchen candidate IDs require native packed rendering");
    session.setup_engine();
    const int step = session.start_step;
    const auto sc = spirula::build_step_config(session.cfg, session.st, step);
    engine().ppisp.cur_run_before_bilagrid = sc.ppisp.run_before_bilagrid;
    engine().ppisp.cur_run_before_color_space =
        sc.ppisp.run_before_color_space && engine().color_space.splat_enabled;
    if (engine().background.enabled)
        engine_set_background_step_params(0, 0.f, sc.loss.num_loss_scales,
                                          sc.loss.loss_scale_min_pixels);
    engine().optim.use_fused_proj_bwd_optim = false;
    engine().optim.skip_grad_zero = false;
    engine().grad.quantize_grad = false;
    const int64_t active = engine().cur_num_splats, capacity = engine().max_num_splats;
    need(active > 0 && capacity >= active && capacity <= INT32_MAX, "invalid checkpoint row count");
    const uint64_t world = world_bytes(), state = owner_state_bytes();
    const uint64_t dense_grad = (uint64_t)capacity *
        (sizeof(float3) * 3 + sizeof(float4) + sizeof(float) +
         (uint64_t)engine().num_sh * sizeof(float3));
    const int sh_degree = std::min(session.cfg.sh_degree,
        step / std::max(session.cfg.sh_degree_warmup_every, 1));
    const int64_t owner_split = ((active + 255) / 256 / 2) * 256;
    std::vector<uint8_t> union_seen((size_t)active, 0);
    uint64_t total_pixels = 0, total_grad_bytes = 0, total_intersections = 0;
    int64_t total_faces = 0;
    std::printf("G1 kitchen mode=actual_native_gradient_traffic checkpoint=%s step=%d input_images=%zu active_rows=%lld capacity_rows=%lld sh_coefficients=%d owner_split_row=%lld owner_block_rows=256 source_layout=checkpoint_native gradient_path=nonfused frozen_topology=1 frozen_appearance_update=1 random_background_sampling=off native_loss_weights=1 native_forward_appearance=1 tile_skip=off world_bytes=%llu owner_state_bytes=%llu dense_fp32_grad_bytes_per_pass=%llu\n",
        cli.resume.c_str(), step, image_ids.size(), (long long)active, (long long)capacity,
        engine().num_sh, (long long)owner_split, (unsigned long long)world, (unsigned long long)state,
        (unsigned long long)dense_grad);
    for (int32_t id : image_ids) {
        DecodedBatch batch;
        engine().dm->fetch_one(id, batch);
        int passes = 0;
        uint64_t image_pixels = 0, image_intersections = 0;
        std::array<uint64_t, 2> owner_candidates{};
        std::vector<uint8_t> image_seen((size_t)active, 0);
        for (int pass = 0;; ++pass) {
            const int views = engine_preview_forward(
                id, session.cfg.primitive, sh_degree,
                session.cfg.packed || session.cfg.use_bvh, true, sc.loss, pass, &passes);
            need(passes > 0 && pass < passes, "invalid native face pass");
            const int width = engine().camera.width, height = engine().camera.height;
            const int64_t nnz = engine().fwd.gaussian_ids.size();
            const int64_t intersections = engine().fwd.flatten_ids.size();
            need(views > 0 && nnz >= 0 && (uint64_t)nnz <=
                 (uint64_t)views * (uint64_t)active && intersections >= 0,
                 "invalid native candidate counts");
            std::vector<int32_t> ids((size_t)nnz);
            if (nnz) backend::memcpy_sync(ids.data(), engine().fwd.gaussian_ids.data_ptr(),
                (size_t)nnz * sizeof(int32_t), backend::MemcpyKind::DeviceToHost);
            std::array<uint64_t, 2> face_candidates{};
            for (int32_t row : ids) {
                need(row >= 0 && row < active, "native candidate row outside active world");
                const int owner = row >= owner_split;
                ++face_candidates[(size_t)owner];
                if (!image_seen[(size_t)row]) ++owner_candidates[(size_t)owner];
                image_seen[(size_t)row] = union_seen[(size_t)row] = 1;
            }
            const auto losses = engine_compute_loss_backward(
                step, sc.loss.weights, sc.loss.w_ssim, sc.loss.num_loss_scales,
                sc.loss.loss_scale_min_pixels, sc.loss.compute_loss_map,
                sc.loss.loss_map_mode, sc.loss.robust_edge_aware_quantile,
                sc.loss.nms_falloff, sc.loss.loss_map_normalize,
                sc.loss.loss_map_clip_quantile, sc.loss.loss_map_power,
                sc.loss.loss_map_accum_mode, sc.loss.saturation_threshold,
                sc.loss.overexposure_reg_weight, sc.loss.color_shift_reg_weight,
                sc.loss.color_shift_reg_beta);
            (void)losses;
            const auto dirty = dirty_gradient_rows();
            const uint64_t dirty_rows = std::count(dirty.begin(), dirty.begin() + active, 1);
            const uint64_t pixels = (uint64_t)views * width * height;
            image_pixels += pixels;
            image_intersections += (uint64_t)intersections;
            total_grad_bytes += dense_grad;
            total_pixels += pixels;
            total_intersections += (uint64_t)intersections;
            total_faces += views;
            const int first_face = batch.face_passes.empty() ? 0 : batch.face_passes[(size_t)pass].k0;
            std::printf("G1 kitchen input=%d face_start=%d post_camera_start=%d faces=%d width=%d height=%d pixels=%llu candidate_rows=%lld owner_half_candidates=%llu,%llu tile_intersections=%lld gradient_dirty_rows=%llu gradient_zero_rows=%llu gradient_payload_bytes=%llu\n",
                id, first_face, batch.post_offsets.empty() ? id : batch.post_offsets[0] + first_face,
                views, width, height, (unsigned long long)pixels, (long long)nnz,
                (unsigned long long)face_candidates[0], (unsigned long long)face_candidates[1],
                (long long)intersections, (unsigned long long)dirty_rows,
                (unsigned long long)(active - dirty_rows), (unsigned long long)dense_grad);
            if (pass + 1 == passes) break;
        }
        std::printf("G1 kitchen image=%d faces=%d pixels=%llu candidate_union=%llu owner_half_union=%llu,%llu tile_intersections=%llu\n",
            id, batch.K, (unsigned long long)image_pixels,
            (unsigned long long)std::count(image_seen.begin(), image_seen.end(), 1),
            (unsigned long long)owner_candidates[0], (unsigned long long)owner_candidates[1],
            (unsigned long long)image_intersections);
    }
    std::printf("G1 kitchen total_faces=%lld total_pixels=%llu candidate_union=%llu total_tile_intersections=%llu replica_world_bytes_two_workers=%llu fp32_gradient_payload_bytes=%llu owner_state_bytes_total=%llu update_semantics=gradient_zero_rows_may_still_change_from_adam_moments_or_regularizers distributed_owner_update=UNVERIFIED transport=NONE performance=NONE\n",
        (long long)total_faces, (unsigned long long)total_pixels,
        (unsigned long long)std::count(union_seen.begin(), union_seen.end(), 1),
        (unsigned long long)total_intersections, (unsigned long long)(2 * world),
        (unsigned long long)total_grad_bytes, (unsigned long long)state);
    session.output_lease.release();
    fs::remove_all(session.out_dir);
}
std::vector<int32_t> parse_images(const std::string& list, int count) {
    std::vector<int32_t> ids;
    for (size_t start = 0; start < list.size();) {
        const size_t end = list.find(',', start);
        const std::string token = list.substr(start, end == std::string::npos ? end : end - start);
        size_t used = 0;
        need(!token.empty(), "empty image ID");
        const long long value = std::stoll(token, &used);
        need(used == token.size() && value >= 0 && value < count, "invalid image ID");
        ids.push_back((int32_t)value);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    need(!ids.empty() && list.back() != ',', "missing image IDs");
    return ids;
}
void source_setup(spirula::TrainerSession& session, const std::string& checkpoint,
                  const std::string& device) {
    need(device.rfind("uuid:", 0) == 0 &&
         backend::device_select_identity(device.c_str()), "explicit source device UUID unavailable");
    TrainConfig cli;
    cli.resume = checkpoint;
    cli.cache_images = "disk";
    session.cfg = ckpt::build_resume_config(cli, "", {"cache_images"});
    session.write_config_json = false;
    session.out_dir_override = (fs::temp_directory_path() /
        ("g1-fpbo-" + std::to_string(Clock::now().time_since_epoch().count()))).string();
    session.check_config();
    session.load_dataset();
    session.setup_engine();
    vkk::g1_set_ordered_raster(true);
    auto sc = engine_resolve_managed_step_config(
        spirula::build_step_config(session.cfg, session.st, session.start_step));
    need(sc.optim.use_fused_proj_bwd_optim && session.cfg.packed &&
         session.cfg.primitive == "3dgs" && sc.optim.quantization_level == 1 &&
         sc.optim.sh_optim_bits == 8 && sc.optim.sh_value_bits == 16 &&
         sc.optim.non_sh_optim_bits == 16 &&
         engine().world.features_sh_quant16_fpbo.initialized(),
         "source must execute packed quantized 3dgs FPBO");
    need(session.cfg.max_steps.value_or(session.cfg.num_iterations) == 30000,
         "source FPBO proof must retain the original 30000-step schedule horizon");
    engine().optim.use_fused_proj_bwd_optim = true;
    engine().optim.skip_grad_zero = false;
    engine().optim.grad_scale = 1;
    engine().optim.zero_grad_in_optim = false;
    engine().grad.quantize_grad = false;
    if (engine().background.enabled)
        engine_set_background_step_params(0, 0.f, sc.loss.num_loss_scales,
                                          sc.loss.loss_scale_min_pixels);
    engine_b1_configure(2, checkpoint, session.start_step);
    packet_source = engine().b1.source_id;
    fs::path cfg = fs::path(checkpoint) / "config.json";
    if (!fs::is_regular_file(cfg)) cfg = fs::path(checkpoint).parent_path() / "config.json";
    packet_layout = hash_file(cfg);
    packet_topology = (uint64_t)engine().cur_num_splats;
    need(packet_source && packet_layout && packet_topology, "missing source identity");
    engine().b1.owner = -1;
    std::printf("G1 source_identity=%llu config_identity=%llu step=%d active=%lld capacity=%lld blocks=%lld last_valid_lanes=%lld fpbo=1 packed=1 quantization=1 sh_optim_bits=8 sh_value_bits=16 non_sh_optim_bits=16 per_splat_bias=%d gradient_scale=1 native_regularizers=once random_background=off tile_skip=off topology=frozen appearance_update=frozen radii_accum=restored_before_each_arm ordered_raster=1\n",
        (unsigned long long)packet_source, (unsigned long long)packet_layout,
        session.start_step, (long long)engine().cur_num_splats,
        (long long)engine().max_num_splats,
        (long long)((engine().cur_num_splats + 255) / 256),
        (long long)((engine().cur_num_splats - 1) % 256 + 1),
        (int)sc.optim.use_per_splat_bias_correction);
    std::printf("G1 source_config lr_means=%.9g lr_quats=%.9g lr_scales=%.9g lr_opacities=%.9g lr_dc=%.9g lr_sh=%.9g scale_reg=%.9g mcmc_opacity_reg=%.9g mcmc_scale_reg=%.9g max_screen_size=%.9g screen_penalty=%.9g loss_scales=%d w_ssim=%.9g loss_weights=",
        sc.optim.lr_means, sc.optim.lr_quats, sc.optim.lr_scales,
        sc.optim.lr_opacities, sc.optim.lr_features_dc, sc.optim.lr_features_sh,
        sc.optim.scale_regularization_weight, sc.optim.mcmc_opacity_reg_weight,
        sc.optim.mcmc_scale_reg_weight, sc.optim.max_screen_size,
        sc.optim.max_screen_size_penalty, sc.loss.num_loss_scales, sc.loss.w_ssim);
    for (float weight : sc.loss.weights) std::printf("%.9g,", weight);
    std::printf(" float_tolerance=0.0003+0.003*maxabs packed_tolerance=0_bytes\n");
}
void source_close(spirula::TrainerSession& session) {
    session.output_lease.release();
    if (!session.out_dir.empty()) fs::remove_all(session.out_dir);
}
ImageResult source_capture(spirula::TrainerSession& session, int id, int step,
                           bool read_render = true, bool read_gradient = true) {
    const auto sc = engine_resolve_managed_step_config(
        spirula::build_step_config(session.cfg, session.st, step));
    const int degree = std::min(session.cfg.sh_degree,
        step / std::max(session.cfg.sh_degree_warmup_every, 1));
    engine().ppisp.cur_run_before_bilagrid = sc.ppisp.run_before_bilagrid;
    engine().ppisp.cur_run_before_color_space =
        sc.ppisp.run_before_color_space && engine().color_space.splat_enabled;
    engine().background.match_luma_pending = sc.background.match_luminance;
    need(sc.loss.color_shift_reg_weight == 0.f,
         "proof requires explicit EMA snapshot when color-shift regularizer is active");
    int passes = 0;
    const int views = engine_preview_forward(
        id, session.cfg.primitive, degree, true, true, sc.loss, 0, &passes);
    need(passes == 1 && views > 0, "FPBO image must have one equal-size face pass");
    if (source_phase_observer) source_phase_observer("after_forward_intersection");
    const int h = engine().camera.height, w = engine().camera.width;
    std::vector<float> rgb;
    if (read_render) {
        rgb.resize((size_t)views * h * w * 3);
        engine_copy_render_to_host(tv(rgb.data(), 4, {views,h,w,3}), nil(), nil(), nil(), nil());
    }
    const Fields prior_stats = statistics();
    auto backward = [&] {
        return engine_compute_loss_backward(
            step, sc.loss.weights, sc.loss.w_ssim, sc.loss.num_loss_scales,
            sc.loss.loss_scale_min_pixels, sc.loss.compute_loss_map,
            sc.loss.loss_map_mode, sc.loss.robust_edge_aware_quantile,
            sc.loss.nms_falloff, sc.loss.loss_map_normalize,
            sc.loss.loss_map_clip_quantile, sc.loss.loss_map_power,
            sc.loss.loss_map_accum_mode, sc.loss.saturation_threshold,
            sc.loss.overexposure_reg_weight, sc.loss.color_shift_reg_weight,
            sc.loss.color_shift_reg_beta);
    };
    (void)backward();
    if (source_phase_observer) source_phase_observer("after_backward_prime");
    restore_statistics(prior_stats);
    const auto losses = backward();
    if (source_phase_observer) source_phase_observer("after_backward");
    if (read_gradient) engine_fpbo_capture_data_gradients(step, sc.optim);
    if (source_phase_observer) source_phase_observer("after_data_gradient_capture");
    ImageResult out;
    out.image = id; out.faces = views; out.width = w; out.height = h;
    if (read_gradient) out.grad = gradient();
    out.render.resize(rgb.size() * sizeof(float));
    if (!rgb.empty()) std::memcpy(out.render.data(), rgb.data(), out.render.size());
    for (const auto& [key, value] : losses) out.loss.push_back(value);
    if (read_render) std::printf("G1 capture step=%d image=%d faces=%d width=%d height=%d pixels=%llu float_gradient_bytes=%llu loss_readback=matched_after_stat_restore\n",
        step, id, views, w, h, (unsigned long long)((uint64_t)views*w*h),
        (unsigned long long)std::accumulate(out.grad.begin(), out.grad.end(), uint64_t(0),
            [](uint64_t n, const Bytes& b) { return n + b.size(); }));
    return out;
}
// Storage observations only; dirtiness is never a sparse-update authorization.
struct O1Field {
    std::string name, pool, shape, format, obligation;
    size_t stride = 0, bound_stride = 0;
    int pair = -1;
    uint32_t transposed_sh = 0;
    size_t cell_bytes = 0;
};
std::ofstream* o1_phases = nullptr;
const char* o1_arm = "setup";
int o1_update = -1;
PoolPhase o1_alias_phase = PoolPhase::None;
bool o1_alias_phase_known = false;
uint64_t o1_stale_alias_checks = 0;
void o1_phase(const char* edge) {
    if (!o1_phases) return;
    auto& out = *o1_phases;
    uint64_t allocated = 0, live = 0;
    for (const auto& r : DevicePool::global().residency()) {
        if (r.arena) {
            need(r.allocated_bytes == 0 && r.live_bytes <= r.requested_bytes, "O1 alias double accounting");
            for (const auto& alias : kAliasRows) {
                const std::string name = slot_name(alias.slot);
                if ((r.name == name || r.name.rfind(name + ".", 0) == 0) &&
                    o1_alias_phase_known && alias.phase != o1_alias_phase) {
                    need(r.live_bytes == 0, "O1 stale alias counted live");
                    ++o1_stale_alias_checks;
                }
            }
        }
        allocated += r.allocated_bytes; live += r.live_bytes;
        out << o1_arm << '\t' << o1_update << '\t' << edge << '\t' << r.name << '\t'
            << to_string(r.category) << '\t' << r.requested_bytes << '\t'
            << r.live_bytes << '\t' << r.allocated_bytes << '\t'
            << (r.arena ? "arena_epoch_validity_not_semantic_liveness" : "last_requested_not_semantic_live") << '\n';
    }
    const auto a = DevicePool::global().arenaStats();
    const auto scratch = engine_get_scratch_bytes();
    out << o1_arm << '\t' << o1_update << '\t' << edge
        << "\talias_arena\tshared\t" << a.want_bytes << "\tunknown\t" << a.cap_bytes
        << "\tcounted_once_slots=" << a.slots << '\n';
    out << o1_arm << '\t' << o1_update << '\t' << edge
        << "\tbackend_scratch\tother\tunknown\tunknown\t" << scratch << "\tretained_capacity\n";
    out << o1_arm << '\t' << o1_update << '\t' << edge
        << "\ttotal_pool_plus_scratch\tall\tunknown\t" << live << '\t'
        << allocated + a.cap_bytes + scratch << "\tlogical_current_epoch_not_semantic_liveness\n";
    out << o1_arm << '\t' << o1_update << '\t' << edge
        << "\tnonpool_driver_pinned_host_image_disk_cache\tunknown\tunknown\tunknown\tunknown"
        << "\tnot_in_pool_total_true_peaks_unknown\n";
    out << o1_arm << '\t' << o1_update << '\t' << edge
        << "\tsemantic_simultaneous_live_bytes\tall\tunknown\tunknown\tunknown"
        << "\tnonalias_last_requested_sizes_cannot_prove_consumer_liveness\n";
    need(out.good(), "O1 phase summary write failed");
}
void o1_pool_edge(PoolPhase next) {
    const auto edge = std::string("before_alias_switch_to_") + to_string(next);
    o1_phase(edge.c_str());
    o1_alias_phase = next;
    o1_alias_phase_known = true;
}
std::vector<O1Field> o1_fields(const Fields& b, size_t rows) {
    std::vector<O1Field> d;
    auto row = [&](std::string name, std::string pool, size_t components, const char* format,
                   const char* obligation) {
        need(d.size() < b.size() && b[d.size()].size() % rows == 0, "O1 row descriptor shape");
        d.push_back({name, pool, std::to_string(rows) + "x" + std::to_string(components),
                     format, obligation, b[d.size()].size() / rows});
    };
    const char* dense = "dense_native_render_read_regularizer_Adam_write_not_visibility_masked";
    row("means", "world.means", 3, "float32", dense);
    row("quats", "world.quats", 4, "float32", dense);
    row("scales", "world.scales", 3, "float32", dense);
    row("opacities", "world.opacities", 1, "float32", dense);
    row("features_dc", "world.features_dc", 3, "float32", dense);
    auto codec = [&](const char* name, const char* pool, size_t components,
                     const char* format, size_t bounds, size_t sh_cell_bytes = 0) {
        const int pair = (int)d.size();
        if (sh_cell_bytes) {
            const auto k = (uint32_t)engine().num_sh;
            need(b[d.size()].size() == (size_t)sh_fpbo_cells(rows, k) * sh_cell_bytes, "O1 transposed SH storage size");
            d.push_back({name, std::string(pool) + ".q",
                std::to_string((rows + 255) / 256) + "x256x" +
                std::to_string(((components + 1) / 2) * 2) + "_transposed_padded_cells",
                format, "dense_native_codec_read_write_full_256_row_reduction",
                0, 0, pair, k, sh_cell_bytes});
        } else {
            row(name, std::string(pool) + ".q", components, format,
                "dense_native_codec_read_write_full_256_row_reduction");
            d.back().pair = pair;
        }
        d.push_back({std::string(name) + ".bounds", std::string(pool) + ".qb",
            std::to_string((rows + 255) / 256) + "x" + std::to_string(bounds / 4),
            "float32", "whole_codec_block_read_write", 0, bounds, pair});
    };
    codec("features_sh", "eng.world.sh_vq16_fpbo", engine().num_sh * 3, "uint16_transposed_codes", 8, sizeof(uint16_t));
    codec("adam.means", "eng.means_qfpbo", 3, "joint_uint16_u_log_s", 16);
    codec("adam.quats", "eng.quats_qfpbo", 4, "joint_uint16_u_log_s", 16);
    codec("adam.scales", "eng.scales_qfpbo", 3, "joint_uint16_u_log_s", 16);
    codec("adam.opacities", "eng.opacities_qfpbo", 1, "joint_uint16_u_log_s", 16);
    codec("adam.features_dc", "eng.features_dc_qfpbo", 3, "joint_uint16_u_log_s", 16);
    codec("adam.features_sh", "eng.sh_quant_fpbo", engine().num_sh * 3, "joint_uint8_transposed_u_log_s", 16,
          QuantizedAdamState<8, 256>::kBytesPerCell);
    row("bias_correction_steps", "eng.bias_correction_steps", 1, "int32", "dense_active_clock_write_when_enabled");
    row("radii", "eng.radii", 1, "float32", "backward_mutation_restored_frozen");
    row("accum_buffer", "eng.accum_buffer", 2, "float32", "backward_mutation_restored_frozen");
    row("densify_oversize", "eng.densify.oversize", 1, "float32", "backward_mutation_restored_frozen");
    row("accum_weight", "raster_bwd.accum_weight", 1, "float32", "backward_mutation_restored_frozen");
    auto shared = [&](std::string name, std::string pool, const char* format = "float32") {
        need(d.size() < b.size(), "O1 shared descriptor overflow");
        d.push_back({name, pool, std::to_string(b[d.size()].size() / 4) + "_flat_native_scalars",
            format, "forward_read_as_enabled_shared_optimizer_frozen"});
    };
    for (const char* n : {"params", "g1", "g2", "accum"}) shared(std::string("ppisp.") + n, std::string("eng.ppisp.") + n);
    {
        const auto& p = engine().ppisp;
        const std::array<const DeviceTensor2D<float>*, 4> tensors{&p.params, &p.g1, &p.g2, &p.accum_f};
        for (size_t i = 0; i < tensors.size(); ++i)
            d[24 + i].shape = std::to_string(tensors[i]->template size<0>()) + "x" +
                              std::to_string(tensors[i]->template size<1>());
    }
    for (const char* n : {"sh_coeffs", "g1", "g2"}) shared(std::string("background.") + n, std::string("eng.bg_sky.") + n);
    for (size_t i = 28; i < 31; ++i) d[i].shape = std::to_string(b[i].size() / sizeof(float3)) + "x3";
    shared("color_shift.ema", "eng.color_shift_reg.ema");
    shared("color_shift.batch_sum", "eng.color_shift_reg.batch_sum");
    shared("color_shift.steps", "", "host_scalar_native_bytes");
    d.back().shape = "1_host_scalar";
    auto grid = [&](const auto& g, const char* kind, bool scalars) {
        if (!g.enabled) return;
        const auto prefix = std::string("eng.bg.") + kind + ".";
        for (const char* n : {"grids", "g1", "g2", "accum"}) shared(std::string("bilagrid.") + kind + "." + n, prefix + n);
        const std::array<const DeviceTensor5D<float>*, 4> tensors{&g.grids, &g.g1, &g.g2, &g.accum_f};
        for (size_t i = 0; i < tensors.size(); ++i) {
            const auto& t = *tensors[i];
            d[d.size() - 4 + i].shape = std::to_string(t.template size<0>()) + "x" +
                std::to_string(t.template size<1>()) + "x" + std::to_string(t.template size<2>()) + "x" +
                std::to_string(t.template size<3>()) + "x" + std::to_string(t.template size<4>());
        }
        if (scalars) shared(std::string("bilagrid.") + kind + ".scalars", prefix + "scalars");
        auto quant = [&](const auto& q, const char* n, const char* format) {
            if (!q.initialized()) return;
            shared(std::string("bilagrid.") + kind + "." + n + ".packed", prefix + n + ".q", format);
            d.back().shape = std::to_string(b[d.size() - 1].size()) + "_packed_bytes";
            shared(std::string("bilagrid.") + kind + "." + n + ".bounds", prefix + n + ".qb");
            d.back().shape = std::to_string(q.n_bounds) + "x" +
                std::to_string(sizeof(*q.bounds_ptr()) / sizeof(float));
        };
        quant(g.grids_quant, "grids_q", "native_value_codes");
        quant(g.quant_state, "bg_quant", "native_joint_Adam_codes");
        quant(g.adagrad_quant, "bg_ag", "native_Adagrad_codes");
    };
    grid(engine().bilagrid_rgb, "rgb", false);
    grid(engine().bilagrid_depth, "depth", true);
    grid(engine().bilagrid_normal, "normal", false);
    need(d.size() == b.size() && d[19].name == "bias_correction_steps" &&
         d[24].name == "ppisp.params", "O1 descriptor/source schema mismatch");
    return d;
}
struct O1Delta { uint64_t bytes = 0; std::vector<uint8_t> rows, blocks; uint64_t padding_bytes = 0; };
O1Delta o1_delta(const Bytes& a, const Bytes& b, size_t rows, size_t stride, size_t bounds,
                 uint32_t transposed_sh = 0, size_t cell_bytes = 0) {
    need(a.size() == b.size(), "O1 field resized");
    O1Delta d{0, std::vector<uint8_t>(rows), std::vector<uint8_t>((rows + 255) / 256)};
    if (stride) need(a.size() == rows * stride, "O1 row stride");
    if (bounds) need(a.size() == d.blocks.size() * bounds, "O1 bound stride");
    const auto address = sh_quant_addr(transposed_sh, 0);
    if (transposed_sh) need(cell_bytes && a.size() == (size_t)sh_fpbo_cells(rows, transposed_sh) * cell_bytes,
                            "O1 transposed SH delta size");
    for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) {
        ++d.bytes;
        if (stride) { d.rows[i / stride] = 1; d.blocks[(i / stride) / 256] = 1; }
        if (transposed_sh) {
            const size_t cell = i / cell_bytes;
            const size_t block = cell / (size_t)address.bounds_stride;
            const size_t local = cell % (size_t)address.bounds_stride;
            const size_t row = block * 256 + (local % (size_t)address.pair_pitch) / 2;
            const size_t component = (local / (size_t)address.pair_pitch) * 2 + local % 2;
            d.blocks[block] = 1;
            if (row < rows && component < (size_t)address.cells_per_splat) d.rows[row] = 1;
            else ++d.padding_bytes;
        }
        if (bounds) {
            const auto block = i / bounds; d.blocks[block] = 1;
            for (size_t r = block * 256; r < std::min(rows, (block + 1) * 256); ++r) d.rows[r] = 1;
        }
    }
    return d;
}
void o1_screen(int argc, char** argv) {
    need(argc == 8 && std::strcmp(argv[2], "--resume") == 0 &&
         std::strcmp(argv[4], "--device") == 0 && std::strcmp(argv[6], "--output") == 0,
         "usage: g1_native_proof --o1-screen --resume CHECKPOINT --device uuid:A --output NEW_DIRECTORY");
    const fs::path root = argv[7], parent = root.parent_path().empty() ? fs::path(".") : root.parent_path();
    need(!root.empty() && !fs::exists(root) && fs::is_directory(parent), "O1 requires new output and existing parent");
    need(fs::space(parent).available > 10ULL * 1024 * 1024 * 1024, "O1 needs 10GiB scratch");
    // One deterministic storage/bounds-tail check; no numerical implementation.
    Bytes a(320, 0), b = a; b[319] = 1;
    auto tail = o1_delta(a, b, 320, 1, 0);
    need(tail.bytes == 1 && tail.rows[319] && std::count(tail.rows.begin(), tail.rows.end(), 1) == 1 &&
         tail.blocks[1] && !tail.blocks[0], "O1 packed tail check");
    a.assign(16, 0); b = a; b[15] = 1; tail = o1_delta(a, b, 320, 0, 8);
    need(tail.bytes == 1 && std::count(tail.rows.begin(), tail.rows.end(), 1) == 64 &&
         tail.blocks[1] && !tail.blocks[0], "O1 bounds tail dependency check");
    // Odd 45-cell SH: separated cell pairs map to one row, while padded
    // tail rows and the unused 46th component dirty bytes/blocks, not rows.
    a.assign((size_t)sh_fpbo_cells(320, 15) * 2, 0); b = a;
    const auto sh_address = sh_quant_addr(15, 0);
    b[(size_t)sh_address.cell(sh_address.base(319), 0) * 2] = 1;
    b[(size_t)sh_address.cell(sh_address.base(319), 2) * 2 + 1] = 1;
    b[(size_t)sh_address.cell(sh_address.base(320), 0) * 2] = 1;
    b[(size_t)sh_address.cell(sh_address.base(319), 45) * 2] = 1;
    tail = o1_delta(a, b, 320, 0, 0, 15, 2);
    need(tail.bytes == 4 && tail.padding_bytes == 2 && tail.rows[319] &&
         std::count(tail.rows.begin(), tail.rows.end(), 1) == 1 &&
         tail.blocks[1] && !tail.blocks[0], "O1 odd transposed SH padded tail check");
    need(fs::create_directory(root), "O1 output creation failed");
    std::ofstream table(root / "fields.tsv"), phases(root / "phases.tsv"), summary(root / "summary.tsv");
    need(table.good() && phases.good() && summary.good(), "O1 summary creation failed");
    phases << "arm\tupdate\tedge\tallocation\tcategory\tlast_requested_bytes\tcurrent_epoch_live_bytes\tretained_allocated_bytes\tnote\n";
    table << "arm\tupdate\tview\tstep\tfield\tshape\tformat\tauthority\tconsumer_phase\tlive_storage_bytes\tallocated_bytes\tchanged_bytes\tchanged_storage_or_bound_dependent_rows\tchanged_blocks\tcodec_union_blocks\tchanged_zero_data_gradient_rows\tchanged_invisible_candidate_rows\tchanged_padding_bytes\tsemantic_obligation\tfrequency\tobservation_stage\n";
    spirula::TrainerSession session;
    try {
        source_setup(session, argv[3], argv[5]);
        const size_t rows = (size_t)engine().max_num_splats;
        need(engine().cur_num_splats == 1000000 && rows == 1000000 &&
             (rows + 255) / 256 == 3907 && (rows - 1) % 256 + 1 == 64, "O1 full source/tail identity");
        need(session.start_step == 10000, "O1 requires retained step-10000 source");
        o1_phases = &phases; source_phase_observer = o1_phase; pool_phase_observer = o1_pool_edge;
        o1_phase("source_setup_before_lazy_allocation");
        // Allocate native backward statistics, then undo the capture's state
        // changes. No optimizer update; new accum_weight storage is zeroed by
        // restore_statistics's existing empty-source rule.
        {
            const Fields loaded = source_state();
            (void)source_capture(session, 1188, session.start_step, false, false);
            restore_source(loaded);
        }
        const Fields source = source_state();
        size_t inactive = 0;
        for (size_t r = 0; r < rows; ++r) {
            float3 mean; std::memcpy(&mean, source[0].data() + r * sizeof(mean), sizeof(mean));
            inactive += !std::isfinite(mean.x) || !std::isfinite(mean.y) || !std::isfinite(mean.z);
        }
        summary << "inactive_nonfinite_mean_rows_preserved\t" << inactive << '\n';
        const auto fields = o1_fields(source, rows);
        const uint64_t state_bytes = std::accumulate(source.begin(), source.end(), uint64_t(0),
            [](uint64_t n, const Bytes& b) { return n + b.size(); });
        const uint64_t grad_bytes = rows * (14 + (uint64_t)engine().num_sh * 3) * 4;
        const uint64_t host_estimate = 5 * state_bytes + grad_bytes + rows * 32;
        need(host_estimate < 8ULL * 1024 * 1024 * 1024, "O1 host snapshot planning cap");
        summary << "source_identity\t" << packet_source << "\nconfig_identity\t" << packet_layout
            << "\nstart_step\t" << session.start_step << "\nrows\t1000000\nblocks\t3907\ntail_valid_rows\t64"
            << "\nstate_live_bytes\t" << state_bytes << "\ngradient_live_bytes\t" << grad_bytes
            << "\nhost_incremental_estimate\t" << host_estimate
            << "\ndevice_diagnostic_incremental\t0\nnative_lazy_render_scratch_incremental\tunknown"
            << "\noutput_estimate\tone_native_checkpoint_plus_summaries\noutput_cap_bytes\t10737418240"
            << "\nordered_raster\t1\nunknowns\tdriver_pinned_host_image_cache_true_peaks"
            << "\nrow_delta_definition\tpacked_storage_rows_or_valid_rows_dependent_on_changed_bounds_not_decoded_value_deltas"
            << "\nvisibility_definition\tprojection_candidates_not_exact_raster_contribution"
            << "\nsemantic_coverage\tdense_1M_rows_3907_blocks_not_observed_memory_bus_traffic"
            << "\nfrequency\tdiagnostic_readback_every_update_candidate_transfers_unestablished"
            << "\nfeasibility\tno_offload_no_safe_host_query_or_native_bounded_chunk_update_established\n";
        std::printf("O1 admission state_bytes=%llu gradient_bytes=%llu host_incremental_estimate=%llu device_diagnostic_incremental=0 lazy_render_scratch=unknown output_cap=10737418240\n",
            (unsigned long long)state_bytes, (unsigned long long)grad_bytes, (unsigned long long)host_estimate);
        o1_phases = &phases; source_phase_observer = o1_phase; pool_phase_observer = o1_pool_edge;
        o1_phase("source_setup");
        const std::array<int, 3> ids{1188,1165,760};
        for (int arm = 0; arm < 2; ++arm) {
            o1_arm = arm ? "carried" : "independent";
            if (arm) restore_source(source);
            for (int u = 0; u < 3; ++u) {
                o1_update = u;
                if (!arm) restore_source(source);
                const int step = session.start_step + (arm ? u : 0);
                const Fields before = source_state();
                o1_phase("restored_before_capture");
                const auto captured = source_capture(session, ids[u], step, false, true);
                const auto dirty = dirty_gradient_rows();
                std::vector<uint8_t> visible(rows, 0);
                const auto candidates = copy_device(engine().fwd.gaussian_ids.data_ptr(),
                    (size_t)engine().fwd.gaussian_ids.size() * sizeof(int32_t));
                for (size_t i = 0; i < candidates.size(); i += sizeof(int32_t)) {
                    int32_t id; std::memcpy(&id, candidates.data() + i, sizeof(id));
                    need(id >= 0 && (size_t)id < rows, "O1 candidate row"); visible[id] = 1;
                }
                auto emit_field = [&](size_t f, const Bytes& observed, const char* stage, size_t union_blocks = 0) {
                    const auto& d = fields[f];
                    const auto delta = o1_delta(before[f], observed, rows, d.stride, d.bound_stride, d.transposed_sh, d.cell_bytes);
                    size_t zero = 0, invisible = 0;
                    for (size_t r = 0; r < rows; ++r) if (delta.rows[r]) { zero += !dirty[r]; invisible += !visible[r]; }
                    uint64_t allocated = d.pool.empty() ? observed.size() : 0;
                    for (const auto& pool : DevicePool::global().breakdown()) if (pool.name == d.pool) allocated += pool.cap_bytes;
                    const bool backward = std::strcmp(stage, "native_backward_before_stat_restore") == 0;
                    const bool shared_capture = std::strcmp(stage, "native_capture_before_shared_restore") == 0;
                    table << o1_arm << '\t' << u << '\t' << ids[u] << '\t' << step << '\t'
                        << d.name << '\t' << d.shape << '\t' << d.format << "\tnative_GPU_or_shared_host_scalar\t"
                        << (backward ? "native_forward_backward_statistics" :
                            shared_capture ? "native_forward_backward_shared" :
                            f < 7 ? "forward_optimizer" : f < 20 ? "optimizer" : "forward_backward_frozen")
                        << '\t' << observed.size() << '\t' << allocated << '\t' << delta.bytes << '\t'
                        << std::count(delta.rows.begin(), delta.rows.end(), 1) << '\t'
                        << std::count(delta.blocks.begin(), delta.blocks.end(), 1) << '\t' << union_blocks
                        << '\t' << zero << '\t' << invisible << '\t' << delta.padding_bytes << '\t'
                        << (backward ? "native_backward_statistics_write_then_diagnostic_restore" :
                            shared_capture ? "native_forward_read_backward_accumulation_as_enabled_then_diagnostic_restore_no_shared_optimizer" : d.obligation)
                        << "\tdiagnostic_each_update_candidate_schedule_missing\t" << stage << '\n';
                };
                {
                    const Fields observed_statistics = statistics();
                    need(observed_statistics.size() == 4, "O1 backward statistics inventory");
                    for (size_t f = 0; f < observed_statistics.size(); ++f)
                        emit_field(20 + f, observed_statistics[f], "native_backward_before_stat_restore");
                }
                {
                    const Fields observed_shared = shared_state();
                    need(observed_shared.size() + 24 == before.size(), "O1 capture shared inventory");
                    for (size_t f = 0; f < observed_shared.size(); ++f)
                        emit_field(24 + f, observed_shared[f], "native_capture_before_shared_restore");
                }
                summary << o1_arm << "." << u << ".gradient_rows\t" << std::count(dirty.begin(), dirty.end(), 1)
                    << "\n" << o1_arm << "." << u << ".candidate_rows\t" << std::count(visible.begin(), visible.end(), 1)
                    << "\n" << o1_arm << "." << u << ".pixels\t" << (uint64_t)captured.faces * captured.width * captured.height << '\n';
                static const std::array<const char*, 6> gradient_names{
                    "means", "quats", "scales", "opacities", "features_dc", "features_sh"};
                for (size_t g = 0; g < captured.grad.size(); ++g) {
                    const auto& data = captured.grad[g];
                    need(data.size() % (rows * sizeof(float)) == 0, "O1 gradient shape");
                    const size_t stride = data.size() / rows;
                    size_t nonzero = 0;
                    for (size_t r = 0; r < rows; ++r) {
                        bool changed = false;
                        for (size_t j = 0; j < stride; j += sizeof(float)) {
                            float value; std::memcpy(&value, data.data() + r * stride + j, sizeof(value));
                            need(std::isfinite(value), "O1 nonfinite data gradient");
                            changed |= value != 0.f;
                        }
                        nonzero += changed;
                    }
                    summary << o1_arm << "." << u << ".gradient." << gradient_names[g]
                        << ".bytes\t" << data.size() << '\n' << o1_arm << "." << u
                        << ".gradient." << gradient_names[g] << ".rows\t" << nonzero << '\n';
                }
                const auto cfg = engine_resolve_managed_step_config(
                    spirula::build_step_config(session.cfg, session.st, step)).optim;
                need(cfg.use_fused_proj_bwd_optim, "O1 effective FPBO disabled");
                restore_source(before);
                o1_phase("before_native_internal_fpbo_update");
                engine_optim_step(step, cfg);
                o1_phase("after_native_internal_fpbo_update");
                const Fields after = source_state();
                if (cfg.use_per_splat_bias_correction) {
                    need(before[19].size() == rows * sizeof(int32_t), "O1 bias clock shape");
                    for (size_t r = 0; r < rows; ++r) {
                        int32_t a, b;
                        std::memcpy(&a, before[19].data() + r * sizeof(a), sizeof(a));
                        std::memcpy(&b, after[19].data() + r * sizeof(b), sizeof(b));
                        need(b == a + 1, "O1 native full-row clock increment");
                    }
                }
                // Same-input strict native internal/external oracle, not another
                // training update: restore and replay, then retain internal state.
                restore_source(before); load_gradient(captured.grad);
                o1_phase("before_native_external_oracle_replay");
                engine_fpbo_step_from_data_gradients(step, cfg);
                compare(after, source_state(), "O1 native internal/external canonical exact parity");
                o1_phase("after_native_external_oracle_replay");
                restore_source(after);
                for (size_t f = 0; f < fields.size(); ++f) {
                    const auto& d = fields[f];
                    size_t union_blocks = 0;
                    if (d.pair >= 0) {
                        const size_t p = (size_t)d.pair;
                        const auto codes = o1_delta(before[p], after[p], rows, fields[p].stride, 0, fields[p].transposed_sh, fields[p].cell_bytes);
                        const auto bounds = o1_delta(before[p + 1], after[p + 1], rows, 0, fields[p + 1].bound_stride);
                        for (size_t block = 0; block < codes.blocks.size(); ++block) union_blocks += codes.blocks[block] || bounds.blocks[block];
                    }
                    emit_field(f, after[f], "optimizer_update", union_blocks);
                    if (f >= 20) need(before[f] == after[f], "O1 frozen field changed");
                }
                need(table.good(), "O1 field summary write failed");
            }
        }
        o1_arm = "readback"; o1_update = 3;
        const Fields final = source_state();
        if (!final[19].empty()) {
            need(source[19].size() == final[19].size() && final[19].size() == rows * sizeof(int32_t),
                 "O1 final carried clock shape");
            int32_t low = INT32_MAX, high = INT32_MIN;
            int32_t initial_low = INT32_MAX, initial_high = INT32_MIN;
            for (size_t r = 0; r < rows; ++r) {
                int32_t clock, initial;
                std::memcpy(&clock, final[19].data() + r * sizeof(clock), sizeof(clock));
                std::memcpy(&initial, source[19].data() + r * sizeof(initial), sizeof(initial));
                need((int64_t)clock == (int64_t)initial + 3, "O1 final carried per-row clock increment");
                low = std::min(low, clock); high = std::max(high, clock);
                initial_low = std::min(initial_low, initial); initial_high = std::max(initial_high, initial);
            }
            summary << "initial_per_splat_clock_min\t" << initial_low
                << "\ninitial_per_splat_clock_max\t" << initial_high
                << "\nfinal_per_splat_clock_min\t" << low
                << "\nfinal_per_splat_clock_max\t" << high
                << "\ncarried_per_splat_clock_increment\t3"
                << "\nglobal_completed_boundary\t" << session.start_step + 3 << '\n';
        }
        engine_save_checkpoint((root / "final.native").string(), true, session.start_step + 3);
        compare(final, source_state(), "O1 save canonical bytes");
        o1_phase("after_checkpoint_save");
        const float poison = 12345.f;
        backend::memcpy_sync(engine().world.means.data_ptr(), &poison, sizeof(poison), backend::MemcpyKind::HostToDevice);
        need(engine_load_checkpoint((root / "final.native").string()) == session.start_step + 3, "O1 readback step");
        need(engine().cur_num_splats == (int64_t)rows && engine().max_num_splats == (int64_t)rows &&
             engine().world.features_sh_quant16_fpbo.initialized(), "O1 readback layout");
        compare(final, source_state(), "O1 poisoned native checkpoint exact readback");
        o1_phase("after_poison_reload_exact_readback");
        table.flush(); phases.flush(); summary.flush();
        uint64_t output_bytes = 0;
        for (const auto& file : fs::recursive_directory_iterator(root)) if (file.is_regular_file()) output_bytes += file.file_size();
        need(output_bytes + 4096 < 10ULL * 1024 * 1024 * 1024, "O1 output cap");
        summary << "updates\t6\noracle_replays\t6\nindependent_source_restores\t3\ncarried_source_restores\t1"
            << "\nfinal_boundary\t" << session.start_step + 3
            << "\nnative_checkpoint_readback_exact\tpass\ndirty_tail_check\tpass"
            << "\noutput_bytes_before_final_summary\t" << output_bytes << '\n';
        summary << "stale_alias_accounting_checks\t" << o1_stale_alias_checks
            << "\nstale_alias_check_status\t" << (o1_stale_alias_checks ? "pass" : "not_exercised_alias_disabled_or_private_fallback") << '\n';
        need(summary.good(), "O1 final summary write failed");
        o1_phases = nullptr; source_phase_observer = nullptr; pool_phase_observer = nullptr;
        source_close(session);
        std::printf("O1 screen logical_updates=6 oracle_replays=6 native_readback=exact_pass output=%s\n", root.string().c_str());
    } catch (...) {
        o1_phases = nullptr; source_phase_observer = nullptr; pool_phase_observer = nullptr;
        source_close(session); throw;
    }
}
bool source_float_field(size_t field) {
    return field < 5 || (field >= 6 && field <= 18 && field % 2 == 0);
}
size_t source_element_bytes(size_t field) {
    if (field == 5 || field == 17) return 2; // native SH value / joint moment codes
    return field < 20 ? 4 : 1;              // remaining frozen-state schema is byte-exact
}
void cleanup_failed_proof(const fs::path& root) {
    if (!fs::exists(root / "failure-evidence")) { fs::remove_all(root); return; }
    for (const char* name : {"image-in0", "image-in1", "image-out0", "image-out1",
                             "owner-in0", "owner-in1"})
        fs::remove(root / name);
}
float field_float(const Bytes& field, size_t element) {
    need((element + 1) * sizeof(float) <= field.size(), "diagnostic float bounds");
    float value;
    std::memcpy(&value, field.data() + element * sizeof(float), sizeof(value));
    return value;
}
void retain_source_failure(const Fields& old_state, const Fields& new_state,
                           const char* label, const Fields* before,
                           const Fields* resident_gradient, const Fields* received_gradient,
                           const OptimConfig* cfg) {
    need(!proof_root.empty(), "canonical failure has no artifact root");
    const fs::path root = proof_root / "failure-evidence";
    fs::create_directories(root);
    // Print before writing: even a disk/write failure must expose the retained location.
    std::fprintf(stderr, "G1 canonical_failure label=%s step=%llu version=%llu artifact_root=%s\n",
        label, (unsigned long long)proof_step, (unsigned long long)(proof_step + 1),
        root.string().c_str());
    std::fflush(stderr);
    uint64_t estimate = 65536;
    auto snapshot_size = [&](const Fields& fields) {
        estimate += sizeof(Header) + fields.size() * sizeof(uint64_t);
        for (const auto& field : fields) estimate += field.size();
    };
    snapshot_size(old_state); snapshot_size(new_state);
    if (before) snapshot_size(*before);
    if (resident_gradient) snapshot_size(*resident_gradient);
    if (received_gradient) snapshot_size(*received_gradient);
    for (size_t field = 0; field < old_state.size(); ++field) {
        const size_t width = source_element_bytes(field);
        need(old_state[field].size() % width == 0, "canonical diagnostic element layout");
        for (size_t offset = 0; offset < old_state[field].size(); offset += width)
            if (std::memcmp(old_state[field].data() + offset,
                            new_state[field].data() + offset, width) != 0)
                estimate += 192; // Full ledger, never sampled or truncated.
    }
    if (before && resident_gradient && received_gradient && old_state.size() > 10)
        for (size_t row = 0; row < (size_t)engine().cur_num_splats; ++row)
            if (std::memcmp(old_state[1].data() + row * 16,
                            new_state[1].data() + row * 16, 16) != 0)
                estimate += 2048; // Four complete quaternion component diagnostic rows.
    uint64_t existing_output = 0;
    for (const auto& entry : fs::recursive_directory_iterator(proof_root))
        if (entry.is_regular_file()) existing_output += entry.file_size();
    need(existing_output + estimate < 10ULL * 1024 * 1024 * 1024 &&
         fs::space(root).available > estimate, "full failure evidence exceeds bounded scratch");
    packet_first = 0; packet_rows = (uint64_t)engine().cur_num_splats;
    packet(root / "resident-native-state.packet", proof_step + 1, proof_step, 21, 2, old_state);
    packet(root / "received-native-state.packet", proof_step + 1, proof_step, 22, 2, new_state);
    if (before) packet(root / "preupdate-native-state-and-moments.packet",
                       proof_step + 1, proof_step, 23, 2, *before);
    if (resident_gradient) packet(root / "resident-data-gradients.packet",
                                  proof_step + 1, proof_step, 24, 2, *resident_gradient);
    if (received_gradient) packet(root / "received-data-gradients.packet",
                                  proof_step + 1, proof_step, 25, 2, *received_gradient);
    std::ofstream metadata(root / "identity.tsv");
    metadata << "label\t" << label << "\nsource\t" << packet_source
             << "\nconfig_layout\t" << packet_layout << "\nattempt\t" << packet_attempt
             << "\nstep\t" << proof_step << "\nversion\t" << proof_step + 1
             << "\nactive_stable_rows\t" << engine().cur_num_splats
             << "\ncapacity\t" << engine().max_num_splats << "\nsh_bases\t" << engine().num_sh
             << "\nfloat_atol\t0.0003\nfloat_rtol\t0.003\ncanonical_tolerance_bytes\t0"
             << "\nprojected_evidence_cap_bytes\t" << estimate << "\n";
    if (cfg) metadata << "lr_quats\t" << cfg->lr_quats
                      << "\nquat_norm_reg_weight\t" << cfg->quat_norm_reg_weight << "\n";
    metadata << "quaternion_diagnostic\traw_native_rows_gradients_and_moment_codes_only"
             << "\nnative_adam_epsilon\t1e-15\npacked_non_sh_code\tuint32:low16_u_high16_log_s"
             << "\npacked_sh_moment_code\tuint16:low8_u_high8_log_s"
             << "\nsh_storage_layout\tFPBO_word_paired_transposed_256_row_blocks"
             << "\ngradient_layout\tfull_capacity_row_major_float32_six_fields\n";
    const std::array<const char*, 20> names = {
        "means", "quats", "scales", "opacities", "features_dc", "SH_values_codes",
        "SH_values_bounds", "means_moment_codes", "means_moment_bounds",
        "quats_moment_codes", "quats_moment_bounds", "scales_moment_codes",
        "scales_moment_bounds", "opacity_moment_codes", "opacity_moment_bounds",
        "DC_moment_codes", "DC_moment_bounds", "SH_moment_codes", "SH_moment_bounds", "bias_steps"};
    for (size_t field = 0; field < old_state.size(); ++field)
        metadata << "field_" << field << '\t'
                 << (field < names.size() ? names[field] : "frozen_shared_or_statistic_bytes")
                 << "\tbytes=" << old_state[field].size()
                 << "\telement_bytes=" << source_element_bytes(field) << "\n";
    metadata << "preexisting_proof_output_bytes\t" << existing_output
             << "\nproof_live_output_cap_bytes\t10737418240\n";
    metadata.close();
    need((bool)metadata, "failure identity write failed");
    std::ofstream ledger(root / "all-canonical-element-mismatches.tsv");
    ledger << "field\telement\tbyte_offset\telement_bytes\ttype\told_bits\tnew_bits\told_value\tnew_value\n";
    for (size_t field = 0; field < old_state.size(); ++field) {
        const size_t width = source_element_bytes(field);
        for (size_t offset = 0; offset < old_state[field].size(); offset += width) {
            if (std::memcmp(old_state[field].data() + offset,
                            new_state[field].data() + offset, width) == 0) continue;
            uint32_t old_bits = 0, new_bits = 0;
            std::memcpy(&old_bits, old_state[field].data() + offset, width);
            std::memcpy(&new_bits, new_state[field].data() + offset, width);
            ledger << field << '\t' << offset / width << '\t' << offset << '\t' << width
                   << '\t' << (source_float_field(field) ? "float32" :
                               field == 19 ? "int32" : width == 1 ? "byte" : "packed_code")
                   << '\t' << old_bits << '\t' << new_bits << '\t';
            if (source_float_field(field)) {
                ledger.precision(9);
                ledger << field_float(old_state[field], offset / 4) << '\t'
                       << field_float(new_state[field], offset / 4);
            } else if (field == 19) ledger << (int32_t)old_bits << '\t' << (int32_t)new_bits;
            else ledger << old_bits << '\t' << new_bits;
            ledger << '\n';
        }
    }
    ledger.close();
    need((bool)ledger, "full canonical mismatch ledger write failed");
    // Raw native evidence only; do not replay tangent projection or the optimizer.
    if (before && resident_gradient && received_gradient && old_state.size() > 10) {
        std::ofstream quat(root / "changed-quaternion-row-raw-gradients-and-moments.tsv");
        quat.precision(9);
        quat << "row\tcomponent\tpreupdate_quat\tresident_raw\treceived_raw\tpreupdate_joint_moment_code\tbound_u_min\tbound_u_max\tbound_log_s_min\tbound_log_s_max\tbias\told_quat\tnew_quat\n";
        const size_t rows = (size_t)engine().cur_num_splats;
        for (size_t row = 0; row < rows; ++row) {
            if (std::memcmp(old_state[1].data() + row * 16,
                            new_state[1].data() + row * 16, 16) == 0) continue;
            int32_t bias;
            std::memcpy(&bias, (*before)[19].data() + row * sizeof(bias), sizeof(bias));
            for (size_t component = 0; component < 4; ++component) {
                const size_t cell = row * 4 + component;
                uint32_t code;
                std::memcpy(&code, (*before)[9].data() + cell * sizeof(code), sizeof(code));
                quat << row << '\t' << component << '\t' << field_float((*before)[1], cell)
                     << '\t' << field_float((*resident_gradient)[1], cell)
                     << '\t' << field_float((*received_gradient)[1], cell)
                     << '\t' << code;
                for (size_t i = 0; i < 4; ++i)
                    quat << '\t' << field_float((*before)[10], (row / 256) * 4 + i);
                quat << '\t' << bias << '\t' << field_float(old_state[1], cell)
                     << '\t' << field_float(new_state[1], cell) << '\n';
            }
        }
        quat.close();
        need((bool)quat, "focused quaternion diagnostic write failed");
    }
}
void report_source(const Fields& a, const Fields& b, const char* label,
                   const Fields* before = nullptr, const Fields* resident_gradient = nullptr,
                   const Fields* received_gradient = nullptr, const OptimConfig* cfg = nullptr) {
    need(a.size() == b.size(), "source state field count");
    bool equal = true;
    for (size_t field = 0; field < a.size(); ++field) {
        need(a[field].size() == b[field].size(), "source field bytes");
        uint64_t mismatched = 0;
        double worst = 0;
        for (size_t i = 0; i < a[field].size(); ++i)
            mismatched += a[field][i] != b[field][i];
        if (source_float_field(field) && a[field].size() % 4 == 0) {
            for (size_t i = 0; i < a[field].size(); i += 4) {
                float x, y;
                std::memcpy(&x, a[field].data() + i, 4);
                std::memcpy(&y, b[field].data() + i, 4);
                if (std::isfinite(x) && std::isfinite(y))
                    worst = std::max(worst, (double)std::abs(x - y));
            }
        }
        if (mismatched) {
            if (source_float_field(field))
                std::printf("G1 %s field=%zu differing_bytes=%llu max_finite_abs=%.9g acceptance=FAIL\n",
                    label, field, (unsigned long long)mismatched, worst);
            else std::printf("G1 %s field=%zu differing_bytes=%llu max_finite_abs=not_applicable acceptance=FAIL\n",
                    label, field, (unsigned long long)mismatched);
        }
        equal &= mismatched == 0;
    }
    if (equal) std::printf("G1 canonical label=%s differing_bytes=0\n", label);
    if (!equal) {
        std::fflush(stdout);
        retain_source_failure(a, b, label, before, resident_gradient, received_gradient, cfg);
    }
    need(equal, "native FPBO packed/value/bounds/bias/context acceptance failed");
}

uint64_t field_bytes(const Fields& fields, size_t begin, size_t end) {
    uint64_t bytes = 0;
    for (size_t i = begin; i < end; ++i) bytes += fields[i].size();
    return bytes;
}
// Replay never regenerates data gradients. The zero-cotangent backward only
// prepares the native forward/screen views required by the external seam.
void replay_context(spirula::TrainerSession& session, int step) {
    const auto sc = engine_resolve_managed_step_config(
        spirula::build_step_config(session.cfg, session.st, step));
    const int degree = std::min(session.cfg.sh_degree,
        step / std::max(session.cfg.sh_degree_warmup_every, 1));
    int passes = 0;
    const int views = engine_preview_forward(
        1188, session.cfg.primitive, degree, true, true, sc.loss, 0, &passes);
    need(passes == 1 && views > 0, "replay context requires one native face pass");
    const int h = engine().camera.height, w = engine().camera.width;
    std::vector<float> zero((size_t)views * h * w * 3, 0.f);
    engine_backward_from_render_grad(tv(zero.data(), 4, {views,h,w,3}),
        tv(zero.data(), 4, {views,h,w,1}), tv(zero.data(), 4, {views,h,w,1}));
}

bool replay_compare(const Fields& a, const Fields& b, const char* label,
                    bool byte_ledger, bool floats = false) {
    need(a.size() == b.size(), "replay comparison field count");
    uint64_t total = 0;
    for (size_t field = 0; field < a.size(); ++field) {
        need(a[field].size() == b[field].size(), "replay comparison field size");
        uint64_t differences = 0, nonfinite = 0;
        double maxabs = 0, squared = 0, reference = 0;
        for (size_t offset = 0; offset < a[field].size(); ++offset) {
            if (a[field][offset] == b[field][offset]) continue;
            ++differences;
            if (byte_ledger)
                std::printf("G1 replay_byte label=%s field=%zu offset=%zu old=%u new=%u\n",
                    label, field, offset, (unsigned)a[field][offset], (unsigned)b[field][offset]);
        }
        if (floats) {
            need(a[field].size() % sizeof(float) == 0, "producer float field size");
            for (size_t offset = 0; offset < a[field].size(); offset += sizeof(float)) {
                const float x = field_float(a[field], offset / 4);
                const float y = field_float(b[field], offset / 4);
                if (!std::isfinite(x) || !std::isfinite(y)) { ++nonfinite; continue; }
                const double delta = (double)x - y;
                maxabs = std::max(maxabs, std::abs(delta));
                squared += delta * delta;
                reference += (double)x * x;
            }
        }
        std::printf("G1 replay_compare label=%s field=%zu bytes=%llu differing_bytes=%llu max_finite_abs=%.9g relative_rms=%.9g nonfinite_pairs=%llu\n",
            label, field, (unsigned long long)a[field].size(),
            (unsigned long long)differences, maxabs,
            reference ? std::sqrt(squared / reference) :
                squared ? std::numeric_limits<double>::infinity() : 0.,
            (unsigned long long)nonfinite);
        total += differences;
    }
    std::printf("G1 replay_exact label=%s differing_bytes=%llu equal=%d\n",
        label, (unsigned long long)total, (int)(total == 0));
    return total == 0;
}

// Private observations retain the native loss/FPBO executor; no captured
// gradient is ever supplied to an optimizer.
struct BackwardObservation {
    EngineDiagnosticField field;
    std::vector<int64_t> shape;
};
using BackwardObservations = std::vector<BackwardObservation>;
constexpr size_t BackwardFieldCap = 180000000;
constexpr size_t BackwardStageCap = size_t(2) << 30;
std::string backward_sha(const Bytes& bytes) {
    spirula::Sha256 sha;
    sha.update(bytes.data(), bytes.size());
    return sha.hex();
}
void backward_field(BackwardObservations& fields, const std::string& name,
                    const void* ptr, size_t count, NpyScalar dtype, std::vector<int64_t> shape) {
    need(count <= BackwardFieldCap, "once/prime field exceeds source bound");
    fields.push_back({{name, uint8_t(dtype), false, copy_device(ptr, count)}, std::move(shape)});
}
template<class T>
void backward_tensor(BackwardObservations& fields, const std::string& name, const T& tensor) {
    const DeviceTensorFloatND view(tensor);
    std::vector<int64_t> shape;
    for (int d = 0; d < view.get_ndim(); ++d) shape.push_back(view.size(d));
    backward_field(fields, name, view.data_ptr(), view.data_ptr() ? size_t(view.numel()) * 4 : 0,
                   NpyScalar::f4, std::move(shape));
}
void backward_statistics(BackwardObservations& fields) {
    backward_tensor(fields, "statistics.radii", engine().optim.radii);
    backward_tensor(fields, "statistics.accum_buffer", engine().optim.accum_buffer);
    backward_tensor(fields, "statistics.densify_oversize", engine().optim.densify_oversize);
    backward_tensor(fields, "statistics.accum_weight", engine().fwd.accum_weight);
}
BackwardObservations backward_observe(const std::string& stage) {
    BackwardObservations fields;
    const auto& s = engine();
    need(s.camera.num == 1 && s.cur_num_splats == 1000000 &&
         s.max_num_splats == 1000000 && s.num_sh == 15, "once/prime source dimensions changed");
    const size_t pixels = size_t(s.camera.num) * s.camera.width * s.camera.height;
    need(pixels <= 12484800, "once/prime image exceeds admitted source pixels");
    if (stage == "forward") {
        backward_tensor(fields, "render.rgb", std::get<0>(s.fwd.renders));
        backward_tensor(fields, "render.depth", std::get<1>(s.fwd.renders));
        backward_tensor(fields, "render.Ts", s.fwd.render_Ts);
        backward_field(fields, "projection.camera_ids", s.fwd.camera_ids.data_ptr(),
            size_t(s.fwd.camera_ids.size()) * 4, NpyScalar::i4, {s.fwd.camera_ids.size()});
        backward_field(fields, "projection.gaussian_ids", s.fwd.gaussian_ids.data_ptr(),
            size_t(s.fwd.gaussian_ids.size()) * 4, NpyScalar::i4, {s.fwd.gaussian_ids.size()});
    } else if (stage == "backward" || stage == "prime") {
        for (const auto& entry : {std::pair{PoolSlot::EngVRgb, "cotangent.rgb"},
                                 std::pair{PoolSlot::EngVDepth, "cotangent.depth"},
                                 std::pair{PoolSlot::EngVTs, "cotangent.Ts"}}) {
            const int channels = entry.first == PoolSlot::EngVRgb ? 3 : 1;
            backward_field(fields, entry.second,
                DevicePool::global().acquire<float>(entry.first, pixels * channels),
                pixels * channels * 4, NpyScalar::f4,
                {s.camera.num, s.camera.height, s.camera.width, channels});
        }
        backward_field(fields, "loss.weighted_vector",
            DevicePool::global().acquire<float>(PoolSlot::PplTotalLosses, (int)LossIndex::length),
            sizeof(float) * (int)LossIndex::length, NpyScalar::f4, {(int)LossIndex::length});
        backward_field(fields, "loss.last_scale_ssim",
            DevicePool::global().acquire<float>(PoolSlot::SsimScalar, 1), 4, NpyScalar::f4, {1});
        need(s.fwd.v_splats_s.size() == 1, "once/prime requires one packed screen tensor");
        const auto& screen = s.fwd.v_splats_s.front();
        need(screen.get_ndim() == 2 && screen.size(1) == 10 &&
             screen.size(0) <= s.camera.num * s.cur_num_splats, "once/prime screen exceeds C*N");
        backward_tensor(fields, "raster.screen", screen);
    } else if (stage == "data_gradient") {
        backward_tensor(fields, "gradient.means", s.grad.means);
        backward_tensor(fields, "gradient.quats", s.grad.quats);
        backward_tensor(fields, "gradient.scales", s.grad.scales);
        backward_tensor(fields, "gradient.opacities", s.grad.opacities);
        backward_tensor(fields, "gradient.dc", s.grad.features_dc);
        backward_tensor(fields, "gradient.sh", s.grad.features_sh);
        const size_t expected[] = {12000000, 16000000, 12000000, 4000000, 12000000, 180000000};
        for (size_t i = 0; i < 6; ++i)
            need(fields[i].field.bytes.size() == expected[i], "once/prime full gradient descriptor mismatch");
    } else throw std::runtime_error("once/prime unknown stage");
    backward_statistics(fields);
    return fields;
}
BackwardObservations backward_canonical(const Bytes& stable_ids) {
    const auto& s = engine();
    Fields payload = source_state();
    std::vector<std::pair<std::string, NpyScalar>> names;
    auto add = [&](const char* name, NpyScalar dtype = NpyScalar::f4) { names.emplace_back(name, dtype); };
    for (const char* name : {"world.means", "world.quats", "world.scales", "world.opacities", "world.features_dc"}) add(name);
    add("eng.world.sh_vq16_fpbo.q", NpyScalar::u1); add("eng.world.sh_vq16_fpbo.qb");
    for (const char* name : {"means", "quats", "scales", "opacities", "features_dc", "sh"}) {
        const std::string prefix = std::string("eng.") + (std::string(name) == "sh" ? "sh_quant_fpbo" : std::string(name) + "_qfpbo");
        names.emplace_back(prefix + ".q", NpyScalar::u1); names.emplace_back(prefix + ".qb", NpyScalar::f4);
    }
    add("eng.bias_correction_steps", NpyScalar::i4);
    for (const char* name : {"eng.radii", "eng.accum_buffer", "eng.densify.oversize", "raster.accum_weight",
         "eng.ppisp.params", "eng.ppisp.g1", "eng.ppisp.g2", "eng.ppisp.accum",
         "eng.bg_sky.coefficients", "eng.bg_sky.g1", "eng.bg_sky.g2",
         "eng.color_shift_reg.ema", "eng.color_shift_reg.batch_sum"}) add(name);
    add("eng.color_shift_reg.steps", NpyScalar::i8);
    // Match shared_state() conditional order, including empty float holders.
    auto grid = [&](const auto& g, const std::string& prefix, bool depth) {
        if (!g.enabled) return;
        for (const char* name : {"grids", "g1", "g2", "accum"}) names.emplace_back(prefix + "." + name, NpyScalar::f4);
        if (depth) names.emplace_back(prefix + ".scalars", NpyScalar::f4);
        for (const auto& q : {std::pair{g.grids_quant.initialized(), ".value_quant"},
                              std::pair{g.quant_state.initialized(), ".optim_quant"},
                              std::pair{g.adagrad_quant.initialized(), ".adagrad_quant"}})
            if (q.first) {
                names.emplace_back(prefix + q.second + ".q", NpyScalar::u1);
                names.emplace_back(prefix + q.second + ".qb", NpyScalar::f4);
            }
    };
    grid(s.bilagrid_rgb, "eng.bg", false);
    grid(s.bilagrid_depth, "eng.bg_depth", true);
    grid(s.bilagrid_normal, "eng.bg_normal", false);
    need(payload.size() == 42 && names.size() == payload.size(), "once/prime canonical schema must be42 plus IDs");
    BackwardObservations result;
    for (size_t i = 0; i < payload.size(); ++i) {
        const auto scalar = npy_scalar_descr(names[i].second);
        need(payload[i].size() <= BackwardFieldCap && payload[i].size() % scalar.second == 0,
             "once/prime canonical source field bound");
        const int64_t count = int64_t(payload[i].size() / scalar.second);
        result.push_back({{names[i].first, uint8_t(names[i].second), false, std::move(payload[i])}, {count}});
    }
    result.push_back({{"stable_ids.implicit_source_row_order", uint8_t(NpyScalar::u4), false, stable_ids},
                      {s.cur_num_splats}});
    return result;
}
size_t backward_observation_bytes(const BackwardObservations& fields) {
    size_t total = 0;
    for (const auto& f : fields) total += f.field.bytes.size();
    return total;
}
void backward_identity(std::ostream& out, const std::string& label, const BackwardObservations& fields,
                       const std::vector<std::string>* digests = nullptr) {
    for (size_t i = 0; i < fields.size(); ++i) {
        const auto& f = fields[i];
        out << "{\"type\":\"once_field\",\"label\":" << std::quoted(label)
            << ",\"index\":" << i << ",\"name\":" << std::quoted(f.field.name)
            << ",\"dtype\":" << std::quoted(npy_scalar_descr(NpyScalar(f.field.dtype)).first)
            << ",\"bytes\":" << f.field.bytes.size() << ",\"shape\":[";
        for (size_t d = 0; d < f.shape.size(); ++d) out << (d ? "," : "") << f.shape[d];
        const std::string sha = digests ? digests->at(i).substr(digests->at(i).size() - 64) : backward_sha(f.field.bytes);
        out << "],\"sha256\":" << std::quoted(sha) << "}\n";
    }
}

// Private, fixed native PPISP witness; no optimizer replay or gradient injection.
constexpr uint64_t PpispMagic = 0x3150534950505353ULL;
constexpr uint64_t PpispOutputCap = 668000000;
#pragma pack(push, 1)
struct PpispPacket {
    uint64_t magic = PpispMagic, version = 1;
    uint32_t B = 1, H = 3060, W = 4080, N = 0, P = 0, indexed = 0;
    uint32_t actual_w = 0, actual_h = 0, step = 10000, input = 590, face = 590;
    uint32_t horizon = 30000, owner = 2, alias = 1, layout = 0, clamp = 0;
    char param_type[32]{}, source_sha[65]{}, config_sha[65]{}, joint_sha[65]{}, rgb_sha[65]{};
    char field_sha[5][65]{};
};
#pragma pack(pop)
static_assert(sizeof(PpispPacket) == 697, "private PPISP packet v1 header layout");
struct PpispInputs {
    PpispPacket header;
    Fields fields;
};
void ppisp_text(char* dst, size_t capacity, const std::string& text) {
    need(text.size() < capacity, "PPISP identity string exceeds bound");
    std::memcpy(dst, text.c_str(), text.size() + 1);
}
bool ppisp_digest_valid(const char* p) {
    for (int i = 0; i < 64; ++i)
        if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) return false;
    return p[64] == 0;
}
std::array<size_t,5> ppisp_lengths(const PpispPacket& h) {
    need(h.magic == PpispMagic && h.version == 1 && h.B == 1 && h.H == 3060 &&
         h.W == 4080 && h.N > 590 && h.N <= 65536 && h.P <= 39 && h.P >= 9 &&
         h.indexed == 1 && h.step == 10000 && h.input == 590 && h.face == 590 &&
         h.horizon == 30000 && h.owner == 2 && h.alias == 1,
         "PPISP packet native witness/dimensions invalid");
    need(std::memchr(h.param_type,0,sizeof(h.param_type)), "PPISP type is not terminated");
    const auto spec = ppisp_param_spec(h.param_type);
    need(spec.num_params == int(h.P) && uint32_t(spec.layout) == h.layout &&
         uint32_t(spec.clamp_output) == h.clamp, "PPISP packet layout/clamp invalid");
    float aw,ah; std::memcpy(&aw,&h.actual_w,4); std::memcpy(&ah,&h.actual_h,4);
    need(aw == 4080.f && ah == 3060.f && ppisp_digest_valid(h.source_sha) &&
         ppisp_digest_valid(h.config_sha), "PPISP packet source/actual dimensions invalid");
    const std::array<size_t,5> sizes{size_t(h.B)*h.H*h.W*12,size_t(h.N)*h.P*4,
        size_t(h.B)*16,size_t(h.B)*4,size_t(h.B)*h.H*h.W*12};
    uint64_t projected=sizeof(h)+2*sizes[0]+(uint64_t(64)<<20);
    for(auto bytes:sizes) projected+=bytes;
    need(projected<=PpispOutputCap,"PPISP packet/full failure pair/log declaration exceeds cap");
    return sizes;
}
void ppisp_raw_write(const fs::path& path, const void* ptr, size_t bytes) {
    need(!fs::exists(path), "PPISP output refuses replacement");
    std::ofstream out(path,std::ios::binary);
    out.write((const char*)ptr,std::streamsize(bytes));
    out.close(); need(bool(out), "PPISP output write failed");
}
void ppisp_save_packet(const fs::path& root, const PpispInputs& packet) {
    const auto lengths = ppisp_lengths(packet.header);
    std::ofstream out(root/"ppisp-input.packet",std::ios::binary);
    out.write((const char*)&packet.header,sizeof(packet.header));
    for (size_t i=0;i<5;++i) {
        need(packet.fields[i].size()==lengths[i], "PPISP write length invalid");
        out.write((const char*)packet.fields[i].data(),std::streamsize(lengths[i]));
    }
    out.close(); need(bool(out),"PPISP packet write failed");
}
PpispInputs ppisp_load_packet(const fs::path& path) {
    need(fs::file_size(path) < 320000000,"PPISP packet exceeds bound");
    PpispInputs p;
    std::ifstream in(path,std::ios::binary);
    in.read((char*)&p.header,sizeof(p.header)); need(bool(in),"PPISP header truncated");
    const auto sizes=ppisp_lengths(p.header);
    uint64_t total=sizeof(p.header); for(auto n:sizes) total+=n;
    need(fs::file_size(path)==total,"PPISP exact packet size/EOF invalid");
    need(ppisp_digest_valid(p.header.joint_sha) && ppisp_digest_valid(p.header.rgb_sha),
         "PPISP captured output witnesses invalid");
    for(size_t i=0;i<5;++i) {
        need(ppisp_digest_valid(p.header.field_sha[i]),"PPISP field SHA invalid");
        p.fields.emplace_back(sizes[i]);
        in.read((char*)p.fields.back().data(),std::streamsize(sizes[i]));
        need(bool(in) && backward_sha(p.fields.back())==p.header.field_sha[i],"PPISP field SHA/truncation invalid");
    }
    need(in.peek()==std::char_traits<char>::eof(),"PPISP trailing data");
    int32_t camera; std::memcpy(&camera,p.fields[3].data(),4);
    need(camera==590,"PPISP actual captured camera witness invalid");
    return p;
}
bool ppisp_operator_proof(PpispInputs& packet, const fs::path& root, std::ostream& log,
                          bool replay) {
    const auto& h=packet.header;
    const auto sizes=ppisp_lengths(h);
    need(packet.fields.size()==5,"PPISP field count invalid");
    std::vector<void*> allocations;
    struct Release { std::vector<void*>& p; ~Release(){for(auto* v:p) backend::device_free(v);} } release{allocations};
    auto allocate=[&](size_t n) {
        void* p=backend::device_malloc(n); need(p,"PPISP private allocation failed");
        allocations.push_back(p); return p;
    };
    std::array<void*,5> input{};
    for(size_t i=0;i<5;++i) {
        need(packet.fields[i].size()==sizes[i],"PPISP operator input length invalid");
        input[i]=allocate(sizes[i]); upload(input[i],packet.fields[i]);
    }
    void* rgb=allocate(sizes[4]); upload(rgb,packet.fields[4]);
    void* grad=allocate(sizes[1]); backend::memset_sync(grad,0,sizes[1]);
    auto image=[&](void* p) {return DeviceTensor3D<float3>(tv(p,4,{h.B,h.H,h.W,3}));};
    float aw,ah; std::memcpy(&aw,&h.actual_w,4); std::memcpy(&ah,&h.actual_h,4);
    auto launch=[&](void* cot,bool joint) {
        auto start=Clock::now();
        ppisp_backward(image(input[0]),tv(input[1],4,{h.N,h.P}),tv(input[2],4,{h.B,4}),
            aw,ah,image(cot),h.param_type,tv(input[3],4,{h.B}),image(cot),
            joint?tv(grad,4,{h.N,h.P}):nil());
        backend::device_synchronize();
        if(const char* error=backend::last_error()) throw std::runtime_error(error);
        return std::chrono::duration<double>(Clock::now()-start).count();
    };
    const double joint_seconds=launch(input[4],true), rgb_seconds=launch(rgb,false);
    const Bytes a=copy_device(input[4],sizes[4]), b=copy_device(rgb,sizes[4]);
    const auto joint_sha=backward_sha(a),rgb_sha=backward_sha(b);
    size_t different=0, nonfinite=0;
    for(size_t i=0;i<a.size();i+=4) {
        different+=std::memcmp(a.data()+i,b.data()+i,4)!=0;
        float x,y; std::memcpy(&x,a.data()+i,4);std::memcpy(&y,b.data()+i,4);
        nonfinite+=!std::isfinite(x)||!std::isfinite(y);
    }
    bool unchanged=true;
    for(size_t i=0;i<4;++i) unchanged &= copy_device(input[i],sizes[i])==packet.fields[i];
    const bool witness=!replay||(joint_sha==h.joint_sha&&rgb_sha==h.rgb_sha);
    if(!replay) {
        ppisp_text(packet.header.joint_sha,sizeof(h.joint_sha),joint_sha);
        ppisp_text(packet.header.rgb_sha,sizeof(h.rgb_sha),rgb_sha);
    }
    log << "{\"type\":\"ppisp_operator\",\"joint_sha256\":"<<std::quoted(joint_sha)
        <<",\"rgb_sha256\":"<<std::quoted(rgb_sha)<<",\"float32_elements\":"<<a.size()/4
        <<",\"different_bits_elements\":"<<different<<",\"nonfinite_pairs\":"<<nonfinite
        <<",\"unchanged_full_inputs\":"<<(unchanged?"true":"false")
        <<",\"capture_output_hash_match\":"<<(witness?"true":"false")
        <<",\"joint_local_seconds\":"<<joint_seconds<<",\"rgb_local_seconds\":"<<rgb_seconds
        <<",\"operator_producer_version\":2,\"image_specialization_axes\":3"
        <<",\"parameter_gradient_selection\":\"uniform_runtime_null_pointer\""
        <<",\"priced\":false,\"baseline\":\"same_new_module_canonical_nonnull_branch_not_old_executable\"}\n";
    if(different||!witness) {
        ppisp_raw_write(root/"joint-rgb.full.bin",a.data(),a.size());
        ppisp_raw_write(root/"nullable-rgb.full.bin",b.data(),b.size());
    }
    return different==0&&unchanged&&witness;
}
void ppisp_price(int argc, char** argv) {
    const auto outer_start = Clock::now();
    need(argc==8 && std::strcmp(argv[2],"--packet")==0 &&
         std::strcmp(argv[4],"--device")==0 && std::strcmp(argv[6],"--output")==0,
         "PPISP price arguments: ppisp-price --packet PACKET --device UUID --output NEW_DIRECTORY");
    const std::string device=argv[5];
    need(device=="uuid:00000000010000000000000000000000" ||
         device=="uuid:00000000020000000000000000000000", "PPISP price requires admitted AMD UUID");
    const fs::path root=argv[7];
    need(fs::create_directory(root),"PPISP price output must be fresh");
    std::ofstream log(root/"ppisp-price.jsonl",std::ios::binary);
    need(bool(log),"PPISP price log open failed");
    log<<std::setprecision(17);
    auto elapsed=[](Clock::time_point start) {
        const double s=std::chrono::duration<double>(Clock::now()-start).count();
        need(std::isfinite(s)&&s>0,"PPISP price nonpositive/nonfinite duration"); return s;
    };
    auto checked_sync=[]() {
        backend::device_synchronize();
        if(const char* error=backend::last_error()) throw std::runtime_error(error);
    };
    try {
        log<<"{\"type\":\"ppisp_price_scope\",\"producer\":\"ppisp-price-v1\",\"device\":"<<std::quoted(device)
           <<",\"packet_path\":"<<std::quoted(fs::path(argv[3]).string())
           <<",\"warm_pairs\":8,\"measured_pairs\":32,\"image_specialization_axes\":3"
           <<",\"operator_producer_version\":2,\"baseline\":\"same_v2_module_nonnull_branch_same_binary\""
           <<",\"parameter_gradient_selection\":\"uniform_runtime_null_pointer\""
           <<",\"clock\":\"steady_clock_launcher_plus_GPU_sync_not_GPU_timestamp\""
           <<",\"end_to_end_speed_claim\":false,\"DCE_verified\":false"
           <<",\"parent_qualification_required\":true,\"Q_decoded_1e_4\":\"unchanged_FAILED\"}\n";
        log.flush(); need(bool(log),"PPISP price scope write failed");
        const auto parse_start=Clock::now();
        const std::string packet_sha=spirula::sha256_file(argv[3]);
        need(packet_sha.size()==64,"PPISP price packet SHA unavailable");
        auto packet=ppisp_load_packet(argv[3]);
        const double parse_seconds=elapsed(parse_start);
        const auto& h=packet.header;
        need(h.N==1195 && h.P==9,"PPISP price requires qualified actual-source N1195 P9");
        const auto sizes=ppisp_lengths(h);
        log<<"{\"type\":\"ppisp_price_input\",\"packet_bytes\":"<<fs::file_size(argv[3])
           <<",\"packet_sha256\":"<<std::quoted(packet_sha)
           <<",\"header_bytes\":"<<sizeof(h)<<",\"B\":"<<h.B<<",\"H\":"<<h.H<<",\"W\":"<<h.W
           <<",\"N\":"<<h.N<<",\"P\":"<<h.P<<",\"indexed\":"<<h.indexed
           <<",\"actual_w_bits\":"<<h.actual_w<<",\"actual_h_bits\":"<<h.actual_h
           <<",\"step\":"<<h.step<<",\"input\":"<<h.input<<",\"face\":"<<h.face
           <<",\"horizon\":"<<h.horizon<<",\"owner\":"<<h.owner<<",\"alias\":"<<h.alias
           <<",\"layout\":"<<h.layout<<",\"clamp\":"<<h.clamp<<",\"param_type\":"<<std::quoted(h.param_type)
           <<",\"source_sha256\":"<<std::quoted(h.source_sha)<<",\"config_sha256\":"<<std::quoted(h.config_sha)
           <<",\"capture_joint_sha256\":"<<std::quoted(h.joint_sha)
           <<",\"capture_rgb_sha256\":"<<std::quoted(h.rgb_sha)<<"}\n";
        std::vector<void*> allocations; allocations.reserve(8);
        struct Release { std::vector<void*>& p; ~Release(){for(auto* v:p) backend::device_free(v);} } release{allocations};
        auto allocate=[&](size_t n) {
            void* p=backend::device_malloc(n); need(p,"PPISP price allocation failed");
            allocations.push_back(p); return p;
        };
        const auto select_start=Clock::now();
        need(backend::device_select_identity(device.c_str()),"PPISP price device unavailable");
        checked_sync(); const double select_seconds=elapsed(select_start);
        const auto alloc_start=Clock::now();
        std::array<void*,5> input{};
        for(size_t i=0;i<5;++i) input[i]=allocate(sizes[i]);
        void* joint=allocate(sizes[4]); void* rgb=allocate(sizes[4]); void* grad=allocate(sizes[1]);
        const double allocation_seconds=elapsed(alloc_start);
        const auto upload_start=Clock::now();
        for(size_t i=0;i<5;++i) upload(input[i],packet.fields[i]);
        checked_sync(); const double upload_seconds=elapsed(upload_start);
        Bytes scratch(sizes[4]);
        auto inputs_check=[&](const char* phase) {
            bool ok=true;
            for(size_t i=0;i<5;++i) {
                backend::memcpy_sync(scratch.data(),input[i],sizes[i],backend::MemcpyKind::DeviceToHost);
                checked_sync();
                const bool equal=std::memcmp(scratch.data(),packet.fields[i].data(),sizes[i])==0;
                spirula::Sha256 sha; sha.update(scratch.data(),sizes[i]);
                const std::string digest=sha.hex();
                ok &= equal && digest==h.field_sha[i];
                log<<"{\"type\":\"ppisp_price_field\",\"phase\":"<<std::quoted(phase)
                   <<",\"index\":"<<i<<",\"dtype\":"<<std::quoted(i==3?"int32":"float32")
                   <<",\"shape\":[";
                if(i==0 || i==4) log<<h.B<<","<<h.H<<","<<h.W<<",3";
                else if(i==1) log<<h.N<<","<<h.P;
                else if(i==2) log<<h.B<<",4";
                else log<<h.B;
                log<<"],\"bytes\":"<<sizes[i]<<",\"sha256\":"<<std::quoted(digest)
                   <<",\"capture_sha256\":"<<std::quoted(h.field_sha[i])<<",\"exact\":"<<(equal?"true":"false")<<"}\n";
            }
            return ok;
        };
        auto image=[&](void* p){return DeviceTensor3D<float3>(tv(p,4,{h.B,h.H,h.W,3}));};
        float aw,ah; std::memcpy(&aw,&h.actual_w,4); std::memcpy(&ah,&h.actual_h,4);
        auto launch=[&](bool is_joint,double& reset_seconds) {
            void* cot=is_joint?joint:rgb;
            const auto reset_start=Clock::now();
            backend::memcpy_sync(cot,input[4],sizes[4],backend::MemcpyKind::DeviceToDevice);
            if(is_joint) backend::memset_sync(grad,0,sizes[1]);
            checked_sync(); reset_seconds=elapsed(reset_start);
            const auto start=Clock::now();
            ppisp_backward(image(input[0]),tv(input[1],4,{h.N,h.P}),tv(input[2],4,{h.B,4}),
                aw,ah,image(cot),h.param_type,tv(input[3],4,{h.B}),image(cot),
                is_joint?tv(grad,4,{h.N,h.P}):nil());
            checked_sync(); return elapsed(start);
        };
        const auto before_start=Clock::now();
        const bool before=inputs_check("before");
        const double before_seconds=elapsed(before_start);
        std::array<double,32> joints{},rgbs{},ratios{};
        const auto warm_start=Clock::now();
        double warm_seconds=0;
        for(int pair=0;before && pair<40;++pair) {
            if(pair==8) warm_seconds=elapsed(warm_start);
            const bool joint_first=(pair%2)==0;
            double j=0,r=0,jreset=0,rreset=0;
            if(joint_first) { j=launch(true,jreset); r=launch(false,rreset); }
            else { r=launch(false,rreset); j=launch(true,jreset); }
            const double ratio=j/r; need(std::isfinite(ratio)&&ratio>0,"PPISP price invalid paired ratio");
            if(pair>=8) { joints[pair-8]=j; rgbs[pair-8]=r; ratios[pair-8]=ratio; }
            log<<"{\"type\":"<<std::quoted(pair<8?"ppisp_price_warm":"ppisp_price_sample")
               <<",\"pair\":"<<(pair<8?pair:pair-8)<<",\"order\":"<<std::quoted(joint_first?"joint_rgb":"rgb_joint")
               <<",\"joint_seconds\":"<<j<<",\"rgb_seconds\":"<<r<<",\"joint_over_rgb\":"<<ratio
               <<",\"joint_reset_seconds\":"<<jreset<<",\"rgb_reset_seconds\":"<<rreset<<"}\n";
        }
        const auto validation_start=Clock::now();
        double reset=0; launch(true,reset);
        const bool after_joint=inputs_check("after_final_joint");
        const Bytes a=copy_device(joint,sizes[4]); checked_sync();
        launch(false,reset);
        const bool after_rgb=inputs_check("after_final_rgb");
        const Bytes b=copy_device(rgb,sizes[4]); checked_sync();
        const auto joint_sha=backward_sha(a),rgb_sha=backward_sha(b);
        size_t different=0,nonfinite=0;
        for(size_t i=0;i<a.size();i+=4) {
            different+=std::memcmp(a.data()+i,b.data()+i,4)!=0;
            float x,y; std::memcpy(&x,a.data()+i,4); std::memcpy(&y,b.data()+i,4);
            nonfinite+=!std::isfinite(x)||!std::isfinite(y);
        }
        const bool witness=joint_sha==h.joint_sha && rgb_sha==h.rgb_sha;
        const std::string packet_after_sha=spirula::sha256_file(argv[3]);
        const bool packet_unchanged=packet_after_sha==packet_sha;
        const bool exact=different==0 && nonfinite==0 && witness && before && after_joint && after_rgb && packet_unchanged;
        log<<"{\"type\":\"ppisp_price_exact\",\"joint_sha256\":"<<std::quoted(joint_sha)
           <<",\"rgb_sha256\":"<<std::quoted(rgb_sha)<<",\"different_bits_elements\":"<<different
           <<",\"nonfinite_pairs\":"<<nonfinite<<",\"capture_output_hash_match\":"<<(witness?"true":"false")
           <<",\"unchanged_full_inputs\":"<<((before&&after_joint&&after_rgb)?"true":"false")
           <<",\"packet_after_sha256\":"<<std::quoted(packet_after_sha)
           <<",\"packet_unchanged\":"<<(packet_unchanged?"true":"false")
           <<",\"full_rgb_bytes_each\":"<<a.size()<<",\"exact\":"<<(exact?"true":"false")<<"}\n";
        if(!exact) {
            log<<"{\"type\":\"ppisp_price_result\",\"accepted\":false,\"parent_qualified\":false}\n";
            log.flush();
            ppisp_raw_write(root/"joint-rgb.full.bin",a.data(),a.size());
            ppisp_raw_write(root/"nullable-rgb.full.bin",b.data(),b.size());
            throw std::runtime_error("PPISP price full witness/input/output failure");
        }
        const double validation_seconds=elapsed(validation_start);
        const auto free_start=Clock::now();
        for(auto* p:allocations) backend::device_free(p);
        allocations.clear();
        const double device_free_seconds=elapsed(free_start);
        auto median=[](auto values) {
            std::sort(values.begin(),values.end());
            return (values[values.size()/2-1]+values[values.size()/2])/2;
        };
        auto aggregate=[&](const char* order,int parity) {
            std::array<double,16> sj{},sr{},sq{};
            for(size_t i=0;i<16;++i) { sj[i]=joints[2*i+parity]; sr[i]=rgbs[2*i+parity]; sq[i]=ratios[2*i+parity]; }
            log<<"{\"type\":\"ppisp_price_order_aggregate\",\"order\":"<<std::quoted(order)
               <<",\"pairs\":16,\"joint_median_seconds\":"<<median(sj)<<",\"rgb_median_seconds\":"<<median(sr)
               <<",\"paired_ratio_median\":"<<median(sq)<<"}\n";
        };
        aggregate("joint_rgb",0); aggregate("rgb_joint",1);
        log<<"{\"type\":\"ppisp_price_aggregate\",\"pairs\":32,\"joint_median_seconds\":"<<median(joints)
           <<",\"rgb_median_seconds\":"<<median(rgbs)<<",\"paired_ratio_median\":"<<median(ratios)
           <<",\"parse_cpu_seconds\":"<<parse_seconds<<",\"device_select_seconds\":"<<select_seconds
           <<",\"allocation_cpu_seconds\":"<<allocation_seconds<<",\"upload_sync_seconds\":"<<upload_seconds
           <<",\"before_input_readback_hash_seconds\":"<<before_seconds
           <<",\"cold_pipeline_in_first_warm_pair\":true,\"warm_outer_seconds\":"<<warm_seconds
           <<",\"final_validation_outer_seconds\":"<<validation_seconds
           <<",\"device_free_cpu_seconds\":"<<device_free_seconds
           <<",\"producer_outer_elapsed_seconds\":"<<elapsed(outer_start)
           <<",\"process_outer_requires_parent_monitor\":true,\"statistical_significance_claim\":false}\n";
        log<<"{\"type\":\"ppisp_price_result\",\"accepted\":true,\"parent_qualified\":false}\n";
        log.close(); need(bool(log),"PPISP price log write failed");
    } catch(const std::exception& error) {
        if(log.is_open()) {
            log<<"{\"type\":\"ppisp_price_failure\",\"accepted\":false,\"parent_qualified\":false,\"error\":"
               <<std::quoted(error.what())<<"}\n";
            log.close();
        }
        throw;
    }
}

void ppisp_private_proof(int argc,char** argv,bool replay) {
    need(argc==8&&std::strcmp(argv[2],replay?"--packet":"--resume")==0&&
         std::strcmp(argv[4],"--device")==0&&std::strcmp(argv[6],"--output")==0,
         "PPISP private proof arguments");
    need(std::string(argv[5]).rfind("uuid:",0)==0&&backend::device_select_identity(argv[5]),
         "PPISP explicit device UUID unavailable");
    const fs::path root=argv[7]; need(fs::create_directory(root),"PPISP output must be fresh");
    std::ofstream log(root/"ppisp-proof.jsonl",std::ios::binary); need(bool(log),"PPISP log open failed");
    log<<std::setprecision(17);
    log<<"{\"type\":\"ppisp_scope\",\"output_cap_bytes\":"<<PpispOutputCap
       <<",\"device\":"<<std::quoted(argv[5])<<",\"replay\":"<<(replay?"true":"false")
       <<",\"gpu_serializer_claim\":false,\"DCE_verified\":false,\"Q_decoded_1e_4\":\"unchanged_FAILED\"}\n";
    if(replay) {
        auto packet=ppisp_load_packet(argv[3]);
        const bool ok=ppisp_operator_proof(packet,root,log,true);
        log.close();need(bool(log),"PPISP replay log write failed"); need(ok,"PPISP exact replay proof failed"); return;
    }
    need(spirula::env("B1_OWNER")&&std::string(spirula::env("B1_OWNER"))=="all"&&
         spirula::env("B1_VIEWS")&&!ckpt::read_b1_contract(argv[3])&&
         !spirula::env("NO_TILE_SKIP")&&!spirula::env("B1_REFRESH_CADENCE")&&
         !std::getenv("SS_B1_VIEW_POLICY")&&!std::getenv("SSPLAT_B1_VIEW_POLICY"),
         "PPISP capture requires unchanged original all-owned source environment");
    spirula::TrainerSession session;
    struct Cleanup {
        ~Cleanup(){engine_diagnostic_ppisp_probe({});}
    } cleanup;
    TrainConfig cli; cli.resume=argv[3];cli.cache_images="disk";
    session.cfg=ckpt::build_resume_config(cli,"",{"cache_images"});
    session.write_config_json=false;session.diagnostic_sync_sampler=true;
    session.out_dir_override=(root/"session").string();
    try {
        session.check_config();session.load_dataset();session.setup_engine();
        need(session.start_step==10000&&session.cfg.num_iterations==30000&&engine().b1.owner==2&&
             engine().cur_num_splats==1000000&&engine().max_num_splats==1000000&&
             engine().ppisp.enabled&&engine().b1.contract&&
             session.b1_view_policy=="full"&&engine().b1.contract->view_policy=="full"&&
             session.ds.train_indices==session.b1_full_train_indices,
             "PPISP original native source contract invalid");
        const auto before=engine_diagnostic_capture(false);
        for(auto mask:before.owned_blocks) need(mask==1,"PPISP canonical all-owned mask has holes");
        for(const auto& f:before.fields) {
            const auto dtype=npy_scalar_descr(NpyScalar(f.dtype));
            log<<"{\"type\":\"ppisp_source_slot\",\"phase\":\"before\",\"name\":"<<std::quoted(f.name)
               <<",\"dtype\":"<<std::quoted(dtype.first)<<",\"flat_storage_shape\":["
               <<f.bytes.size()/dtype.second<<"],\"bytes\":"<<f.bytes.size()<<",\"sha256\":"
               <<std::quoted(backward_sha(f.bytes))<<",\"serialized\":"<<(f.serialized?"true":"false")<<"}\n";
        }
        std::vector<uint32_t> stable(1000000);std::iota(stable.begin(),stable.end(),0);
        Bytes ids((const uint8_t*)stable.data(),(const uint8_t*)stable.data()+stable.size()*4);
        std::vector<EngineDiagnosticField> frozen_before;
        {
            auto canonical=backward_canonical(ids);
            backward_identity(log,"source-before-full-canonical",canonical);
            // source_state(): fields0..19 owned world/optimizer,20..23 raster
            // statistics;24..41 are the complete shared_state() inventory.
            for(size_t i=24;i<canonical.size()-1;++i)
                frozen_before.push_back(std::move(canonical[i].field));
            need(frozen_before.size()==18,"PPISP complete frozen shared inventory changed");
        }
        log<<"{\"type\":\"ppisp_source\",\"source_state_sha256\":"
           <<std::quoted(engine().b1.contract->source_state_sha256)<<",\"config_sha256\":"
           <<std::quoted(ckpt::b1_config_hash(session.cfg))<<",\"checkpoint\":"
           <<std::quoted(fs::absolute(argv[3]).string())<<",\"CPU_source28_provenance\":\"parent_independent_original_TAR_verifier_required_not_GPU_serializer\",\"host_state\":"
           <<std::quoted(before.host_signature)<<"}\n";
        PpispInputs packet; int captures=0; bool exact=false;
        engine_diagnostic_ppisp_probe([&](uint64_t image,uint64_t params,uint64_t intr,
            uint64_t cams,uint64_t cot,int B,int H,int W,int N,int P,float aw,float ah,std::string_view type) {
            need(++captures==1,"PPISP native capture must occur exactly once");
            const auto* actual=engine_diagnostic_ppisp_input();
            need(actual&&actual->subs.size()==1&&actual->subs[0],"PPISP actual native input witness absent");
            const auto& batch=*actual->subs[0];
            need(batch.indices==std::vector<int32_t>{590}&&batch.post_offsets==std::vector<int32_t>{590}&&
                 batch.K==1&&batch.num==1&&batch.width==4080&&batch.height==3060,
                 "PPISP actual next_train_step input/face/dimensions differ");
            auto& h=packet.header; h.B=B;h.H=H;h.W=W;h.N=N;h.P=P;h.indexed=cams?1:0;
            std::memcpy(&h.actual_w,&aw,4);std::memcpy(&h.actual_h,&ah,4);
            ppisp_text(h.param_type,sizeof(h.param_type),std::string(type));
            ppisp_text(h.source_sha,sizeof(h.source_sha),engine().b1.contract->source_state_sha256);
            ppisp_text(h.config_sha,sizeof(h.config_sha),ckpt::b1_config_hash(session.cfg));
            const auto spec=ppisp_param_spec(std::string(type)); h.layout=uint32_t(spec.layout);h.clamp=spec.clamp_output;
            const auto lengths=ppisp_lengths(h);
            const uint64_t pointers[5]={image,params,intr,cams,cot};
            for(size_t i=0;i<5;++i) {
                packet.fields.push_back(copy_device((const void*)pointers[i],lengths[i]));
                ppisp_text(h.field_sha[i],65,backward_sha(packet.fields.back()));
            }
            int32_t camera;std::memcpy(&camera,packet.fields[3].data(),4);
            need(camera==590,"PPISP first actual input/face is not590");
            log<<"{\"type\":\"ppisp_actual_input\",\"input\":590,\"face\":590,\"B\":"<<B
               <<",\"H\":"<<H<<",\"W\":"<<W<<",\"N\":"<<N<<",\"P\":"<<P
               <<",\"actual_width_float32_bits\":"<<h.actual_w<<",\"actual_height_float32_bits\":"<<h.actual_h
               <<",\"param_type\":"<<std::quoted(h.param_type)<<",\"layout\":"<<h.layout
               <<",\"clamp\":"<<h.clamp<<",\"cotangent_inplace_alias\":true,\"step\":10000,\"horizon\":30000}\n";
            exact=ppisp_operator_proof(packet,root,log,false);
            for(size_t i=0;i<5;++i)
                need(copy_device((const void*)pointers[i],lengths[i])==packet.fields[i],
                     "PPISP probe mutated original borrowed engine inputs");
            ppisp_save_packet(root,packet);
            const char* names[5]={"pre_rgb","params_full","intrinsics","camera_indices","incoming_cotangent"};
            uint64_t offset=sizeof(h);
            for(size_t i=0;i<5;++i) {
                log<<"{\"type\":\"ppisp_packet_field\",\"name\":"<<std::quoted(names[i])
                   <<",\"dtype\":"<<std::quoted(i==3?"<i4":"<f4")<<",\"bytes\":"<<lengths[i]
                   <<",\"offset\":"<<offset<<",\"shape\":[";
                if(i==0||i==4) log<<B<<","<<H<<","<<W<<",3";
                else if(i==1) log<<N<<","<<P;
                else if(i==2) log<<B<<",4"; else log<<B;
                log<<"],\"sha256\":"<<std::quoted(h.field_sha[i])<<"}\n";offset+=lengths[i];
            }
        });
        (void)session.train_step(10000); // Genuine ordinary managed backward+optimizer, once.
        engine_diagnostic_ppisp_probe({});
        backend::device_synchronize();if(const char* e=backend::last_error())throw std::runtime_error(e);
        need(captures==1,"PPISP ordinary step did not capture exactly once");
        engine_b1_verify_context();
        const auto after=engine_diagnostic_capture(false);
        for(const auto& f:after.fields) {
            const auto dtype=npy_scalar_descr(NpyScalar(f.dtype));
            log<<"{\"type\":\"ppisp_source_slot\",\"phase\":\"after\",\"name\":"<<std::quoted(f.name)
               <<",\"dtype\":"<<std::quoted(dtype.first)<<",\"flat_storage_shape\":["
               <<f.bytes.size()/dtype.second<<"],\"bytes\":"<<f.bytes.size()<<",\"sha256\":"
               <<std::quoted(backward_sha(f.bytes))<<"}\n";
        }
        {
            const auto canonical=backward_canonical(ids);
            backward_identity(log,"source-after-full-canonical",canonical);
            need(canonical.size()==43&&frozen_before.size()==18,"PPISP frozen shared coverage invalid");
            bool frozen_exact=true;
            for(size_t i=24;i<canonical.size()-1;++i) {
                const auto& f=canonical[i].field;
                const auto sha=backward_sha(f.bytes);
                const auto& prior=frozen_before[i-24];
                const bool equal=f.name==prior.name&&f.dtype==prior.dtype&&f.bytes==prior.bytes;
                frozen_exact &= equal;
                log<<"{\"type\":\"ppisp_frozen_field\",\"canonical_index\":"<<i
                   <<",\"name\":"<<std::quoted(f.name)<<",\"dtype\":"
                   <<std::quoted(npy_scalar_descr(NpyScalar(f.dtype)).first)
                   <<",\"bytes\":"<<f.bytes.size()<<",\"before_sha256\":"
                   <<std::quoted(backward_sha(prior.bytes))<<",\"after_sha256\":"<<std::quoted(sha)
                   <<",\"byte_exact\":"<<(equal?"true":"false")<<"}\n";
            }
            need(frozen_exact,"PPISP complete frozen shared values/optimizer inventory changed");
        }
        auto clocks=[](const EngineDiagnosticSnapshot& s)->const Bytes&{
            for(const auto& f:s.fields)if(f.name=="eng.bias_correction_steps")return f.bytes;
            throw std::runtime_error("PPISP full source clock field missing");
        };
        const auto& a=clocks(before);const auto& b=clocks(after);
        need(a.size()==4000000&&b.size()==a.size(),"PPISP full clock lengths invalid");
        for(size_t i=0;i<a.size();i+=4) {
            int32_t x,y;std::memcpy(&x,a.data()+i,4);std::memcpy(&y,b.data()+i,4);
            need(int64_t(y)==int64_t(x)+1,"PPISP full1M +1 clock invariant failed");
        }
        need(after.rows==before.rows&&after.max_rows==before.max_rows&&after.source_id==before.source_id&&
             after.owned_blocks==before.owned_blocks,"PPISP source control/topology invariant failed");
        need(ckpt::b1_config_hash(session.cfg)==packet.header.config_sha&&
             engine().b1.contract->source_step==10000&&engine().b1.contract->horizon==30000&&
             engine().b1.contract->source_state_sha256==packet.header.source_sha,
             "PPISP native source/config/horizon changed");
        need(log.tellp()>=0&&uint64_t(log.tellp())<=(uint64_t(64)<<20),
             "PPISP diagnostic log exceeds declared64MiB bound");
        log<<"{\"type\":\"ppisp_complete\",\"native_updates\":1,\"captures\":1,\"auxiliary_backwards\":0,"
               "\"optimizer_gradient_replacements\":0,\"full1M_clock_increment\":1,\"frozen_context_verified\":true,"
               "\"frozen_shared_canonical_first\":24,\"frozen_shared_field_count\":18,"
               "\"frozen_shared_comparison\":\"name_dtype_length_every_byte\","
               "\"local_exact\":"<<(exact?"true":"false")<<"}\n";
        log.close();need(bool(log),"PPISP capture log write failed");
        source_close(session);need(exact,"PPISP captured exact RGB proof failed; full pair retained");
    } catch(...) {engine_diagnostic_ppisp_probe({});source_close(session);throw;}
}
std::vector<std::string> backward_digests(const BackwardObservations& fields) {
    std::vector<std::string> result;
    for (const auto& f : fields)
        result.push_back(f.field.name + ":" + std::to_string(f.field.dtype) + ":" +
                         std::to_string(f.field.bytes.size()) + ":" + backward_sha(f.field.bytes));
    return result;
}
bool backward_compare(std::ostream& out, const BackwardObservations& reference,
                      const BackwardObservations& actual, const std::string& label,
                      const fs::path& root, bool& full_pair_written) {
    need(reference.size() == actual.size(), "once/prime observation field count changed");
    bool equal = true;
    for (size_t i = 0; i < actual.size(); ++i) {
        const auto& a = reference[i]; const auto& b = actual[i];
        need(a.field.name == b.field.name && a.field.dtype == b.field.dtype, "once/prime named schema changed");
        const bool same_shape = a.shape == b.shape && a.field.bytes.size() == b.field.bytes.size();
        out << "{\"type\":\"once_delta\",\"label\":" << std::quoted(label)
            << ",\"field\":" << std::quoted(a.field.name) << ",\"shape_equal\":" << (same_shape ? "true" : "false");
        if (!same_shape) {
            out << ",\"numeric_comparison\":false}\n";
            need(label.find("-0-") == std::string::npos, "once/prime same-prestate first-event shape differs");
            equal = false; continue;
        }
        uint64_t different = 0, outside = 0, nonfinite_bad = 0, finite = 0;
        size_t first = a.field.bytes.size(), worst_cell = 0;
        double maxabs = 0, square = 0, refsq = 0;
        for (size_t j = 0; j < a.field.bytes.size(); ++j)
            if (a.field.bytes[j] != b.field.bytes[j]) { ++different; first = std::min(first, j); }
        if (a.field.dtype == uint8_t(NpyScalar::f4))
            for (size_t j = 0; j < a.field.bytes.size(); j += 4) {
                float x, y;
                std::memcpy(&x, a.field.bytes.data() + j, 4); std::memcpy(&y, b.field.bytes.data() + j, 4);
                if (!std::isfinite(x) || !std::isfinite(y)) { nonfinite_bad += std::memcmp(&x, &y, 4) != 0; continue; }
                ++finite;
                const double delta = std::abs(double(x) - double(y));
                if (delta > maxabs) { maxabs = delta; worst_cell = j / 4; }
                square += delta * delta; refsq += double(x) * double(x);
                outside += std::min(delta, delta / (1e-6 + std::abs(double(x)))) > 1e-4;
            }
        out << ",\"different_bytes\":" << different << ",\"finite_pairs\":" << finite
            << ",\"nonfinite_bit_mismatch\":" << nonfinite_bad << ",\"maxabs\":" << maxabs
            << ",\"relRMS\":" << std::sqrt(square / std::max(refsq, 1e-30))
            << ",\"screen_outside\":" << outside << ",\"worst_cell\":" << worst_cell
            << ",\"first_byte\":";
        if (different) out << first; else out << "null";
        out << ",\"first_raw_block_256_bytes\":";
        if (different) out << first / 256; else out << "null";
        out << ",\"first_reference_hex\":\"";
        constexpr char hex[] = "0123456789abcdef";
        const size_t end = different ? std::min(first + 64, a.field.bytes.size()) : first;
        for (size_t j = first; j < end; ++j) out << hex[a.field.bytes[j] >> 4] << hex[a.field.bytes[j] & 15];
        out << "\",\"first_actual_hex\":\"";
        for (size_t j = first; j < end; ++j) out << hex[b.field.bytes[j] >> 4] << hex[b.field.bytes[j] & 15];
        out << "\"}\n";
        equal &= different == 0;
        if (different && !full_pair_written) {
            need(a.field.bytes.size() + b.field.bytes.size() <= 360000000, "once/prime affected-field pair cap");
            for (const auto& entry : {std::pair{"reference", &a.field.bytes}, std::pair{"actual", &b.field.bytes}}) {
                std::ofstream file(root / (std::string("first-affected-") + entry.first + ".bin"), std::ios::binary);
                file.write(reinterpret_cast<const char*>(entry.second->data()), std::streamsize(entry.second->size()));
                need(bool(file), "once/prime affected-field write");
            }
            out << "{\"type\":\"once_full_pair\",\"label\":" << std::quoted(label)
                << ",\"field\":" << std::quoted(a.field.name) << ",\"bytes_each\":" << a.field.bytes.size()
                << ",\"reference_sha256\":" << std::quoted(backward_sha(a.field.bytes))
                << ",\"actual_sha256\":" << std::quoted(backward_sha(b.field.bytes)) << "}\n";
            full_pair_written = true;
        }
    }
    return equal;
}

BackwardObservations backward_losses(const std::map<std::string, float>& losses) {
    BackwardObservations fields;
    for (const auto& [name, value] : losses) {
        Bytes bytes(sizeof(value));
        std::memcpy(bytes.data(), &value, sizeof(value));
        fields.push_back({{"returned_loss." + name, uint8_t(NpyScalar::f4), false, std::move(bytes)}, {1}});
    }
    return fields;
}

void once_vs_prime_check(spirula::TrainerSession& session,
                        const EngineDiagnosticSnapshot& baseline,
                        const fs::path& root, const char* resume) {
    need(spirula::env("WORKER_DEVICE_LIMIT_BYTES") &&
         std::string(spirula::env("WORKER_DEVICE_LIMIT_BYTES")) == "25769803776",
         "once/prime requires explicit actual Vulkan backing cap24GiB");
    need(baseline.rows == 1000000 && baseline.max_rows == 1000000 && baseline.num_sh == 15 &&
         session.ds.train_indices.size() == 1195, "once/prime requires bound original1M/1195-training source");
    const int first = session.start_step;
    const auto run_state = session.st;
    const auto config_hash = ckpt::b1_config_hash(session.cfg);
    const Fields source_stats = statistics();
    const auto source_weight = engine().fwd.accum_weight;
    const auto source_mode = engine().fwd.accum_mode;
    std::vector<uint32_t> stable(size_t(baseline.rows));
    std::iota(stable.begin(), stable.end(), 0);
    const Bytes stable_ids(reinterpret_cast<const uint8_t*>(stable.data()),
                           reinterpret_cast<const uint8_t*>(stable.data() + stable.size()));
    std::ofstream out(root / "once-vs-prime.jsonl", std::ios::binary);
    need(bool(out), "once/prime output cannot open");
    out << std::setprecision(17);
    bool full_pair_written = false, ordinary_exact = true, capture_gates = true, q_screen = true;
    int native_updates = 0, prime_calls = 0, captures = 0;
    const auto started = Clock::now();
    auto reset = [&] {
        engine_diagnostic_restore(baseline);
        // The persistent diagnostic baseline deliberately does not own raster
        // scratch. Restore its original holder/mode here, not in production.
        engine().fwd.accum_weight = source_weight; engine().fwd.accum_mode = source_mode;
        restore_statistics(source_stats);
        session.st = run_state;
        need(statistics() == source_stats && engine().fwd.accum_weight.data_ptr() == source_weight.data_ptr() &&
             engine().fwd.accum_weight.size() == source_weight.size() && engine().fwd.accum_mode == source_mode,
             "once/prime transient source reset differs");
        engine_diagnostic_verify_start(baseline);
    };
    auto source_record = [&](const char* phase) {
        const auto current = engine_diagnostic_capture(false);
        for (const auto& f : current.fields) {
            const auto dtype = npy_scalar_descr(NpyScalar(f.dtype));
            out << "{\"type\":\"once_source_slot\",\"phase\":" << std::quoted(phase)
                << ",\"name\":" << std::quoted(f.name) << ",\"dtype\":" << std::quoted(dtype.first)
                << ",\"flat_storage_shape\":[" << f.bytes.size() / dtype.second
                << "],\"bytes\":" << f.bytes.size() << ",\"serialized\":" << (f.serialized ? "true" : "false")
                << ",\"sha256\":" << std::quoted(backward_sha(f.bytes)) << "}\n";
        }
        backward_identity(out, std::string(phase) + "-canonical43", backward_canonical(stable_ids));
        out << "{\"type\":\"once_source_host\",\"phase\":" << std::quoted(phase)
            << ",\"sha256\":" << std::quoted(current.host_signature)
            << ",\"common_sha256\":" << std::quoted(current.common_host_signature) << "}\n";
    };
    out << "{\"type\":\"once_scope\",\"native_updates\":135,\"extra_prime_backwards\":54,"
           "\"auxiliary_captures\":54,\"source_step\":10000,\"horizon\":30000,\"canonical_fields\":43,"
           "\"canonical_runtime_fields\":42,\"stable_id_provenance\":\"implicit_original_source_row_order;"
           "not_GPU_ID_readback;bound_native_B1_fixed_topology_masks_world_clocks_context\","
           "\"ordered_raster\":false,\"gradient_substitution\":false,\"new_output_reservation\":427108864,"
           "\"field_cap\":180000000,\"stage_workspace_cap\":2147483648,"
           "\"quality_recovery_qualification\":false,\"source\":"
        << std::quoted(fs::absolute(resume).string()) << ",\"config_sha256\":" << std::quoted(config_hash) << "}\n";
    try {
        source_record("initial");
        engine_diagnostic_self_check(baseline);
        reset();
        for (const int owner : {2, 0, 1}) {
            const auto role_started = Clock::now();
            for (const int endpoint : {1, 8}) {
                std::map<std::string, BackwardObservations> native_stages, primed_stages;
                std::map<std::string, std::vector<std::string>> native_digests, primed_digests;
                BackwardObservations native_canonical, primed_canonical, native_gradient;
                EngineDiagnosticSnapshot native_state, primed_state;
                size_t retained = 0;
                for (const std::string arm : {"N_A", "N_B", "N_C", "P", "P_C"}) {
                    reset();
                    auto bind_role = [&] {
                        if (owner != 2) {
                            engine_b1_configure(owner, resume, first);
                            engine_b1_bind_contract(session.cfg, spirula::env("B1_VIEWS"),
                                session.ds.train_indices, resume, "full", session.b1_full_train_indices);
                        }
                    };
                    bind_role();
                    auto starting = engine_diagnostic_capture(false);
                    engine_diagnostic_verify_start(starting);
                    need(starting.common_host_signature == baseline.common_host_signature &&
                         starting.fields.size() == baseline.fields.size(), "once/prime role common source metadata differs");
                    for (size_t f = 0; f < starting.fields.size(); ++f)
                        need(starting.fields[f].name == baseline.fields[f].name &&
                             starting.fields[f].dtype == baseline.fields[f].dtype &&
                             starting.fields[f].bytes == baseline.fields[f].bytes,
                             "once/prime role device source differs");
                    if (owner == 2)
                        for (auto block : starting.owned_blocks) need(block == 1, "once/prime all-owned mask has a hole");
                    const auto means = std::find_if(starting.fields.begin(), starting.fields.end(),
                        [](const auto& f) { return f.name == "world.means"; });
                    need(means != starting.fields.end(), "once/prime source means slot missing");
                    // Independent rejection of partial-world changes in both mask
                    // classes. Re-reading every slot after native source reset is
                    // required, not merely undoing the one modified cell.
                    for (const uint32_t mask_value : {1u, 0u}) {
                        size_t row = 0;
                        for (; row < size_t(starting.rows); ++row) {
                            float x;
                            std::memcpy(&x, means->bytes.data() + row * 12, 4);
                            if (starting.owned_blocks[row / 256] == mask_value && std::isfinite(x)) break;
                        }
                        if (row == size_t(starting.rows)) {
                            need(owner == 2 && mask_value == 0, "once/prime missing finite mask-class test row");
                            continue;
                        }
                        uint32_t bits;
                        auto* cell = reinterpret_cast<uint32_t*>(engine().world.means.data_ptr()) + row * 3;
                        backend::memcpy_sync(&bits, cell, 4, backend::MemcpyKind::DeviceToHost);
                        bits ^= 1u;
                        backend::memcpy_sync(cell, &bits, 4, backend::MemcpyKind::HostToDevice);
                        bool rejected = false;
                        try { engine_diagnostic_verify_start(starting); }
                        catch (const std::runtime_error& error) {
                            rejected = std::string(error.what()) == "B2 causal independent starting field inequality: world.means";
                        }
                        need(rejected, "once/prime partial world poison was not rejected");
                        reset(); bind_role();
                        engine_diagnostic_verify_start(starting);
                        out << "{\"type\":\"once_partial_reset_check\",\"owner\":" << owner
                            << ",\"endpoint\":" << endpoint << ",\"arm\":" << std::quoted(arm)
                            << ",\"mask_class\":" << mask_value << ",\"row\":" << row
                            << ",\"rejected\":true,\"all_source_slots_restored_exact\":true}\n";
                    }
                    const bool prime = arm == "P" || arm == "P_C";
                    const bool capture = arm == "N_C" || arm == "P_C";
                    const std::string prefix = "owner" + std::to_string(owner) + "-end" + std::to_string(endpoint) + "-" + arm;
                    const auto source_clocks = std::find_if(starting.fields.begin(), starting.fields.end(),
                        [](const auto& f) { return f.name == "eng.bias_correction_steps"; });
                    need(source_clocks != starting.fields.end() && source_clocks->bytes.size() == 4000000,
                         "once/prime source clock schema");
                    spirula::Sha256 mask_sha;
                    mask_sha.update(reinterpret_cast<const uint8_t*>(starting.owned_blocks.data()),
                                    starting.owned_blocks.size() * sizeof(uint32_t));
                    out << "{\"type\":\"once_arm_start\",\"label\":" << std::quoted(prefix)
                        << ",\"source_id\":" << starting.source_id << ",\"host_sha256\":" << std::quoted(starting.host_signature)
                        << ",\"frozen_context\":" << std::quoted(engine().b1.frozen_hash)
                        << ",\"mask_sha256\":" << std::quoted(mask_sha.hex())
                        << ",\"owned_block_count\":" << std::count(starting.owned_blocks.begin(), starting.owned_blocks.end(), 1u)
                        << ",\"mask_block_count\":" << starting.owned_blocks.size()
                        << ",\"rows\":" << starting.rows << ",\"tail_valid_lanes\":64"
                        << ",\"transient_weight_bytes\":" << source_stats[3].size()
                        << ",\"transient_weight_mode\":" << int(source_mode) << ",\"reset_exact\":true}\n";
                    { std::ofstream host(root / (prefix + "-host.txt"), std::ios::binary);
                      host << starting.host_state; need(bool(host), "once/prime host binding write"); }
                    engine_diagnostic_begin_arm(arm == "N_A" ? "A" : arm);
                    bool precapture_exact = true, callback_active = false;
                    BackwardObservations priming_observation;
                    int event = 0;
                    EngineStepConfig resolved;
                    Clock::time_point event_started, observer_exit;
                    auto observe = [&](const std::string& stage, BackwardObservations fields) {
                        const auto observation_started = Clock::now();
                        const size_t local = backward_observation_bytes(fields);
                        need(retained + local + backward_observation_bytes(priming_observation) <= BackwardStageCap,
                             "once/prime stage workspace cap");
                        const std::string key = std::to_string(event) + "-" + stage;
                        const std::string label = prefix + "-" + key;
                        auto digests = backward_digests(fields);
                        backward_identity(out, label, fields, &digests);
                        if (stage == "backward" && prime) {
                            need(!priming_observation.empty(), "once/prime missing same-forward first backward");
                            (void)backward_compare(out, priming_observation, fields,
                                label + "-same-forward-first-vs-second", root, full_pair_written);
                            priming_observation.clear();
                        }
                        if (stage != "prime" && stage != "prime_returned_loss") {
                            auto& references = prime ? primed_digests : native_digests;
                            if (arm == "N_A" || arm == "P") {
                                references.emplace(key, digests);
                                if (arm == "P") {
                                    const bool exact = native_digests.at(key) == digests;
                                    precapture_exact &= exact;
                                    out << "{\"type\":\"once_stage_gate\",\"label\":" << std::quoted(label + "-vs-N_A")
                                        << ",\"exact\":" << (exact ? "true" : "false") << "}\n";
                                }
                            }
                            else if (stage != "data_gradient") {
                                const bool exact = references.at(key) == digests;
                                precapture_exact &= exact;
                                out << "{\"type\":\"once_stage_gate\",\"label\":" << std::quoted(label)
                                    << ",\"exact\":" << (exact ? "true" : "false") << "}\n";
                            }
                            if (event + 1 == endpoint) {
                                if (arm == "N_A" || arm == "P") {
                                    if (arm == "P")
                                        (void)backward_compare(out, native_stages.at(stage), fields,
                                            label + "-vs-N_A", root, full_pair_written);
                                    retained += local;
                                    (prime ? primed_stages : native_stages).emplace(stage, std::move(fields));
                                } else if (stage == "data_gradient") {
                                    if (arm == "N_C") { retained += local; native_gradient = std::move(fields); }
                                    else (void)backward_compare(out, native_gradient, fields,
                                        label + "-vs-N_C-auxiliary-only", root, full_pair_written);
                                } else (void)backward_compare(out, (prime ? primed_stages : native_stages).at(stage),
                                                              fields, label, root, full_pair_written);
                            }
                        }
                        if (stage == "prime") priming_observation = std::move(fields);
                        out << "{\"type\":\"once_observer_wall\",\"label\":" << std::quoted(label)
                            << ",\"readback_digest_compare_seconds\":"
                            << std::chrono::duration<double>(Clock::now() - observation_started).count() << "}\n";
                    };
                    engine_diagnostic_mask_probe([&](const char* stage, int step) {
                        need(!callback_active, "once/prime recursive diagnostic callback");
                        callback_active = true;
                        const auto entered = Clock::now();
                        const bool forward = std::strcmp(stage, "forward") == 0;
                        out << "{\"type\":\"once_native_phase_wall\",\"label\":" << std::quoted(prefix)
                            << ",\"event\":" << event << ",\"phase\":" << std::quoted(forward
                                ? "managed_entry_to_forward_observer" : std::strcmp(stage, "backward") == 0
                                ? "forward_observer_exit_to_native_backward_complete"
                                : "backward_observer_exit_to_aux_capture_complete")
                            << ",\"seconds\":" << std::chrono::duration<double>(entered - (forward ? event_started : observer_exit)).count()
                            << ",\"bin_policy_input\":\"recorded_A_seconds_replayed_not_this_wall\"}\n";
                        need(retained + backward_observation_bytes(priming_observation) + 314000000 <= BackwardStageCap,
                             "once/prime preallocation stage workspace cap");
                        observe(stage, backward_observe(stage));
                        if (prime && std::strcmp(stage, "forward") == 0) {
                            const auto prior = statistics();
                            const auto prime_started = Clock::now();
                            const auto losses = engine_compute_loss_backward(
                                step, resolved.loss.weights, resolved.loss.w_ssim, resolved.loss.num_loss_scales,
                                resolved.loss.loss_scale_min_pixels, resolved.loss.compute_loss_map,
                                resolved.loss.loss_map_mode, resolved.loss.robust_edge_aware_quantile,
                                resolved.loss.nms_falloff, resolved.loss.loss_map_normalize,
                                resolved.loss.loss_map_clip_quantile, resolved.loss.loss_map_power,
                                resolved.loss.loss_map_accum_mode, resolved.loss.saturation_threshold,
                                resolved.loss.overexposure_reg_weight, resolved.loss.color_shift_reg_weight,
                                resolved.loss.color_shift_reg_beta);
                            ++prime_calls;
                            const double prime_seconds = std::chrono::duration<double>(Clock::now() - prime_started).count();
                            observe("prime_returned_loss", backward_losses(losses));
                            need(retained + backward_observation_bytes(priming_observation) + 314000000 <= BackwardStageCap,
                                 "once/prime prime workspace cap");
                            observe("prime", backward_observe("prime"));
                            const auto restore_started = Clock::now();
                            restore_statistics(prior);
                            backend::device_synchronize();
                            const auto restored = statistics();
                            need(prior.size() == 4 && restored.size() == 4, "once/prime statistics schema");
                            for (size_t f = 0; f < 4; ++f)
                                if (f == 3 && prior[f].empty())
                                    need(std::all_of(restored[f].begin(), restored[f].end(), [](uint8_t b) { return b == 0; }),
                                         "once/prime absent statistics logical-zero restore failed");
                                else need(prior[f] == restored[f], "once/prime four statistics restore differs");
                            out << "{\"type\":\"once_prime_timing\",\"label\":" << std::quoted(prefix)
                                << ",\"event\":" << event << ",\"loss_backward_seconds\":" << prime_seconds
                                << ",\"statistics_restore_verify_seconds\":"
                                << std::chrono::duration<double>(Clock::now() - restore_started).count()
                                << ",\"four_statistics_restore_exact_or_absent_logical_zero\":true}\n";
                        }
                        if (std::strcmp(stage, "data_gradient") == 0) ++captures;
                        callback_active = false;
                        observer_exit = Clock::now();
                    }, false, capture);
                    for (event = 0; event < endpoint; ++event) {
                        need(ckpt::b1_config_hash(session.cfg) == config_hash, "once/prime configuration changed");
                        resolved = engine_resolve_managed_step_config(spirula::build_step_config(session.cfg, session.st, first + event));
                        need(resolved.optim.use_fused_proj_bwd_optim && !resolved.optim.split_batch &&
                             resolved.loss.color_shift_reg_weight == 0.f, "once/prime requires native single-pass FPBO config");
                        event_started = Clock::now();
                        const auto losses = session.train_step(first + event);
                        ++native_updates;
                        backend::device_synchronize();
                        if (const char* error = backend::last_error()) throw std::runtime_error(error);
                        need(!engine().grad.quantize_grad, "once/prime FPBO quantized data gradients unexpectedly active");
                        engine_b1_verify_context();
                        engine_diagnostic_step_state(first + event + 1, true, &starting);
                        const Bytes clocks = copy_device(engine().optim.bias_correction_steps.data_ptr(), 4000000);
                        for (size_t row = 0; row < size_t(starting.rows); ++row) {
                            int32_t before, now;
                            std::memcpy(&before, source_clocks->bytes.data() + row * 4, 4);
                            std::memcpy(&now, clocks.data() + row * 4, 4);
                            const int64_t expected = int64_t(before) +
                                (starting.owned_blocks[row / 256] ? event + 1 : 0);
                            need(now == expected, "once/prime all-row clock invariant failed");
                        }
                        out << "{\"type\":\"once_clock_check\",\"label\":" << std::quoted(prefix)
                            << ",\"event\":" << event << ",\"rows_including_inactive\":1000000,"
                               "\"owned_increment\":"
                            << event + 1 << ",\"unowned_increment\":0,\"exact\":true,"
                               "\"sha256\":" << std::quoted(backward_sha(clocks)) << "}\n";
                        out << "{\"type\":\"once_native_phase_wall\",\"label\":" << std::quoted(prefix)
                            << ",\"event\":" << event
                            << ",\"phase\":\"last_observer_exit_to_Internal_optimizer_and_frozen_poststep_complete\","
                               "\"seconds\":" << std::chrono::duration<double>(Clock::now() - observer_exit).count() << "}\n";
                        observe("returned_loss", backward_losses(losses));
                        out << "{\"type\":\"once_event_wall\",\"label\":" << std::quoted(prefix)
                            << ",\"event\":" << event << ",\"managed_step_and_observers_seconds\":"
                            << std::chrono::duration<double>(Clock::now() - event_started).count() << "}\n";
                        need(std::chrono::duration<double>(Clock::now() - role_started).count() <= 900 &&
                             std::chrono::duration<double>(Clock::now() - started).count() <= 2700,
                             "once/prime wall admission exceeded");
                    }
                    engine_diagnostic_mask_probe({}, false, false);
                    engine_diagnostic_end_arm();
                    auto actual = engine_diagnostic_capture(false);
                    need(engine_diagnostic_compare(starting, actual, actual, out, prefix + "-invariants",
                         endpoint, {}, true, false), "once/prime native invariant failed");
                    auto canonical = backward_canonical(stable_ids);
                    backward_identity(out, prefix + "-canonical43", canonical);
                    if (arm == "N_A") { native_state = std::move(actual); native_canonical = std::move(canonical); }
                    else {
                        const auto& reference_state = prime && arm == "P_C" ? primed_state : native_state;
                        const auto& reference_canonical = prime && arm == "P_C" ? primed_canonical : native_canonical;
                        const bool exact = backward_compare(out, reference_canonical, canonical, prefix + "-canonical43",
                                                            root, full_pair_written);
                        const bool screen = engine_diagnostic_compare(starting, reference_state, actual, out,
                            prefix + "-unchanged-Q-screen", endpoint, {}, false, false);
                        q_screen &= screen;
                        if (arm == "N_B") ordinary_exact &= precapture_exact && exact;
                        if (capture) capture_gates &= precapture_exact && exact;
                        out << "{\"type\":\"once_endpoint_gate\",\"label\":" << std::quoted(prefix)
                            << ",\"precapture_exact\":" << (precapture_exact ? "true" : "false")
                            << ",\"canonical_exact\":" << (exact ? "true" : "false")
                            << ",\"unchanged_Q_screen\":" << (screen ? "true" : "false")
                            << ",\"mismatch_attribution\":\"independent_trajectory_not_proven_capture_or_priming_cause\"}\n";
                        if (arm == "P") { primed_state = std::move(actual); primed_canonical = std::move(canonical); }
                    }
                    out.flush(); need(bool(out) && uint64_t(out.tellp()) < (uint64_t(32) << 20),
                                      "once/prime summary exceeds records admission");
                }
            }
        }
        reset();
        source_record("final");
        need(native_updates == 135 && prime_calls == 54 && captures == 54, "once/prime bounded accounting differs");
        out << "{\"type\":\"once_complete\",\"native_updates\":" << native_updates
            << ",\"prime_backwards\":" << prime_calls << ",\"auxiliary_captures\":" << captures
            << ",\"ordinary_repeat_exact\":" << (ordinary_exact ? "true" : "false")
            << ",\"capture_equivalence_established\":" << (capture_gates && ordinary_exact ? "true" : "false")
            << ",\"all_decoded_screens_pass\":" << (q_screen ? "true" : "false")
            << ",\"source_reset_exact\":true,\"independent_source_TAR_verification\":\"parent_CPU_consumer_required\","
               "\"operational_complete\":true,\"quality_recovery_qualification\":false,"
               "\"wall_boundary\":\"after_source_setup_through_final_reset_records_before_engine_close\","
               "\"harness_wall_seconds\":"
            << std::chrono::duration<double>(Clock::now() - started).count() << "}\n";
        out.close(); need(bool(out), "once/prime final records write");
        source_close(session);
    } catch (...) {
        engine_diagnostic_mask_probe({}, false, false);
        try { reset(); } catch (...) { source_close(session); throw; }
        source_close(session);
        throw;
    }
}

void native_mask_check(int argc, char** argv, bool once_vs_prime) {
    need(argc == 8 && std::strcmp(argv[2], "--resume") == 0 &&
         std::strcmp(argv[4], "--device") == 0 &&
         std::strcmp(argv[6], "--output") == 0, "native mask check arguments");
    need(spirula::env("B1_OWNER") && std::string(spirula::env("B1_OWNER")) == "all" &&
         spirula::env("B1_VIEWS") && *spirula::env("B1_VIEWS"),
         "native mask check requires SS_B1_OWNER=all SS_B1_VIEWS");
    need(!spirula::env("NO_TILE_SKIP") && !spirula::env("B1_REFRESH_CADENCE"),
         "native mask check refuses tile-skip override or refresh experiment");
    need(!std::getenv("SS_B1_VIEW_POLICY") && !std::getenv("SSPLAT_B1_VIEW_POLICY"),
         "native mask check forbids view policy environment overrides");
    need(!ckpt::read_b1_contract(argv[3]), "native mask check requires original unmarked source");
    need(std::string(argv[5]).rfind("uuid:", 0) == 0 &&
         backend::device_select_identity(argv[5]), "native mask check device unavailable");
    const fs::path root = argv[7];
    need(fs::create_directory(root), "native mask output must be a new directory");
    spirula::TrainerSession session;
    TrainConfig cli; cli.resume = argv[3]; cli.cache_images = "disk";
    session.cfg = ckpt::build_resume_config(cli, "", {"cache_images"});
    session.write_config_json = false;
    session.diagnostic_sync_sampler = true;
    session.out_dir_override = (root / "session").string();
    session.check_config(); session.load_dataset(); session.setup_engine();
    vkk::g1_set_ordered_raster(false);
    const int first = session.start_step;
    need(first == 10000 && session.cfg.num_iterations == 30000 &&
         engine().b1.owner == 2 && engine().b1.contract &&
         session.b1_view_policy == "full" && engine().b1.contract->view_policy == "full" &&
         session.ds.train_indices == session.b1_full_train_indices,
         "native mask check requires full-view source10000 horizon30000");
    const auto config_hash = ckpt::b1_config_hash(session.cfg);
    const auto sc = engine_resolve_managed_step_config(
        spirula::build_step_config(session.cfg, session.st, first));
    need(sc.loss.color_shift_reg_weight == 0.f,
         "native mask check requires source frozen color-shift EMA");
    // Match the native lazy loss constants before recording the reset schema.
    auto* loss_weights = DevicePool::global().acquire<float>(PoolSlot::EngVLosses, (int)LossIndex::length);
    std::array<float, (int)LossIndex::length> initial_loss_weights;
    initial_loss_weights.fill(1.f);
    initial_loss_weights[(int)LossIndex::RgbPSNR] = 0.f;
    backend::memcpy_sync(loss_weights, initial_loss_weights.data(), sizeof(initial_loss_weights),
                         backend::MemcpyKind::HostToDevice);
    engine().v_losses_uploaded = true;
    const auto baseline = engine_diagnostic_capture(true);
    size_t snapshot_bytes = 0;
    for (const auto& field : baseline.fields) snapshot_bytes += field.bytes.size();
    need(snapshot_bytes <= (size_t(1) << 29), "native mask snapshot exceeds 512 MiB admission");
    if (once_vs_prime) {
        once_vs_prime_check(session, baseline, root, argv[3]);
        return;
    }
    const auto run_state = session.st;
    std::ofstream out(root / "comparison.jsonl", std::ios::binary);
    need(bool(out), "native mask comparison cannot open");
    out << "{\"type\":\"native_mask_scope\",\"source_step\":" << first
        << ",\"horizon\":30000,\"events_per_arm\":8,\"arms\":4,\"updates\":32,"
           "\"ordered_raster\":false,\"production_background_and_tile_policy\":true,"
           "\"shared_and_topology_frozen\":true,\"whole_trainer_equivalence\":false,"
           "\"gradient_substitution\":false,\"snapshot_bytes\":" << snapshot_bytes
        << ",\"host_snapshot_projection\":" << 6 * snapshot_bytes
        << ",\"stage_workspace_cap_bytes\":2147483648,\"output_projection_bytes\":67108864,"
           "\"config_sha256\":\"" << config_hash << "\"}\n";
    out << "{\"type\":\"native_mask_settings\",\"device\":\"" << argv[5]
        << "\",\"rows\":" << baseline.rows << ",\"capacity\":" << baseline.max_rows
        << ",\"sh_coefficients\":" << baseline.num_sh << ",\"sh_degree\":"
        << first / std::max(session.cfg.sh_degree_warmup_every, 1)
        << ",\"lr_means\":" << sc.optim.lr_means << ",\"lr_quats\":" << sc.optim.lr_quats
        << ",\"lr_scales\":" << sc.optim.lr_scales << ",\"lr_opacities\":" << sc.optim.lr_opacities
        << ",\"lr_dc\":" << sc.optim.lr_features_dc << ",\"lr_sh\":" << sc.optim.lr_features_sh
        << ",\"background_seed\":" << sc.background.seed
        << ",\"background_randomize_weight\":" << sc.background.randomize_weight
        << ",\"ssim_weight\":" << sc.loss.w_ssim << ",\"loss_weights\":[";
    for (size_t i = 0; i < sc.loss.weights.size(); ++i)
        out << (i ? "," : "") << sc.loss.weights[i];
    out << "],\"forward_fields\":\"rgb,depth,Ts_after_native_appearance\","
           "\"backward_fields\":\"rgb,depth,Ts_cotangents,current_native_weighted_loss_vector,"
           "last_native_scale_ssim_scalar,raster_screen_gradient_tensors\","
           "\"data_gradient_fields\":\"means,quats,scales,opacities,dc,sh_before_regularizers\","
           "\"regularizer_stage\":\"fused_native_update_not_separately_materialized\","
           "\"timing_replay\":\"native_A_bin_policy_seconds_only\"}\n";
    using Stages = std::map<std::string, Fields>;
    std::array<Stages, 2> reference_stages;
    std::array<EngineDiagnosticSnapshot, 2> reference_states;
    size_t retained_stage_bytes = 0;
    bool invariants = true;
    try {
        for (const std::string arm : {"A", "B", "owner0", "owner1"}) {
            engine_diagnostic_restore(baseline);
            session.st = run_state;
            const int owner = arm == "owner0" ? 0 : arm == "owner1" ? 1 : 2;
            if (owner != 2) {
                engine_b1_configure(owner, argv[3], first);
                engine_b1_bind_contract(session.cfg, spirula::env("B1_VIEWS"),
                    session.ds.train_indices, argv[3], "full", session.b1_full_train_indices);
            }
            const auto starting = engine_diagnostic_capture(false);
            engine_diagnostic_verify_start(starting);
            need(starting.common_host_signature == baseline.common_host_signature,
                 "native mask common host/source/settings/sampler prestate differs");
            need(starting.fields.size() == baseline.fields.size(), "native mask reset field schema");
            for (size_t i = 0; i < baseline.fields.size(); ++i)
                need(starting.fields[i].name == baseline.fields[i].name &&
                     starting.fields[i].bytes == baseline.fields[i].bytes,
                     "native mask common actual device prestate differs");
            out << "{\"type\":\"native_mask_start\",\"arm\":\"" << arm
                << "\",\"owner\":" << owner << ",\"host_sha256\":\"" << starting.host_signature
                << "\",\"source_id\":" << starting.source_id
                << ",\"frozen_context\":\"" << engine().b1.frozen_hash
                << "\",\"common_device_exact\":true,\"role_contract_bound\":true}\n";
            { std::ofstream host(root / (arm + "-start-host.txt"), std::ios::binary);
              host << starting.host_state; need(bool(host), "native mask host evidence write"); }
            engine_diagnostic_begin_arm(arm);
            int updates = 0;
            engine_diagnostic_mask_probe([&](const char* stage, int) {
                if (updates != 1 && updates != 8) return;
                const size_t probe = updates == 1 ? 0 : 1;
                Fields fields;
                size_t local_bytes = 0;
                auto capture = [&](const void* ptr, size_t bytes) {
                    need(retained_stage_bytes + local_bytes + bytes <= (size_t(2) << 30),
                         "native mask stage workspace exceeds 2 GiB");
                    fields.push_back(copy_device(ptr, bytes)); local_bytes += bytes;
                };
                auto tensor = [&](const auto& t) {
                    capture(t.data_ptr(), t.data_ptr() ? size_t(t.numel()) * sizeof(*t.data_ptr()) : 0);
                };
                const std::string name = stage;
                if (name == "forward") {
                    tensor(std::get<0>(engine().fwd.renders));
                    tensor(std::get<1>(engine().fwd.renders));
                    tensor(engine().fwd.render_Ts);
                } else if (name == "backward") {
                    const size_t pixels = size_t(engine().camera.num) *
                        engine().camera.height * engine().camera.width;
                    for (const auto slot : {PoolSlot::EngVRgb, PoolSlot::EngVDepth, PoolSlot::EngVTs}) {
                        const size_t count = pixels * (slot == PoolSlot::EngVRgb ? 3 : 1);
                        capture(DevicePool::global().acquire<float>(slot, count), count * sizeof(float));
                    }
                    capture(DevicePool::global().acquire<float>(PoolSlot::PplTotalLosses, (int)LossIndex::length),
                        sizeof(float) * (int)LossIndex::length);
                    capture(DevicePool::global().acquire<float>(PoolSlot::SsimScalar, 1), sizeof(float));
                    for (const auto& t : engine().fwd.v_splats_s) tensor(t);
                } else {
                    const auto& g = engine().grad;
                    local_bytes = bytes(g.means) + bytes(g.quats) + bytes(g.scales) +
                        bytes(g.opacities) + bytes(g.features_dc) + bytes(g.features_sh);
                    need(retained_stage_bytes + local_bytes <= (size_t(2) << 30),
                         "native mask gradient workspace exceeds 2 GiB");
                    fields = gradient();
                }
                const std::string label = arm + "-" + name + "-" + std::to_string(updates);
                out << "{\"type\":\"native_mask_stage\",\"arm\":\"" << arm
                    << "\",\"stage\":\"" << name << "\",\"updates\":" << updates
                    << ",\"fields\":" << fields.size() << ",\"bytes\":" << local_bytes << "}\n";
                for (size_t i = 0; i < fields.size(); ++i) {
                    spirula::Sha256 sha; sha.update(fields[i].data(), fields[i].size());
                    out << "{\"type\":\"native_mask_stage_identity\",\"label\":\"" << label
                        << "\",\"field\":" << i << ",\"sha256\":\"" << sha.hex() << "\"}\n";
                }
                if (arm == "A") {
                    retained_stage_bytes += local_bytes;
                    reference_stages[probe].emplace(name, std::move(fields));
                } else {
                    const auto& reference = reference_stages[probe].at(name);
                    bool same_shape = reference.size() == fields.size();
                    for (size_t i = 0; same_shape && i < fields.size(); ++i)
                        same_shape = reference[i].size() == fields[i].size();
                    if (same_shape) (void)replay_compare(reference, fields, label.c_str(), false, true);
                    else {
                        out << "{\"type\":\"native_mask_stage_shape_delta\",\"label\":\"" << label
                            << "\",\"reference_fields\":" << reference.size()
                            << ",\"actual_fields\":" << fields.size() << ",\"numeric_comparison\":false}\n";
                        need(updates != 1, "same-prestate first-event stage shape differs");
                    }
                }
            }, owner == 2, true);
            for (updates = 1; updates <= 8; ++updates) {
                need(ckpt::b1_config_hash(session.cfg) == config_hash, "native mask configuration changed");
                (void)session.train_step(first + updates - 1);
                backend::device_synchronize();
                if (const char* error = backend::last_error()) throw std::runtime_error(error);
                if (owner != 2) engine_b1_verify_context();
                engine_diagnostic_step_state(first + updates, true, &starting);
                if (updates == 1 || updates == 8) {
                    const size_t probe = updates == 1 ? 0 : 1;
                    auto actual = engine_diagnostic_capture(false);
                    if (arm == "A") reference_states[probe] = std::move(actual);
                    else invariants = engine_diagnostic_compare(starting, reference_states[probe], actual,
                        out, arm, updates, root, true, owner != 2) && invariants;
                }
            }
            engine_diagnostic_mask_probe({}, false, false);
            engine_diagnostic_end_arm();
            out.flush(); need(bool(out), "native mask comparison write");
        }
        engine_diagnostic_restore(baseline);
        engine_diagnostic_verify_start(baseline);
        session.st = run_state;
        out << "{\"type\":\"native_mask_complete\",\"restored_exact\":true,"
               "\"same_eight_native_inputs\":true,\"ownership_role_restored\":true,"
               "\"invariants\":" << (invariants ? "true" : "false")
            << ",\"numerical_deltas_descriptive\":true,\"quality_recovery_qualification\":false}\n";
        out.close(); need(bool(out), "native mask final comparison write");
        source_close(session);
        need(invariants, "native mask exact state/clock/context invariant failed; evidence retained");
    } catch (...) {
        engine_diagnostic_mask_probe({}, false, false);
        engine_diagnostic_restore(baseline);
        session.st = run_state;
        source_close(session);
        throw;
    }
}

void replay_identity(const Fields& fields, const char* label) {
    uint64_t digest = 14695981039346656037ULL;
    for (size_t field = 0; field < fields.size(); ++field) {
        const uint64_t size = fields[field].size();
        for (int j = 0; j < 8; ++j)
            digest = (digest ^ (uint8_t)(size >> (j * 8))) * 1099511628211ULL;
        digest = hash_bytes(fields[field], digest);
        std::printf("G1 replay_input label=%s field=%zu bytes=%llu hash=%llu\n",
            label, field, (unsigned long long)size,
            (unsigned long long)hash_bytes(fields[field]));
    }
    std::printf("G1 replay_identity label=%s fields=%zu hash=%llu\n",
        label, fields.size(), (unsigned long long)digest);
}

void source_replay(int argc, char** argv) {
    need(argc == 8 && std::strcmp(argv[2], "--resume") == 0 &&
         std::strcmp(argv[4], "--device") == 0 &&
         std::strcmp(argv[6], "--evidence") == 0, "FPBO replay arguments");
    spirula::TrainerSession session;
    source_setup(session, argv[3], argv[5]);
    try {
        const fs::path root = argv[7];
        need(fs::file_size(root / "identity.tsv") <= 65536, "oversized replay identity");
        std::ifstream metadata(root / "identity.tsv");
        need((bool)metadata, "missing replay identity");
        std::vector<std::pair<std::string, std::string>> identity;
        std::string line;
        while (std::getline(metadata, line)) {
            const size_t tab = line.find('\t');
            need(tab != std::string::npos, "malformed replay identity");
            const std::string key = line.substr(0, tab);
            need(std::none_of(identity.begin(), identity.end(),
                [&](const auto& item) { return item.first == key; }), "duplicate replay identity key");
            identity.emplace_back(key, line.substr(tab + 1));
        }
        need(metadata.eof(), "replay identity read failed");
        auto number = [&](const char* key) -> uint64_t {
            const auto item = std::find_if(identity.begin(), identity.end(),
                [&](const auto& entry) { return entry.first == key; });
            need(item != identity.end() && !item->second.empty() &&
                 item->second.find_first_not_of("0123456789") == std::string::npos,
                 "missing or invalid replay identity number");
            size_t used = 0;
            const auto value = std::stoull(item->second, &used);
            need(used == item->second.size(), "invalid replay identity number");
            return value;
        };
        need(number("source") == packet_source &&
             number("config_layout") == packet_layout &&
             number("active_stable_rows") == packet_topology &&
             number("capacity") == (uint64_t)engine().max_num_splats &&
             number("sh_bases") == (uint64_t)engine().num_sh &&
             number("canonical_tolerance_bytes") == 0, "checkpoint/evidence identity mismatch");
        proof_step = number("step");
        need(proof_step >= (uint64_t)session.start_step &&
             proof_step - (uint64_t)session.start_step < 6 &&
             proof_step < (uint64_t)std::numeric_limits<int>::max() &&
             number("version") == proof_step + 1, "replay source step/version mismatch");
        packet_attempt = number("attempt");
        need(packet_attempt != 0, "missing replay attempt");
        packet_first = 0; packet_rows = packet_topology;
        replay_context(session, (int)proof_step);
        const auto sc = engine_resolve_managed_step_config(
            spirula::build_step_config(session.cfg, session.st, (int)proof_step));
        const auto mode = (DensifyAccumMode)sc.loss.loss_map_accum_mode;
        if (sc.loss.compute_loss_map && accum_any(mode)) {
            engine().fwd.accum_weight.resize(PoolSlot::RasterBwdAccumWeight,
                engine().cur_num_splats * accum_lanes(mode));
            engine().fwd.accum_weight.zero();
            engine().fwd.accum_mode = mode;
        }
        const auto schema = source_state();
        const Fields before = receive(root / "preupdate-native-state-and-moments.packet",
            proof_step + 1, proof_step, 23, 2, schema.size());
        for (size_t field = 0; field < schema.size(); ++field) {
            need(before[field].size() == schema[field].size(), "replay preupdate native schema mismatch");
            const auto key = "field_" + std::to_string(field);
            const auto entry = std::find_if(identity.begin(), identity.end(),
                [&](const auto& item) { return item.first == key; });
            const std::string suffix = "\tbytes=" + std::to_string(schema[field].size()) +
                "\telement_bytes=" + std::to_string(source_element_bytes(field));
            need(entry != identity.end() && entry->second.size() >= suffix.size() &&
                 entry->second.compare(entry->second.size() - suffix.size(),
                     suffix.size(), suffix) == 0, "replay field identity mismatch");
        }
        replay_identity(before, "preupdate");
        const auto cfg = sc.optim;
        std::printf("G1 replay device=%s source=%llu config=%llu attempt=%llu step=%llu version=%llu repeat=2 gradient_scale=1 source_recapture=NONE bootstrap=zero_pixel_cotangents host_staging_cap=8589934592 ordered_raster=1\n",
            argv[5], (unsigned long long)packet_source, (unsigned long long)packet_layout,
            (unsigned long long)packet_attempt, (unsigned long long)proof_step,
            (unsigned long long)(proof_step + 1));
        // Two immutable gradient inputs; at most three output snapshots live.
        const uint64_t state_bytes = field_bytes(before, 0, before.size());
        const uint64_t gradient_bytes = (uint64_t)engine().max_num_splats * (14 + 3 * K) * 4;
        need(6 * state_bytes + 2 * gradient_bytes < 8ULL * 1024 * 1024 * 1024,
             "replay exceeds bounded host staging");
        const std::array<Fields, 2> gradients = {
            receive(root / "resident-data-gradients.packet",
                proof_step + 1, proof_step, 24, 2, 6),
            receive(root / "received-data-gradients.packet",
                proof_step + 1, proof_step, 25, 2, 6)};
        const bool producer_inputs_equal = replay_compare(gradients[0], gradients[1],
            "retained_resident_vs_received_data_gradients", false, true);
        bool repeats_equal = true, saved_equal = true;
        for (int variant = 0; variant < 2; ++variant) {
            const char* name = variant ? "received" : "resident";
            const Fields& grad = gradients[(size_t)variant];
            const Fields saved = receive(root / (std::string(name) + "-native-state.packet"),
                proof_step + 1, proof_step, variant ? 22 : 21, 2, before.size());
            replay_identity(grad, name);
            replay_identity(saved, variant ? "saved_received" : "saved_resident");
            Fields first;
            for (int repeat = 0; repeat < 2; ++repeat) {
                restore_source(before);
                load_gradient(grad);
                engine().optim.grad_scale = 1.f;
                engine_fpbo_step_from_data_gradients((int)proof_step, cfg);
                Fields actual = source_state();
                const std::string label = std::string(name) + "_repeat" +
                    std::to_string(repeat + 1) + "_vs_saved";
                saved_equal &= replay_compare(saved, actual, label.c_str(), true);
                if (repeat == 0) first = std::move(actual);
                else {
                    const std::string repeat_label = std::string(name) + "_repeat1_vs_repeat2";
                    repeats_equal &= replay_compare(first, actual, repeat_label.c_str(), true);
                }
            }
        }
        std::printf("G1 replay_summary same_input_updater_repeatable=%d saved_outputs_exact=%d retained_gradient_inputs_exact=%d independent_gradient_producer_variance=retained_input_comparison source_recapture=NONE gate2=NOT_WAIVED timing=BLOCKED\n",
            (int)repeats_equal, (int)saved_equal, (int)producer_inputs_equal);
        need(repeats_equal && saved_equal, "retained FPBO replay exact comparison failed");
    } catch (...) { source_close(session); throw; }
    source_close(session);
}

void source_producer_diagnostic(int argc, char** argv) {
    need(argc == 8 && std::strcmp(argv[2], "--resume") == 0 &&
         std::strcmp(argv[4], "--device") == 0 &&
         std::strcmp(argv[6], "--image") == 0, "FPBO producer diagnostic arguments");
    spirula::TrainerSession session;
    source_setup(session, argv[3], argv[5]);
    try {
        const auto ids = parse_images(argv[7], session.ds.num_cameras);
        need(ids.size() == 1, "producer diagnostic requires one source image");
        const int step = session.start_step;
        const auto sc = engine_resolve_managed_step_config(
            spirula::build_step_config(session.cfg, session.st, step));
        need(sc.loss.color_shift_reg_weight == 0.f,
             "producer diagnostic requires frozen color-shift EMA");
        replay_context(session, step);
        const Fields frozen = source_state();
        replay_identity(frozen, "producer_frozen_native_state");
        const int degree = std::min(session.cfg.sh_degree,
            step / std::max(session.cfg.sh_degree_warmup_every, 1));
        Fields first_pixels, first_screen, first_render;
        bool pixels_equal = true, screen_equal = true, render_equal = true;
        for (int repeat = 0; repeat < 2; ++repeat) {
            restore_source(frozen);
            engine().ppisp.cur_run_before_bilagrid = sc.ppisp.run_before_bilagrid;
            engine().ppisp.cur_run_before_color_space =
                sc.ppisp.run_before_color_space && engine().color_space.splat_enabled;
            engine().background.match_luma_pending = sc.background.match_luminance;
            int passes = 0;
            const int views = engine_preview_forward(
                ids[0], session.cfg.primitive, degree, true, true, sc.loss, 0, &passes);
            need(passes == 1 && views > 0, "producer diagnostic requires one face pass");
            const int h = engine().camera.height, w = engine().camera.width;
            const uint64_t pixels = (uint64_t)views * h * w;
            uint64_t screen_bytes = 0;
            for (const auto& tensor : engine().fwd.splats_s)
                screen_bytes += (uint64_t)tensor.numel() * sizeof(float);
            need(3 * field_bytes(frozen, 0, frozen.size()) + pixels * 4 * 32 +
                 3 * screen_bytes < 8ULL * 1024 * 1024 * 1024,
                 "producer diagnostic exceeds bounded host staging");
            Fields render{copy_device(std::get<0>(engine().fwd.renders).data_ptr(),
                (size_t)pixels * 3 * sizeof(float))};
            const Fields prior_stats = statistics();
            auto backward = [&] {
                return engine_compute_loss_backward(
                    step, sc.loss.weights, sc.loss.w_ssim, sc.loss.num_loss_scales,
                    sc.loss.loss_scale_min_pixels, sc.loss.compute_loss_map,
                    sc.loss.loss_map_mode, sc.loss.robust_edge_aware_quantile,
                    sc.loss.nms_falloff, sc.loss.loss_map_normalize,
                    sc.loss.loss_map_clip_quantile, sc.loss.loss_map_power,
                    sc.loss.loss_map_accum_mode, sc.loss.saturation_threshold,
                    sc.loss.overexposure_reg_weight, sc.loss.color_shift_reg_weight,
                    sc.loss.color_shift_reg_beta);
            };
            (void)backward();
            restore_statistics(prior_stats);
            (void)backward();
            // Raster consumes these slots read-only. Read them immediately
            // after native backward: these are its pre-raster input cotangents,
            // including the native appearance/background backward hooks.
            static_assert(slot_phase(PoolSlot::EngVRgb) == PoolPhase::None &&
                          slot_phase(PoolSlot::EngVDepth) == PoolPhase::None &&
                          slot_phase(PoolSlot::EngVTs) == PoolPhase::None,
                          "producer pixel readback requires non-aliased cotangent slots");
            Fields pixel;
            for (const auto slot : {PoolSlot::EngVRgb, PoolSlot::EngVDepth, PoolSlot::EngVTs}) {
                const size_t count = (size_t)pixels * (slot == PoolSlot::EngVRgb ? 3 : 1);
                const float* ptr = DevicePool::global().acquire<float>(slot, count);
                pixel.push_back(copy_device(ptr, count * sizeof(float)));
            }
            Fields screen;
            for (const auto& tensor : engine().fwd.v_splats_s)
                screen.push_back(copy_device(tensor.data_ptr(),
                    (size_t)tensor.numel() * sizeof(float)));
            need(!screen.empty(), "producer diagnostic missing native raster screen gradients");
            std::printf("G1 producer device=%s source=%llu config=%llu step=%d image=%d repeat=%d faces=%d width=%d height=%d pixel_fields=rgb,depth,Ts screen_fields=%zu source_path=native_loss_backward pixel_stage=raster_input_after_native_hooks pixel_readback=immediate_after_backward fpbo_update=NONE gradient_substitution=NONE random_background=off tile_skip=off topology=frozen appearance_update=frozen backward_stat_prime=1 ordered_raster=1\n",
                argv[5], (unsigned long long)packet_source, (unsigned long long)packet_layout,
                step, ids[0], repeat + 1, views, w, h, screen.size());
            if (repeat == 0) {
                first_pixels = std::move(pixel);
                first_screen = std::move(screen);
                first_render = std::move(render);
            } else {
                render_equal = replay_compare(first_render, render, "producer_render", false, true);
                pixels_equal = replay_compare(first_pixels, pixel, "producer_pixel_cotangents", false, true);
                screen_equal = replay_compare(first_screen, screen, "producer_raster_screen_gradients", false, true);
            }
        }
        restore_source(frozen);
        std::printf("G1 producer_summary render_exact=%d pixel_cotangents_exact=%d raster_screen_gradients_exact=%d first_divergence=%s same_input_updater=NOT_RUN gate2=NOT_WAIVED timing=BLOCKED\n",
            (int)render_equal, (int)pixels_equal, (int)screen_equal,
            !render_equal ? "forward_render" : !pixels_equal ? "pixel_cotangents" :
                !screen_equal ? "raster_screen_gradients" : "NONE_OBSERVED");
    } catch (...) { source_close(session); throw; }
    source_close(session);
}
void source_packet_range(int owner, size_t rows) {
    const size_t blocks = (rows + 255) / 256, split = blocks / 2;
    packet_first = owner == 0 ? 0 : split * 256;
    packet_rows = owner == 0 ? split * 256 : rows - split * 256;
}
void source_owner_mask(int owner) {
    const size_t blocks = (size_t)((engine().cur_num_splats + 255) / 256);
    const size_t split = blocks / 2;
    std::vector<uint32_t> mask(blocks);
    for (size_t b = 0; b < blocks; ++b) mask[b] = (uint32_t)((b >= split) == (owner == 1));
    need(mask.back() == (uint32_t)(owner == 1), "last partial block owner");
    need(engine().b1.owned_blocks.size() == (int64_t)blocks, "owner mask shape");
    upload(engine().b1.owned_blocks.data_ptr(),
           Bytes((const uint8_t*)mask.data(), (const uint8_t*)(mask.data() + mask.size())));
    engine().b1.owner = owner;
    source_packet_range(owner, (size_t)engine().cur_num_splats);
}
std::pair<size_t, size_t> source_block_range(size_t field, size_t bytes,
                                           size_t block, size_t active, size_t capacity) {
    need(active && capacity >= active && block < (active + 255) / 256, "owner block topology");
    const size_t max_blocks = (capacity + 255) / 256;
    const bool packed = field == 5 || field == 17 ||
        (field >= 6 && field <= 18 && field % 2 == 0);
    const size_t stride = packed ? bytes / max_blocks : bytes / capacity * 256;
    need(stride && (packed ? stride * max_blocks == bytes : bytes % capacity == 0),
         "owner block stride");
    const size_t lo = block * stride;
    const size_t count = packed ? stride : std::min(size_t{256}, active - block * 256) * (stride / 256);
    need(lo + count <= bytes, "owner block bounds");
    return {lo, count};
}
std::pair<size_t, size_t> source_owner_range(size_t field, size_t bytes,
                                           int owner, size_t active, size_t capacity) {
    need(owner == 0 || owner == 1, "owner projection identity");
    const size_t blocks = (active + 255) / 256, split = blocks / 2;
    const size_t begin = owner ? split : 0, end = owner ? blocks : split;
    if (begin == end) return {0, 0};
    const auto first = source_block_range(field, bytes, begin, active, capacity);
    const auto last = source_block_range(field, bytes, end - 1, active, capacity);
    return {first.first, last.first + last.second - first.first};
}
Fields project_source_owner(const Fields& state, int owner, size_t count,
                            size_t active, size_t capacity) {
    need(count <= 20 && state.size() >= count, "projection field count");
    Fields result;
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto range = source_owner_range(i, state[i].size(), owner, active, capacity);
        result.emplace_back(state[i].begin() + range.first,
                            state[i].begin() + range.first + range.second);
    }
    return result;
}
void install_source_projection(Fields& state, const Fields& projected, int owner,
                               size_t count, size_t active, size_t capacity, bool upload_peer = false) {
    need(projected.size() == count && count <= 20 && state.size() >= count, "projection schema");
    // Validate every slice before changing CPU or GPU state.
    for (size_t i = 0; i < count; ++i)
        need(projected[i].size() == source_owner_range(i, state[i].size(), owner, active, capacity).second,
             "projection slice length");
    void* world[7]{};
    if (upload_peer) {
        need(count == 7, "peer upload world schema");
        auto& w = engine().world;
        world[0] = w.means.data_ptr(); world[1] = w.quats.data_ptr();
        world[2] = w.scales.data_ptr(); world[3] = w.opacities.data_ptr();
        world[4] = w.features_dc.data_ptr();
        world[5] = w.features_sh_quant16_fpbo.packed_ptr();
        world[6] = w.features_sh_quant16_fpbo.bounds_ptr();
    }
    for (size_t i = 0; i < count; ++i) {
        const auto range = source_owner_range(i, state[i].size(), owner, active, capacity);
        std::copy(projected[i].begin(), projected[i].end(), state[i].begin() + range.first);
        if (upload_peer && range.second)
            backend::memcpy_sync((uint8_t*)world[i] + range.first, projected[i].data(),
                                 range.second, backend::MemcpyKind::HostToDevice);
    }
}
void merge_source_owner(Fields& base, const Fields& before,
                        const Fields& next, int owner,
                        size_t active, size_t capacity, bool frozen_validated = false) {
    need(base.size() >= 24 && next.size() == (frozen_validated ? 20 : base.size()) &&
         before.size() == base.size(), "source snapshot schema");
    const size_t blocks = (active + 255) / 256, split = blocks / 2;
    const size_t begin = (owner ? split : 0), end = (owner ? blocks : split);
    need(owner == 0 || owner == 1, "owner merge identity");
    for (size_t i = 0; i < next.size(); ++i) {
        need(base[i].size() == next[i].size(), "owner field shape");
        if (i >= 20) {
            compare(before[i], next[i], "frozen statistic changed during owner update");
            continue;
        }
        for (size_t b = 0; b < blocks; ++b) {
            const auto range = source_block_range(i, base[i].size(), b, active, capacity);
            const size_t lo = range.first, count = range.second;
            if (b >= begin && b < end)
                std::copy(next[i].begin() + lo, next[i].begin() + lo + count,
                          base[i].begin() + lo);
            else need(std::equal(before[i].begin() + lo, before[i].begin() + lo + count,
                                 next[i].begin() + lo),
                      "unowned FPBO block changed");
        }
        if (resident_owned()) {
            const auto last = source_block_range(i, base[i].size(), blocks - 1, active, capacity);
            const size_t tail = last.first + last.second;
            need(std::equal(before[i].begin() + tail, before[i].end(), next[i].begin() + tail),
                 "unowned capacity padding changed");
        }
    }
}
void packet_projection_check() {
    const auto previous_transport = state_transport;
    state_transport = "resident-owned";
    constexpr size_t active = 576, capacity = 768;
    Fields before(24);
    for (size_t i = 0; i < before.size(); ++i) {
        const bool packed = i == 5 || i == 17 || (i >= 6 && i <= 18 && i % 2 == 0);
        before[i].resize(i >= 20 ? 4 : packed ? 3 * 16 : capacity * 4, 1);
    }
    for (int owner = 0; owner < 2; ++owner) {
        Fields next = before, merged = before, projected = project_source_owner(before, owner, 20, active, capacity);
        for (size_t i = 0; i < projected.size(); ++i) std::fill(projected[i].begin(), projected[i].end(), 2);
        install_source_projection(next, projected, owner, 20, active, capacity);
        merge_source_owner(merged, merged, next, owner, active, capacity);
        compare(next, merged, "owned projection alias merge exact");
        const auto unowned = source_owner_range(19, next[19].size(), 1 - owner, active, capacity);
        next[19][unowned.first] ^= 1;
        bool unowned_rejected = false;
        try { merge_source_owner(merged, merged, next, owner, active, capacity); }
        catch (const std::runtime_error&) { unowned_rejected = true; }
        need(unowned_rejected, "unowned native clock mutation accepted before projection");
        next[19][unowned.first] ^= 1;
        projected.back().push_back(3);
        bool rejected = false;
        try { install_source_projection(merged, projected, owner, 20, active, capacity); }
        catch (const std::runtime_error&) { rejected = true; }
        need(rejected, "malformed projected clock slice accepted");
        compare(next, merged, "malformed projection has no partial install");
    }
    state_transport = previous_transport;
}
constexpr int EdgeRows = 576;
OptimConfig edge_config() {
    auto c = optim();
    c.use_fused_proj_bwd_optim = true;
    c.quantization_level = 1;
    return c;
}
void edge_setup(const std::string& device) {
    need(device.rfind("uuid:", 0) == 0 &&
         backend::device_select_identity(device.c_str()), "explicit edge device UUID unavailable");
    initialize(0, EdgeRows, true);
    vkk::g1_set_ordered_raster(true);
    std::printf("G1 edge_native_setup device=%s ordered_raster=1\n", device.c_str());
    engine().optim.accum_buffer.zero();
    engine().optim.grad_scale = 1.f;
    engine().optim.zero_grad_in_optim = false;
    engine().b1.owner = -1;
    engine().b1.owned_blocks.resize_dynamic(VramCategory::Other, "g1.edges.owned_blocks",
                                           (EdgeRows + 255) / 256);
    engine().b1.source_id = Source;
    packet_source = Source;
    packet_layout = 0x4650424f53483301ULL;
    packet_topology = EdgeRows;
}

void refresh_proof(const std::string& device, const std::string& checkpoint = "") {
    spirula::TrainerSession session;
    const bool actual_source = !checkpoint.empty();
    const auto started = Clock::now();
    const fs::path root = fs::temp_directory_path() /
        ("b1-refresh-" + std::to_string(Clock::now().time_since_epoch().count()));
    try {
        if (actual_source) source_setup(session, checkpoint, device);
        else edge_setup(device);
        fs::create_directory(root);
        auto& s = engine();
        const int64_t start_step = actual_source ? session.start_step : 0;
        const int64_t boundary = start_step + 1;
        const size_t active = (size_t)s.cur_num_splats;
        const size_t blocks = (active + 255) / 256;
        const fs::path source = actual_source ? fs::path(checkpoint) : root / "source.ckpt";
        if (!actual_source) {
            s.primitive = "3dgs";
            s.packed = true;
            auto means = copy_device(s.world.means.data_ptr(), active * sizeof(float3));
            const float inactive = std::numeric_limits<float>::quiet_NaN();
            std::memcpy(means.data() + (active - 1) * sizeof(float3), &inactive, sizeof inactive);
            upload(s.world.means.data_ptr(), means);
            engine_save_checkpoint(source.string(), true, 0);
            std::ofstream config(source / "config.json");
            config << "{\"num_iterations\":10}";
        }
        const auto slots = DevicePool::global().saved(SaveClass::Resume);
        std::vector<void*> pointers = {s.world.means.data_ptr(), s.world.quats.data_ptr(),
            s.world.scales.data_ptr(), s.world.opacities.data_ptr(), s.world.features_dc.data_ptr(),
            s.world.features_sh_quant16_fpbo.packed_ptr(),
            s.world.features_sh_quant16_fpbo.bounds_ptr()};
        uint64_t saved_bytes = 0, largest_slot = 0;
        for (const auto& slot : slots) {
            saved_bytes += slot.nbytes;
            largest_slot = std::max(largest_slot, (uint64_t)slot.nbytes);
        }
        need(saved_bytes + largest_slot + (uint64_t{1} << 30) < 8ULL * 1024 * 1024 * 1024,
             "refresh proof exceeds bounded host scratch");
        Fields original;
        for (const auto& slot : slots) original.push_back(copy_device(slot.ptr, slot.nbytes));
        std::vector<size_t> render_slots;
        for (const auto ptr : pointers) {
            const auto slot = std::find_if(slots.begin(), slots.end(),
                [&](const auto& entry) { return entry.ptr == ptr; });
            need(slot != slots.end(), "refresh render field missing from saved slots");
            render_slots.push_back((size_t)(slot - slots.begin()));
        }
        size_t inactive_rows = 0;
        const auto& means = original[render_slots[0]];
        for (size_t row = 0; row < active; ++row) {
            float3 mean;
            std::memcpy(&mean, means.data() + row * sizeof mean, sizeof mean);
            inactive_rows += !std::isfinite(mean.x) || !std::isfinite(mean.y) || !std::isfinite(mean.z);
        }
        auto restore = [&]() {
            for (size_t index = 0; index < slots.size(); ++index)
                upload(const_cast<void*>(slots[index].ptr), original[index]);
        };
        try {
            auto configure = [&](int owner) {
                engine_b1_configure(owner, source.string(), start_step);
                if (actual_source)
                    engine_b1_bind_contract(session.cfg, "", session.ds.train_indices,
                                            source.string(), "full", session.ds.train_indices);
                auto& c = *s.b1.contract;
                c.version = 3; c.view_policy = "full"; c.refresh_cadence = 1;
                c.refresh_generation = c.refresh_parent = start_step;
                c.refresh_group_sha256 = c.refresh_commit_sha256 = std::string(64, 'a');
                c.refresh_payload0_sha256 = c.refresh_payload1_sha256 = std::string(64, '0');
                if (!actual_source) c.full_train_indices_sha256 = ckpt::b1_indices_hash({0, 1});
                need(boundary <= c.horizon, "refresh source has no next completed boundary");
            };
            configure(1);
            std::vector<uint32_t> peer_mask(blocks);
            auto read_mask = [&]() {
                backend::memcpy_sync(peer_mask.data(), s.b1.owned_blocks.data_ptr(),
                                     peer_mask.size() * sizeof(uint32_t),
                                     backend::MemcpyKind::DeviceToHost);
                if (const char* error = backend::last_error()) throw std::runtime_error(error);
            };
            read_mask();
            int peer_owner = 1;
            if (!peer_mask.back()) {
                peer_owner = 0;
                configure(peer_owner);
                read_mask();
            }
            need(peer_mask.back() == 1, "refresh tail must belong to peer");
            size_t peer_inactive = 0;
            for (size_t row = 0; row < active; ++row) {
                float3 mean;
                std::memcpy(&mean, means.data() + row * sizeof mean, sizeof mean);
                if (peer_mask[row / 256])
                    peer_inactive += !std::isfinite(mean.x) || !std::isfinite(mean.y) || !std::isfinite(mean.z);
            }
            auto xor_peer = [&](Bytes& bytes, size_t field) {
                const bool block_field = field >= 5;
                const size_t rows = block_field ? (size_t)s.world.features_sh_quant16_fpbo.n_bounds :
                                                 (size_t)s.world.means.size();
                need(rows && bytes.size() % rows == 0, "refresh native field stride");
                const size_t stride = bytes.size() / rows;
                for (size_t block = 0; block < blocks; ++block) {
                    if (!peer_mask[block]) continue;
                    const size_t first = block_field ? block : block * 256;
                    const size_t count = block_field ? 1 : std::min(size_t{256}, active - first);
                    for (size_t byte = first * stride; byte < (first + count) * stride; ++byte)
                        bytes[byte] ^= 1;
                }
            };
            Bytes scratch;
            // Raw XOR bytes discriminate installation only; never render or train this state.
            for (size_t field = 0; field < pointers.size(); ++field) {
                scratch = original[render_slots[field]];
                xor_peer(scratch, field);
                upload(pointers[field], scratch);
            }
            scratch = Bytes{};
            const auto peer = root / "peer.payload";
            const auto peer_hash = engine_b1_export_refresh(peer.string(), boundary);
            restore();
            configure(1 - peer_owner);
            const auto own = root / "own.payload";
            const auto own_hash = engine_b1_export_refresh(own.string(), boundary);
            auto check_saved = [&](bool installed) {
                for (size_t index = 0; index < slots.size(); ++index) {
                    scratch.resize(slots[index].nbytes);
                    if (!scratch.empty())
                        backend::memcpy_sync(scratch.data(), slots[index].ptr, scratch.size(),
                                             backend::MemcpyKind::DeviceToHost);
                    if (const char* error = backend::last_error()) throw std::runtime_error(error);
                    const auto render = std::find(render_slots.begin(), render_slots.end(), index);
                    if (installed && render != render_slots.end())
                        xor_peer(scratch, (size_t)(render - render_slots.begin()));
                    need(scratch == original[index],
                         "refresh saved state mismatch (owned/non-render/padding/peer)");
                }
                scratch = Bytes{};
            };
            auto rejects = [&](const fs::path& path, int64_t step, const std::string& hash) {
                const auto frozen = s.b1.frozen_hash;
                const auto pending = s.b1.refresh_pending_boundary;
                const auto exported = s.b1.refresh_export_sha256;
                const auto installed = s.b1.refresh_installed_sha256;
                bool rejected = false;
                try { engine_b1_install_refresh(path.string(), step, hash); }
                catch (const std::runtime_error&) { rejected = true; }
                need(rejected, "invalid refresh payload accepted");
                check_saved(false);
                need(s.b1.frozen_hash == frozen && s.b1.refresh_pending_boundary == pending &&
                     s.b1.refresh_export_sha256 == exported && s.b1.refresh_installed_sha256 == installed,
                     "refused refresh changed context or phase");
            };
            rejects(peer, boundary + 1, peer_hash);
            const auto parent_commit = s.b1.contract->refresh_commit_sha256;
            s.b1.contract->refresh_commit_sha256 = std::string(64, 'd');
            rejects(peer, boundary, peer_hash);
            s.b1.contract->refresh_commit_sha256 = parent_commit;
            rejects(peer, boundary, std::string(64, 'b'));
            const auto partial = root / "partial.payload";
            fs::copy_file(peer, partial);
            fs::resize_file(partial, fs::file_size(partial) - 1);
            rejects(partial, boundary, ckpt::b1_file_hash(partial));
            engine_b1_install_refresh(peer.string(), boundary, peer_hash);
            check_saved(true);
            const auto payload0 = peer_owner == 0 ? peer_hash : own_hash;
            const auto payload1 = peer_owner == 1 ? peer_hash : own_hash;
            const auto commit = std::string(64, 'c');
            engine_b1_commit_refresh(boundary, payload0, payload1, commit);
            engine_b1_verify_context();
            const auto& c = *s.b1.contract;
            need(c.refresh_generation == boundary && c.refresh_parent == start_step &&
                 c.refresh_commit_sha256 == commit && c.refresh_payload0_sha256 == payload0 &&
                 c.refresh_payload1_sha256 == payload1 && c.frozen_context == s.b1.frozen_hash &&
                 s.b1.refresh_pending_boundary == -1 && s.b1.refresh_export_sha256.empty() &&
                 s.b1.refresh_installed_sha256.empty(), "refresh lineage/context did not commit");
            std::printf("B1_REFRESH native proof PASS mode=%s rows=%zu capacity=%lld blocks=%zu tail=%zu inactive=%zu peer_inactive=%zu peer_blocks=%zu start=%lld boundary=%lld horizon=%lld train_indices=%zu saved_bytes=%llu peer_payload_bytes=%llu own_payload_bytes=%llu seconds=%.6f peer_exact=1 owned_exact=1 nonrender_exact=1 padding_exact=1 refusal_full_saved_exact=1 stale_hash_parent_truncated_refused=1 commit_context_exact=1 proof=raw_source_native_install trained_trajectory=NOT_PROVED optimizer_updates=0 synthetic_rerender=NONE\n",
                actual_source ? "retained_source" : "tiny_fixture", active,
                (long long)s.max_num_splats, blocks, (active - 1) % 256 + 1,
                inactive_rows, peer_inactive, (size_t)std::count(peer_mask.begin(), peer_mask.end(), 1u),
                (long long)start_step, (long long)boundary, (long long)c.horizon,
                actual_source ? session.ds.train_indices.size() : size_t{2},
                (unsigned long long)saved_bytes, (unsigned long long)fs::file_size(peer),
                (unsigned long long)fs::file_size(own),
                std::chrono::duration<double>(Clock::now() - started).count());
            restore();
            check_saved(false);
        } catch (...) {
            restore();
            throw;
        }
        engine_b1_configure(-1, "", 0);
        engine_reset();
        if (actual_source) source_close(session);
        fs::remove_all(root);
    } catch (...) {
        engine_reset();
        if (actual_source) source_close(session);
        fs::remove_all(root);
        throw;
    }
}
ImageResult edge_capture(int id, int step) {
    const View v = view(id);
    set_camera_params(v.w, v.h, "PINHOLE", "NONE",
        tv(v.matrix.data(), 4, {1,4,4}), tv(v.intr.data(), 4, {1,4}),
        tv(v.distortion.data(), 4, {1,8}));
    set_training_data(tv(v.gt.data(), 1, {1,v.h,v.w,3}), nil(), nil(), nil());
    engine().optim.use_fused_proj_bwd_optim = true;
    engine().optim.skip_grad_zero = false;
    engine().grad.quantize_grad = false;
    forward_3dgs("3dgs", 3, true);
    ImageResult result;
    result.image = id; result.faces = 1; result.width = v.w; result.height = v.h;
    result.render.resize((size_t)v.h * v.w * 3 * sizeof(float));
    engine_copy_render_to_host(tv(result.render.data(), 4, {1,v.h,v.w,3}),
                              nil(), nil(), nil(), nil());
    std::array<float, (int)LossWeightIndex::length> weights{};
    weights[(int)LossWeightIndex::RgbSupL1] = 1.f;
    const auto stats = statistics();
    (void)engine_compute_loss_backward(step, weights, 0.f, 1, 0,
        false, 0, 0.f, 0.f, false, 0.f, 1.f, 0, 0.f);
    restore_statistics(stats);
    const auto losses = engine_compute_loss_backward(step, weights, 0.f, 1, 0,
        false, 0, 0.f, 0.f, false, 0.f, 1.f, 0, 0.f);
    engine_fpbo_capture_data_gradients(step, edge_config());
    result.grad = gradient();
    for (const auto& [key, value] : losses) result.loss.push_back(value);
    return result;
}
Bytes image_identity(int image, int faces, int width, int height) {
    const std::array<int64_t, 5> values = {image, faces, width, height,
                                         engine().cur_num_splats};
    return Bytes((const uint8_t*)values.data(),
                 (const uint8_t*)(values.data() + values.size()));
}
Bytes captured_loss_bytes(const ImageResult& result) {
    Bytes loss(result.loss.size() * sizeof(float));
    std::memcpy(loss.data(), result.loss.data(), loss.size());
    return loss;
}
void append_capture(Fields& output, const ImageResult& result) {
    output.insert(output.end(), result.grad.begin(), result.grad.end());
    output.push_back(result.render);
    output.push_back(captured_loss_bytes(result));
    output.push_back(image_identity(result.image, result.faces, result.width, result.height));
}
void source_worker(int argc, char** argv) {
    need(argc == 10, "source worker arguments");
    const bool image_worker = std::strcmp(argv[1], "source-grad") == 0;
    need(image_worker || std::strcmp(argv[1], "source-owner") == 0, "source worker mode");
    const int step = std::stoi(argv[5]), owner = std::stoi(argv[6]);
    const int image = std::stoi(argv[8]);
    packet_attempt = std::stoull(argv[9]);
    need(packet_attempt != 0, "missing trusted worker attempt");
    const bool edges = std::strcmp(argv[7], "@fpbo-edges") == 0;
    need(owner == 0 || owner == 1, "invalid owner");
    spirula::TrainerSession session;
    if (edges) edge_setup(argv[2]);
    else source_setup(session, argv[7], argv[2]);
    proof_root = fs::path(argv[4]).parent_path();
    proof_step = (uint64_t)step;
    try {
    need(step >= (edges ? 0 : session.start_step) && image >= -1 &&
         image < (edges ? Views : session.ds.num_cameras), "invalid worker step/image");
    auto capture = [&](int id) {
        return edges ? edge_capture(id, step) : source_capture(session, id, step);
    };
    // Allocate forward/statistic buffers even on a process-fresh empty worker.
    (void)capture(image < 0 ? (edges ? 1 : 1188) : image);
    source_packet_range(owner, (size_t)engine().cur_num_splats);
    const size_t count = source_state().size();
    const auto incoming = receive(argv[3], (uint64_t)step + 1, step,
        image_worker ? 13 : 11, owner, count + (image_worker ? 0 : 6));
    const Fields before(incoming.begin(), incoming.begin() + count);
    restore_source(before);
    Fields output;
    if (image_worker) {
        engine().b1.owner = -1; // Full-world native depth/tile ordering, not shard RGBA.
        ImageResult observed;
        if (image >= 0) observed = capture(image);
        const auto captured = source_state();
        report_source(Fields(before.begin(), before.begin() + 20),
                      Fields(captured.begin(), captured.begin() + 20),
                      "image_capture_parameters_optimizer_bias_unchanged");
        restore_source(before); // Frozen appearance and all statistics, including loss priming.
        output = source_state();
        report_source(before, output, "image_worker_canonical_unchanged");
        if (image >= 0) append_capture(output, observed);
        else output.push_back(image_identity(-1, 0, 0, 0));
    } else {
        (void)capture(image < 0 ? (edges ? 1 : 1188) : image);
        restore_source(before);
        source_owner_mask(owner);
        load_gradient(Fields(incoming.begin() + count, incoming.end()));
        engine().optim.grad_scale = 1.f;
        engine_fpbo_step_from_data_gradients(step, edges ? edge_config() :
            engine_resolve_managed_step_config(
                spirula::build_step_config(session.cfg, session.st, step)).optim);
        output = source_state();
    }
    packet(argv[4], (uint64_t)step + 1, step, image_worker ? 14 : 12, owner, output);
    if (!image_worker) {
        const fs::path native_dir = std::string(argv[4]) + ".native";
        fs::remove_all(native_dir);
        save_canonical_snapshot(native_dir, step + 1);
        report_source(output, source_state(), "native_owner_artifact_write_unchanged");
    }
    if (!edges) source_close(session);
    } catch (...) {
        if (!edges) source_close(session);
        throw;
    }
}
#ifdef _WIN32
Child source_launch(const char* exe, const char* device, const fs::path& in,
                    const fs::path& out, int step, int owner,
                    const char* checkpoint, int image, const char* mode = "source-owner") {
    const std::string s = std::to_string(step), o = std::to_string(owner),
                      id = std::to_string(image), attempt = std::to_string(packet_attempt);
    Child pid = _spawnl(_P_NOWAIT, exe, exe, mode, device,
                        in.string().c_str(), out.string().c_str(), s.c_str(),
                        o.c_str(), checkpoint, id.c_str(), attempt.c_str(), nullptr);
    need(pid != -1, "source owner spawn failed");
    return pid;
}
#else
Child source_launch(const char* exe, const char* device, const fs::path& in,
                    const fs::path& out, int step, int owner,
                    const char* checkpoint, int image, const char* mode = "source-owner") {
    Child pid = fork();
    need(pid >= 0, "source owner fork failed");
    if (pid == 0) {
        const std::string s = std::to_string(step), o = std::to_string(owner),
                          id = std::to_string(image), attempt = std::to_string(packet_attempt);
        execl(exe, exe, mode, device, in.string().c_str(),
              out.string().c_str(), s.c_str(), o.c_str(), checkpoint,
              id.c_str(), attempt.c_str(), (char*)nullptr);
        _exit(127);
    }
    return pid;
}
#endif
void scale_gradients(Fields& b, float scale) {
    for (auto& field : b)
        for (size_t i = 0; i < field.size(); i += sizeof(float)) {
            float v;
            std::memcpy(&v, field.data() + i, sizeof(v));
            v *= scale;
            std::memcpy(field.data() + i, &v, sizeof(v));
        }
}
void compare_captured(const ImageResult& ref, const Fields& fields, size_t offset) {
    need(fields.size() >= offset + 8, "source capture fields missing");
    compare(ref.grad, Fields(fields.begin() + offset, fields.begin() + offset + 6),
            "native source float data gradients", 0.0003f);
    compare(ref.render, fields[offset + 6], "native source render", 0.0003f);
    compare(captured_loss_bytes(ref), fields[offset + 7], "native source loss", 0.0003f);
    if (fields.size() > offset + 8)
        compare(image_identity(ref.image, ref.faces, ref.width, ref.height),
                fields[offset + 8], "source image/face/dimension/stable-row identity");
}
struct SourceTraffic {
    uint64_t worker_input = 0, worker_gradient = 0, worker_render_loss_identity = 0;
    uint64_t worker_context_return = 0, worker_shared_stat_return = 0;
    uint64_t owner_native_artifact = 0;
    uint64_t owner_input = 0, owner_return = 0, owner_shared_stat_return = 0;
    uint64_t files = 0;
};
template<class Capture>
Fields source_image_phase(const char* exe, const DevicePair& devices,
                          const char* checkpoint, const fs::path& root, int step,
                          const std::vector<int32_t>& images, const Fields& before,
                          Capture capture, SourceTraffic& traffic, Fields& reference_reduced,
                          bool inject_worker_failure = false) {
    need(!images.empty() && images.size() <= 3, "bounded image batch required");
    const size_t active = (size_t)engine().cur_num_splats;
    Fields resident_sum, received_sum;
    std::set<std::string> accepted;
    for (size_t begin = 0; begin < images.size(); begin += 2) {
        const fs::path in[2] = {root / "image-in0", root / "image-in1"};
        const fs::path out[2] = {root / "image-out0", root / "image-out1"};
        int ids[2] = {images[begin], begin + 1 < images.size() ? images[begin + 1] : -1};
        for (int owner = 0; owner < 2; ++owner) {
            source_packet_range(owner, active);
            fs::remove(out[owner]);
            packet(in[owner], (uint64_t)step + 1, step, 13, owner, before);
            traffic.worker_input += field_bytes(before, 0, before.size());
            traffic.files += fs::file_size(in[owner]);
        }
        if (inject_worker_failure) fs::resize_file(in[1], sizeof(Header) + 1);
        const Child a = source_launch(exe, devices[0].c_str(), in[0], out[0],
                                       step, 0, checkpoint, ids[0], "source-grad");
        Child b;
        try { b = source_launch(exe, devices[1].c_str(), in[1], out[1],
                                 step, 1, checkpoint, ids[1], "source-grad"); }
        catch (...) { join(a); throw; }
        const bool a_ok = join(a), b_ok = join(b);
        need(a_ok && b_ok, "image worker failed: next version publication forbidden");
        // Receive and reduce in declared image order, never completion order.
        for (int owner = 0; owner < 2; ++owner) {
            source_packet_range(owner, active);
            const auto reply = receive_once(out[owner], (uint64_t)step + 1, step,
                14, owner, before.size() + (ids[owner] < 0 ? 1 : 9), accepted, ids[owner]);
            traffic.files += fs::file_size(out[owner]);
            traffic.worker_context_return += field_bytes(reply, 0, 20);
            traffic.worker_shared_stat_return += field_bytes(reply, 20, before.size());
            report_source(before, Fields(reply.begin(), reply.begin() + before.size()),
                          "worker_frozen_context_return");
            if (ids[owner] < 0) {
                compare(image_identity(-1, 0, 0, 0), reply.back(), "empty worker identity");
                traffic.worker_render_loss_identity += reply.back().size();
            } else {
                restore_source(before);
                const auto reference = capture(ids[owner]);
                restore_source(before);
                compare_captured(reference, reply, before.size());
                add(resident_sum, reference.grad);
                const Fields grad(reply.begin() + before.size(),
                                  reply.begin() + before.size() + 6);
                add(received_sum, grad);
                traffic.worker_gradient += field_bytes(grad, 0, grad.size());
                traffic.worker_render_loss_identity += field_bytes(reply, before.size() + 6,
                                                                    reply.size());
            }
            fs::remove(in[owner]); fs::remove(out[owner]);
        }
    }
    scale_gradients(resident_sum, 1.f / (float)images.size());
    scale_gradients(received_sum, 1.f / (float)images.size());
    compare(resident_sum, received_sum, "equal-image-weight ordered gradient reduction", 0.0003f);
    reference_reduced = std::move(resident_sum);
    return received_sum;
}
Fields source_owner_phase(const char* exe, const DevicePair& devices,
                          const char* checkpoint, const fs::path& root, int step, int image,
                          const Fields& before, const Fields& reduced,
                          const Fields& resident, SourceTraffic& traffic) {
    const size_t active = (size_t)engine().cur_num_splats;
    const size_t capacity = (size_t)engine().max_num_splats;
    const fs::path in[2] = {root / "owner-in0", root / "owner-in1"};
    const fs::path out[2] = {root / "owner-out0", root / "owner-out1"};
    for (int owner = 0; owner < 2; ++owner) {
        source_packet_range(owner, active);
        fs::remove(out[owner]);
        Fields request = before;
        request.insert(request.end(), reduced.begin(), reduced.end());
        packet(in[owner], (uint64_t)step + 1, step, 11, owner, request);
        traffic.owner_input += field_bytes(request, 0, request.size());
        traffic.files += fs::file_size(in[owner]);
    }
    const Child a = source_launch(exe, devices[0].c_str(), in[0], out[0],
                                   step, 0, checkpoint, image);
    Child b;
    try { b = source_launch(exe, devices[1].c_str(), in[1], out[1],
                             step, 1, checkpoint, image); }
    catch (...) { join(a); throw; }
    const bool a_ok = join(a), b_ok = join(b);
    need(a_ok && b_ok, "owner failed: incomplete version cannot publish");
    Fields merged = before;
    std::set<std::string> accepted;
    for (int owner = 0; owner < 2; ++owner) {
        source_packet_range(owner, active);
        const auto reply = receive_once(out[owner], (uint64_t)step + 1, step,
                                        12, owner, before.size(), accepted);
        traffic.files += fs::file_size(out[owner]);
        traffic.owner_return += field_bytes(reply, 0, 20);
        const fs::path native_dir = out[owner].string() + ".native";
        traffic.owner_native_artifact += fs::file_size(native_dir / "state.tar") +
                                         fs::file_size(native_dir / "splat.ply");
        traffic.owner_shared_stat_return += field_bytes(reply, 20, reply.size());
        merge_source_owner(merged, before, reply, owner, active, capacity);
        fs::remove(in[owner]);
    }
    need(accepted.size() == 2, "incomplete version publication refused");
    report_source(resident, merged, "same_input_resident_vs_two_owner",
                  &before, &reduced, &reduced);
    return merged;
}
template<class Capture>
void source_stop_readback(const char* exe, const DevicePair& devices,
                          const char* checkpoint, const fs::path& root, int boundary,
                          const Fields& canonical, int image, Capture capture) {
    const size_t active = (size_t)engine().cur_num_splats;
    packet_first = 0; packet_rows = active;
    std::vector<uint32_t> row_ids(active);
    std::iota(row_ids.begin(), row_ids.end(), 0);
    Fields artifact = canonical;
    artifact.emplace_back((const uint8_t*)row_ids.data(),
                          (const uint8_t*)(row_ids.data() + row_ids.size()));
    const fs::path published = root / "completed-native-state";
    packet(published, boundary, boundary - 1, 15, 2, artifact);
    const uint64_t completed_hash = hash_file(published);
    // Exercise the same coordinator acceptance gate with one successful child
    // and one process-fresh child rejecting a truncated next-version input.
    SourceTraffic failure_traffic;
    Fields unused;
    bool failed_before_publication = false;
    try {
        (void)source_image_phase(exe, devices, checkpoint, root, boundary,
            std::vector<int32_t>{image}, canonical, capture, failure_traffic, unused, true);
    } catch (const std::runtime_error& e) {
        failed_before_publication = std::strcmp(e.what(),
            "image worker failed: next version publication forbidden") == 0;
    }
    need(failed_before_publication && fs::exists(root / "image-out0") &&
         !fs::exists(root / "image-out1") && hash_file(published) == completed_hash,
         "worker failure acceptance/publication gate failed");
    packet_first = 0; packet_rows = active;
    const auto readback = receive(published, boundary, boundary - 1, 15, 2,
                                  canonical.size() + 1);
    need(readback.back().size() == active * sizeof(uint32_t), "artifact source row count");
    for (size_t row = 0; row < active; ++row) {
        uint32_t id;
        std::memcpy(&id, readback.back().data() + row * sizeof(id), sizeof(id));
        need(id == row, "artifact stable source row identity");
    }
    restore_source(Fields(readback.begin(), readback.begin() + canonical.size()));
    report_source(canonical, source_state(), "completed_native_artifact_readback");
    for (int owner = 0; owner < 2; ++owner) {
        source_packet_range(owner, active);
        const auto expected = receive(root / ("owner-out" + std::to_string(owner)),
                                      boundary, boundary - 1, 12, owner, canonical.size());
        const fs::path native_dir = root / ("owner-out" + std::to_string(owner) + ".native");
        const float poison = 12345.f;
        backend::memcpy_sync(engine().world.means.data_ptr(), &poison, sizeof(poison),
                             backend::MemcpyKind::HostToDevice);
        need(engine_load_checkpoint(native_dir.string()) == boundary,
             "completed native owner checkpoint step mismatch");
        need((size_t)engine().cur_num_splats == active &&
             engine().world.features_sh_quant16_fpbo.initialized(),
             "native owner readback source/layout identity mismatch");
        report_source(expected, source_state(), "completed_native_owner_checkpoint_readback");
    }
    restore_source(canonical);
    for (const char* name : {"image-in0", "image-in1", "image-out0", "image-out1"})
        fs::remove(root / name);
    std::printf("G1 worker_failure=observed next_version_publication=refused completed_boundary=%d source_and_stable_ids=validated native_artifact_readback=pass globally_merged_resumable_checkpoint=NO artifact_root=%s\n",
                boundary, root.string().c_str());
}
#ifdef _WIN32
std::string timing_quote(const std::string& value) {
    // _spawnl concatenates argv on Windows; quote whitespace and trailing slashes.
    std::string result = "\"";
    size_t slashes = 0;
    for (char c : value) {
        if (c == '\\') { ++slashes; continue; }
        result.append(c == '"' ? slashes * 2 + 1 : slashes, '\\');
        result += c;
        slashes = 0;
    }
    result.append(slashes * 2, '\\');
    return result + '"';
}
double timing_seconds(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}
uint64_t timing_tree_bytes(const fs::path& root) {
    uint64_t bytes = 0;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        std::error_code error;
        const bool regular = e.is_regular_file(error);
        if (error == std::errc::no_such_file_or_directory) continue;
        need(!error, "scratch file status query failed");
        if (!regular) continue;
        const auto size = e.file_size(error);
        if (error == std::errc::no_such_file_or_directory) continue;
        need(!error, "scratch file size query failed");
        bytes += size;
    }
    return bytes;
}
// Proof-local events carry commands only; tensors retain validated binary packets.
struct TimingEvents {
    HANDLE request = nullptr, done = nullptr;
    TimingEvents(uint64_t attempt, int owner, bool create) {
        const std::string name = "Local\\SpirulaG1Timing-" +
            std::to_string(attempt) + "-" + std::to_string(owner);
        request = create ? CreateEventA(nullptr, FALSE, FALSE, (name + "-request").c_str()) :
            OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, (name + "-request").c_str());
        done = create ? CreateEventA(nullptr, FALSE, FALSE, (name + "-done").c_str()) :
            OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, (name + "-done").c_str());
        if (!request || !done) {
            if (request) CloseHandle(request);
            if (done) CloseHandle(done);
            throw std::runtime_error("timing event creation/open failed");
        }
    }
    ~TimingEvents() { CloseHandle(request); CloseHandle(done); }
    TimingEvents(const TimingEvents&) = delete;
    TimingEvents& operator=(const TimingEvents&) = delete;
};
void timing_device_sample(std::ofstream& out, Clock::time_point start,
                          const char* phase, int step) {
    const auto m = backend::memory_usage();
    out << timing_seconds(start) << '\t' << phase << '\t' << step << '\t'
        << m.has_process << '\t' << m.process_bytes << '\t'
        << m.has_used << '\t' << m.used_bytes << '\t'
        << m.has_total << '\t' << m.total_bytes << '\n';
    out.flush();
    need((bool)out, "device telemetry write failed");
}
void timing_worker(int argc, char** argv) {
    need(argc == 11, "persistent timing worker arguments");
    state_transport = argv[9];
    need(state_transport == "full" || resident_owned(), "state transport must be full or resident-owned");
    select_packet_hash(argv[10]);
    const int owner = std::stoi(argv[5]);
    need(owner == 0 || owner == 1, "persistent timing owner");
    packet_attempt = std::stoull(argv[6]);
    const uint64_t map_capacity = std::stoull(argv[8]);
    need(map_capacity <= 1024ULL * 1024 * 1024 &&
         (std::strcmp(argv[7], "resource") == 0 || std::strcmp(argv[7], "benchmark") == 0),
         "invalid persistent worker staging/profile mode");
    std::unique_ptr<TimingMapping> mapping;
    if (map_capacity) mapping = std::make_unique<TimingMapping>(packet_attempt, owner, map_capacity, false);
    const bool resource = std::strcmp(argv[7], "resource") == 0;
    const fs::path root = argv[4], base = root / ("worker" + std::to_string(owner));
    if (resource) {
        FILE* log = nullptr;
        need(freopen_s(&log, (base.string() + ".backend-profile.log").c_str(), "w", stderr) == 0,
             "worker profile log open failed");
    }
    TimingEvents events(packet_attempt, owner, false);
    spirula::TrainerSession session;
    const auto started = Clock::now();
    try {
        source_setup(session, argv[3], argv[2]);
        const auto prime_begin = Clock::now();
        const auto prime = source_capture(session, 0, session.start_step, false, false);
        const double prime_seconds = timing_seconds(prime_begin);
        Fields local = resident_owned() ? source_state() : Fields{};
        const size_t count = resident_owned() ? local.size() : source_state().size();
        const size_t active = (size_t)engine().cur_num_splats, capacity = (size_t)engine().max_num_splats;
        std::ofstream phases(base.string() + ".phases.tsv");
        phases.precision(17);
        phases << "step\top\tpacket_read_seconds\tcontext_seconds\tnative_update_seconds\tcheckpoint_seconds\tpacket_write_seconds\tlogical_read_bytes\tlogical_write_bytes\timage\tfaces\twidth\theight\tpixels\tread_payload_hash\twrite_payload_hash\tpacket_hash\tstate_transport\tloss_return_payload_bytes\n";
        phases << session.start_step << "\t4\t0\t" << prime_seconds << "\t0\t0\t0\t0\t0\t0\t"
            << prime.faces << '\t' << prime.width << '\t' << prime.height << '\t'
            << (uint64_t)prime.faces * prime.width * prime.height << "\t0\t0\t"
            << packet_hash_algorithm << '\t' << state_transport << "\t0\n";
        std::ofstream telemetry;
        if (resource) {
            telemetry.open(base.string() + ".device.tsv");
            telemetry << "seconds\tphase\tstep\thas_live_pool\tlive_pool_bytes\thas_global_estimate\tglobal_estimate_bytes\thas_total\ttotal_bytes\n";
            timing_device_sample(telemetry, started, "ready", session.start_step);
        }
        need(SetEvent(events.done), "worker ready signal failed");
        int next_step = session.start_step;
        bool awaiting_owner = false, checkpoint_saved = false;
        for (;;) {
            need(WaitForSingleObject(events.request, 300000) == WAIT_OBJECT_0,
                 "persistent worker command timeout");
            std::ifstream command(base.string() + ".job");
            int op = -1, step = -1, image = -2;
            std::string job_state, job_hash;
            command >> op >> step >> image >> job_state >> job_hash;
            need((bool)command && job_state == state_transport && job_hash == packet_hash_algorithm &&
                 (op == 0 || op == 1 || op == 2 || op == 3), "invalid persistent command/mode");
            command.close();
            fs::remove(base.string() + ".job");
            if (op == 0) {
                engine_reset();
                if (resource) timing_device_sample(telemetry, started, "shutdown", step);
                source_close(session);
                std::ofstream transport_stats(base.string() + ".transport.tsv");
                transport_stats.precision(17);
                transport_stats << "hash_seconds\t" << packet_hash_seconds
                    << "\nmap_copy_seconds\t" << packet_copy_seconds << '\n';
                transport_stats.flush();
                need((bool)transport_stats, "worker transport telemetry failed");
                need(SetEvent(events.done), "worker shutdown signal failed");
                return;
            }
            need(image >= -1 && image < session.ds.num_cameras &&
                 ((op == 1 && !awaiting_owner && step == next_step) ||
                  (op == 2 && awaiting_owner && !checkpoint_saved && step == next_step) ||
                  (op == 3 && !awaiting_owner && !checkpoint_saved && step == next_step - 1)),
                 "stale or repeated timing phase/version");
            source_packet_range(owner, (size_t)engine().cur_num_splats);
            const auto read_begin = Clock::now();
            const uint64_t read_bytes = mapping ? mapping->length() : fs::file_size(base.string() + ".in");
            const size_t input_count = resident_owned() && op != 3 ? (op == 1 ? 7 : 6) :
                count + (op == 2 ? 6 : 0);
            auto incoming = mapping ?
                mapping->read((uint64_t)step + 1, step, op == 1 ? 13 : op == 2 ? 11 : 17,
                              owner, input_count) :
                receive(base.string() + ".in", (uint64_t)step + 1, step,
                        op == 1 ? 13 : op == 2 ? 11 : 17, owner, input_count);
            const uint64_t read_payload_hash = packet_read_payload_hash;
            if (!mapping) fs::remove(base.string() + ".in");
            const double read_seconds = timing_seconds(read_begin);
            double context_seconds = 0, native_seconds = 0, checkpoint_seconds = 0;
            const auto context_begin = Clock::now();
            Fields before, output, data;
            if (resident_owned() && op != 3) {
                if (op == 1) install_source_projection(local, incoming, 1 - owner, 7, active, capacity, true);
                else data = std::move(incoming);
            } else {
                before.reserve(count);
                for (size_t i = 0; i < count; ++i) before.push_back(std::move(incoming[i]));
                for (size_t i = count; i < incoming.size(); ++i) data.push_back(std::move(incoming[i]));
                restore_source(before);
                if (resident_owned()) local = std::move(before);
            }
            engine().b1.owner = -1;
            ImageResult captured;
            if (op == 1) {
                if (image >= 0) captured = source_capture(session, image, step, resource && resident_owned(), true);
                restore_source(resident_owned() ? local : before);
                output = std::move(captured.grad);
                if (resource && resident_owned() && image >= 0) {
                    output.push_back(std::move(captured.render));
                    output.push_back(captured_loss_bytes(captured));
                }
                output.push_back(image_identity(image, captured.faces, captured.width, captured.height));
                context_seconds = timing_seconds(context_begin);
            } else if (op == 2) {
                // Reuse this worker's image stash (startup stash for empty warmup).
                // restore_source retains native world pointers; external FPBO does
                // not consume projection/camera VJP. The native guard still runs.
                context_seconds = timing_seconds(context_begin);
                const auto native_begin = Clock::now();
                source_owner_mask(owner);
                load_gradient(data);
                engine().optim.grad_scale = 1.f;
                engine_fpbo_step_from_data_gradients(step, engine_resolve_managed_step_config(
                    spirula::build_step_config(session.cfg, session.st, step)).optim);
                output = source_state();
                // Attest actual post-update GPU readback, never the incoming hash.
                const auto frozen = frozen_state_digest(output);
                if (resident_owned()) {
                    // Validate full GPU readback before modifying cache or publishing owned slices.
                    merge_source_owner(local, local, output, owner, active, capacity);
                    output = project_source_owner(local, owner, 20, active, capacity);
                } else output.resize(20);
                const auto attestation = frozen_owner_attestation(frozen, step, owner);
                output.emplace_back(attestation.begin(), attestation.end());
                native_seconds = timing_seconds(native_begin);
            } else {
                source_owner_mask(owner);
                output = source_state();
                const auto checkpoint_begin = Clock::now();
                save_canonical_snapshot(root / ("owner" + std::to_string(owner) + ".native"),
                                        step + 1);
                compare(output, source_state(), "final checkpoint changed canonical bytes");
                checkpoint_seconds = timing_seconds(checkpoint_begin);
            }
            if (op == 1) awaiting_owner = true;
            else if (op == 2) { awaiting_owner = false; ++next_step; }
            else checkpoint_saved = true;
            const auto write_begin = Clock::now();
            uint64_t write_bytes = 0;
            if (mapping) write_bytes = mapping->write((uint64_t)step + 1, step,
                op == 1 ? 14 : op == 2 ? 12 : 16, owner, output);
            else {
                packet(base.string() + ".out", (uint64_t)step + 1, step,
                       op == 1 ? 14 : op == 2 ? 12 : 16, owner, output);
                need(SetFileAttributesA((base.string() + ".out").c_str(), FILE_ATTRIBUTE_TEMPORARY),
                     "temporary packet attribute failed");
                write_bytes = fs::file_size(base.string() + ".out");
            }
            const uint64_t write_payload_hash = packet_write_payload_hash;
            const double write_seconds = timing_seconds(write_begin);
            phases << step << '\t' << op << '\t' << read_seconds << '\t'
                << context_seconds << '\t' << native_seconds << '\t' << checkpoint_seconds
                << '\t' << write_seconds << '\t' << read_bytes << '\t'
                << write_bytes << '\t' << captured.image << '\t'
                << captured.faces << '\t' << captured.width << '\t' << captured.height << '\t'
                << (uint64_t)captured.faces * captured.width * captured.height
                << '\t' << read_payload_hash << '\t' << write_payload_hash << '\t'
                << packet_hash_algorithm << '\t' << state_transport << '\t'
                << (resource && resident_owned() && op == 1 && image >= 0 ?
                    captured.loss.size() * sizeof(float) : 0) << '\n';
            phases.flush();
            need((bool)phases, "worker phase telemetry write failed");
            if (resource) timing_device_sample(telemetry, started, op == 1 ? "image" :
                op == 2 ? "owner" : "checkpoint", step);
            need(SetEvent(events.done), "worker completion signal failed");
        }
    } catch (...) { source_close(session); throw; }
}
struct TimingChildren {
    Child handles[2] = {-1, -1};
    uint64_t command_write_bytes = 0;
    double command_write_seconds = 0;
    TimingEvents events0, events1;
    TimingChildren() : events0(packet_attempt, 0, true), events1(packet_attempt, 1, true) {}
    TimingEvents& events(int owner) { return owner == 0 ? events0 : events1; }
    ~TimingChildren() {
        for (Child& child : handles) if (child != -1) {
            if (WaitForSingleObject((HANDLE)child, 0) == WAIT_TIMEOUT)
                TerminateProcess((HANDLE)child, 1);
            join(child);
            child = -1;
        }
    }
};
struct TimingStats {
    double writes = 0, reads = 0, barrier = 0, reduce = 0, publication = 0, checkpoint = 0;
    uint64_t fs_write = 0, fs_read = 0, context = 0, gradient = 0, owner_return = 0;
    uint64_t identity = 0, sampled_sum_max = 0;
    uint64_t render_return = 0, loss_return = 0;
    double worker_reads = 0, worker_writes = 0, worker_context = 0, worker_native = 0;
    double worker_checkpoint = 0;
    double worker_hash = 0, worker_copy = 0;
    uint64_t worker_fs_read = 0, worker_fs_write = 0;
    uint64_t command_write_bytes = 0;
    double command_write_seconds = 0;
    uint64_t worker_image_exposures = 0, worker_image_pixels = 0;
    uint64_t worker_context_exposures = 0, worker_context_pixels = 0;
    uint64_t worker_prime_exposures = 0, worker_prime_pixels = 0;
    uint64_t context_native = 0, context_frozen = 0;
    uint64_t owner_native = 0, owner_frozen = 0, owner_digest = 0;
    IO_COUNTERS worker_io{};
};
bool timing_join(TimingChildren& children, int owner, TimingStats& stats) {
    const HANDLE process = (HANDLE)children.handles[owner];
    need(WaitForSingleObject(process, 300000) == WAIT_OBJECT_0, "worker exit timeout");
    IO_COUNTERS io{};
    need(GetProcessIoCounters(process, &io), "final worker OS I/O accounting failed");
    stats.worker_io.ReadOperationCount += io.ReadOperationCount;
    stats.worker_io.WriteOperationCount += io.WriteOperationCount;
    stats.worker_io.OtherOperationCount += io.OtherOperationCount;
    stats.worker_io.ReadTransferCount += io.ReadTransferCount;
    stats.worker_io.WriteTransferCount += io.WriteTransferCount;
    stats.worker_io.OtherTransferCount += io.OtherTransferCount;
    const bool ok = join(children.handles[owner]);
    children.handles[owner] = -1;
    return ok;
}
using TimingGetMemory = BOOL (WINAPI*)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
TimingGetMemory timing_memory_function() {
    // Supported Windows exports K32GetProcessMemoryInfo; no Psapi link required.
    static const auto fn = (TimingGetMemory)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo");
    need(fn != nullptr, "Windows process memory accounting unavailable");
    return fn;
}
void timing_rss(std::ofstream& out, Clock::time_point started, TimingChildren* children,
                TimingStats& stats, int step) {
    uint64_t sum = 0;
    for (int owner = -1; owner < (children ? 2 : 0); ++owner) {
        HANDLE h = owner < 0 ? GetCurrentProcess() : (HANDLE)children->handles[owner];
        // A failure/shutdown can race a sample; exited processes have no live RSS.
        if (owner >= 0 && WaitForSingleObject(h, 0) == WAIT_OBJECT_0) continue;
        PROCESS_MEMORY_COUNTERS memory{};
        memory.cb = sizeof(memory);
        if (!timing_memory_function()(h, &memory, sizeof(memory))) {
            if (owner >= 0 && WaitForSingleObject(h, 0) == WAIT_OBJECT_0) continue;
            throw std::runtime_error("worker RSS query failed");
        }
        sum += memory.WorkingSetSize;
        out << timing_seconds(started) << '\t' << step << '\t' << owner << '\t'
            << GetProcessId(h) << '\t' << memory.WorkingSetSize << '\t'
            << memory.PeakWorkingSetSize << '\n';
    }
    stats.sampled_sum_max = std::max(stats.sampled_sum_max, sum);
    need(sum < 12ULL * 1024 * 1024 * 1024, "observed process RSS exceeds 12 GiB admission");
    out.flush();
    need((bool)out, "RSS telemetry write failed");
}
void timing_wait(TimingChildren& children, int owner, std::ofstream& rss,
                 Clock::time_point started, TimingStats& stats, int step) {
    const auto begin = Clock::now();
    HANDLE waits[] = {children.events(owner).done, (HANDLE)children.handles[owner]};
    for (;;) {
        const DWORD status = WaitForMultipleObjects(2, waits, FALSE, 100);
        if (status == WAIT_OBJECT_0) break;
        need(status == WAIT_TIMEOUT, "persistent child failed: publication forbidden");
        timing_rss(rss, started, &children, stats, step);
        need(timing_seconds(begin) < 300, "persistent child barrier timeout");
    }
}
void timing_command(TimingChildren& children, const fs::path& root, int owner,
                    int op, int step, int image) {
    const auto started = Clock::now();
    const fs::path base = root / ("worker" + std::to_string(owner));
    fs::remove(base.string() + ".out");
    {
        std::ofstream job(base.string() + ".job", std::ios::trunc);
        job << op << ' ' << step << ' ' << image << ' ' << state_transport << ' '
            << packet_hash_algorithm << '\n';
        job.flush();
        need((bool)job, "persistent command write failed");
    }
    children.command_write_bytes += fs::file_size(base.string() + ".job");
    children.command_write_seconds += timing_seconds(started);
    need(SetEvent(children.events(owner).request), "persistent request signal failed");
}
void timing_packet(const fs::path& root, int owner, int step, int kind,
                   const Fields& fields, TimingStats& stats, const Fields* tail = nullptr) {
    const auto begin = Clock::now();
    if (resident_owned() && kind == 11) stats.gradient += field_bytes(fields, 0, fields.size());
    else {
        stats.context += field_bytes(fields, 0, fields.size());
        stats.context_native += field_bytes(fields, 0, std::min(size_t{20}, fields.size()));
        stats.context_frozen += field_bytes(fields, std::min(size_t{20}, fields.size()), fields.size());
    }
    if (tail) stats.gradient += field_bytes(*tail, 0, tail->size());
    source_packet_range(owner, (size_t)engine().cur_num_splats);
    const fs::path path = root / ("worker" + std::to_string(owner) + ".in");
    if (timing_slots[owner])
        stats.fs_write += timing_slots[owner]->write((uint64_t)step + 1, step, kind, owner, fields, tail);
    else {
        packet(path, (uint64_t)step + 1, step, kind, owner, fields, tail);
        need(SetFileAttributesA(path.string().c_str(), FILE_ATTRIBUTE_TEMPORARY),
             "temporary packet attribute failed");
        stats.fs_write += fs::file_size(path);
    }
    stats.writes += timing_seconds(begin);
    need(timing_tree_bytes(root) < 8ULL * 1024 * 1024 * 1024 &&
         fs::space(root).available > 10ULL * 1024 * 1024 * 1024,
         "observed live scratch exceeds 8 GiB or free space below 10 GiB");
}
Fields timing_receive(const fs::path& root, int owner, int step, int kind,
                      size_t count, TimingStats& stats) {
    const auto begin = Clock::now();
    source_packet_range(owner, (size_t)engine().cur_num_splats);
    const fs::path path = root / ("worker" + std::to_string(owner) + ".out");
    if (!timing_slots[owner]) need(fs::exists(path), "missing packet");
    const uint64_t bytes = timing_slots[owner] ? timing_slots[owner]->length() : fs::file_size(path);
    auto fields = timing_slots[owner] ?
        timing_slots[owner]->read((uint64_t)step + 1, step, kind, owner, count) :
        receive(path, (uint64_t)step + 1, step, kind, owner, count);
    stats.fs_read += bytes;
    if (!timing_slots[owner]) fs::remove(path);
    stats.reads += timing_seconds(begin);
    return fields;
}
void timing_barrier(TimingChildren& children, std::ofstream& rss, Clock::time_point started,
                    TimingStats& stats, int step) {
    const auto begin = Clock::now();
    timing_wait(children, 0, rss, started, stats, step);
    timing_wait(children, 1, rss, started, stats, step);
    stats.barrier += timing_seconds(begin);
    timing_rss(rss, started, &children, stats, step);
}
void fixed_work_timing(const char* exe, int argc, char** argv) {
    const auto started = Clock::now();
    IO_COUNTERS parent_initial_io{};
    need(GetProcessIoCounters(GetCurrentProcess(), &parent_initial_io), "initial parent OS I/O accounting failed");
    const bool smoke = std::strcmp(argv[1], "--fpbo-timing-smoke") == 0;
    need(argc >= 8 && (argc - 8) % 2 == 0 &&
         std::strcmp(argv[2], "--resume") == 0 &&
         std::strcmp(argv[4], "--devices") == 0 && std::strcmp(argv[6], "--output") == 0,
         "fixed timing arguments");
    std::string method = "g1", raster = "ordered", transport = "file";
    bool finite = false, transport_set = false, work_set = false;
    const std::string spec = argv[5];
    int requested_exposures = 1200;
    bool method_set = false, raster_set = false, count_set = false;
    bool state_set = false, hash_set = false;
    for (int i = 8; i < argc; i += 2) {
        if (std::strcmp(argv[i], "--method") == 0) {
            need(!method_set, "duplicate timing method");
            method_set = true;
            method = argv[i + 1];
        } else if (std::strcmp(argv[i], "--measured-exposures") == 0) {
            need(!smoke && !count_set, "measured exposures only allowed once for --fpbo-timing");
            count_set = true;
            requested_exposures = measured_exposure_count(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--raster") == 0) {
            need(!raster_set, "duplicate raster selection");
            raster_set = true;
            raster = argv[i + 1];
        } else if (std::strcmp(argv[i], "--transport") == 0) {
            need(!transport_set, "duplicate transport");
            transport_set = true;
            transport = argv[i + 1];
        } else if (std::strcmp(argv[i], "--state-transport") == 0) {
            need(!state_set, "duplicate state transport");
            state_set = true;
            state_transport = argv[i + 1];
        } else if (std::strcmp(argv[i], "--packet-hash") == 0) {
            need(!hash_set, "duplicate packet hash");
            hash_set = true;
            select_packet_hash(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--updates") == 0) {
            need(!work_set && std::strcmp(argv[i + 1], "3") == 0,
                 "--updates accepts only 3 once for --fpbo-timing or --fpbo-timing-smoke (three measured pairs)");
            work_set = true;
            finite = true;
        } else need(false, "unknown timing option");
    }
    need(method == "g1" || method == "control", "timing method must be control or g1");
    const bool distributed = method == "g1";
    need(state_transport == "full" || resident_owned(), "state transport must be full or resident-owned");
    need(distributed || (!state_set && !hash_set),
         "state transport and packet hash selectors require persistent g1 (controls use ordinary native path)");
    need(transport == "file" || transport == "mapped", "transport must be file or mapped");
    need(transport == "file" || distributed, "mapped transport requires g1");
    need(!finite || !count_set, "finite work unit fixes six measured exposures");
    if (finite) requested_exposures = 6;
    need(raster == "ordered" || raster == "resident", "raster must be ordered or resident");
    need(raster == "ordered" || (!smoke && !distributed),
         "resident raster is only compatible with control --fpbo-timing");
    need(smoke || requested_exposures <= (30000 - 10000 - 101 - 1) * (distributed ? 2 : 1),
         "measured exposure schedule exceeds source optimizer horizon");
    const size_t comma = spec.find(',');
    need(comma != std::string::npos && spec.find(',', comma + 1) == std::string::npos,
         "exactly two timing device UUIDs required");
    DevicePair devices{spec.substr(0, comma), spec.substr(comma + 1)};
    need(devices[0].rfind("uuid:", 0) == 0 && devices[1].rfind("uuid:", 0) == 0 &&
         devices[0] != devices[1], "distinct explicit timing UUIDs required");
    const bool profiled = spirula::env("PROFILE") != nullptr;
    need(smoke || !profiled, "unprofiled benchmark required; use timing-smoke for SS_PROFILE");
    const fs::path root = fs::absolute(argv[7]);
    need(!fs::exists(root), "timing output directory must not already exist");
    need(fs::space(root.parent_path()).available > 10ULL * 1024 * 1024 * 1024,
         "timing scratch admission requires 10 GiB free");
    fs::create_directory(root);
    proof_root = root;
    packet_attempt = (uint64_t)Clock::now().time_since_epoch().count();
    std::ofstream exposures(root / "exposures.tsv"), rss(root / "rss.tsv");
    exposures << "ordinal\tphase\toptimizer_step\tbatch_index\timage\tfaces\twidth\theight\tpixels\tweight\n";
    rss << "seconds\tstep\towner_minus1_parent\tpid\tworking_set_bytes\tpeak_working_set_bytes\n";
    TimingStats stats;
    spirula::TrainerSession session;
    try {
        source_setup(session, argv[3], devices[0]);
        vkk::g1_set_ordered_raster(raster == "ordered");
        std::printf("G1 timing method=%s effective_raster=%s ordered_raster=%d measured_exposures=%d\n",
                    method.c_str(), raster.c_str(), raster == "ordered",
                    smoke && !finite ? 4 : requested_exposures);
        need(session.start_step == 10000 && session.ds.num_cameras == 1195,
             "fixed timing requires exact step10000 and 1195 training cameras");
        need(session.ds.train_indices.size() == 1195, "fixed image manifest requires 1195 training IDs");
        for (int id = 0; id < 1195; ++id)
            need(session.ds.train_indices[id] == id, "fixed training IDs must be ordered dataset IDs 0..1194");
        const size_t active = (size_t)engine().cur_num_splats;
        const size_t capacity = (size_t)engine().max_num_splats;
        need(active == 1000000, "fixed source topology must have one million rows");
        const auto prime = source_capture(session, 0, session.start_step, false, false);
        const uint64_t parent_prime_pixels = (uint64_t)prime.faces * prime.width * prime.height;
        {
            std::ofstream priming(root / "parent.prime.tsv");
            priming << "image\tfaces\twidth\theight\tpixels\n0\t" << prime.faces << '\t'
                << prime.width << '\t' << prime.height << '\t' << parent_prime_pixels << '\n';
            priming.flush();
            need((bool)priming, "parent priming manifest write failed");
        }
        Fields canonical = source_state();
        // Frozen context never publishes updates in this runner; parent owns it.
        const uint64_t frozen_identity = frozen_state_digest(canonical);
        const uint64_t dense = (uint64_t)capacity * (3 + 4 + 3 + 1 + 3 +
            (uint64_t)engine().num_sh * 3) * sizeof(float);
        const uint64_t full_packet = sizeof(Header) + field_bytes(canonical, 0, canonical.size()) +
            canonical.size() * sizeof(uint64_t);
        const int warm = smoke || finite ? 1 : 101, pairs = smoke && !finite ? 2 : requested_exposures / 2;
        const int measured_updates = distributed ? pairs : pairs * 2;
        const int updates = warm + measured_updates + 1;
        uint64_t smoke_render_max = 0;
        int smoke_render_max_image = -1;
        const uint64_t smoke_loss_bytes = smoke && resident_owned() ? prime.loss.size() * sizeof(float) : 0;
        if (smoke && resident_owned()) {
            // Same primary-image IDs as schedule.tsv; metadata only, no decode or native forward.
            const uint64_t images = std::min(uint64_t{1195}, (uint64_t)warm + pairs * 2 + 1);
            const auto& post = session.post;
            for (uint64_t image = 0; image < images; ++image) {
                need(image < session.ds.widths.size() && image < session.ds.heights.size(),
                     "scheduled render input size metadata missing");
                const int faces = post.any_warp ? post.K_per_camera.at((size_t)image) : 1;
                const size_t offset = post.any_warp ? (size_t)post.post_offsets.at((size_t)image) : 0;
                const int width = faces > 1 ? post.post_widths.at(offset) : session.ds.widths[image];
                const int height = faces > 1 ? post.post_heights.at(offset) : session.ds.heights[image];
                need(faces > 0 && width > 0 && height > 0, "scheduled render metadata invalid");
                for (int face = 1; face < faces; ++face)
                    need(post.post_widths.at(offset + face) == width &&
                         post.post_heights.at(offset + face) == height,
                         "FPBO image must have one equal-size face pass");
                const uint64_t area = (uint64_t)width * height;
                need(area <= (256ULL * 1024 * 1024) / (3 * sizeof(float)) / (uint64_t)faces,
                     "scheduled render exceeds packet field cap");
                const uint64_t pixels = (uint64_t)faces * area;
                const uint64_t bytes = pixels * 3 * sizeof(float);
                if (bytes > smoke_render_max) { smoke_render_max = bytes; smoke_render_max_image = (int)image; }
            }
        }
        const uint64_t gradient_packet = sizeof(Header) + dense + 7 * sizeof(uint64_t) +
            5 * sizeof(int64_t) + (smoke && resident_owned() ?
                smoke_render_max + smoke_loss_bytes + 2 * sizeof(uint64_t) : 0);
        const uint64_t map_capacity = transport == "mapped" ?
            (resident_owned() ? std::max(full_packet, gradient_packet) :
             full_packet + dense + 6 * sizeof(uint64_t)) : 0;
        need(map_capacity <= 1024ULL * 1024 * 1024, "largest exact packet exceeds 1 GiB");
        const uint64_t mapping_backing = map_capacity ? 2 * (map_capacity + sizeof(TimingMapping::Control)) : 0;
        need((field_bytes(canonical, 0, canonical.size()) + dense) * 12 + 2 * mapping_backing <
             12ULL * 1024 * 1024 * 1024, "12 GiB conservative host planning admission failed");
        std::unique_ptr<TimingMapping> mappings[2];
        if (map_capacity) for (int owner = 0; owner < 2; ++owner) {
            mappings[owner] = std::make_unique<TimingMapping>(packet_attempt, owner, map_capacity, true);
            timing_slots[owner] = mappings[owner].get();
        }
        Fields frozen_stats, frozen_shared;
        if (!distributed) {
            need(canonical.size() >= 24, "native frozen-state schema");
            frozen_stats.reserve(4);
            for (size_t i = 20; i < 24; ++i) frozen_stats.push_back(std::move(canonical[i]));
            for (size_t i = 24; i < canonical.size(); ++i) frozen_shared.push_back(std::move(canonical[i]));
            canonical.clear(); // Resident control retains no unnecessary full CPU replica.
        }
        {
            std::ofstream declared(root / "schedule.tsv");
            declared << "ordinal\tphase\toptimizer_step\tbatch_index\timage\tweight\n";
            uint64_t exposure = 0;
            for (int iteration = 0; iteration < updates; ++iteration) {
                const bool measured = iteration >= warm && iteration < warm + measured_updates;
                const int batch = measured && distributed ? 2 : 1;
                for (int b = 0; b < batch; ++b) {
                    declared << exposure << '\t' << (iteration < warm ? "warmup" : measured ? "measured" : "final")
                        << '\t' << 10000 + iteration << '\t' << b << '\t' << exposure % 1195
                        << '\t' << 1.0 / batch << '\n';
                    ++exposure;
                }
            }
            declared.flush();
            need((bool)declared, "predeclared schedule write failed");
        }
        std::ofstream device;
        if (smoke) {
            device.open(root / "parent.device.tsv");
            device << "seconds\tphase\tstep\thas_live_pool\tlive_pool_bytes\thas_global_estimate\tglobal_estimate_bytes\thas_total\ttotal_bytes\n";
            timing_device_sample(device, started, "ready", session.start_step);
        }
        // ponytail: one event-acknowledged slot per owner; host copies remain budgeted above.
        IO_COUNTERS parent_ready_io{};
        need(GetProcessIoCounters(GetCurrentProcess(), &parent_ready_io), "startup parent OS I/O accounting failed");
        const double common_startup_seconds = timing_seconds(started);
        const auto worker_startup_begin = Clock::now();
        std::unique_ptr<TimingChildren> children;
        if (distributed) {
            children = std::make_unique<TimingChildren>();
            for (int owner = 0; owner < 2; ++owner) {
                const auto o = std::to_string(owner), attempt = std::to_string(packet_attempt);
                const auto quoted_exe = timing_quote(exe), checkpoint = timing_quote(argv[3]),
                    directory = timing_quote(root.string());
                children->handles[owner] = _spawnl(_P_NOWAIT, exe, quoted_exe.c_str(), "timing-worker",
                    devices[owner].c_str(), checkpoint.c_str(), directory.c_str(), o.c_str(),
                    attempt.c_str(), smoke ? "resource" : "benchmark", std::to_string(map_capacity).c_str(),
                    state_transport.c_str(), packet_hash_algorithm.c_str(), nullptr);
                need(children->handles[owner] != -1, "persistent worker launch failed");
            }
            timing_barrier(*children, rss, started, stats, session.start_step);
        } else timing_rss(rss, started, nullptr, stats, session.start_step);
        const double worker_startup_seconds = distributed ? timing_seconds(worker_startup_begin) : 0;
        const auto train_begin = Clock::now();
        double warmup_seconds = 0;
        Clock::time_point final_update_begin;
        uint64_t ordinal = 0, total_pixels = 0, measured_pixels = 0, observed_measured_exposures = 0;
        double measured_seconds = 0;
        Clock::time_point measured_begin;
        bool owner_rejections_checked = false;
        for (int iteration = 0; iteration < updates; ++iteration) {
            const int step = session.start_step + iteration;
            const bool measured = iteration >= warm && iteration < warm + measured_updates;
            if (iteration == warm) {
                warmup_seconds = timing_seconds(train_begin);
                measured_begin = Clock::now();
            }
            if (iteration == warm + measured_updates) final_update_begin = Clock::now();
            const int batch_size = measured && distributed ? 2 : 1;
            const char* phase = iteration < warm ? "warmup" : measured ? "measured" : "final";
            std::array<int, 2> ids{};
            for (int b = 0; b < batch_size; ++b) ids[b] = (int)((ordinal + b) % 1195);
            const Fields& before = canonical;
            const auto cfg = engine_resolve_managed_step_config(
                spirula::build_step_config(session.cfg, session.st, step)).optim;
            need(cfg.use_fused_proj_bwd_optim, "native FPBO disabled during timing");
            Fields reduced, reference;
            if (distributed) {
                for (int owner = 0; owner < 2; ++owner) {
                    if (resident_owned()) {
                        const auto peer = project_source_owner(before, 1 - owner, 7, active, capacity);
                        timing_packet(root, owner, step, 13, peer, stats);
                    } else timing_packet(root, owner, step, 13, before, stats);
                    timing_command(*children, root, owner, 1, step,
                                   owner < batch_size ? ids[owner] : -1);
                }
                timing_barrier(*children, rss, started, stats, step);
                for (int owner = 0; owner < 2; ++owner) {
                    auto reply = timing_receive(root, owner, step, 14,
                        owner < batch_size ? (smoke && resident_owned() ? 9 : 7) : 1, stats);
                    const Bytes& identity = reply.back();
                    need(identity.size() == 5 * sizeof(int64_t), "timing image identity length");
                    std::array<int64_t, 5> values{};
                    std::memcpy(values.data(), identity.data(), identity.size());
                    need(values[0] == (owner < batch_size ? ids[owner] : -1) &&
                         values[4] == (int64_t)active, "timing image identity mismatch");
                    if (owner >= batch_size) {
                        compare(image_identity(-1, 0, 0, 0), identity, "empty timing worker");
                        continue;
                    }
                    need(values[1] > 0 && values[2] > 0 && values[3] > 0, "invalid image dimensions");
                    const uint64_t pixels = (uint64_t)values[1] * values[2] * values[3];
                    total_pixels += pixels;
                    if (measured) { measured_pixels += pixels; ++observed_measured_exposures; }
                    exposures << ordinal++ << '\t' << phase << '\t' << step << '\t' << owner
                        << '\t' << values[0] << '\t' << values[1] << '\t' << values[2] << '\t'
                        << values[3] << '\t' << pixels << '\t' << 1.0 / batch_size << '\n';
                    reply.pop_back();
                    if (smoke) {
                        restore_source(before);
                        const auto oracle = source_capture(session, ids[owner], step, resident_owned(), true);
                        if (resident_owned()) {
                            compare(captured_loss_bytes(oracle), reply.back(), "native source loss", 0.0003f);
                            reply.pop_back();
                            compare(oracle.render, reply.back(), "persistent resident exact native render");
                            reply.pop_back();
                        }
                        compare(oracle.grad, reply, "persistent worker exact native gradients");
                        add(reference, oracle.grad);
                    }
                    const auto reduce_begin = Clock::now();
                    if (reduced.empty()) reduced = std::move(reply);
                    else add(reduced, reply);
                    stats.reduce += timing_seconds(reduce_begin);
                }
                const auto reduce_begin = Clock::now();
                scale_gradients(reduced, 1.f / batch_size);
                stats.reduce += timing_seconds(reduce_begin);
                Fields expected;
                if (smoke) {
                    scale_gradients(reference, 1.f / batch_size);
                    compare(reference, reduced, "persistent ordered batch exact gradient reduction");
                    restore_source(before);
                    Fields internal;
                    if (batch_size == 1) {
                        engine().b1.owner = -1;
                        engine_optim_step(step, cfg);
                        internal = source_state();
                        restore_source(before);
                    }
                    load_gradient(reference);
                    engine().b1.owner = -1;
                    engine_fpbo_step_from_data_gradients(step, cfg);
                    expected = source_state();
                    if (batch_size == 1) compare(internal, expected, "smoke native internal vs external exact bytes");
                }
                for (int owner = 0; owner < 2; ++owner) {
                    if (resident_owned()) timing_packet(root, owner, step, 11, reduced, stats);
                    else timing_packet(root, owner, step, 11, before, stats, &reduced);
                    timing_command(*children, root, owner, 2, step, ids[batch_size - 1]);
                }
                timing_barrier(*children, rss, started, stats, step);
                Fields next = before;
                for (int owner = 0; owner < 2; ++owner) {
                    if (smoke && iteration == 0 && owner == 1 && transport == "file") {
                        const fs::path reply_path = root / "worker1.out",
                            withheld = root / "withheld-owner.out";
                        fs::rename(reply_path, withheld);
                        bool missing = false;
                        try { (void)timing_receive(root, owner, step, 12, 21, stats); }
                        catch (const std::runtime_error& e) {
                            missing = std::strcmp(e.what(), "missing packet") == 0;
                        }
                        fs::rename(withheld, reply_path);
                        need(missing && !fs::exists(root / "completed-native-state"),
                             "missing compact owner allowed partial publication");
                    }
                    auto reply = timing_receive(root, owner, step, 12, 21, stats);
                    if (smoke && iteration == 0 && owner == 1) {
                        const auto valid = reply.back();
                        for (int failure = 0; failure < 3; ++failure) {
                            if (failure == 2) reply.back()[0] ^= 1;
                            bool rejected = false;
                            try {
                                validate_frozen_owner(reply,
                                    frozen_identity ^ (failure == 0 ? uint64_t{1} : uint64_t{0}),
                                    step + (failure == 1 ? 1 : 0), owner);
                            } catch (const std::runtime_error&) { rejected = true; }
                            need(rejected && !fs::exists(root / "completed-native-state"),
                                 "invalid compact owner allowed partial publication");
                            reply.back() = valid;
                        }
                        owner_rejections_checked = true;
                    }
                    validate_frozen_owner(reply, frozen_identity, step, owner);
                    reply.pop_back();
                    const auto publication_begin = Clock::now();
                    if (resident_owned()) install_source_projection(next, reply, owner, 20, active, capacity);
                    else merge_source_owner(next, before, reply, owner, active, capacity, true);
                    stats.publication += timing_seconds(publication_begin);
                }
                if (smoke) compare(expected, next, "persistent merged exact resident native state");
                canonical = std::move(next); // Both validated complete owners or no publication.
            } else {
                const auto image = source_capture(session, ids[0], step, false, false);
                restore_statistics(frozen_stats);
                restore_shared(frozen_shared);
                engine().b1.owner = -1;
                engine_optim_step(step, cfg); // Internal FPBO, no dense CPU gradient roundtrip.
                const uint64_t pixels = (uint64_t)image.faces * image.width * image.height;
                total_pixels += pixels;
                if (measured) { measured_pixels += pixels; ++observed_measured_exposures; }
                exposures << ordinal++ << '\t' << phase << '\t' << step << "\t0\t" << ids[0]
                    << '\t' << image.faces << '\t' << image.width << '\t' << image.height
                    << '\t' << pixels << "\t1\n";
            }
            exposures.flush();
            need((bool)exposures, "exposure manifest write failed");
            if (smoke) timing_device_sample(device, started, phase, step);
            if (!distributed) {
                backend::device_synchronize();
                timing_rss(rss, started, nullptr, stats, step);
            }
            if (iteration == warm + measured_updates - 1)
                measured_seconds = timing_seconds(measured_begin);
        }
        const double final_update_seconds = timing_seconds(final_update_begin);
        const int boundary = session.start_step + updates;
        need(observed_measured_exposures == (uint64_t)pairs * 2 &&
             ordinal == (uint64_t)warm + pairs * 2 + 1 &&
             boundary <= 30000 && updates == warm + measured_updates + 1,
             "actual exposure/update/global-clock totals differ from declared schedule");
        const auto checkpoint_begin = Clock::now();
        if (distributed) {
            for (int owner = 0; owner < 2; ++owner) {
                timing_packet(root, owner, boundary - 1, 17, canonical, stats);
                timing_command(*children, root, owner, 3, boundary - 1, 0);
            }
            timing_barrier(*children, rss, started, stats, boundary - 1);
            for (int owner = 0; owner < 2; ++owner) {
                const auto expected = timing_receive(root, owner, boundary - 1, 16, canonical.size(), stats);
                compare(canonical, expected, "final native owner publication identity");
                const float poison = 12345.f;
                backend::memcpy_sync(engine().world.means.data_ptr(), &poison, sizeof(poison),
                                     backend::MemcpyKind::HostToDevice);
                need(engine_load_checkpoint((root / ("owner" + std::to_string(owner) + ".native")).string()) == boundary,
                     "native final owner checkpoint clock mismatch");
                need((size_t)engine().cur_num_splats == active &&
                     engine().world.features_sh_quant16_fpbo.initialized(), "native final layout mismatch");
                compare(expected, source_state(), "poisoned final native owner checkpoint readback");
            }
            restore_source(canonical);
        } else {
            canonical = source_state();
            engine_save_checkpoint((root / "control.native").string(), true, boundary);
            const float poison = 12345.f;
            backend::memcpy_sync(engine().world.means.data_ptr(), &poison, sizeof(poison),
                                 backend::MemcpyKind::HostToDevice);
            need(engine_load_checkpoint((root / "control.native").string()) == boundary,
                 "native control final clock mismatch");
            compare(canonical, source_state(), "poisoned control native checkpoint readback");
        }
        packet_first = 0; packet_rows = active;
        Fields artifact = canonical;
        std::vector<uint32_t> stable(active);
        std::iota(stable.begin(), stable.end(), 0);
        artifact.emplace_back((const uint8_t*)stable.data(), (const uint8_t*)(stable.data() + stable.size()));
        const fs::path published = root / "completed-native-state";
        packet(published, boundary, boundary - 1, 15, 2, artifact);
        compare(artifact, receive(published, boundary, boundary - 1, 15, 2, artifact.size()),
                "final completed artifact stable IDs and canonical bytes");
        const uint64_t final_hash = hash_file(published);
        stats.checkpoint += timing_seconds(checkpoint_begin);
        const auto shutdown_begin = Clock::now();
        bool failure_checked = false;
        if (distributed && smoke) {
            for (int owner = 0; owner < 2; ++owner) {
                if (resident_owned()) {
                    const auto peer = project_source_owner(canonical, 1 - owner, 7, active, capacity);
                    timing_packet(root, owner, boundary, 13, peer, stats);
                } else timing_packet(root, owner, boundary, 13, canonical, stats);
                if (owner == 1) {
                    if (timing_slots[owner]) InterlockedExchange64(&timing_slots[owner]->view->length, sizeof(Header) + 1);
                    else fs::resize_file(root / "worker1.in", sizeof(Header) + 1);
                }
                timing_command(*children, root, owner, 1, boundary, owner == 0 ? 0 : -1);
            }
            bool rejected = false;
            try { timing_barrier(*children, rss, started, stats, boundary); }
            catch (const std::runtime_error& e) {
                rejected = std::strcmp(e.what(), "persistent child failed: publication forbidden") == 0;
            }
            need(rejected && (transport == "mapped" ?
                 timing_slots[0]->view->length > 0 :
                 fs::exists(root / "worker0.out") && !fs::exists(root / "worker1.out")) &&
                 hash_file(published) == final_hash, "persistent failed-child publication refusal");
            failure_checked = true;
            timing_command(*children, root, 0, 0, boundary, -1);
            timing_wait(*children, 0, rss, started, stats, boundary);
            need(timing_join(*children, 0, stats), "successful worker shutdown failed");
            need(!timing_join(*children, 1, stats), "injected failed child unexpectedly succeeded");
        } else if (distributed) {
            for (int owner = 0; owner < 2; ++owner) timing_command(*children, root, owner, 0, boundary, -1);
            timing_barrier(*children, rss, started, stats, boundary);
            for (int owner = 0; owner < 2; ++owner) {
                need(timing_join(*children, owner, stats), "persistent worker shutdown failed");
            }
        }
        if (children) {
            stats.command_write_bytes = children->command_write_bytes;
            stats.command_write_seconds = children->command_write_seconds;
        }
        children.reset();
        const double worker_shutdown_and_failureproof_seconds = distributed ? timing_seconds(shutdown_begin) : 0;
        if (distributed) {
            for (int owner = 0; owner < 2; ++owner) {
                std::ifstream phases(root / ("worker" + std::to_string(owner) + ".phases.tsv"));
                std::string header;
                std::getline(phases, header);
                need((bool)phases, "missing worker phase telemetry");
                int step, op, image, faces, width, height;
                double read, context, native, checkpoint, write;
                uint64_t read_bytes, write_bytes, pixels, read_payload_hash, write_payload_hash;
                uint64_t loss_bytes = 0;
                std::string phase_hash, phase_state;
                while (phases >> step >> op >> read >> context >> native >> checkpoint >> write
                       >> read_bytes >> write_bytes >> image >> faces >> width >> height >> pixels
                       >> read_payload_hash >> write_payload_hash >> phase_hash >> phase_state >> loss_bytes) {
                    need(loss_bytes == (smoke && resident_owned() && op == 1 && faces > 0 ?
                         smoke_loss_bytes : 0), "unexpected worker loss payload");
                    need(phase_hash == packet_hash_algorithm && phase_state == state_transport,
                         "mixed worker telemetry modes");
                    need(std::isfinite(read) && std::isfinite(context) && std::isfinite(native) &&
                         std::isfinite(checkpoint) && std::isfinite(write), "nonfinite worker telemetry");
                    stats.worker_reads += read; stats.worker_writes += write;
                    stats.worker_context += context; stats.worker_native += native;
                    stats.worker_checkpoint += checkpoint;
                    stats.worker_fs_read += read_bytes; stats.worker_fs_write += write_bytes;
                    if (op == 1 || op == 2 || op == 3) {
                        const uint64_t count = op == 1 ? (faces > 0 ? (smoke && resident_owned() ? 9 : 7) : 1) :
                            op == 2 ? 21 : canonical.size();
                        need(write_bytes >= sizeof(Header) + count * sizeof(uint64_t), "worker packet byte accounting");
                        const uint64_t payload = write_bytes - sizeof(Header) - count * sizeof(uint64_t);
                        if (op == 1) {
                            need(payload >= 5 * sizeof(int64_t), "worker identity byte accounting");
                            stats.identity += 5 * sizeof(int64_t);
                            const uint64_t render = smoke && resident_owned() ? pixels * 3 * sizeof(float) : 0;
                            need(payload >= 5 * sizeof(int64_t) + render + loss_bytes, "render/loss return byte accounting");
                            stats.render_return += render;
                            stats.loss_return += loss_bytes;
                            stats.gradient += payload - 5 * sizeof(int64_t) - render - loss_bytes;
                        } else {
                            stats.owner_return += payload;
                            uint64_t core_bytes = 0;
                            for (size_t i = 0; i < 20; ++i)
                                core_bytes += resident_owned() && op == 2 ?
                                    source_owner_range(i, canonical[i].size(), owner, active, capacity).second :
                                    canonical[i].size();
                            stats.owner_native += core_bytes;
                            stats.owner_frozen += op == 2 ? 0 : field_bytes(canonical, 20, canonical.size());
                            if (op == 2) {
                                need(payload == core_bytes + sizeof(uint64_t),
                                     "compact owner return byte accounting");
                                stats.owner_digest += sizeof(uint64_t);
                            }
                        }
                    }
                    if (faces > 0) {
                        if (op == 1) { ++stats.worker_image_exposures; stats.worker_image_pixels += pixels; }
                        else if (op == 2) { ++stats.worker_context_exposures; stats.worker_context_pixels += pixels; }
                        else if (op == 4) { ++stats.worker_prime_exposures; stats.worker_prime_pixels += pixels; }
                    }
                }
                need(phases.eof(), "malformed worker phase telemetry");
                if (!(failure_checked && owner == 1)) {
                    std::ifstream transport_stats(root / ("worker" + std::to_string(owner) + ".transport.tsv"));
                    std::string hash_label, copy_label;
                    double hash = 0, copy = 0;
                    transport_stats >> hash_label >> hash >> copy_label >> copy;
                    need((bool)transport_stats && hash_label == "hash_seconds" &&
                         copy_label == "map_copy_seconds" && std::isfinite(hash) && hash >= 0 &&
                         std::isfinite(copy) && copy >= 0, "invalid worker transport telemetry");
                    stats.worker_hash += hash; stats.worker_copy += copy;
                }
            }
        }
        for (int owner = 0; owner < 2; ++owner)
            for (const char* suffix : {".job", ".in", ".out"})
                fs::remove(root / ("worker" + std::to_string(owner) + suffix));
        need(timing_tree_bytes(root) < 8ULL * 1024 * 1024 * 1024 &&
             fs::space(root).available > 10ULL * 1024 * 1024 * 1024, "final scratch/free-space cap exceeded");
        const uint64_t schedule_hash = hash_file(root / "schedule.tsv"),
            exposure_hash = hash_file(root / "exposures.tsv");
        const fs::path native0 = root / (distributed ? "owner0.native" : "control.native");
        const uint64_t state0_hash = hash_file(native0 / "state.tar"),
            splat0_hash = hash_file(native0 / "splat.ply"),
            state1_hash = distributed ? hash_file(root / "owner1.native" / "state.tar") : 0,
            splat1_hash = distributed ? hash_file(root / "owner1.native" / "splat.ply") : 0;
        const uint64_t native_bytes = timing_tree_bytes(native0) +
            (distributed ? timing_tree_bytes(root / "owner1.native") : 0);
        source_close(session);
        engine_reset();
        if (smoke) timing_device_sample(device, started, "shutdown", boundary);
        IO_COUNTERS parent_final_io{};
        need(GetProcessIoCounters(GetCurrentProcess(), &parent_final_io), "final parent OS I/O accounting failed");
        const double wall = timing_seconds(started);
        std::ofstream result(root / "result.tsv");
        result.precision(17);
        result << "field\tvalue\nmethod\t" << method << "\nmode\t" << (smoke ? "smoke" : "benchmark")
            << "\nprofiled\t" << profiled << "\nsource_identity\t" << packet_source
            << "\ncontext_native_payload_bytes\t" << stats.context_native
            << "\ncontext_frozen_stat_appearance_payload_bytes\t" << stats.context_frozen
            << "\ntransport\t" << transport << "\nfinite_work_unit\t" << finite
            << "\nstate_transport\t" << state_transport << "\npacket_hash\t" << packet_hash_algorithm
            << "\npacket_integrity_bits\t64\nsmoke_render_return_payload_bytes\t" << stats.render_return
            << "\nsmoke_loss_return_payload_bytes\t" << stats.loss_return
            << "\nmapping_capacity_per_worker_bytes\t" << map_capacity
            << "\nmapping_capacity_source\t" << (resident_owned() ?
                "max_full_checkpoint_scheduled_gradient_reply" : "full_state_plus_dense_gradients")
            << "\nsmoke_scheduled_max_render_payload_bytes\t" << smoke_render_max
            << "\nsmoke_scheduled_max_render_image\t" << smoke_render_max_image
            << "\nsmoke_loss_payload_bytes_per_image\t" << smoke_loss_bytes
            << "\nmapping_pagefile_backing_bytes\t" << mapping_backing
            << "\nmapping_physical_peak\tunknown"
            << "\nworker_packet_hash_seconds\t" << stats.worker_hash
            << "\nworker_map_copy_seconds\t" << stats.worker_copy
            << "\nparent_packet_hash_seconds\t" << packet_hash_seconds
            << "\nparent_map_copy_seconds\t" << packet_copy_seconds
            << "\npacket_phase_accounting\thash_and_copy_are_subsets_not_additional_wall"
            << "\nowner_native_return_payload_bytes\t" << stats.owner_native
            << "\nowner_frozen_stat_appearance_return_payload_bytes\t" << stats.owner_frozen
            << "\nowner_frozen_attestation_payload_bytes\t" << stats.owner_digest
            << "\nfrozen_context_identity\t" << frozen_identity
            << "\npacket_framing_write_bytes\t" << stats.fs_write + stats.worker_fs_write -
                stats.context - stats.gradient - stats.owner_return - stats.identity - stats.render_return - stats.loss_return
            << "\ncompleted_state_artifact_bytes\t" << fs::file_size(published)
            << "\nconfig_identity\t" << packet_layout << "\nattempt\t" << packet_attempt
            << "\ndevice0\t" << devices[0] << "\ndevice1\t" << devices[1]
            << "\nstart_step\t10000\nhorizon\t30000\nordered_raster\t" << (raster == "ordered")
            << "\neffective_raster\t" << raster
            << "\nfrozen_topology\t1\nfrozen_appearance\t1\nfrozen_statistics\t1"
            << "\nwarmup_updates\t" << warm << "\nmeasured_updates\t" << measured_updates
            << "\nfinal_updates\t1\nupdates\t" << updates << "\nexposures\t" << ordinal
            << "\nmeasured_exposures\t" << pairs * 2 << "\noptimizer_trajectories_equivalent\t0"
            << "\nexposure_count_semantics\tdeclared_primary_training_only"
            << "\nworker_context_refresh_forwards\t" << stats.worker_context_exposures
            << "\nworker_context_refresh_pixels\t" << stats.worker_context_pixels
            << "\nworker_primary_image_forwards\t" << stats.worker_image_exposures
            << "\nworker_primary_image_pixels\t" << stats.worker_image_pixels
            << "\nstartup_priming_forwards\t" << 1 + stats.worker_prime_exposures
            << "\nstartup_priming_pixels\t" << parent_prime_pixels + stats.worker_prime_pixels
            << "\nsmoke_oracle_forwards\t" << (smoke && distributed ? ordinal : 0)
            << "\ntotal_actual_image_forwards\t" << (distributed ?
                stats.worker_image_exposures + stats.worker_context_exposures +
                stats.worker_prime_exposures + 1 + (smoke ? ordinal : 0) : ordinal + 1)
            << "\ncomplete_wall_seconds\t" << wall << "\nmeasured_seconds\t" << measured_seconds
            << "\ncommon_startup_source_input_and_priming_seconds\t" << common_startup_seconds
            << "\nworker_startup_context_discovery_seconds\t" << worker_startup_seconds
            << "\nwarmup_seconds\t" << warmup_seconds << "\nfinal_update_seconds\t" << final_update_seconds
            << "\nworker_shutdown_and_failureproof_seconds\t" << worker_shutdown_and_failureproof_seconds
            << "\nowner_forward_context_policy\tretained_image_stash_startup_stash_for_empty_warmup"
            << "\ncomplete_wall_boundary\trunner_entry_through_native_readback_worker_join_parent_engine_pool_reset"
            << "\npixels\t" << total_pixels << "\nmeasured_pixels\t" << measured_pixels
            << "\nmeasured_exposures_per_second\t" << pairs * 2 / measured_seconds
            << "\nmeasured_megapixels_per_second\t" << measured_pixels / measured_seconds / 1e6
            << "\npacket_write_seconds\t" << stats.writes << "\npacket_read_validation_seconds\t" << stats.reads
            << "\nchild_barrier_seconds\t" << stats.barrier << "\ncpu_reduce_seconds\t" << stats.reduce
            << "\nowned_merge_publication_seconds\t" << stats.publication << "\ncheckpoint_readback_seconds\t" << stats.checkpoint
            << "\nworker_packet_read_validation_seconds\t" << stats.worker_reads
            << "\nworker_packet_write_seconds\t" << stats.worker_writes
            << "\nworker_state_restore_image_capture_and_priming_seconds\t" << stats.worker_context
            << "\nworker_gradient_upload_native_update_and_state_readback_seconds\t" << stats.worker_native
            << "\nworker_native_checkpoint_seconds\t" << stats.worker_checkpoint
            << "\nworker_logical_packet_read_bytes\t" << stats.worker_fs_read
            << "\nworker_logical_packet_write_bytes\t" << stats.worker_fs_write
            << "\nparent_logical_packet_write_bytes\t" << stats.fs_write
            << "\nparent_logical_packet_read_bytes\t" << stats.fs_read
            << "\nmap_write_logical_bytes\t" << (map_capacity ? stats.fs_write + stats.worker_fs_write : 0)
            << "\nmap_read_logical_bytes\t" << (map_capacity ? stats.fs_read + stats.worker_fs_read : 0)
            << "\nworker_logical_file_read_bytes\t" << (map_capacity ? 0 : stats.worker_fs_read)
            << "\nworker_logical_file_write_bytes\t" << (map_capacity ? 0 : stats.worker_fs_write)
            << "\nparent_logical_packet_file_write_bytes\t" << (map_capacity ? 0 : stats.fs_write)
            << "\nparent_logical_packet_file_read_bytes\t" << (map_capacity ? 0 : stats.fs_read)
            << "\ncontext_payload_bytes\t" << stats.context << "\ngradient_payload_bytes_both_directions\t" << stats.gradient
            << "\nowner_return_payload_bytes\t" << stats.owner_return << "\nimage_identity_payload_bytes\t" << stats.identity
            << "\nsampled_simultaneous_rss_sum_max_bytes\t" << stats.sampled_sum_max
            << "\noutput_artifact_bytes\t" << timing_tree_bytes(root)
            << "\ncompleted_boundary\t" << boundary << "\ncompleted_artifact_hash\t" << final_hash
            << "\nschedule_hash\t" << schedule_hash << "\nexposures_hash\t" << exposure_hash
            << "\nnative_output0_state_hash\t" << state0_hash << "\nnative_output0_splat_hash\t" << splat0_hash
            << "\nnative_output1_state_hash\t" << state1_hash << "\nnative_output1_splat_hash\t" << splat1_hash
            << "\nnative_checkpoint_artifact_bytes\t" << native_bytes
            << "\ncommand_metadata_write_bytes\t" << stats.command_write_bytes
            << "\ncommand_metadata_write_seconds\t" << stats.command_write_seconds
            << "\nbackend_profile_worker_logs\t" << (smoke && distributed ? "worker0.backend-profile.log,worker1.backend-profile.log" : "not_created")
            << "\nnative_checkpoint_readback_pass\t1\nworker_failure_refusal_exercised\t" << failure_checked
            << "\nworker_failure_refusal_pass\t" << (failure_checked ? "pass" : "not_exercised")
            << "\nowner_attestation_stale_identity_version_digest_missing_refusal_exercised\t"
                << owner_rejections_checked
            << "\nowner_attestation_refusal_pass\t"
                << (owner_rejections_checked ? "pass" : "not_exercised")
            << "\nglobally_merged_resumable_checkpoint\t0\nhost_staging_planning_cap_bytes\t12884901888"
            << "\nlive_scratch_cap_bytes\t8589934592\noutstanding_versions\t1"
            << "\npinned_host_peak\tunknown\ntrue_simultaneous_host_peak\tunknown\ndriver_device_peak\tunknown"
            << "\nos_io_semantics\tWindows_process_transfer_counters_include_files_devices_console_not_physical_disk"
            << "\nparent_startup_os_io_read_bytes\t" << parent_ready_io.ReadTransferCount - parent_initial_io.ReadTransferCount
            << "\nparent_startup_os_io_write_bytes\t" << parent_ready_io.WriteTransferCount - parent_initial_io.WriteTransferCount
            << "\nparent_total_os_io_read_bytes\t" << parent_final_io.ReadTransferCount - parent_initial_io.ReadTransferCount
            << "\nparent_total_os_io_write_bytes\t" << parent_final_io.WriteTransferCount - parent_initial_io.WriteTransferCount
            << "\nparent_total_os_io_other_bytes\t" << parent_final_io.OtherTransferCount - parent_initial_io.OtherTransferCount
            << "\nworker_total_os_io_read_bytes\t" << stats.worker_io.ReadTransferCount
            << "\nworker_total_os_io_write_bytes\t" << stats.worker_io.WriteTransferCount
            << "\nworker_total_os_io_other_bytes\t" << stats.worker_io.OtherTransferCount
            << "\nsource_loader_individual_file_io_breakdown\tunavailable"
            << "\nphysical_disk_write_bytes\tunknown\nbackend_copy_sync_profile\t"
            << (profiled ? "SS_PROFILE_process_exit_aggregates_separate" : "disabled") << '\n';
        result.flush();
        need((bool)result && std::isfinite(wall) && std::isfinite(measured_seconds) &&
             measured_seconds > 0, "invalid or incomplete timing result");
        timing_slots[0] = timing_slots[1] = nullptr;
    } catch (...) {
        timing_slots[0] = timing_slots[1] = nullptr;
        fs::remove(root / "result.tsv");
        for (int owner = 0; owner < 2; ++owner)
            for (const char* suffix : {".job", ".in", ".out"})
                fs::remove(root / ("worker" + std::to_string(owner) + suffix));
        source_close(session);
        engine_reset();
        throw;
    }
}
#endif

void source_proof(const char* exe, int argc, char** argv) {
    const bool distributed = std::strcmp(argv[1], "--fpbo-two-owner") == 0;
    need(argc >= 8 && std::strcmp(argv[2], "--resume") == 0 &&
         std::strcmp(argv[4], distributed ? "--devices" : "--device") == 0 &&
         std::strcmp(argv[6], "--images") == 0, "source proof arguments");
    DevicePair devices{};
    if (distributed) {
        const std::string spec = argv[5];
        const size_t comma = spec.find(',');
        need(comma != std::string::npos && comma != 0 &&
             comma + 1 < spec.size(), "two devices required");
        devices = {spec.substr(0, comma), spec.substr(comma + 1)};
        need(devices[0] != devices[1], "distinct owner devices required");
    } else devices = {argv[5], argv[5]};
    spirula::TrainerSession session;
    source_setup(session, argv[3], devices[0]);
    const auto ids = parse_images(argv[7], session.ds.num_cameras);
    need(ids.size() == 3 && ids[0] == 1188 && ids[1] == 1165 && ids[2] == 760,
         "source proof requires fixed low/median/high views 1188,1165,760");
    const size_t active = (size_t)engine().cur_num_splats;
    const size_t capacity = (size_t)engine().max_num_splats;
    need(active == 1000000 && (active + 255) / 256 == 3907 &&
         (active - 1) % 256 + 1 == 64, "source row/partial-tail identity mismatch");
    const uint64_t dense = (uint64_t)capacity * (3 + 4 + 3 + 1 + 3 +
        (uint64_t)engine().num_sh * 3) * sizeof(float);
    const uint64_t resident = world_bytes() + owner_state_bytes() + dense;
    need(resident * 12 < 8ULL * 1024 * 1024 * 1024 &&
         fs::space(fs::temp_directory_path()).available > 10ULL * 1024 * 1024 * 1024,
         "source proof exceeds host/staging or scratch admission");
    std::printf("G1 resource_admission world_state_gradient_estimate=%llu host_staging_cap=8589934592 scratch_output_cap=10737418240 outstanding_versions=1 replica_per_owner=full_world device_peak=UNVERIFIED\n",
        (unsigned long long)resident);
    const fs::path root = fs::temp_directory_path() /
        ("g1-source-" + std::to_string(Clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    proof_root = root;
    packet_attempt = (uint64_t)Clock::now().time_since_epoch().count();
    need(packet_attempt != 0, "proof attempt nonce");
    std::printf("G1 attempt=%llu tolerance_float_atol=0.0003 rtol=0.003 canonical_tolerance=0_bytes image_weight=equal reduction_order=declared replicas=full_world capacity_claim=NONE\n",
                (unsigned long long)packet_attempt);
    try {
        const int start = session.start_step;
        // Prime backward statistics and one-behind scalar readback before
        // either compared optimizer path; no duplicated backward per arm.
        (void)source_capture(session, ids[0], start);
        Fields canonical = source_state();
        const int updates = distributed ? 6 : 3;
        for (int iteration = 0; iteration < updates; ++iteration) {
            const int step = start + iteration;
            proof_step = (uint64_t)step;
            const auto cfg = engine_resolve_managed_step_config(
                spirula::build_step_config(session.cfg, session.st, step)).optim;
            need(cfg.use_fused_proj_bwd_optim, "effective FPBO disabled mid-proof");
            restore_source(canonical);
            const auto first = source_capture(session, ids[(size_t)iteration % ids.size()], step);
            restore_source(canonical);
            const Fields frozen = source_state();
            engine_optim_step(step, cfg);
            const Fields native = source_state();
            restore_source(frozen);
            load_gradient(first.grad);
            engine().optim.grad_scale = 1.f;
            engine_fpbo_step_from_data_gradients(step, cfg);
            report_source(native, source_state(), "native_internal_vs_external",
                          &frozen, &first.grad, &first.grad, &cfg);
            restore_source(canonical);
            const std::vector<int32_t> batch = distributed && iteration < 3
                ? std::vector<int32_t>{ids[(size_t)iteration]}
                : (!distributed && iteration == 0 ? std::vector<int32_t>{ids[0]} : ids);
            SourceTraffic traffic;
            Fields sum, reference_sum;
            if (distributed) {
                sum = source_image_phase(exe, devices, argv[3], root, step, batch, canonical,
                    [&](int id) { return source_capture(session, id, step); }, traffic, reference_sum);
            } else {
                for (int id : batch) {
                    restore_source(canonical);
                    const auto observed = source_capture(session, id, step);
                    if (id == first.image) {
                        compare(first.grad, observed.grad, "resident native float data gradients", 0.0003f);
                        compare(first.render, observed.render, "resident native render", 0.0003f);
                        Bytes a(first.loss.size() * sizeof(float)), b(observed.loss.size() * sizeof(float));
                        std::memcpy(a.data(), first.loss.data(), a.size());
                        std::memcpy(b.data(), observed.loss.data(), b.size());
                        compare(a, b, "resident native loss", 0.0003f);
                    }
                    add(sum, observed.grad);
                }
                scale_gradients(sum, 1.f / (float)batch.size());
            }
            const Fields before = canonical;
            Fields oracle_result;
            if (distributed) {
                restore_source(before);
                load_gradient(reference_sum);
                engine().optim.grad_scale = 1.f;
                engine_fpbo_step_from_data_gradients(step, cfg);
                oracle_result = source_state();
            }
            restore_source(before);
            load_gradient(sum);
            engine().optim.grad_scale = 1.f;
            engine_fpbo_step_from_data_gradients(step, cfg);
            if (distributed)
                report_source(oracle_result, source_state(), "resident_image_reduction_vs_received_update",
                              &before, &reference_sum, &sum, &cfg);
            const Fields resident_result = source_state();
            if (!distributed) {
                Fields owned = before;
                for (int owner = 0; owner < 2; ++owner) {
                    restore_source(before);
                    source_owner_mask(owner);
                    load_gradient(sum);
                    engine().optim.grad_scale = 1.f;
                    engine_fpbo_step_from_data_gradients(step, cfg);
                    merge_source_owner(owned, before, source_state(), owner, active, capacity);
                }
                engine().b1.owner = -1;
                report_source(resident_result, owned, "all_owned_vs_complementary_blocks",
                              &before, &sum, &sum, &cfg);
                restore_source(resident_result);
            }
            if (distributed) {
                canonical = source_owner_phase(exe, devices, argv[3], root, step, batch.back(),
                                                before, sum, resident_result, traffic);
                std::printf("G1 source step=%d owners=2 images=%zu published_version=%d worker_input_bytes=%llu worker_gradient_return_bytes=%llu worker_render_loss_identity_bytes=%llu worker_context_return_bytes=%llu worker_shared_stat_return_bytes=%llu owner_input_bytes=%llu owner_canonical_return_bytes=%llu owner_shared_stat_return_bytes=%llu file_staged_bytes=%llu last_valid_lanes=64 parity=pass\n",
                    step, batch.size(), step + 1,
                    (unsigned long long)traffic.worker_input,
                    (unsigned long long)traffic.worker_gradient,
                    (unsigned long long)traffic.worker_render_loss_identity,
                    (unsigned long long)traffic.worker_context_return,
                    (unsigned long long)traffic.worker_shared_stat_return,
                    (unsigned long long)traffic.owner_input,
                    (unsigned long long)traffic.owner_return,
                    (unsigned long long)traffic.owner_shared_stat_return,
                    (unsigned long long)traffic.files);
                std::printf("G1 owner_native_output_artifact_bytes=%llu (state.tar+splat.ply, separate from transport)\n",
                            (unsigned long long)traffic.owner_native_artifact);
            } else {
                canonical = resident_result;
                std::printf("G1 source step=%d native_vs_external=pass images=%zu\n",
                    step, batch.size());
            }
        }
        restore_source(canonical);
        if (distributed)
            source_stop_readback(exe, devices, argv[3], root, start + updates, canonical,
                ids[0], [&](int id) { return source_capture(session, id, start + updates); });
        else fs::remove_all(root);
        source_close(session);
    } catch (...) { cleanup_failed_proof(root); source_close(session); throw; }
}
void edge_proof(const char* exe, const DevicePair& devices, bool stop_check = false) {
    need(devices[0] != devices[1], "two distinct edge device UUIDs required");
    edge_setup(devices[0]);
    const auto warm = edge_capture(0, 0);
    const size_t carried = EdgeRows - 2, always = EdgeRows - 1;
    auto nonzero_row = [&](const Fields& grad, size_t row) {
        bool nonzero = false;
        for (const auto& field : grad) {
            need(field.size() % EdgeRows == 0, "edge gradient stable row layout");
            const size_t width = field.size() / EdgeRows;
            for (size_t p = row * width; p < (row + 1) * width; p += sizeof(float)) {
                float value;
                std::memcpy(&value, field.data() + p, sizeof(value));
                nonzero |= value != 0.f;
            }
        }
        return nonzero;
    };
    need(nonzero_row(warm.grad, carried), "native edge warmup must seed carried momentum");
    load_gradient(warm.grad);
    engine().optim.grad_scale = 1.f;
    engine_fpbo_step_from_data_gradients(0, edge_config());
    auto* means = (float*)engine().world.means.data_ptr();
    // Exact position/depth ties are deliberately present after native warmup.
    backend::memcpy_sync(means + 3, means, 3 * sizeof(float), backend::MemcpyKind::DeviceToDevice);
    const float invisible = -100.f;
    backend::memcpy_sync(means + carried * 3 + 2, &invisible, sizeof(invisible),
                         backend::MemcpyKind::HostToDevice);
    backend::memcpy_sync(means + always * 3 + 2, &invisible, sizeof(invisible),
                         backend::MemcpyKind::HostToDevice);
    const auto allocated = edge_capture(1, 1);
    for (const auto& field : allocated.grad)
        need(std::all_of(field.begin(), field.end(), [](uint8_t v) { return v == 0; }),
             "empty native FPBO image has nonzero data gradient");
    Fields canonical = source_state();
    compare(Bytes(canonical[0].begin(), canonical[0].begin() + 12),
            Bytes(canonical[0].begin() + 12, canonical[0].begin() + 24), "exact depth tie positions");
    const size_t dc_offset = carried * 3 * sizeof(float);
    const Bytes carried_dc(canonical[4].begin() + dc_offset,
                           canonical[4].begin() + dc_offset + 12);
    const fs::path root = fs::temp_directory_path() /
        ("g1-fpbo-edges-" + std::to_string(Clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    proof_root = root;
    packet_attempt = (uint64_t)Clock::now().time_since_epoch().count();
    std::printf("G1 fpbo_edges rows=576 blocks=3 last_valid_lanes=64 shared_stats=frozen float_atol=0.0003 rtol=0.003 canonical_tolerance=0_bytes replicas=full_world capacity_claim=NONE attempt=%llu ordered_raster=1\n",
                (unsigned long long)packet_attempt);
    try {
        const int updates = stop_check ? 1 : 3;
        for (int iteration = 0; iteration < updates; ++iteration) {
            const int step = iteration + 1;
            proof_step = (uint64_t)step;
            const std::vector<int32_t> images = iteration == 0
                ? std::vector<int32_t>{1} : std::vector<int32_t>{0, 2, 1};
            restore_source(canonical);
            backend::memcpy_sync(means + 3, means, 3 * sizeof(float),
                                 backend::MemcpyKind::DeviceToDevice);
            canonical = source_state();
            const auto first = edge_capture(images[0], step);
            restore_source(canonical);
            engine_optim_step(step, edge_config());
            const auto native = source_state();
            restore_source(canonical);
            load_gradient(first.grad);
            engine().optim.grad_scale = 1.f;
            engine_fpbo_step_from_data_gradients(step, edge_config());
            const auto native_cfg = edge_config();
            report_source(native, source_state(), "edge_native_internal_vs_external",
                          &canonical, &first.grad, &first.grad, &native_cfg);
            restore_source(canonical);
            SourceTraffic traffic;
            Fields reference_reduced;
            const Fields reduced = source_image_phase(exe, devices, "@fpbo-edges", root,
                step, images, canonical, [&](int id) { return edge_capture(id, step); },
                traffic, reference_reduced);
            need(!nonzero_row(reduced, carried) && !nonzero_row(reduced, always),
                 "invisible edge rows received native data gradients");
            if (iteration > 0) {
                restore_source(canonical);
                const auto left = edge_capture(0, step);
                restore_source(canonical);
                const auto right = edge_capture(2, step);
                need(left.width != right.width && left.height != right.height &&
                     nonzero_row(left.grad, 0) && nonzero_row(right.grad, 0),
                     "unequal overlapping native image work not exercised");
                restore_source(canonical);
                (void)edge_capture(1, step);
            }
            restore_source(canonical);
            load_gradient(reference_reduced);
            engine().optim.grad_scale = 1.f;
            engine_fpbo_step_from_data_gradients(step, edge_config());
            const auto oracle_result = source_state();
            restore_source(canonical);
            load_gradient(reduced);
            engine().optim.grad_scale = 1.f;
            engine_fpbo_step_from_data_gradients(step, edge_config());
            const auto resident = source_state();
            const auto cfg = edge_config();
            report_source(oracle_result, resident, "edge_resident_image_reduction_vs_received_update",
                          &canonical, &reference_reduced, &reduced, &cfg);
            canonical = source_owner_phase(exe, devices, "@fpbo-edges", root, step, 1,
                                            canonical, reduced, resident, traffic);
            restore_source(canonical);
            if (iteration == 0)
                need(!std::equal(carried_dc.begin(), carried_dc.end(),
                                 canonical[4].begin() + dc_offset),
                     "invisible native FPBO carried momentum did not advance DC");
            need(canonical[19].size() == EdgeRows * sizeof(int32_t), "edge bias row layout");
            for (size_t row = 0; row < EdgeRows; ++row) {
                int32_t bias;
                std::memcpy(&bias, canonical[19].data() + row * sizeof(bias), sizeof(bias));
                need(bias == iteration + 2, "invisible/tail native FPBO bias did not advance once");
            }
            std::printf("G1 fpbo_edges step=%d images=%zu process_fresh_workers=1 empty_worker=1 owner_visibility=empty carried_momentum=pass valid_tail=pass unowned_context=unchanged parity=pass file_staged_bytes=%llu\n",
                        step, images.size(), (unsigned long long)traffic.files);
            std::printf("G1 edge_owner_native_output_artifact_bytes=%llu (state.tar+splat.ply)\n",
                        (unsigned long long)traffic.owner_native_artifact);
        }
        const int boundary = updates + 1;
        source_stop_readback(exe, devices, "@fpbo-edges", root, boundary, canonical, 0,
                             [&](int id) { return edge_capture(id, boundary); });
        if (stop_check)
            std::printf("G1 fpbo_stop_check successful_empty_edge_update=1 worker_failure_publication_guard=pass native_checkpoint_readback=pass full_gate2_numerical_acceptance=NOT_CLAIMED timing=BLOCKED\n");
    } catch (...) { cleanup_failed_proof(root); throw; }
}
void coordinator(const char* exe, const DevicePair& devices) {
    need(backend::device_select_identity(devices[0].c_str()), "resident device UUID unavailable");
    initialize();
    const auto warm = render_backward(0, 0);
    need(warm.grad[4].size() == N * 3 * sizeof(float), "native DC gradient layout");
    float warm_dc[3];
    std::memcpy(warm_dc, warm.grad[4].data() + (N - 2) * sizeof(warm_dc), sizeof(warm_dc));
    need(std::any_of(std::begin(warm_dc), std::end(warm_dc),
                     [](float value) { return value != 0.f; }), "carried row needs native DC gradient");
    load_gradient(warm.grad);
    engine_optim_step(0, optim());
    auto* means = (float*)engine().world.means.data_ptr();
    backend::memcpy_sync(means + 3, means, 3 * sizeof(float), backend::MemcpyKind::DeviceToDevice);
    const float behind_camera = -100.f;
    backend::memcpy_sync(means + (N - 2) * 3 + 2,
                         &behind_camera, sizeof(behind_camera), backend::MemcpyKind::HostToDevice);
    Fields canonical = state();
    const size_t dc_offset = (N - 2) * 3 * sizeof(float);
    const Bytes carried_dc(canonical[4].begin() + dc_offset,
                           canonical[4].begin() + dc_offset + 3 * sizeof(float));
    const fs::path root = fs::temp_directory_path() /
        ("g1-native-" + std::to_string(Clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    try {
        uint64_t transmitted = 0;
        double preparation_s = 0, gradient_workers_s = 0, reduction_s = 0, owner_workers_s = 0;
        auto started = Clock::now();
        for (int step = 0; step < Steps; ++step) {
            const uint64_t version = (uint64_t)step + 1;
            Fields expected;
            std::array<ImageResult, Views> reference;
            for (int id = 0; id < Views; ++id) {
                reference[id] = render_backward(id, step);
                add(expected, reference[id].grad);
            }
            const auto prep_begin = Clock::now();
            const fs::path in[2] = {root / "in0", root / "in1"};
            const fs::path out[2] = {root / "out0", root / "out1"};
            Fields render_world(canonical.begin(), canonical.begin() + WorldFields);
            for (int owner = 0; owner < 2; ++owner) {
                packet(in[owner], version, step, 1, owner, render_world);
                transmitted += fs::file_size(in[owner]);
            }
            const auto grad_begin = Clock::now();
            run_pair(exe, "grad", devices, in, out, step);
            gradient_workers_s += std::chrono::duration<double>(Clock::now() - grad_begin).count();
            const auto reduce_begin = Clock::now();
            Fields received;
            for (int owner = 0; owner < 2; ++owner) {
                const auto ids = images_for(owner, step);
                transmitted += fs::file_size(out[owner]);
                auto rows = receive(out[owner], version, step, 2, owner,
                                    9 * std::count_if(ids.begin(), ids.end(), [](int id) { return id >= 0; }));
                for (int n = 0; n < 2 && ids[n] >= 0; ++n) {
                    const int id = ids[n];
                    const size_t off = (size_t)n * 9;
                    Fields grad(rows.begin() + off, rows.begin() + off + 6);
                    compare(reference[id].render, rows[off + 6], "render parity", 0.0003f);
                    Bytes loss(reference[id].loss.size() * sizeof(float));
                    std::memcpy(loss.data(), reference[id].loss.data(), loss.size());
                    compare(loss, rows[off + 7], "native loss parity", 0.0003f);
                    need(rows[off + 8].size() == (N + 1) * sizeof(uint32_t), "source ID count mismatch");
                    uint32_t image_id;
                    std::memcpy(&image_id, rows[off + 8].data(), 4);
                    need(image_id == (uint32_t)id, "image identity mismatch");
                    for (uint32_t source_row = 0; source_row < N; ++source_row) {
                        uint32_t got;
                        std::memcpy(&got, rows[off + 8].data() + (source_row + 1) * 4, 4);
                        need(got == source_row, "gradient source row mismatch");
                    }
                    compare(reference[id].grad, grad, "image gradient parity", 0.0003f);
                    if (id == 1)
                        for (const auto& field : grad)
                            need(std::all_of(field.begin(), field.end(), [](uint8_t v) { return v == 0; }),
                                 "empty-view owner sent nonzero gradient");
                    if (owner == 1 && step > 0)
                        need(std::any_of(grad.begin(), grad.end(), [](const Bytes& field) {
                            return std::any_of(field.begin(), field.end(), [](uint8_t v) { return v != 0; });
                        }), "device 1 sent empty native gradient");
                    add(received, grad);
                }
            }
            compare(expected, received, "reduced gradient parity", 0.001f);
            need(!row_nonzero(received, N - 2), "carried invisible row received gradient");
            need(!row_nonzero(received, N - 1), "always-invisible row received gradient");
            reduction_s += std::chrono::duration<double>(Clock::now() - reduce_begin).count();
            load_gradient(received);
            engine_optim_step(step, optim());
            const Fields resident = state();
            preparation_s += std::chrono::duration<double>(grad_begin - prep_begin).count();
            const auto owner_begin = Clock::now();
            for (int owner = 0; owner < 2; ++owner) {
                Fields request = half(canonical, owner);
                const Fields owner_grads = half(received, owner);
                request.insert(request.end(), owner_grads.begin(), owner_grads.end());
                packet(in[owner], version, step, 3, owner, request);
                transmitted += fs::file_size(in[owner]);
            }
            run_pair(exe, "owner", devices, in, out, step);
            transmitted += fs::file_size(out[0]) + fs::file_size(out[1]);
            const auto a = receive(out[0], version, step, 4, 0, canonical.size());
            const auto b = receive(out[1], version, step, 4, 1, canonical.size());
            merge(canonical, a, b);
            owner_workers_s += std::chrono::duration<double>(Clock::now() - owner_begin).count();
            compare(resident, canonical, "parameters/moments/codecs/bias parity");
            if (step == 0)
                need(!std::equal(carried_dc.begin(), carried_dc.end(),
                                 resident[4].begin() + dc_offset),
                     "native Adam failed to carry invisible row moment");
            need(canonical.back().size() == N * sizeof(int32_t), "bias count layout");
            for (int row = 0; row < N; ++row) {
                int32_t clock;
                std::memcpy(&clock, canonical.back().data() + row * sizeof(clock), sizeof(clock));
                need(clock == step + 2, "native optimizer row clock did not advance exactly once");
            }
            std::printf("G1 synthetic step=%d parity=pass file_staged_bytes=%llu\n", step + 1,
                        (unsigned long long)transmitted);
        }
        const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
        std::printf("G1 synthetic file-staged startup-inclusive PARITY diagnostic non-FPBO SH3 steps=%d file_staged_bytes=%llu wall_seconds=%.6f (not streaming performance or capacity evidence)\n",
                    Steps, (unsigned long long)transmitted, seconds);
        std::printf("G1 preparation_seconds=%.6f worker_gradient_seconds=%.6f reduction_and_validation_seconds=%.6f owner_update_and_commit_seconds=%.6f\n",
                    preparation_s, gradient_workers_s, reduction_s, owner_workers_s);
        const auto stale = root / "stale";
        packet(stale, 1, 0, 1, 0, canonical);
        bool rejected = false;
        try { receive(stale, 2, 1, 1, 0, canonical.size()); }
        catch (const std::runtime_error&) { rejected = true; }
        need(rejected, "stale packet accepted");
        packet(stale, 1, 0, 1, 0, canonical);
        {
            std::fstream f(stale, std::ios::binary | std::ios::in | std::ios::out);
            uint64_t wrong_source = Source + 1;
            f.seekp(offsetof(Header, source));
            f.write((char*)&wrong_source, sizeof(wrong_source));
            need((bool)f, "cannot corrupt source test packet");
        }
        rejected = false;
        try { receive(stale, 1, 0, 1, 0, canonical.size()); }
        catch (const std::runtime_error&) { rejected = true; }
        need(rejected, "mismatched source packet accepted");
        packet(stale, 1, 0, 1, 0, canonical);
        fs::resize_file(stale, sizeof(Header) + 1);
        rejected = false;
        try { receive(stale, 1, 0, 1, 0, canonical.size()); }
        catch (const std::runtime_error&) { rejected = true; }
        need(rejected, "partial packet accepted");
        fs::remove_all(root);
    } catch (...) { fs::remove_all(root); throw; }
}
}
int main(int argc, char** argv) {
    try {
        if (argc >= 2 && std::strcmp(argv[1],"ppisp-price")==0) {
            ppisp_price(argc,argv); return 0;
        }
        if (argc >= 2 && (std::strcmp(argv[1],"--ppisp-native-capture")==0 ||
                          std::strcmp(argv[1],"--ppisp-packet-replay")==0)) {
            ppisp_private_proof(argc,argv,std::strcmp(argv[1],"--ppisp-packet-replay")==0);
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "--synthetic") {
            need(std::string(argv[2]).rfind("uuid:", 0) == 0 &&
                 std::string(argv[3]).rfind("uuid:", 0) == 0 &&
                 std::strcmp(argv[2], argv[3]) != 0, "two distinct explicit device UUIDs required");
            coordinator(argv[0], {argv[2], argv[3]});
        } else if (argc == 4 && std::strcmp(argv[1], "--fpbo-edges") == 0) {
            edge_proof(argv[0], {argv[2], argv[3]});
        } else if (argc == 4 && std::strcmp(argv[1], "--fpbo-stop-check") == 0) {
            edge_proof(argv[0], {argv[2], argv[3]}, true);
        } else if ((argc == 2 || (argc == 4 && std::strcmp(argv[2], "--packet-hash") == 0)) &&
                   std::strcmp(argv[1], "--packet-check") == 0) {
            if (argc == 4) select_packet_hash(argv[3]);
            packet_check();
        } else if (argc == 3 && std::strcmp(argv[1], "--b1-refresh-check") == 0) {
            refresh_proof(argv[2]);
        } else if (argc == 6 && std::strcmp(argv[1], "--b1-refresh-source-check") == 0 &&
                   std::strcmp(argv[2], "--resume") == 0 &&
                   std::strcmp(argv[4], "--device") == 0) {
            refresh_proof(argv[5], argv[3]);
        } else if (argc >= 2 && std::strcmp(argv[1], "--o1-screen") == 0) {
            o1_screen(argc, argv);
        } else if (argc >= 2 && std::strcmp(argv[1], "--native-mask-check") == 0) {
            native_mask_check(argc, argv, false);
        } else if (argc >= 2 && std::strcmp(argv[1], "--once-vs-prime-check") == 0) {
            native_mask_check(argc, argv, true);
        } else if (argc == 8 && std::strcmp(argv[1], "--fpbo-replay") == 0) {
            source_replay(argc, argv);
        } else if (argc == 8 && std::strcmp(argv[1], "--fpbo-producer") == 0) {
            source_producer_diagnostic(argc, argv);
        } else if (argc == 8 && (std::strcmp(argv[1], "--fpbo") == 0 ||
                                 std::strcmp(argv[1], "--fpbo-two-owner") == 0)) {
            source_proof(argv[0], argc, argv);
        } else if (argc == 8 && std::string(argv[1]) == "--kitchen") {
            kitchen(argc, argv);
        } else if (argc == 7 && (std::strcmp(argv[1], "grad") == 0 ||
                                 std::strcmp(argv[1], "owner") == 0)) {
            worker(argc, argv);
        } else if (argc == 10 && (std::strcmp(argv[1], "source-owner") == 0 ||
                                   std::strcmp(argv[1], "source-grad") == 0)) {
            source_worker(argc, argv);
#ifdef _WIN32
        } else if (argc >= 2 &&
                   (std::strcmp(argv[1], "--fpbo-timing") == 0 ||
                    std::strcmp(argv[1], "--fpbo-timing-smoke") == 0)) {
            fixed_work_timing(argv[0], argc, argv);
        } else if (argc == 11 && std::strcmp(argv[1], "timing-worker") == 0) {
            timing_worker(argc, argv);
#endif
        } else {
            for (int i = 1; i < argc; ++i)
                need(std::strcmp(argv[i], "--state-transport") != 0 && std::strcmp(argv[i], "--packet-hash") != 0,
                     "state transport/packet hash selector unsupported for this path or platform");
            std::fprintf(stderr, "Windows persistent G1 timing: --state-transport full|resident-owned (default full), --packet-hash fnv|sha256 (default fnv; SHA256 truncated to 64-bit integrity), --transport file|mapped (default file), --updates 3 (five updates/eight primary exposures). Ordinary native controls reject state/hash selectors. Smoke without --updates retains two measured pairs. CPU --packet-check accepts --packet-hash. See docs/testing.md.\n");
            std::fprintf(stderr, "Private discriminator: --once-vs-prime-check --resume CHECKPOINT --device uuid:A --output NEW_DIRECTORY (same SS_B1_OWNER=all/SS_B1_VIEWS admission as --native-mask-check;135 native updates;source events0..7;no checkpoint export;parent CPU source-TAR verifier required). --native-mask-check remains the original four-arm32-update smoke.\n");
            throw std::runtime_error("usage: g1_native_proof --native-mask-check --resume CHECKPOINT --device uuid:A --output NEW_DIRECTORY (SS_B1_OWNER=all SS_B1_VIEWS=MANIFEST; B1_VIEW_POLICY env absent; canonical full view; 32 native updates) | --b1-refresh-check uuid:A | --b1-refresh-source-check --resume CHECKPOINT --device uuid:A | --o1-screen --resume CHECKPOINT --device uuid:A --output NEW_DIRECTORY | --packet-check | --synthetic uuid:A uuid:B | --fpbo-edges uuid:A uuid:B | --fpbo-stop-check uuid:A uuid:B | --fpbo --resume CHECKPOINT --device uuid:A --images 1188,1165,760 | --fpbo-two-owner --resume CHECKPOINT --devices uuid:A,uuid:B --images 1188,1165,760 | --fpbo-replay --resume CHECKPOINT --device uuid:A --evidence DIRECTORY | --fpbo-producer --resume CHECKPOINT --device uuid:A --image ID | --fpbo-timing or --fpbo-timing-smoke --resume CHECKPOINT --devices uuid:A,uuid:B --output NEW_DIRECTORY [--method control|g1] [--measured-exposures N (positive even, timing only)] [--raster ordered|resident (resident: control timing only)] (Windows)");
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "g1_native_proof: %s\n", e.what());
        return 1;
    }
}
