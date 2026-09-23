#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace app::agent {
struct Config;
namespace pairing {
class Leader;
}
}

namespace app::agent::admin {

enum class Operation : std::uint8_t {
    Reboot = 1,
    Activate = 2,
};

struct Intent {
    Operation operation = Operation::Reboot;
    std::string worker_id;
    std::string leader_id;
    std::uint64_t leader_epoch = 0;
    std::string intent_id;
    std::uint64_t issued_at_unix = 0;
    std::uint64_t expires_at_unix = 0;
    std::string update_id;
    std::string package_sha256;
    std::uint64_t security_version = 0;
    std::string signer_public_key_pem;
    std::vector<std::uint8_t> signature;
};

// Signs only a valid, time-bounded intent for an approved worker. The supplied
// leader identity and epoch must match the signing pairing state.
std::optional<Intent> SignIntent(pairing::Leader& leader, Intent intent,
                                 std::string* error = nullptr);

// Checks local operation grant, paired-leader identity, admin-pinned update
// signer, signature, operation fields, and time bounds. Worker/epoch grants and
// replay protection remain the broker's protected-state responsibility.
bool VerifyIntent(const Config& policy, const Intent& intent,
                  std::uint64_t now_unix, std::string* error = nullptr);

}  // namespace app::agent::admin
