#pragma once

#include "app/AgentTls.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace app::agent {

struct TransferFile {
    std::string path;       // Normalized UTF-8, relative, slash-separated.
    std::uint64_t size = 0;
    std::string sha256;     // Lowercase SHA-256 hex.
};

enum class TransferError {
    None,
    InvalidArgument,
    InvalidManifest,
    QuotaExceeded,
    Filesystem,
    Integrity,
    Protocol,
    PeerRejected,
    Interrupted,
};

struct TransferResult {
    TransferError error = TransferError::None;
    std::string message;
    explicit operator bool() const noexcept { return error == TransferError::None; }
};

// Caller authorizes the attempt, serializes channel use and closes on failure.
// Budget includes payloads, 4 KiB per file/directory, and stale partials;
// resumed partials count within their declared file size.
TransferResult SendArtifacts(TlsChannel& channel,
                             const std::filesystem::path& source_root,
                             const std::vector<TransferFile>& approved_manifest,
                             std::uint64_t disk_budget_bytes);
TransferResult ReceiveArtifacts(TlsChannel& channel,
                                const std::filesystem::path& staging_root,
                                const std::vector<TransferFile>& approved_manifest,
                                std::uint64_t disk_budget_bytes);

namespace detail {

// Framed transport seam for deterministic protocol tests. Production callers
// should use the TlsChannel overloads above.
struct FrameTransport {
    void* context = nullptr;
    bool (*send)(void*, const std::uint8_t*, std::size_t, std::string*) = nullptr;
    bool (*receive)(void*, std::vector<std::uint8_t>&, std::string*) = nullptr;
};

TransferResult SendArtifacts(FrameTransport transport,
                             const std::filesystem::path& source_root,
                             const std::vector<TransferFile>& approved_manifest,
                             std::uint64_t disk_budget_bytes);
TransferResult ReceiveArtifacts(FrameTransport transport,
                                const std::filesystem::path& staging_root,
                                const std::vector<TransferFile>& approved_manifest,
                                std::uint64_t disk_budget_bytes);

}  // namespace detail
}  // namespace app::agent
