#include "app/AgentAdminBroker.h"
#include "app/DeviceLease.h"
#include "backend/api/BackendRuntime.h"
#include "data/JsonWrite.h"
#include "i18n/catalog/Cli.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

struct ProbeResult {
    bool success = false;
    bool lease_acquired = false;
    std::string backend;
    std::string device;
    std::string name;
    std::string type;
    uint32_t api_version = 0;
    uint32_t driver_version = 0;
    std::string error;
};

const char* backend_name() {
#ifdef SS_BACKEND_VULKAN
    return "vulkan";
#else
    return "unsupported";
#endif
}

void emit_result(const ProbeResult& result) {
    JsonWriter writer;
    writer.object()
        .field("success", result.success)
        .field("lease_acquired", result.lease_acquired)
        .field("backend", result.backend)
        .field("device", result.device)
        .field("name", result.name)
        .field("type", result.type)
        .field("api_version", static_cast<long long>(result.api_version))
        .field("driver_version", static_cast<long long>(result.driver_version))
        .field("error", result.error)
        .end();
    const std::string output = writer.str();
    std::fwrite(output.data(), 1, output.size(), stdout);
    std::fflush(stdout);
}

bool parse_hold_ms(const char* text, int& value) {
    if (!text || !text[0]) return false;
    const char* end = text + std::strlen(text);
    const auto parsed = std::from_chars(text, end, value);
    return parsed.ec == std::errc() && parsed.ptr == end && value >= 0 &&
           value <= 600000;
}

bool parse_args(int argc, char** argv, std::string& selector, int& hold_ms,
                std::string& error) {
    if (argc < 2 || !argv || !argv[1] || std::strcmp(argv[1], "probe") != 0) {
        error = "unsupported command";
        return false;
    }

    bool selector_set = false;
    bool hold_set = false;
    for (int i = 2; i < argc; ++i) {
        if (!argv[i]) {
            error = "invalid arguments";
            return false;
        }
        if (std::strcmp(argv[i], "--device") == 0) {
            if (selector_set || i + 1 >= argc || !argv[i + 1] ||
                !argv[i + 1][0]) {
                error = "invalid device option";
                return false;
            }
            selector = argv[++i];
            selector_set = true;
        } else if (std::strcmp(argv[i], "--hold-ms") == 0) {
            if (hold_set || i + 1 >= argc ||
                !parse_hold_ms(argv[i + 1], hold_ms)) {
                error = "invalid hold duration";
                return false;
            }
            ++i;
            hold_set = true;
        } else {
            error = "unsupported command";
            return false;
        }
    }
    if (!selector_set) {
        error = "device option is required";
        return false;
    }
    return true;
}

#ifdef SS_BACKEND_VULKAN
bool backend_ok() {
    return backend::last_error() == nullptr;
}

std::string bounded_name(const backend::DeviceInfo& info) {
    size_t length = 0;
    while (length < sizeof(info.name) && info.name[length]) ++length;
    return std::string(info.name, length);
}

bool select_device(const std::string& requested, ProbeResult& result) {
    const int count = backend::device_count();
    bool usable = false;
    for (int i = 0; i < count; ++i)
        usable = backend::device_info(i).usable || usable;
    if (count <= 0 || !usable) {
        result.error = "no usable device";
        return false;
    }

    if (!backend::device_select_identity(requested.c_str())) {
        result.error = "device selection failed";
        return false;
    }

    const int index = backend::device_current();
    if (index < 0 || index >= count) {
        result.error = "device selection failed";
        return false;
    }
    const backend::DeviceInfo info = backend::device_info(index);
    if (!info.usable) {
        result.error = "device selection failed";
        return false;
    }

    result.device = backend::device_current_selector();
    if (result.device.empty()) {
        result.error = "device selection failed";
        return false;
    }
    result.name = bounded_name(info);
    result.type = info.type ? info.type : "";
    result.api_version = info.api_version;
    result.driver_version = info.driver_version;
    return true;
}
#endif

}  // namespace

int spirula_agent_service_probe_main(int argc, char** argv);
int spirula_agent_run_main(int argc, char** argv);
int spirula_agent_service_main(int argc, char** argv);

int spirula_agent_main(int argc, char** argv) {
    if (argc >= 2 && argv && argv[1]) {
        if (std::strcmp(argv[1], "service-probe") == 0)
            return spirula_agent_service_probe_main(argc, argv);
        if (std::strcmp(argv[1], "run") == 0)
            return spirula_agent_run_main(argc, argv);
        if (std::strcmp(argv[1], "service") == 0)
            return spirula_agent_service_main(argc, argv);
        if (std::strcmp(argv[1], "broker-service") == 0 && argc == 2)
            return app::agent::admin::run_broker_service();
    }
    if (argc == 2 && argv && argv[1] &&
        std::strcmp(argv[1], "--help") == 0) {
        for (const std::string& line : spirula::i18n::wrap(
                 spirula::i18n::msg::cli::agent_help_intro.get(), 78))
            std::printf("%s\n", line.c_str());
        std::fputc('\n', stdout);
        std::fputs(
            "usage: spirula agent probe --device <selector> "
            "[--hold-ms <0..600000>]\n"
            "       spirula agent run --config-root <absolute> "
            "--state-root <absolute> --storage-root <absolute>\n"
            "       spirula agent service --config-root <absolute> "
            "--state-root <absolute> --storage-root <absolute> (Windows SCM)\n",
            stdout);
        return 0;
    }

    ProbeResult result;
    result.backend = backend_name();

    std::string requested;
    int hold_ms = 0;
    if (!parse_args(argc, argv, requested, hold_ms, result.error)) {
        emit_result(result);
        return 2;
    }

#ifndef SS_BACKEND_VULKAN
    result.error = "unsupported backend";
    emit_result(result);
    return 1;
#else
    app::DeviceLease lease;
    void* allocation = nullptr;
    try {
        if (!select_device(requested, result)) {
            emit_result(result);
            return 1;
        }

        std::string lease_error;
        if (!lease.try_acquire(result.device, lease_error)) {
            result.error = lease_error;
            emit_result(result);
            return 1;
        }
        result.lease_acquired = true;

        constexpr size_t kProbeBytes = 4096;
        allocation = backend::device_malloc(kProbeBytes);
        if (!allocation) {
            result.error = "device allocation failed";
        } else {
            backend::memset_sync(allocation, 0x5a, kProbeBytes);
            if (!backend_ok()) {
                result.error = "device memset failed";
            } else {
                std::vector<uint8_t> readback(kProbeBytes);
                backend::memcpy_sync(readback.data(), allocation, kProbeBytes,
                                     backend::MemcpyKind::DeviceToHost);
                if (!backend_ok()) {
                    result.error = "device copy failed";
                } else if (!std::all_of(readback.begin(), readback.end(),
                                        [](uint8_t value) {
                                            return value == 0x5a;
                                        })) {
                    result.error = "device memory verification failed";
                } else {
                    result.success = true;
                }
            }
            void* to_free = allocation;
            allocation = nullptr;
            backend::device_free(to_free);
            if (result.success && !backend_ok()) {
                result.success = false;
                result.error = "device free failed";
            }
        }
    } catch (...) {
        result.success = false;
        result.error = "probe failed";
    }
    if (allocation) backend::device_free(allocation);

    emit_result(result);
    if (result.success && hold_ms > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
    return result.success ? 0 : 1;
#endif
}
