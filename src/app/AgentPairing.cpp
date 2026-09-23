#include "app/AgentPairing.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <limits>
#include <mutex>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_ciphersuites.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/platform_util.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <aclapi.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace app::agent::pairing {
namespace {

namespace fs = std::filesystem;
using Fingerprint = TlsChannel::PeerFingerprint;
using Socket = TlsChannel::NativeSocket;
constexpr std::size_t kMaxStateBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaxPemBytes = TlsChannel::kMaxPemBytes;
constexpr std::size_t kMaxFrameBytes = TlsChannel::kMaxFrameBytes;
constexpr std::size_t kMaxWorkers = 4096;
constexpr std::size_t kMaxInvitations = 4096;
constexpr std::size_t kMaxActiveWorkers = 64;
constexpr std::size_t kMaxLabelBytes = 128;
constexpr std::uint32_t kIoTimeoutMs = 10'000;
constexpr std::uint64_t kMaxLeaderEpoch = (std::uint64_t{1} << 53) - 1;
constexpr std::array<int, 3> kTlsCiphers = {
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    0,
};
constexpr std::string_view kStateLeaderMagic = "SSPAIRL1";
constexpr std::string_view kStateWorkerMagic = "SSPAIRW1";
constexpr std::string_view kInviteMagic = "SPINV001";
constexpr std::string_view kWireMagic = "SPPAIR01";
constexpr std::string_view kSignatureDomain = "spirula-agent-pairing-v1";
constexpr char kLeaderFile[] = "leader.bin";
constexpr char kWorkerFile[] = "worker.bin";
constexpr std::string_view kUpdateSignerMagic = "SSUPDSG1";
constexpr char kUpdateSignerFile[] = "update-signer.bin";

void SetError(std::string* out, std::string_view message) {
    if (out) out->assign(message.data(), message.size());
}

bool Nonzero(const Fingerprint& bytes) {
    std::uint8_t bits = 0;
    for (std::uint8_t byte : bytes) bits |= byte;
    return bits != 0;
}

bool EqualSecret(const std::uint8_t* a, const std::uint8_t* b,
                 std::size_t size) {
    std::uint8_t difference = 0;
    for (std::size_t i = 0; i != size; ++i) difference |= a[i] ^ b[i];
    return difference == 0;
}

std::string Hex(const std::uint8_t* bytes, std::size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out(size * 2, '0');
    for (std::size_t i = 0; i != size; ++i) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 15];
    }
    return out;
}

bool Unhex(std::string_view text, std::uint8_t* out, std::size_t size) {
    if (text.size() != size * 2) return false;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (std::size_t i = 0; i != size; ++i) {
        const int hi = digit(text[i * 2]);
        const int lo = digit(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

bool IsDnsName(std::string_view name) {
    if (name.empty() || name.size() > 253 || name.front() == '.' ||
        name.back() == '.') return false;
    std::size_t label = 0;
    bool first_hyphen = false;
    char previous = '\0';
    for (unsigned char c : name) {
        if (c == '.') {
            if (label == 0 || label > 63 || first_hyphen || previous == '-')
                return false;
            label = 0;
            previous = '\0';
            continue;
        }
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool number = c >= '0' && c <= '9';
        if (!alpha && !number && c != '-') return false;
        if (label == 0) first_hyphen = c == '-';
        if (first_hyphen || ++label > 63) return false;
        previous = static_cast<char>(c);
    }
    return label != 0 && previous != '-';
}

bool Hash(const std::uint8_t* data, std::size_t size,
          std::array<std::uint8_t, 32>& out) {
    return mbedtls_sha256(data, size, out.data(), 0) == 0;
}

bool Hash(const std::string& data, std::array<std::uint8_t, 32>& out) {
    return Hash(reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), out);
}

struct Rng final {
    mbedtls_entropy_context entropy{};
    mbedtls_ctr_drbg_context drbg{};
    bool initialized = false;

    Rng() {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
    }
    Rng(const Rng&) = delete;
    Rng& operator=(const Rng&) = delete;
    ~Rng() {
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    }

    bool Init() {
        static constexpr unsigned char personalization[] =
            "spirula-agent-pairing-v1";
        initialized = mbedtls_ctr_drbg_seed(
            &drbg, mbedtls_entropy_func, &entropy, personalization,
            sizeof(personalization) - 1) == 0;
        return initialized;
    }
    bool Fill(std::uint8_t* output, std::size_t size) {
        return initialized &&
               mbedtls_ctr_drbg_random(&drbg, output, size) == 0;
    }
};

struct Pk final {
    mbedtls_pk_context value{};
    Pk() { mbedtls_pk_init(&value); }
    ~Pk() { mbedtls_pk_free(&value); }
    Pk(const Pk&) = delete;
    Pk& operator=(const Pk&) = delete;
};

struct Cert final {
    mbedtls_x509_crt value{};
    Cert() { mbedtls_x509_crt_init(&value); }
    ~Cert() { mbedtls_x509_crt_free(&value); }
    Cert(const Cert&) = delete;
    Cert& operator=(const Cert&) = delete;
};

bool ParseCertificate(const std::string& pem, Cert& cert) {
    return !pem.empty() && pem.size() <= kMaxPemBytes &&
           pem.find('\0') == std::string::npos &&
           mbedtls_x509_crt_parse(
               &cert.value,
               reinterpret_cast<const unsigned char*>(pem.c_str()),
               pem.size() + 1) == 0 && cert.value.raw.p;
}

bool ParsePrivateKey(const std::string& pem, Rng& rng, Pk& key) {
    return !pem.empty() && pem.size() <= kMaxPemBytes &&
           pem.find('\0') == std::string::npos &&
           mbedtls_pk_parse_key(
               &key.value,
               reinterpret_cast<const unsigned char*>(pem.c_str()),
               pem.size() + 1, nullptr, 0,
               mbedtls_ctr_drbg_random, &rng.drbg) == 0;
}

bool ParsePublicKey(const std::string& pem, Pk& key) {
    return !pem.empty() && pem.size() <= 16 * 1024 &&
           pem.find('\0') == std::string::npos &&
           mbedtls_pk_parse_public_key(
               &key.value,
               reinterpret_cast<const unsigned char*>(pem.c_str()),
               pem.size() + 1) == 0 &&
           mbedtls_pk_can_do(&key.value, MBEDTLS_PK_ECDSA) &&
           mbedtls_pk_get_bitlen(&key.value) == 256;
}

bool KeyMatches(const Cert& cert, const Pk& key, Rng& rng) {
    return mbedtls_pk_check_pair(&cert.value.pk, &key.value,
                                 mbedtls_ctr_drbg_random, &rng.drbg) == 0;
}

bool ComputeFingerprint(const mbedtls_pk_context& key, Fingerprint& out) {
    std::array<unsigned char, 4096> der{};
    const int length = mbedtls_pk_write_pubkey_der(
        const_cast<mbedtls_pk_context*>(&key), der.data(), der.size());
    if (length <= 0 || static_cast<std::size_t>(length) > der.size()) return false;
    return mbedtls_sha256(der.data() + der.size() - length,
                          static_cast<std::size_t>(length), out.data(), 0) == 0;
}

bool ComputeFingerprint(const Cert& cert, Fingerprint& out) {
    return ComputeFingerprint(cert.value.pk, out);
}

struct BufferWiper final {
    unsigned char* data;
    std::size_t size;
    ~BufferWiper() { if (size) mbedtls_platform_zeroize(data, size); }
};
bool WritePrivateKey(const Pk& key, std::string& out) {
    std::vector<unsigned char> pem(16 * 1024, 0);
    BufferWiper wipe{pem.data(), pem.size()};
    const int result = mbedtls_pk_write_key_pem(
        const_cast<mbedtls_pk_context*>(&key.value), pem.data(), pem.size());
    if (result != 0) return false;
    out.assign(reinterpret_cast<const char*>(pem.data()));
    return out.find('\0') == std::string::npos;
}

bool WritePublicKey(const Pk& key, std::string& out) {
    std::vector<unsigned char> pem(16 * 1024, 0);
    const int result = mbedtls_pk_write_pubkey_pem(
        const_cast<mbedtls_pk_context*>(&key.value), pem.data(), pem.size());
    if (result != 0) return false;
    out.assign(reinterpret_cast<const char*>(pem.data()));
    mbedtls_platform_zeroize(pem.data(), pem.size());
    return out.find('\0') == std::string::npos;
}

bool GenerateKey(Rng& rng, Pk& key, std::string& private_pem,
                 std::string* public_pem = nullptr) {
    const mbedtls_pk_info_t* info = mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY);
    if (!info || mbedtls_pk_setup(&key.value, info) != 0 ||
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
                            mbedtls_pk_ec(key.value),
                            mbedtls_ctr_drbg_random, &rng.drbg) != 0 ||
        !WritePrivateKey(key, private_pem)) return false;
    return !public_pem || WritePublicKey(key, *public_pem);
}

bool MakeSerial(Rng& rng, std::array<unsigned char, 16>& serial) {
    if (!rng.Fill(serial.data(), serial.size())) return false;
    serial[0] &= 0x7f;
    std::uint8_t any = 0;
    for (std::uint8_t byte : serial) any |= byte;
    if (!any) serial.back() = 1;
    return true;
}

bool FormatUtc(std::time_t value, char (&out)[15]) {
    std::tm tm{};
#ifdef _WIN32
    if (gmtime_s(&tm, &value) != 0) return false;
#else
    if (!gmtime_r(&value, &tm)) return false;
#endif
    return std::strftime(out, sizeof(out), "%Y%m%d%H%M%S", &tm) == 14;
}

bool WriteCertificate(Rng& rng, const std::string& subject_name,
                      const std::string& issuer_name, Pk& subject_key,
                      Pk& issuer_key, bool is_ca, std::string& pem) {
    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);
    std::array<unsigned char, 16> serial{};
    char not_before[15]{};
    char not_after[15]{};
    const std::time_t now = std::time(nullptr);
    const bool prepared = MakeSerial(rng, serial) &&
        FormatUtc(now - 300, not_before) &&
        FormatUtc(now + (is_ca ? 10LL : 5LL) * 365 * 24 * 60 * 60, not_after) &&
        mbedtls_x509write_crt_set_serial_raw(&crt, serial.data(), serial.size()) == 0 &&
        mbedtls_x509write_crt_set_validity(&crt, not_before, not_after) == 0 &&
        mbedtls_x509write_crt_set_subject_name(&crt, subject_name.c_str()) == 0 &&
        mbedtls_x509write_crt_set_issuer_name(&crt, issuer_name.c_str()) == 0 &&
        mbedtls_x509write_crt_set_basic_constraints(&crt, is_ca ? 1 : 0,
                                                     is_ca ? 0 : -1) == 0 &&
        mbedtls_x509write_crt_set_key_usage(
            &crt, is_ca ? (MBEDTLS_X509_KU_DIGITAL_SIGNATURE |
                           MBEDTLS_X509_KU_KEY_CERT_SIGN |
                           MBEDTLS_X509_KU_CRL_SIGN)
                        : MBEDTLS_X509_KU_DIGITAL_SIGNATURE) == 0;
    if (!prepared) {
        mbedtls_x509write_crt_free(&crt);
        return false;
    }
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &subject_key.value);
    mbedtls_x509write_crt_set_issuer_key(&crt, &issuer_key.value);
    std::vector<unsigned char> buffer(32 * 1024, 0);
    const int result = mbedtls_x509write_crt_pem(
        &crt, buffer.data(), buffer.size(), mbedtls_ctr_drbg_random, &rng.drbg);
    mbedtls_x509write_crt_free(&crt);
    if (result != 0) {
        mbedtls_platform_zeroize(buffer.data(), buffer.size());
        return false;
    }
    pem.assign(reinterpret_cast<const char*>(buffer.data()));
    mbedtls_platform_zeroize(buffer.data(), buffer.size());
    return pem.size() <= kMaxPemBytes && pem.find('\0') == std::string::npos;
}

bool VerifyCertificate(const Cert& cert, const Cert& ca) {
    std::uint32_t flags = 0;
    return mbedtls_x509_crt_verify(
               const_cast<mbedtls_x509_crt*>(&cert.value),
               const_cast<mbedtls_x509_crt*>(&ca.value), nullptr, nullptr,
               &flags, nullptr, nullptr) == 0 && flags == 0;
}

struct Writer final {
    std::vector<std::uint8_t> bytes;
    bool ok = true;

    void U8(std::uint8_t value) { bytes.push_back(value); }
    void U32(std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8)
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void U64(std::uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8)
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void Raw(const std::uint8_t* value, std::size_t size) {
        if (size) bytes.insert(bytes.end(), value, value + size);
    }
    void Raw(std::string_view value) {
        Raw(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
    }
    void String(std::string_view value, std::size_t maximum) {
        if (value.size() > maximum || value.size() > UINT32_MAX) {
            ok = false;
            return;
        }
        U32(static_cast<std::uint32_t>(value.size()));
        Raw(value);
    }
};

struct Reader final {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t at = 0;
    bool ok = true;

    bool Need(std::size_t count) {
        if (count > size - std::min(size, at)) {
            ok = false;
            return false;
        }
        return true;
    }
    std::uint8_t U8() {
        if (!Need(1)) return 0;
        return data[at++];
    }
    std::uint32_t U32() {
        if (!Need(4)) return 0;
        std::uint32_t value = 0;
        for (int i = 0; i != 4; ++i) value = (value << 8) | data[at++];
        return value;
    }
    std::uint64_t U64() {
        if (!Need(8)) return 0;
        std::uint64_t value = 0;
        for (int i = 0; i != 8; ++i) value = (value << 8) | data[at++];
        return value;
    }
    bool Raw(std::uint8_t* out, std::size_t count) {
        if (!Need(count)) return false;
        std::memcpy(out, data + at, count);
        at += count;
        return true;
    }
    bool Magic(std::string_view magic) {
        if (!Need(magic.size())) return false;
        const bool same = std::memcmp(data + at, magic.data(), magic.size()) == 0;
        at += magic.size();
        return ok = ok && same;
    }
    std::string String(std::size_t maximum) {
        const std::uint32_t length = U32();
        if (!ok || length > maximum || !Need(length)) {
            ok = false;
            return {};
        }
        std::string value(reinterpret_cast<const char*>(data + at), length);
        at += length;
        if (value.find('\0') != std::string::npos) ok = false;
        return value;
    }
    bool Bytes(std::vector<std::uint8_t>& value, std::size_t maximum) {
        const std::uint32_t length = U32();
        if (!ok || length > maximum || !Need(length)) {
            ok = false;
            return false;
        }
        value.assign(data + at, data + at + length);
        at += length;
        return true;
    }
    bool Done() const { return ok && at == size; }
};

struct StringWiper final {
    std::string& value;
    ~StringWiper() {
        if (!value.empty()) mbedtls_platform_zeroize(value.data(), value.size());
    }
};

bool Base64UrlEncode(const std::vector<std::uint8_t>& input, std::string& output) {
    const std::size_t capacity = 4 * ((input.size() + 2) / 3) + 4;
    std::vector<unsigned char> encoded(capacity, 0);
    BufferWiper wipe{encoded.data(), encoded.size()};
    std::size_t written = 0;
    if (mbedtls_base64_encode(encoded.data(), encoded.size(), &written,
                              input.data(), input.size()) != 0) return false;
    output.assign(reinterpret_cast<const char*>(encoded.data()), written);
    for (char& c : output) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!output.empty() && output.back() == '=') output.pop_back();
    return true;
}

bool Base64UrlDecode(std::string_view input, std::vector<std::uint8_t>& output) {
    if (input.empty() || input.size() > 128 * 1024 || input.size() % 4 == 1)
        return false;
    std::string encoded(input);
    StringWiper wipe{encoded};
    for (char& c : encoded) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
        else if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9'))) return false;
    }
    while (encoded.size() % 4) encoded.push_back('=');
    output.assign(96 * 1024, 0);
    std::size_t written = 0;
    if (mbedtls_base64_decode(output.data(), output.size(), &written,
                              reinterpret_cast<const unsigned char*>(encoded.data()),
                              encoded.size()) != 0) {
        mbedtls_platform_zeroize(output.data(), output.size());
        output.clear();
        return false;
    }
    output.resize(written);
    return true;
}
struct ByteVectorWiper final {
    std::vector<std::uint8_t>& bytes;
    ~ByteVectorWiper() {
        if (!bytes.empty()) mbedtls_platform_zeroize(bytes.data(), bytes.size());
    }
};
template <std::size_t N>
struct ByteArrayWiper final {
    std::array<std::uint8_t, N>& bytes;
    ~ByteArrayWiper() { mbedtls_platform_zeroize(bytes.data(), bytes.size()); }
};

struct InvitationData final {
    std::array<std::uint8_t, 16> id{};
    std::array<std::uint8_t, 32> secret{};
    std::uint64_t expires = 0;
    std::uint64_t epoch = 0;
    std::string leader_id;
    std::string server_name;
    Fingerprint leader_pin{};
    std::string ca_pem;
    ~InvitationData() { mbedtls_platform_zeroize(secret.data(), secret.size()); }
};

bool EncodeInvitation(const InvitationData& data, std::string& code) {
    Writer writer;
    ByteVectorWiper wipe{writer.bytes};
    writer.Raw(kInviteMagic);
    writer.Raw(data.id.data(), data.id.size());
    writer.Raw(data.secret.data(), data.secret.size());
    writer.U64(data.expires);
    writer.U64(data.epoch);
    writer.String(data.leader_id, 64);
    writer.String(data.server_name, 253);
    writer.Raw(data.leader_pin.data(), data.leader_pin.size());
    writer.String(data.ca_pem, kMaxPemBytes);
    std::string encoded;
    StringWiper wipe_encoded{encoded};
    if (!writer.ok || !Base64UrlEncode(writer.bytes, encoded)) return false;
    code = "sp1." + encoded;
    return true;
}

bool DecodeInvitation(const std::string& code, InvitationData& out) {
    if (code.size() < 5 || code.size() > 128 * 1024 ||
        code.compare(0, 4, "sp1.") != 0) return false;
    std::vector<std::uint8_t> decoded;
    if (!Base64UrlDecode(std::string_view(code).substr(4), decoded)) return false;
    ByteVectorWiper wipe{decoded};
    Reader reader{decoded.data(), decoded.size()};
    if (!reader.Magic(kInviteMagic) ||
        !reader.Raw(out.id.data(), out.id.size()) ||
        !reader.Raw(out.secret.data(), out.secret.size())) return false;
    out.expires = reader.U64();
    out.epoch = reader.U64();
    out.leader_id = reader.String(64);
    out.server_name = reader.String(253);
    if (!reader.Raw(out.leader_pin.data(), out.leader_pin.size())) return false;
    out.ca_pem = reader.String(kMaxPemBytes);
    if (!reader.Done() || !out.expires || !out.epoch ||
        out.epoch > kMaxLeaderEpoch || out.leader_id.size() != 64 ||
        !IsDnsName(out.server_name) || !Nonzero(out.leader_pin)) return false;
    std::array<std::uint8_t, 32> parsed_id{};
    if (!Unhex(out.leader_id, parsed_id.data(), parsed_id.size())) return false;
    Cert ca;
    Fingerprint ca_pin{};
    return ParseCertificate(out.ca_pem, ca) &&
           mbedtls_x509_crt_get_ca_istrue(&ca.value) == 1 &&
           ComputeFingerprint(ca, ca_pin) && EqualSecret(ca_pin.data(), parsed_id.data(), 32);
}

std::uint64_t UnixNow() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    return seconds > 0 ? static_cast<std::uint64_t>(seconds) : 0;
}

struct InvitationRecord final {
    std::array<std::uint8_t, 16> id{};
    std::array<std::uint8_t, 32> secret_hash{};
    std::uint64_t expires = 0;
    std::uint8_t state = 0;  // issued, redeemed, expired, rejected
    std::string worker_id;
};

struct WorkerRecord final {
    std::string id;
    std::string label;
    std::uint8_t state = 0;  // pending, approved, rejected, revoked
    std::string public_key_pem;
    std::string certificate_pem;
    std::string invitation_id;
    std::uint64_t created = 0;
    Fingerprint spki{};
};

struct LeaderData final {
    std::string server_name;
    std::string leader_id;
    std::uint64_t epoch = 1;
    std::string ca_certificate;
    std::string ca_private_key;
    std::string server_certificate;
    std::string server_private_key;
    Fingerprint leader_pin{};
    std::vector<InvitationRecord> invitations;
    std::vector<WorkerRecord> workers;
};

struct WorkerData final {
    std::uint8_t state = 0;  // pending, paired
    std::string worker_id;
    std::string label;
    std::string private_key;
    std::string public_key;
    std::string certificate;
    std::string ca_certificate;
    std::string leader_id;
    std::uint64_t leader_epoch = 0;
    Fingerprint leader_pin{};
    std::string invitation_id;
    std::string server_name;
};

void SerializeLeader(const LeaderData& data, Writer& writer) {
    writer.Raw(kStateLeaderMagic);
    writer.String(data.server_name, 253);
    writer.String(data.leader_id, 64);
    writer.U64(data.epoch);
    writer.String(data.ca_certificate, kMaxPemBytes);
    writer.String(data.ca_private_key, kMaxPemBytes);
    writer.String(data.server_certificate, kMaxPemBytes);
    writer.String(data.server_private_key, kMaxPemBytes);
    writer.Raw(data.leader_pin.data(), data.leader_pin.size());
    writer.U32(static_cast<std::uint32_t>(data.invitations.size()));
    for (const InvitationRecord& item : data.invitations) {
        writer.Raw(item.id.data(), item.id.size());
        writer.Raw(item.secret_hash.data(), item.secret_hash.size());
        writer.U64(item.expires);
        writer.U8(item.state);
        writer.String(item.worker_id, 64);
    }
    writer.U32(static_cast<std::uint32_t>(data.workers.size()));
    for (const WorkerRecord& item : data.workers) {
        writer.String(item.id, 64);
        writer.String(item.label, kMaxLabelBytes);
        writer.U8(item.state);
        writer.String(item.public_key_pem, 16 * 1024);
        writer.String(item.certificate_pem, kMaxPemBytes);
        writer.String(item.invitation_id, 32);
        writer.U64(item.created);
        writer.Raw(item.spki.data(), item.spki.size());
    }
}

bool DeserializeLeader(const std::vector<std::uint8_t>& bytes, LeaderData& data) {
    Reader reader{bytes.data(), bytes.size()};
    if (!reader.Magic(kStateLeaderMagic)) return false;
    data.server_name = reader.String(253);
    data.leader_id = reader.String(64);
    data.epoch = reader.U64();
    data.ca_certificate = reader.String(kMaxPemBytes);
    data.ca_private_key = reader.String(kMaxPemBytes);
    data.server_certificate = reader.String(kMaxPemBytes);
    data.server_private_key = reader.String(kMaxPemBytes);
    if (!reader.Raw(data.leader_pin.data(), data.leader_pin.size())) return false;
    const std::uint32_t invite_count = reader.U32();
    if (!reader.ok || invite_count > kMaxInvitations) return false;
    data.invitations.reserve(invite_count);
    for (std::uint32_t i = 0; i != invite_count; ++i) {
        InvitationRecord item;
        if (!reader.Raw(item.id.data(), item.id.size()) ||
            !reader.Raw(item.secret_hash.data(), item.secret_hash.size())) return false;
        item.expires = reader.U64();
        item.state = reader.U8();
        item.worker_id = reader.String(64);
        if (!reader.ok || item.state > 3 || !item.expires) return false;
        data.invitations.push_back(std::move(item));
    }
    const std::uint32_t worker_count = reader.U32();
    if (!reader.ok || worker_count > kMaxWorkers) return false;
    data.workers.reserve(worker_count);
    for (std::uint32_t i = 0; i != worker_count; ++i) {
        WorkerRecord item;
        item.id = reader.String(64);
        item.label = reader.String(kMaxLabelBytes);
        item.state = reader.U8();
        item.public_key_pem = reader.String(16 * 1024);
        item.certificate_pem = reader.String(kMaxPemBytes);
        item.invitation_id = reader.String(32);
        item.created = reader.U64();
        if (!reader.Raw(item.spki.data(), item.spki.size())) return false;
        if (!reader.ok || item.state > 3 || item.id.size() != 64 ||
            item.invitation_id.size() != 32 || item.created == 0 ||
            !Nonzero(item.spki)) return false;
        data.workers.push_back(std::move(item));
    }
    if (!reader.Done() || data.epoch == 0 || data.epoch > kMaxLeaderEpoch ||
        !IsDnsName(data.server_name) ||
        data.leader_id.size() != 64 || !Nonzero(data.leader_pin)) return false;

    Rng rng;
    Pk ca_key, server_key;
    Cert ca, server;
    if (!rng.Init() || !ParseCertificate(data.ca_certificate, ca) ||
        !ParseCertificate(data.server_certificate, server) ||
        !ParsePrivateKey(data.ca_private_key, rng, ca_key) ||
        !ParsePrivateKey(data.server_private_key, rng, server_key) ||
        mbedtls_x509_crt_get_ca_istrue(&ca.value) != 1 ||
        !KeyMatches(ca, ca_key, rng) || !KeyMatches(server, server_key, rng) ||
        !VerifyCertificate(server, ca)) return false;
    Fingerprint ca_pin{}, server_pin{};
    if (!ComputeFingerprint(ca, ca_pin) || !ComputeFingerprint(server, server_pin) ||
        Hex(ca_pin.data(), ca_pin.size()) != data.leader_id ||
        !EqualSecret(server_pin.data(), data.leader_pin.data(), 32)) return false;
    std::set<std::string> ids;
    for (const WorkerRecord& item : data.workers) {
        if (!ids.insert(item.id).second || Hex(item.spki.data(), 32) != item.id)
            return false;
        Pk public_key;
        Fingerprint parsed{};
        if (!ParsePublicKey(item.public_key_pem, public_key) ||
            !ComputeFingerprint(public_key.value, parsed) ||
            !EqualSecret(parsed.data(), item.spki.data(), 32)) return false;
        if (item.state == 1 || item.state == 3) {
            Cert cert;
            if (!ParseCertificate(item.certificate_pem, cert) ||
                !VerifyCertificate(cert, ca) || !ComputeFingerprint(cert, parsed) ||
                !EqualSecret(parsed.data(), item.spki.data(), 32)) return false;
        } else if (!item.certificate_pem.empty()) {
            return false;
        }
    }
    return true;
}

void SerializeWorker(const WorkerData& data, Writer& writer) {
    writer.Raw(kStateWorkerMagic);
    writer.U8(data.state);
    writer.String(data.worker_id, 64);
    writer.String(data.label, kMaxLabelBytes);
    writer.String(data.private_key, kMaxPemBytes);
    writer.String(data.public_key, 16 * 1024);
    writer.String(data.certificate, kMaxPemBytes);
    writer.String(data.ca_certificate, kMaxPemBytes);
    writer.String(data.leader_id, 64);
    writer.U64(data.leader_epoch);
    writer.Raw(data.leader_pin.data(), data.leader_pin.size());
    writer.String(data.invitation_id, 32);
    writer.String(data.server_name, 253);
}

bool DeserializeWorker(const std::vector<std::uint8_t>& bytes, WorkerData& data) {
    Reader reader{bytes.data(), bytes.size()};
    if (!reader.Magic(kStateWorkerMagic)) return false;
    data.state = reader.U8();
    data.worker_id = reader.String(64);
    data.label = reader.String(kMaxLabelBytes);
    data.private_key = reader.String(kMaxPemBytes);
    data.public_key = reader.String(16 * 1024);
    data.certificate = reader.String(kMaxPemBytes);
    data.ca_certificate = reader.String(kMaxPemBytes);
    data.leader_id = reader.String(64);
    data.leader_epoch = reader.U64();
    if (!reader.Raw(data.leader_pin.data(), data.leader_pin.size())) return false;
    data.invitation_id = reader.String(32);
    data.server_name = reader.String(253);
    if (!reader.Done() || data.state < 1 || data.state > 2 ||
        data.worker_id.size() != 64 || data.leader_id.size() != 64 ||
        data.invitation_id.size() != 32 || data.leader_epoch == 0 ||
        data.leader_epoch > kMaxLeaderEpoch ||
        !Nonzero(data.leader_pin) || !IsDnsName(data.server_name)) return false;

    Rng rng;
    Pk key;
    Cert cert, ca;
    Fingerprint local_pin{}, ca_pin{};
    if (!rng.Init() || !ParsePrivateKey(data.private_key, rng, key) ||
        !ComputeFingerprint(key.value, local_pin) ||
        Hex(local_pin.data(), local_pin.size()) != data.worker_id ||
        !ParseCertificate(data.ca_certificate, ca) ||
        mbedtls_x509_crt_get_ca_istrue(&ca.value) != 1 ||
        !ComputeFingerprint(ca, ca_pin) || Hex(ca_pin.data(), ca_pin.size()) != data.leader_id)
        return false;
    if (data.state == 1) {
        Pk public_key;
        Fingerprint public_pin{};
        return data.certificate.empty() && !data.public_key.empty() &&
               ParsePublicKey(data.public_key, public_key) &&
               ComputeFingerprint(public_key.value, public_pin) &&
               EqualSecret(public_pin.data(), local_pin.data(), local_pin.size()) &&
               mbedtls_pk_check_pair(&public_key.value, &key.value,
                                     mbedtls_ctr_drbg_random, &rng.drbg) == 0;
    }
    Fingerprint cert_pin{};
    return !data.certificate.empty() && data.public_key.empty() &&
           ParseCertificate(data.certificate, cert) &&
           KeyMatches(cert, key, rng) && VerifyCertificate(cert, ca) &&
           ComputeFingerprint(cert, cert_pin) &&
           EqualSecret(cert_pin.data(), local_pin.data(), local_pin.size());
}

#ifdef _WIN32
struct PairingTokenUser final {
    HANDLE token = nullptr;
    std::vector<unsigned char> buffer;
    PSID sid = nullptr;
    ~PairingTokenUser() { if (token) CloseHandle(token); }
};

bool CurrentUser(PairingTokenUser& user) {
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &user.token)) return false;
    DWORD needed = 0;
    GetTokenInformation(user.token, TokenUser, nullptr, 0, &needed);
    if (!needed) return false;
    user.buffer.resize(needed);
    if (!GetTokenInformation(user.token, TokenUser, user.buffer.data(), needed, &needed))
        return false;
    user.sid = reinterpret_cast<TOKEN_USER*>(user.buffer.data())->User.Sid;
    return user.sid && IsValidSid(user.sid);
}
struct MachineSids {
    std::array<unsigned char, SECURITY_MAX_SID_SIZE> administrators{};
    std::array<unsigned char, SECURITY_MAX_SID_SIZE> system{};
    std::vector<unsigned char> service;

    bool Load() {
        DWORD size = static_cast<DWORD>(administrators.size());
        if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
                                administrators.data(), &size)) return false;
        size = static_cast<DWORD>(system.size());
        if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system.data(), &size))
            return false;
        DWORD sid_size = 0, domain_size = 0;
        SID_NAME_USE use{};
        LookupAccountNameW(nullptr, L"NT SERVICE\\SpirulaRemoteWorker", nullptr,
                           &sid_size, nullptr, &domain_size, &use);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !sid_size)
            return false;
        service.resize(sid_size);
        std::vector<wchar_t> domain(domain_size + 1);
        return LookupAccountNameW(nullptr, L"NT SERVICE\\SpirulaRemoteWorker",
                                  service.data(), &sid_size, domain.data(),
                                  &domain_size, &use) != FALSE;
    }

    bool Authorized(PSID current) {
        BOOL elevated = FALSE;
        return EqualSid(current, service.data()) ||
               EqualSid(current, system.data()) ||
               (CheckTokenMembership(nullptr, administrators.data(), &elevated) &&
                elevated);
    }

    bool Trusted(PSID sid) {
        return EqualSid(sid, administrators.data()) ||
               EqualSid(sid, system.data()) ||
               EqualSid(sid, service.data());
    }
};

bool MachineSecurity(PairingTokenUser& user, MachineSids& sids,
                     PSECURITY_DESCRIPTOR& descriptor, PACL& acl,
                     SECURITY_ATTRIBUTES& attributes) {
    if (!sids.Authorized(user.sid)) return false;
    EXPLICIT_ACCESSW entries[3]{};
    PSID principals[] = {sids.administrators.data(), sids.system.data(),
                         sids.service.data()};
    for (std::size_t i = 0; i != 3; ++i) {
        entries[i].grfAccessPermissions = FILE_ALL_ACCESS;
        entries[i].grfAccessMode = SET_ACCESS;
        entries[i].grfInheritance = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
        entries[i].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entries[i].Trustee.TrusteeType =
            i == 0 ? TRUSTEE_IS_GROUP : TRUSTEE_IS_USER;
        entries[i].Trustee.ptstrName = reinterpret_cast<LPWSTR>(principals[i]);
    }
    if (SetEntriesInAclW(3, entries, nullptr, &acl) != ERROR_SUCCESS) return false;
    descriptor = static_cast<PSECURITY_DESCRIPTOR>(
        LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH));
    PSID owner = EqualSid(user.sid, sids.service.data())
        ? user.sid : principals[0];
    if (!descriptor || !InitializeSecurityDescriptor(
                           descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorOwner(descriptor, owner, FALSE) ||
        !SetSecurityDescriptorDacl(descriptor, TRUE, acl, FALSE) ||
        !SetSecurityDescriptorControl(descriptor, SE_DACL_PROTECTED,
                                      SE_DACL_PROTECTED)) return false;
    attributes = {};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    return true;
}

bool VerifyMachineHandle(HANDLE handle, MachineSids& sids,
                         bool directory, bool root = false) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        (((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory))
        return false;
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD result = GetSecurityInfo(
        handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION |
                                    DACL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, nullptr, &descriptor);
    if (result != ERROR_SUCCESS || !descriptor) {
        if (descriptor) LocalFree(descriptor);
        return false;
    }
    bool safe = owner && dacl &&
                (root ? (EqualSid(owner, sids.administrators.data()) ||
                         EqualSid(owner, sids.system.data()))
                      : sids.Trusted(owner));
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    if (!root &&
        (!GetSecurityDescriptorControl(descriptor, &control, &revision) ||
         !(control & SE_DACL_PROTECTED))) safe = false;
    bool admin_access = false, service_access = false;
    ACL_SIZE_INFORMATION info_acl{};
    if (!safe || !GetAclInformation(dacl, &info_acl, sizeof(info_acl),
                                    AclSizeInformation)) safe = false;
    for (DWORD i = 0; safe && i < info_acl.AceCount; ++i) {
        void* raw = nullptr;
        if (!GetAce(dacl, i, &raw)) { safe = false; break; }
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        if (header->AceType == ACCESS_DENIED_ACE_TYPE ||
            header->AceType == SYSTEM_AUDIT_ACE_TYPE) continue;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            safe = false;
            break;
        }
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
        PSID sid = const_cast<DWORD*>(&ace->SidStart);
        if (EqualSid(sid, sids.administrators.data())) admin_access = true;
        else if (EqualSid(sid, sids.service.data())) service_access = true;
        else if (!EqualSid(sid, sids.system.data())) {
            if (!root) { safe = false; break; }
            if ((header->AceFlags & INHERIT_ONLY_ACE) &&
                (IsWellKnownSid(sid, WinCreatorOwnerSid) ||
                 IsWellKnownSid(sid, WinCreatorGroupSid))) continue;
            ACCESS_MASK rights = ace->Mask;
            GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE,
                                    FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
            MapGenericMask(&rights, &mapping);
            constexpr ACCESS_MASK write_rights =
                FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_ADD_FILE |
                FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD |
                FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | DELETE |
                WRITE_DAC | WRITE_OWNER;
            if (rights & write_rights) safe = false;
        }
    }
    LocalFree(descriptor);
    return safe && (root || (admin_access && service_access));
}

bool PrivateSecurity(PairingTokenUser& user, PSECURITY_DESCRIPTOR& descriptor,
                     PACL& acl, SECURITY_ATTRIBUTES& attributes) {
    PSID system_sid = nullptr;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    if (!AllocateAndInitializeSid(&authority, 1, SECURITY_LOCAL_SYSTEM_RID,
                                  0, 0, 0, 0, 0, 0, 0, &system_sid)) return false;
    EXPLICIT_ACCESSW entries[2]{};
    const DWORD count = EqualSid(user.sid, system_sid) ? 1 : 2;
    for (DWORD i = 0; i != count; ++i) {
        EXPLICIT_ACCESSW& entry = entries[i];
        entry.grfAccessPermissions = FILE_ALL_ACCESS;
        entry.grfAccessMode = SET_ACCESS;
        entry.grfInheritance = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
        entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(i == 0 ? user.sid : system_sid);
    }
    const DWORD acl_result = SetEntriesInAclW(count, entries, nullptr, &acl);
    FreeSid(system_sid);
    if (acl_result != ERROR_SUCCESS) return false;
    descriptor = static_cast<PSECURITY_DESCRIPTOR>(
        LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH));
    if (!descriptor || !InitializeSecurityDescriptor(descriptor,
                                                      SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorOwner(descriptor, user.sid, FALSE) ||
        !SetSecurityDescriptorDacl(descriptor, TRUE, acl, FALSE) ||
        !SetSecurityDescriptorControl(descriptor, SE_DACL_PROTECTED,
                                      SE_DACL_PROTECTED)) return false;
    attributes = {};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    return true;
}

void FreeSecurity(PSECURITY_DESCRIPTOR descriptor, PACL acl) {
    if (acl) LocalFree(acl);
    if (descriptor) LocalFree(descriptor);
}

bool VerifyPrivateHandle(HANDLE handle, PSID user_sid, bool directory) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        (((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory))
        return false;
    PSID owner = nullptr;
    PACL dacl = nullptr;
    const DWORD result = GetSecurityInfo(handle, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, nullptr, nullptr);
    if (result != ERROR_SUCCESS || !owner || !dacl ||
        (!EqualSid(owner, user_sid))) return false;
    ACL_SIZE_INFORMATION acl_info{};
    if (!GetAclInformation(dacl, &acl_info, sizeof(acl_info), AclSizeInformation))
        return false;
    PSID system_sid = nullptr;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    if (!AllocateAndInitializeSid(&authority, 1, SECURITY_LOCAL_SYSTEM_RID,
                                  0, 0, 0, 0, 0, 0, 0, &system_sid)) return false;
    bool safe = false;
    bool user_access = false;
    for (DWORD i = 0; i < acl_info.AceCount; ++i) {
        void* raw = nullptr;
        if (!GetAce(dacl, i, &raw)) { safe = false; break; }
        const ACE_HEADER* header = static_cast<ACE_HEADER*>(raw);
        if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
            PSID sid = const_cast<DWORD*>(&ace->SidStart);
            if (!EqualSid(sid, user_sid) && !EqualSid(sid, system_sid)) {
                safe = false;
                break;
            }
            if (EqualSid(sid, user_sid)) user_access = true;
            safe = true;
        } else if (header->AceType != ACCESS_DENIED_ACE_TYPE &&
                   header->AceType != SYSTEM_AUDIT_ACE_TYPE) {
            safe = false;
            break;
        }
    }
    FreeSid(system_sid);
    return safe && user_access;
}
#else
struct PrivateDirectory final {
    int fd = -1;
    int lock_fd = -1;
    ~PrivateDirectory() {
        if (lock_fd >= 0) {
            ::flock(lock_fd, LOCK_UN);
            ::close(lock_fd);
        }
        if (fd >= 0) ::close(fd);
    }
};
#endif

class SecureStore final {
public:
    SecureStore() = default;
    SecureStore(const SecureStore&) = delete;
    SecureStore& operator=(const SecureStore&) = delete;
    ~SecureStore() {
#ifdef _WIN32
        if (lock_handle_ != INVALID_HANDLE_VALUE) {
            UnlockFileEx(lock_handle_, 0, 1, 0, &lock_overlapped_);
            CloseHandle(lock_handle_);
        }
#endif
    }
    bool Open(const fs::path& root, std::string* error, bool machine = false) {
        if (!root.is_absolute()) {
            SetError(error, "pairing storage root must be absolute");
            return false;
        }
#ifndef _WIN32
        if (machine) {
            SetError(error, "machine worker pairing requires Windows service permissions");
            return false;
        }
#endif
#ifdef _WIN32
        PairingTokenUser user;
        if (!CurrentUser(user)) {
            SetError(error, "could not determine pairing storage identity");
            return false;
        }
        MachineSids sids;
        if (machine && (!sids.Load() || !sids.Authorized(user.sid))) {
            SetError(error, "machine pairing requires the installed service or an elevated administrator");
            return false;
        }
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        PACL acl = nullptr;
        SECURITY_ATTRIBUTES attributes{};
        if (!(machine ? MachineSecurity(user, sids, descriptor, acl, attributes)
                      : PrivateSecurity(user, descriptor, acl, attributes))) {
            FreeSecurity(descriptor, acl);
            SetError(error, "could not create restricted pairing storage permissions");
            return false;
        }
        const DWORD root_attributes = GetFileAttributesW(root.c_str());
        if (root_attributes == INVALID_FILE_ATTRIBUTES ||
            !(root_attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (root_attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            FreeSecurity(descriptor, acl);
            SetError(error, "pairing storage root is unsafe");
            return false;
        }
        if (machine) {
            HANDLE root_handle = CreateFileW(root.c_str(), READ_CONTROL,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr);
            const bool trusted = root_handle != INVALID_HANDLE_VALUE &&
                VerifyMachineHandle(root_handle, sids, true, true);
            if (root_handle != INVALID_HANDLE_VALUE) CloseHandle(root_handle);
            if (!trusted) {
                FreeSecurity(descriptor, acl);
                SetError(error, "machine pairing storage root is not administrator-owned");
                return false;
            }
        }
        const fs::path directory = root / L"agent-pairing";
        if (!CreateDirectoryW(directory.c_str(), &attributes) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            FreeSecurity(descriptor, acl);
            SetError(error, "could not create pairing storage directory");
            return false;
        }
        HANDLE handle = CreateFileW(directory.c_str(), READ_CONTROL,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        const bool safe = handle != INVALID_HANDLE_VALUE &&
            (machine ? VerifyMachineHandle(handle, sids, true)
                     : VerifyPrivateHandle(handle, user.sid, true));
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        if (!safe) {
            FreeSecurity(descriptor, acl);
            SetError(error, "pairing storage directory is not private");
            return false;
        }
        const fs::path lock_path = directory / L"pairing.lock";
        lock_overlapped_ = {};
        HANDLE lock_handle = CreateFileW(lock_path.c_str(),
            GENERIC_READ | GENERIC_WRITE | READ_CONTROL,
            FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        const bool locked = lock_handle != INVALID_HANDLE_VALUE &&
            (machine ? VerifyMachineHandle(lock_handle, sids, false)
                     : VerifyPrivateHandle(lock_handle, user.sid, false)) &&
            LockFileEx(lock_handle, LOCKFILE_EXCLUSIVE_LOCK |
                       LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &lock_overlapped_);
        if (!locked) {
            if (lock_handle != INVALID_HANDLE_VALUE) CloseHandle(lock_handle);
            FreeSecurity(descriptor, acl);
            SetError(error, "pairing storage is already open or its lock is unsafe");
            return false;
        }
        lock_handle_ = lock_handle;
        FreeSecurity(descriptor, acl);
        directory_ = directory;
        machine_ = machine;
#else
        struct stat root_stat{};
        if (::lstat(root.c_str(), &root_stat) != 0 || !S_ISDIR(root_stat.st_mode) ||
            S_ISLNK(root_stat.st_mode) || root_stat.st_uid != ::geteuid() ||
            (root_stat.st_mode & 0022) != 0) {
            SetError(error, "pairing storage root is unsafe");
            return false;
        }
        int root_fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (root_fd < 0) {
            SetError(error, "could not open pairing storage root");
            return false;
        }
        if (::mkdirat(root_fd, "agent-pairing", 0700) != 0 && errno != EEXIST) {
            ::close(root_fd);
            SetError(error, "could not create pairing storage directory");
            return false;
        }
        int child = ::openat(root_fd, "agent-pairing",
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        ::close(root_fd);
        struct stat st{};
        if (child < 0 || ::fstat(child, &st) != 0 || !S_ISDIR(st.st_mode) ||
            st.st_uid != ::geteuid() || (st.st_mode & 0077) != 0) {
            if (child >= 0) ::close(child);
            SetError(error, "pairing storage directory is not private");
            return false;
        }
        directory_.fd = child;
        int lock_fd = ::openat(child, "pairing.lock",
            O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        struct stat lock_stat{};
        if (lock_fd < 0 || ::fstat(lock_fd, &lock_stat) != 0 ||
            !S_ISREG(lock_stat.st_mode) || lock_stat.st_uid != ::geteuid() ||
            ::fchmod(lock_fd, 0600) != 0) {
            if (lock_fd >= 0) ::close(lock_fd);
            SetError(error, "pairing storage lock is unsafe");
            return false;
        }
        int lock_result;
        do {
            lock_result = ::flock(lock_fd, LOCK_EX | LOCK_NB);
        } while (lock_result != 0 && errno == EINTR);
        if (lock_result != 0) {
            ::close(lock_fd);
            SetError(error, "pairing storage is already open or its lock is unsafe");
            return false;
        }
        directory_.lock_fd = lock_fd;
#endif
        return true;
    }

    enum class ReadResult { Ok, Missing, Error };

    ReadResult Read(const char* name, std::vector<std::uint8_t>& bytes,
                    std::string* error) const {
#ifdef _WIN32
        PairingTokenUser user;
        if (!CurrentUser(user)) {
            SetError(error, "could not inspect pairing storage identity");
            return ReadResult::Error;
        }
        const fs::path file = directory_ / name;
        HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ | READ_CONTROL,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND) return ReadResult::Missing;
            SetError(error, "could not read pairing state");
            return ReadResult::Error;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        MachineSids sids;
        const bool safe = (!machine_ || sids.Load()) &&
            (machine_ ? VerifyMachineHandle(handle, sids, false)
                      : VerifyPrivateHandle(handle, user.sid, false)) &&
            GetFileInformationByHandle(handle, &info) &&
            (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32 | info.nFileSizeLow)
                <= kMaxStateBytes;
        if (!safe) {
            CloseHandle(handle);
            SetError(error, "pairing state file is not private or is oversized");
            return ReadResult::Error;
        }
        const std::size_t length = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32 |
                                    info.nFileSizeLow);
        bytes.resize(length);
        DWORD read = 0;
        const bool ok = length == 0 ||
            (ReadFile(handle, bytes.data(), static_cast<DWORD>(length), &read, nullptr) &&
             read == length);
        CloseHandle(handle);
        if (!ok) {
            SetError(error, "could not read pairing state");
            bytes.clear();
            return ReadResult::Error;
        }
#else
        int fd = ::openat(directory_.fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            if (errno == ENOENT) return ReadResult::Missing;
            SetError(error, "could not read pairing state");
            return ReadResult::Error;
        }
        struct stat st{};
        if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
            st.st_uid != ::geteuid() || (st.st_mode & 0077) != 0 ||
            st.st_size < 0 || static_cast<std::uint64_t>(st.st_size) > kMaxStateBytes) {
            ::close(fd);
            SetError(error, "pairing state file is not private or is oversized");
            return ReadResult::Error;
        }
        bytes.resize(static_cast<std::size_t>(st.st_size));
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t got = ::read(fd, bytes.data() + offset, bytes.size() - offset);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) break;
            offset += static_cast<std::size_t>(got);
        }
        ::close(fd);
        if (offset != bytes.size()) {
            SetError(error, "could not read pairing state");
            bytes.clear();
            return ReadResult::Error;
        }
#endif
        return ReadResult::Ok;
    }

    bool Write(const char* name, const std::vector<std::uint8_t>& bytes,
               std::string* error) const {
        if (bytes.empty() || bytes.size() > kMaxStateBytes) {
            SetError(error, "pairing state is outside the permitted size");
            return false;
        }
        static std::atomic<std::uint64_t> sequence{0};
#ifdef _WIN32
        PairingTokenUser user;
        if (!CurrentUser(user)) {
            SetError(error, "could not inspect pairing storage identity");
            return false;
        }
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        PACL acl = nullptr;
        SECURITY_ATTRIBUTES attributes{};
        MachineSids sids;
        if (machine_ && !sids.Load()) {
            SetError(error, "machine pairing service identity is unavailable");
            return false;
        }
        if (!(machine_ ? MachineSecurity(user, sids, descriptor, acl, attributes)
                       : PrivateSecurity(user, descriptor, acl, attributes))) {
            FreeSecurity(descriptor, acl);
            SetError(error, "could not create restricted pairing file permissions");
            return false;
        }
        fs::path temporary;
        HANDLE handle = INVALID_HANDLE_VALUE;
        for (int tries = 0; tries != 32 && handle == INVALID_HANDLE_VALUE; ++tries) {
            temporary = directory_ / (std::wstring(name, name + std::strlen(name)) +
                L".tmp-" + std::to_wstring(sequence.fetch_add(1)));
            handle = CreateFileW(temporary.c_str(), GENERIC_WRITE | READ_CONTROL,
                0, &attributes, CREATE_NEW,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (handle == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS &&
                GetLastError() != ERROR_ALREADY_EXISTS) break;
        }
        if (handle == INVALID_HANDLE_VALUE) {
            FreeSecurity(descriptor, acl);
            SetError(error, "could not create restricted pairing state file");
            return false;
        }
        DWORD written = 0;
        bool ok = bytes.size() <= MAXDWORD &&
            WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                      nullptr) && written == bytes.size() && FlushFileBuffers(handle);
        CloseHandle(handle);
        if (ok) ok = MoveFileExW(temporary.c_str(), (directory_ / name).c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        if (!ok) DeleteFileW(temporary.c_str());
        FreeSecurity(descriptor, acl);
        if (!ok) SetError(error, "could not persist pairing state");
        return ok;
#else
        std::string temporary = std::string(name) + ".tmp-" +
                                std::to_string(sequence.fetch_add(1));
        int fd = -1;
        for (int tries = 0; tries != 32 && fd < 0; ++tries) {
            temporary = std::string(name) + ".tmp-" +
                        std::to_string(sequence.fetch_add(1));
            fd = ::openat(directory_.fd, temporary.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (fd < 0 && errno != EEXIST) break;
        }
        if (fd < 0) {
            SetError(error, "could not create restricted pairing state file");
            return false;
        }
        bool ok = ::fchmod(fd, 0600) == 0;
        std::size_t offset = 0;
        while (ok && offset < bytes.size()) {
            const ssize_t written = ::write(fd, bytes.data() + offset,
                                            bytes.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { ok = false; break; }
            offset += static_cast<std::size_t>(written);
        }
        if (ok) ok = ::fsync(fd) == 0;
        ::close(fd);
        if (ok) ok = ::renameat(directory_.fd, temporary.c_str(),
                                directory_.fd, name) == 0;
        if (ok) ok = ::fsync(directory_.fd) == 0;
        if (!ok) ::unlinkat(directory_.fd, temporary.c_str(), 0);
        if (!ok) SetError(error, "could not persist pairing state");
        return ok;
#endif
    }

    bool Remove(const char* name, std::string* error) const {
#ifdef _WIN32
        PairingTokenUser user;
        if (!CurrentUser(user)) {
            SetError(error, "could not inspect pairing storage identity");
            return false;
        }
        const fs::path file = directory_ / name;
        HANDLE handle = CreateFileW(file.c_str(), DELETE | READ_CONTROL,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND ||
                GetLastError() == ERROR_PATH_NOT_FOUND) return true;
            SetError(error, "could not remove pairing state");
            return false;
        }
        FILE_DISPOSITION_INFO disposition{TRUE};
        MachineSids sids;
        const bool removed = (!machine_ || sids.Load()) &&
            (machine_ ? VerifyMachineHandle(handle, sids, false)
                      : VerifyPrivateHandle(handle, user.sid, false)) &&
            SetFileInformationByHandle(handle, FileDispositionInfo, &disposition,
                                       sizeof(disposition)) != 0;
        CloseHandle(handle);
        if (!removed) SetError(error, "pairing state file is not private or could not be removed");
        return removed;
#else
        struct stat st{};
        if (::fstatat(directory_.fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) return true;
            SetError(error, "could not inspect pairing state before removal");
            return false;
        }
        if (!S_ISREG(st.st_mode) || st.st_uid != ::geteuid() ||
            (st.st_mode & 0077) != 0 ||
            ::unlinkat(directory_.fd, name, 0) != 0 ||
            ::fsync(directory_.fd) != 0) {
            SetError(error, "pairing state file is not private or could not be removed");
            return false;
        }
        return true;
#endif
    }
private:
#ifdef _WIN32
    fs::path directory_;
    bool machine_ = false;
    HANDLE lock_handle_ = INVALID_HANDLE_VALUE;
    OVERLAPPED lock_overlapped_{};
#else
    PrivateDirectory directory_;
#endif
};

bool SaveLeader(const SecureStore& store, const LeaderData& data,
                std::string* error) {
    Writer writer;
    SerializeLeader(data, writer);
    const bool ok = writer.ok && store.Write(kLeaderFile, writer.bytes, error);
    if (!writer.bytes.empty()) mbedtls_platform_zeroize(writer.bytes.data(), writer.bytes.size());
    if (!writer.ok) SetError(error, "pairing state exceeded its bounds");
    return ok;
}

bool SaveWorker(const SecureStore& store, const WorkerData& data,
                std::string* error) {
    Writer writer;
    SerializeWorker(data, writer);
    const bool ok = writer.ok && store.Write(kWorkerFile, writer.bytes, error);
    if (!writer.bytes.empty()) mbedtls_platform_zeroize(writer.bytes.data(), writer.bytes.size());
    if (!writer.ok) SetError(error, "pairing state exceeded its bounds");
    return ok;
}

bool CreateLeaderData(std::string server_name, LeaderData& data) {
    if (!IsDnsName(server_name)) return false;
    Rng rng;
    if (!rng.Init()) return false;
    Pk ca_key, server_key;
    if (!GenerateKey(rng, ca_key, data.ca_private_key) ||
        !GenerateKey(rng, server_key, data.server_private_key)) return false;
    if (!WriteCertificate(rng, "CN=Spirula Agent Pairing CA",
                          "CN=Spirula Agent Pairing CA", ca_key, ca_key, true,
                          data.ca_certificate) ||
        !WriteCertificate(rng, "CN=" + server_name,
                          "CN=Spirula Agent Pairing CA", server_key, ca_key, false,
                          data.server_certificate)) return false;
    Cert ca, server;
    Fingerprint ca_fingerprint{};
    if (!ParseCertificate(data.ca_certificate, ca) ||
        !ParseCertificate(data.server_certificate, server) ||
        !ComputeFingerprint(ca, ca_fingerprint) || !ComputeFingerprint(server, data.leader_pin))
        return false;
    // The stable ID is the CA SPKI; the TLS pin is the separate server SPKI.
    data.leader_id = Hex(ca_fingerprint.data(), ca_fingerprint.size());
    data.server_name = std::move(server_name);
    return true;
}

std::string InvitationId(const std::array<std::uint8_t, 16>& id) {
    return Hex(id.data(), id.size());
}

InvitationRecord* FindInvitation(LeaderData& data, std::string_view id) {
    for (InvitationRecord& invite : data.invitations)
        if (InvitationId(invite.id) == id) return &invite;
    return nullptr;
}

const InvitationRecord* FindInvitation(const LeaderData& data,
                                       std::string_view id) {
    for (const InvitationRecord& invite : data.invitations)
        if (InvitationId(invite.id) == id) return &invite;
    return nullptr;
}

WorkerRecord* FindWorker(LeaderData& data, const std::string& id) {
    for (WorkerRecord& worker : data.workers)
        if (worker.id == id) return &worker;
    return nullptr;
}

const WorkerRecord* FindWorker(const LeaderData& data, const std::string& id) {
    for (const WorkerRecord& worker : data.workers)
        if (worker.id == id) return &worker;
    return nullptr;
}

bool ActiveWorkerCount(const LeaderData& data, std::size_t& count) {
    count = 0;
    for (const WorkerRecord& worker : data.workers)
        if (worker.state == 1) ++count;
    return count <= kMaxActiveWorkers;
}

bool Sign(Rng& rng, Pk& key, const std::vector<std::uint8_t>& message,
          std::vector<std::uint8_t>& signature) {
    std::array<std::uint8_t, 32> digest{};
    if (!Hash(message.data(), message.size(), digest)) return false;
    signature.assign(256, 0);
    std::size_t length = 0;
    if (mbedtls_pk_sign(&key.value, MBEDTLS_MD_SHA256, digest.data(), digest.size(),
                        signature.data(), signature.size(), &length,
                        mbedtls_ctr_drbg_random, &rng.drbg) != 0) return false;
    signature.resize(length);
    return true;
}

bool VerifySignature(Pk& key, const std::vector<std::uint8_t>& message,
                     const std::vector<std::uint8_t>& signature) {
    std::array<std::uint8_t, 32> digest{};
    return Hash(message.data(), message.size(), digest) &&
           mbedtls_pk_verify(&key.value, MBEDTLS_MD_SHA256, digest.data(),
                             digest.size(), signature.data(), signature.size()) == 0;
}

bool ValidWorkerIdentity(std::string_view id) {
    if (id.size() != 64) return false;
    for (char c : id)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

bool LoadUpdateSigner(const SecureStore& store, const std::string& ca_pem,
                      bool create_if_missing, Rng& rng, Pk& private_key,
                      std::string& public_pem, Fingerprint& fingerprint,
                      std::string* error) {
    if (!rng.Init()) {
        SetError(error, "could not initialize update signer randomness");
        return false;
    }
    std::vector<std::uint8_t> bytes;
    ByteVectorWiper wipe_bytes{bytes};
    const auto read = store.Read(kUpdateSignerFile, bytes, error);
    if (read == SecureStore::ReadResult::Error) return false;

    Pk public_key;
    std::string private_pem;
    StringWiper wipe_private{private_pem};
    if (read == SecureStore::ReadResult::Missing) {
        if (!create_if_missing) {
            SetError(error, "update signer has not been initialized");
            return false;
        }
        if (!GenerateKey(rng, private_key, private_pem, &public_pem) ||
            !ParsePublicKey(public_pem, public_key) ||
            !mbedtls_pk_can_do(&private_key.value, MBEDTLS_PK_ECDSA) ||
            mbedtls_pk_get_bitlen(&private_key.value) != 256 ||
            mbedtls_pk_check_pair(&public_key.value, &private_key.value,
                                  mbedtls_ctr_drbg_random, &rng.drbg) != 0) {
            SetError(error, "could not generate a P-256 update signer");
            return false;
        }
        Writer record;
        record.Raw(kUpdateSignerMagic);
        record.String(private_pem, kMaxPemBytes);
        record.String(public_pem, kMaxPemBytes);
        ByteVectorWiper wipe_record{record.bytes};
        if (!record.ok || !ComputeFingerprint(public_key.value, fingerprint)) {
            SetError(error, "could not encode the update signer");
            return false;
        }
        Cert ca;
        Fingerprint ca_fingerprint{};
        if (!ParseCertificate(ca_pem, ca) ||
            !ComputeFingerprint(ca, ca_fingerprint) ||
            EqualSecret(fingerprint.data(), ca_fingerprint.data(), fingerprint.size())) {
            SetError(error, "update signer must be independent of the enrollment CA");
            return false;
        }
        if (!store.Write(kUpdateSignerFile, record.bytes, error)) return false;
        return true;
    }

    Reader record{bytes.data(), bytes.size()};
    if (!record.Magic(kUpdateSignerMagic)) {
        SetError(error, "persisted update signer is invalid");
        return false;
    }
    private_pem = record.String(kMaxPemBytes);
    public_pem = record.String(kMaxPemBytes);
    if (!record.Done() || !ParsePrivateKey(private_pem, rng, private_key) ||
        !ParsePublicKey(public_pem, public_key) ||
        !mbedtls_pk_can_do(&private_key.value, MBEDTLS_PK_ECDSA) ||
        mbedtls_pk_get_bitlen(&private_key.value) != 256 ||
        mbedtls_pk_check_pair(&public_key.value, &private_key.value,
                              mbedtls_ctr_drbg_random, &rng.drbg) != 0 ||
        !ComputeFingerprint(public_key.value, fingerprint)) {
        SetError(error, "persisted update signer does not contain a valid P-256 key pair");
        return false;
    }
    Cert ca;
    Fingerprint ca_fingerprint{};
    if (!ParseCertificate(ca_pem, ca) ||
        !ComputeFingerprint(ca, ca_fingerprint) ||
        EqualSecret(fingerprint.data(), ca_fingerprint.data(), fingerprint.size())) {
        SetError(error, "persisted update signer reuses the enrollment CA identity");
        return false;
    }
    return true;
}

std::vector<std::uint8_t> ProofMessage(std::uint8_t operation,
                                       const std::array<std::uint8_t, 32>& challenge,
                                       const std::array<std::uint8_t, 16>& invitation,
                                       std::string_view worker_id) {
    Writer writer;
    writer.Raw(kSignatureDomain);
    writer.U8(operation);
    writer.Raw(challenge.data(), challenge.size());
    writer.Raw(invitation.data(), invitation.size());
    writer.String(worker_id, 64);
    return std::move(writer.bytes);
}


enum class ReplyState : std::uint8_t { Pending = 0, Approved = 1, Rejected = 2,
                                      Expired = 3, Consumed = 4, Revoked = 5 };

void BuildReply(ReplyState status, std::string_view worker_id,
                std::uint64_t epoch, std::string_view leader_id,
                std::string_view certificate, Writer& writer) {
    writer.Raw(kWireMagic);
    writer.U8(4);
    writer.U8(static_cast<std::uint8_t>(status));
    writer.String(worker_id, 64);
    writer.U64(epoch);
    writer.String(leader_id, 64);
    writer.String(certificate, kMaxPemBytes);
}

bool ParseReply(const std::vector<std::uint8_t>& bytes, ReplyState& state,
                std::string& worker_id, std::uint64_t& epoch,
                std::string& leader_id, std::string& certificate) {
    Reader reader{bytes.data(), bytes.size()};
    if (!reader.Magic(kWireMagic) || reader.U8() != 4) return false;
    const std::uint8_t raw_state = reader.U8();
    if (raw_state > static_cast<std::uint8_t>(ReplyState::Revoked)) return false;
    state = static_cast<ReplyState>(raw_state);
    worker_id = reader.String(64);
    epoch = reader.U64();
    leader_id = reader.String(64);
    certificate = reader.String(kMaxPemBytes);
    return reader.Done() && epoch != 0 && epoch <= kMaxLeaderEpoch &&
           leader_id.size() == 64 && (worker_id.empty() || worker_id.size() == 64);
}

bool EncodeRedeemRequest(const InvitationData& invitation, const WorkerData& worker,
                         const std::array<std::uint8_t, 32>& challenge,
                         std::vector<std::uint8_t>& signature,
                         Rng& rng, Pk& key, Writer& writer) {
    const auto message = ProofMessage(1, challenge, invitation.id, worker.worker_id);
    if (!Sign(rng, key, message, signature)) return false;
    writer.Raw(kWireMagic);
    writer.U8(1);
    writer.Raw(invitation.id.data(), invitation.id.size());
    writer.Raw(invitation.secret.data(), invitation.secret.size());
    writer.String(worker.worker_id, 64);
    writer.String(worker.label, kMaxLabelBytes);
    writer.String(worker.public_key, 16 * 1024);
    writer.String(std::string_view(reinterpret_cast<const char*>(signature.data()),
                                   signature.size()), 256);
    return writer.ok;
}

bool EncodeStatusRequest(const InvitationData& invitation, const WorkerData& worker,
                         const std::array<std::uint8_t, 32>& challenge,
                         std::vector<std::uint8_t>& signature,
                         Rng& rng, Pk& key, Writer& writer) {
    const auto message = ProofMessage(2, challenge, invitation.id, worker.worker_id);
    if (!Sign(rng, key, message, signature)) return false;
    writer.Raw(kWireMagic);
    writer.U8(2);
    writer.Raw(invitation.id.data(), invitation.id.size());
    writer.Raw(invitation.secret.data(), invitation.secret.size());
    writer.String(worker.worker_id, 64);
    writer.String(std::string_view(reinterpret_cast<const char*>(signature.data()),
                                   signature.size()), 256);
    return writer.ok;
}

#ifdef _WIN32
constexpr short kReadEvent = POLLRDNORM;
constexpr short kWriteEvent = POLLWRNORM;
#else
constexpr short kReadEvent = POLLIN;
constexpr short kWriteEvent = POLLOUT;
#endif

struct SocketTransport final {
    Socket socket = TlsChannel::kInvalidSocket;
    const std::atomic<bool>* canceled = nullptr;

#ifdef _WIN32
    bool winsock = false;
#endif

    ~SocketTransport() { Close(); }
    bool Start(Socket incoming, std::string* error) {
        socket = incoming;
        if (socket == TlsChannel::kInvalidSocket) {
            SetError(error, "invalid enrollment socket");
            return false;
        }
#ifdef _WIN32
        WSADATA data{};
        const int started = WSAStartup(MAKEWORD(2, 2), &data);
        if (started != 0 || LOBYTE(data.wVersion) != 2 || HIBYTE(data.wVersion) != 2) {
            if (started == 0) WSACleanup();
            SetError(error, "could not initialize enrollment socket transport");
            return false;
        }
        winsock = true;
        u_long enabled = 1;
        if (ioctlsocket(static_cast<SOCKET>(socket), FIONBIO, &enabled) != 0) {
            SetError(error, "could not configure enrollment socket");
            return false;
        }
#ifdef SO_NOSIGPIPE
        const int no_sigpipe = 1;
        if (setsockopt(static_cast<SOCKET>(socket), SOL_SOCKET, SO_NOSIGPIPE,
                       reinterpret_cast<const char*>(&no_sigpipe),
                       sizeof(no_sigpipe)) != 0) {
            SetError(error, "could not configure enrollment socket SIGPIPE policy");
            return false;
        }
#endif
#else
        if (socket > static_cast<Socket>(std::numeric_limits<int>::max())) {
            SetError(error, "invalid enrollment socket");
            return false;
        }
        const int flags = fcntl(static_cast<int>(socket), F_GETFL, 0);
        if (flags < 0 || fcntl(static_cast<int>(socket), F_SETFL,
                               flags | O_NONBLOCK) < 0) {
            SetError(error, "could not configure enrollment socket");
            return false;
        }
#ifdef SO_NOSIGPIPE
        const int no_sigpipe = 1;
        if (setsockopt(static_cast<int>(socket), SOL_SOCKET, SO_NOSIGPIPE,
                       &no_sigpipe, sizeof(no_sigpipe)) != 0) {
            SetError(error, "could not configure enrollment socket SIGPIPE policy");
            return false;
        }
#endif
#endif
        return true;
    }
    void Close() noexcept {
        if (socket == TlsChannel::kInvalidSocket) return;
#ifdef _WIN32
        closesocket(static_cast<SOCKET>(socket));
#else
        ::close(static_cast<int>(socket));
#endif
        socket = TlsChannel::kInvalidSocket;
#ifdef _WIN32
        if (winsock) { WSACleanup(); winsock = false; }
#endif
    }
    bool Wait(short event, std::chrono::steady_clock::time_point deadline) const {
        for (;;) {
            if (canceled && canceled->load(std::memory_order_relaxed)) return false;
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) return false;
#ifdef _WIN32
            WSAPOLLFD descriptor{};
            descriptor.fd = static_cast<SOCKET>(socket);
#else
            pollfd descriptor{};
            descriptor.fd = static_cast<int>(socket);
#endif
            descriptor.events = event;
            const auto timeout = static_cast<int>(std::min<std::int64_t>(
                remaining, canceled ? 100 : 60'000));
#ifdef _WIN32
            const int result = WSAPoll(&descriptor, 1, timeout);
#else
            const int result = ::poll(&descriptor, 1, timeout);
#endif
            if (canceled && canceled->load(std::memory_order_relaxed)) return false;
            if (result > 0)
                return (descriptor.revents & (event | POLLERR | POLLHUP)) != 0;
            if (result != 0 || !canceled) return false;
        }
    }
};

struct SocketGuard final {
    Socket socket = TlsChannel::kInvalidSocket;
    explicit SocketGuard(Socket incoming) : socket(incoming) {}
    ~SocketGuard() {
        if (socket == TlsChannel::kInvalidSocket) return;
#ifdef _WIN32
        closesocket(static_cast<SOCKET>(socket));
#else
        if (socket <= static_cast<Socket>(std::numeric_limits<int>::max()))
            ::close(static_cast<int>(socket));
#endif
    }
    void Release() noexcept { socket = TlsChannel::kInvalidSocket; }
};

int SocketSend(void* context, const unsigned char* data, std::size_t size) {
    auto* transport = static_cast<SocketTransport*>(context);
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
    auto* transport = static_cast<SocketTransport*>(context);
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

class EnrollmentTls final {
public:
    SocketTransport transport;
    Rng rng;
    Cert local_certificate;
    Pk local_key;
    Cert peer_ca;
    mbedtls_ssl_config config{};
    mbedtls_ssl_context ssl{};
    bool has_config = false;
    bool has_ssl = false;
    Fingerprint expected_pin{};
    bool pin_verified = false;

    EnrollmentTls() {
        mbedtls_ssl_config_init(&config);
        mbedtls_ssl_init(&ssl);
    }
    ~EnrollmentTls() {
        if (has_ssl) mbedtls_ssl_free(&ssl);
        if (has_config) mbedtls_ssl_config_free(&config);
    }
    EnrollmentTls(const EnrollmentTls&) = delete;
    EnrollmentTls& operator=(const EnrollmentTls&) = delete;

    static int VerifyPin(void* context, mbedtls_x509_crt* cert, int depth,
                         std::uint32_t* flags) {
        auto* self = static_cast<EnrollmentTls*>(context);
        if (!self || !cert || !flags) return MBEDTLS_ERR_X509_FATAL_ERROR;
        if (depth == 0) {
            Fingerprint actual{};
            self->pin_verified = ComputeFingerprint(cert->pk, actual) &&
                EqualSecret(actual.data(), self->expected_pin.data(), actual.size());
            if (!self->pin_verified) *flags |= MBEDTLS_X509_BADCERT_NOT_TRUSTED;
        }
        return 0;
    }

    bool Start(Socket socket, bool server, const std::string& local_cert,
               const std::string& local_pem_key, const std::string& ca_pem,
               const Fingerprint& pin, const std::string& server_name,
               std::string* error,
               const std::atomic<bool>* canceled = nullptr) {
        if (!transport.Start(socket, error) || !rng.Init()) {
            SetError(error, "could not initialize enrollment TLS");
            return false;
        }
        transport.canceled = server ? canceled : nullptr;

        if (server &&
            (!ParseCertificate(local_cert, local_certificate) ||
             !ParsePrivateKey(local_pem_key, rng, local_key) ||
             !KeyMatches(local_certificate, local_key, rng))) {
            SetError(error, "invalid enrollment TLS identity");
            return false;
        }
        expected_pin = pin;
        pin_verified = false;
        if (!server) {
            if (!Nonzero(pin) || !ParseCertificate(ca_pem, peer_ca) ||
                mbedtls_x509_crt_get_ca_istrue(&peer_ca.value) != 1 ||
                !IsDnsName(server_name)) {
                SetError(error, "invalid pinned enrollment leader identity");
                return false;
            }
        }
        int result = mbedtls_ssl_config_defaults(
            &config, server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
            MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
        if (result != 0) {
            SetError(error, "could not configure enrollment TLS");
            return false;
        }
        has_config = true;
        mbedtls_ssl_conf_min_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_max_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_ciphersuites(&config, kTlsCiphers.data());
        mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &rng.drbg);
        if (server) {
            // The invitation token and worker-key proof authenticate enrollment clients.
            mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_NONE);
            result = mbedtls_ssl_conf_own_cert(&config, &local_certificate.value,
                                               &local_key.value);
            if (result != 0) {
                SetError(error, "could not configure enrollment TLS identity");
                return false;
            }
        } else {
            mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_REQUIRED);
            mbedtls_ssl_conf_ca_chain(&config, &peer_ca.value, nullptr);
            mbedtls_ssl_conf_verify(&config, VerifyPin, this);
        }
        if (mbedtls_ssl_setup(&ssl, &config) != 0) {
            SetError(error, "could not configure enrollment TLS identity");
            return false;
        }
        has_ssl = true;
        if (!server && mbedtls_ssl_set_hostname(&ssl, server_name.c_str()) != 0) {
            SetError(error, "could not configure enrollment server name");
            return false;
        }
        mbedtls_ssl_set_bio(&ssl, &transport, SocketSend, SocketReceive, nullptr);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kIoTimeoutMs);
        for (;;) {
            result = mbedtls_ssl_handshake(&ssl);
            if (result == 0) break;
            if (result != MBEDTLS_ERR_SSL_WANT_READ &&
                result != MBEDTLS_ERR_SSL_WANT_WRITE) {
                SetError(error, "pinned enrollment TLS handshake failed");
                return false;
            }
            const short event = result == MBEDTLS_ERR_SSL_WANT_READ
                ? kReadEvent : kWriteEvent;
            if (!transport.Wait(event, deadline)) {
                SetError(error, "enrollment TLS handshake timed out");
                return false;
            }
        }
        if (server) return true;
        if (mbedtls_ssl_get_verify_result(&ssl) != 0 || !pin_verified) {
            SetError(error, "enrollment leader certificate or SPKI pin failed");
            return false;
        }
        const mbedtls_x509_crt* peer = mbedtls_ssl_get_peer_cert(&ssl);
        if (!peer) {
            SetError(error, "enrollment leader did not present a certificate");
            return false;
        }
        return true;
    }

    bool WriteAll(const std::uint8_t* data, std::size_t size,
                  std::string* error) {
        std::size_t offset = 0;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kIoTimeoutMs);
        while (offset < size) {
            const int result = mbedtls_ssl_write(&ssl, data + offset, size - offset);
            if (result > 0) { offset += static_cast<std::size_t>(result); continue; }
            if ((result == MBEDTLS_ERR_SSL_WANT_READ ||
                 result == MBEDTLS_ERR_SSL_WANT_WRITE) &&
                transport.Wait(result == MBEDTLS_ERR_SSL_WANT_READ ? kReadEvent : kWriteEvent,
                               deadline)) continue;
            SetError(error, "enrollment TLS write failed");
            return false;
        }
        return true;
    }
    bool ReadAll(std::uint8_t* data, std::size_t size, std::string* error) {
        std::size_t offset = 0;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kIoTimeoutMs);
        while (offset < size) {
            const int result = mbedtls_ssl_read(&ssl, data + offset, size - offset);
            if (result > 0) { offset += static_cast<std::size_t>(result); continue; }
            if ((result == MBEDTLS_ERR_SSL_WANT_READ ||
                 result == MBEDTLS_ERR_SSL_WANT_WRITE) &&
                transport.Wait(result == MBEDTLS_ERR_SSL_WANT_READ ? kReadEvent : kWriteEvent,
                               deadline)) continue;
            SetError(error, "enrollment TLS read failed");
            return false;
        }
        return true;
    }
    bool SendFrame(const std::vector<std::uint8_t>& payload, std::string* error) {
        if (payload.empty() || payload.size() > kMaxFrameBytes) {
            SetError(error, "enrollment message exceeds its limit");
            return false;
        }
        const std::uint32_t size = static_cast<std::uint32_t>(payload.size());
        const std::array<std::uint8_t, 4> header = {
            static_cast<std::uint8_t>(size >> 24),
            static_cast<std::uint8_t>(size >> 16),
            static_cast<std::uint8_t>(size >> 8),
            static_cast<std::uint8_t>(size),
        };
        return WriteAll(header.data(), header.size(), error) &&
               WriteAll(payload.data(), payload.size(), error);
    }
    bool ReceiveFrame(std::vector<std::uint8_t>& payload, std::string* error) {
        std::array<std::uint8_t, 4> header{};
        if (!ReadAll(header.data(), header.size(), error)) return false;
        const std::uint32_t size = (std::uint32_t(header[0]) << 24) |
                                   (std::uint32_t(header[1]) << 16) |
                                   (std::uint32_t(header[2]) << 8) | header[3];
        if (size == 0 || size > kMaxFrameBytes) {
            SetError(error, "invalid enrollment message size");
            return false;
        }
        payload.resize(size);
        return ReadAll(payload.data(), payload.size(), error);
    }
};

bool MakeChallenge(EnrollmentTls& tls, std::array<std::uint8_t, 32>& challenge,
                   std::string* error) {
    if (!tls.rng.Fill(challenge.data(), challenge.size())) {
        SetError(error, "could not create enrollment challenge");
        return false;
    }
    Writer writer;
    writer.Raw(kWireMagic);
    writer.U8(3);
    writer.Raw(challenge.data(), challenge.size());
    return tls.SendFrame(writer.bytes, error);
}

bool ReadChallenge(EnrollmentTls& tls, std::array<std::uint8_t, 32>& challenge,
                   std::string* error) {
    std::vector<std::uint8_t> frame;
    if (!tls.ReceiveFrame(frame, error)) return false;
    Reader reader{frame.data(), frame.size()};
    if (!reader.Magic(kWireMagic) || reader.U8() != 3 ||
        !reader.Raw(challenge.data(), challenge.size()) || !reader.Done()) {
        SetError(error, "invalid enrollment challenge");
        return false;
    }
    return true;
}

bool ParseRequestHeader(Reader& reader, std::uint8_t& type,
                        std::array<std::uint8_t, 16>& invite_id,
                        std::array<std::uint8_t, 32>& secret,
                        std::string& worker_id, std::string& label,
                        std::string& public_key, std::vector<std::uint8_t>& signature) {
    if (!reader.Magic(kWireMagic)) return false;
    type = reader.U8();
    if ((type != 1 && type != 2) ||
        !reader.Raw(invite_id.data(), invite_id.size()) ||
        !reader.Raw(secret.data(), secret.size())) return false;
    worker_id = reader.String(64);
    if (type == 1) {
        label = reader.String(kMaxLabelBytes);
        public_key = reader.String(16 * 1024);
    }
    // DER signatures are opaque bytes and may contain NULs.
    if (!reader.Bytes(signature, 256)) return false;
    return reader.Done() && worker_id.size() == 64 && !signature.empty() &&
           (type != 1 || (!public_key.empty() && label.size() <= kMaxLabelBytes));
}

bool PrepareApprovedWorker(const WorkerData& worker, std::uint64_t epoch,
                           std::string certificate, WorkerData& next,
                           std::string* error) {
    if (epoch != worker.leader_epoch || certificate.empty()) {
        SetError(error, "leader approval response omitted a valid worker certificate");
        return false;
    }
    Rng rng;
    Pk private_key;
    Cert cert, ca;
    Fingerprint worker_pin{};
    if (!rng.Init() || !ParsePrivateKey(worker.private_key, rng, private_key) ||
        !ParseCertificate(certificate, cert) ||
        !ParseCertificate(worker.ca_certificate, ca) ||
        !KeyMatches(cert, private_key, rng) || !VerifyCertificate(cert, ca) ||
        !ComputeFingerprint(cert, worker_pin) ||
        Hex(worker_pin.data(), worker_pin.size()) != worker.worker_id) {
        SetError(error, "approved worker certificate did not match its private identity");
        return false;
    }
    next = worker;
    next.state = 2;
    next.certificate = std::move(certificate);
    next.public_key.clear();
    return true;
}

}  // namespace

struct Leader::Impl final {
    SecureStore store;
    LeaderData data;
    mutable std::mutex mutex;
};

struct Worker::Impl final {
    SecureStore store;
    WorkerData data;
    bool has_state = false;
    mutable std::mutex mutex;
};

Leader::Leader(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Leader::Leader(Leader&&) noexcept = default;
Leader& Leader::operator=(Leader&&) noexcept = default;
Leader::~Leader() = default;

std::optional<Leader> Leader::Open(const fs::path& state_root,
                                   std::string server_name,
                                   std::string* error) {
    auto impl = std::make_unique<Impl>();
    if (!impl->store.Open(state_root, error)) return std::nullopt;
    std::vector<std::uint8_t> bytes;
    ByteVectorWiper wipe_state{bytes};
    const auto read = impl->store.Read(kLeaderFile, bytes, error);
    if (read == SecureStore::ReadResult::Error) return std::nullopt;
    if (read == SecureStore::ReadResult::Missing) {
        if (!CreateLeaderData(std::move(server_name), impl->data) ||
            !SaveLeader(impl->store, impl->data, error)) {
            SetError(error, "could not initialize leader pairing identity");
            return std::nullopt;
        }
    } else if (!DeserializeLeader(bytes, impl->data) ||
               impl->data.server_name != server_name) {
        SetError(error, "leader pairing state is invalid or belongs to another identity");
        return std::nullopt;
    }
    return Leader(std::move(impl));
}

std::optional<Invitation> Leader::IssueInvitation(std::chrono::seconds lifetime,
                                                  std::string* error) {
    if (!impl_ || lifetime.count() < 1 || lifetime > std::chrono::hours(1)) {
        SetError(error, "invitation lifetime must be between one second and one hour");
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->data.invitations.size() >= kMaxInvitations) {
        SetError(error, "invitation capacity is exhausted");
        return std::nullopt;
    }
    const std::uint64_t now = UnixNow();
    if (!now || static_cast<std::uint64_t>(lifetime.count()) >
                    std::numeric_limits<std::uint64_t>::max() - now) {
        SetError(error, "could not determine invitation expiration");
        return std::nullopt;
    }
    InvitationData package;
    Rng rng;
    if (!rng.Init() || !rng.Fill(package.id.data(), package.id.size()) ||
        !rng.Fill(package.secret.data(), package.secret.size())) {
        SetError(error, "could not create a cryptographic invitation");
        return std::nullopt;
    }
    package.expires = now + static_cast<std::uint64_t>(lifetime.count());
    package.epoch = impl_->data.epoch;
    package.leader_id = impl_->data.leader_id;
    package.server_name = impl_->data.server_name;
    Cert ca, server;
    Fingerprint ca_pin{}, server_pin{};
    if (!ParseCertificate(impl_->data.ca_certificate, ca) ||
        !ComputeFingerprint(ca, ca_pin) ||
        Hex(ca_pin.data(), ca_pin.size()) != package.leader_id ||
        !ParseCertificate(impl_->data.server_certificate, server) ||
        !ComputeFingerprint(server, server_pin) ||
        !EqualSecret(server_pin.data(), impl_->data.leader_pin.data(), 32)) {
        SetError(error, "leader pairing identity is invalid");
        return std::nullopt;
    }
    package.leader_pin = server_pin;
    package.ca_pem = impl_->data.ca_certificate;
    std::string code;
    std::array<std::uint8_t, 32> token_hash{};
    if (!EncodeInvitation(package, code) ||
        !Hash(package.secret.data(), package.secret.size(), token_hash)) {
        SetError(error, "could not encode invitation");
        return std::nullopt;
    }
    LeaderData next = impl_->data;
    InvitationRecord record;
    record.id = package.id;
    record.secret_hash = token_hash;
    record.expires = package.expires;
    next.invitations.push_back(record);
    if (!SaveLeader(impl_->store, next, error)) return std::nullopt;
    impl_->data = std::move(next);
    return Invitation{std::move(code), package.expires};
}

bool Leader::HandleEnrollmentConnection(Socket socket, std::string* error,
                                         const std::atomic<bool>* canceled) {
    SocketGuard ownership(socket);
    if (!impl_) {
        SetError(error, "leader pairing state is unavailable");
        return false;
    }
    EnrollmentTls tls;
    std::string certificate, key;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        certificate = impl_->data.server_certificate;
        key = impl_->data.server_private_key;
    }
    ownership.Release();
    if (!tls.Start(socket, true, certificate, key, {}, {}, {}, error, canceled))
        return false;
    std::array<std::uint8_t, 32> challenge{};
    if (!MakeChallenge(tls, challenge, error)) return false;
    std::vector<std::uint8_t> frame;
    ByteVectorWiper wipe_frame{frame};
    if (!tls.ReceiveFrame(frame, error)) return false;
    Reader reader{frame.data(), frame.size()};
    std::uint8_t type = 0;
    std::array<std::uint8_t, 16> invite_id{};
    std::array<std::uint8_t, 32> secret{};
    ByteArrayWiper<32> wipe_secret{secret};
    std::string worker_id, label, public_pem;
    std::vector<std::uint8_t> signature;
    if (!ParseRequestHeader(reader, type, invite_id, secret, worker_id,
                            label, public_pem, signature)) {
        SetError(error, "invalid pairing enrollment request");
        return false;
    }
    const std::string invite_text = InvitationId(invite_id);
    const std::uint64_t now = UnixNow();
    if (!now) {
        SetError(error, "system clock is unavailable for enrollment");
        return false;
    }
    Writer reply;
    ReplyState status = ReplyState::Rejected;
    std::string reply_worker, reply_cert;
    std::uint64_t reply_epoch = 0;
    std::string reply_leader;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const InvitationRecord* existing_invite = FindInvitation(impl_->data, invite_text);
        if (!existing_invite) {
            status = ReplyState::Rejected;
        } else {
            std::array<std::uint8_t, 32> actual_hash{};
            if (!Hash(secret.data(), secret.size(), actual_hash) ||
                !EqualSecret(actual_hash.data(), existing_invite->secret_hash.data(), 32)) {
                status = ReplyState::Rejected;
            } else if (existing_invite->state == 0 && now >= existing_invite->expires) {
                LeaderData next = impl_->data;
                InvitationRecord* invite = FindInvitation(next, invite_text);
                invite->state = 2;
                if (!SaveLeader(impl_->store, next, error)) return false;
                impl_->data = std::move(next);
                status = ReplyState::Expired;
            } else if (type == 1 && existing_invite->state == 0) {
                Pk public_key;
                Fingerprint spki{};
                const bool parsed = ParsePublicKey(public_pem, public_key) &&
                                    ComputeFingerprint(public_key.value, spki) &&
                                    Hex(spki.data(), spki.size()) == worker_id;
                const auto proof = ProofMessage(1, challenge, invite_id, worker_id);
                if (!parsed || label.find('\0') != std::string::npos ||
                    !VerifySignature(public_key, proof, signature) ||
                    FindWorker(impl_->data, worker_id)) {
                    status = ReplyState::Rejected;
                } else if (impl_->data.workers.size() >= kMaxWorkers) {
                    status = ReplyState::Rejected;
                } else {
                    LeaderData next = impl_->data;
                    WorkerRecord worker;
                    worker.id = worker_id;
                    worker.label = label;
                    worker.state = 0;
                    worker.public_key_pem = public_pem;
                    worker.invitation_id = invite_text;
                    worker.created = now;
                    worker.spki = spki;
                    InvitationRecord* invite = FindInvitation(next, invite_text);
                    invite->state = 1;
                    invite->worker_id = worker_id;
                    next.workers.push_back(std::move(worker));
                    if (!SaveLeader(impl_->store, next, error)) return false;
                    impl_->data = std::move(next);
                    status = ReplyState::Pending;
                    reply_worker = worker_id;
                }
            } else if (existing_invite->state == 1) {
                const WorkerRecord* worker = FindWorker(impl_->data, worker_id);
                if (worker && existing_invite->worker_id == worker_id) {
                    Pk public_key;
                    const bool parsed = ParsePublicKey(worker->public_key_pem, public_key);
                    const auto proof = ProofMessage(type, challenge, invite_id, worker_id);
                    if (parsed && VerifySignature(public_key, proof, signature)) {
                        reply_worker = worker_id;
                        reply_epoch = impl_->data.epoch;
                        reply_leader = impl_->data.leader_id;
                        if (worker->state == 0) status = ReplyState::Pending;
                        else if (worker->state == 1) {
                            status = ReplyState::Approved;
                            reply_cert = worker->certificate_pem;
                        } else if (worker->state == 2) status = ReplyState::Rejected;
                        else status = ReplyState::Revoked;
                    }
                }
            } else if (existing_invite->state == 2) {
                status = ReplyState::Expired;
            } else {
                status = ReplyState::Consumed;
            }
        }
        reply_epoch = reply_epoch ? reply_epoch : impl_->data.epoch;
        reply_leader = reply_leader.empty() ? impl_->data.leader_id : reply_leader;
    }
    BuildReply(status, reply_worker, reply_epoch, reply_leader, reply_cert, reply);
    if (!reply.ok || !tls.SendFrame(reply.bytes, error)) return false;
    if (status == ReplyState::Pending || status == ReplyState::Approved)
        return true;
    SetError(error, status == ReplyState::Expired ? "invitation expired" :
                      status == ReplyState::Consumed ? "invitation was already consumed" :
                      "enrollment was not approved");
    return false;

}
std::vector<WorkerInfo> Leader::Workers() const {
    std::vector<WorkerInfo> out;
    if (!impl_) return out;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    out.reserve(impl_->data.workers.size());
    for (const WorkerRecord& worker : impl_->data.workers) {
        const WorkerStatus status = worker.state == 0 ? WorkerStatus::PendingApproval :
            worker.state == 1 ? WorkerStatus::Paired : WorkerStatus::Unpaired;
        out.push_back({worker.id, worker.label, status, worker.state == 3});
    }
    return out;
}

bool Leader::Approve(const std::string& worker_id, std::string* error) {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const WorkerRecord* current = FindWorker(impl_->data, worker_id);
    if (!current || current->state != 0) {
        SetError(error, current && current->state == 1
                            ? "worker was already approved"
                            : "worker is not pending approval");
        return false;
    }
    std::size_t active = 0;
    if (!ActiveWorkerCount(impl_->data, active) || active >= kMaxActiveWorkers) {
        SetError(error, "approved worker capacity is exhausted");
        return false;
    }
    LeaderData next = impl_->data;
    WorkerRecord* worker = FindWorker(next, worker_id);
    Rng rng;
    Pk ca_key, worker_public;
    if (!rng.Init() || !ParsePrivateKey(next.ca_private_key, rng, ca_key) ||
        !ParsePublicKey(worker->public_key_pem, worker_public) ||
        !WriteCertificate(rng, "CN=" + worker_id, "CN=Spirula Agent Pairing CA",
                          worker_public, ca_key, false, worker->certificate_pem)) {
        SetError(error, "could not issue worker identity certificate");
        return false;
    }
    Cert certificate, ca;
    Fingerprint issued_pin{};
    if (!ParseCertificate(worker->certificate_pem, certificate) ||
        !ParseCertificate(next.ca_certificate, ca) ||
        !VerifyCertificate(certificate, ca) ||
        !ComputeFingerprint(certificate, issued_pin) ||
        !EqualSecret(issued_pin.data(), worker->spki.data(), 32)) {
        SetError(error, "could not validate worker identity certificate");
        return false;
    }
    worker->state = 1;
    if (!SaveLeader(impl_->store, next, error)) return false;
    impl_->data = std::move(next);
    return true;
}

bool Leader::Reject(const std::string& worker_id, std::string* error) {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const WorkerRecord* current = FindWorker(impl_->data, worker_id);
    if (!current || current->state != 0) {
        SetError(error, "worker is not pending approval");
        return false;
    }
    LeaderData next = impl_->data;
    WorkerRecord* worker = FindWorker(next, worker_id);
    worker->state = 2;
    for (InvitationRecord& invite : next.invitations)
        if (InvitationId(invite.id) == worker->invitation_id) invite.state = 3;
    if (!SaveLeader(impl_->store, next, error)) return false;
    impl_->data = std::move(next);
    return true;
}

bool Leader::Revoke(const std::string& worker_id, std::string* error) {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const WorkerRecord* current = FindWorker(impl_->data, worker_id);
    if (!current || current->state != 1) {
        SetError(error, "worker is not active");
        return false;
    }
    LeaderData next = impl_->data;
    FindWorker(next, worker_id)->state = 3;
    if (!SaveLeader(impl_->store, next, error)) return false;
    impl_->data = std::move(next);
    return true;
}

std::optional<TlsChannel::Options> Leader::OptionsForLeader(std::string* error) const {
    if (!impl_) return std::nullopt;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    TlsChannel::Options options;
    options.enabled = true;
    options.paired = true;
    options.local_certificate_pem = impl_->data.server_certificate;
    options.local_private_key_pem = impl_->data.server_private_key;
    options.peer_trust_pem = impl_->data.ca_certificate;
    options.peer_crl_pem.clear();
    for (const WorkerRecord& worker : impl_->data.workers) {
        if (worker.state == 1) options.approved_peer_spki_sha256.push_back(worker.spki);
    }
    if (options.approved_peer_spki_sha256.empty() ||
        options.approved_peer_spki_sha256.size() > kMaxActiveWorkers) {
        SetError(error, "there are no active paired workers for the listener");
        return std::nullopt;
    }
    return options;
}

std::optional<WorkerInfo> Leader::WorkerForPeer(const Fingerprint& peer_spki) const {
    if (!impl_ || !Nonzero(peer_spki)) return std::nullopt;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const WorkerRecord& worker : impl_->data.workers) {
        if (EqualSecret(worker.spki.data(), peer_spki.data(), peer_spki.size())) {
            const WorkerStatus status = worker.state == 0 ? WorkerStatus::PendingApproval :
                worker.state == 1 ? WorkerStatus::Paired : WorkerStatus::Unpaired;
            return WorkerInfo{worker.id, worker.label, status, worker.state == 3};
        }
    }
    return std::nullopt;
}

std::string Leader::LeaderId() const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->data.leader_id;
}

std::uint64_t Leader::LeaderEpoch() const {
    if (!impl_) return 0;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->data.epoch;
}
TlsChannel::PeerFingerprint Leader::EnrollmentPeerFingerprint() const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->data.leader_pin;
}

bool Leader::InitializeUpdateSigner(
    std::string& public_key_pem, std::string& fingerprint_sha256,
    std::string* error) {
    public_key_pem.clear();
    fingerprint_sha256.clear();
    if (!impl_) {
        SetError(error, "leader pairing identity is unavailable");
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Rng rng;
    Pk private_key;
    Fingerprint fingerprint{};
    if (!LoadUpdateSigner(impl_->store, impl_->data.ca_certificate, true,
                          rng, private_key, public_key_pem, fingerprint, error))
        return false;
    fingerprint_sha256 = Hex(fingerprint.data(), fingerprint.size());
    return true;
}

bool Leader::SignUpdateOffer(
    const std::string& worker_id, const std::vector<std::uint8_t>& payload,
    std::vector<std::uint8_t>& signature, std::string& signer_public_key_pem,
    std::string* error) const {
    signer_public_key_pem.clear();
    if (!impl_ || !ValidWorkerIdentity(worker_id) || payload.empty() ||
        payload.size() > 4096) {
        SetError(error, "update offer signing input is invalid");
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const WorkerRecord* worker = FindWorker(impl_->data, worker_id);
    if (!worker || worker->state != 1) {
        SetError(error, "update offers can be signed only for an approved worker");
        return false;
    }
    Rng rng;
    Pk private_key;
    std::string public_pem;
    Fingerprint fingerprint{};
    std::vector<std::uint8_t> signed_bytes;
    if (!LoadUpdateSigner(impl_->store, impl_->data.ca_certificate, false,
                          rng, private_key, public_pem, fingerprint, error) ||
        !Sign(rng, private_key, payload, signed_bytes) ||
        signed_bytes.empty() || signed_bytes.size() > 256) {
        if (error && error->empty())
            SetError(error, "could not sign the update offer");
        return false;
    }
    signature = std::move(signed_bytes);
    signer_public_key_pem = std::move(public_pem);
    return true;
}

bool VerifyDetachedUpdateSignature(
    const std::string& public_key_pem,
    const std::vector<std::uint8_t>& message,
    const std::vector<std::uint8_t>& signature,
    Fingerprint& fingerprint, std::string* error) {
    fingerprint = {};
    if (message.empty() || message.size() > 4096 ||
        signature.empty() || signature.size() > 256) {
        SetError(error, "detached update signature input is invalid");
        return false;
    }
    Pk public_key;
    if (!ParsePublicKey(public_key_pem, public_key) ||
        !ComputeFingerprint(public_key.value, fingerprint) ||
        !VerifySignature(public_key, message, signature)) {
        fingerprint = {};
        SetError(error, "detached update signature is invalid");
        return false;
    }
    return true;
}

Worker::Worker(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Worker::Worker(Worker&&) noexcept = default;
Worker& Worker::operator=(Worker&&) noexcept = default;
Worker::~Worker() = default;

std::optional<Worker> Worker::Open(const fs::path& state_root, std::string* error) {
    return OpenWithMode(state_root, error, false);
}

std::optional<Worker> Worker::OpenMachine(const fs::path& state_root,
                                          std::string* error) {
    return OpenWithMode(state_root, error, true);
}

std::optional<Worker> Worker::OpenWithMode(const fs::path& state_root,
                                           std::string* error, bool machine) {
    auto impl = std::make_unique<Impl>();
    if (!impl->store.Open(state_root, error, machine)) return std::nullopt;
    std::vector<std::uint8_t> bytes;
    const auto read = impl->store.Read(kWorkerFile, bytes, error);
    if (read == SecureStore::ReadResult::Error) return std::nullopt;
    if (read == SecureStore::ReadResult::Ok) {
        if (!DeserializeWorker(bytes, impl->data)) {
            if (!bytes.empty()) mbedtls_platform_zeroize(bytes.data(), bytes.size());
            SetError(error, "worker pairing state is invalid");
            return std::nullopt;
        }
        impl->has_state = true;
        if (!bytes.empty()) mbedtls_platform_zeroize(bytes.data(), bytes.size());
    }
    return Worker(std::move(impl));
}

WorkerStatus Worker::Status() const noexcept {
    if (!impl_) return WorkerStatus::Unpaired;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->has_state) return WorkerStatus::Unpaired;
    return impl_->data.state == 1 ? WorkerStatus::PendingApproval : WorkerStatus::Paired;
}

std::string Worker::WorkerId() const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->has_state ? impl_->data.worker_id : std::string{};
}

std::string Worker::LeaderId() const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->has_state ? impl_->data.leader_id : std::string{};
}
bool Worker::IsPairedTo(const std::string& worker_id,
                        const std::string& leader_id) const {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->has_state && impl_->data.state == 2 &&
           impl_->data.worker_id == worker_id &&
           impl_->data.leader_id == leader_id;
}

std::uint64_t Worker::LeaderEpoch() const noexcept {
    if (!impl_) return 0;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->has_state ? impl_->data.leader_epoch : 0;
}

bool Worker::Forget(std::string* error) {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->has_state) return true;
    if (!impl_->store.Remove(kWorkerFile, error)) return false;
    if (!impl_->data.private_key.empty())
        mbedtls_platform_zeroize(impl_->data.private_key.data(),
                                 impl_->data.private_key.size());
    impl_->data = WorkerData{};
    impl_->has_state = false;
    return true;
}

bool Worker::Redeem(Socket socket, const std::string& invitation_code,
                    const Fingerprint& expected_leader_spki,
                    const std::string& leader_server_name, std::string label,
                    std::string* error) {
    SocketGuard ownership(socket);
    if (!impl_ || !Nonzero(expected_leader_spki) ||
        !IsDnsName(leader_server_name) || label.size() > kMaxLabelBytes ||
        label.find('\0') != std::string::npos) {
        SetError(error, "invalid worker pairing input");
        return false;
    }
    InvitationData invite;
    if (!DecodeInvitation(invitation_code, invite) ||
        !EqualSecret(invite.leader_pin.data(), expected_leader_spki.data(), 32) ||
        invite.server_name != leader_server_name) {
        SetError(error, "invitation or configured leader identity is invalid");
        return false;
    }
    WorkerData worker;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->has_state && impl_->data.state != 1) {
            SetError(error, "worker is already paired; explicit re-pair is required");
            return false;
        }
        if (impl_->has_state) {
            worker = impl_->data;
            if (worker.invitation_id != InvitationId(invite.id) ||
                worker.leader_id != invite.leader_id ||
                worker.leader_epoch != invite.epoch ||
                worker.server_name != leader_server_name ||
                !EqualSecret(worker.leader_pin.data(), invite.leader_pin.data(), 32)) {
                SetError(error, "pending worker identity belongs to another invitation");
                return false;
            }
        } else {
            Rng rng;
            Pk key;
            Fingerprint fingerprint{};
            if (!rng.Init() || !GenerateKey(rng, key, worker.private_key,
                                             &worker.public_key) ||
                !ComputeFingerprint(key.value, fingerprint)) {
                SetError(error, "could not generate worker private identity");
                return false;
            }
            worker.state = 1;
            worker.worker_id = Hex(fingerprint.data(), fingerprint.size());
            worker.label = std::move(label);
            worker.leader_id = invite.leader_id;
            worker.leader_epoch = invite.epoch;
            worker.leader_pin = expected_leader_spki;
            worker.ca_certificate = invite.ca_pem;
            worker.invitation_id = InvitationId(invite.id);
            worker.server_name = leader_server_name;
            if (!SaveWorker(impl_->store, worker, error)) return false;
            impl_->data = worker;
            impl_->has_state = true;
        }
    }
    EnrollmentTls tls;
    ownership.Release();
    if (!tls.Start(socket, false, {}, {}, worker.ca_certificate,
                   expected_leader_spki, leader_server_name, error)) return false;
    std::array<std::uint8_t, 32> challenge{};
    if (!ReadChallenge(tls, challenge, error)) return false;
    Rng rng;
    Pk key;
    if (!rng.Init() || !ParsePrivateKey(worker.private_key, rng, key)) {
        SetError(error, "pending worker identity is invalid");
        return false;
    }
    std::vector<std::uint8_t> signature;
    Writer request;
    ByteVectorWiper wipe_request{request.bytes};
    if (!EncodeRedeemRequest(invite, worker, challenge, signature, rng, key, request) ||
        !tls.SendFrame(request.bytes, error)) {
        SetError(error, "could not send enrollment request");
        return false;
    }
    std::vector<std::uint8_t> response;
    if (!tls.ReceiveFrame(response, error)) return false;
    ReplyState status{};
    std::string returned_id, leader_id, certificate;
    std::uint64_t epoch = 0;
    if (!ParseReply(response, status, returned_id, epoch, leader_id, certificate) ||
        leader_id != invite.leader_id || epoch != invite.epoch) {
        SetError(error, "leader enrollment response is invalid");
        return false;
    }
    const bool active = status == ReplyState::Pending || status == ReplyState::Approved;
    if ((active && returned_id != worker.worker_id) ||
        (!active && !returned_id.empty() && returned_id != worker.worker_id)) {
        SetError(error, "leader enrollment response names another worker");
        return false;
    }
    if (status == ReplyState::Pending) return true;
    if (status == ReplyState::Approved) {
        WorkerData next;
        if (!PrepareApprovedWorker(worker, epoch, std::move(certificate), next, error))
            return false;
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->has_state || impl_->data.worker_id != worker.worker_id ||
            impl_->data.invitation_id != worker.invitation_id ||
            impl_->data.state != 1) {
            SetError(error, "worker identity changed during approval");
            return false;
        }
        if (!SaveWorker(impl_->store, next, error)) return false;
        impl_->data = std::move(next);
        return true;
    }
    std::string cleanup_error;
    if (!Forget(&cleanup_error)) {
        SetError(error, cleanup_error);
        return false;
    }
    SetError(error, status == ReplyState::Expired ? "invitation expired" :
                  status == ReplyState::Consumed ? "invitation was already consumed" :
                  "leader rejected the enrollment request");
    return false;
}

bool Worker::CheckApproval(Socket socket, const std::string& invitation_code,
                           const Fingerprint& expected_leader_spki,
                           const std::string& leader_server_name,
                           std::string* error) {
    SocketGuard ownership(socket);
    if (!impl_ || !Nonzero(expected_leader_spki) ||
        !IsDnsName(leader_server_name)) {
        SetError(error, "worker is not awaiting approval");
        return false;
    }
    InvitationData invite;
    WorkerData worker;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->has_state) {
            SetError(error, "worker is not awaiting approval");
            return false;
        }
        worker = impl_->data;
    }
    if (worker.state != 1 || !DecodeInvitation(invitation_code, invite) ||
        worker.invitation_id != InvitationId(invite.id) ||
        !EqualSecret(invite.leader_pin.data(), expected_leader_spki.data(), 32) ||
        worker.leader_id != invite.leader_id || worker.leader_epoch != invite.epoch ||
        !EqualSecret(worker.leader_pin.data(), invite.leader_pin.data(), 32) ||
        worker.server_name != leader_server_name ||
        invite.server_name != leader_server_name) {
        SetError(error, "worker approval request does not match its invitation");
        return false;
    }
    EnrollmentTls tls;
    ownership.Release();
    if (!tls.Start(socket, false, {}, {}, worker.ca_certificate,
                   expected_leader_spki, leader_server_name, error)) return false;
    std::array<std::uint8_t, 32> challenge{};
    if (!ReadChallenge(tls, challenge, error)) return false;
    Rng rng;
    Pk key;
    if (!rng.Init() || !ParsePrivateKey(worker.private_key, rng, key)) {
        SetError(error, "pending worker identity is invalid");
        return false;
    }
    std::vector<std::uint8_t> signature;
    Writer request;
    ByteVectorWiper wipe_request{request.bytes};
    if (!EncodeStatusRequest(invite, worker, challenge, signature, rng, key, request) ||
        !tls.SendFrame(request.bytes, error)) return false;
    std::vector<std::uint8_t> response;
    if (!tls.ReceiveFrame(response, error)) return false;
    ReplyState status{};
    std::string returned_id, leader_id, certificate;
    std::uint64_t epoch = 0;
    if (!ParseReply(response, status, returned_id, epoch, leader_id, certificate) ||
        leader_id != invite.leader_id || epoch != invite.epoch) {
        SetError(error, "leader approval response is invalid");
        return false;
    }
    const bool active = status == ReplyState::Pending || status == ReplyState::Approved;
    if ((active && returned_id != worker.worker_id) ||
        (!active && !returned_id.empty() && returned_id != worker.worker_id)) {
        SetError(error, "leader approval response names another worker");
        return false;
    }
    if (status == ReplyState::Pending) return true;
    if (status != ReplyState::Approved) {
        std::string cleanup_error;
        if (!Forget(&cleanup_error)) {
            SetError(error, cleanup_error);
            return false;
        }
        SetError(error, status == ReplyState::Revoked ? "worker identity was revoked" :
                      "worker enrollment was rejected");
        return false;
    }
    WorkerData next;
    if (!PrepareApprovedWorker(worker, epoch, std::move(certificate), next, error))
        return false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->has_state || impl_->data.worker_id != worker.worker_id ||
            impl_->data.state != 1) {
            SetError(error, "worker identity changed during approval");
            return false;
        }
        if (!SaveWorker(impl_->store, next, error)) return false;
        impl_->data = std::move(next);
    }
    return true;
}

std::optional<TlsChannel::ClientOptions> Worker::ClientOptionsForLeader(
    std::string address, std::string server_name, std::uint16_t port,
    std::string* error) const {
    if (!impl_ || address.empty() || address.size() > 45 ||
        address.find('\0') != std::string::npos || !IsDnsName(server_name) || !port) {
        SetError(error, "worker has no valid paired TLS endpoint");
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->has_state || impl_->data.state != 2 ||
        impl_->data.certificate.empty() || impl_->data.private_key.empty()) {
        SetError(error, "worker has no approved persisted identity");
        return std::nullopt;
    }
    const WorkerData& worker = impl_->data;
    if (worker.server_name != server_name) {
        SetError(error, "configured leader name does not match paired identity");
        return std::nullopt;
    }
    TlsChannel::ClientOptions options;
    options.tls.enabled = true;
    options.tls.paired = true;
    options.tls.local_certificate_pem = worker.certificate;
    options.tls.local_private_key_pem = worker.private_key;
    options.tls.peer_trust_pem = worker.ca_certificate;
    options.tls.expected_peer_spki_sha256 = worker.leader_pin;
    options.connect_address = std::move(address);
    options.server_name = std::move(server_name);
    options.port = port;
    return options;
}

}  // namespace app::agent::pairing
