// M0.4 host-staged transfer benchmark for one explicitly selected Vulkan GPU.
// Binary modes exchange exactly iterations fixed-size payloads.

#include "backend/api/BackendRuntime.h"

#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

constexpr unsigned kWarmup = 4;
using Clock = std::chrono::steady_clock;

bool parse_positive(const char* text, unsigned long long* value) {
    if (!text || !text[0] || text[0] == '-') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || parsed == 0)
        return false;
    *value = parsed;
    return true;
}

bool parse_bytes(const char* text, size_t* bytes) {
    unsigned long long parsed = 0;
    if (!parse_positive(text, &parsed) ||
        parsed > static_cast<unsigned long long>((std::numeric_limits<size_t>::max)()))
        return false;
    const size_t value = static_cast<size_t>(parsed);
    if (value > (std::numeric_limits<size_t>::max)() - 15) return false;
    *bytes = value;
    return true;
}

bool parse_iterations(const char* text, uint64_t* iterations) {
    unsigned long long parsed = 0;
    if (!parse_positive(text, &parsed) ||
        parsed > static_cast<unsigned long long>(
                     (std::numeric_limits<uint64_t>::max)()))
        return false;
    *iterations = static_cast<uint64_t>(parsed);
    return true;
}

unsigned char pattern_byte(size_t offset) {
    const uint64_t x = static_cast<uint64_t>(offset);
    return static_cast<unsigned char>(
        (x * 0x9e3779b1u + (x >> 8) * 0x85ebca6bu + 0x27d4eb2fu) & 0xffu);
}

void fill_pattern(unsigned char* data, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) data[i] = pattern_byte(i);
}

bool check_pattern(const unsigned char* data, size_t bytes, FILE* log,
                   uint64_t iteration) {
    for (size_t i = 0; i < bytes; ++i) {
        const unsigned char expected = pattern_byte(i);
        if (data[i] != expected) {
            std::fprintf(log,
                         "payload validation failed iteration=%llu offset=%zu "
                         "got=%u want=%u\n",
                         static_cast<unsigned long long>(iteration), i,
                         static_cast<unsigned>(data[i]),
                         static_cast<unsigned>(expected));
            return false;
        }
    }
    return true;
}

bool backend_ok(FILE* log, const char* operation) {
    const char* error = backend::last_error();
    if (!error) return true;
    std::fprintf(log, "backend operation failed operation=%s detail=%s\n",
                 operation, error);
    return false;
}

bool completed_copy(void* dst, const void* src, size_t bytes,
                    backend::MemcpyKind kind, FILE* log,
                    const char* operation) {
    backend::memcpy_async(dst, src, bytes, kind, backend::kDefaultStream);
    backend::stream_synchronize(backend::kDefaultStream);
    return backend_ok(log, operation);
}

bool read_exact(unsigned char* data, size_t bytes, FILE* input, FILE* log,
                uint64_t iteration) {
    size_t done = 0;
    while (done < bytes) {
        const size_t want = bytes - done;
        const size_t got = std::fread(data + done, 1, want, input);
        if (got == 0) {
            if (std::feof(input)) {
                std::fprintf(log,
                             "short read iteration=%llu bytes=%zu/%zu: EOF\n",
                             static_cast<unsigned long long>(iteration), done,
                             bytes);
            } else if (std::ferror(input)) {
                std::fprintf(log,
                             "read failed iteration=%llu bytes=%zu/%zu: %s\n",
                             static_cast<unsigned long long>(iteration), done,
                             bytes, std::strerror(errno));
            } else {
                std::fprintf(log,
                             "short read iteration=%llu bytes=%zu/%zu: no "
                             "progress\n",
                             static_cast<unsigned long long>(iteration), done,
                             bytes);
            }
            return false;
        }
        done += got;
        if (std::ferror(input)) {
            std::fprintf(log,
                         "read failed iteration=%llu bytes=%zu/%zu: %s\n",
                         static_cast<unsigned long long>(iteration), done, bytes,
                         std::strerror(errno));
            return false;
        }
        if (done < bytes && std::feof(input)) {
            std::fprintf(log,
                         "short read iteration=%llu bytes=%zu/%zu: EOF\n",
                         static_cast<unsigned long long>(iteration), done, bytes);
            return false;
        }
    }
    return true;
}

bool write_exact(const unsigned char* data, size_t bytes, FILE* output,
                 FILE* log, uint64_t iteration) {
    size_t done = 0;
    while (done < bytes) {
        const size_t want = bytes - done;
        const size_t written = std::fwrite(data + done, 1, want, output);
        if (written == 0) {
            std::fprintf(log,
                         "write failed iteration=%llu bytes=%zu/%zu: %s\n",
                         static_cast<unsigned long long>(iteration), done, bytes,
                         std::strerror(errno));
            return false;
        }
        done += written;
        if (std::ferror(output)) {
            std::fprintf(log,
                         "write failed iteration=%llu bytes=%zu/%zu: %s\n",
                         static_cast<unsigned long long>(iteration), done, bytes,
                         std::strerror(errno));
            return false;
        }
    }
    if (std::fflush(output) != 0) {
        std::fprintf(log, "flush failed iteration=%llu: %s\n",
                     static_cast<unsigned long long>(iteration),
                     std::strerror(errno));
        return false;
    }
    return true;
}

void print_timing(FILE* log, const char* name, size_t bytes,
                  uint64_t iterations, double seconds) {
    const double total_bytes = static_cast<double>(bytes) *
                               static_cast<double>(iterations);
    const double total_ms = seconds * 1000.0;
    const double avg_ms = total_ms / static_cast<double>(iterations);
    const double gib_per_s = seconds > 0.0
                                 ? total_bytes / seconds / (1024.0 * 1024.0 * 1024.0)
                                 : 0.0;
    std::fprintf(log,
                 "%s bytes=%zu iterations=%llu total_ms=%.3f "
                 "avg_ms=%.6f gib_per_s=%.3f completed=1\n",
                 name, bytes, static_cast<unsigned long long>(iterations),
                 total_ms, avg_ms, gib_per_s);
}

bool set_binary_stdio() {
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1) return false;
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) return false;
#endif
    return true;
}

void usage(const char* program) {
    std::fprintf(stderr,
                 "usage: %s local|send|recv uuid:<hex> bytes iterations\n"
                 "send writes exactly iterations * bytes raw payload bytes to "
                 "stdout; recv reads that stream from stdin\n",
                 program);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        usage(argv[0]);
        return 2;
    }

    const bool local = std::strcmp(argv[1], "local") == 0;
    const bool send = std::strcmp(argv[1], "send") == 0;
    const bool recv = std::strcmp(argv[1], "recv") == 0;
    if (!local && !send && !recv) {
        usage(argv[0]);
        return 2;
    }

    size_t bytes = 0;
    uint64_t iterations = 0;
    if (!parse_bytes(argv[3], &bytes) ||
        !parse_iterations(argv[4], &iterations)) {
        std::fprintf(stderr, "bytes and iterations must be positive integers\n");
        return 2;
    }

    if (std::strncmp(argv[2], "uuid:", 5) != 0 || argv[2][5] == '\0') {
        std::fprintf(stderr, "device must be selected by a uuid:<hex> selector\n");
        return 2;
    }

    if (!local && !set_binary_stdio()) {
        std::fprintf(stderr, "failed to set binary stdio mode: %s\n",
                     std::strerror(errno));
        return 1;
    }

#ifndef _WIN32
    if (send && std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        std::fprintf(stderr, "failed to configure broken-pipe handling\n");
        return 1;
    }
#endif

    FILE* log = local ? stdout : stderr;
    if (!backend::device_select_identity(argv[2])) {
        const std::string error = backend::device_selection_error();
        std::fprintf(stderr, "device selection failed: %s\n",
                     error.empty() ? "unknown Vulkan device selection error"
                                    : error.c_str());
        return 1;
    }
    const std::string selected = backend::device_current_selector();
    if (selected.empty()) {
        std::fprintf(stderr, "device selection did not resolve to a UUID\n");
        return 1;
    }
    std::fprintf(log,
                 "m0_staging_bench mode=%s device=%s bytes=%zu iterations=%llu "
                 "warmup=%u\n",
                 argv[1], selected.c_str(), bytes,
                 static_cast<unsigned long long>(iterations), kWarmup);

    void* device = nullptr;
    unsigned char* host = nullptr;
    auto cleanup = [&] {
        if (device) {
            backend::device_free(device);
            device = nullptr;
        }
        if (host) {
            backend::host_free_pinned(host);
            host = nullptr;
        }
    };
    auto fail = [&]() -> int {
        cleanup();
        if (const char* error = backend::last_error())
            std::fprintf(log, "cleanup failed: %s\n", error);
        return 1;
    };

    device = backend::device_malloc(bytes);
    if (!device) {
        const char* error = backend::last_error();
        std::fprintf(log, "device allocation failed%s%s\n",
                     error ? ": " : "", error ? error : "unknown error");
        return fail();
    }
    if (!backend_ok(log, "device_malloc")) return fail();

    host = static_cast<unsigned char*>(backend::host_malloc_pinned(bytes));
    if (!host) {
        const char* error = backend::last_error();
        std::fprintf(log, "pinned host allocation failed%s%s\n",
                     error ? ": " : "", error ? error : "unknown error");
        return fail();
    }
    if (!backend_ok(log, "host_malloc_pinned")) return fail();

    if (!backend::device_identity_matches_current(argv[2])) {
        std::fprintf(log, "live Vulkan context is not the requested device=%s\n",
                     selected.c_str());
        return fail();
    }

    fill_pattern(host, bytes);
    for (unsigned warmup = 0; warmup < kWarmup; ++warmup) {
        if (!completed_copy(device, host, bytes, backend::MemcpyKind::HostToDevice,
                            log, "warmup_h2d"))
            return fail();
        if (!completed_copy(host, device, bytes, backend::MemcpyKind::DeviceToHost,
                            log, "warmup_d2h"))
            return fail();
        if (!check_pattern(host, bytes, log, warmup)) return fail();
    }

    if (local) {
        double h2d_seconds = 0.0;
        for (uint64_t i = 0; i < iterations; ++i) {
            backend::memset_sync(device, 0xa5, bytes);
            if (!backend_ok(log, "poison_device")) return fail();
            const auto h2d_start = Clock::now();
            if (!completed_copy(device, host, bytes,
                                backend::MemcpyKind::HostToDevice, log,
                                "h2d"))
                return fail();
            h2d_seconds +=
                std::chrono::duration<double>(Clock::now() - h2d_start).count();
        }

        double d2h_seconds = 0.0;
        for (uint64_t i = 0; i < iterations; ++i) {
            std::memset(host, 0xa5, bytes);
            const auto d2h_start = Clock::now();
            if (!completed_copy(host, device, bytes,
                                backend::MemcpyKind::DeviceToHost, log,
                                "d2h"))
                return fail();
            d2h_seconds +=
                std::chrono::duration<double>(Clock::now() - d2h_start).count();
            if (!check_pattern(host, bytes, log, i)) return fail();
        }
        print_timing(log, "h2d", bytes, iterations, h2d_seconds);
        print_timing(log, "d2h", bytes, iterations, d2h_seconds);
    } else if (send) {
        double d2h_seconds = 0.0;
        double write_seconds = 0.0;
        const auto stream_start = Clock::now();
        for (uint64_t i = 0; i < iterations; ++i) {
            std::memset(host, 0xa5, bytes);
            const auto d2h_start = Clock::now();
            if (!completed_copy(host, device, bytes,
                                backend::MemcpyKind::DeviceToHost, log,
                                "d2h"))
                return fail();
            d2h_seconds +=
                std::chrono::duration<double>(Clock::now() - d2h_start).count();
            if (!check_pattern(host, bytes, log, i)) return fail();

            const auto write_start = Clock::now();
            if (!write_exact(host, bytes, stdout, log, i)) return fail();
            write_seconds +=
                std::chrono::duration<double>(Clock::now() - write_start).count();
            if (std::fgetc(stdin) != 1) {
                std::fprintf(log, "missing receiver completion iteration=%llu\n",
                             static_cast<unsigned long long>(i));
                return fail();
            }
        }
        print_timing(log, "d2h", bytes, iterations, d2h_seconds);
        print_timing(log, "pipe_write", bytes, iterations, write_seconds);
        print_timing(log, "completed_stream_including_first_peer_wait", bytes, iterations,
                     std::chrono::duration<double>(Clock::now() - stream_start).count());
    } else {
        double read_seconds = 0.0;
        double h2d_seconds = 0.0;
        double verify_seconds = 0.0;
        const auto stream_start = Clock::now();
        for (uint64_t i = 0; i < iterations; ++i) {
            const auto read_start = Clock::now();
            if (!read_exact(host, bytes, stdin, log, i)) return fail();
            read_seconds +=
                std::chrono::duration<double>(Clock::now() - read_start).count();
            if (!check_pattern(host, bytes, log, i)) return fail();

            backend::memset_sync(device, 0xa5, bytes);
            if (!backend_ok(log, "poison_device")) return fail();
            const auto h2d_start = Clock::now();
            if (!completed_copy(device, host, bytes,
                                backend::MemcpyKind::HostToDevice, log,
                                "h2d"))
                return fail();
            h2d_seconds +=
                std::chrono::duration<double>(Clock::now() - h2d_start).count();
            const auto verify_start = Clock::now();
            if (!completed_copy(host, device, bytes,
                                backend::MemcpyKind::DeviceToHost, log,
                                "verify_d2h"))
                return fail();
            verify_seconds +=
                std::chrono::duration<double>(Clock::now() - verify_start).count();
            if (!check_pattern(host, bytes, log, i)) return fail();
            if (std::fputc(1, stdout) == EOF || std::fflush(stdout) != 0) {
                std::fprintf(log, "completion ack failed iteration=%llu\n",
                             static_cast<unsigned long long>(i));
                return fail();
            }
        }

        print_timing(log, "pipe_read", bytes, iterations, read_seconds);
        print_timing(log, "h2d", bytes, iterations, h2d_seconds);
        print_timing(log, "verify_d2h", bytes, iterations, verify_seconds);
        print_timing(log, "completed_stream_including_first_peer_wait", bytes, iterations,
                     std::chrono::duration<double>(Clock::now() - stream_start).count());
    }

    cleanup();
    if (const char* error = backend::last_error()) {
        std::fprintf(log, "cleanup failed: %s\n", error);
        return 1;
    }
    std::fprintf(log, "m0_staging_bench PASS\n");
    return 0;
}
