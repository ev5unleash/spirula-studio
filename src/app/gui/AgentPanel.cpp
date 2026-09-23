#include "app/gui/AgentPanel.h"

#include "app/AgentAdminBroker.h"
#include "app/AgentConfig.h"
#include "app/AgentLeader.h"
#include "app/AgentPairing.h"
#include "app/AgentServiceManager.h"
#include "app/AgentState.h"
#include "app/AgentTls.h"
#include "app/AppPaths.h"
#include "app/gui/FileDialog.h"
#include "app/gui/Layout.h"
#include "app/gui/Ui.h"
#include "i18n/catalog/AgentPanel.h"
#include "i18n/catalog/Dataset.h"
#include "i18n/catalog/Gui.h"

#include "imgui.h"

#include <cerrno>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shlobj.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace gui {
namespace {
namespace agent = app::agent;
namespace ap = spirula::i18n::msg::agent_panel;
namespace gm = spirula::i18n::msg::gui;
namespace dm = spirula::i18n::msg::dataset;
namespace update = agent::update;
namespace fs = std::filesystem;
using spirula::i18n::Msg;
using Fingerprint = agent::TlsChannel::PeerFingerprint;
using NativeSocket = agent::TlsChannel::NativeSocket;

void wipe(std::string& value) noexcept {
    volatile char* data = value.empty() ? nullptr : value.data();
    for (std::size_t i = 0; i < value.size(); ++i) data[i] = 0;
    value.clear();
}

void wipe(Fingerprint& value) noexcept {
    volatile std::uint8_t* data = value.data();
    for (std::size_t i = 0; i < value.size(); ++i) data[i] = 0;
}

std::string path_text(const fs::path& path) {
    return path.generic_u8string();
}

std::string fingerprint_text(const Fingerprint& pin) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string text(pin.size() * 2, '0');
    for (std::size_t i = 0; i < pin.size(); ++i) {
        text[i * 2] = hex[pin[i] >> 4];
        text[i * 2 + 1] = hex[pin[i] & 15];
    }
    return text;
}

bool parse_fingerprint(const std::string& text, Fingerprint& pin) {
    if (text.size() != pin.size() * 2) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < pin.size(); ++i) {
        const int high = nibble(text[i * 2]);
        const int low = nibble(text[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        pin[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

std::string timestamp(std::uint64_t millis) {
    if (!millis) return {};
    const std::time_t seconds = static_cast<std::time_t>(millis / 1000);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &seconds) != 0) return {};
#else
    if (!localtime_r(&seconds, &local)) return {};
#endif
    std::ostringstream text;
    text << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return text.str();
}

bool service_stopped(const agent::service::Status& status) {
    return status.state == agent::service::State::NotInstalled ||
           status.state == agent::service::State::Stopped ||
           status.state == agent::service::State::Failed;
}

const Msg& pairing_label(agent::pairing::WorkerStatus status) {
    switch (status) {
        case agent::pairing::WorkerStatus::Unpaired: return ap::unpaired;
        case agent::pairing::WorkerStatus::PendingApproval: return ap::pending_approval;
        case agent::pairing::WorkerStatus::Paired: return ap::paired;
    }
    return ap::unknown;
}

const Msg& connection_label(agent::wire::ConnectionState state) {
    switch (state) {
        case agent::wire::ConnectionState::Connected: return ap::connected;
        case agent::wire::ConnectionState::Reconnecting: return ap::reconnecting;
        case agent::wire::ConnectionState::Disconnected: return ap::disconnected;
    }
    return ap::unknown;
}

const Msg& scheduling_label(agent::wire::SchedulingState state) {
    switch (state) {
        case agent::wire::SchedulingState::Accepting: return ap::accepting;
        case agent::wire::SchedulingState::Pausing: return ap::pausing;
        case agent::wire::SchedulingState::Paused: return ap::paused;
        case agent::wire::SchedulingState::Stopped: return ap::stopped;
    }
    return ap::unknown;
}

const Msg& compatibility_label(agent::wire::CompatibilityState state) {
    switch (state) {
        case agent::wire::CompatibilityState::Unknown: return ap::unknown;
        case agent::wire::CompatibilityState::Compatible: return ap::compatible;
        case agent::wire::CompatibilityState::Incompatible: return ap::incompatible;
    }
    return ap::unknown;
}

const Msg& health_label(agent::wire::HealthState state) {
    switch (state) {
        case agent::wire::HealthState::Healthy: return ap::healthy;
        case agent::wire::HealthState::Degraded: return ap::degraded;
        case agent::wire::HealthState::Unhealthy: return ap::unhealthy;
    }
    return ap::unknown;
}

const Msg& activity_label(agent::wire::ActivityState state) {
    switch (state) {
        case agent::wire::ActivityState::Idle: return ap::idle;
        case agent::wire::ActivityState::Feature: return ap::feature;
        case agent::wire::ActivityState::Reconstruction: return ap::reconstruction;
        case agent::wire::ActivityState::Training: return ap::training;
    }
    return ap::unknown;
}

const Msg& service_label(agent::service::State state) {
    switch (state) {
        case agent::service::State::NotInstalled: return ap::service_not_installed;
        case agent::service::State::Stopped: return ap::service_stopped;
        case agent::service::State::StartPending: return ap::service_start_pending;
        case agent::service::State::Running: return ap::service_running;
        case agent::service::State::StopPending: return ap::service_stop_pending;
        case agent::service::State::Paused: return ap::service_paused;
        case agent::service::State::Failed: return ap::service_failed;
        case agent::service::State::Unknown: return ap::unknown;
    }
    return ap::unknown;
}

const Msg& command_action_label(agent::wire::CommandAction action) {
    switch (action) {
        case agent::wire::CommandAction::Maintenance: return ap::maintenance;
        case agent::wire::CommandAction::Online: return ap::online;
        case agent::wire::CommandAction::Pause: return gm::pause;
        case agent::wire::CommandAction::Resume: return gm::resume;
        case agent::wire::CommandAction::Stop: return gm::stop;
        case agent::wire::CommandAction::RebootMachine: return ap::request_reboot;
        case agent::wire::CommandAction::ActivateUpdate: return ap::request_update;
        case agent::wire::CommandAction::RestartService:
            return ap::restart_spirula;
        case agent::wire::CommandAction::ForceRestartService:
            return ap::force_restart_spirula;
        }
        return ap::unknown;
    }

const Msg& command_state_label(agent::LeaderServer::CommandState state) {
    switch (state) {
        case agent::LeaderServer::CommandState::Pending: return ap::command_pending;
        case agent::LeaderServer::CommandState::Applied: return ap::command_applied;
        case agent::LeaderServer::CommandState::Rejected: return ap::command_rejected;
        case agent::LeaderServer::CommandState::Failed: return ap::command_failed;
        case agent::LeaderServer::CommandState::Expired: return ap::command_expired;
        case agent::LeaderServer::CommandState::Uncertain: return ap::command_uncertain;
        case agent::LeaderServer::CommandState::Disconnected: return ap::disconnected;
    }
    return ap::unknown;
}
const Msg& acknowledgment_label(agent::wire::AcknowledgmentOutcome outcome) {
    switch (outcome) {
        case agent::wire::AcknowledgmentOutcome::Accepted:
            return ap::command_ack_accepted;
        case agent::wire::AcknowledgmentOutcome::Rejected:
            return ap::command_rejected;
        case agent::wire::AcknowledgmentOutcome::Completed:
            return ap::command_ack_completed;
        case agent::wire::AcknowledgmentOutcome::Failed:
            return ap::command_failed;
    }
    return ap::unknown;
}

bool live_job_state(agent::LeaderServer::FeatureJobState state) {
    switch (state) {
        case agent::LeaderServer::FeatureJobState::Staging:
        case agent::LeaderServer::FeatureJobState::Queued:
        case agent::LeaderServer::FeatureJobState::Offered:
        case agent::LeaderServer::FeatureJobState::TransferringInputs:
        case agent::LeaderServer::FeatureJobState::Running:
        case agent::LeaderServer::FeatureJobState::Unknown:
        case agent::LeaderServer::FeatureJobState::ReceivingOutput:
            return true;
        case agent::LeaderServer::FeatureJobState::Succeeded:
        case agent::LeaderServer::FeatureJobState::Failed:
        case agent::LeaderServer::FeatureJobState::Interrupted:
        case agent::LeaderServer::FeatureJobState::Rejected:
        case agent::LeaderServer::FeatureJobState::Superseded:
            return false;
    }
    return false;
}
bool live_job_state(agent::LeaderServer::ReconstructionJobState state) {
    switch (state) {
        case agent::LeaderServer::ReconstructionJobState::Staging:
        case agent::LeaderServer::ReconstructionJobState::Queued:
        case agent::LeaderServer::ReconstructionJobState::Offered:
        case agent::LeaderServer::ReconstructionJobState::TransferringInputs:
        case agent::LeaderServer::ReconstructionJobState::Running:
        case agent::LeaderServer::ReconstructionJobState::Unknown:
        case agent::LeaderServer::ReconstructionJobState::ReceivingOutput:
            return true;
        case agent::LeaderServer::ReconstructionJobState::Succeeded:
        case agent::LeaderServer::ReconstructionJobState::Failed:
        case agent::LeaderServer::ReconstructionJobState::Interrupted:
        case agent::LeaderServer::ReconstructionJobState::Rejected:
        case agent::LeaderServer::ReconstructionJobState::Superseded:
            return false;
    }
    return false;
}
bool live_job_state(agent::LeaderServer::TrainingJobState state) {
    switch (state) {
        case agent::LeaderServer::TrainingJobState::Staging:
        case agent::LeaderServer::TrainingJobState::Queued:
        case agent::LeaderServer::TrainingJobState::Offered:
        case agent::LeaderServer::TrainingJobState::TransferringInputs:
        case agent::LeaderServer::TrainingJobState::Running:
        case agent::LeaderServer::TrainingJobState::Unknown:
        case agent::LeaderServer::TrainingJobState::ReceivingOutput:
            return true;
        case agent::LeaderServer::TrainingJobState::Succeeded:
        case agent::LeaderServer::TrainingJobState::Failed:
        case agent::LeaderServer::TrainingJobState::Interrupted:
        case agent::LeaderServer::TrainingJobState::Rejected:
        case agent::LeaderServer::TrainingJobState::Superseded:
            return false;
    }
    return false;
}

const Msg& job_state_label(agent::LeaderServer::FeatureJobState state) {
    switch (state) {
        case agent::LeaderServer::FeatureJobState::Staging: return ap::feature_state_staging;
        case agent::LeaderServer::FeatureJobState::Queued: return ap::feature_state_queued;
        case agent::LeaderServer::FeatureJobState::Offered: return ap::feature_state_offered;
        case agent::LeaderServer::FeatureJobState::TransferringInputs:
            return ap::feature_state_transferring_inputs;
        case agent::LeaderServer::FeatureJobState::Running: return ap::worker_working;
        case agent::LeaderServer::FeatureJobState::Unknown: return ap::unknown;
        case agent::LeaderServer::FeatureJobState::ReceivingOutput:
            return ap::feature_state_receiving_output;
        case agent::LeaderServer::FeatureJobState::Succeeded:
            return ap::feature_state_verified;
        case agent::LeaderServer::FeatureJobState::Failed: return ap::command_failed;
        case agent::LeaderServer::FeatureJobState::Interrupted:
            return ap::feature_state_interrupted;
        case agent::LeaderServer::FeatureJobState::Rejected: return ap::command_rejected;
        case agent::LeaderServer::FeatureJobState::Superseded:
            return ap::feature_state_superseded;
    }
    return ap::unknown;
}
const Msg& job_state_label(agent::LeaderServer::ReconstructionJobState state) {
    switch (state) {
        case agent::LeaderServer::ReconstructionJobState::Staging: return ap::feature_state_staging;
        case agent::LeaderServer::ReconstructionJobState::Queued: return ap::feature_state_queued;
        case agent::LeaderServer::ReconstructionJobState::Offered: return ap::feature_state_offered;
        case agent::LeaderServer::ReconstructionJobState::TransferringInputs:
            return ap::feature_state_transferring_inputs;
        case agent::LeaderServer::ReconstructionJobState::Running: return ap::worker_working;
        case agent::LeaderServer::ReconstructionJobState::Unknown: return ap::unknown;
        case agent::LeaderServer::ReconstructionJobState::ReceivingOutput:
            return ap::feature_state_receiving_output;
        case agent::LeaderServer::ReconstructionJobState::Succeeded:
            return ap::feature_state_verified;
        case agent::LeaderServer::ReconstructionJobState::Failed: return ap::command_failed;
        case agent::LeaderServer::ReconstructionJobState::Interrupted:
            return ap::feature_state_interrupted;
        case agent::LeaderServer::ReconstructionJobState::Rejected:
            return ap::command_rejected;
        case agent::LeaderServer::ReconstructionJobState::Superseded:
            return ap::feature_state_superseded;
    }
    return ap::unknown;
}
const Msg& job_state_label(agent::LeaderServer::TrainingJobState state) {
    switch (state) {
        case agent::LeaderServer::TrainingJobState::Staging: return ap::feature_state_staging;
        case agent::LeaderServer::TrainingJobState::Queued: return ap::feature_state_queued;
        case agent::LeaderServer::TrainingJobState::Offered: return ap::feature_state_offered;
        case agent::LeaderServer::TrainingJobState::TransferringInputs:
            return ap::feature_state_transferring_inputs;
        case agent::LeaderServer::TrainingJobState::Running: return ap::worker_working;
        case agent::LeaderServer::TrainingJobState::Unknown: return ap::unknown;
        case agent::LeaderServer::TrainingJobState::ReceivingOutput:
            return ap::feature_state_receiving_output;
        case agent::LeaderServer::TrainingJobState::Succeeded:
            return ap::feature_state_verified;
        case agent::LeaderServer::TrainingJobState::Failed: return ap::command_failed;
        case agent::LeaderServer::TrainingJobState::Interrupted:
            return ap::feature_state_interrupted;
        case agent::LeaderServer::TrainingJobState::Rejected: return ap::command_rejected;
        case agent::LeaderServer::TrainingJobState::Superseded:
            return ap::feature_state_superseded;
    }
    return ap::unknown;
}

std::string job_progress_text(const std::optional<double>& progress) {
    if (!progress || !std::isfinite(*progress) ||
        *progress < 0.0 || *progress > 1.0)
        return ap::unknown.get();
    std::ostringstream text;
    text << std::fixed << std::setprecision(0) << *progress * 100.0 << '%';
    return text.str();
}
std::string command_target_text(agent::wire::CommandAction action,
                                const std::string& target_job_id) {
    if (action != agent::wire::CommandAction::Stop)
        return ap::command_target_worker.get();
    return target_job_id.empty() ? ap::command_target_all_active.get()
                                 : target_job_id;
}


int update_os_choice(const std::string& platform) {
    if (platform == "windows") return 1;
    if (platform == "linux") return 2;
    if (platform == "macos") return 3;
    return 0;
}

const char* update_os_value(int choice) {
    switch (choice) {
        case 1: return "windows";
        case 2: return "linux";
        case 3: return "macos";
        default: return "";
    }
}

const char* update_architecture_value(int choice) {
    switch (choice) {
        case 1: return "x86_64";
        case 2: return "aarch64";
        default: return "";
    }
}
std::string capability_list(const std::vector<agent::wire::Capability>& values) {
    std::string text;
    for (const agent::wire::Capability value : values) {
        if (!text.empty()) text += ", ";
        switch (value) {
            case agent::wire::Capability::Feature: text += ap::feature.get(); break;
            case agent::wire::Capability::Reconstruction:
                text += ap::reconstruction.get();
                break;
            case agent::wire::Capability::Training: text += ap::training.get(); break;
        }
    }
    return text;
}

void field(const Msg& label, const std::string& value) {
    ui::Text(label);
    ImGui::SameLine(px(190.0f));
    if (value.empty()) ui::Text(ap::field_unavailable);
    else ui::TextRaw(value);
}

bool parse_u64(const std::string& text, std::uint64_t& value) {
    if (text.empty()) return false;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

std::vector<std::string> parse_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream input(text);
    for (std::string line; std::getline(input, line);) {
        const auto first = std::find_if_not(line.begin(), line.end(),
            [](unsigned char c) { return std::isspace(c) != 0; });
        const auto last = std::find_if_not(line.rbegin(), line.rend(),
            [](unsigned char c) { return std::isspace(c) != 0; }).base();
        if (first < last) lines.emplace_back(first, last);
    }
    return lines;
}

std::string join_lines(const std::vector<std::string>& lines) {
    std::string text;
    for (const std::string& line : lines) {
        if (!text.empty()) text += '\n';
        text += line;
    }
    return text;
}

#ifdef _WIN32
struct SocketRuntime {
    bool ready = false;
    bool started = false;
    SocketRuntime() {
        WSADATA data{};
        started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        ready = started && LOBYTE(data.wVersion) == 2 &&
                HIBYTE(data.wVersion) == 2;
        if (started && !ready) {
            WSACleanup();
            started = false;
        }
    }
    ~SocketRuntime() { if (started) WSACleanup(); }
};
#else
struct SocketRuntime {
    bool ready = true;
};
#endif

void close_socket(NativeSocket socket) noexcept {
    if (socket == agent::TlsChannel::kInvalidSocket) return;
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(socket));
#else
    close(static_cast<int>(socket));
#endif
}
struct SocketOwner {
    explicit SocketOwner(NativeSocket socket) noexcept : value(socket) {}
    SocketOwner(const SocketOwner&) = delete;
    SocketOwner& operator=(const SocketOwner&) = delete;
    NativeSocket value = agent::TlsChannel::kInvalidSocket;
    ~SocketOwner() { close_socket(value); }
    NativeSocket release() noexcept {
        return std::exchange(value, agent::TlsChannel::kInvalidSocket);
    }
};

NativeSocket connect_enrollment(const std::string& address, std::uint16_t port,
                                std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICHOST;
    addrinfo* addresses = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(address.c_str(), service.c_str(), &hints, &addresses) != 0) {
        error = "enrollment address must be a numeric IPv4 or IPv6 address";
        return agent::TlsChannel::kInvalidSocket;
    }
    NativeSocket connected = agent::TlsChannel::kInvalidSocket;
    for (addrinfo* item = addresses; item; item = item->ai_next) {
#ifdef _WIN32
        SOCKET socket = ::socket(item->ai_family, item->ai_socktype,
                                 item->ai_protocol);
        if (socket == INVALID_SOCKET) continue;
        u_long nonblocking = 1;
        if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0) {
            closesocket(socket);
            continue;
        }
        int result = ::connect(socket, item->ai_addr,
                               static_cast<int>(item->ai_addrlen));
        if (result != 0) {
            const int code = WSAGetLastError();
            if (code != WSAEWOULDBLOCK && code != WSAEINPROGRESS &&
                code != WSAEINVAL) {
                closesocket(socket);
                continue;
            }
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(socket, &writable);
            timeval timeout{10, 0};
            if (select(0, nullptr, &writable, nullptr, &timeout) <= 0) {
                closesocket(socket);
                continue;
            }
            int socket_error = 0;
            int error_size = sizeof(socket_error);
            if (getsockopt(socket, SOL_SOCKET, SO_ERROR,
                           reinterpret_cast<char*>(&socket_error),
                           &error_size) != 0 || socket_error != 0) {
                closesocket(socket);
                continue;
            }
        }
        nonblocking = 0;
        if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0) {
            closesocket(socket);
            continue;
        }
        connected = static_cast<NativeSocket>(socket);
#else
        const int socket = ::socket(item->ai_family, item->ai_socktype,
                                    item->ai_protocol);
        if (socket < 0) continue;
        const int flags = fcntl(socket, F_GETFL, 0);
        if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) != 0) {
            close(socket);
            continue;
        }
        int result = ::connect(socket, item->ai_addr, item->ai_addrlen);
        if (result != 0 && errno != EINPROGRESS) {
            close(socket);
            continue;
        }
        if (result != 0) {
            pollfd descriptor{socket, POLLOUT, 0};
            if (poll(&descriptor, 1, 10000) <= 0) {
                close(socket);
                continue;
            }
            int socket_error = 0;
            socklen_t error_size = sizeof(socket_error);
            if (getsockopt(socket, SOL_SOCKET, SO_ERROR, &socket_error,
                           &error_size) != 0 || socket_error != 0) {
                close(socket);
                continue;
            }
        }
        if (fcntl(socket, F_SETFL, flags) != 0) {
            close(socket);
            continue;
        }
        connected = static_cast<NativeSocket>(socket);
#endif
        break;
    }
    freeaddrinfo(addresses);
    if (connected == agent::TlsChannel::kInvalidSocket)
        error = "could not connect to the configured enrollment endpoint";
    return connected;
}

fs::path default_machine_root() {
#ifdef _WIN32
    PWSTR common = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &common)))
        return {};
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(common, &CoTaskMemFree);
    return fs::path(owned.get()) / L"Spirula" / L"RemoteWorker";
#elif defined(__APPLE__)
    return fs::path("/Library/Application Support/Spirula/RemoteWorker");
#else
    return fs::path("/var/lib/spirula/remote-worker");
#endif
}

agent::service::Configuration default_service_configuration() {
    agent::service::Configuration config;
    config.executable = fs::u8path(app::exe_path());
#ifdef _WIN32
    const fs::path base = default_machine_root();
    config.config_root = base / L"config";
    config.state_root = base / L"state";
    config.storage_root = base / L"storage";
#elif defined(__APPLE__)
    const fs::path base = default_machine_root();
    config.config_root = base / "config";
    config.state_root = base / "state";
    config.storage_root = base / "work";
#else
    config.config_root = fs::path("/etc/spirula/remote-worker");
    const fs::path base = default_machine_root();
    config.state_root = base / "state";
    config.storage_root = base / "work";
#endif
    return config;
}
std::optional<agent::pairing::Worker> open_managed_worker(
        const fs::path& root, std::string* error) {
    const agent::service::Result security =
        agent::service::validate_state_security(root);
    if (!security.success) {
        if (error)
            *error = security.error.empty()
                ? "worker state security validation failed" : security.error;
        return std::nullopt;
    }
    return agent::pairing::Worker::OpenMachine(root, error);
}


}  // namespace

struct AgentPanel::Impl {
    enum class Kind {
        LoadPolicy, SavePolicy, StartLeader, StopLeader, IssueInvitation,
        Approve, Reject, Revoke, SendCommand, RequestReboot, RequestUpdate,
        InstallAdminBroker, UninstallAdminBroker, RefreshPairing, Redeem,
        CheckApproval, Forget, RetryRebind, ServiceInstall, ServiceUninstall,
        ServiceStart, ServiceStop, StartupEnable, StartupDisable,
        SubmitFeatureShard, SupersedeFeatureShard,
        SubmitTraining, SupersedeTraining
#ifdef SS_TOOL_SFM
        , SubmitReconstruction, SupersedeReconstruction
#endif
    };
    enum class PathTarget {
        LeaderState, ConfigRoot, StateRoot, StorageRoot, UpdatePackage
    };

    struct Request {
        Kind kind = Kind::LoadPolicy;
        agent::LeaderServer::Options leader_options;
        agent::service::Configuration service_config;
        agent::Config policy;
        agent::admin::TargetBinding admin_target;
        fs::path config_root;
        fs::path update_package_file;
        update::PackageManifest update_manifest;
        std::uint64_t update_security_version = 0;
        std::string worker_id;
        std::string worker_name;
        std::string target_job_id;
        bool confirmed = false;
        std::string address;
        std::string server_name;
        std::string code;
        Fingerprint pin{};
        std::uint16_t port = 0;
        agent::wire::CommandAction command = agent::wire::CommandAction::Pause;
        AgentPanel::FeatureShardSubmission feature;
        AgentPanel::TrainingSubmission training;
#ifdef SS_TOOL_SFM
        AgentPanel::ReconstructionSubmission reconstruction;
#endif
    };

    struct CommandEntry {
        std::string worker_name;
        std::string worker_id;
        std::string target_job_id;
        std::string error;
        agent::wire::CommandAction action = agent::wire::CommandAction::Pause;
    };

    struct JobEntry {
        std::string job_id;
        const Msg* kind = nullptr;
        const Msg* state = nullptr;
        std::optional<double> progress;
        bool live = false;
        bool result_verified = false;
    };


    struct ErrorEntry {
        std::string worker_name;
        std::string error;
    };

    struct ViewState {
        bool busy = false;
        bool leader_running = false;
        bool snapshots_available = false;
        std::uint16_t operational_port = 0;
        std::uint16_t enrollment_port = 0;
        std::string leader_pin;
        std::string update_signer_pin;
        std::vector<agent::LeaderServer::WorkerSnapshot> workers;
        std::vector<agent::LeaderServer::CommandResult> command_snapshots;
        std::vector<agent::LeaderServer::FeatureJobSnapshot> feature_jobs;
        std::map<std::string, std::string> feature_errors;
        std::vector<agent::LeaderServer::TrainingJobSnapshot> training_jobs;
        std::map<std::string, std::string> training_errors;
#ifdef SS_TOOL_SFM
        std::vector<agent::LeaderServer::ReconstructionJobSnapshot>
            reconstruction_jobs;
        std::map<std::string, std::string> reconstruction_errors;
#endif
        agent::service::Status service;
        std::string service_query_error;
        bool policy_loaded = false;
        fs::path policy_root;
        agent::Config policy;
        std::string policy_error;
        std::uint64_t policy_revision = 0;
        bool pairing_known = false;
        agent::pairing::WorkerStatus pairing_status =
            agent::pairing::WorkerStatus::Unpaired;
        std::string worker_id;
        std::string paired_leader_id;
        std::uint64_t paired_leader_epoch = 0;
        std::uint64_t pairing_revision = 0;
        bool rebind_pending = false;
        std::string rebind_error;
        bool operation_done = false;
        std::string operation_error;
        std::deque<CommandEntry> commands;
        std::deque<ErrorEntry> errors;
    };

    struct Event {
        bool finished = false;
        bool has_invitation = false;
        std::string invitation;
        std::uint64_t invitation_expiry = 0;
        bool clear_pairing_secrets = false;
    };

    struct PendingRebind {
        fs::path state_root;
        std::string worker_id;
        std::string leader_id;
        std::uint64_t leader_epoch = 0;
    };

    enum class ConfirmKind {
        None, LeaderStart, LeaderStop, Approve, Reject, Revoke, Command,
        RequestReboot, RequestUpdate, InstallAdminBroker, UninstallAdminBroker,
        Redeem, CheckApproval, Forget, RetryRebind, PolicySave,
        ServiceInstall, ServiceUninstall, ServiceStart, ServiceStop,
        StartupEnable, StartupDisable
    };

    Impl()
        : _leader_state(path_text(fs::u8path(app::config_dir()))),
          _leader_bind("127.0.0.1"), _leader_server_name("localhost"),
          _pair_address("127.0.0.1"), _pair_server_name("localhost"),
          _leader_operation_port(47000), _leader_enrollment_port(47001),
          _pair_port(47001) {
        const agent::service::Configuration defaults =
            default_service_configuration();
        _service_executable = path_text(defaults.executable);
        _config_root = path_text(defaults.config_root);
        _worker_state_root = path_text(defaults.state_root);
        _storage_root = path_text(defaults.storage_root);
        _policy.leader_address = _pair_address;
        _policy.leader_server_name = _pair_server_name;
        _policy.leader_port = 47000;
        _policy.max_concurrent_jobs = 1;
        _policy.disk_budget_bytes = 1024ULL * 1024ULL * 1024ULL;
        _policy_address = _policy.leader_address;
        _policy_server_name = _policy.leader_server_name;
        _policy_port = _policy.leader_port;
        _disk_budget_text = std::to_string(_policy.disk_budget_bytes);
        state.service.state = agent::service::State::Unknown;
        _published = std::make_shared<const ViewState>(state);
        _thread = std::thread([this] { worker_loop(); });
    }

    ~Impl() { shutdown(); }

    std::shared_ptr<const ViewState> view() const {
        return std::atomic_load_explicit(&_published, std::memory_order_acquire);
    }

    void publish() {
        std::atomic_store_explicit(&_published,
            std::make_shared<const ViewState>(state), std::memory_order_release);
    }

    bool enqueue(Request request) {
        {
            std::lock_guard<std::mutex> lock(_queue_mutex);
            if (_closing || _queued) {
                wipe_request(request);
                return false;
            }
            _queued = true;
            _queue.push_back(std::move(request));
        }
        _queue_cv.notify_one();
        return true;
    }

    bool enqueue_feature(Request request) {
        {
            std::lock_guard<std::mutex> lock(_queue_mutex);
            if (_closing || _queue.size() >= 256) return false;
            _queue.push_back(std::move(request));
        }
        _queue_cv.notify_one();
        return true;
    }

    static void wipe_request(Request& request) noexcept {
        wipe(request.code);
        wipe(request.pin);
    }

    void post_event(Event event) {
        std::lock_guard<std::mutex> lock(_event_mutex);
        _events.push_back(std::move(event));
    }

    void finish(bool success, std::string error = {},
                bool report_operation = true) {
        state.operation_done = report_operation;
        state.operation_error = report_operation && !success
            ? std::move(error) : std::string{};
        state.busy = false;
        publish();
        Event event;
        event.finished = true;
        post_event(std::move(event));
    }

    void set_pairing(agent::pairing::Worker& worker) {
        state.pairing_known = true;
        state.pairing_status = worker.Status();
        state.worker_id = worker.WorkerId();
        state.paired_leader_id = worker.LeaderId();
        state.paired_leader_epoch = worker.LeaderEpoch();
        ++state.pairing_revision;
    }

    bool installed_root(const agent::service::Status& current,
                        const fs::path& root, std::string& error) {
        if (!current.configuration ||
            current.configuration->state_root != root) {
            error = "worker service state root changed; refresh the installed service configuration";
            return false;
        }
        return true;
    }

    bool require_pairing_service(
            const agent::service::Configuration& expected,
            std::string& error) {
        const agent::service::Status current = agent::service::query_status();
        state.service = current;
        if (!current.configuration) {
            error = "install the local worker service before pairing";
            return false;
        }
        if (current.configuration->executable != expected.executable ||
            current.configuration->config_root != expected.config_root ||
            current.configuration->state_root != expected.state_root ||
            current.configuration->storage_root != expected.storage_root) {
            error = "installed worker service configuration changed; refresh before pairing";
            return false;
        }
        if (!service_stopped(current)) {
            error = "stop the local worker service before changing pairing state";
            return false;
        }
        return true;
    }

    bool check_existing_rebind(const fs::path& root, std::string& error) {
        const agent::service::Status current = agent::service::query_status();
        state.service = current;
        if (!installed_root(current, root, error)) return false;
        std::string open_error;
        auto worker = open_managed_worker(root, &open_error);
        if (!worker) {
            error = open_error.empty() ? "could not inspect local worker pairing" : open_error;
            return false;
        }
        set_pairing(*worker);
        if (worker->Status() != agent::pairing::WorkerStatus::Paired) {
            state.rebind_pending = false;
            state.rebind_error.clear();
            _pending_rebind.reset();
            return true;
        }
        const std::string worker_id = worker->WorkerId();
        const std::string leader_id = worker->LeaderId();
        const std::uint64_t epoch = worker->LeaderEpoch();
        if (worker_id.empty() || leader_id.empty() || !epoch) {
            error = "paired worker identity is incomplete";
            return false;
        }
        try {
            const agent::State existing = agent::load_machine_state(root);
            if (existing.command_leader_id != leader_id ||
                existing.command_leader_epoch != epoch) {
                _pending_rebind = PendingRebind{root, worker_id, leader_id, epoch};
                state.rebind_pending = true;
                state.rebind_error = "approved pairing differs from the saved command watermark";
                error = state.rebind_error;
                return false;
            }
            state.rebind_pending = false;
            state.rebind_error.clear();
            _pending_rebind.reset();
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
        return true;
    }

    bool apply_rebind(const PendingRebind& pending, std::string& error) {
        const agent::service::Status current = agent::service::query_status();
        state.service = current;
        if (!service_stopped(current)) {
            error = "stop the local worker service before rebinding command state";
            state.rebind_pending = true;
            state.rebind_error = error;
            return false;
        }
        if (!installed_root(current, pending.state_root, error)) {
            state.rebind_pending = true;
            state.rebind_error = error;
            return false;
        }
        try {
            auto worker = open_managed_worker(pending.state_root, &error);
            if (!worker) throw std::runtime_error(
                error.empty() ? "could not inspect approved worker identity" : error);
            set_pairing(*worker);
            if (worker->Status() != agent::pairing::WorkerStatus::Paired ||
                worker->WorkerId() != pending.worker_id ||
                worker->LeaderId() != pending.leader_id ||
                worker->LeaderEpoch() != pending.leader_epoch) {
                error = "approved worker identity changed before command-state update";
                state.rebind_pending = true;
                state.rebind_error = error;
                return false;
            }
            agent::State saved = agent::load_machine_state(pending.state_root);
            agent::rebind_command_leader(saved, pending.leader_id,
                                         pending.leader_epoch);
            agent::save_machine_state(pending.state_root, saved);
            state.rebind_pending = false;
            state.rebind_error.clear();
            _pending_rebind.reset();
            return true;
        } catch (const std::exception& e) {
            state.rebind_pending = true;
            state.rebind_error = e.what();
            error = state.rebind_error;
            return false;
        }
    }
    bool complete_approved_pairing(
            std::optional<agent::pairing::Worker>& worker,
            const fs::path& state_root, Event& event, std::string& error) {
        const std::string worker_id = worker->WorkerId();
        const std::string leader_id = worker->LeaderId();
        const std::uint64_t epoch = worker->LeaderEpoch();
        worker.reset();
        event.clear_pairing_secrets = true;
        if (worker_id.empty() || leader_id.empty() || !epoch) {
            error = "approved worker identity is incomplete";
            return false;
        }
        PendingRebind pending{state_root, worker_id, leader_id, epoch};
        _pending_rebind = pending;
        state.rebind_pending = true;
        return apply_rebind(pending, error);
    }

    void add_command(CommandEntry entry) {
        state.commands.push_front(std::move(entry));
        while (state.commands.size() > 24) state.commands.pop_back();
    }
    void refresh_command_snapshots() {
        if (!_leader) return;
        state.command_snapshots = _leader->CommandSnapshots();
        std::sort(state.command_snapshots.begin(),
                  state.command_snapshots.end(),
            [](const auto& left, const auto& right) {
                return left.issued_at_ms > right.issued_at_ms;
            });
    }

    void add_recent_error(const std::string& worker_id,
                          const std::string& name,
                          const std::string& error) {
        const auto previous = _last_errors.find(worker_id);
        if (error.empty()) {
            _last_errors.erase(worker_id);
            return;
        }
        if (previous != _last_errors.end() && previous->second == error) return;
        _last_errors[worker_id] = error;
        while (_last_errors.size() > 64)
            _last_errors.erase(_last_errors.begin());
        state.errors.push_front({name, error});
        while (state.errors.size() > 16) state.errors.pop_back();
    }

    void poll() {
        try {
            state.service = agent::service::query_status();
            state.service_query_error.clear();
        } catch (const std::exception& e) {
            state.service.state = agent::service::State::Unknown;
            state.service_query_error = e.what();
        }
        state.leader_running = _leader && _leader->Running();
        refresh_command_snapshots();
        state.snapshots_available = state.leader_running;
        if (state.snapshots_available) {
            state.workers = _leader->Snapshot();
            state.feature_jobs = _leader->FeatureJobs();
            state.training_jobs = _leader->TrainingJobs();
#ifdef SS_TOOL_SFM
            state.reconstruction_jobs = _leader->ReconstructionJobs();
#endif
            state.operational_port = _leader->OperationalPort();
            state.enrollment_port = _leader->EnrollmentPort();
            for (const auto& worker : state.workers)
                add_recent_error(worker.id, worker.label, worker.error);
        } else {
            state.workers.clear();
            state.feature_jobs.clear();
            state.feature_errors.clear();
            state.training_jobs.clear();
            state.training_errors.clear();
#ifdef SS_TOOL_SFM
            state.reconstruction_jobs.clear();
            state.reconstruction_errors.clear();
#endif
            state.operational_port = 0;
        }
        publish();
    }

    void process(Request& request) {
        std::string error;
        bool success = false;
        bool command_result_received = false;
        Event event;
        try {
            switch (request.kind) {
                case Kind::LoadPolicy: {
                    agent::Config loaded = agent::load_config(request.config_root);
                    state.policy = std::move(loaded);
                    state.policy_root = request.config_root;
                    state.policy_loaded = true;
                    state.policy_error.clear();
                    ++state.policy_revision;
                    success = true;
                    break;
                }
                case Kind::SavePolicy: {
                    const auto current = agent::service::query_status();
                    state.service = current;
                    if (!service_stopped(current)) {
                        error = "stop the local worker service before saving machine policy";
                        break;
                    }
                    if (current.configuration &&
                        (current.configuration->config_root != request.config_root ||
                         !state.policy_loaded ||
                         state.policy_root != request.config_root)) {
                        error = "load the installed worker policy before saving it";
                        break;
                    }
                    agent::save_config(request.config_root, request.policy);
                    state.policy = request.policy;
                    state.policy_loaded = true;
                    state.policy_root = request.config_root;
                    state.policy_error.clear();
                    ++state.policy_revision;
                    success = true;
                    break;
                }
                case Kind::StartLeader: {
                    if (_leader && _leader->Running()) {
                        error = "leader is already running";
                        break;
                    }
                    auto started = agent::LeaderServer::Start(
                        std::move(request.leader_options), &error);
                    if (!started) break;
                    _leader = std::move(started);
                    state.leader_pin = fingerprint_text(
                        _leader->EnrollmentPeerFingerprint());
                    state.update_signer_pin = _leader->UpdateSignerFingerprint();
                    state.leader_running = true;
                    state.snapshots_available = true;
                    state.operational_port = _leader->OperationalPort();
                    state.enrollment_port = _leader->EnrollmentPort();
                    success = true;
                    break;
                }
                case Kind::StopLeader:
                    if (!_leader) {
                        error = "leader is not running";
                        break;
                    }
                    _leader->Stop();
                    refresh_command_snapshots();
                    _leader.reset();
                    state.leader_running = false;
                    state.snapshots_available = false;
                    state.workers.clear();
                    state.operational_port = 0;
                    state.enrollment_port = 0;
                    state.leader_pin.clear();
                    state.update_signer_pin.clear();
                    success = true;
                    break;
                case Kind::IssueInvitation: {
                    if (!_leader || !_leader->Running()) {
                        error = "leader is not running";
                        break;
                    }
                    auto invitation = _leader->IssueInvitation(
                        std::chrono::minutes(5), &error);
                    if (!invitation) break;
                    event.has_invitation = true;
                    event.invitation = std::move(invitation->code);
                    event.invitation_expiry = invitation->expires_at_unix;
                    success = true;
                    break;
                }
                case Kind::Approve:
                    success = _leader && _leader->Approve(request.worker_id, &error);
                    if (!success && error.empty()) error = "leader is not running";
                    break;
                case Kind::Reject:
                    success = _leader && _leader->Reject(request.worker_id, &error);
                    if (!success && error.empty()) error = "leader is not running";
                    break;
                case Kind::Revoke:
                    success = _leader && _leader->Revoke(request.worker_id, &error);
                    if (!success && error.empty()) error = "leader is not running";
                    break;
                case Kind::RequestReboot:
                case Kind::RequestUpdate: {
                    std::optional<agent::LeaderServer::CommandResult> result;
                    if (!request.confirmed) {
                        error = ap::admin_action_confirmation_required.get();
                    } else if (_leader) {
                        if (request.kind == Kind::RequestReboot) {
                            result = _leader->RequestReboot(
                                request.worker_id, request.confirmed, &error);
                        } else {
                            result = _leader->RequestUpdate(
                                request.worker_id, request.update_package_file,
                                std::move(request.update_manifest),
                                request.update_security_version,
                                request.confirmed, &error);
                        }
                    }
                    if (result) {
                        success = true;
                        command_result_received = true;
                        refresh_command_snapshots();
                    } else if (error.empty()) {
                        error = ap::leader_stopped.get();
                    }
                    if (!result) {
                        CommandEntry entry;
                        entry.worker_name = request.worker_name;
                        entry.worker_id = request.worker_id;
                        entry.action = request.kind == Kind::RequestReboot
                            ? agent::wire::CommandAction::RebootMachine
                            : agent::wire::CommandAction::ActivateUpdate;
                        entry.error = error;
                        add_command(std::move(entry));
                    }
                    break;
                }
                case Kind::InstallAdminBroker: {
                    if (!request.confirmed) {
                        error = ap::admin_action_confirmation_required.get();
                        break;
                    }
                    const agent::service::Result result =
                        agent::admin::install_broker(request.admin_target);
                    success = result.success;
                    error = result.error;
                    if (!success && error.empty())
                        error = ap::admin_broker_failed.get();
                    break;
                }
                case Kind::UninstallAdminBroker: {
                    if (!request.confirmed) {
                        error = ap::admin_action_confirmation_required.get();
                        break;
                    }
                    const agent::service::Result result =
                        agent::admin::uninstall_broker();
                    success = result.success;
                    error = result.error;
                    if (!success && error.empty())
                        error = ap::admin_broker_failed.get();
                    break;
                }
                case Kind::SendCommand: {
                    bool pending = false;
                    if (_leader) {
                        for (const auto& command : _leader->CommandSnapshots()) {
                            if (command.worker_id == request.worker_id &&
                                command.state ==
                                    agent::LeaderServer::CommandState::Pending) {
                                pending = true;
                                break;
                            }
                        }
                    }
                    if (pending) {
                        error = ap::command_pending_conflict.get();
                    } else if ((request.command ==
                                    agent::wire::CommandAction::Stop ||
                                request.command ==
                                    agent::wire::CommandAction::ForceRestartService) &&
                               !request.confirmed) {
                        error = ap::command_confirmation_required.get();
                    } else {
                        std::optional<agent::LeaderServer::CommandResult> result;
                        if (_leader) {
                            if (request.command == agent::wire::CommandAction::Stop ||
                                request.command ==
                                    agent::wire::CommandAction::ForceRestartService) {
                                result = _leader->SendCommand(
                                    request.worker_id, request.command,
                                    request.target_job_id, request.confirmed,
                                    std::chrono::seconds(30), &error);
                            } else {
                                result = _leader->SendCommand(
                                    request.worker_id, request.command,
                                    std::chrono::seconds(30), &error);
                            }
                        }
                        if (result) {
                            success = true;
                            command_result_received = true;
                            refresh_command_snapshots();
                        } else if (error.empty()) {
                            error = ap::leader_stopped.get();
                        }
                    }
                    if (!success || !error.empty()) {
                        CommandEntry entry;
                        entry.worker_name = request.worker_name;
                        entry.worker_id = request.worker_id;
                        entry.target_job_id = request.target_job_id;
                        entry.action = request.command;
                        entry.error = error;
                        add_command(std::move(entry));
                    }
                    break;
                }
                case Kind::RefreshPairing: {
                    if (!require_pairing_service(request.service_config, error)) break;
                    auto worker = open_managed_worker(
                        request.service_config.state_root, &error);
                    if (!worker) break;
                    set_pairing(*worker);
                    success = true;
                    if (worker->Status() !=
                        agent::pairing::WorkerStatus::Paired) {
                        state.rebind_pending = false;
                        state.rebind_error.clear();
                        _pending_rebind.reset();
                    } else {
                        const std::string worker_id = worker->WorkerId();
                        const std::string leader_id = worker->LeaderId();
                        const std::uint64_t epoch = worker->LeaderEpoch();
                        if (worker_id.empty() || leader_id.empty() || !epoch) {
                            _pending_rebind.reset();
                            state.rebind_pending = false;
                            state.rebind_error.clear();
                            error = "paired worker identity is incomplete";
                            success = false;
                        } else {
                            try {
                                const agent::State saved =
                                    agent::load_machine_state(
                                        request.service_config.state_root);
                                if (saved.command_leader_id != leader_id ||
                                    saved.command_leader_epoch != epoch) {
                                    _pending_rebind = PendingRebind{
                                        request.service_config.state_root,
                                        worker_id, leader_id, epoch};
                                    state.rebind_pending = true;
                                    state.rebind_error =
                                        "approved pairing differs from the saved command watermark";
                                } else {
                                    state.rebind_pending = false;
                                    state.rebind_error.clear();
                                    _pending_rebind.reset();
                                }
                            } catch (const std::exception& e) {
                                _pending_rebind = PendingRebind{
                                    request.service_config.state_root,
                                    worker_id, leader_id, epoch};
                                state.rebind_pending = true;
                                state.rebind_error = e.what();
                            }
                        }
                    }
                    break;
                }
                case Kind::Redeem: {
                    if (!require_pairing_service(request.service_config, error)) break;
                    SocketRuntime runtime;
                    if (!runtime.ready) {
                        error = "could not initialize the TCP socket runtime";
                        break;
                    }
                    SocketOwner socket{connect_enrollment(
                        request.address, request.port, error)};
                    if (socket.value == agent::TlsChannel::kInvalidSocket) break;
                    auto worker = open_managed_worker(
                        request.service_config.state_root, &error);
                    if (!worker) break;
                    const bool redeemed = worker->Redeem(
                        socket.release(), request.code, request.pin,
                        request.server_name, request.worker_name, &error);
                    set_pairing(*worker);
                    success = redeemed;
                    if (redeemed && worker->Status() ==
                            agent::pairing::WorkerStatus::Paired)
                        success = complete_approved_pairing(
                            worker, request.service_config.state_root,
                            event, error);
                    break;
                }
                case Kind::CheckApproval: {
                    if (!require_pairing_service(request.service_config, error)) break;
                    SocketRuntime runtime;
                    if (!runtime.ready) {
                        error = "could not initialize the TCP socket runtime";
                        break;
                    }
                    SocketOwner socket{connect_enrollment(
                        request.address, request.port, error)};
                    if (socket.value == agent::TlsChannel::kInvalidSocket) break;
                    auto worker = open_managed_worker(
                        request.service_config.state_root, &error);
                    if (!worker) break;
                    const bool checked = worker->CheckApproval(
                        socket.release(), request.code, request.pin,
                        request.server_name, &error);
                    set_pairing(*worker);
                    success = checked;
                    if (checked && worker->Status() ==
                            agent::pairing::WorkerStatus::Paired)
                        success = complete_approved_pairing(
                            worker, request.service_config.state_root,
                            event, error);
                    break;
                }
                case Kind::Forget: {
                    if (!require_pairing_service(request.service_config, error)) break;
                    auto worker = open_managed_worker(
                        request.service_config.state_root, &error);
                    if (!worker) break;
                    success = worker->Forget(&error);
                    if (success) {
                        state.pairing_known = true;
                        state.pairing_status = agent::pairing::WorkerStatus::Unpaired;
                        state.worker_id.clear();
                        state.paired_leader_id.clear();
                        state.paired_leader_epoch = 0;
                        ++state.pairing_revision;
                        event.clear_pairing_secrets = true;
                    }
                    break;
                }
                case Kind::RetryRebind:
                    if (!_pending_rebind) {
                        error = "there is no approved pairing awaiting state rebind";
                        break;
                    }
                    success = apply_rebind(*_pending_rebind, error);
                    break;
                case Kind::ServiceInstall: {
                    const auto result = agent::service::install(
                        request.service_config);
                    success = result.success;
                    error = result.error;
                    if (!success) break;
                    std::string rebind_error;
                    if (!check_existing_rebind(request.service_config.state_root,
                                               rebind_error)) {
                        const auto disabled = agent::service::disable_startup();
                        state.service = agent::service::query_status();
                        if (!disabled.success) {
                            error = rebind_error.empty() ? disabled.error
                                                         : rebind_error + "; " + disabled.error;
                            error += "; could not disable automatic startup";
                            success = false;
                        } else if (!state.rebind_pending) {
                            error = std::move(rebind_error);
                            success = false;
                        }
                    }
                    break;
                }
                case Kind::ServiceUninstall: {
                    if (!check_existing_rebind(request.service_config.state_root,
                                               error)) break;
                    const agent::service::Status current =
                        agent::service::query_status();
                    state.service = current;
                    if (!service_stopped(current) ||
                        current.state == agent::service::State::NotInstalled) {
                        error = "stop the local worker service before uninstalling it";
                        break;
                    }
                    const auto result = agent::service::uninstall();
                    success = result.success;
                    error = result.error;
                    break;
                }
                case Kind::ServiceStart: {
                    if (!check_existing_rebind(request.service_config.state_root,
                                               error)) break;
                    const auto result = agent::service::start();
                    success = result.success;
                    error = result.error;
                    break;
                }
                case Kind::ServiceStop: {
                    const auto result = agent::service::stop();
                    success = result.success;
                    error = result.error;
                    break;
                }
                case Kind::StartupEnable: {
                    if (!check_existing_rebind(request.service_config.state_root,
                                               error)) break;
                    const auto result = agent::service::enable_startup();
                    success = result.success;
                    error = result.error;
                    break;
                }
                case Kind::StartupDisable: {
                    const auto result = agent::service::disable_startup();
                    success = result.success;
                    error = result.error;
                    break;
                }
                case Kind::SubmitFeatureShard:
                    success = _leader && _leader->SubmitFeatureShard(
                        request.feature.worker_id, request.feature.job_id,
                        request.feature.plan_path, request.feature.request_path,
                        request.feature.image_root, request.feature.mask_root,
                        request.feature.disk_budget_bytes, &error);
                    if (!success && error.empty()) error = "leader is not running";
                    break;
                case Kind::SupersedeFeatureShard:
                    success = _leader && _leader->SupersedeFeatureShard(
                        request.feature.job_id, &error);
                    if (!success && error.empty()) error = "leader is not running";
                    break;
                case Kind::SubmitTraining:
                    success = _leader && _leader->SubmitTraining(
                        request.training.worker_id, request.training.job_id,
                        std::move(request.training.config),
                        request.training.preset,
                        request.training.resume_checkpoint,
                        request.training.disk_budget_bytes, &error);
                    if (!success && error.empty()) error = "leader is not running";
                    break;
                case Kind::SupersedeTraining:
                    success = _leader && _leader->SupersedeTraining(
                        request.training.job_id, &error);
                    if (!success && error.empty()) error = "leader is not running";
                    break;

#ifdef SS_TOOL_SFM
                case Kind::SubmitReconstruction:
                    success = _leader && _leader->SubmitReconstruction(
                        request.reconstruction.worker_id,
                        request.reconstruction.job_id,
                        std::move(request.reconstruction.request),
                        request.reconstruction.source_manifest,
                        request.reconstruction.disk_budget_bytes, &error);
                    if (!success && error.empty()) error = "leader is not running";
                    break;
                case Kind::SupersedeReconstruction:
                    success = _leader && _leader->SupersedeReconstruction(
                        request.reconstruction.job_id, &error);
                    if (!success && error.empty()) error = "leader is not running";
                    break;
#endif
            }
        } catch (const std::exception& e) {
            error = e.what();
            success = false;
        } catch (...) {
            error = "worker-management operation failed";
            success = false;
        }
        if (request.kind == Kind::CheckApproval && event.clear_pairing_secrets)
            success = error.empty();
        if (request.kind == Kind::SubmitFeatureShard ||
            request.kind == Kind::SupersedeFeatureShard) {
            if (success) state.feature_errors.erase(request.feature.job_id);
            else state.feature_errors[request.feature.job_id] =
                error.empty() ? "feature-shard operation failed" : std::move(error);
            publish();
            wipe_request(request);
            return;
        }
        if (request.kind == Kind::SubmitTraining ||
            request.kind == Kind::SupersedeTraining) {
            const std::string& job_id = request.training.job_id;
            if (success) state.training_errors.erase(job_id);
            else state.training_errors[job_id] =
                error.empty() ? "training operation failed" : std::move(error);
            publish();
            wipe_request(request);
            return;
        }
#ifdef SS_TOOL_SFM
        if (request.kind == Kind::SubmitReconstruction ||
            request.kind == Kind::SupersedeReconstruction) {
            const std::string& job_id = request.reconstruction.job_id;
            if (success) state.reconstruction_errors.erase(job_id);
            else state.reconstruction_errors[job_id] =
                error.empty() ? "reconstruction operation failed" : std::move(error);
            publish();
            wipe_request(request);
            return;
        }
#endif
        finish(success, std::move(error), !command_result_received);
        if (event.has_invitation || event.clear_pairing_secrets) {
            event.finished = false;
            post_event(std::move(event));
        }
        wipe_request(request);
    }

    void worker_loop() {
        auto next_poll = std::chrono::steady_clock::now();
        for (;;) {
            Request request;
            bool have_request = false;
            {
                std::unique_lock<std::mutex> lock(_queue_mutex);
                _queue_cv.wait_until(lock, next_poll, [this] {
                    return _closing || !_queue.empty();
                });
                if (_closing) break;
                if (!_queue.empty()) {
                    request = std::move(_queue.front());
                    _queue.pop_front();
                    have_request = true;
                }
            }
            if (have_request) {
                const bool management =
                    request.kind != Kind::SubmitFeatureShard &&
                    request.kind != Kind::SupersedeFeatureShard
#ifdef SS_TOOL_SFM
                    && request.kind != Kind::SubmitReconstruction &&
                    request.kind != Kind::SupersedeReconstruction
#endif
                    && request.kind != Kind::SubmitTraining &&
                    request.kind != Kind::SupersedeTraining
                    ;
                if (management) {
                    state.busy = true;
                    state.operation_done = false;
                    state.operation_error.clear();
                    publish();
                }
                process(request);
                if (management) next_poll = std::chrono::steady_clock::now();
            }
            if (std::chrono::steady_clock::now() >= next_poll) {
                poll();
                next_poll = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(750);
            }
        }
        if (_leader) {
            _leader->Stop();
            _leader.reset();
        }
        state.busy = false;
        state.workers.clear();
        state.feature_jobs.clear();
#ifdef SS_TOOL_SFM
        state.reconstruction_jobs.clear();
#endif
        publish();
        state.leader_pin.clear();
        state.update_signer_pin.clear();
        state.training_jobs.clear();
        publish();
    }


    void drain_events() {
        std::deque<Event> events;
        {
            std::lock_guard<std::mutex> lock(_event_mutex);
            events.swap(_events);
        }
        for (Event& event : events) {
            if (event.finished) _queued = false;
            if (event.has_invitation) {
                wipe(_issued_invitation);
                if (_open) {
                    _issued_invitation = std::move(event.invitation);
                    _issued_expiry = event.invitation_expiry;
                } else {
                    wipe(event.invitation);
                    _issued_expiry = 0;
                }
            }
            if (event.clear_pairing_secrets) {
                wipe(_pair_code);
                wipe(_pin_text);
            }
        }
    }

    void confirm(Request request, ConfirmKind kind, const Msg& label) {
        if (_queued) {
            wipe_request(request);
            return;
        }
        if (_pending_request) wipe_request(*_pending_request);
        _pending_request = std::move(request);
        _confirm_kind = kind;
        _confirm_label = &label;
        _confirm_open = true;
    }

    bool queue_confirmed() {
        if (!_pending_request) return false;
        if ((_confirm_kind == ConfirmKind::Command &&
             (_pending_request->command == agent::wire::CommandAction::Stop ||
              _pending_request->command ==
                  agent::wire::CommandAction::ForceRestartService)) ||
            _confirm_kind == ConfirmKind::RequestReboot ||
            _confirm_kind == ConfirmKind::RequestUpdate ||
            _confirm_kind == ConfirmKind::InstallAdminBroker ||
            _confirm_kind == ConfirmKind::UninstallAdminBroker)
            _pending_request->confirmed = true;
        Request request = std::move(*_pending_request);
        _pending_request.reset();
        _confirm_kind = ConfirmKind::None;
        _confirm_open = false;
        return enqueue(std::move(request));
    }

    void cancel_confirmation() {
        if (_pending_request) wipe_request(*_pending_request);
        _pending_request.reset();
        _confirm_kind = ConfirmKind::None;
        _confirm_open = false;
        ImGui::CloseCurrentPopup();
    }

    agent::service::Configuration service_configuration() const {
        agent::service::Configuration config;
        config.executable = fs::u8path(_service_executable);
        config.config_root = fs::u8path(_config_root);
        config.state_root = fs::u8path(_worker_state_root);
        config.storage_root = fs::u8path(_storage_root);
        return config;
    }

    void open_path(PathTarget target, const std::string& start) {
        _path_target = target;
        _path_dialog.open(ap::section_service.get(), FileDialog::Mode::Folder,
                          {}, start);
    }

    void draw_path_field(const Msg& label, std::string& value,
                         PathTarget target, bool enabled) {
        ImGui::PushID(static_cast<int>(target));
        ImGui::BeginDisabled(!enabled);
        ImGui::SetNextItemWidth(-px(94.0f));
        ui::InputText(label, &value);
        ImGui::SameLine();
        if (ui::Button(dm::browse)) open_path(target, value);
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    void draw_path_dialog() {
        if (!_path_dialog.draw()) return;
        const std::string selected = _path_dialog.result();
        if (selected.empty()) return;
        switch (_path_target) {
            case PathTarget::LeaderState: _leader_state = selected; break;
            case PathTarget::ConfigRoot: _config_root = selected; break;
            case PathTarget::StateRoot: _worker_state_root = selected; break;
            case PathTarget::StorageRoot: _storage_root = selected; break;
            case PathTarget::UpdatePackage: _update_package_file = selected; break;
        }
    }
    void open_update_package() {
        const fs::path path = fs::u8path(_update_package_file);
        _path_target = PathTarget::UpdatePackage;
        _path_dialog.open(ap::remote_admin_section.get(), FileDialog::Mode::File,
                          {}, path_text(path.parent_path()));
    }
    void draw_update_package_field() {
        ImGui::PushID("update-package-path");
        ImGui::SetNextItemWidth(-px(94.0f));
        ui::InputText(ap::update_package_path, &_update_package_file);
        ImGui::SameLine();
        if (ui::Button(dm::browse)) open_update_package();
        ImGui::PopID();
    }

    void draw_operation_result(const ViewState& current) {
        if (_queued || current.busy) ui::TextDisabled(ap::operation_busy);
        else if (current.operation_done && current.operation_error.empty())
            ui::Text(ap::operation_succeeded);
        else if (current.operation_done)
            ui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f),
                            ap::operation_failed, {current.operation_error});
    }

    void draw_leader(const ViewState& current) {
        ImGui::PushID("leader");
        ui::SeparatorText(ap::section_leader);
        const bool can_edit = !_queued && !current.busy && !current.leader_running;
        ImGui::BeginDisabled(!can_edit);
        draw_path_field(ap::state_root, _leader_state, PathTarget::LeaderState,
                        can_edit);
        ImGui::SetNextItemWidth(-1);
        ui::InputText(ap::bind_address, &_leader_bind);
        ImGui::SetNextItemWidth(-1);
        ui::InputText(ap::server_name, &_leader_server_name);
        ImGui::SetNextItemWidth(-1);
        ui::InputInt(ap::operational_port, &_leader_operation_port);
        ImGui::SetNextItemWidth(-1);
        ui::InputInt(ap::enrollment_port, &_leader_enrollment_port);
        ui::TextWrapped(ap::private_listener_note);
        ImGui::EndDisabled();

        if (current.leader_running) {
            ui::TextColored(ImVec4(0.35f, 0.85f, 0.45f, 1.0f), ap::leader_running);
            field(ap::operational_port,
                  std::to_string(current.operational_port));
            field(ap::enrollment_port,
                  std::to_string(current.enrollment_port));
            field(ap::leader_pin, current.leader_pin);
            if (_leader_update_signer_pin_input != current.update_signer_pin)
                _leader_update_signer_pin_input = current.update_signer_pin;
            ImGui::SetNextItemWidth(-1);
            ui::InputText(ap::update_signer_sha256,
                          &_leader_update_signer_pin_input,
                          ImGuiInputTextFlags_ReadOnly);
            ImGui::BeginDisabled(_queued || current.busy);
            if (ui::Button(ap::issue_invitation)) {
                Request request;
                request.kind = Kind::IssueInvitation;
                enqueue(std::move(request));
            }
            ImGui::SameLine();
            if (ui::Button(gm::stop)) {
                Request request;
                request.kind = Kind::StopLeader;
                confirm(std::move(request), ConfirmKind::LeaderStop,
                        gm::stop);
            }
            ImGui::EndDisabled();
        } else {
            ui::TextDisabled(ap::leader_stopped);
            ImGui::BeginDisabled(_queued || current.busy);
            if (ui::Button(ap::leader_start)) {
                if (_leader_operation_port < 0 || _leader_operation_port > 65535 ||
                    _leader_enrollment_port < 0 || _leader_enrollment_port > 65535 ||
                    (_leader_operation_port != 0 &&
                     _leader_operation_port == _leader_enrollment_port)) {
                    _local_error = &ap::invalid_port;
                } else {
                    Request request;
                    request.kind = Kind::StartLeader;
                    request.leader_options.state_root = fs::u8path(_leader_state);
                    request.leader_options.bind_address = _leader_bind;
                    request.leader_options.server_name = _leader_server_name;
                    request.leader_options.operational_port =
                        static_cast<std::uint16_t>(_leader_operation_port);
                    request.leader_options.enrollment_port =
                        static_cast<std::uint16_t>(_leader_enrollment_port);
                    confirm(std::move(request), ConfirmKind::LeaderStart,
                            ap::leader_start);
                }
            }
            ImGui::EndDisabled();
        }
        if (!_issued_invitation.empty()) {
            ui::Text(ap::invitation_code);
            ui::TextRaw(_issued_invitation);
            ui::Text(ap::invitation_expiry,
                     {std::to_string(_issued_expiry)});
            if (ui::Button(ap::clear_secret)) {
                wipe(_issued_invitation);
                _issued_expiry = 0;
            }
        }
        ImGui::PopID();
    }

    void draw_worker_table(const ViewState& current) {
        if (!current.snapshots_available) {
            ui::TextDisabled(ap::worker_strip_unavailable);
            return;
        }
        if (current.workers.empty()) {
            ui::TextDisabled(ap::no_workers);
            return;
        }
        if (ImGui::BeginTable("##workers", 5,
                ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                ImVec2(0, px(176.0f)))) {
            ui::TableSetupColumn(ap::worker_name, ImGuiTableColumnFlags_WidthStretch, 1.5f);
            ui::TableSetupColumn(ap::worker_pairing, ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ui::TableSetupColumn(ap::worker_connection, ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ui::TableSetupColumn(ap::worker_ready, ImGuiTableColumnFlags_WidthFixed, px(70.0f));
            ui::TableSetupColumn(ap::last_seen, ImGuiTableColumnFlags_WidthStretch, 1.3f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            for (const auto& worker : current.workers) {
                ImGui::PushID(worker.id.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const std::string name = worker.label.empty() ? worker.id : worker.label;
                if (ui::SelectableRaw(name.c_str(),
                                      _selected_worker == worker.id,
                                      ImGuiSelectableFlags_SpanAllColumns))
                    _selected_worker = worker.id;
                ImGui::TableNextColumn();
                ui::Text(pairing_label(worker.pairing_status));
                ImGui::TableNextColumn();
                ui::Text(connection_label(worker.status.connection));
                ImGui::TableNextColumn();
                ui::Text(worker.ready ? ap::worker_ready : ap::not_ready);
                ImGui::TableNextColumn();
                const std::string seen = timestamp(worker.last_seen_unix_ms);
                if (seen.empty()) ui::Text(ap::field_unavailable);
                else ui::TextRaw(seen);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    const agent::LeaderServer::WorkerSnapshot* selected_worker(
            const ViewState& current) const {
        for (const auto& worker : current.workers)
            if (worker.id == _selected_worker) return &worker;
        return nullptr;
    }
    std::vector<JobEntry> worker_jobs(const ViewState& current,
                                      const std::string& worker_id) const {
        std::vector<JobEntry> jobs;
        for (const auto& job : current.feature_jobs) {
            if (job.job_id.empty() || job.worker_id != worker_id ||
                job.state == agent::LeaderServer::FeatureJobState::Superseded)
                continue;
            jobs.push_back({job.job_id, &ap::feature, &job_state_label(job.state),
                            job.progress, live_job_state(job.state),
                            job.state == agent::LeaderServer::FeatureJobState::Succeeded});
        }
#ifdef SS_TOOL_SFM
        for (const auto& job : current.reconstruction_jobs) {
            if (job.job_id.empty() || job.worker_id != worker_id || !job.current)
                continue;
            jobs.push_back({job.job_id, &ap::reconstruction,
                            &job_state_label(job.state), std::nullopt,
                            live_job_state(job.state),
                            job.state ==
                                agent::LeaderServer::ReconstructionJobState::Succeeded});
        }
#endif
        for (const auto& job : current.training_jobs) {
            if (job.job_id.empty() || job.worker_id != worker_id || !job.current)
                continue;
            jobs.push_back({job.job_id, &ap::training,
                            &job_state_label(job.state), std::nullopt,
                            live_job_state(job.state),
                            job.state == agent::LeaderServer::TrainingJobState::Succeeded});
        }
        return jobs;
    }

    bool has_pending_command(const ViewState& current,
                             const std::string& worker_id) const {
        return std::any_of(current.command_snapshots.begin(),
                           current.command_snapshots.end(),
            [&](const auto& command) {
                return command.worker_id == worker_id &&
                       command.state == agent::LeaderServer::CommandState::Pending;
            });
    }

    void ask_stop(const agent::LeaderServer::WorkerSnapshot& worker,
                  std::string target_job_id) {
        Request request;
        request.kind = Kind::SendCommand;
        request.worker_id = worker.id;
        request.worker_name = worker.label;
        request.command = agent::wire::CommandAction::Stop;
        request.target_job_id = std::move(target_job_id);
        confirm(std::move(request), ConfirmKind::Command, gm::stop);
    }

    void ask_command(const agent::LeaderServer::WorkerSnapshot& worker,
                     agent::wire::CommandAction action) {
        Request request;
        request.kind = Kind::SendCommand;
        request.worker_id = worker.id;
        request.worker_name = worker.label;
        request.command = action;
        confirm(std::move(request), ConfirmKind::Command,
                command_action_label(action));
    }
    void draw_remote_admin(const ViewState& current,
                           const agent::LeaderServer::WorkerSnapshot& worker,
                           bool paired, bool pending_command,
                           const std::string& worker_name) {
        if (_update_target_worker != worker.id) {
            _update_target_worker = worker.id;
            _update_os = update_os_choice(worker.status.platform);
            _update_architecture = 0;
        }
        ImGui::PushID("remote-admin");
        ui::SeparatorText(ap::remote_admin_section);
        ui::TextWrapped(ap::remote_admin_local_policy_note);
        const int reported_os = update_os_choice(worker.status.platform);
        const bool compatible = paired && worker.connected &&
            worker.last_seen_unix_ms != 0 &&
            worker.status.compatibility ==
                agent::wire::CompatibilityState::Compatible;
        if (!compatible)
            ui::TextWrapped(ap::remote_admin_worker_unavailable);
        if (pending_command) ui::TextDisabled(ap::command_pending);

        ImGui::BeginDisabled(_queued || current.busy);
        ImGui::SetNextItemWidth(-1);
        ui::Combo(ap::update_target_os, &_update_os,
                  {&ap::update_choose_os, &ap::update_os_windows,
                   &ap::update_os_linux, &ap::update_os_macos});
        ImGui::SetNextItemWidth(-1);
        ui::Combo(ap::update_target_architecture, &_update_architecture,
                  {&ap::update_choose_architecture, &ap::update_arch_x86_64,
                   &ap::update_arch_aarch64});
        draw_update_package_field();
        ui::TextWrapped(ap::update_source_note);
        ImGui::SetNextItemWidth(-1);
        ui::InputText(ap::update_build, &_update_build);
        ImGui::SetNextItemWidth(-1);
        ui::InputText(ap::update_release, &_update_release);
        ImGui::SetNextItemWidth(-1);
        ui::InputText(ap::update_security_version, &_update_security_version);
        ImGui::EndDisabled();

        const bool os_matches_worker =
            reported_os != 0 && _update_os == reported_os;
        if (!os_matches_worker)
            ui::TextWrapped(ap::remote_admin_os_mismatch);
        const bool can_request = current.leader_running && compatible &&
            !pending_command && !_queued && !current.busy;
        ImGui::BeginDisabled(!can_request);
        if (ui::Button(ap::request_reboot)) {
            Request request;
            request.kind = Kind::RequestReboot;
            request.worker_id = worker.id;
            request.worker_name = worker_name;
            confirm(std::move(request), ConfirmKind::RequestReboot,
                    ap::request_reboot);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!can_request || !os_matches_worker);
        if (ui::Button(ap::request_update)) {
            std::uint64_t security_version = 0;
            if (_update_package_file.empty() || _update_os == 0 ||
                _update_architecture == 0 || _update_build.empty() ||
                _update_release.empty() ||
                !parse_u64(_update_security_version, security_version) ||
                security_version == 0) {
                _local_error = &ap::remote_admin_fields_required;
            } else {
                Request request;
                request.kind = Kind::RequestUpdate;
                request.worker_id = worker.id;
                request.worker_name = worker_name;
                request.update_package_file = fs::u8path(_update_package_file);
                request.update_manifest.os = update_os_value(_update_os);
                request.update_manifest.architecture =
                    update_architecture_value(_update_architecture);
                request.update_manifest.build = _update_build;
                request.update_manifest.release = _update_release;
                request.update_manifest.size = 0;
                request.update_manifest.sha256.clear();
                request.update_security_version = security_version;
                confirm(std::move(request), ConfirmKind::RequestUpdate,
                        ap::request_update);
            }
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    void draw_worker_details(const ViewState& current) {
        const auto* worker = selected_worker(current);
        if (!worker) return;
        ImGui::PushID(worker->id.c_str());
        ImGui::Separator();
        const std::string name = worker->label.empty() ? worker->id : worker->label;
        field(ap::worker_name, name);
        field(ap::worker_pairing,
              worker->revoked ? ap::revoked.get() : pairing_label(worker->pairing_status).get());
        field(ap::worker_connection, connection_label(worker->status.connection).get());
        const std::string seen = timestamp(worker->last_seen_unix_ms);
        field(ap::last_seen, seen);
        if (!worker->last_seen_unix_ms) {
            field(ap::schedule, ap::field_unavailable.get());
            field(ap::compatibility, ap::field_unavailable.get());
            field(ap::health, ap::field_unavailable.get());
            field(ap::activity, ap::field_unavailable.get());
            field(ap::build, ap::field_unavailable.get());
            field(ap::platform, ap::field_unavailable.get());
            field(ap::capabilities, ap::field_unavailable.get());
            field(ap::reported_gpu, ap::field_unavailable.get());
        } else {
            field(ap::schedule, scheduling_label(worker->status.scheduling).get());
            field(ap::compatibility, compatibility_label(worker->status.compatibility).get());
            field(ap::health, health_label(worker->status.health).get());
            field(ap::activity, activity_label(worker->status.activity).get());
            field(ap::build, worker->status.build);
            field(ap::platform, worker->status.platform);
            const std::string caps = capability_list(worker->status.capabilities);
            field(ap::capabilities, caps);
            field(ap::reported_gpu, worker->status.gpu);
        }
        field(ap::available_gpus, ap::field_unavailable.get());
        const auto jobs = worker_jobs(current, worker->id);
        ui::Text(ap::active_jobs);
        if (jobs.empty()) {
            ui::TextDisabled(ap::no_current_jobs);
        } else {
            for (const JobEntry& job : jobs) {
                ui::Text(ap::job_phase_state,
                         {job.kind->get(), job.job_id, job.state->get()});
                ui::Text(ap::job_progress_eta,
                         {job_progress_text(job.progress), ap::unknown.get()});
                if (job.result_verified)
                    ui::TextWrapped(ap::job_worker_result_verified);
            }
        }
        if (!worker->error.empty()) {
            ui::Text(ap::recent_errors);
            ui::TextWrappedRaw(worker->error);
        }
        const bool pending = worker->pairing_status ==
                             agent::pairing::WorkerStatus::PendingApproval &&
                             !worker->revoked;
        const bool paired = worker->pairing_status ==
                            agent::pairing::WorkerStatus::Paired &&
                            !worker->revoked;
        ImGui::BeginDisabled(_queued || current.busy || !current.leader_running);
        if (pending) {
            if (ui::Button(ap::approve)) {
                Request request;
                request.kind = Kind::Approve;
                request.worker_id = worker->id;
                confirm(std::move(request), ConfirmKind::Approve, ap::approve);
            }
            ImGui::SameLine();
            if (ui::Button(ap::reject)) {
                Request request;
                request.kind = Kind::Reject;
                request.worker_id = worker->id;
                confirm(std::move(request), ConfirmKind::Reject, ap::reject);
            }
        }
        if (paired) {
            if (ui::Button(ap::revoke)) {
                Request request;
                request.kind = Kind::Revoke;
                request.worker_id = worker->id;
                confirm(std::move(request), ConfirmKind::Revoke, ap::revoke);
            }
        }
        const bool pending_command = has_pending_command(current, worker->id);
        if (pending_command) ui::TextDisabled(ap::command_pending);
        const bool commandable = paired && worker->connected &&
                                 worker->last_seen_unix_ms != 0 &&
                                 !pending_command;
        ImGui::BeginDisabled(!commandable);
        if (ui::Button(ap::maintenance))
            ask_command(*worker, agent::wire::CommandAction::Maintenance);
        ImGui::SameLine();
        if (ui::Button(ap::online))
            ask_command(*worker, agent::wire::CommandAction::Online);
        ImGui::SameLine();
        if (ui::Button(gm::pause))
            ask_command(*worker, agent::wire::CommandAction::Pause);
        ImGui::SameLine();
        if (ui::Button(gm::resume))
            ask_command(*worker, agent::wire::CommandAction::Resume);
        const bool has_live_jobs = std::any_of(jobs.begin(), jobs.end(),
            [](const JobEntry& job) { return job.live; });
        if (has_live_jobs) {
            const JobEntry* selected_job = nullptr;
            for (const JobEntry& job : jobs) {
                if (job.live && job.job_id == _selected_stop_job) {
                    selected_job = &job;
                    break;
                }
            }
            if (!selected_job) {
                for (const JobEntry& job : jobs) {
                    if (job.live) {
                        _selected_stop_job = job.job_id;
                        selected_job = &job;
                        break;
                    }
                }
            }
            const std::string preview = ui::format(ap::job_phase_state,
                {selected_job->kind->get(), selected_job->job_id,
                 selected_job->state->get()});
            ImGui::SetNextItemWidth(px(260.0f));
            if (ImGui::BeginCombo("##stop-job-target", preview.c_str())) {
                for (std::size_t i = 0; i < jobs.size(); ++i) {
                    const JobEntry& job = jobs[i];
                    if (!job.live) continue;
                    const std::string label = ui::format(ap::job_phase_state,
                        {job.kind->get(), job.job_id, job.state->get()});
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::Selectable(label.c_str(),
                                          _selected_stop_job == job.job_id))
                        _selected_stop_job = job.job_id;
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            selected_job = nullptr;
            for (const JobEntry& job : jobs) {
                if (job.live && job.job_id == _selected_stop_job) {
                    selected_job = &job;
                    break;
                }
            }
            ImGui::SameLine();
            if (selected_job && ui::Button(ap::stop_selected_job))
                ask_stop(*worker, selected_job->job_id);
            if (ui::Button(ap::stop_all_active_jobs)) ask_stop(*worker, {});
        }
        ui::SeparatorText(ap::worker_restart_section);
        const bool restart_compatible = worker->status.compatibility ==
            agent::wire::CompatibilityState::Compatible;
        ImGui::BeginDisabled(!restart_compatible);
        if (ui::Button(ap::restart_spirula))
            ask_command(*worker, agent::wire::CommandAction::RestartService);
        ImGui::SameLine();
        if (ui::Button(ap::force_restart_spirula))
            ask_command(*worker, agent::wire::CommandAction::ForceRestartService);
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        draw_remote_admin(current, *worker, paired, pending_command, name);
        ImGui::PopID();
    }

    void draw_workers(const ViewState& current) {
        ImGui::PushID("workers");
        ui::SeparatorText(ap::section_workers);
        draw_worker_table(current);
        draw_worker_details(current);
        ImGui::PopID();
    }

    void draw_pairing(const ViewState& current) {
        ImGui::PushID("pairing");
        ui::SeparatorText(ap::pairing_section);
        if (current.pairing_known) {
            field(ap::worker_pairing, pairing_label(current.pairing_status).get());
            if (!current.worker_id.empty()) field(ap::worker_id, current.worker_id);
            if (!current.paired_leader_id.empty())
                field(ap::section_leader, current.paired_leader_id);
            if (current.paired_leader_epoch)
                field(ap::pairing_epoch,
                      std::to_string(current.paired_leader_epoch));
        } else {
            ui::TextDisabled(ap::field_unavailable);
        }
        ui::TextWrapped(ap::pairing_stopped_service);
        ui::TextWrapped(ap::pairing_policy_note);
        const bool can_pair = !_queued && !current.busy &&
            service_stopped(current.service) &&
            current.service.configuration.has_value() &&
            !current.rebind_pending;
        ImGui::BeginDisabled(!can_pair);
        ImGui::SetNextItemWidth(-1);
        ui::InputText(ap::enrollment_address, &_pair_address);
        ImGui::SetNextItemWidth(-1);
        ui::InputText(ap::server_name, &_pair_server_name);
        ImGui::SetNextItemWidth(-1);
        ui::InputInt(ap::enrollment_port, &_pair_port);
        ImGui::SetNextItemWidth(-1);
        ui::InputText(ap::worker_label, &_worker_label);
        ImGui::SetNextItemWidth(-1);
        ui::InputText(ap::invitation_input, &_pair_code,
                      ImGuiInputTextFlags_Password);
        ImGui::SetNextItemWidth(-1);
        ui::InputText(ap::expected_pin, &_pin_text,
                      ImGuiInputTextFlags_Password);
        ui::TextWrapped(ap::pin_note);
        if (ui::Button(ap::refresh_pairing)) {
            Request request;
            request.kind = Kind::RefreshPairing;
            request.service_config = service_configuration();
            enqueue(std::move(request));
        }
        ImGui::SameLine();
        if (ui::Button(ap::redeem)) {
            Fingerprint pin{};
            if (!parse_fingerprint(_pin_text, pin)) {
                _local_error = &ap::pairing_input_required;
            } else if (_pair_port < 1 || _pair_port > 65535) {
                wipe(pin);
                _local_error = &ap::invalid_port;
            } else if (_pair_address.empty() || _pair_server_name.empty() ||
                       _worker_label.empty() || _pair_code.empty()) {
                wipe(pin);
                _local_error = &ap::pairing_input_required;
            } else {
                Request request;
                request.kind = Kind::Redeem;
                request.service_config = service_configuration();
                request.address = _pair_address;
                request.server_name = _pair_server_name;
                request.port = static_cast<std::uint16_t>(_pair_port);
                request.worker_name = _worker_label;
                request.code = _pair_code;
                request.pin = pin;
                confirm(std::move(request), ConfirmKind::Redeem, ap::redeem);
            }
        }
        ImGui::SameLine();
        if (ui::Button(ap::check_approval)) {
            Fingerprint pin{};
            if (!parse_fingerprint(_pin_text, pin)) {
                _local_error = &ap::pairing_input_required;
            } else if (_pair_port < 1 || _pair_port > 65535) {
                wipe(pin);
                _local_error = &ap::invalid_port;
            } else if (_pair_address.empty() || _pair_server_name.empty() ||
                       _pair_code.empty()) {
                wipe(pin);
                _local_error = &ap::pairing_input_required;
            } else {
                Request request;
                request.kind = Kind::CheckApproval;
                request.service_config = service_configuration();
                request.address = _pair_address;
                request.server_name = _pair_server_name;
                request.port = static_cast<std::uint16_t>(_pair_port);
                request.code = _pair_code;
                request.pin = pin;
                confirm(std::move(request), ConfirmKind::CheckApproval,
                        ap::check_approval);
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool forget_enabled = current.pairing_known &&
            current.pairing_status != agent::pairing::WorkerStatus::Unpaired &&
            !_queued && !current.busy &&
            service_stopped(current.service) &&
            current.service.configuration.has_value();
        ImGui::BeginDisabled(!forget_enabled);
        if (ui::Button(ap::forget_pairing)) {
            Request request;
            request.kind = Kind::Forget;
            request.service_config = service_configuration();
            confirm(std::move(request), ConfirmKind::Forget, ap::forget_pairing);
        }
        ImGui::EndDisabled();
        if (current.rebind_pending) {
            ui::TextWrapped(ap::rebind_pending_note);
            if (!current.rebind_error.empty())
                ui::TextWrappedRaw(current.rebind_error);
            ImGui::BeginDisabled(_queued || current.busy ||
                                 !service_stopped(current.service));
            if (ui::Button(ap::retry_rebind)) {
                Request request;
                request.kind = Kind::RetryRebind;
                confirm(std::move(request), ConfirmKind::RetryRebind,
                        ap::retry_rebind);
            }
            ImGui::EndDisabled();
        }
        ImGui::PopID();
    }

    void draw_service(const ViewState& current) {
        ImGui::PushID("service");
        ui::SeparatorText(ap::section_service);
        const bool uninstalled = current.service.state ==
                                 agent::service::State::NotInstalled;
        const bool installed = current.service.state !=
                                   agent::service::State::NotInstalled &&
                               current.service.state != agent::service::State::Unknown;
        const bool stopped = service_stopped(current.service);
        field(ap::service_status, service_label(current.service.state).get());
        field(ap::startup, installed
              ? (current.service.startup_enabled ? ap::startup_enabled.get()
                                                  : ap::startup_disabled.get())
              : ap::field_unavailable.get());
        if (!current.service.error.empty())
            ui::TextWrappedRaw(current.service.error);
        if (!current.service_query_error.empty())
            ui::TextWrappedRaw(current.service_query_error);
        const bool paths_editable = !_queued && !current.busy && uninstalled;
        field(ap::executable, _service_executable);
        draw_path_field(ap::config_root, _config_root, PathTarget::ConfigRoot,
                        paths_editable);
        draw_path_field(ap::state_root, _worker_state_root, PathTarget::StateRoot,
                        paths_editable);
        draw_path_field(ap::storage_root, _storage_root, PathTarget::StorageRoot,
                        paths_editable);
        ui::TextWrapped(ap::roots_note);
        ui::SeparatorText(ap::permissions_section);
        ui::TextWrapped(ap::permissions_note);

        ImGui::BeginDisabled(_queued || current.busy);
        if (uninstalled) {
            if (ui::Button(ap::service_install)) {
                Request request;
                request.kind = Kind::ServiceInstall;
                request.service_config = service_configuration();
                confirm(std::move(request), ConfirmKind::ServiceInstall,
                        ap::service_install);
            }
        }
        if (installed) {
            if (!uninstalled) ImGui::SameLine();
            ImGui::BeginDisabled(!stopped || current.rebind_pending);
            if (ui::Button(ap::service_uninstall)) {
                Request request;
                request.kind = Kind::ServiceUninstall;
                request.service_config = service_configuration();
                confirm(std::move(request), ConfirmKind::ServiceUninstall,
                        ap::service_uninstall);
            }
            ImGui::EndDisabled();
        }
        if (installed && stopped) {
            ImGui::SameLine();
            ImGui::BeginDisabled(current.rebind_pending);
            if (ui::Button(ap::service_start)) {
                Request request;
                request.kind = Kind::ServiceStart;
                request.service_config = service_configuration();
                confirm(std::move(request), ConfirmKind::ServiceStart,
                        ap::service_start);
            }
            ImGui::EndDisabled();
        }
        if (current.service.state == agent::service::State::Running ||
            current.service.state == agent::service::State::Paused) {
            ImGui::SameLine();
            if (ui::Button(ap::service_stop)) {
                Request request;
                request.kind = Kind::ServiceStop;
                confirm(std::move(request), ConfirmKind::ServiceStop,
                        ap::service_stop);
            }
        }
        if (installed) {
            ImGui::SameLine();
            ImGui::BeginDisabled(current.service.startup_enabled ||
                                 current.rebind_pending || !stopped);
            if (ui::Button(ap::startup_enable)) {
                Request request;
                request.kind = Kind::StartupEnable;
                request.service_config = service_configuration();
                confirm(std::move(request), ConfirmKind::StartupEnable,
                        ap::startup_enable);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!current.service.startup_enabled);
            if (ui::Button(ap::startup_disable)) {
                Request request;
                request.kind = Kind::StartupDisable;
                confirm(std::move(request), ConfirmKind::StartupDisable,
                        ap::startup_disable);
            }
            ImGui::EndDisabled();
        }
        ImGui::EndDisabled();
        draw_admin_broker(current);
        ImGui::PopID();
    }
    void draw_admin_broker(const ViewState& current) {
        ImGui::PushID("admin-broker");
        ui::SeparatorText(ap::admin_broker_section);
        ui::TextWrapped(ap::admin_broker_note);
#ifndef _WIN32
        (void)current;
        ui::TextWrapped(ap::admin_broker_windows_only);
#else
        const bool policy_current = current.policy_loaded &&
            current.policy_error.empty() &&
            current.policy_root == fs::u8path(_config_root);
        const bool binding_ready = current.pairing_known &&
            current.pairing_status == agent::pairing::WorkerStatus::Paired &&
            !current.worker_id.empty() && !current.paired_leader_id.empty() &&
            current.paired_leader_epoch != 0;
        const bool signer_pin_ready = policy_current &&
            !current.policy.update_signer_sha256.empty() &&
            _policy.update_signer_sha256 ==
                current.policy.update_signer_sha256;
        const bool ready = current.service.state == agent::service::State::Stopped &&
            current.service.configuration.has_value() &&
            current.service.error.empty() &&
            current.service_query_error.empty() && binding_ready &&
            signer_pin_ready && !current.rebind_pending &&
            !_queued && !current.busy;
        if (!ready) ui::TextWrapped(ap::admin_broker_prerequisites);
        ImGui::BeginDisabled(_queued || current.busy);
        ImGui::SetNextItemWidth(-1);
        ui::InputText(ap::admin_broker_initial_security_version,
                      &_admin_initial_security_version);
        ImGui::EndDisabled();
        ui::TextWrapped(ap::admin_broker_security_version_note);
        std::uint64_t initial_security_version = 0;
        const bool initial_security_version_valid =
            parse_u64(_admin_initial_security_version,
                      initial_security_version) && initial_security_version > 0;
        ImGui::BeginDisabled(!ready);
        ImGui::BeginDisabled(!initial_security_version_valid);
        if (ui::Button(ap::admin_broker_install)) {
            Request request;
            request.kind = Kind::InstallAdminBroker;
            request.worker_name = ap::section_service.get();
            request.admin_target = {current.worker_id, current.paired_leader_id,
                                    current.paired_leader_epoch,
                                    initial_security_version};
            confirm(std::move(request), ConfirmKind::InstallAdminBroker,
                    ap::admin_broker_install);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ui::Button(ap::admin_broker_uninstall)) {
            Request request;
            request.kind = Kind::UninstallAdminBroker;
            confirm(std::move(request), ConfirmKind::UninstallAdminBroker,
                    ap::admin_broker_uninstall);
        }
        ImGui::EndDisabled();
#endif
        ImGui::PopID();
    }

    void sync_policy_from_state(const ViewState& current) {
        if (current.policy_revision == _seen_policy_revision) return;
        _seen_policy_revision = current.policy_revision;
        _policy = current.policy;
        _policy_address = current.policy.leader_address;
        _policy_server_name = current.policy.leader_server_name;
        _policy_port = current.policy.leader_port;
        _allowed_gpu_text = join_lines(current.policy.allowed_vulkan_uuids);
        _max_jobs = static_cast<int>(std::min<std::uint32_t>(
            current.policy.max_concurrent_jobs,
            static_cast<std::uint32_t>(std::numeric_limits<int>::max())));
        _disk_budget_text = std::to_string(current.policy.disk_budget_bytes);
        _allow_update = current.policy.allow_remote_update;
        _allow_reboot = current.policy.allow_reboot;
        _policy_dirty = false;
    }

    void draw_policy(const ViewState& current) {
        sync_policy_from_state(current);
        ImGui::PushID("policy");
        ui::SeparatorText(ap::section_policy);
        const bool policy_current = current.policy_loaded &&
            current.policy_root == fs::u8path(_config_root);
        if (!policy_current)
            ui::TextWrapped(ap::policy_not_loaded);
        if (!current.policy_error.empty())
            ui::TextWrappedRaw(current.policy_error);
        if (current.rebind_pending || !service_stopped(current.service))
            ui::TextWrapped(ap::policy_stopped_service);
        const bool can_edit = !_queued && !current.busy &&
                              service_stopped(current.service) &&
                              !current.rebind_pending &&
                              (!current.service.configuration || policy_current);
        ImGui::BeginDisabled(!can_edit);
        draw_path_field(ap::config_root, _config_root, PathTarget::ConfigRoot,
                        can_edit && !current.service.configuration);
        ImGui::SetNextItemWidth(-1);
        if (ui::InputText(ap::leader_address, &_policy_address)) _policy_dirty = true;
        ImGui::SetNextItemWidth(-1);
        if (ui::InputText(ap::server_name, &_policy_server_name)) _policy_dirty = true;
        ImGui::SetNextItemWidth(-1);
        if (ui::InputInt(ap::operational_port, &_policy_port)) _policy_dirty = true;
        ImGui::SetNextItemWidth(-1);
        if (ui::InputTextMultilineRaw("##allowed-uuids", &_allowed_gpu_text,
                                      ImVec2(-1, px(74.0f))))
            _policy_dirty = true;
        ui::Text(ap::allowed_gpus);
        ImGui::SetNextItemWidth(-1);
        if (ui::InputInt(ap::max_jobs, &_max_jobs)) _policy_dirty = true;
        ImGui::SetNextItemWidth(-1);
        if (ui::InputText(ap::disk_budget, &_disk_budget_text)) _policy_dirty = true;
        if (ui::Checkbox(ap::allow_remote_update, &_allow_update)) _policy_dirty = true;
        if (ui::Checkbox(ap::allow_reboot, &_allow_reboot)) _policy_dirty = true;
        ImGui::SetNextItemWidth(-1);
        if (ui::InputText(ap::update_signer_sha256,
                          &_policy.update_signer_sha256))
            _policy_dirty = true;
        ui::TextWrapped(ap::update_signer_pin_note);
        ImGui::EndDisabled();
        ImGui::BeginDisabled(_queued || current.busy);
        if (ui::Button(ap::policy_load)) {
            Request request;
            request.kind = Kind::LoadPolicy;
            request.config_root = fs::u8path(_config_root);
            enqueue(std::move(request));
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!_policy_dirty || !can_edit);
        if (ui::Button(ap::policy_save)) {
            if (_policy_port < 1 || _policy_port > 65535 || _max_jobs < 1) {
                _local_error = &ap::policy_invalid;
            } else {
                std::uint64_t budget = 0;
                if (!parse_u64(_disk_budget_text, budget) || budget == 0) {
                    _local_error = &ap::policy_invalid;
                } else {
                    Request request;
                    request.kind = Kind::SavePolicy;
                    request.config_root = fs::u8path(_config_root);
                    request.policy = _policy;
                    request.policy.leader_address = _policy_address;
                    request.policy.leader_server_name = _policy_server_name;
                    request.policy.leader_port =
                        static_cast<std::uint16_t>(_policy_port);
                    request.policy.allowed_vulkan_uuids =
                        parse_lines(_allowed_gpu_text);
                    request.policy.max_concurrent_jobs =
                        static_cast<std::uint32_t>(_max_jobs);
                    request.policy.disk_budget_bytes = budget;
                    request.policy.allow_remote_update = _allow_update;
                    request.policy.allow_reboot = _allow_reboot;
                    confirm(std::move(request), ConfirmKind::PolicySave,
                            ap::policy_save);
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    const Msg& confirm_label(ConfirmKind kind,
                             const Request* request) const {
        switch (kind) {
            case ConfirmKind::LeaderStart: return ap::leader_start;
            case ConfirmKind::LeaderStop: return gm::stop;
            case ConfirmKind::Approve: return ap::approve;
            case ConfirmKind::Reject: return ap::reject;
            case ConfirmKind::Revoke: return ap::revoke;
            case ConfirmKind::Command:
                return request ? command_action_label(request->command) : ap::unknown;
            case ConfirmKind::RequestReboot: return ap::request_reboot;
            case ConfirmKind::RequestUpdate: return ap::request_update;
            case ConfirmKind::InstallAdminBroker: return ap::admin_broker_install;
            case ConfirmKind::UninstallAdminBroker:
                return ap::admin_broker_uninstall;
            case ConfirmKind::Redeem: return ap::redeem;
            case ConfirmKind::CheckApproval: return ap::check_approval;
            case ConfirmKind::Forget: return ap::forget_pairing;
            case ConfirmKind::RetryRebind: return ap::retry_rebind;
            case ConfirmKind::PolicySave: return ap::policy_save;
            case ConfirmKind::ServiceInstall: return ap::service_install;
            case ConfirmKind::ServiceUninstall: return ap::service_uninstall;
            case ConfirmKind::ServiceStart: return ap::service_start;
            case ConfirmKind::ServiceStop: return ap::service_stop;
            case ConfirmKind::StartupEnable: return ap::startup_enable;
            case ConfirmKind::StartupDisable: return ap::startup_disable;
            case ConfirmKind::None: break;
        }
        return ap::unknown;
    }

    void draw_confirmation() {
        if (_confirm_open) {
            ui::OpenPopup(ap::confirm_title);
            _confirm_open = false;
        }
        const char* popup_id = ui::detail::label(ap::confirm_title);
        if (!ui::BeginPopupModal(ap::confirm_title, nullptr,
                                 ImGuiWindowFlags_AlwaysAutoResize)) {
            if (_pending_request && !ImGui::IsPopupOpen(popup_id)) {
                wipe_request(*_pending_request);
                _pending_request.reset();
                _confirm_kind = ConfirmKind::None;
            }
            return;
        }
        if (!_pending_request) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        const Msg& action = confirm_label(_confirm_kind,
                                          _pending_request ? &*_pending_request : nullptr);
        ui::Text(ap::confirm_action, {action.get()});
        if (_confirm_kind == ConfirmKind::Command &&
            _pending_request->command == agent::wire::CommandAction::Stop) {
            const std::string worker_name = _pending_request->worker_name.empty()
                ? _pending_request->worker_id : _pending_request->worker_name;
            ui::Text(ap::command_confirmation_target,
                {worker_name,
                 command_target_text(_pending_request->command,
                                     _pending_request->target_job_id)});
        }
        if (_confirm_kind == ConfirmKind::Command &&
            (_pending_request->command ==
                 agent::wire::CommandAction::RestartService ||
             _pending_request->command ==
                 agent::wire::CommandAction::ForceRestartService)) {
            const std::string worker_name = _pending_request->worker_name.empty()
                ? _pending_request->worker_id : _pending_request->worker_name;
            ui::Text(ap::remote_admin_confirmation_target,
                     {worker_name, _pending_request->worker_id});
            if (_pending_request->command ==
                agent::wire::CommandAction::RestartService)
                ui::TextWrapped(ap::restart_spirula_confirmation);
            else
                ui::TextWrapped(ap::force_restart_spirula_confirmation);
        }
        if (_confirm_kind == ConfirmKind::RequestReboot ||
            _confirm_kind == ConfirmKind::RequestUpdate ||
            _confirm_kind == ConfirmKind::InstallAdminBroker) {
            const std::string worker_name = _pending_request->worker_name.empty()
                ? _pending_request->worker_id : _pending_request->worker_name;
            const std::string worker_id = _pending_request->worker_id.empty()
                ? _pending_request->admin_target.worker_id
                : _pending_request->worker_id;
            ui::Text(ap::remote_admin_confirmation_target,
                     {worker_name, worker_id});
        }
        if (_confirm_kind == ConfirmKind::RequestReboot)
            ui::TextWrapped(ap::remote_admin_reboot_confirmation);
        if (_confirm_kind == ConfirmKind::RequestUpdate) {
            ui::TextWrapped(ap::remote_admin_update_confirmation);
            ui::TextWrapped(ap::remote_admin_package_confirmation,
                            {path_text(_pending_request->update_package_file)});
            ui::TextWrapped(ap::remote_admin_update_metadata_confirmation,
                {_pending_request->update_manifest.os,
                 _pending_request->update_manifest.architecture,
                 _pending_request->update_manifest.build,
                 _pending_request->update_manifest.release,
                 std::to_string(_pending_request->update_security_version)});
        }
        if (_confirm_kind == ConfirmKind::InstallAdminBroker) {
            ui::TextWrapped(ap::admin_broker_install_confirmation);
            ui::TextWrapped(ap::admin_broker_binding_confirmation,
                {_pending_request->admin_target.worker_id,
                 _pending_request->admin_target.leader_id,
                 std::to_string(_pending_request->admin_target.leader_epoch)});
            ui::TextWrapped(ap::admin_broker_security_version_confirmation,
                {std::to_string(
                    _pending_request->admin_target.initial_security_version)});
        }
        if (_confirm_kind == ConfirmKind::UninstallAdminBroker)
            ui::TextWrapped(ap::admin_broker_uninstall_confirmation);
        if (_confirm_kind == ConfirmKind::LeaderStart && _pending_request) {
            ui::Text(ap::bind_address);
            ImGui::SameLine();
            ui::TextRaw(_pending_request->leader_options.bind_address);
            ui::Text(ap::operational_port);
            ImGui::SameLine();
            ui::TextRaw(std::to_string(
                _pending_request->leader_options.operational_port));
            ui::Text(ap::enrollment_port);
            ImGui::SameLine();
            ui::TextRaw(std::to_string(
                _pending_request->leader_options.enrollment_port));
        }
        ImGui::BeginDisabled(_queued);
        if (ui::Button(ap::confirm)) {
            queue_confirmed();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ui::Button(gm::cancel)) cancel_confirmation();
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }

    void draw_commands(const ViewState& current) {
        ui::SeparatorText(ap::recent_commands);
        if (current.commands.empty() && current.command_snapshots.empty()) {
            ui::TextDisabled(ap::field_unavailable);
            return;
        }
        for (const CommandEntry& command : current.commands) {
            const std::string name = command.worker_name.empty()
                ? command.worker_id : command.worker_name;
            ui::Text(ap::command_error_context,
                {name, command_action_label(command.action).get(),
                 command_target_text(command.action, command.target_job_id)});
            if (!command.error.empty())
                ui::TextWrapped(ap::command_error, {command.error});
        }
        const std::size_t history_count =
            std::min<std::size_t>(24, current.command_snapshots.size());
        for (std::size_t i = 0; i < history_count; ++i) {
            const auto& command = current.command_snapshots[i];
            std::string name = command.worker_id;
            for (const auto& worker : current.workers) {
                if (worker.id == command.worker_id && !worker.label.empty()) {
                    name = worker.label;
                    break;
                }
            }
            const bool accepted_pending =
                command.state == agent::LeaderServer::CommandState::Pending &&
                command.acknowledgment_received &&
                command.acknowledgment ==
                    agent::wire::AcknowledgmentOutcome::Accepted;
            const Msg& status = accepted_pending
                ? ap::command_pending_accepted : command_state_label(command.state);
            ui::Text(ap::command_outcome,
                {name, command_action_label(command.action).get(), status.get(),
                 command_target_text(command.action, command.target_job_id),
                 std::to_string(command.sequence)});
            if (command.acknowledgment_received && !accepted_pending)
                ui::Text(ap::command_acknowledgment,
                         {acknowledgment_label(command.acknowledgment).get()});
        }
    }

    void draw_errors(const ViewState& current) {
        ui::SeparatorText(ap::recent_errors);
        if (current.errors.empty()) {
            ui::TextDisabled(ap::field_unavailable);
            return;
        }
        for (const ErrorEntry& entry : current.errors) {
            if (!entry.worker_name.empty()) {
                ui::TextRaw(entry.worker_name);
                ImGui::SameLine();
            }
            ui::TextWrappedRaw(entry.error);
        }
    }

    void draw() {
        drain_events();
        const auto current = view();
        if (current->service.configuration) {
            const auto& installed = *current->service.configuration;
            const std::string config_root = path_text(installed.config_root);
            if (_config_root != config_root) _policy_dirty = false;
            _service_executable = path_text(installed.executable);
            _config_root = config_root;
            _worker_state_root = path_text(installed.state_root);
            _storage_root = path_text(installed.storage_root);
            if (_policy_root_loaded != config_root && !_queued) {
                Request request;
                request.kind = Kind::LoadPolicy;
                request.config_root = installed.config_root;
                if (enqueue(std::move(request))) _policy_root_loaded = config_root;
            }
        }
        if (_issued_expiry && static_cast<std::uint64_t>(std::time(nullptr)) >=
                                  _issued_expiry) {
            wipe(_issued_invitation);
            _issued_expiry = 0;
        }
        if (_was_open && !_open) {
            wipe(_pair_code);
            wipe(_pin_text);
            wipe(_issued_invitation);
            _issued_expiry = 0;
            if (_pending_request) wipe_request(*_pending_request);
            _pending_request.reset();
            _confirm_kind = ConfirmKind::None;
            _confirm_open = false;
        }
        _was_open = _open;
        if (_open) {
            ImGui::SetNextWindowSize(ImVec2(px(900.0f), px(680.0f)),
                                     ImGuiCond_FirstUseEver);
            if (ImGui::Begin(ui::detail::label(ap::window_title), &_open)) {
                if (current->busy || _queued)
                    ui::TextDisabled(ap::operation_busy);
                if (_local_error) {
                    ui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), *_local_error);
                    _local_error = nullptr;
                }
                draw_operation_result(*current);
                if (ImGui::BeginChild("##agent-panel-scroll", ImVec2(0, 0),
                                      ImGuiChildFlags_Borders)) {
                    draw_leader(*current);
                    draw_workers(*current);
                    draw_commands(*current);
                    draw_errors(*current);
                    draw_pairing(*current);
                    draw_service(*current);
                    draw_policy(*current);
                }
                ImGui::EndChild();
                draw_confirmation();
            }
            ImGui::End();
        }
        draw_path_dialog();
    }

    void draw_status_strip() {
        const auto current = view();
        const ImVec2 size(ImGui::GetContentRegionAvail().x,
                          ImGui::GetContentRegionAvail().y);
        if (size.x <= 0 || size.y <= 0) return;
        if (ui::InvisibleButtonRaw("##worker-management-strip", size))
            _open = true;
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        std::string text;
        if (current->snapshots_available) {
            std::size_t ready = 0;
            std::size_t working = 0;
            std::size_t attention = 0;
            for (const auto& worker : current->workers) {
                ready += worker.ready ? 1u : 0u;
                const bool activity_working =
                    worker.status.activity == agent::wire::ActivityState::Feature ||
                    worker.status.activity ==
                        agent::wire::ActivityState::Reconstruction ||
                    worker.status.activity == agent::wire::ActivityState::Training;
                working += worker.connected && worker.last_seen_unix_ms &&
                           activity_working ? 1u : 0u;
                attention += worker.revoked ||
                    worker.pairing_status != agent::pairing::WorkerStatus::Paired ||
                    !worker.connected ||
                    worker.status.compatibility !=
                        agent::wire::CompatibilityState::Compatible ||
                    worker.status.health != agent::wire::HealthState::Healthy ||
                    !worker.error.empty() ? 1u : 0u;
            }
            text = ui::format(ap::worker_strip_summary,
                {static_cast<int>(current->workers.size()),
                 static_cast<int>(ready), static_cast<int>(working),
                 static_cast<int>(attention)});
        } else {
            text = ap::worker_strip_unavailable.get();
        }
        const ImVec2 at = ImGui::GetItemRectMin();
        const ImVec2 pad = ImGui::GetStyle().FramePadding;
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(at.x + pad.x, at.y + pad.y),
            ImGui::GetColorU32(ImGuiCol_Text), text.c_str());
    }

    void open() { _open = true; }


    void shutdown() {
        if (_shutdown) return;
        _shutdown = true;
        {
            std::lock_guard<std::mutex> lock(_queue_mutex);
            _closing = true;
            for (Request& request : _queue) wipe_request(request);
            _queue.clear();
        }
        _queue_cv.notify_all();
        if (_thread.joinable()) _thread.join();
        {
            std::lock_guard<std::mutex> lock(_event_mutex);
            for (Event& event : _events) wipe(event.invitation);
            _events.clear();
        }
        wipe(_issued_invitation);
        wipe(_pair_code);
        wipe(_pin_text);
        if (_pending_request) wipe_request(*_pending_request);
        _pending_request.reset();
    }

    std::string _service_executable;
    std::string _config_root;
    std::string _worker_state_root;
    std::string _storage_root;
    std::string _leader_state;
    std::string _leader_bind;
    std::string _leader_update_signer_pin_input;
    std::string _leader_server_name;
    std::string _pair_address;
    std::string _pair_server_name;
    std::string _worker_label = "Spirula worker";
    std::string _pair_code;
    std::string _pin_text;
    std::string _issued_invitation;
    std::uint64_t _issued_expiry = 0;
    std::string _policy_address;
    std::string _policy_server_name;
    std::string _allowed_gpu_text;
    std::string _disk_budget_text;
    std::string _update_package_file;
    std::string _update_build;
    std::string _update_release;
    std::string _update_security_version;
    std::string _admin_initial_security_version;
    std::string _update_target_worker;
    agent::Config _policy;
    int _leader_operation_port = 0;
    int _leader_enrollment_port = 0;
    int _pair_port = 0;
    int _policy_port = 0;
    int _update_os = 0;
    int _update_architecture = 0;
    int _max_jobs = 1;
    bool _allow_update = false;
    bool _allow_reboot = false;
    bool _policy_dirty = false;
    std::uint64_t _seen_policy_revision = 0;
    std::string _policy_root_loaded;
    std::string _selected_worker;
    std::string _selected_stop_job;
    const Msg* _local_error = nullptr;
    bool _open = false;
    bool _was_open = false;
    bool _queued = false;
    bool _confirm_open = false;
    bool _shutdown = false;
    ConfirmKind _confirm_kind = ConfirmKind::None;
    const Msg* _confirm_label = nullptr;
    std::optional<Request> _pending_request;
    FileDialog _path_dialog;
    PathTarget _path_target = PathTarget::LeaderState;
    ViewState state;
    std::shared_ptr<const ViewState> _published;
    std::unique_ptr<agent::LeaderServer> _leader;
    std::optional<PendingRebind> _pending_rebind;
    std::map<std::string, std::string> _last_errors;
    std::mutex _queue_mutex;
    std::condition_variable _queue_cv;
    std::deque<Request> _queue;
    bool _closing = false;
    std::thread _thread;
    std::mutex _event_mutex;
    std::deque<Event> _events;
};

AgentPanel::AgentPanel() : _impl(std::make_unique<Impl>()) {}
AgentPanel::~AgentPanel() = default;
void AgentPanel::open() { _impl->open(); }
void AgentPanel::draw() { _impl->draw(); }
void AgentPanel::draw_status_strip() { _impl->draw_status_strip(); }
void AgentPanel::shutdown() { if (_impl) _impl->shutdown(); }

bool AgentPanel::leader_running() const {
    return _impl->view()->leader_running;
}

std::vector<agent::LeaderServer::WorkerSnapshot> AgentPanel::workers() const {
    return _impl->view()->workers;
}

std::vector<agent::LeaderServer::FeatureJobSnapshot>
AgentPanel::feature_jobs() const {
    return _impl->view()->feature_jobs;
}

std::string AgentPanel::feature_error(const std::string& job_id) const {
    const auto current = _impl->view();
    const auto found = current->feature_errors.find(job_id);
    return found == current->feature_errors.end() ? std::string{} : found->second;
}

bool AgentPanel::submit_feature_shard(FeatureShardSubmission submission) {
    if (!_impl->view()->leader_running) return false;
    Impl::Request request;
    request.kind = Impl::Kind::SubmitFeatureShard;
    request.feature = std::move(submission);
    return _impl->enqueue_feature(std::move(request));
}

bool AgentPanel::supersede_feature_shard(const std::string& job_id) {
    if (!_impl->view()->leader_running) return false;
    Impl::Request request;
    request.kind = Impl::Kind::SupersedeFeatureShard;
    request.feature.job_id = job_id;
    return _impl->enqueue_feature(std::move(request));
}

#ifdef SS_TOOL_SFM
std::vector<agent::LeaderServer::ReconstructionJobSnapshot>
AgentPanel::reconstruction_jobs() const {
    return _impl->view()->reconstruction_jobs;
}

std::string AgentPanel::reconstruction_error(const std::string& job_id) const {
    const auto current = _impl->view();
    const auto found = current->reconstruction_errors.find(job_id);
    return found == current->reconstruction_errors.end() ? std::string{}
                                                         : found->second;
}

bool AgentPanel::submit_reconstruction(ReconstructionSubmission submission) {
    if (!_impl->view()->leader_running) return false;
    Impl::Request request;
    request.kind = Impl::Kind::SubmitReconstruction;
    request.reconstruction = std::move(submission);
    return _impl->enqueue_feature(std::move(request));
}

bool AgentPanel::supersede_reconstruction(const std::string& job_id) {
    if (!_impl->view()->leader_running) return false;
    Impl::Request request;
    request.kind = Impl::Kind::SupersedeReconstruction;
    request.reconstruction.job_id = job_id;
    return _impl->enqueue_feature(std::move(request));
}
#endif
std::vector<agent::LeaderServer::TrainingJobSnapshot>
AgentPanel::training_jobs() const {
    return _impl->view()->training_jobs;
}

std::string AgentPanel::training_error(const std::string& job_id) const {
    const auto current = _impl->view();
    const auto found = current->training_errors.find(job_id);
    return found == current->training_errors.end() ? std::string{}
                                                    : found->second;
}

bool AgentPanel::submit_training(TrainingSubmission submission) {
    if (!_impl->view()->leader_running) return false;
    Impl::Request request;
    request.kind = Impl::Kind::SubmitTraining;
    request.training = std::move(submission);
    return _impl->enqueue_feature(std::move(request));
}

bool AgentPanel::supersede_training(const std::string& job_id) {
    if (!_impl->view()->leader_running) return false;
    Impl::Request request;
    request.kind = Impl::Kind::SupersedeTraining;
    request.training.job_id = job_id;
    return _impl->enqueue_feature(std::move(request));
}

}  // namespace gui
