// Smoke test for the Vulkan backend runtime (backend/api/BackendRuntime.h).
// Exercises every API the engine layer uses: malloc/free, pinned host
// memory, all memcpy kinds (sync + async, pinned and pageable), memset,
// pointer classification, streams, events (incl. timing), and the sticky
// error channel. Run on each available device via SS_VK_DEVICE.

#include "backend/api/BackendRuntime.h"
#include "backend/vulkan/VulkanInternal.h"
#include "backend/vulkan/kernels/ImageStrip.h"
#include "core/SourcePath.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <limits>

static int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", SS_FILE, __LINE__, #cond);    \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

#define CHECK_NO_ERROR()                                                  \
    do {                                                                  \
        const char* err_ = backend::last_error();                         \
        if (err_) {                                                       \
            std::printf("FAIL %s:%d: unexpected backend error: %s\n",     \
                        SS_FILE, __LINE__, err_);                         \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

static uint32_t transfer_pattern(size_t word, uint32_t seed) {
    return (static_cast<uint32_t>(word) * 2654435761u) ^
           (static_cast<uint32_t>(word >> 16) * 2246822519u) ^
           (seed * 3266489917u);
}

static int transfer_bench() {
    using namespace backend;
    using Clock = std::chrono::steady_clock;
    const char* route = "none";
    const char* phase = "configuration";
    size_t bytes = 0;
    auto require_no_error = [&] {
        if (const char* error = last_error())
            throw std::runtime_error(error);
    };
    try {
        const auto staging_bytes = vk::staging_max_chunk();
        if (staging_bytes > (size_t{256} << 20))
            throw std::runtime_error("SS_VK_STAGING_MB must be at most 256 for this benchmark");
        device_synchronize();
        require_no_error();
        std::printf("TRANSFER_BENCH_CONFIG device=%s warmups=1 repeats=3 "
                    "timer=steady_clock scope=memcpy_sync+device_synchronize "
                    "pre_sync=excluded allocation=excluded validation=excluded "
                    "staging_bytes=%llu max_owned_buffer_bytes=%zu\n",
                    device_current_selector().c_str(),
                    static_cast<unsigned long long>(staging_bytes), size_t{512} << 20);
        for (size_t size : {size_t{32} << 20, size_t{256} << 20}) {
            bytes = size;
            phase = "device_allocation";
            std::unique_ptr<void, decltype(&device_free)> device(
                device_malloc_checked(bytes, "transfer benchmark"), &device_free);
            require_no_error();
            for (bool pinned : {false, true}) {
                route = pinned ? "pinned" : "pageable";
                phase = "host_allocation";
                std::unique_ptr<void, void (*)(void*)> host(
                    pinned ? host_malloc_pinned(bytes) : std::malloc(bytes),
                    pinned ? &host_free_pinned : &std::free);
                require_no_error();
                if (!host) throw std::runtime_error("host buffer allocation failed");
                auto* words = static_cast<uint32_t*>(host.get());
                const size_t word_count = bytes / sizeof(uint32_t);
                std::array<double, 3> h2d{}, d2h{};
                auto timed_copy = [&](void* dst, const void* src, MemcpyKind kind) {
                    device_synchronize();
                    require_no_error();
                    const auto begin = Clock::now();
                    memcpy_sync(dst, src, bytes, kind);
                    device_synchronize();
                    const auto end = Clock::now();
                    require_no_error();
                    return std::chrono::duration<double, std::milli>(end - begin).count();
                };
                for (int repeat = 0; repeat < 4; ++repeat) {
                    const uint32_t seed = static_cast<uint32_t>(repeat + 1);
                    for (size_t i = 0; i < word_count; ++i)
                        words[i] = transfer_pattern(i, seed);
                    phase = "H2D";
                    const double upload_ms = timed_copy(
                        device.get(), host.get(), MemcpyKind::HostToDevice);
                    // Every destination byte differs, so a skipped readback cannot pass.
                    for (size_t i = 0; i < word_count; ++i) words[i] = ~words[i];
                    phase = "D2H";
                    const double download_ms = timed_copy(
                        host.get(), device.get(), MemcpyKind::DeviceToHost);
                    phase = "validation";
                    for (size_t i = 0; i < word_count; ++i) {
                        const uint32_t expected = transfer_pattern(i, seed);
                        if (words[i] != expected) {
                            std::printf("TRANSFER_BENCH_MISMATCH route=%s bytes=%zu "
                                        "repeat=%d offset_bytes=%zu expected_u32=%u actual_u32=%u\n",
                                        route, bytes, repeat, i * sizeof(uint32_t),
                                        static_cast<unsigned>(expected),
                                        static_cast<unsigned>(words[i]));
                            throw std::runtime_error("full buffer roundtrip validation failed");
                        }
                    }
                    std::printf("TRANSFER_BENCH_SAMPLE route=%s bytes=%zu "
                                "warmup=%d repeat=%d seed=%u h2d_ms=%.6f d2h_ms=%.6f "
                                "validated_bytes=%zu status=PASSED\n",
                                route, bytes, repeat == 0 ? 1 : 0, repeat,
                                static_cast<unsigned>(seed), upload_ms, download_ms, bytes);
                    if (repeat > 0) {
                        h2d[repeat - 1] = upload_ms;
                        d2h[repeat - 1] = download_ms;
                    }
                }
                std::sort(h2d.begin(), h2d.end());
                std::sort(d2h.begin(), d2h.end());
                std::printf("TRANSFER_BENCH route=%s direction=H2D bytes=%zu "
                            "warmups=1 repeats=3 median_ms=%.6f min_ms=%.6f max_ms=%.6f "
                            "validation=full_roundtrip status=PASSED\n",
                            route, bytes, h2d[1], h2d[0], h2d[2]);
                std::printf("TRANSFER_BENCH route=%s direction=D2H bytes=%zu "
                            "warmups=1 repeats=3 median_ms=%.6f min_ms=%.6f max_ms=%.6f "
                            "validation=full_roundtrip status=PASSED\n",
                            route, bytes, d2h[1], d2h[0], d2h[2]);
            }
        }
        phase = "cleanup";
        device_synchronize();
        require_no_error();
        std::printf("TRANSFER_BENCH_RESULT status=PASSED\n");
        return 0;
    } catch (const std::exception& error) {
        std::printf("TRANSFER_BENCH_ERROR route=%s bytes=%zu phase=%s message=\"%s\"\n",
                    route, bytes, phase, error.what());
        return 1;
    }
}

static int strip_finite_check() {
    using namespace backend;
    try {
        const size_t count = size_t{65535} * 256 + 1;
        std::unique_ptr<void, decltype(&device_free)> values(
            device_malloc_checked(count * sizeof(float), "strip finite fixture"), &device_free);
        std::unique_ptr<void, decltype(&device_free)> flag(
            device_malloc_checked(sizeof(uint32_t), "strip finite fixture flag"), &device_free);
        auto* data = static_cast<float*>(values.get());
        auto* invalid = static_cast<uint32_t*>(flag.get());
        memset_sync(data, 0, count * sizeof(float));
        memset_sync(invalid, 0, sizeof(uint32_t));
        const float finite[] = {-0.0f, std::numeric_limits<float>::max(),
            std::numeric_limits<float>::denorm_min()};
        memcpy_sync(data + 255, finite, sizeof(finite), MemcpyKind::HostToDevice);
        vk::image_strip_check_finite(data, count, invalid);
        uint32_t result = 99;
        memcpy_sync(&result, invalid, sizeof(result), MemcpyKind::DeviceToHost);
        CHECK(result == 0);
        const float infinity = std::numeric_limits<float>::infinity();
        memcpy_sync(data + count - 1, &infinity, sizeof(float), MemcpyKind::HostToDevice);
        vk::image_strip_check_finite(data, count, invalid);
        memcpy_sync(&result, invalid, sizeof(result), MemcpyKind::DeviceToHost);
        CHECK(result == 1);
        const float zero = 0;
        memcpy_sync(data + count - 1, &zero, sizeof(float), MemcpyKind::HostToDevice);
        vk::image_strip_check_finite(data, count, invalid);
        memcpy_sync(&result, invalid, sizeof(result), MemcpyKind::DeviceToHost);
        CHECK(result == 1);
        memset_sync(invalid, 0, sizeof(uint32_t));
        const float nan = std::numeric_limits<float>::quiet_NaN();
        memcpy_sync(data + 256, &nan, sizeof(float), MemcpyKind::HostToDevice);
        vk::image_strip_check_finite(data, 513, invalid);
        memcpy_sync(&result, invalid, sizeof(result), MemcpyKind::DeviceToHost);
        CHECK(result == 1);
        vk::image_strip_check_finite(nullptr, 0, nullptr);
        bool rejected = false;
        try { vk::image_strip_check_finite(nullptr, 1, invalid); }
        catch (const std::runtime_error&) { rejected = true; }
        CHECK(rejected);
        rejected = false;
        try {
            vk::image_strip_check_finite(data,
                size_t(std::numeric_limits<uint32_t>::max()) + 1, invalid);
        } catch (const std::runtime_error&) { rejected = true; }
        CHECK(rejected);
        device_synchronize();
        CHECK_NO_ERROR();
        std::printf("strip finite check: %s\n", g_failures ? "FAILED" : "PASSED");
        return g_failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "strip finite check: %s\n", error.what());
        return 1;
    }
}

int main(int argc, char** argv) {
    using namespace backend;
    if (argc >= 2 && std::strcmp(argv[1], "--strip-finite") == 0) {
        if (argc != 2) {
            std::fprintf(stderr, "Usage: vk_runtime_smoke --strip-finite\n");
            return 2;
        }
        return strip_finite_check();
    }
    if (argc >= 2 && std::strcmp(argv[1], "--transfer-bench") == 0) {
        if (argc != 2) {
            std::fprintf(stderr, "Usage: vk_runtime_smoke --transfer-bench\n");
            return 2;
        }
        return transfer_bench();
    }
    if (argc == 2 && std::strcmp(argv[1], "--invalid-backing-cap") == 0) {
#ifdef _WIN32
        _putenv_s("SS_WORKER_DEVICE_LIMIT_BYTES", "invalid");
#else
        setenv("SS_WORKER_DEVICE_LIMIT_BYTES", "invalid", 1);
#endif
        CHECK(vk::stream_impl(kDefaultStream) != nullptr);
        bool invalid_rejected = false;
        try { device_free(device_malloc(16)); }
        catch (const std::runtime_error& e) {
            invalid_rejected = std::string(e.what()).find("must be positive bytes") != std::string::npos;
        }
        CHECK(invalid_rejected);
        backing_report("invalid-configuration-cleanup");
        std::printf("vk_runtime_smoke invalid cap: %s\n", g_failures ? "FAILED" : "PASSED");
        return g_failures ? 1 : 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--backing-cap") == 0) {
#ifdef _WIN32
        _putenv_s("SS_WORKER_DEVICE_LIMIT_BYTES", "1048576");
#else
        setenv("SS_WORKER_DEVICE_LIMIT_BYTES", "1048576", 1);
#endif
        CHECK(vk::backing_cap_smoke());
        void* first = device_malloc(768 << 10);
        CHECK(first != nullptr);
        const auto single = backing_usage();
        void* overlap = device_malloc(128 << 10);
        CHECK(overlap != nullptr);
        const auto overlapping = backing_usage();
        CHECK(overlapping.current > single.current);
        CHECK(overlapping.reservation_peak >= overlapping.current);
        CHECK(overlapping.category_reservation_peak[0] >= overlapping.current);
        device_free(overlap);
        const auto before = backing_usage();
        bool cap_refused = false;
        try { device_free(device_malloc(512 << 10)); }
        catch (const std::runtime_error& e) {
            cap_refused = std::string(e.what()).find("refused before vkAllocateMemory") != std::string::npos;
        }
        CHECK(cap_refused);
        const auto refused = backing_usage();
        CHECK(refused.current == before.current && refused.reserved == 0);
        CHECK(refused.allocation_calls == before.allocation_calls);
        CHECK(refused.failures == before.failures + 1);
        uint8_t pageable[16] = {};
        bool staging_refused = false;
        try { memcpy_sync(first, pageable, sizeof pageable, MemcpyKind::HostToDevice); }
        catch (const std::runtime_error&) { staging_refused = true; }
        CHECK(staging_refused);
        uint64_t address = 0;
        void* mapped = nullptr;
        bool params_refused = false;
        try { vk::params_alloc(16, &address, &mapped); }
        catch (const std::runtime_error&) { params_refused = true; }
        CHECK(params_refused);
        const auto rings_refused = backing_usage();
        CHECK(rings_refused.current == before.current && rings_refused.reserved == 0);
        CHECK(rings_refused.allocation_calls == before.allocation_calls);
        CHECK(rings_refused.category_failures[2] == 1 && rings_refused.category_failures[3] == 1);
        device_free(first);
        CHECK(backing_usage().current == 0);
        std::atomic<int> ready{0};
        std::atomic<int> concurrent_refusals{0};
        void* results[2] = {};
        auto allocate = [&](int i) {
            ready.fetch_add(1);
            while (ready.load() != 2) std::this_thread::yield();
            try { results[i] = device_malloc(768 << 10); }
            catch (const std::runtime_error&) { concurrent_refusals.fetch_add(1); }
        };
        std::thread a(allocate, 0), b(allocate, 1);
        a.join(); b.join();
        CHECK((results[0] != nullptr) != (results[1] != nullptr));
        CHECK(backing_usage().current <= 1048576);
        CHECK(backing_usage().reserved == 0);
        CHECK(concurrent_refusals.load() == 1);
        device_free(results[0]); device_free(results[1]);
        void* again = host_malloc_pinned(768 << 10);
        CHECK(again != nullptr);
        CHECK(backing_usage().category_current[1] != 0);
        host_free_pinned(again);
        const auto final = backing_usage();
        CHECK(final.current == 0 && final.reserved == 0 && final.peak <= final.limit);
        CHECK(final.reservation_peak <= final.limit && final.reservation_peak >= overlapping.current);
        backing_report("bounded-cap-smoke");
        std::printf("vk_runtime_smoke cap: %s\n", g_failures ? "FAILED" : "PASSED");
        return g_failures ? 1 : 0;
    }
    CHECK(!device_select_identity("9999"));
    const std::string detail = device_selection_error();
    CHECK(!detail.empty());
    CHECK(detail.find("9999") != std::string::npos);
    CHECK(detail.find("does not exist") != std::string::npos ||
          detail.find("range") != std::string::npos);
    const std::string selector = device_current_selector();
    CHECK(!selector.empty());
    CHECK(device_select_identity(selector.c_str()));
    CHECK(device_selection_error().empty());

    // --- allocation + classification ---
    const size_t N = 1 << 20;  // 1M floats = 4MB
    void* dev_a = device_malloc(N * sizeof(float));
    void* dev_b = device_malloc(N * sizeof(float));
    CHECK(dev_a != nullptr);
    CHECK(dev_b != nullptr);
    CHECK_NO_ERROR();
    CHECK(is_device_pointer(dev_a));
    CHECK(is_device_pointer((char*)dev_a + 1234));
    CHECK(!is_device_pointer(nullptr));
    int on_stack = 0;
    CHECK(!is_device_pointer(&on_stack));
    CHECK(device_malloc(0) == nullptr);
    CHECK_NO_ERROR();

    float* pinned = (float*)host_malloc_pinned(N * sizeof(float));
    CHECK(pinned != nullptr);
    CHECK(!is_device_pointer(pinned));  // host-pinned is HOST (CUDA parity)
    CHECK_NO_ERROR();

    // --- sync H2D (pageable) + D2H (pageable) round trip ---
    std::vector<float> src(N), dst(N, 0.f);
    for (size_t i = 0; i < N; i++) src[i] = (float)(i % 9973) * 0.25f;
    memcpy_sync(dev_a, src.data(), N * sizeof(float),
                MemcpyKind::HostToDevice);
    CHECK_NO_ERROR();
    memcpy_sync(dst.data(), dev_a, N * sizeof(float),
                MemcpyKind::DeviceToHost);
    CHECK_NO_ERROR();
    CHECK(std::memcmp(src.data(), dst.data(), N * sizeof(float)) == 0);

    // --- D2D + offset arithmetic on device pointers ---
    memcpy_sync(dev_b, dev_a, N * sizeof(float), MemcpyKind::DeviceToDevice);
    std::fill(dst.begin(), dst.end(), 0.f);
    memcpy_sync(dst.data(), (char*)dev_b + 512 * sizeof(float),
                (N - 512) * sizeof(float), MemcpyKind::DeviceToHost);
    CHECK_NO_ERROR();
    CHECK(std::memcmp(src.data() + 512, dst.data(),
                      (N - 512) * sizeof(float)) == 0);

    // --- Auto kind routing ---
    std::fill(dst.begin(), dst.end(), 0.f);
    memcpy_sync(dst.data(), dev_a, N * sizeof(float), MemcpyKind::Auto);
    CHECK(std::memcmp(src.data(), dst.data(), N * sizeof(float)) == 0);
    CHECK_NO_ERROR();

    // --- memset (value and zero, with element tail) ---
    memset_sync(dev_a, 0, N * sizeof(float));
    std::fill(dst.begin(), dst.end(), 1.f);
    memcpy_sync(dst.data(), dev_a, N * sizeof(float),
                MemcpyKind::DeviceToHost);
    for (size_t i = 0; i < N; i++)
        if (dst[i] != 0.f) { CHECK(dst[i] == 0.f); break; }
    memset_sync(dev_a, 0x5A, 64);
    uint8_t bytes64[64];
    memcpy_sync(bytes64, dev_a, 64, MemcpyKind::DeviceToHost);
    for (int i = 0; i < 64; i++)
        if (bytes64[i] != 0x5A) { CHECK(bytes64[i] == 0x5A); break; }
    CHECK_NO_ERROR();

    // --- async path: pinned D2H readback with events (AsyncReadout shape) ---
    memcpy_sync(dev_a, src.data(), N * sizeof(float),
                MemcpyKind::HostToDevice);
    std::memset(pinned, 0, N * sizeof(float));
    Event* evt = event_create(false);
    memcpy_async(pinned, dev_a, N * sizeof(float), MemcpyKind::DeviceToHost,
                 kDefaultStream);
    event_record(evt, kDefaultStream);
    event_synchronize(evt);
    CHECK_NO_ERROR();
    CHECK(std::memcmp(src.data(), pinned, N * sizeof(float)) == 0);
    event_destroy(evt);

    // --- async pinned H2D + async D2D + memset_async, one batch ---
    for (size_t i = 0; i < N; i++) pinned[i] = 2.0f * (float)(i % 4096);
    memcpy_async(dev_a, pinned, N * sizeof(float), MemcpyKind::HostToDevice,
                 kDefaultStream);
    memcpy_async(dev_b, dev_a, N * sizeof(float), MemcpyKind::DeviceToDevice,
                 kDefaultStream);
    memset_async(dev_a, 0, N * sizeof(float), kDefaultStream);
    stream_synchronize(kDefaultStream);
    CHECK_NO_ERROR();
    std::fill(dst.begin(), dst.end(), -1.f);
    memcpy_sync(dst.data(), dev_b, N * sizeof(float),
                MemcpyKind::DeviceToHost);
    CHECK(std::memcmp(pinned, dst.data(), N * sizeof(float)) == 0);
    memcpy_sync(dst.data(), dev_a, N * sizeof(float),
                MemcpyKind::DeviceToHost);
    CHECK(dst[0] == 0.f && dst[N - 1] == 0.f);

    // --- async pageable H2D (staging ring, multi-MB) ---
    memcpy_async(dev_a, src.data(), N * sizeof(float),
                 MemcpyKind::HostToDevice, kDefaultStream);
    device_synchronize();
    CHECK_NO_ERROR();
    std::fill(dst.begin(), dst.end(), 0.f);
    memcpy_sync(dst.data(), dev_a, N * sizeof(float),
                MemcpyKind::DeviceToHost);
    CHECK(std::memcmp(src.data(), dst.data(), N * sizeof(float)) == 0);

    // --- timing events ---
    Event* t0 = event_create(true);
    Event* t1 = event_create(true);
    event_record(t0, kDefaultStream);
    memcpy_async(dev_b, dev_a, N * sizeof(float), MemcpyKind::DeviceToDevice,
                 kDefaultStream);
    event_record(t1, kDefaultStream);
    event_synchronize(t1);
    float ms = event_elapsed_ms(t0, t1);
    const char* timing_err = last_error();
    if (timing_err) {
        std::printf("note: event timing unavailable on this device (%s)\n",
                    timing_err);
    } else {
        CHECK(ms >= 0.0f && ms < 10000.0f);
        std::printf("note: 4MB D2D took %.3f ms\n", ms);
    }
    event_destroy(t0);
    event_destroy(t1);

    // --- error channel: bad free reports, then clears ---
    device_free((char*)dev_a + 64);  // not a base pointer
    CHECK(last_error() != nullptr);
    CHECK(last_error() == nullptr);  // cleared by the read

    // --- cleanup (device_free syncs in-flight work per contract) ---
    memcpy_async(dev_b, dev_a, N * sizeof(float), MemcpyKind::DeviceToDevice,
                 kDefaultStream);  // leave work in flight
    device_free(dev_a);
    device_free(dev_b);
    host_free_pinned(pinned);
    device_synchronize();
    CHECK_NO_ERROR();

    std::printf(g_failures == 0 ? "vk_runtime_smoke: ALL PASSED\n"
                                : "vk_runtime_smoke: %d FAILURES\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
