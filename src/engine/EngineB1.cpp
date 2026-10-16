#include "engine/Engine.h"
#include "engine/EngineState.h"
#include "app/B1Group.h"
#include "checkpoint/Resume.h"
#include "core/CheckpointIO.h"
#include "core/Sha256.h"

#if defined(SS_BACKEND_VULKAN)
#include "backend/vulkan/kernels/B1Ownership.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int64_t kB1Block = 256;

uint64_t _b1_source_id(const std::string& source_checkpoint_dir) {
    const std::filesystem::path path =
        std::filesystem::path(source_checkpoint_dir) / "state.tar";
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("B1 source checkpoint has no state.tar");
    uint64_t hash = 1469598103934665603ull;
    std::vector<char> buffer(1 << 20);
    while (input) {
        input.read(buffer.data(), (std::streamsize)buffer.size());
        const std::streamsize count = input.gcount();
        for (std::streamsize i = 0; i < count; ++i) {
            hash ^= (uint8_t)buffer[(size_t)i];
            hash *= 1099511628211ull;
        }
    }
    if (!input.eof()) throw std::runtime_error("B1 source checkpoint read failed");
    if (!hash) throw std::runtime_error("B1 source checkpoint ID is zero");
    return hash;
}

uint64_t _b1_read_le(std::istream& in, size_t bytes, const char* path) {
    unsigned char raw[8] = {};
    in.read(reinterpret_cast<char*>(raw), (std::streamsize)bytes);
    if (in.gcount() != (std::streamsize)bytes)
        throw std::runtime_error(std::string("B1 sidecar truncated: ") + path);
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; ++i)
        value |= (uint64_t)raw[i] << (8 * i);
    return value;
}

bool _b1_regular_file(const std::filesystem::path& path) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) throw std::runtime_error("B1 sidecar stat failed: " + path.string());
    if (!exists) return false;
    if (!std::filesystem::is_regular_file(path, ec) || ec)
        throw std::runtime_error("B1 sidecar is not a regular file: " + path.string());
    return true;
}

bool _b1_load_sidecars(const std::string& source_checkpoint_dir,
                        int64_t active_n, uint32_t block_count, int owner,
                        int64_t checkpoint_step, std::vector<uint32_t>& mask,
                        uint64_t& source_id) {
    if (source_checkpoint_dir.empty()) return false;
    const std::filesystem::path root(source_checkpoint_dir);
    const std::filesystem::path blocks_path = root / "b1-owned-blocks.bin";
    const std::filesystem::path ids_path = root / "b1-owned-ids.bin";
    const std::filesystem::path excluded_path = root / "b1-excluded-ids.bin";
    const bool has_blocks = _b1_regular_file(blocks_path);
    const bool has_ids = _b1_regular_file(ids_path);
    const bool has_excluded = _b1_regular_file(excluded_path);
    if (!has_blocks && !has_ids && !has_excluded) return false;
    if (!has_blocks || !has_ids || !has_excluded)
        throw std::runtime_error("B1 sidecars must be present together");

    const uint64_t block_bytes = 32ull + (uint64_t)block_count * sizeof(uint32_t);
    std::error_code ec;
    if (std::filesystem::file_size(blocks_path, ec) != block_bytes || ec)
        throw std::runtime_error("B1 block sidecar has invalid byte length");
    std::ifstream blocks(blocks_path, std::ios::binary);
    if (!blocks) throw std::runtime_error("cannot open B1 block sidecar");
    if (_b1_read_le(blocks, sizeof(uint64_t), blocks_path.string().c_str()) !=
        (uint64_t)active_n)
        throw std::runtime_error("B1 block sidecar active N mismatch");
    source_id = _b1_read_le(blocks, sizeof(uint64_t), blocks_path.string().c_str());
    if (!source_id ||
        _b1_read_le(blocks, sizeof(uint64_t), blocks_path.string().c_str()) !=
            (uint64_t)checkpoint_step)
        throw std::runtime_error("B1 block sidecar source or step mismatch");
    if (_b1_read_le(blocks, sizeof(uint32_t), blocks_path.string().c_str()) !=
        block_count)
        throw std::runtime_error("B1 block sidecar block count mismatch");
    if (_b1_read_le(blocks, sizeof(uint32_t), blocks_path.string().c_str()) !=
        (uint32_t)owner)
        throw std::runtime_error("B1 block sidecar owner mismatch");
    for (uint32_t& value : mask) {
        value = (uint32_t)_b1_read_le(
            blocks, sizeof(uint32_t), blocks_path.string().c_str());
        if (value > 1 || (owner == 2 && value != 1))
            throw std::runtime_error("B1 block sidecar has invalid mask value");
    }

    std::vector<uint8_t> seen((size_t)active_n, 0);
    auto read_ids = [&](const std::filesystem::path& path) {
        ec.clear();
        const uint64_t bytes = std::filesystem::file_size(path, ec);
        if (ec || bytes < sizeof(uint64_t))
            throw std::runtime_error("B1 ID sidecar has invalid byte length");
        std::ifstream ids(path, std::ios::binary);
        if (!ids) throw std::runtime_error("cannot open B1 ID sidecar");
        const uint64_t count = _b1_read_le(ids, sizeof(uint64_t), path.string().c_str());
        if (count > (std::numeric_limits<uint64_t>::max() - sizeof(uint64_t)) /
                        sizeof(uint32_t) ||
            sizeof(uint64_t) + count * sizeof(uint32_t) != bytes)
            throw std::runtime_error("B1 ID sidecar has invalid byte length");
        uint32_t previous = 0;
        for (uint64_t i = 0; i < count; ++i) {
            const uint32_t id =
                (uint32_t)_b1_read_le(ids, sizeof(uint32_t), path.string().c_str());
            if ((uint64_t)id >= (uint64_t)active_n || mask[id / kB1Block] == 0 ||
                (i != 0 && id <= previous) || seen[id]++)
                throw std::runtime_error("B1 ID sidecar has invalid ownership or order");
            previous = id;
        }
    };
    read_ids(ids_path);
    read_ids(excluded_path);
    for (size_t i = 0; i < seen.size(); ++i)
        if ((mask[i / 256] != 0) != (seen[i] != 0))
            throw std::runtime_error("B1 sidecars do not cover owned rows");
    return true;
}

std::string _b1_context_hash(bool owned = false, bool render = true) {
    auto& s = engine();
    if (s.b1.owner != 0 && s.b1.owner != 1) return ckpt::b1_indices_hash({});
    const size_t blocks = (size_t)((s.cur_num_splats + 255) / 256);
    std::vector<uint32_t> mask(blocks);
    backend::memcpy_sync(mask.data(), s.b1.owned_blocks.data_ptr(),
                         blocks * sizeof(uint32_t), backend::MemcpyKind::DeviceToHost);
    if (const char* error = backend::last_error()) throw std::runtime_error(error);
    spirula::Sha256 hash;
    auto add = [&](const void* ptr, size_t rows, size_t stride, bool per_block = false) {
        if (!ptr || rows == 0 || stride == 0) return;
        if (rows > std::numeric_limits<size_t>::max() / stride)
            throw std::runtime_error("B1 context hash size overflow");
        std::vector<uint8_t> data(rows * stride);
        backend::memcpy_sync(data.data(), ptr, data.size(),
                             backend::MemcpyKind::DeviceToHost);
        if (const char* error = backend::last_error()) throw std::runtime_error(error);
        const size_t active = per_block ? blocks : (size_t)s.cur_num_splats;
        if (rows < active) throw std::runtime_error("B1 context hash short buffer");
        for (size_t i = 0; i < active; ++i) {
            if ((mask[per_block ? i : i / 256] != 0) != owned) continue;
            hash.update(data.data() + i * stride, stride);
        }
    };
    const size_t n = (size_t)s.cur_num_splats;
    if (render) {
        add(s.world.means.data_ptr(), n, sizeof(float3));
        add(s.world.quats.data_ptr(), n, sizeof(float4));
        add(s.world.scales.data_ptr(), n, sizeof(float3));
        add(s.world.opacities.data_ptr(), n, sizeof(float));
        add(s.world.features_dc.data_ptr(), n, sizeof(float3));
    }
    add(s.optim.bias_correction_steps.data_ptr(), n, sizeof(int32_t));
    add(s.optim.accum_buffer.data_ptr(), n, sizeof(float2));
    const size_t max_n = (size_t)s.max_num_splats;
    auto codec = [&](const auto& q, bool padded) {
        if (!q.initialized() || q.n_bounds < (int64_t)blocks ||
            q.packed_bytes() % (padded ? q.n_bounds : (int64_t)max_n) != 0)
            throw std::runtime_error("B1 context hash requires FPBO quantized state");
        if (padded)
            add(q.packed_ptr(), (size_t)q.n_bounds,
                (size_t)q.packed_bytes() / (size_t)q.n_bounds, true);
        else
            add(q.packed_ptr(), max_n, (size_t)q.packed_bytes() / max_n);
        add(q.bounds_ptr(), (size_t)q.n_bounds,
            (size_t)q.bounds_bytes() / (size_t)q.n_bounds, true);
    };
    if (render) codec(s.world.features_sh_quant16_fpbo, true);
    codec(s.optim.sh_quant_state_fpbo, true);
    codec(s.optim.means_quant_state_fpbo, false);
    codec(s.optim.quats_quant_state_fpbo, false);
    codec(s.optim.scales_quant_state_fpbo, false);
    codec(s.optim.opacities_quant_state_fpbo, false);
    codec(s.optim.features_dc_quant_state_fpbo, false);
    return hash.hex();
}

void _b1_disable() {
    engine().b1.owner = -1;
    engine().b1.source_id = 0;
    engine().b1.frozen_hash.clear();
    engine().b1.refresh_pending_boundary = -1;
    engine().b1.refresh_export_sha256.clear();
    engine().b1.refresh_installed_sha256.clear();
    engine().b1.contract.reset();
#if defined(SS_BACKEND_VULKAN)
    vkk::b1_set_owned_blocks(nullptr);
#endif
    engine().b1.owned_blocks = DeviceVector<uint32_t>();
    DevicePool::global().free_dynamic_prefix("eng.b1.owned_blocks");
}

uint32_t _b1_blocks(int64_t n, const char* name) {
    if (n <= 0 || n > (int64_t)std::numeric_limits<uint32_t>::max())
        throw std::runtime_error(std::string("engine_b1_configure: invalid ") + name);
    const int64_t blocks = (n + kB1Block - 1) / kB1Block;
    if ((uint64_t)blocks > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("engine_b1_configure: block count out of range");
    return (uint32_t)blocks;
}

struct RefreshField {
    void* ptr;
    size_t rows, stride;
    bool block;
};

std::vector<RefreshField> refresh_fields() {
    auto& s = engine();
    const size_t n = (size_t)s.cur_num_splats;
    auto& q = s.world.features_sh_quant16_fpbo;
    const size_t blocks = (n + 255) / 256;
    if (!q.initialized() || q.n_bounds < (int64_t)blocks ||
        q.packed_bytes() % q.n_bounds || q.bounds_bytes() % q.n_bounds)
        throw std::runtime_error("B1 refresh requires native FPBO render codec");
    return {{s.world.means.data_ptr(), n, sizeof(float3), false},
            {s.world.quats.data_ptr(), n, sizeof(float4), false},
            {s.world.scales.data_ptr(), n, sizeof(float3), false},
            {s.world.opacities.data_ptr(), n, sizeof(float), false},
            {s.world.features_dc.data_ptr(), n, sizeof(float3), false},
            {q.packed_ptr(), blocks, (size_t)q.packed_bytes() / (size_t)q.n_bounds, true},
            {q.bounds_ptr(), blocks, (size_t)q.bounds_bytes() / (size_t)q.n_bounds, true}};
}

std::vector<uint32_t> refresh_mask() {
    auto& s = engine();
    std::vector<uint32_t> mask((size_t)(s.cur_num_splats + 255) / 256);
    backend::memcpy_sync(mask.data(), s.b1.owned_blocks.data_ptr(), mask.size() * 4,
                         backend::MemcpyKind::DeviceToHost);
    if (const char* error = backend::last_error()) throw std::runtime_error(error);
    return mask;
}

void refresh_boundary(int64_t boundary) {
    const auto& b = engine().b1;
    if (!b.contract || b.contract->version != 3 || b.owner < 0 || b.owner > 1)
        throw std::runtime_error("B1 refresh requires a live-context owner contract");
    const auto& c = *b.contract;
    if (boundary != c.refresh_generation + c.refresh_cadence || boundary > c.horizon)
        throw std::runtime_error("B1 refresh stale or skipped boundary");
}

std::string refresh_header(int owner, int64_t boundary) {
    const auto& c = *engine().b1.contract;
    JsonWriter j;
    j.object().field("version", 1).field("owner", owner)
        .field("boundary", (long long)boundary).field("parent", (long long)c.refresh_generation)
        .field("rows", (long long)c.rows).field("source", c.source_state_sha256)
        .field("group", c.refresh_group_sha256).field("views", c.full_train_indices_sha256)
        .field("parent_commit", c.refresh_commit_sha256)
        .field("config", c.config_sha256).end();
    return j.str();
}

void refresh_copy(void* destination, const void* source, size_t bytes,
                  backend::MemcpyKind kind) {
    backend::memcpy_sync(destination, source, bytes, kind);
    if (const char* error = backend::last_error()) throw std::runtime_error(error);
}

void refresh_write(std::ostream& out, const void* data, size_t bytes) {
    out.write(static_cast<const char*>(data), (std::streamsize)bytes);
    if (!out) throw std::runtime_error("B1 refresh payload write failed");
}

void refresh_read(std::istream& in, void* data, size_t bytes) {
    in.read(static_cast<char*>(data), (std::streamsize)bytes);
    if (!in) throw std::runtime_error("B1 refresh payload truncated");
}

size_t refresh_field_bytes(const RefreshField& field, const std::vector<uint32_t>& mask) {
    size_t bytes = 0;
    for (size_t block = 0; block < mask.size(); ++block) {
        if (!mask[block]) continue;
        const size_t count = field.block ? 1 : std::min(size_t{256}, field.rows - block * 256);
        if (count > (std::numeric_limits<size_t>::max() - bytes) / field.stride)
            throw std::runtime_error("B1 refresh field size overflow");
        bytes += count * field.stride;
    }
    return bytes;
}
}  // namespace

void engine_b1_configure(int owner, const std::string& source_checkpoint_dir,
                         int64_t checkpoint_step) {
    const auto saved_contract = source_checkpoint_dir.empty()
        ? std::optional<ckpt::B1Contract>{}
        : ckpt::read_b1_contract(source_checkpoint_dir);
    if (saved_contract && saved_contract->owner != owner)
        throw std::runtime_error("B1 owner checkpoint cannot disable or change ownership");
    _b1_disable();
    if (owner == -1) return;
    if (owner < 0 || owner > 2)
        throw std::runtime_error("engine_b1_configure: owner must be -1, 0, 1, or 2");

#if !defined(SS_BACKEND_VULKAN)
    throw std::runtime_error("engine_b1_configure: requires the Vulkan backend");
#else
    auto& s = engine();
    if (s.primitive != "3dgs" || s.world.sh_value_bits != 16 ||
        s.optim.sh_optim_bits != 8 || s.optim.non_sh_optim_bits != 16)
        throw std::runtime_error("engine_b1_configure: requires quantized 3dgs FPBO");
    if (!s.optim.use_fused_proj_bwd_optim)
        throw std::runtime_error("engine_b1_configure: requires FPBO");
    if (!s.packed)
        throw std::runtime_error("engine_b1_configure: requires packed layout");
    const uint32_t mask_blocks = _b1_blocks(s.cur_num_splats, "cur_num_splats");
    if (s.cur_num_splats > s.max_num_splats ||
        s.max_num_splats > (int64_t)std::numeric_limits<uint32_t>::max() ||
        s.world.means.data_ptr() == nullptr ||
        s.world.means.size() < s.cur_num_splats)
        throw std::runtime_error("engine_b1_configure: invalid world mean bounds");
    if ((uint64_t)mask_blocks >
        (uint64_t)std::numeric_limits<size_t>::max() / sizeof(uint32_t))
        throw std::runtime_error("engine_b1_configure: mask size out of range");

    std::vector<uint32_t> mask(mask_blocks, 0u);
    uint64_t source_id = 0;
    const bool restored_mask = _b1_load_sidecars(
        source_checkpoint_dir, s.cur_num_splats, mask_blocks, owner,
        checkpoint_step, mask, source_id);
    if (!restored_mask) source_id = _b1_source_id(source_checkpoint_dir);
    if (!restored_mask && owner == 2)
        std::fill(mask.begin(), mask.end(), 1u);
    if (!restored_mask && owner != 2) {
        std::vector<float3> means((size_t)s.cur_num_splats);
        backend::memcpy_sync(means.data(), s.world.means.data_ptr(),
                             means.size() * sizeof(float3),
                             backend::MemcpyKind::DeviceToHost);
        if (const char* error = backend::last_error())
            throw std::runtime_error(error);

        std::vector<double> mean_y(mask_blocks);
        int64_t invalid_rows = 0;
        for (uint32_t block = 0; block < mask_blocks; ++block) {
            const int64_t begin = (int64_t)block * kB1Block;
            const int64_t end = std::min(begin + kB1Block, s.cur_num_splats);
            double sum = 0.0;
            int64_t valid = 0;
            for (int64_t i = begin; i < end; ++i) {
                const float3 v = means[(size_t)i];
                if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) {
                    ++invalid_rows;
                    continue;
                }
                sum += v.y;
                ++valid;
            }
            mean_y[block] = valid ? sum / (double)valid
                                  : std::numeric_limits<double>::infinity();
        }
        if (invalid_rows)
            std::printf("B1 nonfinite source rows excluded from spatial ranking: %lld\n",
                        (long long)invalid_rows);

        std::vector<uint32_t> order(mask_blocks);
        std::iota(order.begin(), order.end(), 0u);
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            return mean_y[a] != mean_y[b] ? mean_y[a] < mean_y[b] : a < b;
        });
        const uint32_t owner0_blocks = mask_blocks / 2;
        for (uint32_t rank = 0; rank < mask_blocks; ++rank)
            mask[order[rank]] = ((rank < owner0_blocks) ? 0u : 1u) == (uint32_t)owner;
    }

    s.b1.owned_blocks.resize_dynamic(
        VramCategory::Other, "eng.b1.owned_blocks", mask_blocks);
    backend::memcpy_sync(s.b1.owned_blocks.data_ptr(), mask.data(),
                         mask.size() * sizeof(uint32_t),
                         backend::MemcpyKind::HostToDevice);
    if (const char* error = backend::last_error())
        throw std::runtime_error(error);
    s.b1.owner = owner;
    s.b1.source_id = source_id;
    vkk::b1_set_owned_blocks(s.b1.owned_blocks.data_ptr());
    s.b1.frozen_hash = _b1_context_hash();
    if (saved_contract) {
        if (saved_contract->frozen_context != s.b1.frozen_hash)
            throw std::runtime_error("B1 frozen context changed in checkpoint");
        s.b1.contract = saved_contract;
    } else {
        ckpt::B1Contract c;
        c.owner = owner;
        c.source_step = c.completed_step = checkpoint_step;
        c.rows = s.cur_num_splats;
        auto config_path = std::filesystem::path(source_checkpoint_dir) / "config.json";
        if (!std::filesystem::is_regular_file(config_path))
            config_path = std::filesystem::path(source_checkpoint_dir).parent_path() / "config.json";
        const auto config = ckpt::config_from_json(config_path);
        c.horizon = config.num_iterations;
        c.config_sha256 = ckpt::b1_config_hash(config);
        c.source_state_sha256 = ckpt::b1_file_hash(
            std::filesystem::path(source_checkpoint_dir) / "state.tar");
        c.frozen_shared_sha256 = ckpt::b1_shared_hash(source_checkpoint_dir);
        c.frozen_context = s.b1.frozen_hash;
        c.views_sha256 = c.train_indices_sha256 = ckpt::b1_indices_hash({});
        s.b1.contract = std::move(c);
    }
    std::printf("B1 owner=%d owned_blocks=%zu total_blocks=%u active_rows=%lld\n",
                owner, (size_t)std::count(mask.begin(), mask.end(), 1u),
                mask_blocks, (long long)s.cur_num_splats);
#endif
}

void engine_b1_verify_context() {
    if (engine().b1.owner != 0 && engine().b1.owner != 1) return;
    const auto after = _b1_context_hash();
    if (after != engine().b1.frozen_hash)
        throw std::runtime_error("B1 unowned parameter/optimizer state changed");
    std::printf("B1 context unchanged hash=%s\n", after.c_str());
}

void engine_b1_bind_contract(const TrainConfig& config, const std::string& views,
                            const std::vector<int32_t>& indices,
                            const std::string& source, const std::string& view_policy,
                            const std::vector<int32_t>& full_indices) {
    auto& b = engine().b1;
    if (!b.contract || b.owner < 0)
        throw std::runtime_error("B1 ownership not configured");
    auto& c = *b.contract;
    if (c.mask_sha256.empty())
        c.version = app::b1_worker_contract_version();
    const auto config_hash = ckpt::b1_config_hash(config);
    const auto views_hash = views.empty() ? ckpt::b1_indices_hash({})
                                          : ckpt::b1_file_hash(views);
    const auto indices_hash = ckpt::b1_indices_hash(indices);
    const auto full_indices_hash = ckpt::b1_indices_hash(full_indices);
    if ((view_policy != "partitioned" && view_policy != "full") ||
        (b.owner == 2 && view_policy != "full"))
        throw std::runtime_error("B1 view policy invalid");
    if (full_indices.empty() || (view_policy == "full" && indices != full_indices))
        throw std::runtime_error("B1 full training coverage changed");
    if (!c.mask_sha256.empty() &&
        (c.view_policy != view_policy ||
         (c.version >= 2 && c.full_train_indices_sha256 != full_indices_hash)))
        throw std::runtime_error("B1 owner view policy changed");
    if (c.config_sha256 != config_hash || c.horizon != config.num_iterations)
        throw std::runtime_error("B1 numerical/data configuration changed");
    if (!c.mask_sha256.empty() &&
        (c.views_sha256 != views_hash || c.train_indices_sha256 != indices_hash))
        throw std::runtime_error("B1 owner view schedule changed");
    if (ckpt::read_b1_contract(source) ||
        ckpt::b1_file_hash(std::filesystem::path(source) / "state.tar") != c.source_state_sha256 ||
        ckpt::b1_shared_hash(source) != c.frozen_shared_sha256 ||
        ckpt::read_state_json(source).get_double("step", -1) != c.source_step)
        throw std::runtime_error("B1 original source identity changed");
    if (engine().optim.use_per_splat_bias_correction) {
        std::ifstream input(std::filesystem::path(source) / "state.tar", std::ios::binary);
        const auto members = ckpt::tar_index(input);
        const auto clock = std::find_if(members.begin(), members.end(), [](const auto& m) {
            return m.name == "eng.bias_correction_steps.npy";
        });
        if (clock == members.end()) throw std::runtime_error("B1 source birth clocks missing");
        const auto info = ckpt::npy_locate(input, clock->data_offset, clock->size);
        if (info.descr != "<i4" || info.data_bytes < (uint64_t)c.rows * 4)
            throw std::runtime_error("B1 source birth clocks invalid");
        std::vector<int32_t> initial((size_t)c.rows), current((size_t)c.rows);
        input.seekg((std::streamoff)info.data_offset);
        input.read((char*)initial.data(), (std::streamsize)(initial.size() * 4));
        if (!input) throw std::runtime_error("B1 source birth clocks truncated");
        backend::memcpy_sync(current.data(), engine().optim.bias_correction_steps.data_ptr(),
                             current.size() * 4, backend::MemcpyKind::DeviceToHost);
        std::vector<uint32_t> owned((size_t)(c.rows + 255) / 256);
        backend::memcpy_sync(owned.data(), b.owned_blocks.data_ptr(),
                             owned.size() * 4, backend::MemcpyKind::DeviceToHost);
        if (const char* error = backend::last_error()) throw std::runtime_error(error);
        for (size_t i = 0; i < initial.size(); ++i) {
            const int64_t expected = (int64_t)initial[i] +
                (owned[i / 256] ? c.completed_step - c.source_step : 0);
            if (expected > INT32_MAX || current[i] != expected)
                throw std::runtime_error("B1 source-relative birth clock mismatch");
        }
    }
    c.views_sha256 = views_hash;
    c.train_indices_sha256 = indices_hash;
    if (c.version >= 2) {
        c.view_policy = view_policy;
        c.full_train_indices_sha256 = full_indices_hash;
    }
    if (c.version == 3) {
        const auto cadence = app::b1_refresh_cadence();
        const auto group = app::b1_refresh_group_hash();
        if (view_policy != "full" || b.owner > 1 || cadence <= 0 || group.size() != 64)
            throw std::runtime_error("B1 live refresh binding missing");
        if (c.mask_sha256.empty()) {
            c.refresh_cadence = cadence;
            c.refresh_generation = c.refresh_parent = c.source_step;
            c.refresh_group_sha256 = c.refresh_commit_sha256 = group;
            c.refresh_payload0_sha256 = c.refresh_payload1_sha256 = std::string(64, '0');
        } else if (c.refresh_cadence != cadence || c.refresh_group_sha256 != group ||
                   c.completed_step != c.refresh_generation) {
            throw std::runtime_error("B1 live resume is not a committed group boundary");
        }
        const auto mask = refresh_mask();
        spirula::Sha256 digest;
        for (uint32_t value : mask) {
            const uint8_t bytes[4] = {(uint8_t)value, 0, 0, 0};
            digest.update(bytes, sizeof bytes);
        }
        c.mask_sha256 = digest.hex();
    } else if (app::b1_refresh_cadence() != 0) {
        throw std::runtime_error("B1 frozen owner cannot join a live refresh group");
    }
}

std::string engine_b1_export_refresh(const std::string& path, int64_t boundary) {
    refresh_boundary(boundary);
    engine_b1_verify_context();
    auto& b = engine().b1;
    if (b.refresh_pending_boundary != -1)
        throw std::runtime_error("B1 refresh already pending");
    if (std::filesystem::exists(path))
        throw std::runtime_error("B1 refresh payload cannot replace an existing file");
    const auto mask = refresh_mask();
    const auto fields = refresh_fields();
    const auto header = refresh_header(b.owner, boundary);
    uint64_t payload_bytes = 0;
    for (const auto& field : fields) {
        const auto bytes = refresh_field_bytes(field, mask);
        if (!field.ptr || bytes > (uint64_t{1} << 30) - payload_bytes)
            throw std::runtime_error("B1 refresh payload exceeds native staging limit");
        payload_bytes += bytes;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("B1 refresh payload cannot open");
    const uint64_t header_bytes = header.size();
    refresh_write(out, &header_bytes, sizeof header_bytes);
    refresh_write(out, header.data(), header.size());
    refresh_write(out, mask.data(), mask.size() * 4);
    for (const auto& field : fields) {
        const uint64_t bytes = refresh_field_bytes(field, mask);
        refresh_write(out, &bytes, sizeof bytes);
        std::vector<uint8_t> buffer((field.block ? 1 : 256) * field.stride);
        for (size_t block = 0; block < mask.size(); ++block) {
            if (!mask[block]) continue;
            const size_t offset = field.block ? block : block * 256;
            const size_t count = field.block ? 1 : std::min(size_t{256}, field.rows - offset);
            const size_t length = count * field.stride;
            refresh_copy(buffer.data(), static_cast<uint8_t*>(field.ptr) + offset * field.stride,
                         length, backend::MemcpyKind::DeviceToHost);
            refresh_write(out, buffer.data(), length);
        }
    }
    out.close();
    if (!out) throw std::runtime_error("B1 refresh payload close failed");
    b.refresh_export_sha256 = ckpt::b1_file_hash(path);
    b.refresh_pending_boundary = boundary;
    return b.refresh_export_sha256;
}

void engine_b1_install_refresh(const std::string& path, int64_t boundary,
                               const std::string& expected_sha256) {
    refresh_boundary(boundary);
    auto& b = engine().b1;
    if (b.refresh_pending_boundary != boundary || !b.refresh_installed_sha256.empty() ||
        expected_sha256.size() != 64 || ckpt::b1_file_hash(path) != expected_sha256)
        throw std::runtime_error("B1 refresh payload identity or phase mismatch");
    engine_b1_verify_context();
    const auto owned_before = _b1_context_hash(true);
    const auto nonrender_before = _b1_context_hash(false, false);
    const auto local_mask = refresh_mask();
    const auto fields = refresh_fields();
    std::ifstream in(path, std::ios::binary);
    uint64_t header_bytes = 0;
    refresh_read(in, &header_bytes, sizeof header_bytes);
    const auto expected_header = refresh_header(1 - b.owner, boundary);
    if (header_bytes != expected_header.size())
        throw std::runtime_error("B1 refresh header length mismatch");
    std::string header((size_t)header_bytes, '\0');
    refresh_read(in, header.data(), header.size());
    if (header != expected_header) throw std::runtime_error("B1 refresh lineage mismatch");
    std::vector<uint32_t> peer_mask(local_mask.size());
    refresh_read(in, peer_mask.data(), peer_mask.size() * 4);
    for (size_t block = 0; block < peer_mask.size(); ++block)
        if (local_mask[block] > 1 || peer_mask[block] != 1 - local_mask[block])
            throw std::runtime_error("B1 refresh masks are not complementary");
    // Validate the complete packet before the first device write. Only one compact peer
    // render payload is staged; optimizer state and decoded PLY are never exchanged.
    std::vector<std::vector<uint8_t>> payloads;
    uint64_t staged = 0;
    for (const auto& field : fields) {
        uint64_t bytes = 0;
        refresh_read(in, &bytes, sizeof bytes);
        if (bytes != refresh_field_bytes(field, peer_mask) ||
            bytes > (uint64_t{1} << 30) - staged)
            throw std::runtime_error("B1 refresh field size or staging limit mismatch");
        staged += bytes;
        payloads.emplace_back((size_t)bytes);
        refresh_read(in, payloads.back().data(), (size_t)bytes);
    }
    if (in.peek() != std::char_traits<char>::eof() || ckpt::b1_file_hash(path) != expected_sha256)
        throw std::runtime_error("B1 refresh trailing bytes or changed payload");
    for (size_t index = 0; index < fields.size(); ++index) {
        const auto& field = fields[index];
        size_t position = 0;
        std::vector<uint8_t> readback((field.block ? 1 : 256) * field.stride);
        for (size_t block = 0; block < peer_mask.size(); ++block) {
            if (!peer_mask[block]) continue;
            const size_t offset = field.block ? block : block * 256;
            const size_t count = field.block ? 1 : std::min(size_t{256}, field.rows - offset);
            const size_t bytes = count * field.stride;
            auto* destination = static_cast<uint8_t*>(field.ptr) + offset * field.stride;
            const auto* source = payloads[index].data() + position;
            refresh_copy(destination, source, bytes, backend::MemcpyKind::HostToDevice);
            refresh_copy(readback.data(), destination, bytes, backend::MemcpyKind::DeviceToHost);
            if (std::memcmp(source, readback.data(), bytes))
                throw std::runtime_error("B1 refresh native peer readback mismatch");
            position += bytes;
        }
    }
    if (_b1_context_hash(true) != owned_before ||
        _b1_context_hash(false, false) != nonrender_before)
        throw std::runtime_error("B1 refresh changed owned or unowned non-render state");
    b.frozen_hash = _b1_context_hash();
    b.contract->frozen_context = b.frozen_hash;
    b.refresh_installed_sha256 = expected_sha256;
    std::printf("B1_REFRESH installed boundary=%lld peer_bytes=%llu peer_sha256=%s owned_exact=1 nonrender_exact=1\n",
                (long long)boundary, (unsigned long long)staged, expected_sha256.c_str());
}

void engine_b1_commit_refresh(int64_t boundary, const std::string& payload0,
                              const std::string& payload1, const std::string& commit) {
    refresh_boundary(boundary);
    auto& b = engine().b1;
    if (b.refresh_pending_boundary != boundary || commit.size() != 64 ||
        commit.find_first_not_of("0123456789abcdef") != std::string::npos ||
        (b.owner == 0 ? payload0 : payload1) != b.refresh_export_sha256 ||
        (b.owner == 0 ? payload1 : payload0) != b.refresh_installed_sha256)
        throw std::runtime_error("B1 refresh commit payload or phase mismatch");
    auto& c = *b.contract;
    c.refresh_parent = c.refresh_generation;
    c.refresh_generation = boundary;
    c.refresh_payload0_sha256 = payload0;
    c.refresh_payload1_sha256 = payload1;
    c.refresh_commit_sha256 = commit;
    b.refresh_pending_boundary = -1;
    b.refresh_export_sha256.clear();
    b.refresh_installed_sha256.clear();
}
