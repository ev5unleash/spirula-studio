#include "app/AgentTls.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_ciphersuites.h>
#include <mbedtls/x509_crl.h>
#include <mbedtls/x509_crt.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace app::agent {
namespace {

using Clock = std::chrono::steady_clock;
using NativeSocket = TlsChannel::NativeSocket;
using Deadline = Clock::time_point;
#ifdef _WIN32
constexpr short kReadEvent = POLLRDNORM;
constexpr short kWriteEvent = POLLWRNORM;
#else
constexpr short kReadEvent = POLLIN;
constexpr short kWriteEvent = POLLOUT;
#endif


constexpr std::uint32_t kMinTimeoutMs = 1;
constexpr std::size_t kMaxPasswordBytes = 4096;
constexpr std::size_t kMaxServerNameBytes = 253;
constexpr int kTls12Ciphers[] = {
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    0,
};

void SetError(std::string* out, std::string message) {
    if (out) *out = std::move(message);
}

std::string ErrorCode(const char* operation, int code) {
    return std::string(operation) + " failed (Mbed TLS/socket error " +
           std::to_string(code) + ")";
}

bool IsBoundedTimeout(std::uint32_t timeout) {
    return timeout >= kMinTimeoutMs && timeout <= TlsChannel::kMaxTimeoutMs;
}

bool CheckPem(const std::string& pem, const char* label, std::string* error) {
    if (pem.empty() || pem.size() > TlsChannel::kMaxPemBytes ||
        pem.find('\0') != std::string::npos) {
        SetError(error, std::string("invalid or oversized ") + label + " PEM");
        return false;
    }
    return true;
}

bool IsNonzeroFingerprint(const TlsChannel::PeerFingerprint& fingerprint) {
    std::uint8_t value = 0;
    for (const std::uint8_t byte : fingerprint) value |= byte;
    return value != 0;
}

bool IsDnsName(std::string_view name) {
    if (name.empty() || name.size() > kMaxServerNameBytes ||
        name.front() == '.' || name.back() == '.') {
        return false;
    }
    std::size_t label_length = 0;
    bool label_starts_hyphen = false;
    char previous = '\0';
    for (const unsigned char c : name) {
        if (c == '.') {
            if (label_length == 0 || label_length > 63 || previous == '-')
                return false;
            label_length = 0;
            previous = '\0';
            continue;
        }
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool digit = c >= '0' && c <= '9';
        if (!alpha && !digit && c != '-') return false;
        if (label_length == 0) label_starts_hyphen = c == '-';
        if (label_starts_hyphen || ++label_length > 63) return false;
        previous = static_cast<char>(c);
    }
    return label_length != 0 && previous != '-';
}

bool ValidateOptions(const TlsChannel::Options& options, bool server,
                     std::string* error) {
    if (!options.enabled || !options.paired) {
        SetError(error, "TLS is disabled or the peer is not paired");
        return false;
    }
    if (!CheckPem(options.local_certificate_pem, "local certificate", error) ||
        !CheckPem(options.local_private_key_pem, "local private key", error) ||
        !CheckPem(options.peer_trust_pem, "peer trust", error) ||
        (!options.peer_crl_pem.empty() &&
         !CheckPem(options.peer_crl_pem, "peer CRL", error))) {
        return false;
    }
    if (options.local_private_key_password.size() > kMaxPasswordBytes ||
        options.local_private_key_password.find('\0') != std::string::npos) {
        SetError(error, "invalid private-key password");
        return false;
    }
    if (server && !options.approved_peer_spki_sha256.empty()) {
        const auto& approved = options.approved_peer_spki_sha256;
        if (approved.size() > 64 ||
            IsNonzeroFingerprint(options.expected_peer_spki_sha256)) {
            SetError(error, "invalid server peer identity allowlist");
            return false;
        }
        for (std::size_t i = 0; i < approved.size(); ++i) {
            if (!IsNonzeroFingerprint(approved[i]) ||
                std::find(approved.begin(), approved.begin() + i,
                          approved[i]) != approved.begin() + i) {
                SetError(error, "invalid or duplicate server peer identity");
                return false;
            }
        }
    } else if (!IsNonzeroFingerprint(options.expected_peer_spki_sha256) ||
               !options.approved_peer_spki_sha256.empty()) {
        SetError(error, "peer SPKI SHA-256 fingerprint is missing or ambiguous");
        return false;
    }
    if (!IsBoundedTimeout(options.handshake_timeout_ms) ||
        !IsBoundedTimeout(options.io_timeout_ms)) {
        SetError(error, "TLS timeout is outside the permitted range");
        return false;
    }
    return true;
}

Deadline MakeDeadline(std::uint32_t timeout_ms) {
    return Clock::now() + std::chrono::milliseconds(timeout_ms);
}

int RemainingMs(Deadline deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - Clock::now()).count();
    if (remaining <= 0) return 0;
    return static_cast<int>(std::min<std::int64_t>(remaining, 60'000));
}

struct Transport final {
    NativeSocket socket = TlsChannel::kInvalidSocket;
#ifdef _WIN32
    bool winsock_started = false;
#endif

    Transport() = default;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    ~Transport() {
        Close();
#ifdef _WIN32
        if (winsock_started) WSACleanup();
#endif
    }

    bool Start(std::string* error) {
#ifdef _WIN32
        WSADATA data{};
        const int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0 || LOBYTE(data.wVersion) != 2 || HIBYTE(data.wVersion) != 2) {
            if (result == 0) WSACleanup();
            SetError(error, ErrorCode("Winsock initialization", result));
            return false;
        }
        winsock_started = true;
#else
        (void)error;
#endif
        return true;
    }

    void Close() noexcept {
        if (socket == TlsChannel::kInvalidSocket) return;
#ifdef _WIN32
        closesocket(static_cast<SOCKET>(socket));
#else
        if (socket <= static_cast<NativeSocket>(std::numeric_limits<int>::max()))
            ::close(static_cast<int>(socket));
#endif
        socket = TlsChannel::kInvalidSocket;
    }

    bool SetNonBlocking(std::string* error) {
#ifdef _WIN32
        u_long enabled = 1;
        if (ioctlsocket(static_cast<SOCKET>(socket), FIONBIO, &enabled) != 0) {
            SetError(error, ErrorCode("setting socket nonblocking", WSAGetLastError()));
            return false;
        }
#ifdef SO_NOSIGPIPE
        const int no_sigpipe = 1;
        if (setsockopt(static_cast<SOCKET>(socket), SOL_SOCKET, SO_NOSIGPIPE,
                       reinterpret_cast<const char*>(&no_sigpipe),
                       sizeof(no_sigpipe)) != 0) {
            SetError(error, ErrorCode("setting socket SIGPIPE policy", WSAGetLastError()));
            return false;
        }
#endif
#else
        const int flags = fcntl(static_cast<int>(socket), F_GETFL, 0);
        if (flags < 0 || fcntl(static_cast<int>(socket), F_SETFL,
                               flags | O_NONBLOCK) < 0) {
            SetError(error, ErrorCode("setting socket nonblocking", errno));
            return false;
        }
#ifdef SO_NOSIGPIPE
        const int no_sigpipe = 1;
        if (setsockopt(static_cast<int>(socket), SOL_SOCKET, SO_NOSIGPIPE,
                       &no_sigpipe, sizeof(no_sigpipe)) != 0) {
            SetError(error, ErrorCode("setting socket SIGPIPE policy", errno));
            return false;
        }
#endif
#endif
        return true;
    }

    bool Wait(short event, Deadline deadline) const {
        for (;;) {
            const int timeout = RemainingMs(deadline);
            if (timeout == 0) return false;
#ifdef _WIN32
            WSAPOLLFD descriptor{};
            descriptor.fd = static_cast<SOCKET>(socket);
            descriptor.events = event;
            const int result = WSAPoll(&descriptor, 1, timeout);
            if (result > 0)
                return (descriptor.revents & (event | POLLERR | POLLHUP)) != 0;
            if (result == 0) return false;
            if (WSAGetLastError() != WSAEINTR) return false;
#else
            pollfd descriptor{};
            descriptor.fd = static_cast<int>(socket);
            descriptor.events = event;
            const int result = ::poll(&descriptor, 1, timeout);
            if (result > 0)
                return (descriptor.revents & (event | POLLERR | POLLHUP)) != 0;
            if (result == 0) return false;
            if (errno != EINTR) return false;
            #endif
        }
    }
};

int SocketSend(void* context, const unsigned char* data, std::size_t size) {
    auto* transport = static_cast<Transport*>(context);
    const int length = static_cast<int>(std::min<std::size_t>(
        size, static_cast<std::size_t>(std::numeric_limits<int>::max())));
#ifdef _WIN32
    const int result = ::send(static_cast<SOCKET>(transport->socket),
                              reinterpret_cast<const char*>(data), length, 0);
    if (result == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK || error == WSAEINTR)
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return result;
#else
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    const ssize_t result = ::send(static_cast<int>(transport->socket), data,
                                  static_cast<std::size_t>(length), flags);
    if (result < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return static_cast<int>(result);
#endif
}

int SocketReceive(void* context, unsigned char* data, std::size_t size) {
    auto* transport = static_cast<Transport*>(context);
    const int length = static_cast<int>(std::min<std::size_t>(
        size, static_cast<std::size_t>(std::numeric_limits<int>::max())));
#ifdef _WIN32
    const int result = ::recv(static_cast<SOCKET>(transport->socket),
                              reinterpret_cast<char*>(data), length, 0);
    if (result == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK || error == WSAEINTR)
            return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return result == 0 ? MBEDTLS_ERR_SSL_CONN_EOF : result;
#else
    const ssize_t result = ::recv(static_cast<int>(transport->socket), data,
                                  static_cast<std::size_t>(length), 0);
    if (result < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return result == 0 ? MBEDTLS_ERR_SSL_CONN_EOF : static_cast<int>(result);
#endif
}

bool IsCrlForIssuer(const mbedtls_x509_crl& crl,
                    const mbedtls_x509_buf& issuer) {
    return crl.issuer_raw.len == issuer.len && issuer.p && crl.issuer_raw.p &&
           std::memcmp(crl.issuer_raw.p, issuer.p, issuer.len) == 0;
}

bool HasCrlForIssuer(const mbedtls_x509_buf& issuer,
                     const mbedtls_x509_crl* crls) {
    for (const mbedtls_x509_crl* crl = crls; crl; crl = crl->next)
        if (IsCrlForIssuer(*crl, issuer)) return true;
    return false;
}

bool IsTrustAnchor(const mbedtls_x509_crt& cert,
                   const mbedtls_x509_crt* trust) {
    for (const mbedtls_x509_crt* ca = trust; ca; ca = ca->next)
        if (cert.raw.len == ca->raw.len && cert.raw.p && ca->raw.p &&
            std::memcmp(cert.raw.p, ca->raw.p, cert.raw.len) == 0) {
            return true;
        }
    return false;
}

bool HasOnlyCaTrustAnchors(const mbedtls_x509_crt* trust) {
    bool found = false;
    for (const mbedtls_x509_crt* ca = trust; ca; ca = ca->next) {
        found = true;
        if (mbedtls_x509_crt_get_ca_istrue(ca) != 1) return false;
    }
    return found;
}


bool SpkiFingerprint(const mbedtls_x509_crt* peer,
                     TlsChannel::PeerFingerprint& actual) {
    if (!peer) return false;
    std::array<unsigned char, 4096> der{};
    const int der_size = mbedtls_pk_write_pubkey_der(&peer->pk, der.data(), der.size());
    if (der_size <= 0 || static_cast<std::size_t>(der_size) > der.size())
        return false;
    const unsigned char* der_start = der.data() + der.size() - der_size;
    return mbedtls_sha256(der_start, static_cast<std::size_t>(der_size),
                          actual.data(), 0) == 0;
}

}  // namespace

struct TlsChannel::Impl final {
    Transport transport;
    mbedtls_entropy_context entropy{};
    mbedtls_ctr_drbg_context drbg{};
    mbedtls_x509_crt local_certificate{};
    mbedtls_pk_context local_private_key{};
    mbedtls_x509_crt peer_trust{};
    mbedtls_x509_crl peer_crl{};
    mbedtls_ssl_config config{};
    mbedtls_ssl_context ssl{};
    bool open = false;
    std::uint32_t io_timeout_ms = 0;
    PeerFingerprint expected_peer_spki_sha256{};
    bool peer_pin_verified = false;
    std::vector<PeerFingerprint> approved_peer_spki_sha256;
    PeerFingerprint authenticated_peer_spki_sha256{};

    Impl() {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        mbedtls_x509_crt_init(&local_certificate);
        mbedtls_pk_init(&local_private_key);
        mbedtls_x509_crt_init(&peer_trust);
        mbedtls_x509_crl_init(&peer_crl);
        mbedtls_ssl_config_init(&config);
        mbedtls_ssl_init(&ssl);
    }

    static int VerifyPeer(void* context, mbedtls_x509_crt* cert, int depth,
                         std::uint32_t* flags) {
        auto* self = static_cast<Impl*>(context);
        if (!self || !cert || !flags)
            return MBEDTLS_ERR_X509_FATAL_ERROR;
        if (depth == 0 && IsTrustAnchor(*cert, &self->peer_trust)) {
            *flags |= MBEDTLS_X509_BADCERT_NOT_TRUSTED;
            return 0;
        }
        if (depth == 0) {
            self->peer_pin_verified =
                SpkiFingerprint(cert, self->authenticated_peer_spki_sha256) &&
                (self->approved_peer_spki_sha256.empty()
                     ? self->authenticated_peer_spki_sha256 ==
                           self->expected_peer_spki_sha256
                     : std::find(self->approved_peer_spki_sha256.begin(),
                                 self->approved_peer_spki_sha256.end(),
                                 self->authenticated_peer_spki_sha256) !=
                           self->approved_peer_spki_sha256.end());
            if (!self->peer_pin_verified)
                *flags |= MBEDTLS_X509_BADCERT_NOT_TRUSTED;
        }
        if (self->peer_crl.raw.p &&
            (depth == 0 || !IsTrustAnchor(*cert, &self->peer_trust)) &&
            !HasCrlForIssuer(cert->issuer_raw, &self->peer_crl))
            *flags |= MBEDTLS_X509_BADCERT_NOT_TRUSTED;
        return 0;
    }

    ~Impl() {
        transport.Close();
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&config);
        mbedtls_x509_crl_free(&peer_crl);
        mbedtls_x509_crt_free(&peer_trust);
        mbedtls_pk_free(&local_private_key);
        mbedtls_x509_crt_free(&local_certificate);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    }

    bool Configure(const Options& options, int endpoint, std::string* error) {
        expected_peer_spki_sha256 = options.expected_peer_spki_sha256;
        approved_peer_spki_sha256 = options.approved_peer_spki_sha256;
        peer_pin_verified = false;
        static constexpr unsigned char personalization[] =
            "spirula-agent-tls-channel-v1";
        int result = mbedtls_ctr_drbg_seed(
            &drbg, mbedtls_entropy_func, &entropy, personalization,
            sizeof(personalization) - 1);
        if (result != 0) {
            SetError(error, ErrorCode("TLS random generator initialization", result));
            return false;
        }

        result = mbedtls_x509_crt_parse(
            &local_certificate,
            reinterpret_cast<const unsigned char*>(options.local_certificate_pem.c_str()),
            options.local_certificate_pem.size() + 1);
        if (result != 0 || !local_certificate.raw.p) {
            SetError(error, ErrorCode("local certificate parsing", result));
            return false;
        }
        const unsigned char* password = options.local_private_key_password.empty()
            ? nullptr
            : reinterpret_cast<const unsigned char*>(
                  options.local_private_key_password.data());
        result = mbedtls_pk_parse_key(
            &local_private_key,
            reinterpret_cast<const unsigned char*>(options.local_private_key_pem.c_str()),
            options.local_private_key_pem.size() + 1, password,
            options.local_private_key_password.size(), mbedtls_ctr_drbg_random, &drbg);
        if (result != 0) {
            SetError(error, ErrorCode("local private-key parsing", result));
            return false;
        }
        result = mbedtls_pk_check_pair(&local_certificate.pk, &local_private_key,
                                       mbedtls_ctr_drbg_random, &drbg);
        if (result != 0) {
            SetError(error, ErrorCode("local certificate/private-key match", result));
            return false;
        }

        result = mbedtls_x509_crt_parse(
            &peer_trust,
            reinterpret_cast<const unsigned char*>(options.peer_trust_pem.c_str()),
            options.peer_trust_pem.size() + 1);
        if (result != 0 || !peer_trust.raw.p) {
            SetError(error, ErrorCode("explicit peer trust parsing", result));
            return false;
        }
        if (!HasOnlyCaTrustAnchors(&peer_trust)) {
            SetError(error, "explicit peer trust must contain CA certificates only");
            return false;
        }
        if (!options.peer_crl_pem.empty()) {
            result = mbedtls_x509_crl_parse(
                &peer_crl,
                reinterpret_cast<const unsigned char*>(options.peer_crl_pem.c_str()),
                options.peer_crl_pem.size() + 1);
            if (result != 0 || !peer_crl.raw.p) {
                SetError(error, ErrorCode("peer CRL parsing", result));
                return false;
            }
        }

        result = mbedtls_ssl_config_defaults(
            &config, endpoint, MBEDTLS_SSL_TRANSPORT_STREAM,
            MBEDTLS_SSL_PRESET_DEFAULT);
        if (result != 0) {
            SetError(error, ErrorCode("TLS configuration defaults", result));
            return false;
        }
        mbedtls_ssl_conf_min_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_max_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_ciphersuites(&config, kTls12Ciphers);
        mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(
            &config, &peer_trust, peer_crl.raw.p ? &peer_crl : nullptr);
        mbedtls_ssl_conf_verify(&config, VerifyPeer, this);
        mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &drbg);
        result = mbedtls_ssl_conf_own_cert(&config, &local_certificate,
                                          &local_private_key);
        if (result != 0) {
            SetError(error, ErrorCode("TLS local identity configuration", result));
            return false;
        }
        result = mbedtls_ssl_setup(&ssl, &config);
        if (result != 0) {
            SetError(error, ErrorCode("TLS context setup", result));
            return false;
        }
        io_timeout_ms = options.io_timeout_ms;

        return true;
    }

    bool Handshake(const Options& options, int endpoint,
                   const std::string* server_name, std::string* error) {
        if (!Configure(options, endpoint, error)) return false;
        if (server_name) {
            const int result = mbedtls_ssl_set_hostname(&ssl, server_name->c_str());
            if (result != 0) {
                SetError(error, ErrorCode("TLS server-name configuration", result));
                return false;
            }
        }
        mbedtls_ssl_set_bio(&ssl, &transport, SocketSend, SocketReceive, nullptr);

        const Deadline deadline = MakeDeadline(options.handshake_timeout_ms);
        for (;;) {
            const int result = mbedtls_ssl_handshake(&ssl);
            if (result == 0) break;
            if (result != MBEDTLS_ERR_SSL_WANT_READ &&
                result != MBEDTLS_ERR_SSL_WANT_WRITE) {
                SetError(error, ErrorCode("mutual TLS handshake", result));
                return false;
            }
            const short event = result == MBEDTLS_ERR_SSL_WANT_READ
                ? kReadEvent : kWriteEvent;
            if (!transport.Wait(event, deadline)) {
                SetError(error, "mutual TLS handshake timed out or socket polling failed");
                return false;
            }
        }

        if (mbedtls_ssl_get_verify_result(&ssl) != 0) {
            SetError(error, "peer certificate chain or validity check failed");
            return false;
        }
        if (!peer_pin_verified) {
            SetError(error, "peer public-key fingerprint does not match the paired identity");
            return false;
        }
        const mbedtls_x509_crt* peer = mbedtls_ssl_get_peer_cert(&ssl);
        if (!peer) {
            SetError(error, "peer did not present a certificate");
            return false;
        }
        open = true;
        return true;
    }

    bool WaitForTls(int result, Deadline deadline) {
        if (result == MBEDTLS_ERR_SSL_WANT_READ)
            return transport.Wait(kReadEvent, deadline);
        if (result == MBEDTLS_ERR_SSL_WANT_WRITE)
            return transport.Wait(kWriteEvent, deadline);
        return false;
    }

    bool WriteAll(const std::uint8_t* data, std::size_t size,
                  Deadline deadline, std::string* error) {
        std::size_t offset = 0;
        while (offset < size) {
            const int result = mbedtls_ssl_write(
                &ssl, data + offset, size - offset);
            if (result > 0) {
                offset += static_cast<std::size_t>(result);
                continue;
            }
            if ((result == MBEDTLS_ERR_SSL_WANT_READ ||
                 result == MBEDTLS_ERR_SSL_WANT_WRITE) &&
                WaitForTls(result, deadline)) {
                continue;
            }
            SetError(error, result == MBEDTLS_ERR_SSL_WANT_READ ||
                                    result == MBEDTLS_ERR_SSL_WANT_WRITE
                         ? "TLS write timed out or socket polling failed"
                         : ErrorCode("TLS write", result));
            return false;
        }
        return true;
    }

    bool ReadAll(std::uint8_t* data, std::size_t size,
                 Deadline deadline, std::string* error) {
        std::size_t offset = 0;
        while (offset < size) {
            const int result = mbedtls_ssl_read(&ssl, data + offset, size - offset);
            if (result > 0) {
                offset += static_cast<std::size_t>(result);
                continue;
            }
            if ((result == MBEDTLS_ERR_SSL_WANT_READ ||
                 result == MBEDTLS_ERR_SSL_WANT_WRITE) &&
                WaitForTls(result, deadline)) {
                continue;
            }
            SetError(error, result == MBEDTLS_ERR_SSL_WANT_READ ||
                                    result == MBEDTLS_ERR_SSL_WANT_WRITE
                         ? "TLS read timed out or socket polling failed"
                         : ErrorCode("TLS read", result));
            return false;
        }
        return true;
    }

    void Abort() noexcept {
        open = false;
        transport.Close();
    }
};

TlsChannel::TlsChannel(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

TlsChannel::TlsChannel(TlsChannel&&) noexcept = default;
TlsChannel& TlsChannel::operator=(TlsChannel&&) noexcept = default;
TlsChannel::~TlsChannel() = default;

std::optional<TlsChannel> TlsChannel::ConnectClient(
    const ClientOptions& options, std::string* error) {
    if (!ValidateOptions(options.tls, false, error)) return std::nullopt;
    if (!options.port || options.connect_address.empty() ||
        options.connect_address.size() > 45 ||
        options.connect_address.find('\0') != std::string::npos ||
        !IsDnsName(options.server_name) ||
        !IsBoundedTimeout(options.connect_timeout_ms)) {
        SetError(error, "invalid leader address, server name, port, or connect timeout");
        return std::nullopt;
    }

    auto impl = std::make_unique<Impl>();
    if (!impl->transport.Start(error)) return std::nullopt;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICHOST;
    const std::string service = std::to_string(options.port);
    addrinfo* resolved = nullptr;
    const int lookup = getaddrinfo(options.connect_address.c_str(), service.c_str(),
                                   &hints, &resolved);
    if (lookup != 0 || !resolved) {
        SetError(error, "leader address must be a numeric IPv4 or IPv6 address");
        if (resolved) freeaddrinfo(resolved);
        return std::nullopt;
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(resolved,
                                                                 freeaddrinfo);

    const Deadline deadline = MakeDeadline(options.connect_timeout_ms);
    bool connected = false;
    int last_error = 0;
    for (const addrinfo* address = addresses.get(); address; address = address->ai_next) {
#ifdef _WIN32
        const SOCKET raw = ::socket(address->ai_family, address->ai_socktype,
                                    address->ai_protocol);
        if (raw == INVALID_SOCKET) {
            last_error = WSAGetLastError();
            continue;
        }
        impl->transport.socket = static_cast<NativeSocket>(raw);
#else
        const int raw = ::socket(address->ai_family, address->ai_socktype,
                                 address->ai_protocol);
        if (raw < 0) {
            last_error = errno;
            continue;
        }
        impl->transport.socket = static_cast<NativeSocket>(raw);
#endif
        if (!impl->transport.SetNonBlocking(error)) return std::nullopt;

#ifdef _WIN32
        const int result = ::connect(static_cast<SOCKET>(impl->transport.socket),
                                     address->ai_addr,
                                     static_cast<int>(address->ai_addrlen));
        if (result == 0) {
            connected = true;
        } else {
            last_error = WSAGetLastError();
            if ((last_error == WSAEWOULDBLOCK || last_error == WSAEINPROGRESS ||
                 last_error == WSAEINTR) &&
                impl->transport.Wait(kWriteEvent, deadline)) {
                int socket_error = 0;
                int error_size = sizeof(socket_error);
                if (getsockopt(static_cast<SOCKET>(impl->transport.socket),
                               SOL_SOCKET, SO_ERROR,
                               reinterpret_cast<char*>(&socket_error),
                               &error_size) == 0 && socket_error == 0) {
                    connected = true;
                } else {
                    last_error = socket_error ? socket_error : WSAGetLastError();
                }
            }
        }
#else
        const int result = ::connect(static_cast<int>(impl->transport.socket),
                                     address->ai_addr, address->ai_addrlen);
        if (result == 0) {
            connected = true;
        } else {
            last_error = errno;
            if ((last_error == EINPROGRESS || last_error == EALREADY ||
                 last_error == EINTR) &&
                impl->transport.Wait(kWriteEvent, deadline)) {
                int socket_error = 0;
                socklen_t error_size = sizeof(socket_error);
                if (getsockopt(static_cast<int>(impl->transport.socket), SOL_SOCKET,
                               SO_ERROR, &socket_error, &error_size) == 0 &&
                    socket_error == 0) {
                    connected = true;
                } else {
                    last_error = socket_error ? socket_error : errno;
                }
            }
        }
#endif
        if (connected) break;
        impl->transport.Close();
        if (RemainingMs(deadline) == 0) break;
    }
    if (!connected) {
        SetError(error, ErrorCode("leader TCP connection", last_error));
        return std::nullopt;
    }

    if (!impl->Handshake(options.tls, MBEDTLS_SSL_IS_CLIENT,
                         &options.server_name, error)) {
        return std::nullopt;
    }
    return std::optional<TlsChannel>(TlsChannel(std::move(impl)));
}

std::optional<TlsChannel> TlsChannel::AcceptServer(
    NativeSocket accepted_socket, const Options& options, std::string* error) {
    auto impl = std::make_unique<Impl>();
    impl->transport.socket = accepted_socket;
#ifndef _WIN32
    if (accepted_socket != kInvalidSocket &&
        accepted_socket > static_cast<NativeSocket>(std::numeric_limits<int>::max())) {
        impl->transport.socket = kInvalidSocket;
        SetError(error, "accepted socket handle is out of range");
        return std::nullopt;
    }
#endif
    if (accepted_socket == kInvalidSocket) {
        SetError(error, "accepted socket is invalid");
        return std::nullopt;
    }
    if (!impl->transport.Start(error) ||
        !ValidateOptions(options, true, error) ||
        !impl->transport.SetNonBlocking(error)) {
        return std::nullopt;
    }
    if (!impl->Handshake(options, MBEDTLS_SSL_IS_SERVER, nullptr, error))
        return std::nullopt;
    return std::optional<TlsChannel>(TlsChannel(std::move(impl)));
}

bool TlsChannel::SendFrame(const std::uint8_t* data, std::size_t size,
                           std::string* error) {
    if (!impl_ || !impl_->open) {
        SetError(error, "TLS channel is closed");
        return false;
    }
    if (size > kMaxFrameBytes || (size != 0 && data == nullptr)) {
        SetError(error, "frame is null or exceeds the 1 MiB limit");
        return false;
    }
    const auto length = static_cast<std::uint32_t>(size);
    const std::uint8_t header[] = {
        static_cast<std::uint8_t>(length >> 24),
        static_cast<std::uint8_t>(length >> 16),
        static_cast<std::uint8_t>(length >> 8),
        static_cast<std::uint8_t>(length),
    };
    const Deadline deadline = MakeDeadline(impl_->io_timeout_ms);
    if (!impl_->WriteAll(header, sizeof(header), deadline, error) ||
        !impl_->WriteAll(data, size, deadline, error)) {
        impl_->Abort();
        return false;
    }
    return true;
}

bool TlsChannel::ReceiveFrame(std::vector<std::uint8_t>& data,
                              std::string* error) {
    data.clear();
    if (!impl_ || !impl_->open) {
        SetError(error, "TLS channel is closed");
        return false;
    }
    std::uint8_t header[4]{};
    const Deadline deadline = MakeDeadline(impl_->io_timeout_ms);
    if (!impl_->ReadAll(header, sizeof(header), deadline, error)) {
        impl_->Abort();
        return false;
    }
    const std::uint32_t length =
        (static_cast<std::uint32_t>(header[0]) << 24) |
        (static_cast<std::uint32_t>(header[1]) << 16) |
        (static_cast<std::uint32_t>(header[2]) << 8) |
        static_cast<std::uint32_t>(header[3]);
    if (length > kMaxFrameBytes) {
        SetError(error, "incoming frame exceeds the 1 MiB limit");
        impl_->Abort();
        return false;
    }
    data.resize(length);
    if (length != 0 && !impl_->ReadAll(data.data(), length, deadline, error)) {
        data.clear();
        impl_->Abort();
        return false;
    }
    return true;
}

bool TlsChannel::IsOpen() const noexcept {
    return impl_ && impl_->open;
}
std::optional<TlsChannel::PeerFingerprint>
TlsChannel::PeerSpkiSha256() const noexcept {
    if (!impl_ || !impl_->open) return std::nullopt;
    return impl_->authenticated_peer_spki_sha256;
}


void TlsChannel::Close() noexcept {
    if (impl_) impl_->Abort();
}

}  // namespace app::agent
