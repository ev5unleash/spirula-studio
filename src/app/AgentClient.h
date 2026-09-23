#pragma once

#include "app/AgentConfig.h"
#include "app/AgentState.h"


namespace app::agent {
namespace wire { struct Status; }

using WorkerReadyCallback = bool (*)(const wire::Status&, void*) noexcept;

using StopRequested = bool (*)(void*) noexcept;

enum class HostExit { Stopped, RestartRequested };

// Runs the paired outbound control plane until stop is requested or an
// authenticated restart command is durably quiesced.
HostExit RunAgentClient(const Config& config,
                        const std::filesystem::path& state_root,
                        const std::filesystem::path& storage_root,
                        bool machine_managed, State& state,
                        StopRequested stop_requested, void* stop_context,
                        WorkerReadyCallback ready_callback = nullptr,
                        void* ready_context = nullptr);

}  // namespace app::agent
