#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace app::agent {

class TlsChannel final {
public:
    using NativeSocket = std::uintptr_t;
    using PeerFingerprint = std::array<std::uint8_t, 32>;

    static constexpr NativeSocket kInvalidSocket =
        std::numeric_limits<NativeSocket>::max();
    static constexpr std::size_t kMaxFrameBytes = 1024 * 1024;
    static constexpr std::size_t kMaxPemBytes = 1024 * 1024;
    static constexpr std::uint32_t kMaxTimeoutMs = 60'000;

    struct Options {
        // Both gates must be true before either endpoint will handshake/connect.
        bool enabled = false;
        bool paired = false;

        std::string local_certificate_pem;
        std::string local_private_key_pem;
        std::string local_private_key_password;
        std::string peer_trust_pem;
        // Optional signed CRL; revocation of a pinned identity also requires
        // the caller to deny paired access before creating a new channel.
        std::string peer_crl_pem;
        PeerFingerprint expected_peer_spki_sha256{};
        // Server-only allowlist; the authenticated pin identifies a worker.
        std::vector<PeerFingerprint> approved_peer_spki_sha256;

        std::uint32_t handshake_timeout_ms = 10'000;
        std::uint32_t io_timeout_ms = 10'000;
    };

    struct ClientOptions {
        Options tls;
        // Numeric IPv4 or IPv6 only; DNS is deliberately not resolved here.
        std::string connect_address;
        // DNS name used for TLS SNI and certificate hostname verification.
        std::string server_name;
        std::uint16_t port = 0;
        std::uint32_t connect_timeout_ms = 10'000;
    };

    // Connects only when enabled and paired. No system trust store is consulted.
    static std::optional<TlsChannel> ConnectClient(
        const ClientOptions& options, std::string* error = nullptr);

    // Takes ownership of an already accepted TCP socket, including on failure.
    // This API never binds or listens; the caller must gate its listener itself.
    static std::optional<TlsChannel> AcceptServer(
        NativeSocket accepted_socket, const Options& options,
        std::string* error = nullptr);

    TlsChannel(TlsChannel&&) noexcept;
    TlsChannel& operator=(TlsChannel&&) noexcept;
    ~TlsChannel();

    TlsChannel(const TlsChannel&) = delete;
    TlsChannel& operator=(const TlsChannel&) = delete;

    bool SendFrame(const std::uint8_t* data, std::size_t size,
                   std::string* error = nullptr);
    bool ReceiveFrame(std::vector<std::uint8_t>& data,
                      std::string* error = nullptr);
    bool IsOpen() const noexcept;
    std::optional<PeerFingerprint> PeerSpkiSha256() const noexcept;
    void Close() noexcept;

private:
    struct Impl;
    explicit TlsChannel(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace app::agent
