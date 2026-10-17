#include "app/ImageStripExecutor.h"

#include "app/AppPaths.h"
#include "app/Subprocess.h"
#include "core/Env.h"
#include "engine/Engine.h"
#include "engine/EngineInternal.h"
#include "engine/EngineState.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>

#if defined(_WIN32) && defined(SS_BACKEND_VULKAN)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "backend/vulkan/kernels/ImageStrip.h"
#include "shaders/screen_layout.h"
#endif

namespace app {

std::string ImageStripExecutor::requested_device() {
    const char* value = spirula::env("IMAGE_STRIP_WORKER_DEVICE");
    return value ? value : "";
}

void ImageStripExecutor::check_config(const TrainConfig& cfg) {
    if (requested_device().empty()) return;
#if defined(_WIN32) && defined(SS_BACKEND_VULKAN)
    if (cfg.primitive != "3dgs" || !cfg.packed ||
        !cfg.use_fused_proj_bwd_optim || cfg.use_bvh)
        throw std::runtime_error("image strip requires packed 3dgs with native FPBO");
    if (spirula::env("B1_OWNER") || spirula::env("B1_SOURCE") ||
        spirula::env("B2_CAUSAL_STEP") || spirula::env("M0_VISIBILITY") ||
        spirula::env("DUMP_CAMERAS") || spirula::env("G1_ORDERED_RASTER"))
        throw std::runtime_error("image strip rejects B1/B2 and debug execution modes");
    if (cfg.background_mode != "color" || cfg.background_color[0] != 0 ||
        cfg.background_color[1] != 0 || cfg.background_color[2] != 0 ||
        cfg.color_shift_reg_weight != 0 || cfg.overexposure_reg != 0)
        throw std::runtime_error("image strip requires fixed black background and no image-space regularizers");
    if (cfg.use_bilateral_grid && cfg.bilagrid_type != "ppisp")
        throw std::runtime_error("image strip supports the native ppisp RGB bilagrid only");
    if (cfg.densify_loss_map_mode == "edge_aware" ||
        cfg.densify_loss_map_mode == "robust_edge_aware")
        throw std::runtime_error("image strip does not support edge-aware loss maps");
    if (cfg.rgb_distortion_reg != 0 || cfg.depth_distortion_reg != 0 ||
        cfg.median_depth_normal_reg_weight != 0)
        throw std::runtime_error("image strip does not support scheduled distortion or depth-normal lanes");
    const auto worker = backend::device_resolve_identity(requested_device().c_str());
    const auto authority = backend::device_current_selector();
    if (worker.rfind("uuid:", 0) != 0 || authority.rfind("uuid:", 0) != 0 ||
        worker == authority)
        throw std::runtime_error("image strip requires two distinct supported Vulkan UUIDs");
    for (int i = 0; i < backend::device_count(); ++i) {
        const auto info = backend::device_info(i);
        if (info.uuid != worker && info.uuid != authority) continue;
        std::string name(info.name);
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (!info.usable || name.find("nvidia") != std::string::npos)
            throw std::runtime_error("image strip requires supported non-NVIDIA Vulkan devices");
    }
#else
    (void)cfg;
    throw std::runtime_error("image strip is supported only by Windows Vulkan builds");
#endif
}

#if defined(_WIN32) && defined(SS_BACKEND_VULKAN)
namespace {
using Clock = std::chrono::steady_clock;
double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

constexpr uint64_t kMagic = 0x5353494d47535452ULL;
constexpr uint32_t kVersion = 5;
constexpr size_t kCapacity = size_t(1) << 30;
constexpr size_t kCommitGranularity = 65536;

enum State : LONG { Idle, Ready, FrameReady, Statistics, StatisticsSum, Complete, Shutdown };
enum Buffer : uint32_t {
    Screen, GaussianIds, TileOffsets, FlattenIds, GroundTruth, Mask, Intrinsics,
    PpispParams, GridValues, GridBounds, ColorMatrix,
    ScreenResult, DensityResult, PpispResult, GridResult, OwnedRgbResult, OwnedLastIdsResult,
    Collective, BufferCount
};
struct Region { uint64_t offset = 0, bytes = 0; };
struct WorkerMetrics {
    double upload_ms = 0, gpu_ms = 0, download_ms = 0, collective_ms = 0;
    double mapping_ms = 0, prepare_ms = 0;
};
struct Frame {
    uint64_t magic = kMagic, attempt = 0, request = 0;
    uint32_t version = kVersion, header_bytes = 0, config_bytes = sizeof(EngineStepConfig);
    uint32_t screen_stride = SCR2_STRIDE;
    uint32_t logical_batch = 1, pixel_passes = 1;
    int32_t step = -1, horizon = 0;
    uint32_t width = 0, height = 0, cut = 0, halo = 0, scales = 0;
    uint32_t gt_begin = 0;
    uint32_t macro_log2 = 0, tile_height = 0, tile_width = 0;
    uint32_t rows = 0, projected = 0, intersections = 0, camera = 0;
    uint32_t has_mask = 0, has_ppisp = 0, has_grid = 0;
    uint32_t audit_pixels = 0;
    uint32_t ppisp_count = 0, grid_l = 0, grid_h = 0, grid_w = 0, grid_value_bits = 0;
    uint32_t color_enabled = 0, color_transfer = 0, color_linear = 0;
    char ppisp_type[32] = {};
    char camera_model[32] = {};
    char camera_distortion[32] = {};
    EngineStepConfig config{};
    Region buffers[BufferCount];
    uint64_t input_begin = 0, input_bytes = 0, output_begin = 0, output_bytes = 0, total_bytes = 0;
};
struct Shared {
    Frame frame;
    volatile LONG state = Idle;
    uint32_t phase = 0, phases_complete = 0;
    uint64_t statistic_count = 0;
    WorkerMetrics metrics;
};
static_assert(std::is_trivially_copyable_v<Frame>);
static_assert(sizeof(Shared) < kCommitGranularity);

[[noreturn]] void fail(const std::string& text) {
    throw std::runtime_error("image strip: " + text);
}
void checked_backend() {
    if (const char* error = backend::last_error()) fail(error);
}
uint64_t product(uint64_t a, uint64_t b) {
    if (a && b > uint64_t(kCapacity) / a) fail("buffer product exceeds the 1 GiB transport bound");
    return a * b;
}
uint64_t align64(uint64_t n) { return (n + 63) & ~uint64_t(63); }
void text_copy(char* dest, size_t capacity, const std::string& text) {
    if (text.size() >= capacity) fail("operator identifier is too long");
    std::memcpy(dest, text.c_str(), text.size() + 1);
}
void layout(Frame& f) {
    uint64_t cursor = align64(sizeof(Shared));
    f.input_begin = cursor;
    auto add = [&](Buffer id, uint64_t bytes) {
        cursor = align64(cursor);
        if (bytes > kCapacity - cursor) fail("frame exceeds the 1 GiB transport bound");
        f.buffers[id] = {cursor, bytes};
        cursor += bytes;
    };
    const uint64_t pixels = product(f.width, f.height);
    const uint64_t gt_pixels = product(f.width, f.height - f.gt_begin);
    const uint64_t cells = product(product(f.grid_l, f.grid_h), product(f.grid_w, 9));
    add(Screen, product(f.projected, SCR2_STRIDE * sizeof(float)));
    add(GaussianIds, product(f.projected, sizeof(int32_t)));
    add(TileOffsets, product(product(f.tile_height, f.tile_width), sizeof(int32_t)));
    add(FlattenIds, product(f.intersections, sizeof(int32_t)));
    add(GroundTruth, product(gt_pixels, sizeof(float3)));
    add(Mask, f.has_mask ? gt_pixels : 0);
    add(Intrinsics, sizeof(float4));
    add(PpispParams, f.has_ppisp ? product(f.ppisp_count, sizeof(float)) : 0);
    add(GridValues, f.has_grid ? product(cells, f.grid_value_bits / 8) : 0);
    add(GridBounds, f.has_grid && f.grid_value_bits == 16 ? product((cells + 255) / 256, sizeof(float2)) : 0);
    add(ColorMatrix, f.color_enabled ? 9 * sizeof(float) : 0);
    f.input_bytes = cursor - f.input_begin;
    f.output_begin = align64(cursor);
    cursor = f.output_begin;
    add(ScreenResult, f.buffers[Screen].bytes);
    add(DensityResult, f.config.loss.compute_loss_map ?
        product(f.rows, sizeof(float) * accum_lanes((DensifyAccumMode)f.config.loss.loss_map_accum_mode)) : 0);
    add(PpispResult, f.buffers[PpispParams].bytes);
    add(GridResult, f.has_grid ? product(cells, sizeof(float)) : 0);
    if (f.audit_pixels) {
        const uint64_t owned_pixels = product(f.height - f.cut, f.width);
        add(OwnedRgbResult, product(owned_pixels, sizeof(float3)));
        add(OwnedLastIdsResult, product(owned_pixels, sizeof(int32_t)));
    } else {
        f.buffers[OwnedRgbResult] = {};
        f.buffers[OwnedLastIdsResult] = {};
    }
    f.output_bytes = cursor - f.output_begin;
    const uint64_t raw_statistics = product(f.height,
        (uint64_t)RawLossIndex::length * sizeof(float));
    const uint64_t map_statistics = f.config.loss.compute_loss_map &&
        (f.config.loss.loss_map_normalize || f.config.loss.loss_map_clip_quantile < 1.0f)
        ? product(pixels, sizeof(float)) : 0;
    add(Collective, std::max<uint64_t>({4096, raw_statistics, map_statistics}));
    f.total_bytes = align64(cursor);
    if (f.total_bytes > kCapacity) fail("frame exceeds transport capacity");
}
void validate(const Frame& f, uint64_t attempt, uint64_t request, int step, int horizon) {
    if (f.magic != kMagic || f.version != kVersion || f.header_bytes != sizeof(Shared) ||
        f.config_bytes != sizeof(EngineStepConfig) || f.screen_stride != SCR2_STRIDE ||
        f.attempt != attempt || f.request != request || f.step != step ||
        f.horizon != horizon || f.step < 0 || f.step >= f.horizon)
        fail("stale or incompatible request identity");
    if (f.logical_batch != 1 || f.pixel_passes != 1 ||
        f.scales != (uint32_t)engine_resolve_num_loss_scales(
            f.config.loss.num_loss_scales, f.config.loss.loss_scale_min_pixels,
            f.height, f.width))
        fail("logical batch, pass count, or pyramid differs from the native request");
    if (!f.width || !f.height || f.width > INT32_MAX || f.height > INT32_MAX ||
        product(f.width, f.height) > INT32_MAX / 3 || f.rows > INT32_MAX ||
        f.projected > INT32_MAX / SCR2_STRIDE || f.intersections > INT32_MAX ||
        f.macro_log2 < kMacroLog2Min || f.macro_log2 > kMacroLog2Max ||
        f.scales < 1 || f.scales > 4 || f.cut > f.height ||
        (f.cut != f.height && f.cut % 64) ||
        f.halo != backend::vk::image_strip_halo(f.scales) ||
        f.gt_begin != (f.cut == f.height ? f.height :
            f.cut > f.halo ? f.cut - f.halo : 0) ||
        f.tile_height != (f.height + bin_tile_y((int)f.macro_log2) - 1) /
                         bin_tile_y((int)f.macro_log2) ||
        f.tile_width != (f.width + bin_tile_x((int)f.macro_log2) - 1) /
                        bin_tile_x((int)f.macro_log2) ||
        (uint64_t(f.tile_height) << f.macro_log2) > 65535 ||
        (uint64_t(f.tile_width) << f.macro_log2) > 65535)
        fail("invalid prepared frame dimensions or bin layout");
    if (f.has_mask > 1 || f.has_ppisp > 1 || f.has_grid > 1 || f.color_enabled > 1 ||
        f.color_linear > 1 || f.audit_pixels > 1 ||
        !std::memchr(f.ppisp_type, 0, sizeof(f.ppisp_type)) ||
        !std::memchr(f.camera_model, 0, sizeof(f.camera_model)) ||
        !std::memchr(f.camera_distortion, 0, sizeof(f.camera_distortion)))
        fail("invalid operator metadata");
    if (f.has_ppisp && (f.ppisp_count != (uint32_t)ppisp_param_spec(f.ppisp_type).num_params))
        fail("PPISP parameter layout differs from the native operator");
    if (f.has_grid && (!f.grid_l || !f.grid_h || !f.grid_w ||
        (f.grid_value_bits != 16 && f.grid_value_bits != 32) ||
        (f.grid_value_bits == 16 && product(product(f.grid_l, f.grid_h), product(f.grid_w, 9)) % 256)))
        fail("bilagrid camera slice crosses a quantization block");
    const auto& c = f.config;
    if (!c.optim.use_fused_proj_bwd_optim || c.loss.color_shift_reg_weight != 0 ||
        c.loss.overexposure_reg_weight != 0 || c.loss.loss_map_mode == 5 ||
        c.loss.loss_map_mode == 6 || c.loss.loss_map_accum_mode < (int)DensifyAccumMode::Max ||
        c.loss.loss_map_accum_mode > (int)DensifyAccumMode::Avg ||
        c.loss.weights[(int)LossWeightIndex::RgbDistReg] != 0 ||
        c.loss.weights[(int)LossWeightIndex::DepthDistReg] != 0 ||
        c.loss.weights[(int)LossWeightIndex::MedianDepthNormalReg] != 0)
        fail("unsupported resolved pixel operator configuration");
    Frame expected = f;
    layout(expected);
    if (expected.input_begin != f.input_begin || expected.input_bytes != f.input_bytes ||
        expected.output_begin != f.output_begin || expected.output_bytes != f.output_bytes ||
        expected.total_bytes != f.total_bytes)
        fail("invalid frame extents");
    for (uint32_t i = 0; i < BufferCount; ++i)
        if (expected.buffers[i].offset != f.buffers[i].offset ||
            expected.buffers[i].bytes != f.buffers[i].bytes)
            fail("invalid buffer offset or byte count");
}

uint32_t frame_collective_phases(const Frame& frame) {
    return frame.scales + 1 + (frame.config.loss.compute_loss_map &&
        (frame.config.loss.loss_map_normalize || frame.config.loss.loss_map_clip_quantile < 1.0f));
}
size_t frame_collective_count(const Frame& frame, uint32_t phase) {
    if (!phase || phase > frame_collective_phases(frame))
        fail("collective phase exceeds the native pixel pipeline");
    if (phase == 2) return 1;
    if (phase > frame.scales + 1) return (size_t)frame.width * frame.height;
    const uint32_t scale = phase > 2 ? phase - 2 : 0;
    return (size_t)std::max(1u, frame.height >> scale) * (size_t)RawLossIndex::length;
}

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE v) : value(v) { if (!value) fail("cannot create IPC handle"); }
    ~Handle() { if (value) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct Mapping {
    Handle handle;
    char* data = nullptr;
    size_t committed = 0;
    explicit Mapping(HANDLE h) : handle(h) {
        data = (char*)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, kCapacity);
        if (!data) fail("cannot map bounded shared storage");
    }
    ~Mapping() { if (data) UnmapViewOfFile(data); }
    void commit(size_t bytes) {
        const size_t needed = (bytes + kCommitGranularity - 1) & ~(kCommitGranularity - 1);
        if (needed > kCapacity) fail("mapping commit exceeds capacity");
        if (needed <= committed) return;
        MEMORYSTATUSEX memory{sizeof(memory)};
        if (!GlobalMemoryStatusEx(&memory) || needed - committed > memory.ullAvailPageFile)
            fail("insufficient system commit for strip mapping");
        if (!VirtualAlloc(data + committed, needed - committed, MEM_COMMIT, PAGE_READWRITE))
            fail("cannot commit shared frame storage");
        committed = needed;
    }
    Shared& shared() const { return *reinterpret_cast<Shared*>(data); }
};
struct Pinned {
    char* data = nullptr;
    size_t capacity = 0;
    ~Pinned() { if (data) backend::host_free_pinned(data); }
    void reserve(size_t n) {
        if (n <= capacity) return;
        if (n > kCapacity) fail("pinned staging exceeds capacity");
        if (data) backend::host_free_pinned(data);
        data = nullptr;
        capacity = 0;
        data = (char*)backend::host_malloc_pinned(n);
        if (!data) backend::throw_out_of_memory(n, "image strip staging");
        capacity = n;
    }
};
struct GpuClock {
    backend::Event* start = backend::event_create(true);
    backend::Event* stop = backend::event_create(true);
    backend::Event* copied = backend::event_create(false);
    double native_ms = 0;
    ~GpuClock() {
        backend::event_destroy(copied);
        backend::event_destroy(stop);
        backend::event_destroy(start);
    }
    void begin() { backend::event_record(start); }
    void end() {
        backend::event_record(stop); // Submits the native GPU batch before any CPU join.
        backend::event_synchronize(stop);
        native_ms += backend::event_elapsed_ms(start, stop);
        checked_backend();
    }
    void fence() {
        backend::event_record(copied);
        backend::event_synchronize(copied);
        checked_backend();
    }
};
void signal(HANDLE event) { if (!SetEvent(event)) fail("cannot publish IPC event"); }
void wait_child(HANDLE ready, HANDLE exited, HANDLE root_lost) {
    HANDLE handles[] = {root_lost, exited, ready};
    const DWORD result = WaitForMultipleObjects(3, handles, FALSE, INFINITE);
    if (result == WAIT_OBJECT_0) fail("training controller disconnected");
    if (result == WAIT_OBJECT_0 + 1) fail("pixel worker exited before the result was consumed");
    if (result != WAIT_OBJECT_0 + 2) fail("IPC wait failed");
}
void wait_parent(HANDLE event, HANDLE parent) {
    HANDLE handles[] = {parent, event};
    const DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    if (result == WAIT_OBJECT_0) fail("authority process exited");
    if (result != WAIT_OBJECT_0 + 1) fail("authority event wait failed");
}
std::string handle_text(HANDLE handle) { return std::to_string((uintptr_t)handle); }
HANDLE parse_handle(const char* text) {
    if (!text || text[0] < '0' || text[0] > '9') fail("invalid inherited handle");
    char* end = nullptr;
    errno = 0;
    const auto value = std::strtoull(text, &end, 10);
    if (errno || !value || !end || end == text || *end || value > UINTPTR_MAX)
        fail("invalid inherited handle");
    HANDLE h = (HANDLE)(uintptr_t)value;
    if (!SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0)) fail("inherited handle is not owned");
    return h;
}
void* device_region(Buffer id, size_t bytes) {
    return DevicePool::global().acquire_dynamic(VramCategory::Other,
        "image_strip.input." + std::to_string(id), bytes);
}
TorchTensorView view(void* ptr, uint32_t bytes, std::vector<int64_t> shape) {
    return {(uint64_t)(uintptr_t)ptr, bytes, std::move(shape)};
}
DeviceVector<float> floats(float* ptr, size_t count) {
    return DeviceVector<float>(view(ptr, sizeof(float), {(int64_t)count}));
}
void finite(const float* values, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (!std::isfinite(values[i])) fail("nonfinite worker gradient or raw statistic");
}
void checked_last_ids(const int* values, size_t count, uint32_t intersections) {
    static_assert(sizeof(int) == sizeof(int32_t));
    // Native empty pixels retain 0; backward also accepts the -1 sentinel.
    const int upper = (int)std::max(1u, intersections);
    for (size_t i = 0; i < count; ++i)
        if (values[i] < -1 || values[i] >= upper)
            fail("worker last-ID audit contains an invalid native flattened index");
}
}  // namespace

struct ImageStripExecutor::Impl {
    Clock::time_point startup = Clock::now();
    Handle request_event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    Handle worker_event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    Handle sum_event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    Handle exited{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle root_lost{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle watcher_done{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle parent{OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId())};
    Mapping mapping{CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE | SEC_RESERVE, 0, (DWORD)kCapacity, nullptr)};
    Pinned staging;
    GpuClock gpu;
    DeviceVector<float> remote_result;
    DeviceVector<uint32_t> invalid_result;
    std::thread supervisor, watcher;
    std::atomic<bool> cancel{false};
    app::proc::ProcessResult outcome;
    const std::atomic<bool>& controller_disconnected;
    Frame frame;
    uint64_t attempt = 0, request = 0, collective_bytes = 0;
    uint32_t phase = 0;
    int horizon = 0, step = -1, last_step = -1;
    bool in_step = false, pending = false, failed = false, dispatched = false;
    double snapshot_ms = 0, map_ms = 0, collective_ms = 0, join_ms = 0, join_wait_ms = 0;
    Clock::time_point step_start;

    Impl(const TrainConfig& cfg, const std::atomic<bool>& disconnected)
        : controller_disconnected(disconnected), horizon(cfg.num_iterations) {
        mapping.commit(sizeof(Shared));
        new (mapping.data) Shared{};
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        attempt = uint64_t(counter.QuadPart) ^ (uint64_t(GetCurrentProcessId()) << 32);
        if (!attempt) attempt = 1;
        mapping.shared().frame.attempt = attempt;
        mapping.shared().frame.horizon = horizon;
        mapping.shared().frame.header_bytes = sizeof(Shared);
        const std::string worker = backend::device_resolve_identity(requested_device().c_str());
        app::proc::ProcessOptions options;
        options.argv = {app::exe_path(), "worker", "--image-strip", handle_text(mapping.handle.value),
            handle_text(request_event.value), handle_text(worker_event.value),
            handle_text(sum_event.value), handle_text(parent.value), std::to_string(attempt),
            std::to_string(horizon), worker};
        if (options.argv[0].empty()) fail("cannot resolve this executable");
        options.inherit_handles = {mapping.handle.value, request_event.value, worker_event.value,
                                    sum_event.value, parent.value};
        options.env_overrides = {{"SS_IMAGE_STRIP_WORKER_DEVICE", ""}, {"SS_WORKER_CONTROL", "1"}};
        options.cancel = &cancel;
        options.on_line = [](const std::string& line) {
            std::fprintf(stderr, "[image-strip-worker] %s\n", line.c_str());
        };
        try {
            supervisor = std::thread([this, options] {
                try { outcome = app::proc::run_process(options); }
                catch (const std::exception& e) { outcome.error_message = e.what(); }
                SetEvent(exited.value);
            });
            watcher = std::thread([this] {
                while (WaitForSingleObject(watcher_done.value, 25) == WAIT_TIMEOUT) {
                    if (!controller_disconnected.load()) continue;
                    cancel.store(true);
                    SetEvent(root_lost.value);
                    return;
                }
            });
            wait_worker();
            if (InterlockedCompareExchange(&mapping.shared().state, Ready, Ready) != Ready ||
                mapping.shared().frame.attempt != attempt)
                fail("worker did not publish matching readiness");
            InterlockedExchange(&mapping.shared().state, Idle);
            engine().image_strip_fork = [this](int s, const EngineStepConfig& c) { fork(s, c); };
            engine().image_strip_join = [this](std::vector<DeviceTensorFloatND>& v) { join(v); };
            std::fprintf(stderr,
                "[image-strip] attempt=%llu authority=%s worker=%s mapping_capacity=%zu "
                "ready_ms=%.3f world_transfer_bytes=0 optimizer_worker=0\n",
                (unsigned long long)attempt, backend::device_current_selector().c_str(),
                worker.c_str(), kCapacity, milliseconds(startup));
        } catch (...) {
            engine().image_strip_fork = {};
            engine().image_strip_join = {};
            engine().image_strip_config = nullptr;
            shutdown(true);
            throw;
        }
    }
    ~Impl() {
        engine().image_strip_fork = {};
        engine().image_strip_join = {};
        engine().image_strip_config = nullptr;
        backend::vk::set_image_strip({});
        shutdown(failed || pending || in_step);
        backend::device_synchronize();
    }
    void shutdown(bool abort) noexcept {
        if (supervisor.joinable()) {
            if (abort || WaitForSingleObject(exited.value, 0) == WAIT_OBJECT_0) cancel.store(true);
            else {
                InterlockedExchange(&mapping.shared().state, Shutdown);
                SetEvent(request_event.value);
            }
            supervisor.join();
        }
        SetEvent(watcher_done.value);
        if (watcher.joinable()) watcher.join();
        if (!abort && (outcome.outcome != app::proc::ProcessOutcome::Success || outcome.exit_code != 0))
            std::fprintf(stderr, "[image-strip] worker shutdown exit=%d outcome=%d %s\n",
                outcome.exit_code, (int)outcome.outcome, outcome.error_message.c_str());
    }
    void check_alive() const {
        if (controller_disconnected.load()) fail("training controller disconnected");
        if (WaitForSingleObject(exited.value, 0) == WAIT_OBJECT_0)
            fail("pixel worker exit=" + std::to_string(outcome.exit_code) +
                 " outcome=" + std::to_string((int)outcome.outcome) + " " + outcome.error_message);
    }
    void wait_worker() const {
        try { wait_child(worker_event.value, exited.value, root_lost.value); }
        catch (...) { check_alive(); throw; }
        check_alive();
    }
    void begin(int s) {
        if (failed || pending || in_step || (last_step >= 0 && s != last_step + 1))
            fail("session is failed, pending, or has a discontinuous logical step");
        check_alive();
        in_step = true;
        dispatched = false;
        step = s;
        phase = 0;
        collective_bytes = 0;
        gpu.native_ms = snapshot_ms = map_ms = collective_ms = join_ms = join_wait_ms = 0;
        step_start = Clock::now();
    }
    void abandon_undispatched_step() {
        if (failed || !in_step || pending || dispatched || phase != 0 ||
            InterlockedCompareExchange(&mapping.shared().state, Idle, Idle) != Idle ||
            backend::vk::image_strip().full_height || engine().image_strip_config)
            fail("cannot abandon an active, dispatched, or failed logical step");
        check_alive();
        in_step = false;
        step = -1;
    }
    void fork(int s, const EngineStepConfig& config) {
        if (!in_step || pending || dispatched || s != step)
            fail("prepared frame is outside its one-pass logical step");
        auto& e = engine();
        if (e.camera.num != 1 || e.primitive != "3dgs" || !e.packed || e.b1.owner >= 0 ||
            e.background.enabled || e.bilagrid_depth.enabled || e.bilagrid_normal.enabled ||
            e.gt.depth.data_ptr() || e.gt.normal.data_ptr() || !e.gt.has_gt ||
            e.fwd.splats_s.size() != 1 || e.fwd.splats_s[0].get_ndim() != 2 ||
            e.fwd.splats_s[0].size(1) != SCR2_STRIDE ||
            (e.bilagrid_rgb.enabled && e.bilagrid_rgb.type != "ppisp"))
            fail("unsupported resolved native prepared-frame lane");
        if (e.cur_num_splats < 0 || e.cur_num_splats > INT32_MAX ||
            e.fwd.splats_s[0].size(0) < 0 ||
            e.fwd.splats_s[0].size(0) > INT32_MAX / SCR2_STRIDE ||
            e.fwd.flatten_ids.size() > INT32_MAX)
            fail("native prepared frame exceeds supported index extents");
        if (e.camera.width <= 0 || e.camera.height <= 0 ||
            e.camera.intrins.size() != 1 || e.fwd.tile_offsets.size<0>() != 1 ||
            e.fwd.gaussian_ids.size() != e.fwd.splats_s[0].size(0) ||
            e.gt.rgb.size<0>() != 1 || e.gt.rgb.size<1>() != e.camera.height ||
            e.gt.rgb.size<2>() != e.camera.width ||
            (e.gt.has_mask && (e.gt.alpha.size<0>() != 1 ||
                e.gt.alpha.size<1>() != e.camera.height || e.gt.alpha.size<2>() != e.camera.width)))
            fail("prepared camera, RGB, mask, or packed-row shapes differ");
        if (!e.gt.rgb.data_ptr() || (e.gt.has_mask && !e.gt.alpha.data_ptr()))
            fail("missing native GT RGB or mask storage");
        Frame f{};
        f.header_bytes = sizeof(Shared);
        f.attempt = attempt;
        f.request = ++request;
        f.step = step;
        f.horizon = horizon;
        f.config = config;
        f.audit_pixels = (bool)e.image_strip_audit_pixels;
        f.width = e.camera.width;
        f.height = e.camera.height;
        f.scales = engine_resolve_num_loss_scales(config.loss.num_loss_scales,
            config.loss.loss_scale_min_pixels, f.height, f.width);
        f.cut = std::min(f.height, ((f.height / 2 + 32) / 64) * 64);
        f.halo = backend::vk::image_strip_halo(f.scales);
        f.gt_begin = f.cut == f.height ? f.height :
            f.cut > f.halo ? f.cut - f.halo : 0;
        f.macro_log2 = e.fwd.macro_log2;
        f.tile_height = (uint32_t)e.fwd.tile_offsets.size<1>();
        f.tile_width = (uint32_t)e.fwd.tile_offsets.size<2>();
        f.rows = (uint32_t)e.cur_num_splats;
        f.projected = (uint32_t)e.fwd.splats_s[0].size(0);
        f.intersections = (uint32_t)e.fwd.flatten_ids.size();
        f.has_mask = e.gt.has_mask;
        f.has_ppisp = e.ppisp.enabled;
        f.has_grid = e.bilagrid_rgb.enabled;
        f.ppisp_count = f.has_ppisp ? e.ppisp.num_params : 0;
        f.color_enabled = e.color_space.splat_enabled;
        f.color_transfer = e.color_space.splat_transfer;
        f.color_linear = e.color_space.splat_is_linear;
        text_copy(f.ppisp_type, sizeof(f.ppisp_type), e.ppisp.param_type);
        text_copy(f.camera_model, sizeof(f.camera_model), e.camera.model_str);
        text_copy(f.camera_distortion, sizeof(f.camera_distortion), e.camera.distortion_str);
        const auto snapshot_start = Clock::now();
        int32_t camera = 0;
        if (e.bilagrid_cur_cam_indices.data_ptr()) {
            if (e.bilagrid_cur_cam_indices.size() != 1) fail("invalid native camera-index extent");
            backend::memcpy_sync(&camera, e.bilagrid_cur_cam_indices.data_ptr(), sizeof(camera),
                                  backend::MemcpyKind::DeviceToHost);
        }
        if (camera < 0 || (f.has_ppisp && camera >= e.ppisp.params.size<0>()) ||
            (f.has_grid && camera >= e.bilagrid_rgb.grids.size<0>())) fail("camera index out of range");
        f.camera = (uint32_t)camera;
        if (f.has_grid) {
            f.grid_l = (uint32_t)e.bilagrid_rgb.grids.size<1>();
            f.grid_h = (uint32_t)e.bilagrid_rgb.grids.size<2>();
            f.grid_w = (uint32_t)e.bilagrid_rgb.grids.size<3>();
            f.grid_value_bits = e.bilagrid_rgb.value_bits;
        }
        layout(f);
        validate(f, attempt, request, step, horizon);
        mapping.commit((size_t)f.total_bytes);
        staging.reserve((size_t)std::max({f.input_bytes, f.output_bytes, f.buffers[Collective].bytes}));
        const uint64_t cells = uint64_t(f.grid_l) * f.grid_h * f.grid_w * 9;
        auto copy = [&](Buffer id, const void* ptr) {
            const Region r = f.buffers[id];
            if (!r.bytes) return;
            if (!ptr) fail("missing native input buffer");
            backend::memcpy_async(staging.data + r.offset - f.input_begin, ptr, (size_t)r.bytes,
                                  backend::MemcpyKind::DeviceToHost);
        };
        copy(Screen, e.fwd.splats_s[0].data_ptr());
        copy(GaussianIds, e.fwd.gaussian_ids.data_ptr());
        copy(TileOffsets, e.fwd.tile_offsets.data_ptr());
        copy(FlattenIds, e.fwd.flatten_ids.data_ptr());
        const size_t gt_offset = (size_t)f.gt_begin * f.width;
        copy(GroundTruth, e.gt.rgb.data_ptr() + gt_offset);
        copy(Mask, e.gt.alpha.data_ptr() ? e.gt.alpha.data_ptr() + gt_offset : nullptr);
        copy(Intrinsics, e.camera.intrins.data_ptr());
        copy(PpispParams, f.has_ppisp ? e.ppisp.params.data_ptr() + camera * f.ppisp_count : nullptr);
        if (f.has_grid && f.grid_value_bits == 16) {
            copy(GridValues, e.bilagrid_rgb.grids_quant.packed_ptr() + camera * cells * 2);
            copy(GridBounds, e.bilagrid_rgb.grids_quant.bounds_ptr() + camera * cells / 256);
        } else if (f.has_grid) copy(GridValues, e.bilagrid_rgb.grids.data_ptr() + camera * cells);
        copy(ColorMatrix, e.color_space.splat_color_matrix.data_ptr());
        gpu.fence();
        snapshot_ms = milliseconds(snapshot_start);
        const auto map_start = Clock::now();
        std::memcpy(mapping.data + f.input_begin, staging.data, (size_t)f.input_bytes);
        std::memcpy(&mapping.shared().frame, &f, sizeof(f));
        mapping.shared().metrics = {};
        mapping.shared().phase = mapping.shared().phases_complete = 0;
        mapping.shared().statistic_count = 0;
        std::memcpy(&frame, &f, sizeof(f));
        pending = true;
        dispatched = true;
        InterlockedExchange(&mapping.shared().state, FrameReady);
        signal(request_event.value);
        map_ms = milliseconds(map_start);
        backend::vk::set_image_strip({f.height, 0, f.cut, f.halo,
            [this](float* values, size_t count) { sum(values, count); }});
        gpu.begin();
    }
    void same_frame() const {
        if (std::memcmp(&mapping.shared().frame, &frame, sizeof(Frame)) != 0)
            fail("request metadata changed before consumption");
    }
    void sum(float* values, size_t count) {
        gpu.end();
        const auto start = Clock::now();
        if (!pending || count > frame.buffers[Collective].bytes / sizeof(float))
            fail("raw-statistic collective exceeds its frame slot");
        const uint32_t next = phase + 1;
        if (count != frame_collective_count(frame, next))
            fail("native collective phase or statistic layout differs from the protocol");
        const size_t bytes = count * sizeof(float);
        backend::memcpy_async(staging.data, values, bytes, backend::MemcpyKind::DeviceToHost);
        gpu.fence();
        wait_worker();
        same_frame();
        auto& shared = mapping.shared();
        if (InterlockedCompareExchange(&shared.state, Statistics, Statistics) != Statistics ||
            shared.phase != ++phase || shared.statistic_count != count)
            fail("collective phase or shape differs between strip roles");
        auto* a = reinterpret_cast<float*>(staging.data);
        const auto* b = reinterpret_cast<const float*>(mapping.data + frame.buffers[Collective].offset);
        finite(a, count);
        finite(b, count);
        for (size_t i = 0; i < count; ++i) a[i] += b[i];
        finite(a, count);
        std::memcpy(mapping.data + frame.buffers[Collective].offset, a, bytes);
        InterlockedExchange(&shared.state, StatisticsSum);
        signal(sum_event.value);
        backend::memcpy_async(values, a, bytes, backend::MemcpyKind::HostToDevice);
        gpu.fence();
        collective_bytes += 2 * bytes;
        collective_ms += milliseconds(start);
        gpu.begin();
    }
    void join(std::vector<DeviceTensorFloatND>& screen) {
        if (!pending || screen.size() != 1 ||
            screen[0].numel() * sizeof(float) != frame.buffers[ScreenResult].bytes)
            fail("native screen-gradient layout differs from the request");
        gpu.end();
        const auto start = Clock::now();
        wait_worker();
        join_wait_ms = milliseconds(start);
        same_frame();
        auto& shared = mapping.shared();
        if (InterlockedCompareExchange(&shared.state, Complete, Complete) != Complete ||
            shared.phases_complete != phase || phase != frame_collective_phases(frame))
            fail("worker did not complete every collective and raster pass");
        std::memcpy(staging.data, mapping.data + frame.output_begin, (size_t)frame.output_bytes);
        if (frame.audit_pixels) {
            const Region r = frame.buffers[OwnedRgbResult];
            const Region ids_region = frame.buffers[OwnedLastIdsResult];
            const size_t pixels = (size_t)(frame.height - frame.cut) * frame.width;
            const size_t count = pixels * 3;
            if (r.bytes != count * sizeof(float) || ids_region.bytes != pixels * sizeof(int32_t))
                fail("worker pixel audit extents differ from owned rows");
            const auto* rgb = reinterpret_cast<const float*>(
                staging.data + r.offset - frame.output_begin);
            const auto* last_ids = reinterpret_cast<const int*>(
                staging.data + ids_region.offset - frame.output_begin);
            finite(rgb, count);
            checked_last_ids(last_ids, pixels, frame.intersections);
            const auto& audit = engine().image_strip_audit_pixels;
            if (!audit) fail("pixel audit callback was removed before consumption");
            audit(rgb, count, last_ids, pixels);
        }
        remote_result.resize_dynamic(VramCategory::Other, "image_strip.merge",
            (int64_t)((frame.buffers[GridResult].offset +
                       frame.buffers[GridResult].bytes - frame.output_begin) / sizeof(float)));
        invalid_result.resize_dynamic(VramCategory::Other, "image_strip.invalid", 1);
        backend::memset_async(invalid_result.data_ptr(), 0, sizeof(uint32_t));
        auto merge = [&](Buffer id, float* dest, size_t count, bool maximum = false) {
            const Region r = frame.buffers[id];
            if (r.bytes != count * sizeof(float) || (count && !dest))
                fail("native merge buffer extent differs from worker output");
            if (!count) return;
            auto* host = reinterpret_cast<float*>(staging.data + r.offset - frame.output_begin);
            finite(host, count);
            auto* device = remote_result.data_ptr() + (r.offset - frame.output_begin) / sizeof(float);
            backend::memcpy_async(device, host, (size_t)r.bytes, backend::MemcpyKind::HostToDevice);
            if (maximum) {
                backend::vk::image_strip_check_finite(dest, count, invalid_result.data_ptr());
                float_max_into(floats(dest, count), floats(device, count), (int64_t)count);
            } else float_add_into(floats(dest, count), floats(device, count), (int64_t)count);
            backend::vk::image_strip_check_finite(dest, count, invalid_result.data_ptr());
        };
        merge(ScreenResult, screen[0].data_ptr(), (size_t)screen[0].numel());
        auto& e = engine();
        merge(DensityResult, e.fwd.accum_weight.data_ptr(),
              frame.buffers[DensityResult].bytes ? (size_t)e.fwd.accum_weight.size() : 0,
              e.fwd.accum_mode == DensifyAccumMode::Max);
        merge(PpispResult, frame.has_ppisp ? e.ppisp.grads.data_ptr() + frame.camera * frame.ppisp_count : nullptr,
              frame.has_ppisp ? frame.ppisp_count : 0);
        merge(GridResult, e.bilagrid_rgb.image_grad.data_ptr(),
              frame.has_grid ? (size_t)e.bilagrid_rgb.image_grad.numel() : 0);
        backend::memcpy_async(staging.data, invalid_result.data_ptr(), sizeof(uint32_t),
                              backend::MemcpyKind::DeviceToHost);
        gpu.fence(); // Mapping and pinned output stay owned until the upload is consumed.
        if (*reinterpret_cast<const uint32_t*>(staging.data))
            fail("merged native screen, density, or appearance gradient is nonfinite");
        backend::vk::set_image_strip({});
        pending = false;
        InterlockedExchange(&shared.state, Idle);
        join_ms = milliseconds(start);
    }
    void finish() {
        if (failed || !in_step || pending || !dispatched) fail("logical update is incomplete");
        gpu.fence();
        check_alive();
        const auto& m = mapping.shared().metrics;
        std::fprintf(stderr,
            "[image-strip] attempt=%llu request=%llu step=%d horizon=%d camera=%u width=%u height=%u cut=%u halo=%u gt_begin=%u gt_rows=%u "
            "rows=%u projected=%u intersections=%u macro_log2=%u input_bytes=%llu output_bytes=%llu "
            "collectives=%u collective_bytes=%llu snapshot_ms=%.3f mapping_ms=%.3f authority_pixel_gpu_ms=%.3f "
            "worker_h2d_ms=%.3f worker_pixel_gpu_ms=%.3f worker_d2h_ms=%.3f worker_mapping_ms=%.3f worker_prepare_ms=%.3f "
            "authority_collective_ms=%.3f worker_collective_ms=%.3f join_wait_ms=%.3f join_ms=%.3f whole_step_ms=%.3f complete=1\n",
            (unsigned long long)attempt, (unsigned long long)request, step, horizon,
            frame.camera, frame.width, frame.height,
            frame.cut, frame.halo, frame.gt_begin, frame.height - frame.gt_begin,
            frame.rows, frame.projected, frame.intersections, frame.macro_log2,
            (unsigned long long)frame.input_bytes, (unsigned long long)frame.output_bytes,
            phase, (unsigned long long)collective_bytes, snapshot_ms, map_ms, gpu.native_ms,
            m.upload_ms, m.gpu_ms, m.download_ms, m.mapping_ms, m.prepare_ms,
            collective_ms, m.collective_ms, join_wait_ms, join_ms, milliseconds(step_start));
        last_step = step;
        in_step = false;
    }
};

ImageStripExecutor::ImageStripExecutor(const TrainConfig& cfg,
    const std::atomic<bool>& disconnected) {
    check_config(cfg);
    impl_ = std::make_unique<Impl>(cfg, disconnected);
}
ImageStripExecutor::~ImageStripExecutor() = default;
void ImageStripExecutor::begin_step(int step) { impl_->begin(step); }
void ImageStripExecutor::abandon_undispatched_step() {
    impl_->abandon_undispatched_step();
}
void ImageStripExecutor::finish_step() { impl_->finish(); }
void ImageStripExecutor::require_checkpointable() const {
    if (impl_->failed || impl_->pending || impl_->in_step) fail("cannot save a partial or poisoned session");
    impl_->check_alive();
}
void ImageStripExecutor::poison() noexcept {
    impl_->failed = true;
    impl_->cancel.store(true);
    engine().image_strip_config = nullptr;
    backend::vk::set_image_strip({});
}

int image_strip_worker_main(int argc, char** argv) {
    try {
        if (argc != 10) fail("invalid hidden worker arguments");
        Mapping mapping(parse_handle(argv[2]));
        Handle request(parse_handle(argv[3])), ready(parse_handle(argv[4])), sum_ready(parse_handle(argv[5]));
        Handle parent(parse_handle(argv[6]));
        size_t attempt_chars = 0, horizon_chars = 0;
        const uint64_t attempt = std::stoull(argv[7], &attempt_chars);
        const int horizon = std::stoi(argv[8], &horizon_chars);
        if (!attempt || horizon <= 0 || argv[7][0] < '0' || argv[7][0] > '9' ||
            argv[8][0] < '0' || argv[8][0] > '9' ||
            attempt_chars != std::strlen(argv[7]) || horizon_chars != std::strlen(argv[8]) ||
            std::string(argv[9]).rfind("uuid:", 0) != 0)
            fail("invalid worker attempt, horizon, or UUID");
        Handle watcher_done(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        std::thread watcher([&] {
            HANDLE handles[] = {watcher_done.value, parent.value};
            for (;;) {
                const DWORD result = WaitForMultipleObjects(2, handles, FALSE, 25);
                if (result == WAIT_OBJECT_0) return;
                if (result == WAIT_OBJECT_0 + 1 || result == WAIT_FAILED)
                    TerminateProcess(GetCurrentProcess(), 43);
                DWORD available = 0;
                if (!PeekNamedPipe(GetStdHandle(STD_INPUT_HANDLE), nullptr, 0, nullptr, &available, nullptr))
                    TerminateProcess(GetCurrentProcess(), 43);
            }
        });
        struct WatcherGuard {
            HANDLE done;
            std::thread& thread;
            ~WatcherGuard() { SetEvent(done); if (thread.joinable()) thread.join(); }
        } watcher_guard{watcher_done.value, watcher};
        if (!backend::device_select_identity(argv[9])) fail(backend::device_selection_error());
        engine_reset();
        engine().image_strip_worker = true;
        engine().optim.use_fused_proj_bwd_optim = true;
        Pinned staging;
        GpuClock gpu;
        auto& shared = mapping.shared();
        if (shared.frame.magic != kMagic || shared.frame.version != kVersion ||
            shared.frame.config_bytes != sizeof(EngineStepConfig) ||
            shared.frame.screen_stride != SCR2_STRIDE ||
            shared.frame.attempt != attempt || shared.frame.horizon != horizon ||
            shared.frame.header_bytes != sizeof(Shared)) fail("worker bootstrap identity mismatch");
        InterlockedExchange(&shared.state, Ready);
        std::fprintf(stderr, "ready pid=%lu device=%s attempt=%llu horizon=%d\n",
            (unsigned long)GetCurrentProcessId(), backend::device_current_selector().c_str(),
            (unsigned long long)attempt, horizon);
        std::fflush(stderr);
        signal(ready.value);
        uint64_t next_request = 1;
        int previous_step = -1;
        for (;;) {
            wait_parent(request.value, parent.value);
            const LONG state = InterlockedCompareExchange(&shared.state, Idle, Idle);
            if (state == Shutdown) break;
            if (state != FrameReady) fail("worker received a non-frame request");
            Frame frame;
            std::memcpy(&frame, &shared.frame, sizeof(frame));
            validate(frame, attempt, next_request, frame.step, horizon);
            if (previous_step >= 0 && frame.step != previous_step + 1) fail("discontinuous worker logical step");
            staging.reserve((size_t)std::max({frame.input_bytes, frame.output_bytes, frame.buffers[Collective].bytes}));
            const auto upload_start = Clock::now();
            WorkerMetrics metrics;
            const auto mapping_start = Clock::now();
            std::memcpy(staging.data, mapping.data + frame.input_begin, (size_t)frame.input_bytes);
            metrics.mapping_ms = milliseconds(mapping_start);
            auto host = [&](Buffer id) { return staging.data + frame.buffers[id].offset - frame.input_begin; };
            const int32_t* ids = reinterpret_cast<const int32_t*>(host(GaussianIds));
            for (uint32_t i = 0; i < frame.projected; ++i)
                if (ids[i] < 0 || (uint32_t)ids[i] >= frame.rows) fail("invalid world ID in prepared frame");
            ids = reinterpret_cast<const int32_t*>(host(FlattenIds));
            for (uint32_t i = 0; i < frame.intersections; ++i)
                if (ids[i] < 0 || (uint32_t)ids[i] >= frame.projected) fail("invalid projected intersection ID");
            ids = reinterpret_cast<const int32_t*>(host(TileOffsets));
            int32_t previous_offset = 0;
            for (uint64_t i = 0; i < uint64_t(frame.tile_height) * frame.tile_width; ++i) {
                if (ids[i] < previous_offset || (uint32_t)ids[i] > frame.intersections)
                    fail("invalid prepared tile offsets");
                previous_offset = ids[i];
            }
            metrics.prepare_ms = milliseconds(upload_start);
            const auto h2d_start = Clock::now();
            void* inputs[ColorMatrix + 1] = {};
            for (uint32_t i = 0; i <= ColorMatrix; ++i) {
                const auto r = frame.buffers[i];
                if (i == GroundTruth || i == Mask) continue;
                if (!r.bytes) continue;
                inputs[i] = device_region((Buffer)i, (size_t)r.bytes);
                backend::memcpy_async(inputs[i], host((Buffer)i), (size_t)r.bytes,
                                      backend::MemcpyKind::HostToDevice);
            }
            const size_t pixels = (size_t)frame.width * frame.height;
            const size_t gt_prefix = (size_t)frame.gt_begin * frame.width;
            auto upload_gt = [&](Buffer id, size_t element_bytes) {
                inputs[id] = device_region(id, pixels * element_bytes);
                if (gt_prefix)
                    backend::memset_async(inputs[id], 0, gt_prefix * element_bytes);
                if (frame.buffers[id].bytes)
                    backend::memcpy_async((char*)inputs[id] + gt_prefix * element_bytes,
                        host(id), (size_t)frame.buffers[id].bytes,
                        backend::MemcpyKind::HostToDevice);
            };
            upload_gt(GroundTruth, sizeof(float3));
            if (frame.has_mask) upload_gt(Mask, sizeof(bool));
            auto& e = engine();
            e.cur_num_splats = e.max_num_splats = frame.rows;
            e.primitive = "3dgs";
            e.packed = true;
            e.camera.num = 1;
            e.camera.width = frame.width;
            e.camera.height = frame.height;
            e.camera.model_str = frame.camera_model;
            e.camera.distortion_str = frame.camera_distortion;
            e.camera.intrins = DeviceVector<float4>(view(inputs[Intrinsics], 4, {1, 4}));
            e.fwd.splats_w.clear();
            e.fwd.splats_s = {DeviceTensorFloatND(view(inputs[Screen], 4, {frame.projected, SCR2_STRIDE, 1}))};
            e.fwd.gaussian_ids = DeviceVector<int32_t>(view(inputs[GaussianIds], 4, {frame.projected}));
            e.fwd.tile_offsets = DeviceTensor3D<int32_t>(view(inputs[TileOffsets], 4,
                {1, frame.tile_height, frame.tile_width, 1}));
            e.fwd.flatten_ids = DeviceVector<int32_t>(view(inputs[FlattenIds], 4, {frame.intersections}));
            e.fwd.macro_log2 = (int)frame.macro_log2;
            e.fwd.accum_weight = DeviceVector<float>();
            e.fwd.v_splats_s.clear();
            e.gt.rgb = DeviceTensor3D<float3>(view(inputs[GroundTruth], 4, {1, frame.height, frame.width, 3}));
            e.gt.alpha = frame.has_mask ? DeviceTensor3D<bool>(view(inputs[Mask], 1,
                {1, frame.height, frame.width, 1})) : DeviceTensor3D<bool>();
            e.gt.has_gt = true;
            e.gt.has_mask = frame.has_mask != 0;
            e.ppisp.enabled = frame.has_ppisp != 0;
            e.ppisp.param_type = frame.ppisp_type;
            e.ppisp.num_params = (int)frame.ppisp_count;
            if (frame.has_ppisp) {
                e.ppisp.params = DeviceTensor2D<float>(view(inputs[PpispParams], 4, {1, frame.ppisp_count, 1}));
                e.ppisp.grads.resize(PoolSlot::EngPpispGrads, 1, frame.ppisp_count);
                e.ppisp.grads.zero();
            }
            e.ppisp.cur_run_before_bilagrid = frame.config.ppisp.run_before_bilagrid;
            e.ppisp.cur_run_before_color_space = frame.config.ppisp.run_before_color_space && frame.color_enabled;
            e.bilagrid_cur_cam_indices = DeviceVector<int32_t>(); // Both current-camera appearance tables are compacted to slot zero.
            e.bilagrid_rgb.enabled = frame.has_grid != 0;
            if (frame.has_grid) {
                auto& bg = e.bilagrid_rgb;
                bg.type = "ppisp";
                bg.C = 9;
                bg.value_bits = (int)frame.grid_value_bits;
                const int64_t cells = int64_t(frame.grid_l) * frame.grid_h * frame.grid_w * 9;
                if (frame.grid_value_bits == 16) {
                    bg.grids.set_shape_no_alloc(1, frame.grid_l, frame.grid_h, frame.grid_w, 9);
                    bg.grids_quant.n_cells = cells;
                    bg.grids_quant.n_bounds = cells / 256;
                    bg.grids_quant.packed = DeviceVector<uint8_t>(view(inputs[GridValues], 1, {cells * 2}));
                    bg.grids_quant.bounds = DeviceVector<float2>(view(inputs[GridBounds], 4, {cells / 256, 2}));
                } else bg.grids = DeviceTensor5D<float>(view(inputs[GridValues], 4,
                    {1, frame.grid_l, frame.grid_h, frame.grid_w, 9, 1}));
            }
            e.color_space.splat_enabled = frame.color_enabled != 0;
            e.color_space.splat_transfer = (int)frame.color_transfer;
            e.color_space.splat_is_linear = frame.color_linear != 0;
            if (frame.color_enabled)
                e.color_space.splat_color_matrix = DeviceTensor2D<float3>(view(inputs[ColorMatrix], 4, {3, 1, 3}));
            gpu.fence();
            metrics.upload_ms = milliseconds(h2d_start);
            uint32_t phase = 0;
            gpu.native_ms = 0;
            backend::vk::set_image_strip({frame.height, frame.cut, frame.height, frame.halo,
                [&](float* values, size_t count) {
                    gpu.end();
                    const auto start = Clock::now();
                    if (count > frame.buffers[Collective].bytes / sizeof(float)) fail("worker collective exceeds slot");
                    if (count != frame_collective_count(frame, phase + 1))
                        fail("worker collective phase or statistic layout differs from the protocol");
                    const size_t bytes = count * sizeof(float);
                    backend::memcpy_async(staging.data, values, bytes, backend::MemcpyKind::DeviceToHost);
                    gpu.fence();
                    finite(reinterpret_cast<const float*>(staging.data), count);
                    std::memcpy(mapping.data + frame.buffers[Collective].offset, staging.data, bytes);
                    shared.phase = ++phase;
                    shared.statistic_count = count;
                    InterlockedExchange(&shared.state, Statistics);
                    signal(ready.value);
                    wait_parent(sum_ready.value, parent.value);
                    if (InterlockedCompareExchange(&shared.state, StatisticsSum, StatisticsSum) != StatisticsSum ||
                        shared.phase != phase || shared.statistic_count != count ||
                        std::memcmp(&shared.frame, &frame, sizeof(Frame))) fail("invalid collective acknowledgement");
                    std::memcpy(staging.data, mapping.data + frame.buffers[Collective].offset, bytes);
                    finite(reinterpret_cast<const float*>(staging.data), count);
                    backend::memcpy_async(values, staging.data, bytes, backend::MemcpyKind::HostToDevice);
                    gpu.fence();
                    metrics.collective_ms += milliseconds(start);
                    gpu.begin();
                }});
            gpu.begin();
            engine_forward_prepared_pixels(e.ppisp.enabled && e.ppisp.cur_run_before_color_space,
                                          false, (int)DistortionType::None);
            engine_prepared_pixels_backward(frame.step, TorchTensorView(0, 4, {}), frame.config);
            gpu.end();
            backend::vk::set_image_strip({});
            metrics.gpu_ms = gpu.native_ms;
            const auto download_start = Clock::now();
            auto copy = [&](Buffer id, const void* source, size_t bytes) {
                const auto r = frame.buffers[id];
                if (r.bytes != bytes || (bytes && !source)) fail("worker result buffer extent differs");
                if (!bytes) return;
                backend::memcpy_async(staging.data + r.offset - frame.output_begin, source, bytes,
                                      backend::MemcpyKind::DeviceToHost);
            };
            if (e.fwd.v_splats_s.size() != 1) fail("worker did not produce native screen gradients");
            copy(ScreenResult, e.fwd.v_splats_s[0].data_ptr(), (size_t)e.fwd.v_splats_s[0].numel() * sizeof(float));
            copy(DensityResult, e.fwd.accum_weight.data_ptr(), (size_t)e.fwd.accum_weight.size() * sizeof(float));
            copy(PpispResult, e.ppisp.grads.data_ptr(), frame.has_ppisp ? frame.ppisp_count * sizeof(float) : 0);
            copy(GridResult, e.bilagrid_rgb.image_grad.data_ptr(),
                 frame.has_grid ? (size_t)e.bilagrid_rgb.image_grad.numel() * sizeof(float) : 0);
            if (frame.audit_pixels) {
                const auto& rgb = std::get<0>(e.fwd.renders);
                const auto& last_ids = e.fwd.last_ids;
                if (rgb.size<0>() != 1 || rgb.size<1>() != frame.height ||
                    rgb.size<2>() != frame.width || !rgb.data_ptr() ||
                    last_ids.size<0>() != 1 || last_ids.size<1>() != frame.height ||
                    last_ids.size<2>() != frame.width || !last_ids.data_ptr())
                    fail("worker pixel audit images are not the full native renders");
                const size_t first = (size_t)frame.cut * frame.width;
                const size_t pixels = (size_t)(frame.height - frame.cut) * frame.width;
                copy(OwnedRgbResult, rgb.data_ptr() + first, pixels * sizeof(float3));
                copy(OwnedLastIdsResult, last_ids.data_ptr() + first, pixels * sizeof(int32_t));
            }
            gpu.fence();
            metrics.download_ms = milliseconds(download_start);
            for (uint32_t id = ScreenResult; id <= OwnedRgbResult; ++id) {
                const auto r = frame.buffers[id];
                if (!r.bytes) continue;
                finite(reinterpret_cast<const float*>(staging.data + r.offset - frame.output_begin),
                       (size_t)r.bytes / sizeof(float));
            }
            if (frame.audit_pixels) {
                const auto r = frame.buffers[OwnedLastIdsResult];
                checked_last_ids(reinterpret_cast<const int*>(staging.data + r.offset - frame.output_begin),
                                 (size_t)r.bytes / sizeof(int32_t), frame.intersections);
            }
            const auto output_mapping_start = Clock::now();
            std::memcpy(mapping.data + frame.output_begin, staging.data, (size_t)frame.output_bytes);
            metrics.mapping_ms += milliseconds(output_mapping_start);
            if (std::memcmp(&shared.frame, &frame, sizeof(Frame))) fail("frame changed while worker was executing");
            if (phase != frame_collective_phases(frame))
                fail("worker did not execute every native collective phase");
            shared.metrics = metrics;
            shared.phases_complete = phase;
            InterlockedExchange(&shared.state, Complete);
            signal(ready.value);
            previous_step = frame.step;
            ++next_request;
        }
        backend::device_synchronize();
        checked_backend();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "image strip worker: %s\n", e.what());
        return 1;
    }
}
#else
struct ImageStripExecutor::Impl {};
ImageStripExecutor::ImageStripExecutor(const TrainConfig&, const std::atomic<bool>&) {
    throw std::runtime_error("image strip requires Windows Vulkan");
}
ImageStripExecutor::~ImageStripExecutor() = default;
void ImageStripExecutor::begin_step(int) { throw std::runtime_error("image strip requires Windows Vulkan"); }
void ImageStripExecutor::abandon_undispatched_step() { throw std::runtime_error("image strip requires Windows Vulkan"); }
void ImageStripExecutor::finish_step() { throw std::runtime_error("image strip requires Windows Vulkan"); }
void ImageStripExecutor::require_checkpointable() const { throw std::runtime_error("image strip requires Windows Vulkan"); }
void ImageStripExecutor::poison() noexcept {}
int image_strip_worker_main(int, char**) {
    std::fprintf(stderr, "image strip worker requires Windows Vulkan\n");
    return 2;
}
#endif

}  // namespace app
