#include "app/AgentLeader.h"
#include "app/AgentTransfer.h"
#include "core/Sha256.h"

#include "core/FilesystemPath.h"
#ifdef SS_TOOL_SFM
#include "app/AgentReconstructionJob.h"
#include "app/AgentFeatureJob.h"
#include "sfm/Pipeline.h"
#include "sfm/core/Model.h"
#include "sfm/core/Features.h"
#include "sfm/core/FeatureWork.h"
#endif

#ifdef SS_TOOL_TRAIN
#include "app/AgentTrainingJob.h"
#include "app/tests/SceneFixture.h"
#include "checkpoint/Resume.h"
#include "config/TrainConfigJson.h"
#include "core/CheckpointIO.h"
#include "data/JsonWrite.h"
#endif
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <mutex>
#include <optional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <condition_variable>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace agent = app::agent;
namespace pairing = app::agent::pairing;
namespace wire = app::agent::wire;
using Socket = agent::TlsChannel::NativeSocket;
using LeaderServer = agent::LeaderServer;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
    if (!condition) ++failures;
    std::fflush(stdout);
}

bool write_native_file(const fs::path& path, const char* bytes, std::size_t size) {
    const auto io_path = spirula::NativeFilesystemPath(path);
    fs::create_directories(io_path.parent_path());
#ifdef _WIN32
    FILE* output = ::_wfopen(io_path.c_str(), L"wb");
    if (!output) return false;
    const bool wrote = size == 0 || std::fwrite(bytes, 1, size, output) == size;
    const bool closed = std::fclose(output) == 0;
    return wrote && closed;
#else
    std::ofstream output(io_path, std::ios::binary | std::ios::trunc);
    output.write(bytes, static_cast<std::streamsize>(size));
    return static_cast<bool>(output);
#endif
}

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

#ifdef _WIN32
using RawSocket = SOCKET;
constexpr RawSocket kBadSocket = INVALID_SOCKET;
void close_socket(RawSocket socket) { if (socket != kBadSocket) closesocket(socket); }
#else
using RawSocket = int;
constexpr RawSocket kBadSocket = -1;
void close_socket(RawSocket socket) { if (socket != kBadSocket) ::close(socket); }
#endif

Socket native_socket(RawSocket socket) {
    return static_cast<Socket>(socket);
}

RawSocket connect_loopback(std::uint16_t port) {
    RawSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kBadSocket) return kBadSocket;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        close_socket(socket);
        return kBadSocket;
    }
    return socket;
}

bool send_wire(agent::TlsChannel& channel, const wire::Message& message) {
    std::string json;
    if (wire::Encode(message, now_ms(), json) != wire::Error::None) return false;
    return channel.SendFrame(reinterpret_cast<const std::uint8_t*>(json.data()), json.size());
}

bool receive_wire(agent::TlsChannel& channel, wire::Message& message) {
    std::vector<std::uint8_t> bytes;
    if (!channel.ReceiveFrame(bytes)) return false;
    return wire::Decode(std::string(bytes.begin(), bytes.end()), now_ms(), message) ==
           wire::Error::None;
}

wire::Status worker_status() {
    wire::Status status;
    status.connection = wire::ConnectionState::Connected;
    status.compatibility = wire::CompatibilityState::Unknown;
    status.scheduling = wire::SchedulingState::Stopped;
    status.activity = wire::ActivityState::Idle;
    status.health = wire::HealthState::Healthy;
    status.build = "leader-test";
#ifdef _WIN32
    status.platform = "windows";
#elif defined(__APPLE__)
    status.platform = "macos";
#else
    status.platform = "linux";
#endif
    status.gpu = "loopback";
    return status;
}


struct FakeWorkerMemory final {
    mutable std::mutex mutex;
    std::uint64_t last_sequence = 0;
    wire::Command last_command;
    wire::AcknowledgmentOutcome last_outcome =
        wire::AcknowledgmentOutcome::Failed;
    bool has_command = false;
    bool pending = false;
    bool completion_requested = false;
    std::optional<wire::Acknowledgment> replay_ack;
    std::string expected_worker_id;
    std::string expected_package_bytes;
    fs::path admin_transfer_root;
    bool admin_update_verified = false;
    std::string admin_received_bytes;
    std::string failure_reason;
    std::atomic<int> effects{0};
    std::atomic<int> deliveries{0};
    std::atomic<int> heartbeat_cycles{0};
    std::atomic<bool> paused{false};
    std::atomic<bool> maintenance{false};
    std::atomic<bool> failed{false};
    std::atomic<int> replayed_acks{0};
    std::atomic<wire::SchedulingState> scheduling{
        wire::SchedulingState::Stopped};
};

class FakeWorker final {
public:
    FakeWorker(agent::TlsChannel channel, std::string leader_id,
               std::uint64_t leader_epoch,
               std::shared_ptr<FakeWorkerMemory> memory =
                   std::make_shared<FakeWorkerMemory>())
        : channel_(std::move(channel)), leader_id_(std::move(leader_id)),
          leader_epoch_(leader_epoch), memory_(std::move(memory)) {}
    ~FakeWorker() { Stop(); }

    bool Start() {
        if (!send_wire(channel_, StatusMessage())) return false;
        wire::Message response;
        if (!receive_wire(channel_, response)) return false;
        if (const auto* status = std::get_if<wire::Status>(&response.payload)) {
            if (status->connection != wire::ConnectionState::Connected ||
                status->scheduling != wire::SchedulingState::Stopped ||
                !status->capabilities.empty())
                return false;
        } else if (std::get_if<wire::Command>(&response.payload)) {
            initial_response_ = std::move(response);
        } else {
            return false;
        }
        thread_ = std::thread([this] { Run(); });
        return true;
    }

    void ConfigureAsync(bool disconnect_after_accept = false,
                        bool duplicate_final_ack = false) noexcept {
        async_commands_.store(true, std::memory_order_release);
        disconnect_after_accept_.store(disconnect_after_accept,
                                       std::memory_order_release);
        duplicate_final_ack_.store(duplicate_final_ack,
                                   std::memory_order_release);
    }
    void ConfigureImmediate(bool duplicate_final_ack = false) noexcept {
        async_commands_.store(false, std::memory_order_release);
        disconnect_after_accept_.store(false, std::memory_order_release);
        duplicate_final_ack_.store(duplicate_final_ack,
                                   std::memory_order_release);
    }

    void PauseHeartbeats() {
        std::unique_lock<std::mutex> lock(mutex_);
        enabled_ = false;
        condition_.wait(lock, [&] { return !in_cycle_; });
    }

    void ResumeHeartbeats() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            enabled_ = true;
        }
        condition_.notify_all();
    }

    void CompletePending() {
        std::lock_guard<std::mutex> lock(memory_->mutex);
        if (memory_->pending) memory_->completion_requested = true;
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
            enabled_ = true;
        }
        condition_.notify_all();
        if (thread_.joinable()) thread_.join();
        channel_.Close();
    }

    int effects() const noexcept {
        return memory_->effects.load(std::memory_order_acquire);
    }
    int deliveries() const noexcept {
        return memory_->deliveries.load(std::memory_order_acquire);
    }
    bool paused() const noexcept {
        return memory_->paused.load(std::memory_order_acquire);
    }
    bool maintenance() const noexcept {
        return memory_->maintenance.load(std::memory_order_acquire);
    }
    bool failed() const noexcept {
        return memory_->failed.load(std::memory_order_acquire);
    }
    wire::Command LastCommand() const {
        std::lock_guard<std::mutex> lock(memory_->mutex);
        return memory_->last_command;
    }
    int heartbeat_cycles() const noexcept {
        return memory_->heartbeat_cycles.load(std::memory_order_acquire);
    }
    bool WaitForHeartbeatCycles(int count) const {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            if (heartbeat_cycles() >= count) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return heartbeat_cycles() >= count;
    }
    bool WaitForAcknowledgmentReplays(int count) const {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            if (memory_->replayed_acks.load(std::memory_order_acquire) >= count)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return memory_->replayed_acks.load(std::memory_order_acquire) >= count;
    }

    void ReplayAcknowledgment(wire::Acknowledgment acknowledgment) {
        std::lock_guard<std::mutex> lock(memory_->mutex);
        memory_->replay_ack = std::move(acknowledgment);
    }

    bool WaitForCommandDeliveries(int count) const {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            if (deliveries() >= count) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return deliveries() >= count;
    }

    bool WaitDisconnected() const {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            if (thread_exited_.load(std::memory_order_acquire)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return thread_exited_.load(std::memory_order_acquire);
    }

private:
    wire::Message StatusMessage() const {
        wire::Status status = worker_status();
        status.scheduling = memory_->scheduling.load(std::memory_order_acquire);
        wire::Message message;
        message.payload = std::move(status);
        return message;
    }

    static bool SameCommand(const wire::Command& left,
                            const wire::Command& right) noexcept {
        return left.command_id == right.command_id &&
               left.leader_id == right.leader_id &&
               left.leader_epoch == right.leader_epoch &&
               left.sequence == right.sequence && left.action == right.action &&
               left.target_job_id == right.target_job_id &&
               left.confirmed == right.confirmed &&
               left.issued_at_ms == right.issued_at_ms &&
               left.expires_at_ms == right.expires_at_ms;
    }

    void CompleteCommandLocked() {
        memory_->pending = false;
        memory_->completion_requested = false;
        memory_->last_outcome = wire::AcknowledgmentOutcome::Completed;
        switch (memory_->last_command.action) {
            case wire::CommandAction::Pause:
                memory_->paused.store(true, std::memory_order_release);
                memory_->scheduling.store(wire::SchedulingState::Paused,
                                          std::memory_order_release);
                break;
            case wire::CommandAction::Resume:
                memory_->paused.store(false, std::memory_order_release);
                memory_->scheduling.store(wire::SchedulingState::Accepting,
                                          std::memory_order_release);
                break;
            case wire::CommandAction::Stop:
                memory_->paused.store(true, std::memory_order_release);
                memory_->scheduling.store(wire::SchedulingState::Stopped,
                                          std::memory_order_release);
                break;
            case wire::CommandAction::Maintenance:
            case wire::CommandAction::Online:
            case wire::CommandAction::RestartService:
            case wire::CommandAction::ForceRestartService:
                break;
            case wire::CommandAction::RebootMachine:
            case wire::CommandAction::ActivateUpdate:
                break;
        }
    }

    wire::Message AcknowledgmentMessage(
        const wire::Command& command,
        wire::AcknowledgmentOutcome outcome) const {
        wire::Message message;
        message.payload = wire::Acknowledgment{
            command.command_id, command.leader_id, command.leader_epoch,
            command.sequence, command.action, outcome};
        return message;
    }
    bool ReceiveAdminUpdate(const wire::Command& command,
                            std::string& failure) {
        if (command.action != wire::CommandAction::ActivateUpdate) return true;
        try {
            if (!command.admin_intent || !command.package_offer) {
                failure = "activate-update command omitted its signed intent or offer";
                return false;
            }
            const auto& intent = *command.admin_intent;
            const auto& offer = *command.package_offer;
            fs::path root;
            std::string expected_worker_id;
            std::string expected_bytes;
            {
                std::lock_guard<std::mutex> lock(memory_->mutex);
                root = memory_->admin_transfer_root;
                expected_worker_id = memory_->expected_worker_id;
                expected_bytes = memory_->expected_package_bytes;
            }
            if (root.empty() || expected_worker_id.empty()) {
                failure = "admin package fixture is not configured";
                return false;
            }
            std::error_code ec;
            fs::create_directories(spirula::NativeFilesystemPath(root), ec);
            if (ec) {
                failure = "could not create fake worker package directory";
                return false;
            }
            const agent::TransferFile package{
                "package.bin", offer.manifest.size, offer.manifest.sha256};
            const auto received = agent::ReceiveArtifacts(
                channel_, root, {package}, offer.manifest.size + 4096);
            if (!received) {
                failure = std::string("package transfer failed: ") + received.message;
                return false;
            }

            std::ifstream input(
                spirula::NativeFilesystemPath(root / "package.bin"),
                std::ios::binary);
            if (!input) {
                failure = "transferred package could not be opened";
                return false;
            }
            std::string bytes;
            bytes.assign(std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>());
            if (input.bad()) {
                failure = "transferred package could not be read";
                return false;
            }
            spirula::Sha256 hash;
            hash.update(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                        bytes.size());
            const std::string digest = hash.hex();
            const bool verified =
                command.confirmed &&
                intent.operation == agent::admin::Operation::Activate &&
                intent.worker_id == expected_worker_id &&
                intent.leader_id == command.leader_id &&
                intent.leader_epoch == command.leader_epoch &&
                intent.intent_id == command.command_id &&
                intent.update_id == command.command_id &&
                intent.update_id == offer.update_id &&
                intent.package_sha256 == offer.manifest.sha256 &&
                intent.expires_at_unix == offer.expires_at_unix &&
                !intent.signature.empty() &&
                !intent.signer_public_key_pem.empty() &&
                offer.worker_id == expected_worker_id &&
                offer.leader_id == command.leader_id &&
                !offer.signature.empty() &&
                intent.signer_public_key_pem == offer.signer_public_key_pem &&
                offer.manifest.size == bytes.size() &&
                offer.manifest.sha256 == digest && bytes == expected_bytes;
            {
                std::lock_guard<std::mutex> lock(memory_->mutex);
                memory_->admin_update_verified = verified;
                memory_->admin_received_bytes = std::move(bytes);
            }
            if (!verified)
                failure = "staged bytes or signed offer bindings failed verification";
            return verified;
        } catch (const std::exception& exception) {
            failure = std::string("admin package handling failed: ") +
                      exception.what();
            return false;
        } catch (...) {
            failure = "admin package handling failed with an unknown error";
            return false;
        }
    }

    void Run() noexcept {
        struct Finished final {
            std::atomic<bool>& value;
            ~Finished() { value.store(true, std::memory_order_release); }
        } finished{thread_exited_};
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [&] { return stop_ || enabled_; });
                if (stop_) return;
                in_cycle_ = true;
            }
            bool keep_running = true;
            wire::Message response;
            if (initial_response_) {
                response = std::move(*initial_response_);
                initial_response_.reset();
            } else {
                keep_running = send_wire(channel_, StatusMessage());
                if (keep_running)
                    keep_running = receive_wire(channel_, response);
            }
            bool disconnect_after_ack = false;
            if (keep_running) {
                if (const auto* status = std::get_if<wire::Status>(&response.payload)) {
                    if (status->scheduling != wire::SchedulingState::Stopped ||
                        !status->capabilities.empty()) {
                        memory_->failed.store(true, std::memory_order_release);
                        keep_running = false;
                    } else {
                        std::optional<wire::Command> completed;
                        std::optional<wire::Acknowledgment> replayed;
                        {
                            std::lock_guard<std::mutex> lock(memory_->mutex);
                            if (memory_->pending &&
                                memory_->completion_requested) {
                                CompleteCommandLocked();
                                completed = memory_->last_command;
                            }
                            if (memory_->replay_ack) {
                                replayed = std::move(memory_->replay_ack);
                                memory_->replay_ack.reset();
                            }
                        }
                        if (completed) {
                            const auto acknowledgment = AcknowledgmentMessage(
                                *completed, wire::AcknowledgmentOutcome::Completed);
                            keep_running = send_wire(channel_, acknowledgment);
                            if (keep_running &&
                                duplicate_final_ack_.load(
                                    std::memory_order_acquire))
                                keep_running = send_wire(channel_, acknowledgment);
                        }
                        if (keep_running && replayed) {
                            wire::Message acknowledgment;
                            acknowledgment.payload = *replayed;
                            keep_running = send_wire(channel_, acknowledgment);
                            if (keep_running)
                                memory_->replayed_acks.fetch_add(
                                    1, std::memory_order_acq_rel);
                        }
                    }
                } else if (const auto* command =
                               std::get_if<wire::Command>(&response.payload)) {
                    std::string admin_failure;
                    bool admin_update_received = false;
                    try {
                        admin_update_received =
                            ReceiveAdminUpdate(*command, admin_failure);
                    } catch (const std::exception& exception) {
                        admin_failure = exception.what();
                    } catch (...) {
                        admin_failure = "admin package handling failed unexpectedly";
                    }
                    if (!admin_update_received) {
                        memory_->failed.store(true, std::memory_order_release);
                        std::lock_guard<std::mutex> lock(memory_->mutex);
                        memory_->failure_reason = admin_failure.empty()
                            ? "admin package handling failed without a diagnostic"
                            : std::move(admin_failure);
                    }
                    wire::AcknowledgmentOutcome outcome =
                        wire::AcknowledgmentOutcome::Rejected;
                    {
                        std::lock_guard<std::mutex> lock(memory_->mutex);
                        if (command->leader_id == leader_id_ &&
                            command->leader_epoch == leader_epoch_) {
                            if (memory_->has_command &&
                                command->command_id ==
                                    memory_->last_command.command_id) {
                                if (SameCommand(*command, memory_->last_command)) {
                                    outcome = memory_->pending
                                        ? wire::AcknowledgmentOutcome::Accepted
                                        : memory_->last_outcome;
                                    memory_->deliveries.fetch_add(
                                        1, std::memory_order_acq_rel);
                                }
                            } else if (command->sequence >
                                       memory_->last_sequence) {
                                memory_->last_sequence = command->sequence;
                                memory_->last_command = *command;
                                memory_->has_command = true;
                                memory_->deliveries.fetch_add(
                                    1, std::memory_order_acq_rel);
                                memory_->effects.fetch_add(
                                    1, std::memory_order_acq_rel);
                                if (async_commands_.load(
                                        std::memory_order_acquire) &&
                                    (command->action == wire::CommandAction::Pause ||
                                     command->action == wire::CommandAction::Stop)) {
                                    memory_->pending = true;
                                    memory_->completion_requested = false;
                                    memory_->last_outcome =
                                        wire::AcknowledgmentOutcome::Accepted;
                                    memory_->scheduling.store(
                                        wire::SchedulingState::Pausing,
                                        std::memory_order_release);
                                    outcome = wire::AcknowledgmentOutcome::Accepted;
                                } else {
                                    switch (command->action) {
                                        case wire::CommandAction::Maintenance:
                                            memory_->maintenance.store(
                                                true, std::memory_order_release);
                                            break;
                                        case wire::CommandAction::Online:
                                            memory_->maintenance.store(
                                                false, std::memory_order_release);
                                            break;
                                        case wire::CommandAction::Pause:
                                        case wire::CommandAction::Resume:
                                        case wire::CommandAction::Stop:
                                        case wire::CommandAction::RestartService:
                                        case wire::CommandAction::ForceRestartService:
                                        case wire::CommandAction::RebootMachine:
                                        case wire::CommandAction::ActivateUpdate:
                                            break;
                                    }
                                    CompleteCommandLocked();
                                    outcome = admin_update_received
                                        ? wire::AcknowledgmentOutcome::Completed
                                        : wire::AcknowledgmentOutcome::Rejected;
                                    memory_->last_outcome = outcome;
                                }
                            }
                        }
                    }
                    keep_running = send_wire(
                        channel_, AcknowledgmentMessage(*command, outcome));
                    disconnect_after_ack =
                        outcome == wire::AcknowledgmentOutcome::Accepted &&
                        disconnect_after_accept_.load(
                            std::memory_order_acquire);
                    if (keep_running &&
                        outcome == wire::AcknowledgmentOutcome::Completed &&
                        duplicate_final_ack_.load(std::memory_order_acquire))
                        keep_running = send_wire(
                            channel_, AcknowledgmentMessage(*command, outcome));
                } else {
                    memory_->failed.store(true, std::memory_order_release);
                    keep_running = false;
                }
            }
            if (disconnect_after_ack) {
                channel_.Close();
                keep_running = false;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                in_cycle_ = false;
            }
            condition_.notify_all();
            memory_->heartbeat_cycles.fetch_add(1, std::memory_order_acq_rel);
            if (!keep_running) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    agent::TlsChannel channel_;
    std::string leader_id_;
    std::uint64_t leader_epoch_ = 0;
    std::shared_ptr<FakeWorkerMemory> memory_;
    std::optional<wire::Message> initial_response_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool enabled_ = true;
    bool in_cycle_ = false;
    bool stop_ = false;
    std::atomic<bool> async_commands_{false};
    std::atomic<bool> disconnect_after_accept_{false};
    std::atomic<bool> duplicate_final_ack_{false};
    std::atomic<bool> thread_exited_{false};
};

#ifdef SS_TOOL_SFM
namespace fw = sfm::feature_work;

void write_text_file(const fs::path& path, const std::string& text) {
    if (!write_native_file(path, text.data(), text.size()))
        throw std::runtime_error("could not write feature fixture");
}

struct FeatureFixture final {
    fs::path root;
    fs::path plan_path;
    fs::path request_path;
    fs::path image_root;
    fs::path mask_root;
    fw::FeaturePlan plan;
    fw::FeatureRequest request;
};

FeatureFixture make_feature_fixture(const fs::path& root,
                                    const std::string& attempt_id) {
    FeatureFixture fixture;
    fixture.root = root;
    fixture.plan_path = root / "plan.json";
    fixture.request_path = root / "request.json";
    fixture.image_root = root / "images";
    fixture.mask_root = root / "masks";
    const std::string image_bytes = "leader feature loopback image";
    write_text_file(fixture.image_root / "image.jpg", image_bytes);
    fs::create_directories(spirula::NativeFilesystemPath(fixture.mask_root));

    auto& plan = fixture.plan;
    plan.extraction.frontend = "sift";
    plan.extraction.descriptor = "sift-u8";
    plan.extraction.descriptor_dim = 4;
    plan.extraction.descriptor_dtype = 0;
    plan.extraction.implementation = "loopback-test";
    fw::PlanImage image;
    image.global_index = 0;
    image.logical_name = "image.jpg";
    image.feature_path = fw::featurePathForImage(image.logical_name);
    image.source_digest = fw::sha256Text(image_bytes);
    image.owner_shard = 7;
    image.artifact_key = fw::imageArtifactKey(
        image.source_digest, {}, plan.extraction);
    plan.images.push_back(std::move(image));
    plan.dataset_digest = fw::datasetSnapshotDigest(plan.images);
    plan.digest = fw::planDigest(plan);
    fw::validatePlan(plan);

    auto& request = fixture.request;
    request.plan_digest = fw::planDigest(plan);
    request.plan_path = "plan.json";
    request.shard = 7;
    request.attempt_id = attempt_id;
    request.image_indices = {0};
    request.digest = fw::requestDigest(request);
    fw::validateRequest(plan, request);
    fw::writePlanFile(
        spirula::NativeFilesystemPath(fixture.plan_path).u8string(), plan);
    fw::writeRequestFile(
        spirula::NativeFilesystemPath(fixture.request_path).u8string(), request);
    return fixture;
}

wire::Status feature_worker_status() {
    wire::Status status;
    status.connection = wire::ConnectionState::Connected;
    status.compatibility = wire::CompatibilityState::Compatible;
    status.scheduling = wire::SchedulingState::Accepting;
    status.activity = wire::ActivityState::Idle;
    status.health = wire::HealthState::Healthy;
    status.capabilities = {wire::Capability::Feature};
#ifdef SS_VERSION
    status.build = SS_VERSION;
#else
    status.build = "unknown";
#endif
    status.platform = "loopback";
    status.gpu = "AMD Vulkan loopback";
    return status;
}

class FeatureWorker final {
public:
    FeatureWorker(agent::TlsChannel channel, fs::path artifacts_root,
                  std::uint64_t disk_budget)
        : channel_(std::move(channel)), artifacts_root_(std::move(artifacts_root)),
          disk_budget_(disk_budget) {}
    ~FeatureWorker() { Stop(); }

    bool Start() {
        wire::Message status;
        status.payload = feature_worker_status();
        if (!send_wire(channel_, status)) return false;
        wire::Message response;
        if (!receive_wire(channel_, response) ||
            !std::get_if<wire::Status>(&response.payload))
            return false;
        thread_ = std::thread([this] { Run(); });
        return true;
    }

    bool WaitForInputs(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(15),
            [&] { return inputs_ >= count || failed_ || stopping_; }) &&
            inputs_ >= count;
    }
    bool WaitForCommitted(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(15),
            [&] { return committed_ >= count || failed_ || stopping_; }) &&
            committed_ >= count;
    }
    bool WaitForRejected(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(15),
            [&] { return rejected_ >= count || failed_ || stopping_; }) &&
            rejected_ >= count;
    }
    void ReleaseNextResult() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++release_count_;
        }
        condition_.notify_all();
    }
    bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }
    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            release_count_ = inputs_;
        }
        condition_.notify_all();
        if (thread_.joinable()) thread_.join();
        channel_.Close();
    }

private:
    bool WaitToPublish() {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [&] { return stopping_ || release_count_ != 0; });
        if (stopping_) return false;
        --release_count_;
        return true;
    }

    bool BuildOutput(const agent::FeatureInputBundle& input, const fs::path& root,
                     std::vector<agent::TransferFile>& manifest) {
        try {
            fs::create_directories(spirula::NativeFilesystemPath(root));
            fw::writeRequestFile(
                spirula::NativeFilesystemPath(root / "request.json").u8string(),
                input.request);
            fw::WorkerBinding binding;
            binding.extraction_request = input.request;
            binding.request_path = "request.json";
            binding.image_root = input.image_root.u8string();
            binding.mask_root = input.mask_root.u8string();
            binding.model_root = (artifacts_root_ / "model").u8string();
            binding.device = "AMD Vulkan loopback";
            binding.output_dir = root.u8string();
            binding.max_payload_bytes = disk_budget_;
            binding.max_images =
                static_cast<std::uint32_t>(input.request.image_indices.size());
            binding.digest = fw::workerBindingDigest(binding);
            fw::writeWorkerBindingFile(
                spirula::NativeFilesystemPath(root / "binding.json").u8string(),
                binding);

            fw::ShardResult result;
            result.plan_digest = fw::planDigest(input.plan);
            result.request_digest = fw::requestDigest(input.request);
            result.shard = input.request.shard;
            result.complete = true;
            for (const std::uint32_t index : input.request.image_indices) {
                const fw::PlanImage& image = input.plan.images[index];
                const fs::path payload =
                    root / "payload" / fs::u8path(image.feature_path);
                fs::create_directories(
                    spirula::NativeFilesystemPath(payload.parent_path()));
                sfm::FeatureSet empty_features;
                empty_features.width = 32;
                empty_features.height = 24;
                empty_features.dim = input.plan.extraction.descriptor_dim;
                empty_features.dtype = static_cast<sfm::DType>(
                    input.plan.extraction.descriptor_dtype);
                sfm::writeFeatures(
                    spirula::NativeFilesystemPath(payload).u8string(), empty_features);

                fw::ImageReceipt receipt;
                receipt.request_digest = input.request.digest;
                receipt.global_index = index;
                receipt.logical_name = image.logical_name;
                receipt.feature_path = image.feature_path;
                receipt.artifact_key = image.artifact_key;
                receipt.payload_digest = fw::sha256File(
                    spirula::NativeFilesystemPath(payload).u8string());
                receipt.payload_bytes =
                    fs::file_size(spirula::NativeFilesystemPath(payload));
                receipt.feature_count = 0;
                receipt.descriptor_dtype = input.plan.extraction.descriptor_dtype;
                receipt.descriptor_dim = input.plan.extraction.descriptor_dim;
                receipt.width = empty_features.width;
                receipt.height = empty_features.height;
                receipt.producer_build = input.plan.extraction.implementation;
                receipt.producer_device = "AMD Vulkan loopback";
                receipt.digest = fw::receiptDigest(receipt);
                std::string receipt_name = std::to_string(index);
                if (receipt_name.size() < 8)
                    receipt_name.insert(0, 8 - receipt_name.size(), '0');
                fw::writeReceiptFile(
                    spirula::NativeFilesystemPath(
                        root / "receipts" / (receipt_name + ".json")).u8string(),
                    receipt);

                fw::ImageOutcome outcome;
                outcome.global_index = index;
                outcome.status = fw::outcome_status::zero_features;
                outcome.receipt_digest = receipt.digest;
                outcome.digest = fw::outcomeDigest(outcome);
                result.outcomes.push_back(std::move(outcome));
            }
            result.digest = fw::shardResultDigest(result);
            fw::writeShardResultFile(
                spirula::NativeFilesystemPath(root / "result.json").u8string(),
                result);
            manifest = agent::FeatureOutputManifest(
                input.request_path, root, disk_budget_);
            return !manifest.empty();
        } catch (const std::exception& exception) {
            std::fprintf(stderr, "diag feature fake output: %s\n",
                         exception.what());
            return false;
        } catch (...) {
            std::fprintf(stderr, "diag feature fake output: unknown failure\n");
            return false;
        }
    }

    bool HandleOffer(const wire::FeatureOffer& offer) {
        wire::FeatureDecision accepted;
        accepted.job_id = offer.job_id;
        accepted.attempt_id = offer.attempt_id;
        accepted.step = wire::FeatureDecision::Step::Offer;
        accepted.decision = wire::FeatureDecision::Decision::Accepted;
        if (!send_wire(channel_, wire::Message{wire::kSchemaVersion, accepted})) return false;

        const fs::path attempt_root = artifacts_root_ /
            fs::u8path(offer.job_id) / fs::u8path(offer.attempt_id);
        const fs::path input_root = attempt_root / "input";
        fs::create_directories(spirula::NativeFilesystemPath(input_root));
        const auto received = agent::ReceiveArtifacts(
            channel_, input_root, offer.inputs, disk_budget_);
        if (!received) return false;
        try {
            const fw::FeatureRequest request = fw::readRequestFile(
                spirula::NativeFilesystemPath(
                    input_root / "request.json").u8string());
            const auto plan_path = fs::u8path(request.plan_path);
            if (plan_path.is_absolute() || plan_path.has_root_path())
                return false;
            for (const auto& part : plan_path)
                if (part == "..") return false;
            const fw::FeaturePlan plan = fw::readPlanFile(
                spirula::NativeFilesystemPath(input_root / plan_path).u8string());
            const agent::FeatureInputBundle input = agent::VerifyFeatureInputs(
                input_root, plan, request, offer.inputs, disk_budget_);
            wire::FeatureDecision inputs;
            inputs.job_id = offer.job_id;
            inputs.attempt_id = offer.attempt_id;
            inputs.step = wire::FeatureDecision::Step::Inputs;
            inputs.decision = wire::FeatureDecision::Decision::Accepted;
            if (!send_wire(channel_, wire::Message{wire::kSchemaVersion, inputs})) return false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++inputs_;
            }
            condition_.notify_all();
            if (!WaitToPublish()) return false;

            const fs::path output_root = attempt_root / "output";
            std::vector<agent::TransferFile> outputs;
            if (!BuildOutput(input, output_root, outputs)) return false;
            wire::FeatureResult result;
            result.job_id = offer.job_id;
            result.attempt_id = offer.attempt_id;
            result.outcome = wire::FeatureResult::Outcome::Succeeded;
            result.outputs = outputs;
            if (!send_wire(channel_, wire::Message{wire::kSchemaVersion, result})) return false;
            wire::Message decision_message;
            if (!receive_wire(channel_, decision_message)) return false;
            const auto* decision =
                std::get_if<wire::FeatureDecision>(&decision_message.payload);
            if (!decision || decision->job_id != offer.job_id ||
                decision->attempt_id != offer.attempt_id ||
                decision->step != wire::FeatureDecision::Step::Output)
                return false;
            if (decision->decision == wire::FeatureDecision::Decision::Rejected) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++rejected_;
                }
                condition_.notify_all();
                return true;
            }
            if (decision->decision != wire::FeatureDecision::Decision::Accepted ||
                !agent::SendArtifacts(channel_, output_root, outputs, disk_budget_))
                return false;
            if (!receive_wire(channel_, decision_message)) return false;
            decision = std::get_if<wire::FeatureDecision>(&decision_message.payload);
            if (!decision || decision->job_id != offer.job_id ||
                decision->attempt_id != offer.attempt_id ||
                decision->step != wire::FeatureDecision::Step::Output ||
                decision->decision != wire::FeatureDecision::Decision::Committed)
                return false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++committed_;
            }
            condition_.notify_all();
            return true;
        } catch (...) {
            return false;
        }
    }

    void Run() noexcept {
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopping_) return;
            }
            wire::Message status_message;
            status_message.payload = feature_worker_status();
            wire::Message response;
            if (!send_wire(channel_, status_message) ||
                !receive_wire(channel_, response)) {
                break;
            }
            if (const auto* offer =
                    std::get_if<wire::FeatureOffer>(&response.payload)) {
                if (!HandleOffer(*offer)) break;
            } else if (!std::get_if<wire::Status>(&response.payload)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        failed_.store(true, std::memory_order_release);
        condition_.notify_all();
    }

    agent::TlsChannel channel_;
    fs::path artifacts_root_;
    std::uint64_t disk_budget_ = 0;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t inputs_ = 0;
    std::size_t release_count_ = 0;
    std::size_t committed_ = 0;
    std::size_t rejected_ = 0;
    bool stopping_ = false;
    std::atomic<bool> failed_{false};
};
wire::Status reconstruction_worker_status() {
    wire::Status status;
    status.connection = wire::ConnectionState::Connected;
    status.compatibility = wire::CompatibilityState::Compatible;
    status.scheduling = wire::SchedulingState::Accepting;
    status.activity = wire::ActivityState::Idle;
    status.health = wire::HealthState::Healthy;
    status.capabilities = {wire::Capability::Reconstruction};
    status.build = agent::ReconstructionBuildIdentity();
    status.platform = "loopback";
    status.gpu = "AMD Vulkan loopback";
    return status;
}

sfm::Reconstruction reconstruction_model() {
    sfm::Reconstruction model;
    model.cameras.emplace(1, sfm::Camera::defaultFor(
        1, 2, 2, 1.2, sfm::CamModel::Pinhole));
    for (std::uint32_t id = 1; id <= 2; ++id) {
        sfm::Image image;
        image.id = id;
        image.camera_id = 1;
        image.name = id == 1 ? "cam0/a.ppm" : "cam1/a.ppm";
        image.registered = true;
        image.pose = {sfm::mat3Identity(), {0, 0, 0}};
        image.points2D = {{1, 1}};
        image.point3D_ids = {10};
        model.images.emplace(id, std::move(image));
    }
    sfm::Point3D point;
    point.xyz = {0, 0, 2};
    point.rgb[0] = point.rgb[1] = point.rgb[2] = 128;
    point.track = {{1, 0}, {2, 0}};
    model.points3D.emplace(10, std::move(point));
    return model;
}

class ReconstructionWorker final {
public:
    ReconstructionWorker(agent::TlsChannel channel, fs::path artifacts_root,
                         std::uint64_t disk_budget)
        : channel_(std::move(channel)), artifacts_root_(std::move(artifacts_root)),
          disk_budget_(disk_budget) {}
    ~ReconstructionWorker() { Stop(); }

    bool Start() {
        wire::Message status;
        status.payload = reconstruction_worker_status();
        if (!send_wire(channel_, status)) return false;
        wire::Message response;
        if (!receive_wire(channel_, response) ||
            !std::get_if<wire::Status>(&response.payload))
            return false;
        thread_ = std::thread([this] { Run(); });
        return true;
    }

    bool WaitForInputs(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(15),
            [&] { return inputs_ >= count || failed_ || stopping_; }) &&
            inputs_ >= count;
    }
    bool WaitForCommitted(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(15),
            [&] { return committed_ >= count || failed_ || stopping_; }) &&
            committed_ >= count;
    }
    bool WaitForRejected(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(15),
            [&] { return rejected_ >= count || failed_ || stopping_; }) &&
            rejected_ >= count;
    }
    void ReleaseNextResult() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++release_count_;
        }
        condition_.notify_all();
    }
    bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }
    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            release_count_ = inputs_;
        }
        condition_.notify_all();
        if (thread_.joinable()) thread_.join();
        channel_.Close();
    }

private:
    bool WaitToPublish() {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [&] { return stopping_ || release_count_ != 0; });
        if (stopping_) return false;
        --release_count_;
        return true;
    }

    bool HandleOffer(const wire::PortableOffer& offer) {
        if (offer.workload != wire::PortableWorkload::Reconstruction) return false;
        wire::PortableDecision accepted;
        accepted.workload = wire::PortableWorkload::Reconstruction;
        accepted.job_id = offer.job_id;
        accepted.attempt_id = offer.attempt_id;
        accepted.step = wire::PortableDecision::Step::Offer;
        accepted.decision = wire::PortableDecision::Decision::Accepted;
        if (!send_wire(channel_, wire::Message{wire::kSchemaVersion, accepted})) return false;

        const fs::path attempt_root = artifacts_root_ /
            fs::u8path(offer.job_id) / fs::u8path(offer.attempt_id);
        const fs::path input_root = attempt_root / "input";
        fs::create_directories(spirula::NativeFilesystemPath(input_root));
        if (!agent::ReceiveArtifacts(channel_, input_root, offer.inputs,
                                     disk_budget_))
            return false;
        try {
            const auto inputs = agent::VerifyReconstructionInputs(
                input_root, offer.inputs, offer.input_identity_sha256,
                disk_budget_);
            wire::PortableDecision inputs_accepted;
            inputs_accepted.workload = wire::PortableWorkload::Reconstruction;
            inputs_accepted.job_id = offer.job_id;
            inputs_accepted.attempt_id = offer.attempt_id;
            inputs_accepted.step = wire::PortableDecision::Step::Inputs;
            inputs_accepted.decision = wire::PortableDecision::Decision::Accepted;
            if (!send_wire(channel_, wire::Message{wire::kSchemaVersion,
                                                    inputs_accepted}))
                return false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++inputs_;
            }
            condition_.notify_all();
            if (!WaitToPublish()) return false;

            const fs::path output_root = attempt_root / "output";
            const fs::path sparse_root = output_root / "sparse";
            fs::create_directories(spirula::NativeFilesystemPath(sparse_root / "0"));
            reconstruction_model().writeBinary(
                spirula::NativeFilesystemPath(sparse_root / "0").u8string());
            const auto outputs = agent::ReconstructionOutputManifest(
                inputs, sparse_root, disk_budget_);
            wire::PortableResult result;
            result.workload = wire::PortableWorkload::Reconstruction;
            result.job_id = offer.job_id;
            result.attempt_id = offer.attempt_id;
            result.outcome = wire::PortableResult::Outcome::Succeeded;
            result.outputs = outputs;
            wire::Message response;
            if (!send_wire(channel_, wire::Message{wire::kSchemaVersion, result}) ||
                !receive_wire(channel_, response))
                return false;
            const auto* decision =
                std::get_if<wire::PortableDecision>(&response.payload);
            if (!decision || decision->workload !=
                    wire::PortableWorkload::Reconstruction ||
                decision->job_id != offer.job_id ||
                decision->attempt_id != offer.attempt_id ||
                decision->step != wire::PortableDecision::Step::Output)
                return false;
            if (decision->decision == wire::PortableDecision::Decision::Rejected) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++rejected_;
                }
                condition_.notify_all();
                return true;
            }
            if (decision->decision != wire::PortableDecision::Decision::Accepted ||
                !agent::SendArtifacts(channel_, sparse_root, outputs, disk_budget_) ||
                !receive_wire(channel_, response))
                return false;
            decision = std::get_if<wire::PortableDecision>(&response.payload);
            if (!decision || decision->workload !=
                    wire::PortableWorkload::Reconstruction ||
                decision->job_id != offer.job_id ||
                decision->attempt_id != offer.attempt_id ||
                decision->step != wire::PortableDecision::Step::Output ||
                decision->decision != wire::PortableDecision::Decision::Committed)
                return false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++committed_;
            }
            condition_.notify_all();
            return true;
        } catch (...) {
            return false;
        }
    }

    void Run() noexcept {
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopping_) return;
            }
            wire::Message status;
            status.payload = reconstruction_worker_status();
            wire::Message response;
            if (!send_wire(channel_, status) || !receive_wire(channel_, response))
                break;
            if (const auto* offer =
                    std::get_if<wire::PortableOffer>(&response.payload)) {
                if (!HandleOffer(*offer)) break;
            } else if (!std::get_if<wire::Status>(&response.payload)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        failed_.store(true, std::memory_order_release);
        condition_.notify_all();
    }

    agent::TlsChannel channel_;
    fs::path artifacts_root_;
    std::uint64_t disk_budget_ = 0;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t inputs_ = 0;
    std::size_t release_count_ = 0;
    std::size_t committed_ = 0;
    std::size_t rejected_ = 0;
    bool stopping_ = false;
    std::atomic<bool> failed_{false};
};

#endif

struct NetworkScope final {
#ifdef _WIN32
    NetworkScope() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            throw std::runtime_error("Winsock startup failed");
    }
    ~NetworkScope() { WSACleanup(); }
#else
    NetworkScope() = default;
#endif
};

std::optional<agent::TlsChannel> connect_worker(pairing::Worker& worker,
                                                 const LeaderServer& server,
                                                 std::string* error) {
    auto options = worker.ClientOptionsForLeader("127.0.0.1", "leader.local",
        server.OperationalPort(), error);
    if (!options) return std::nullopt;
    return agent::TlsChannel::ConnectClient(*options, error);
}

bool wait_for_snapshot(LeaderServer& server, const std::string& worker_id,
                       bool ready, bool connected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        for (const auto& worker : server.Snapshot()) {
            if (worker.id == worker_id && worker.ready == ready &&
                worker.connected == connected)
                return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

bool wait_for_scheduling(LeaderServer& server, const std::string& worker_id,
                         wire::SchedulingState expected) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(3);
    do {
        for (const auto& worker : server.Snapshot())
            if (worker.id == worker_id && worker.connected &&
                worker.status.scheduling == expected)
                return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

bool wait_for_command_state(LeaderServer& server, const std::string& command_id,
                            LeaderServer::CommandState expected) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(3);
    do {
        for (const auto& command : server.CommandSnapshots())
            if (command.command_id == command_id && command.state == expected)
                return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

bool wait_for_final_command(LeaderServer& server, const std::string& command_id,
                            LeaderServer::CommandResult& result) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(10);
    do {
        for (const auto& command : server.CommandSnapshots()) {
            if (command.command_id != command_id) continue;
            result = command;
            if (command.state == LeaderServer::CommandState::Applied ||
                command.state == LeaderServer::CommandState::Rejected ||
                command.state == LeaderServer::CommandState::Failed ||
                command.state == LeaderServer::CommandState::Expired)
                return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

#ifdef SS_TOOL_TRAIN
wire::Status training_worker_status() {
    wire::Status status;
    status.connection = wire::ConnectionState::Connected;
    status.compatibility = wire::CompatibilityState::Compatible;
    status.scheduling = wire::SchedulingState::Accepting;
    status.activity = wire::ActivityState::Idle;
    status.health = wire::HealthState::Healthy;
    status.online = true;
    status.capabilities = {wire::Capability::Training};
#ifdef SS_VERSION
    status.build = SS_VERSION;
#else
    status.build = "unknown";
#endif
    status.platform = "loopback";
    status.gpu = "AMD Vulkan loopback";
    return status;
}

std::string training_config_json(const TrainConfig& config,
                                 const std::string& preset) {
    JsonWriter writer;
    writer.object().field("preset", preset);
    for (const auto& [key, value] : train_config_json_pairs(config))
        writer.field_raw(key, value);
    return writer.end().str();
}

void write_training_text(const fs::path& path, const std::string& value) {
    if (!write_native_file(path, value.data(), value.size()))
        throw std::runtime_error("training loopback fixture write failed");
}

void write_training_npy(std::ostream& tar, const std::string& name,
                        std::size_t count) {
    std::string bytes = ckpt::npy_header("<f4", count);
    bytes.append(count * sizeof(float), '\0');
    ckpt::tar_write_bytes(tar, name + ".npy", bytes.data(), bytes.size());
}

void write_training_state(const fs::path& path) {
    std::ostringstream tar(std::ios::binary | std::ios::out);
    const std::string state =
        "{\"format_version\":1,\"step\":3,\"full_resume\":1,"
        "\"max_num_splats\":1,\"cur_num_splats\":1,\"num_sh\":0}";
    ckpt::tar_write_bytes(tar, "state.json", state.data(), state.size());
    for (const auto& [name, count] : {
             std::pair{"world.means", 3u}, std::pair{"world.quats", 4u},
             std::pair{"world.scales", 3u}, std::pair{"world.opacities", 1u},
             std::pair{"world.features_dc", 3u}, std::pair{"eng.radii", 1u},
             std::pair{"eng.accum_buffer", 2u},
             std::pair{"eng.g1_means", 3u}, std::pair{"eng.g2_means", 3u},
             std::pair{"eng.g1_quats", 4u}, std::pair{"eng.g2_quats", 4u},
             std::pair{"eng.g1_scales", 3u}, std::pair{"eng.g2_scales", 3u},
             std::pair{"eng.g1_opacities", 1u}, std::pair{"eng.g2_opacities", 1u},
             std::pair{"eng.g1_features_dc", 3u},
             std::pair{"eng.g2_features_dc", 3u}})
        write_training_npy(tar, name, count);
    ckpt::tar_finish(tar);
    const std::string bytes = tar.str();
    if (!write_native_file(path, bytes.data(), bytes.size()))
        throw std::runtime_error("cannot create loopback checkpoint archive");
}

void create_training_output(const agent::TrainingInputBundle& inputs,
                            const std::string& preset) {
    constexpr char checkpoint_name[] = "step-000000003.ckpt";
    const fs::path checkpoint = inputs.output_root / checkpoint_name;
    fs::create_directories(spirula::NativeFilesystemPath(checkpoint));
    write_training_text(inputs.output_root / "config.json",
                        training_config_json(inputs.config, preset));
    write_training_text(inputs.output_root / "scene_transform.json", "{}\n");
    write_training_text(inputs.output_root / "metrics.json",
                        "{\"avg_psnr\":1.0}\n");
    write_training_text(checkpoint / "config.json",
                        training_config_json(inputs.config, preset));
    write_training_text(checkpoint / "splat.ply",
        "ply\nformat ascii 1.0\nelement vertex 1\n"
        "property float x\nproperty float y\nproperty float z\n"
        "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n"
        "property float opacity\nproperty float scale_0\nproperty float scale_1\n"
        "property float scale_2\nproperty float rot_0\nproperty float rot_1\n"
        "property float rot_2\nproperty float rot_3\nend_header\n"
        "0 0 0 0 0 0 0 -2 -2 -2 1 0 0 0\n");
    write_training_state(checkpoint / "state.tar");
}

class TrainingWorker final {
public:
    TrainingWorker(agent::TlsChannel channel, fs::path artifacts_root,
                   std::uint64_t disk_budget)
        : channel_(std::move(channel)), artifacts_root_(std::move(artifacts_root)),
          disk_budget_(disk_budget) {}
    ~TrainingWorker() { Stop(); }

    bool Start() {
        wire::Message status;
        status.payload = training_worker_status();
        wire::Message response;
        if (!send_wire(channel_, status) || !receive_wire(channel_, response))
            return false;
        if (!std::get_if<wire::Status>(&response.payload)) {
            const auto* offer =
                std::get_if<wire::PortableOffer>(&response.payload);
            if (!offer || offer->workload != wire::PortableWorkload::Training)
                return false;
            pending_response_ = std::move(response);
        }
        thread_ = std::thread([this] { Run(); });
        return true;
    }
    bool WaitForInputs(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(20),
            [&] { return inputs_ >= count || failed_ || stopping_; }) &&
            inputs_ >= count;
    }
    bool WaitForCommitted(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(20),
            [&] { return committed_ >= count || failed_ || stopping_; }) &&
            committed_ >= count;
    }
    bool WaitForRejected(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(20),
            [&] { return rejected_ >= count || failed_ || stopping_; }) &&
            rejected_ >= count;
    }
    void ReleaseNextResult() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++release_count_;
        }
        condition_.notify_all();
    }
    bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }
    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            release_count_ = inputs_;
        }
        condition_.notify_all();
        if (thread_.joinable()) thread_.join();
        channel_.Close();
    }

private:
    bool WaitToPublish() {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [&] { return stopping_ || release_count_ != 0; });
        if (stopping_) return false;
        --release_count_;
        return true;
    }

    bool HandleOffer(const wire::PortableOffer& offer) {
        if (offer.workload != wire::PortableWorkload::Training) return false;
        wire::PortableDecision accepted;
        accepted.workload = wire::PortableWorkload::Training;
        accepted.job_id = offer.job_id;
        accepted.attempt_id = offer.attempt_id;
        accepted.step = wire::PortableDecision::Step::Offer;
        accepted.decision = wire::PortableDecision::Decision::Accepted;
        if (!send_wire(channel_, wire::Message{wire::kSchemaVersion, accepted})) return false;

        const fs::path attempt_root = artifacts_root_ /
            fs::u8path(offer.job_id) / fs::u8path(offer.attempt_id);
        const fs::path input_root = attempt_root / "input";
        fs::create_directories(spirula::NativeFilesystemPath(input_root));
        if (!agent::ReceiveArtifacts(channel_, input_root, offer.inputs,
                                     disk_budget_))
            return false;
        try {
            const auto inputs = agent::VerifyTrainingInputs(
                input_root, offer.inputs, offer.input_identity_sha256,
                disk_budget_);
            wire::PortableDecision inputs_accepted;
            inputs_accepted.workload = wire::PortableWorkload::Training;
            inputs_accepted.job_id = offer.job_id;
            inputs_accepted.attempt_id = offer.attempt_id;
            inputs_accepted.step = wire::PortableDecision::Step::Inputs;
            inputs_accepted.decision = wire::PortableDecision::Decision::Accepted;
            if (!send_wire(channel_, wire::Message{wire::kSchemaVersion,
                                                    inputs_accepted}))
                return false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++inputs_;
            }
            condition_.notify_all();
            if (!WaitToPublish()) return false;

            create_training_output(inputs, "3dgs");
            const auto outputs = agent::TrainingOutputManifest(
                inputs, inputs.output_root, "step-000000003.ckpt", disk_budget_);
            wire::PortableResult result;
            result.workload = wire::PortableWorkload::Training;
            result.job_id = offer.job_id;
            result.attempt_id = offer.attempt_id;
            result.outcome = wire::PortableResult::Outcome::Succeeded;
            result.outputs = outputs.files;
            result.output_metadata = outputs.returned_checkpoint;
            wire::Message response;
            if (!send_wire(channel_, wire::Message{wire::kSchemaVersion, result}) ||
                !receive_wire(channel_, response))
                return false;
            auto* decision = std::get_if<wire::PortableDecision>(&response.payload);
            if (!decision || decision->workload !=
                    wire::PortableWorkload::Training ||
                decision->job_id != offer.job_id ||
                decision->attempt_id != offer.attempt_id ||
                decision->step != wire::PortableDecision::Step::Output)
                return false;
            if (decision->decision == wire::PortableDecision::Decision::Rejected) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++rejected_;
                }
                condition_.notify_all();
                return true;
            }
            if (decision->decision != wire::PortableDecision::Decision::Accepted ||
                !agent::SendArtifacts(channel_, inputs.output_root,
                                      outputs.files, disk_budget_) ||
                !receive_wire(channel_, response))
                return false;
            decision = std::get_if<wire::PortableDecision>(&response.payload);
            if (!decision || decision->workload !=
                    wire::PortableWorkload::Training ||
                decision->job_id != offer.job_id ||
                decision->attempt_id != offer.attempt_id ||
                decision->step != wire::PortableDecision::Step::Output ||
                decision->decision != wire::PortableDecision::Decision::Committed)
                return false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++committed_;
            }
            condition_.notify_all();
            return true;
        } catch (...) {
            return false;
        }
    }

    void Run() noexcept {
        bool have_initial_response = pending_response_.has_value();
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopping_) return;
            }
            wire::Message response;
            if (have_initial_response) {
                response = std::move(*pending_response_);
                pending_response_.reset();
                have_initial_response = false;
            } else {
                wire::Message status;
                status.payload = training_worker_status();
                if (!send_wire(channel_, status) || !receive_wire(channel_, response))
                    break;
            }
            if (const auto* offer =
                    std::get_if<wire::PortableOffer>(&response.payload)) {
                if (!HandleOffer(*offer)) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!stopping_) failed_.store(true, std::memory_order_release);
                    condition_.notify_all();
                    return;
                }
            } else if (!std::get_if<wire::Status>(&response.payload)) {
                failed_.store(true, std::memory_order_release);
                condition_.notify_all();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!stopping_) failed_.store(true, std::memory_order_release);
        }
        condition_.notify_all();
    }

    agent::TlsChannel channel_;
    fs::path artifacts_root_;
    std::uint64_t disk_budget_ = 0;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t inputs_ = 0;
    std::size_t release_count_ = 0;
    std::size_t committed_ = 0;
    std::size_t rejected_ = 0;
    bool stopping_ = false;
    std::optional<wire::Message> pending_response_;
    std::atomic<bool> failed_{false};
};

struct TrainingFixture final {
    fs::path dataset;
    TrainConfig config;
};

TrainingFixture make_training_fixture(const fs::path& root) {
    TrainingFixture fixture;
    fixture.dataset = root / "dataset";
    test::write_scene(fixture.dataset);
    fixture.config.data = fixture.dataset.u8string();
    fixture.config.data_format = "nerfstudio";
    fixture.config.save_full_checkpoint = true;
    fixture.config.disable_viewer = true;
    fixture.config.eval_mode = "all";
    return fixture;
}

bool wait_training_job(LeaderServer& server, const std::string& job_id,
                       LeaderServer::TrainingJobState expected,
                       LeaderServer::TrainingJobSnapshot* snapshot = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(20);
    do {
        for (const auto& job : server.TrainingJobs()) {
            if (job.job_id == job_id && job.state == expected) {
                if (snapshot) *snapshot = job;
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
#endif

bool wait_for_error(LeaderServer& server, const std::string& worker_id) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        for (const auto& worker : server.Snapshot()) {
            if (worker.id == worker_id && !worker.connected && !worker.ready &&
                !worker.error.empty())
                return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

#ifdef SS_TOOL_SFM
bool wait_feature_state(LeaderServer& server, const std::string& job_id,
                        const std::string& attempt_id,
                        LeaderServer::FeatureJobState expected) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(15);
    do {
        for (const auto& job : server.FeatureJobs())
            if (job.job_id == job_id && job.attempt_id == attempt_id &&
                job.state == expected)
                return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

void diagnose_feature_job(LeaderServer& server, const std::string& job_id) {
    for (const auto& job : server.FeatureJobs()) {
        if (job.job_id != job_id) continue;
        std::error_code ec;
        const bool result_exists = fs::is_regular_file(
            spirula::NativeFilesystemPath(job.result_root / "result.json"), ec);
        const std::string root = job.result_root.string();
        std::fprintf(stderr,
            "diag feature job=%s attempt=%s state=%d error=%s root=%s result=%d fs_error=%d\n",
            job.job_id.c_str(), job.attempt_id.c_str(),
            static_cast<int>(job.state), job.error.c_str(), root.c_str(),
            result_exists, ec.value());
    }
    for (const auto& worker : server.Snapshot())
        if (!worker.error.empty())
            std::fprintf(stderr, "diag feature worker=%s connected=%d error=%s\n",
                worker.id.c_str(), worker.connected, worker.error.c_str());
}

int run_feature_roundtrip(const fs::path& root) {
    constexpr std::uint64_t budget = 1024 * 1024;
    const fs::path leader_root = root / "feature-leader";
    const fs::path worker_root_a = root / "feature-worker-a";
    const fs::path worker_root_b = root / "feature-worker-b";
    fs::create_directories(spirula::NativeFilesystemPath(leader_root));
    fs::create_directories(spirula::NativeFilesystemPath(worker_root_a));
    fs::create_directories(spirula::NativeFilesystemPath(worker_root_b));
    LeaderServer::Options options;
    options.state_root = leader_root;
    options.bind_address = "127.0.0.1";
    options.server_name = "leader.local";
    std::string error;
    auto server = LeaderServer::Start(options, &error);
    check(server && server->Running(), "starts feature leader for loopback transfer");
    if (!server) return 1;

    auto pair_worker = [&](const fs::path& worker_root, const char* label)
        -> std::optional<pairing::Worker> {
        auto paired = pairing::Worker::Open(worker_root, &error);
        auto invitation = server->IssueInvitation();
        if (!paired || !invitation) return std::nullopt;
        const auto leader_pin = server->EnrollmentPeerFingerprint();
        RawSocket enrollment = connect_loopback(server->EnrollmentPort());
        if (enrollment == kBadSocket || !paired->Redeem(
                native_socket(enrollment), invitation->code, leader_pin,
                "leader.local", label, &error))
            return std::nullopt;
        const std::string id = paired->WorkerId();
        if (!server->Approve(id, &error)) return std::nullopt;
        enrollment = connect_loopback(server->EnrollmentPort());
        if (enrollment == kBadSocket || !paired->CheckApproval(
                native_socket(enrollment), invitation->code, leader_pin,
                "leader.local", &error))
            return std::nullopt;
        return paired;
    };
    auto worker_a = pair_worker(worker_root_a, "feature loopback worker A");
    auto worker_b = pair_worker(worker_root_b, "feature loopback worker B");
    check(worker_a.has_value() && worker_b.has_value(),
          "pairs two independently authenticated feature workers");
    if (!worker_a || !worker_b) return 1;
    const std::string worker_id_a = worker_a->WorkerId();
    const std::string worker_id_b = worker_b->WorkerId();
    auto channel_a = connect_worker(*worker_a, *server, &error);
    auto channel_b = connect_worker(*worker_b, *server, &error);
    check(channel_a.has_value() && channel_b.has_value(),
          "connects both paired feature workers concurrently");
    if (!channel_a || !channel_b) return 1;
    FeatureWorker fake_a(std::move(*channel_a), worker_root_a / "artifacts", budget);
    FeatureWorker fake_b(std::move(*channel_b), worker_root_b / "artifacts", budget);
    check(fake_a.Start() && fake_b.Start(),
          "both workers advertise matching build and Vulkan feature capability");
    if (!wait_for_snapshot(*server, worker_id_a, true, true) ||
        !wait_for_snapshot(*server, worker_id_b, true, true)) {
        check(false, "both feature workers become dispatch-eligible");
        return 1;
    }

    auto success_fixture = make_feature_fixture(
        root / "success-source", "attempt-shared");
    auto stale_fixture = make_feature_fixture(
        root / "stale-source", "attempt-shared");
    check(server->SubmitFeatureShard(worker_id_a, "feature-success",
        success_fixture.plan_path, success_fixture.request_path,
        success_fixture.image_root, success_fixture.mask_root, budget, &error),
        "accepts a feature shard into asynchronous staging");
    check(server->SubmitFeatureShard(worker_id_b, "feature-stale",
        stale_fixture.plan_path, stale_fixture.request_path,
        stale_fixture.image_root, stale_fixture.mask_root, budget, &error),
        "submits the same request attempt ID for a second worker's shard");
    check(fake_a.WaitForInputs(1) &&
              wait_feature_state(*server, "feature-success", "attempt-shared",
                  LeaderServer::FeatureJobState::Running) &&
              fake_b.WaitForInputs(1) &&
              wait_feature_state(*server, "feature-stale", "attempt-shared",
                  LeaderServer::FeatureJobState::Running),
          "transfers and verifies both input bundles on independent sessions");
    bool worker_a_busy = false;
    bool worker_b_busy = false;
    for (const auto& item : server->Snapshot()) {
        if (item.id == worker_id_a) worker_a_busy = item.connected && !item.ready;
        if (item.id == worker_id_b) worker_b_busy = item.connected && !item.ready;
    }
    check(worker_a_busy && worker_b_busy,
          "workers with active feature attempts are connected but not Ready");
    fake_a.ReleaseNextResult();
    const bool feature_committed = fake_a.WaitForCommitted(1) &&
        wait_feature_state(*server, "feature-success", "attempt-shared",
            LeaderServer::FeatureJobState::Succeeded);
    if (!feature_committed)
        diagnose_feature_job(*server, "feature-success");
    check(feature_committed,
          "publishes only a transferred and leader-verified feature result");
    fs::path successful_result_root;
    for (const auto& job : server->FeatureJobs())
        if (job.job_id == "feature-success" &&
            job.attempt_id == "attempt-shared")
            successful_result_root = job.result_root;
    const bool feature_result_present = !successful_result_root.empty() &&
        fs::is_regular_file(spirula::NativeFilesystemPath(
            successful_result_root / "result.json"));
    if (!feature_result_present)
        diagnose_feature_job(*server, "feature-success");
    check(feature_result_present,
          "committed result remains available under the leader-owned result root");

    check(server->SupersedeFeatureShard("feature-stale", &error),
          "persists explicit supersession before late result publication");
    fake_b.ReleaseNextResult();
    const bool stale_result_rejected = fake_b.WaitForRejected(1) &&
        !fake_a.failed() && !fake_b.failed();
    if (!stale_result_rejected) {
        diagnose_feature_job(*server, "feature-success");
        diagnose_feature_job(*server, "feature-stale");
    }
    check(stale_result_rejected,
          "rejects a late result while the other worker remains successful");
    bool fenced = false;
    for (const auto& job : server->FeatureJobs())
        if (job.job_id == "feature-stale" &&
            job.attempt_id == "attempt-shared")
            fenced = job.state == LeaderServer::FeatureJobState::Superseded &&
                     !fs::exists(spirula::NativeFilesystemPath(
                         job.result_root / "result.json"));
    check(fenced, "superseded attempt never publishes into the result root");
    fake_a.Stop();
    fake_b.Stop();
    server->Stop();
    return 0;

}
bool wait_reconstruction_job(
    LeaderServer& server, const std::string& job_id,
    LeaderServer::ReconstructionJobState expected,
    LeaderServer::ReconstructionJobSnapshot* snapshot = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(15);
    do {
        for (const auto& job : server.ReconstructionJobs()) {
            if (job.job_id == job_id && job.state == expected) {
                if (snapshot) *snapshot = job;
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

struct ReconstructionFixture final {
    fs::path root;
    fs::path image_root;
    fs::path manifest;
    sfm::AutoRequest request;
};

ReconstructionFixture make_reconstruction_fixture(const fs::path& root) {
    ReconstructionFixture fixture;
    fixture.root = root;
    fixture.image_root = root / "images";
    fixture.manifest = root / "manifest.json";
    const std::string ppm = "P6\n2 2\n255\n" + std::string(12, 'x');
    write_text_file(fixture.image_root / "cam0/a.ppm", ppm);
    write_text_file(fixture.image_root / "cam1/a.ppm", ppm);
    write_text_file(root / "masks/cam0/a.ppm.png", "mask-a");
    write_text_file(root / "masks/cam1/a.ppm.png", "mask-b");
    write_text_file(root / "capture.bin", "telemetry-payload");
    write_text_file(fixture.manifest,
        "{\"image_dir\":\"images\",\"mask_dir\":\"masks\","
        "\"camera_mode\":\"folder\","
        "\"cameras\":[{\"prefix\":\"cam0\",\"model\":\"pinhole\",\"focal\":1.2},"
        "{\"prefix\":\"cam1\",\"model\":\"pinhole\",\"focal\":1.2}],"
        "\"captures\":[{\"prefix\":\"clip\",\"telemetry\":\"capture.bin\","
        "\"fps\":30,\"source_export_mapping\":\"mapping-1\","
        "\"timing_estimate\":\"estimate-1\","
        "\"synchronization_decision\":\"decision-1\"}],"
        "\"rigs\":[{\"name\":\"stereo\",\"captures\":[\"clip\"],"
        "\"members\":[{\"prefix\":\"cam0\",\"rotation\":[1,0,0,0],"
        "\"translation\":[0,0,0]},{\"prefix\":\"cam1\","
        "\"rotation\":[1,0,0,0],\"translation\":[0.1,0,0]}]}]}");
    const std::string parse_error = sfm::parse_auto_args(
        {fixture.image_root.u8string(), "-o", (root / "workspace").u8string(),
         "--manifest", fixture.manifest.u8string(), "--quality", "low",
         "--camera-mode", "image"},
        fixture.request, false);
    if (!parse_error.empty()) throw std::runtime_error(parse_error);
    return fixture;
}

int run_reconstruction_roundtrip(const fs::path& root) {
    constexpr std::uint64_t budget = 1024 * 1024;
    const fs::path leader_root = root / "reconstruction-leader";
    const fs::path worker_root = root / "reconstruction-worker";
    fs::create_directories(spirula::NativeFilesystemPath(leader_root));
    fs::create_directories(spirula::NativeFilesystemPath(worker_root));
    LeaderServer::Options options;
    options.state_root = leader_root;
    options.bind_address = "127.0.0.1";
    options.server_name = "leader.local";
    std::string error;
    auto server = LeaderServer::Start(options, &error);
    check(server && server->Running(),
          "starts a leader for whole-reconstruction loopback transfer");
    if (!server) return 1;

    auto worker = pairing::Worker::Open(worker_root, &error);
    auto invitation = server->IssueInvitation();
    check(worker.has_value() && invitation.has_value(),
          "creates a paired reconstruction worker identity");
    if (!worker || !invitation) return 1;
    const auto leader_pin = server->EnrollmentPeerFingerprint();
    RawSocket enrollment = connect_loopback(server->EnrollmentPort());
    const bool redeemed = enrollment != kBadSocket && worker->Redeem(
        native_socket(enrollment), invitation->code, leader_pin,
        "leader.local", "reconstruction loopback worker", &error);
    const std::string worker_id = worker->WorkerId();
    const bool paired = redeemed && server->Approve(worker_id, &error);
    check(paired,
          "approves the reconstruction worker through authenticated enrollment");
    if (!paired) return 1;
    enrollment = connect_loopback(server->EnrollmentPort());
    const bool approved = enrollment != kBadSocket && worker->CheckApproval(
        native_socket(enrollment), invitation->code, leader_pin,
        "leader.local", &error);
    check(approved, "worker verifies approval and receives its operational identity");
    if (!approved) return 1;

    auto channel = connect_worker(*worker, *server, &error);
    check(channel.has_value(),
          "connects the paired reconstruction worker over operational mTLS");
    if (!channel) return 1;
    ReconstructionWorker fake(std::move(*channel), worker_root / "artifacts", budget);
    check(fake.Start(),
          "worker advertises the exact build and supported AMD Vulkan capability");
    if (!wait_for_snapshot(*server, worker_id, true, true)) {
        check(false, "reconstruction worker becomes dispatch-eligible");
        return 1;
    }

    const auto success_fixture =
        make_reconstruction_fixture(root / "successful-source");
    check(server->SubmitReconstruction(worker_id, "reconstruction-success",
        success_fixture.request, success_fixture.manifest, budget, &error),
        "accepts a whole reconstruction into asynchronous portable staging");
    LeaderServer::ReconstructionJobSnapshot success;
    check(fake.WaitForInputs(1) &&
              wait_reconstruction_job(*server, "reconstruction-success",
                  LeaderServer::ReconstructionJobState::Running, &success),
          "transfers and verifies the exact reconstruction input bundle");
    check(success.paired_leader_id == worker->LeaderId() &&
              success.paired_leader_epoch == worker->LeaderEpoch() &&
              success.required_build == agent::ReconstructionBuildIdentity() &&
              success.source_manifest_sha256.size() == 64 &&
              success.input_identity_sha256.size() == 64,
          "records the paired leader epoch, build, and input provenance");
    fake.ReleaseNextResult();
    check(fake.WaitForCommitted(1) &&
              wait_reconstruction_job(*server, "reconstruction-success",
                  LeaderServer::ReconstructionJobState::Succeeded, &success),
          "commits only a transferred sparse model accepted by leader parsers");
    check(fs::is_regular_file(spirula::NativeFilesystemPath(
              success.result_root / "0/cameras.bin")) &&
              fs::is_regular_file(spirula::NativeFilesystemPath(
                  success.result_root / "0/images.bin")) &&
              fs::is_regular_file(spirula::NativeFilesystemPath(
                  success.result_root / "0/points3D.bin")),
          "keeps the verified model in the leader-owned result tree");

    const auto stale_fixture =
        make_reconstruction_fixture(root / "stale-source");
    check(server->SubmitReconstruction(worker_id, "reconstruction-stale",
        stale_fixture.request, stale_fixture.manifest, budget, &error),
        "submits a second portable reconstruction on the now-idle worker");
    LeaderServer::ReconstructionJobSnapshot stale;
    check(fake.WaitForInputs(2) &&
              wait_reconstruction_job(*server, "reconstruction-stale",
                  LeaderServer::ReconstructionJobState::Running, &stale),
          "stages and transfers the second reconstruction before publication");
    check(server->SupersedeReconstruction("reconstruction-stale", &error),
          "persists explicit supersession before a worker publishes late output");
    fake.ReleaseNextResult();
    check(fake.WaitForRejected(1),
          "rejects a stale reconstruction result before artifact transfer");
    check(wait_reconstruction_job(*server, "reconstruction-stale",
              LeaderServer::ReconstructionJobState::Superseded, &stale) &&
              !stale.current &&
              !fs::exists(spirula::NativeFilesystemPath(stale.result_root)) &&
              !fake.failed(),
          "superseded reconstruction cannot publish into the result root");
    fake.Stop();
    check(wait_for_snapshot(*server, worker_id, false, false) &&
              wait_reconstruction_job(*server, "reconstruction-success",
                  LeaderServer::ReconstructionJobState::Succeeded),
          "keeps a verified success terminal after its worker disconnects");
    server->Stop();
    server.reset();
    server = LeaderServer::Start(options, &error);
    check(server && server->Running(),
          "reopens the leader with its durable reconstruction records");
    if (server) {
        LeaderServer::ReconstructionJobSnapshot persisted_success;
        LeaderServer::ReconstructionJobSnapshot persisted_stale;
        check(wait_reconstruction_job(*server, "reconstruction-success",
                  LeaderServer::ReconstructionJobState::Succeeded,
                  &persisted_success) &&
                  fs::is_regular_file(spirula::NativeFilesystemPath(
                      persisted_success.result_root / "0/cameras.bin")) &&
                  wait_reconstruction_job(*server, "reconstruction-stale",
                      LeaderServer::ReconstructionJobState::Superseded,
                      &persisted_stale) &&
                  !persisted_stale.current &&
                  !fs::exists(spirula::NativeFilesystemPath(
                      persisted_stale.result_root)),
              "rehydrates the verified result and supersede fence after restart");
        server->Stop();
    }
    return 0;
}
#endif

#ifdef SS_TOOL_TRAIN
void diagnose_training_job(LeaderServer& server, const std::string& job_id) {
    for (const auto& job : server.TrainingJobs()) {
        if (job.job_id != job_id) continue;
        std::fprintf(stderr,
            "diag training job=%s attempt=%s state=%d current=%d error=%s result=%s checkpoint=%s returned=%s resume=%s data=%s\n",
            job.job_id.c_str(), job.attempt_id.c_str(),
            static_cast<int>(job.state), job.current, job.error.c_str(),
            job.result_root.string().c_str(),
            job.checkpoint_dir.string().c_str(),
            job.returned_checkpoint.c_str(),
            job.resume_config.resume.c_str(),
            job.resume_config.data.c_str());
    }
    for (const auto& worker : server.Snapshot())
        if (!worker.error.empty())
            std::fprintf(stderr, "diag training worker=%s connected=%d error=%s\n",
                worker.id.c_str(), worker.connected, worker.error.c_str());
}

int run_training_roundtrip(const fs::path& root) {
    constexpr std::uint64_t budget = 64u << 20;
    const fs::path leader_root = root / "training-leader";
    const fs::path worker_root = root / "training-worker";
    fs::create_directories(spirula::NativeFilesystemPath(leader_root));
    fs::create_directories(spirula::NativeFilesystemPath(worker_root));
    LeaderServer::Options options;
    options.state_root = leader_root;
    options.bind_address = "127.0.0.1";
    options.server_name = "leader.local";
    std::string error;
    auto server = LeaderServer::Start(options, &error);
    check(server && server->Running(),
          "starts a training leader with portable job persistence");
    if (!server) return 1;

    auto worker = pairing::Worker::Open(worker_root, &error);
    auto invitation = server->IssueInvitation();
    check(worker.has_value() && invitation.has_value(),
          "creates and enrolls a training worker identity");
    if (!worker || !invitation) return 1;
    const auto leader_pin = server->EnrollmentPeerFingerprint();
    RawSocket enrollment = connect_loopback(server->EnrollmentPort());
    const bool redeemed = enrollment != kBadSocket && worker->Redeem(
        native_socket(enrollment), invitation->code, leader_pin,
        "leader.local", "training loopback worker", &error);
    const std::string worker_id = worker->WorkerId();
    const bool paired = redeemed && server->Approve(worker_id, &error);
    check(paired, "approves the training worker through authenticated enrollment");
    if (!paired) return 1;
    enrollment = connect_loopback(server->EnrollmentPort());
    const bool approved = enrollment != kBadSocket && worker->CheckApproval(
        native_socket(enrollment), invitation->code, leader_pin,
        "leader.local", &error);
    check(approved, "worker confirms its durable training pairing");
    if (!approved) return 1;

    auto channel = connect_worker(*worker, *server, &error);
    check(channel.has_value(), "connects the training worker over operational mTLS");
    if (!channel) return 1;
    TrainingWorker interrupted(std::move(*channel), worker_root / "artifacts", budget);
    check(interrupted.Start(),
          "advertises exact-build Training capability on AMD Vulkan");
    if (!wait_for_snapshot(*server, worker_id, true, true)) {
        check(false, "training worker becomes dispatch eligible");
        return 1;
    }

    const auto fixture = make_training_fixture(root / "success-source");
    check(server->SubmitTraining(worker_id, "training-success",
              fixture.config, "3dgs", {}, budget, &error),
          "accepts input staging asynchronously without invoking the trainer");
    LeaderServer::TrainingJobSnapshot unknown;
    check(interrupted.WaitForInputs(1) &&
              wait_training_job(*server, "training-success",
                  LeaderServer::TrainingJobState::Running, &unknown),
          "transfers and verifies the staged training inputs");
    check(unknown.paired_leader_id == worker->LeaderId() &&
              unknown.paired_leader_epoch == worker->LeaderEpoch() &&
              unknown.required_build == training_worker_status().build &&
              unknown.input_identity_sha256.size() == 64 &&
              unknown.current,
          "persists job identity, paired leader epoch, exact build and input identity");

    interrupted.Stop();
    check(wait_for_snapshot(*server, worker_id, false, false) &&
              wait_training_job(*server, "training-success",
                  LeaderServer::TrainingJobState::Unknown, &unknown),
          "records worker disconnect as Unknown without losing the attempt");
    const std::string attempt_id = unknown.attempt_id;
    server->Stop();
    server.reset();
    server = LeaderServer::Start(options, &error);
    check(server && server->Running() &&
              wait_training_job(*server, "training-success",
                  LeaderServer::TrainingJobState::Unknown, &unknown) &&
              unknown.attempt_id == attempt_id,
          "rehydrates the same Unknown attempt after leader restart");
    if (!server) return 1;

    channel = connect_worker(*worker, *server, &error);
    check(channel.has_value(), "reconnects the paired worker for same-attempt replay");
    if (!channel) return 1;
    TrainingWorker replay(std::move(*channel), worker_root / "artifacts", budget);
    check(replay.Start(), "accepts a replayed portable training offer");
    LeaderServer::TrainingJobSnapshot success;
    check(replay.WaitForInputs(1) &&
              wait_training_job(*server, "training-success",
                  LeaderServer::TrainingJobState::Running, &success) &&
              success.attempt_id == attempt_id,
          "replays inputs under the unchanged durable attempt ID");
    replay.ReleaseNextResult();
    check(replay.WaitForCommitted(1) &&
              wait_training_job(*server, "training-success",
                  LeaderServer::TrainingJobState::Succeeded, &success),
          "verifies and commits the portable resumable training output");
    const bool gui_resume_rebound =
        fs::is_regular_file(spirula::NativeFilesystemPath(
            success.result_root / "step-000000003.ckpt" / "state.tar")) &&
        success.returned_checkpoint == "step-000000003.ckpt" &&
        success.checkpoint_dir ==
            success.result_root / success.returned_checkpoint &&
        fs::is_directory(spirula::NativeFilesystemPath(
            fs::u8path(success.resume_config.data)));
    if (!gui_resume_rebound)
        diagnose_training_job(*server, "training-success");
    check(gui_resume_rebound,
          "exposes the leader-local checkpoint and dataset rebound to the GUI");
    const TrainConfig cli_resume =
        ckpt::build_resume_config(success.resume_config, "3dgs", {});
    check(cli_resume.resume == success.checkpoint_dir.u8string() &&
              cli_resume.data == success.resume_config.data,
          "returned config opens through the native resume path");

    TrainConfig next_config = success.resume_config;
    next_config.resume.clear();
    const fs::path resume_workspace = root / "resume-attempt";
    fs::create_directories(spirula::NativeFilesystemPath(resume_workspace));
    const auto resumed = agent::StageTrainingInputs(
        next_config, "3dgs", success.checkpoint_dir, resume_workspace, budget);
    const auto resumed_verified = agent::VerifyTrainingInputs(
        resumed.root, resumed.files, resumed.identity_sha256, budget);
    check(resumed_verified.mode == agent::TrainingMode::Resume &&
              agent::MakeTrainingInvocation(resumed_verified).mode ==
                  agent::TrainingMode::Resume,
          "returned checkpoint can be staged for a later native training resume");

    const auto stale_fixture = make_training_fixture(root / "stale-source");
    check(server->SubmitTraining(worker_id, "training-stale",
              stale_fixture.config, "3dgs", {}, budget, &error),
          "submits another portable job after the verified success");
    LeaderServer::TrainingJobSnapshot stale;
    check(replay.WaitForInputs(2) &&
              wait_training_job(*server, "training-stale",
                  LeaderServer::TrainingJobState::Running, &stale),
          "transfers inputs for a second training attempt before supersession");
    check(server->SupersedeTraining("training-stale", &error),
          "durably supersedes the active training attempt");
    replay.ReleaseNextResult();
    check(replay.WaitForRejected(1) &&
              wait_training_job(*server, "training-stale",
                  LeaderServer::TrainingJobState::Superseded, &stale) &&
              !stale.current &&
              !fs::exists(spirula::NativeFilesystemPath(stale.result_root)) &&
              !replay.failed(),
          "rejects stale training output before artifact transfer or publication");
    replay.Stop();
    check(wait_for_snapshot(*server, worker_id, false, false),
          "closes the replay worker cleanly");

    server->Stop();
    server.reset();
    server = LeaderServer::Start(options, &error);
    check(server && server->Running(),
          "reopens durable training attempts and verified artifacts");
    if (server) {
        LeaderServer::TrainingJobSnapshot persisted_success;
        LeaderServer::TrainingJobSnapshot persisted_stale;
        const bool success_rehydrated = wait_training_job(*server,
            "training-success", LeaderServer::TrainingJobState::Succeeded,
            &persisted_success);
        const bool stale_rehydrated = wait_training_job(*server,
            "training-stale", LeaderServer::TrainingJobState::Superseded,
            &persisted_stale);
        const bool training_rehydrated = success_rehydrated &&
            fs::is_regular_file(spirula::NativeFilesystemPath(
                persisted_success.checkpoint_dir / "state.tar")) &&
            persisted_success.returned_checkpoint == "step-000000003.ckpt" &&
            persisted_success.resume_config.resume ==
                persisted_success.checkpoint_dir.u8string() &&
            fs::is_directory(spirula::NativeFilesystemPath(
                fs::u8path(persisted_success.resume_config.data))) &&
            stale_rehydrated && !persisted_stale.current &&
            !fs::exists(spirula::NativeFilesystemPath(
                persisted_stale.result_root));
        if (!training_rehydrated) {
            diagnose_training_job(*server, "training-success");
            diagnose_training_job(*server, "training-stale");
        }
        check(training_rehydrated,
              "rehydrates the committed resume bundle and supersession fence");
        server->Stop();
    }
    return 0;
}
#endif

int run(const fs::path& root) {
    const fs::path leader_root = root / "leader";
    const fs::path worker_root = root / "worker";
    fs::create_directories(spirula::NativeFilesystemPath(leader_root));
    fs::create_directories(spirula::NativeFilesystemPath(worker_root));
    LeaderServer::Options options;
    options.state_root = leader_root;
    options.bind_address = "127.0.0.1";
    options.server_name = "leader.local";

    std::string error;
    auto server = LeaderServer::Start(options, &error);
    check(server && server->Running(), "starts the headless leader on private loopback listeners");
    if (!server) return 1;
    check(server->EnrollmentPort() != server->OperationalPort(),
          "keeps enrollment and operational listeners on distinct ports");

    auto worker = pairing::Worker::Open(worker_root, &error);
    auto invitation = server->IssueInvitation();
    check(worker.has_value() && invitation.has_value(),
          "creates worker identity and issues enrollment invitation");
    if (!worker || !invitation) return 1;
    const auto leader_pin = server->EnrollmentPeerFingerprint();
    check(leader_pin != agent::TlsChannel::PeerFingerprint{},
          "retains the enrolled leader identity for worker pin verification");
    if (leader_pin == agent::TlsChannel::PeerFingerprint{}) return 1;
    const std::string update_signer_fingerprint =
        server->UpdateSignerFingerprint();
    std::string enrollment_fingerprint;
    static constexpr char hex[] = "0123456789abcdef";
    enrollment_fingerprint.reserve(leader_pin.size() * 2);
    for (const auto byte : leader_pin) {
        enrollment_fingerprint.push_back(hex[byte >> 4]);
        enrollment_fingerprint.push_back(hex[byte & 15]);
    }
    check(update_signer_fingerprint.size() == 64 &&
              update_signer_fingerprint.find_first_not_of(
                  "0123456789abcdef") == std::string::npos &&
              update_signer_fingerprint != enrollment_fingerprint,
          "initializes an independent 64-hex update signer after Start");

    RawSocket enrollment = connect_loopback(server->EnrollmentPort());
    const bool redeemed = enrollment != kBadSocket && worker->Redeem(
        native_socket(enrollment), invitation->code, leader_pin, "leader.local",
        "loopback worker", &error);
    check(redeemed, "enrolls a worker through the separate authenticated enrollment listener");
    if (!redeemed) return 1;
    const std::string worker_id = worker->WorkerId();
    check(server->Approve(worker_id, &error), "approves the pending worker");
    enrollment = connect_loopback(server->EnrollmentPort());
    const bool approved = enrollment != kBadSocket && worker->CheckApproval(
        native_socket(enrollment), invitation->code, leader_pin, "leader.local", &error);
    check(approved && worker->Status() == pairing::WorkerStatus::Paired,
          "worker verifies approval and receives its operational certificate");
    if (!approved) return 1;

    // A paired worker that fails its initial status exchange never becomes Ready.
    auto malformed_channel = connect_worker(*worker, *server, &error);
    check(malformed_channel.has_value(), "authenticates a paired operational TLS client");
    if (malformed_channel) {
        const std::string malformed = "not a wire message";
        malformed_channel->SendFrame(
            reinterpret_cast<const std::uint8_t*>(malformed.data()), malformed.size());
        malformed_channel->Close();
    }
    check(wait_for_error(*server, worker_id),
          "malformed operational traffic leaves the worker down and not Ready");

    auto channel = connect_worker(*worker, *server, &error);
    check(channel.has_value(), "authenticates the real paired worker over operational mTLS");
    if (!channel) return 1;
    const std::string leader_id = worker->LeaderId();
    const std::uint64_t leader_epoch = worker->LeaderEpoch();
    auto fake_memory = std::make_shared<FakeWorkerMemory>();
    FakeWorker fake(std::move(*channel), leader_id, leader_epoch, fake_memory);
    check(fake.Start(), "exchanges initial worker and leader status frames");
    check(wait_for_snapshot(*server, worker_id, false, true),
          "publishes authenticated identity and status axes while jobs remain undispatchable");
    const auto ready_snapshot = server->Snapshot();
    bool readiness_hidden = false;
    for (const auto& item : ready_snapshot) {
        if (item.id == worker_id)
            readiness_hidden = item.connected && !item.ready &&
                item.status.compatibility == wire::CompatibilityState::Unknown &&
                item.status.capabilities.empty() &&
                item.status.scheduling == wire::SchedulingState::Stopped &&
                item.last_seen_unix_ms != 0;
    }
    check(readiness_hidden, "snapshot never advertises undispatchable worker capabilities");

    auto applied = server->SendCommand(worker_id, wire::CommandAction::Maintenance,
                                       std::chrono::seconds(5), &error);
    check(applied && applied->state == LeaderServer::CommandState::Applied &&
              applied->acknowledgment == wire::AcknowledgmentOutcome::Completed &&
              applied->acknowledgment_received && fake.effects() == 1 &&
              fake.maintenance() && !fake.failed(),
          "dispatches and durably records a sequenced terminal command");
    if (!applied) return 1;

    fake.ConfigureAsync(false, true);
    auto pause = server->SendCommand(worker_id, wire::CommandAction::Pause,
                                     std::chrono::seconds(3), &error);
    check(pause && pause->state == LeaderServer::CommandState::Pending &&
              pause->acknowledgment == wire::AcknowledgmentOutcome::Accepted &&
              pause->acknowledgment_received && fake.effects() == 2 &&
              !fake.paused(),
          "returns Accepted as Pending without claiming Pause completed");
    if (!pause) return 1;
    check(wait_for_command_state(*server, pause->command_id,
                                 LeaderServer::CommandState::Pending) &&
              wait_for_scheduling(*server, worker_id,
                                  wire::SchedulingState::Pausing),
          "publishes worker-reported Pausing while the safe point is pending");
    std::this_thread::sleep_for(std::chrono::milliseconds(3100));
    const auto command_count = server->CommandSnapshots().size();
    const auto blocked_resume = server->SendCommand(
        worker_id, wire::CommandAction::Resume, std::chrono::seconds(5), &error);
    check(!blocked_resume && server->CommandSnapshots().size() == command_count &&
              wait_for_command_state(*server, pause->command_id,
                                     LeaderServer::CommandState::Pending),
          "does not let Resume supersede an Accepted Pause");
    fake.CompletePending();
    check(wait_for_command_state(*server, pause->command_id,
                                 LeaderServer::CommandState::Applied) &&
              wait_for_scheduling(*server, worker_id,
                                  wire::SchedulingState::Paused) &&
              fake.paused() && !fake.failed(),
          "publishes Paused only after the worker's terminal safe-point ACK");

    auto unconfirmed_stop = server->SendCommand(
        worker_id, wire::CommandAction::Stop, std::chrono::seconds(5), &error);
    auto unconfirmed_all_stop = server->SendCommand(
        worker_id, wire::CommandAction::Stop, std::string{}, false,
        std::chrono::seconds(5), &error);
    check(!unconfirmed_stop && !unconfirmed_all_stop,
          "rejects Stop-all unless the caller explicitly confirms");
    const auto history_before_restart_guards =
        server->CommandSnapshots().size();
    const int deliveries_before_restart_guards = fake.deliveries();
    const auto legacy_force_restart = server->SendCommand(
        worker_id, wire::CommandAction::ForceRestartService,
        std::chrono::seconds(5), &error);
    const auto unconfirmed_force_restart = server->SendCommand(
        worker_id, wire::CommandAction::ForceRestartService,
        std::string{}, false,
        std::chrono::seconds(5), &error);
    const auto confirmed_safe_restart = server->SendCommand(
        worker_id, wire::CommandAction::RestartService, std::string{}, true,
        std::chrono::seconds(5), &error);
    const auto targeted_force_restart = server->SendCommand(
        worker_id, wire::CommandAction::ForceRestartService,
        "job-target-1", true,
        std::chrono::seconds(5), &error);
    check(!legacy_force_restart && !unconfirmed_force_restart &&
              !confirmed_safe_restart && !targeted_force_restart &&
              server->CommandSnapshots().size() == history_before_restart_guards &&
              fake.deliveries() == deliveries_before_restart_guards,
          "requires confirmation only for targetless force restart and "
          "never queues invalid restart commands");

    fake.PauseHeartbeats();
    auto expired = server->SendCommand(worker_id, wire::CommandAction::Pause,
                                       std::chrono::milliseconds(1), &error);
    check(expired && expired->state == LeaderServer::CommandState::Expired &&
              fake.effects() == 2 && fake.maintenance(),
          "expires an unsent command without applying or replaying its effect");
    if (!expired) return 1;
    const int deliveries_before_expired_pause = fake.deliveries();
    fake.ResumeHeartbeats();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(fake.deliveries() == deliveries_before_expired_pause,
          "does not deliver an expired command on a later heartbeat");

    const int deliveries_before_expired_stop = fake.deliveries();
    fake.ConfigureAsync(true, false);
    auto stop_pending = server->SendCommand(
        worker_id, wire::CommandAction::Stop, "job-target-1", true,
        std::chrono::seconds(5), &error);
    check(stop_pending &&
              stop_pending->state == LeaderServer::CommandState::Pending &&
              stop_pending->acknowledgment ==
                  wire::AcknowledgmentOutcome::Accepted &&
              stop_pending->target_job_id == "job-target-1" &&
              stop_pending->confirmed && fake.effects() == 3,
          "accepts a confirmed target Stop as pending");
    if (!stop_pending) return 1;
    const std::string stop_command_id = stop_pending->command_id;
    const std::uint64_t stop_sequence = stop_pending->sequence;
    const int stop_delivery_count = fake.deliveries();
    check(fake.WaitDisconnected() &&
              wait_for_snapshot(*server, worker_id, false, false),
          "keeps the accepted Stop live after its worker disconnects");
    fake.Stop();
    server->Stop();
    server.reset();

    server = LeaderServer::Start(options, &error);
    check(server && server->Running(),
          "reopens the same leader identity and persisted command state");
    if (!server) return 1;
    check(server->UpdateSignerFingerprint() == update_signer_fingerprint,
          "loads the same dedicated signer after explicit leader restart");
    channel = connect_worker(*worker, *server, &error);
    check(channel.has_value(), "reconnects the paired worker after leader restart");
    if (!channel) return 1;
    FakeWorker after_restart(std::move(*channel), leader_id, leader_epoch,
                             fake_memory);
    after_restart.ConfigureAsync(false, true);
    check(after_restart.Start(), "starts the post-restart status exchange");
    check(after_restart.WaitForCommandDeliveries(stop_delivery_count + 1),
          "reoffers the identical live Stop after reconnect and leader restart");
    const wire::Command replayed_stop = after_restart.LastCommand();
    check(replayed_stop.command_id == stop_command_id &&
              replayed_stop.sequence == stop_sequence &&
              replayed_stop.action == wire::CommandAction::Stop &&
              replayed_stop.target_job_id == "job-target-1" &&
              replayed_stop.confirmed && after_restart.effects() == 3,
          "replay preserves command identity and does not repeat its effect");
    bool persisted_stop_pending = false;
    for (const auto& command : server->CommandSnapshots()) {
        if (command.command_id == stop_command_id)
            persisted_stop_pending =
                command.state == LeaderServer::CommandState::Pending &&
                command.target_job_id == "job-target-1" &&
                command.confirmed && command.acknowledgment_received;
    }
    check(persisted_stop_pending,
          "restores the persisted command status, target, and confirmation");

    after_restart.CompletePending();
    check(wait_for_command_state(*server, stop_command_id,
                                 LeaderServer::CommandState::Applied) &&
              wait_for_scheduling(*server, worker_id,
                                  wire::SchedulingState::Stopped) &&
              !after_restart.failed(),
          "completes the reconnected Stop and accepts duplicate terminal ACKs");

    after_restart.ConfigureAsync(false, false);
    auto resumed = server->SendCommand(worker_id, wire::CommandAction::Pause,
                                       std::chrono::seconds(5), &error);
    check(resumed && resumed->state == LeaderServer::CommandState::Pending &&
              resumed->acknowledgment ==
                  wire::AcknowledgmentOutcome::Accepted &&
              resumed->sequence > expired->sequence &&
              after_restart.effects() == 4,
          "keeps post-restart Pause Pending until its safe point completes");
    if (!resumed) return 1;
    check(wait_for_scheduling(*server, worker_id, wire::SchedulingState::Pausing),
          "publishes Pausing for the post-restart worker status");

    const int cycles_before_replay = after_restart.heartbeat_cycles();
    after_restart.ReplayAcknowledgment(wire::Acknowledgment{
        stop_command_id, leader_id, leader_epoch, stop_sequence,
        wire::CommandAction::Stop, wire::AcknowledgmentOutcome::Completed});
    check(after_restart.WaitForAcknowledgmentReplays(1) &&
              after_restart.WaitForHeartbeatCycles(cycles_before_replay + 2) &&
              wait_for_command_state(*server, resumed->command_id,
                                     LeaderServer::CommandState::Pending) &&
              !after_restart.failed(),
          "accepts a retained terminal ACK idempotently during a newer command");
    after_restart.CompletePending();
    check(wait_for_command_state(*server, resumed->command_id,
                                 LeaderServer::CommandState::Applied) &&
              wait_for_scheduling(*server, worker_id,
                                  wire::SchedulingState::Paused) &&
              after_restart.paused() && !after_restart.failed(),
          "publishes Paused only after the newer command completes");

    after_restart.PauseHeartbeats();
    auto expired_stop = server->SendCommand(
        worker_id, wire::CommandAction::Stop, std::string{}, true,
        std::chrono::milliseconds(1), &error);
    check(expired_stop &&
              expired_stop->state == LeaderServer::CommandState::Expired &&
              expired_stop->target_job_id.empty() &&
              expired_stop->confirmed && after_restart.effects() == 4,
          "records confirmed Stop-all but never applies it after TTL expiry");
    if (!expired_stop) return 1;
    after_restart.ResumeHeartbeats();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(after_restart.deliveries() == deliveries_before_expired_stop + 3 &&
              after_restart.effects() == 4 &&
              wait_for_command_state(*server, expired_stop->command_id,
                                     LeaderServer::CommandState::Expired),
          "never redelivers the expired confirmed Stop-all");

    const fs::path update_source = root / "leader-update" / "approved.pkg";
    const std::string update_bytes = "leader-approved-update-payload\n";
    if (!write_native_file(update_source, update_bytes.data(),
                           update_bytes.size()))
        return 1;
    spirula::Sha256 update_hash;
    update_hash.update(
        reinterpret_cast<const std::uint8_t*>(update_bytes.data()),
        update_bytes.size());
    const std::string update_digest = update_hash.hex();
    {
        std::lock_guard<std::mutex> lock(fake_memory->mutex);
        fake_memory->expected_worker_id = worker_id;
        fake_memory->expected_package_bytes = update_bytes;
        fake_memory->admin_transfer_root = root / "fake-worker-update";
    }
    agent::update::PackageManifest manifest;
    manifest.os = worker_status().platform;
    manifest.architecture = "x86_64";
    manifest.build = "leader-test-update";
    manifest.release = "m5-test-1";
    manifest.size = 0;
    manifest.sha256.clear();
    error.clear();
    const auto update = server->RequestUpdate(
        worker_id, update_source, manifest, 42, true, &error);
    const std::string request_error = error;
    LeaderServer::CommandResult final_update;
    const bool found_final_update = update &&
        wait_for_final_command(*server, update->command_id, final_update);
    const bool update_applied = found_final_update &&
        final_update.state == LeaderServer::CommandState::Applied;
    const wire::Command update_command = after_restart.LastCommand();
    std::string received_update;
    std::string worker_update_error;
    bool update_transfer_verified = false;
    {
        std::lock_guard<std::mutex> lock(fake_memory->mutex);
        update_transfer_verified = fake_memory->admin_update_verified;
        received_update = fake_memory->admin_received_bytes;
        worker_update_error = fake_memory->failure_reason;
    }
    const bool update_binding_valid = update && update_applied &&
        found_final_update && final_update.command_id == update->command_id &&
        final_update.state == LeaderServer::CommandState::Applied &&
        final_update.acknowledgment == wire::AcknowledgmentOutcome::Completed &&
        final_update.acknowledgment_received &&
        update_command.action == wire::CommandAction::ActivateUpdate &&
        update_command.confirmed && update_command.admin_intent &&
        update_command.package_offer &&
        update_command.admin_intent->signature.size() != 0 &&
        update_command.package_offer->signature.size() != 0 &&
        update_command.admin_intent->signer_public_key_pem ==
            update_command.package_offer->signer_public_key_pem &&
        update_command.admin_intent->worker_id == worker_id &&
        update_command.admin_intent->leader_id == leader_id &&
        update_command.admin_intent->leader_epoch == leader_epoch &&
        update_command.admin_intent->intent_id == update_command.command_id &&
        update_command.admin_intent->update_id == update_command.command_id &&
        update_command.admin_intent->package_sha256 == update_digest &&
        update_command.admin_intent->security_version == 42 &&
        update_command.package_offer->worker_id == worker_id &&
        update_command.package_offer->leader_id == leader_id &&
        update_command.package_offer->update_id == update_command.command_id &&
        update_command.admin_intent->expires_at_unix ==
            update_command.package_offer->expires_at_unix &&
        update_command.package_offer->manifest.os ==
            worker_status().platform &&
        update_command.package_offer->manifest.architecture == "x86_64" &&
        update_command.package_offer->manifest.size == update_bytes.size() &&
        update_command.package_offer->manifest.sha256 == update_digest &&
        !after_restart.failed() &&
        update_transfer_verified && received_update == update_bytes;
    if (!update_binding_valid)
        std::fprintf(stderr,
            "diag admin update requested=%d final=%d state=%d ack=%d "
            "ack_received=%d worker_failed=%d transfer_verified=%d "
            "received_bytes=%zu expected_bytes=%zu request_error=%s "
            "worker_error=%s\n",
            update.has_value(), found_final_update,
            static_cast<int>(final_update.state),
            static_cast<int>(final_update.acknowledgment),
            final_update.acknowledgment_received, after_restart.failed(),
            update_transfer_verified, received_update.size(), update_bytes.size(),
            request_error.c_str(), worker_update_error.c_str());
    check(update_binding_valid,
          "derives and signs the update digest, then transfers exact bytes to the paired target");
    if (!update_binding_valid) return 1;

    RawSocket stalled_enrollment = connect_loopback(server->EnrollmentPort());
    check(stalled_enrollment != kBadSocket,
          "opens an unauthenticated enrollment socket for cancellation checks");
    if (stalled_enrollment != kBadSocket)
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto revoke_started = std::chrono::steady_clock::now();
    const bool revocation_persisted = server->Revoke(worker_id, &error);
    const auto revoke_elapsed = std::chrono::steady_clock::now() - revoke_started;
    check(stalled_enrollment != kBadSocket && revocation_persisted &&
              revoke_elapsed < std::chrono::seconds(2),
          "revokes an active worker without waiting for unauthenticated enrollment TLS");
    check(after_restart.WaitDisconnected(),
          "revocation closes the active operational TLS channel");
    after_restart.Stop();
    auto rejected_reconnect = connect_worker(*worker, *server, &error);
    check(!rejected_reconnect, "revoked identity cannot reconnect without reapproval");
    check(wait_for_snapshot(*server, worker_id, false, false),
          "revoked worker remains disconnected and not Ready");
    bool revoked = false;
    for (const auto& item : server->Snapshot())
        if (item.id == worker_id) revoked = item.revoked && !item.connected && !item.ready;
    check(revoked, "snapshot reports the revoked worker as disconnected, not failed");
    const auto stop_started = std::chrono::steady_clock::now();
    server->Stop();
    const auto stop_elapsed = std::chrono::steady_clock::now() - stop_started;
    if (stop_elapsed >= std::chrono::seconds(2))
        std::fprintf(stderr, "leader Stop with pending enrollment took %lld ms\n",
            static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    stop_elapsed).count()));
    check(stalled_enrollment != kBadSocket && stop_elapsed < std::chrono::seconds(2),
          "Stop interrupts an accepted enrollment handshake promptly");
    close_socket(stalled_enrollment);
    return failures ? 1 : 0;
}

}  // namespace

int main() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("spirula-agent-leader-" + std::to_string(nonce));
    try {
        NetworkScope network;
        fs::create_directories(spirula::NativeFilesystemPath(root));
        run(root);
#ifdef SS_TOOL_SFM
        run_feature_roundtrip(root / "feature");
        run_reconstruction_roundtrip(root / "reconstruction");
#endif
#ifdef SS_TOOL_TRAIN
        run_training_roundtrip(root / "training");
#endif
        std::error_code error;
        fs::remove_all(spirula::NativeFilesystemPath(root), error);
        if (error) check(false, "removes temporary leader state");
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::printf("FAIL leader scenario: %s\n", error.what());
        std::error_code cleanup_error;
        fs::remove_all(spirula::NativeFilesystemPath(root), cleanup_error);
        return 1;
    }
}
