#include "app/AgentAdminIntent.h"

#include "app/AgentConfig.h"
#include "app/AgentPairing.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <limits>
#include <string_view>
#include <utility>

namespace app::agent::admin {
namespace {
using Fingerprint = TlsChannel::PeerFingerprint;

constexpr std::uint64_t kMaxIntentLifetimeSeconds = 5 * 60;
constexpr std::uint64_t kMaxFutureIssueSkewSeconds = 30;
constexpr std::uint64_t kMaxLeaderEpoch = (std::uint64_t{1} << 53) - 1;
constexpr std::size_t kMaxIdentifierBytes = 64;
constexpr std::size_t kMaxPublicKeyPemBytes = 16 * 1024;
constexpr std::size_t kMaxSignatureBytes = 256;
constexpr std::size_t kMaxCanonicalBytes = 4096;

void SetError(std::string* error, std::string_view message) {
    if (error) error->assign(message.data(), message.size());
}

bool Reject(std::string* error, std::string_view message) {
    SetError(error, message);
    return false;
}

bool ValidSha256(std::string_view value) {
    if (value.size() != 64) return false;
    for (char ch : value)
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
    return true;
}

bool ValidIdentifier(std::string_view value) {
    const auto alphanumeric = [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9');
    };
    if (value.empty() || value.size() > kMaxIdentifierBytes ||
        !alphanumeric(static_cast<unsigned char>(value.front())))
        return false;
    for (unsigned char ch : value)
        if (!alphanumeric(ch) && ch != '-' && ch != '_' && ch != '.')
            return false;
    return true;
}

bool DecodeSha256(std::string_view value, Fingerprint& out) {
    if (!ValidSha256(value)) return false;
    const auto digit = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int high = digit(value[2 * i]);
        const int low = digit(value[2 * i + 1]);
        out[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

bool ValidFields(const Intent& intent, std::string* error) {
    if (intent.operation != Operation::Reboot &&
        intent.operation != Operation::Activate)
        return Reject(error, "admin intent operation is invalid");
    if (!ValidSha256(intent.worker_id) || !ValidSha256(intent.leader_id) ||
        !intent.leader_epoch || intent.leader_epoch > kMaxLeaderEpoch ||
        !ValidIdentifier(intent.intent_id))
        return Reject(error, "admin intent identity is invalid");
    if (intent.operation == Operation::Reboot) {
        if (!intent.update_id.empty() || !intent.package_sha256.empty() ||
            intent.security_version)
            return Reject(error, "reboot intent contains activation fields");
    } else if (!ValidIdentifier(intent.update_id) ||
               !ValidSha256(intent.package_sha256) || !intent.security_version) {
        return Reject(error, "activation intent fields are invalid");
    }
    return true;
}

bool ValidTimes(const Intent& intent, std::uint64_t now_unix,
                std::string* error) {
    if (!now_unix || !intent.issued_at_unix || !intent.expires_at_unix ||
        intent.expires_at_unix <= intent.issued_at_unix ||
        intent.expires_at_unix - intent.issued_at_unix >
            kMaxIntentLifetimeSeconds)
        return Reject(error, "admin intent time bounds are invalid");
    if (intent.issued_at_unix > now_unix &&
        intent.issued_at_unix - now_unix > kMaxFutureIssueSkewSeconds)
        return Reject(error, "admin intent issue time is too far in the future");
    if (intent.expires_at_unix <= now_unix)
        return Reject(error, "admin intent has expired");
    return true;
}

bool AppendString(std::vector<std::uint8_t>& bytes, std::string_view value,
                  std::size_t maximum) {
    if (value.size() > maximum ||
        value.size() > std::numeric_limits<std::uint16_t>::max())
        return false;
    const auto length = static_cast<std::uint16_t>(value.size());
    bytes.push_back(static_cast<std::uint8_t>(length >> 8));
    bytes.push_back(static_cast<std::uint8_t>(length));
    for (unsigned char ch : value) bytes.push_back(ch);
    return true;
}

void AppendU64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

bool CanonicalIntent(const Intent& intent, std::vector<std::uint8_t>& bytes) {
    static constexpr std::string_view domain = "spirula-agent-admin-intent-v1";
    bytes.clear();
    bytes.reserve(256);
    for (unsigned char ch : domain) bytes.push_back(ch);
    bytes.push_back(1);  // Encoding version.
    bytes.push_back(static_cast<std::uint8_t>(intent.operation));
    if (!AppendString(bytes, intent.worker_id, 64) ||
        !AppendString(bytes, intent.leader_id, 64))
        return false;
    AppendU64(bytes, intent.leader_epoch);
    if (!AppendString(bytes, intent.intent_id, kMaxIdentifierBytes)) return false;
    AppendU64(bytes, intent.issued_at_unix);
    AppendU64(bytes, intent.expires_at_unix);
    if (intent.operation == Operation::Activate &&
        (!AppendString(bytes, intent.update_id, kMaxIdentifierBytes) ||
         !AppendString(bytes, intent.package_sha256, 64)))
        return false;
    if (intent.operation == Operation::Activate)
        AppendU64(bytes, intent.security_version);
    return !bytes.empty() && bytes.size() <= kMaxCanonicalBytes;
}

bool EqualFingerprint(const Fingerprint& left, const Fingerprint& right) {
    std::uint8_t difference = 0;
    for (std::size_t i = 0; i < left.size(); ++i)
        difference |= left[i] ^ right[i];
    return difference == 0;
}

std::uint64_t UnixNow() {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return seconds > 0 ? static_cast<std::uint64_t>(seconds) : 0;
}

}  // namespace

std::optional<Intent> SignIntent(pairing::Leader& leader, Intent intent,
                                 std::string* error) {
    if (error) error->clear();
    intent.signer_public_key_pem.clear();
    intent.signature.clear();
    if (!ValidFields(intent, error)) return std::nullopt;

    const std::string leader_id = leader.LeaderId();
    const std::uint64_t leader_epoch = leader.LeaderEpoch();
    if (!ValidSha256(leader_id) || intent.leader_id != leader_id ||
        !leader_epoch || leader_epoch > kMaxLeaderEpoch ||
        intent.leader_epoch != leader_epoch) {
        SetError(error, "admin intent leader identity does not match signer");
        return std::nullopt;
    }
    const std::uint64_t now_unix = UnixNow();
    if (!ValidTimes(intent, now_unix, error)) return std::nullopt;

    std::vector<std::uint8_t> canonical;
    if (!CanonicalIntent(intent, canonical)) {
        SetError(error, "admin intent cannot be canonically encoded");
        return std::nullopt;
    }
    if (!leader.SignUpdateOffer(intent.worker_id, canonical, intent.signature,
                                intent.signer_public_key_pem, error))
        return std::nullopt;
    if (intent.signature.empty() || intent.signature.size() > kMaxSignatureBytes ||
        intent.signer_public_key_pem.empty() ||
        intent.signer_public_key_pem.size() > kMaxPublicKeyPemBytes) {
        SetError(error, "admin intent signer output exceeds its limits");
        return std::nullopt;
    }
    return intent;
}

bool VerifyIntent(const Config& policy, const Intent& intent,
                  std::uint64_t now_unix, std::string* error) {
    if (error) error->clear();
    if (!ValidFields(intent, error) || !ValidTimes(intent, now_unix, error))
        return false;
    if ((intent.operation == Operation::Reboot && !policy.allow_reboot) ||
        (intent.operation == Operation::Activate && !policy.allow_remote_update))
        return Reject(error, "admin intent operation is not granted by policy");
    if (!ValidSha256(policy.leader_id) || intent.leader_id != policy.leader_id)
        return Reject(error, "admin intent is not from the paired leader");

    Fingerprint approved_signer{};
    if (!DecodeSha256(policy.update_signer_sha256, approved_signer) ||
        policy.update_signer_sha256 == policy.leader_id)
        return Reject(error, "admin update signer pin is invalid");
    if (intent.signer_public_key_pem.empty() ||
        intent.signer_public_key_pem.size() > kMaxPublicKeyPemBytes ||
        intent.signature.empty() || intent.signature.size() > kMaxSignatureBytes)
        return Reject(error, "admin intent signature fields exceed their limits");

    std::vector<std::uint8_t> canonical;
    if (!CanonicalIntent(intent, canonical))
        return Reject(error, "admin intent cannot be canonically encoded");
    Fingerprint signer{};
    if (!pairing::VerifyDetachedUpdateSignature(
            intent.signer_public_key_pem, canonical, intent.signature,
            signer, error))
        return false;
    if (!EqualFingerprint(signer, approved_signer))
        return Reject(error, "admin intent signer does not match policy pin");
    return true;
}

}  // namespace app::agent::admin
