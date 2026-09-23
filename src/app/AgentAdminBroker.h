#pragma once

#include "app/AgentAdminIntent.h"
#include "app/AgentServiceManager.h"
#include "app/AgentUpdatePackage.h"

#include <cstdint>
#include <string>
namespace app::agent {
namespace wire { struct Status; }
}
namespace app::agent::admin {

struct TargetBinding {
    std::string worker_id;
    std::string leader_id;
    std::uint64_t leader_epoch = 0;
    std::uint64_t initial_security_version = 0;
};
enum class IntentOutcomeState : std::uint8_t {
    Unknown,
    Pending,
    Committed,
    RolledBack,
    Failed,
};

struct IntentOutcome {
    IntentOutcomeState state = IntentOutcomeState::Unknown;
    std::string update_id;
    std::uint64_t security_version = 0;
    std::string detail;
};


// An elevated local-operator action. Creates the protected LocalSystem broker
// service and pins its initial worker/leader/epoch grant; it never starts it.
service::Result install_broker(const TargetBinding& binding);
service::Result uninstall_broker();

// Worker-side bounded named-pipe request. Activation requires an original
// signed offer and a broker-verified post-initialization health report.
service::Result submit_intent(const Intent& intent,
                              const update::PackageOffer* offer = nullptr);
// Queries only the broker-owned durable result; callers must not resubmit a
// side effect when the returned state is pending.
service::Result query_intent_outcome(const std::string& intent_id,
                                     IntentOutcome& outcome);

// Called only by the installed restricted worker's readiness callback.
service::Result report_worker_ready(const std::string& activation_id,
                                    const wire::Status& status);

// SCM entrypoint for the separate protected broker executable.
int run_broker_service();

}  // namespace app::agent::admin
