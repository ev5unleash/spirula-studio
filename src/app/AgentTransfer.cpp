#include "app/AgentTransfer.h"
#include "core/FilesystemPath.h"

#include "data/Json.h"
#include "data/JsonWrite.h"

#include <mbedtls/sha256.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace app::agent {
using spirula::NativeFilesystemPath;
using spirula::LogicalFilesystemPath;
namespace {
namespace fs = std::filesystem;

constexpr std::uint32_t kProtocolVersion = 1;
constexpr std::size_t kChunkBytes = 64 * 1024;
constexpr std::size_t kChunkHeaderBytes = 9;
constexpr std::size_t kMaxControlBytes = 1024 * 1024;
constexpr std::size_t kMaxManifestFiles = 4096;
constexpr std::size_t kMaxPathBytes = 4096;
constexpr std::uint64_t kMetadataReservationBytes = 4096;  // One 4 KiB unit per file or directory.
constexpr char kStageDir[] = ".agent-transfer-v1";
constexpr std::uint8_t kChunkTag = 'C';

TransferResult Failure(TransferError error, const char* message) {
    return {error, message};
}

const char* ErrorCode(TransferError error) {
    switch (error) {
        case TransferError::InvalidArgument: return "invalid-argument";
        case TransferError::InvalidManifest: return "invalid-manifest";
        case TransferError::QuotaExceeded: return "quota-exceeded";
        case TransferError::Filesystem: return "filesystem";
        case TransferError::Integrity: return "integrity";
        case TransferError::Protocol: return "protocol";
        case TransferError::PeerRejected: return "peer-rejected";
        case TransferError::Interrupted: return "interrupted";
        case TransferError::None: return "none";
    }
    return "protocol";
}

TransferError DecodeErrorCode(const std::string& code) {
    if (code == "invalid-argument") return TransferError::InvalidArgument;
    if (code == "invalid-manifest") return TransferError::InvalidManifest;
    if (code == "quota-exceeded") return TransferError::QuotaExceeded;
    if (code == "filesystem") return TransferError::Filesystem;
    if (code == "integrity") return TransferError::Integrity;
    if (code == "peer-rejected") return TransferError::PeerRejected;
    if (code == "interrupted") return TransferError::Interrupted;
    return TransferError::Protocol;
}

const char* ErrorMessage(TransferError error) {
    switch (error) {
        case TransferError::InvalidArgument: return "invalid transfer arguments";
        case TransferError::InvalidManifest: return "manifest rejected";
        case TransferError::QuotaExceeded: return "transfer exceeds disk budget";
        case TransferError::Filesystem: return "rooted file operation failed";
        case TransferError::Integrity: return "file digest mismatch";
        case TransferError::Protocol: return "transfer protocol violation";
        case TransferError::PeerRejected: return "peer rejected transfer";
        case TransferError::Interrupted: return "framed transfer interrupted";
        case TransferError::None: return "";
    }
    return "transfer failed";
}

class Sha256 final {
public:
    Sha256() {
        mbedtls_sha256_init(&context_);
        valid_ = mbedtls_sha256_starts(&context_, 0) == 0;
    }
    ~Sha256() { mbedtls_sha256_free(&context_); }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    bool Update(const void* data, std::size_t size) {
        return valid_ && (size == 0 || mbedtls_sha256_update(
            &context_, static_cast<const unsigned char*>(data), size) == 0);
    }
    bool Finish(std::string& hex) {
        std::array<unsigned char, 32> digest{};
        if (!valid_ || mbedtls_sha256_finish(&context_, digest.data()) != 0)
            return false;
        static constexpr char digits[] = "0123456789abcdef";
        hex.resize(64);
        for (std::size_t i = 0; i < digest.size(); ++i) {
            hex[2 * i] = digits[digest[i] >> 4];
            hex[2 * i + 1] = digits[digest[i] & 15];
        }
        return true;
    }

private:
    mbedtls_sha256_context context_{};
    bool valid_ = false;
};

bool HashString(std::string_view text, std::string& hex) {
    Sha256 hash;
    return hash.Update(text.data(), text.size()) && hash.Finish(hex);
}

bool HashStream(std::ifstream& input, std::uint64_t expected_size,
                std::string& hex) {
    input.clear();
    input.seekg(0, std::ios::beg);
    if (!input) return false;
    Sha256 hash;
    std::array<char, kChunkBytes> buffer{};
    std::uint64_t total = 0;
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = input.gcount();
        if (got < 0 || total > expected_size ||
            static_cast<std::uint64_t>(got) > expected_size - total)
            return false;
        if (got > 0 && !hash.Update(buffer.data(), static_cast<std::size_t>(got)))
            return false;
        total += static_cast<std::uint64_t>(got);
    }
    return !input.bad() && total == expected_size && hash.Finish(hex);
}

bool HashFile(const fs::path& path, std::uint64_t expected_size,
              std::string& hex) {
    std::ifstream input(NativeFilesystemPath(path), std::ios::binary);
    return input && HashStream(input, expected_size, hex);
}

bool IsValidUtf8Path(const std::string& value) {
    if (value.empty() || value.size() > kMaxPathBytes) return false;
    for (std::size_t i = 0; i < value.size();) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c < 0x20 || c == 0x7f || c == '\\') return false;
        if (c < 0x80) {
            ++i;
            continue;
        }
        std::size_t count = 0;
        unsigned char second_min = 0x80, second_max = 0xbf;
        if (c >= 0xc2 && c <= 0xdf) count = 2;
        else if (c >= 0xe0 && c <= 0xef) {
            count = 3;
            if (c == 0xe0) second_min = 0xa0;
            if (c == 0xed) second_max = 0x9f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            count = 4;
            if (c == 0xf0) second_min = 0x90;
            if (c == 0xf4) second_max = 0x8f;
        } else return false;
        if (i + count > value.size()) return false;
        const auto second = static_cast<unsigned char>(value[i + 1]);
        if (second < second_min || second > second_max) return false;
        for (std::size_t j = 2; j < count; ++j) {
            const auto continuation = static_cast<unsigned char>(value[i + j]);
            if (continuation < 0x80 || continuation > 0xbf) return false;
        }
        i += count;
    }
    return true;
}

std::string LowerAscii(std::string text) {
    for (char& ch : text)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    return text;
}

struct FoldedPathLess {
    bool operator()(std::string_view a, std::string_view b) const {
        const std::size_t common = std::min(a.size(), b.size());
        for (std::size_t i = 0; i < common; ++i) {
            const unsigned char left = static_cast<unsigned char>(a[i]);
            const unsigned char right = static_cast<unsigned char>(b[i]);
            const unsigned char folded_left = static_cast<unsigned char>(
                left >= 'A' && left <= 'Z' ? left + ('a' - 'A') : left);
            const unsigned char folded_right = static_cast<unsigned char>(
                right >= 'A' && right <= 'Z' ? right + ('a' - 'A') : right);
            if (folded_left != folded_right) return folded_left < folded_right;
        }
        return a.size() < b.size();
    }
};

bool AddDiskReservation(std::uint64_t amount, std::uint64_t budget,
                        std::uint64_t& reserved) {
    if (reserved > budget ||
        amount > std::numeric_limits<std::uint64_t>::max() - reserved ||
        amount > budget - reserved) return false;
    reserved += amount;
    return true;
}

bool ComputeManifestDiskReservation(const std::vector<TransferFile>& files,
                                   std::uint64_t budget,
                                   std::uint64_t& reserved) {
    reserved = 0;
    std::set<std::string_view, FoldedPathLess> directories;
    for (const TransferFile& file : files) {
        if (!AddDiskReservation(file.size, budget, reserved) ||
            !AddDiskReservation(kMetadataReservationBytes, budget, reserved))
            return false;
        for (std::size_t slash = file.path.find('/');
             slash != std::string::npos;
             slash = file.path.find('/', slash + 1)) {
            const std::string_view directory(file.path.data(), slash);
            if (directories.insert(directory).second &&
                !AddDiskReservation(kMetadataReservationBytes, budget, reserved))
                return false;
        }
    }
    return true;
}

bool IsWindowsReservedName(const std::string& component) {
    const std::string stem = LowerAscii(component.substr(0, component.find('.')));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul")
        return true;
    if (stem.size() == 4 &&
        (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) &&
        stem[3] >= '1' && stem[3] <= '9') return true;
    return false;
}

bool ValidRelativePath(const std::string& path) {
    if (!IsValidUtf8Path(path) || path.front() == '/' || path.back() == '/')
        return false;
    std::size_t start = 0;
    while (start < path.size()) {
        const std::size_t end = path.find('/', start);
        const std::string component = path.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (component.empty() || component == "." || component == ".." ||
            component.size() > 255 || component.back() == '.' ||
            component.back() == ' ' || IsWindowsReservedName(component) ||
            LowerAscii(component) == kStageDir) return false;
        for (const unsigned char ch : component)
            if (ch == ':' || ch == '<' || ch == '>' || ch == '"' ||
                ch == '|' || ch == '?' || ch == '*') return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

bool ValidDigest(const std::string& digest) {
    if (digest.size() != 64) return false;
    for (char ch : digest)
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
    return true;
}

TransferResult ValidateManifest(std::vector<TransferFile>& files,
                                std::uint64_t budget) {
    if (files.size() > kMaxManifestFiles)
        return Failure(TransferError::InvalidManifest, ErrorMessage(TransferError::InvalidManifest));
    for (const TransferFile& file : files) {
        if (!ValidRelativePath(file.path) || !ValidDigest(file.sha256) ||
            file.size > static_cast<std::uint64_t>(
                std::numeric_limits<std::streamoff>::max()))
            return Failure(TransferError::InvalidManifest, ErrorMessage(TransferError::InvalidManifest));
    }
    std::uint64_t reservation = 0;
    if (!ComputeManifestDiskReservation(files, budget, reservation))
        return Failure(TransferError::QuotaExceeded, ErrorMessage(TransferError::QuotaExceeded));
    std::sort(files.begin(), files.end(), [](const TransferFile& a, const TransferFile& b) {
        return a.path < b.path;
    });
    std::set<std::string> paths;
    for (const TransferFile& file : files) {
        const std::string folded = LowerAscii(file.path);
        if (!paths.insert(folded).second)
            return Failure(TransferError::InvalidManifest, ErrorMessage(TransferError::InvalidManifest));
        for (std::size_t slash = folded.find('/'); slash != std::string::npos;
             slash = folded.find('/', slash + 1)) {
            if (paths.count(folded.substr(0, slash)))
                return Failure(TransferError::InvalidManifest, ErrorMessage(TransferError::InvalidManifest));
        }
    }
    return {};
}

bool SameManifest(const std::vector<TransferFile>& a,
                  const std::vector<TransferFile>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].path != b[i].path || a[i].size != b[i].size ||
            a[i].sha256 != b[i].sha256) return false;
    return true;
}

bool IsReparsePoint(const fs::path& path) {
#ifdef _WIN32
    const DWORD attributes =
        GetFileAttributesW(NativeFilesystemPath(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    (void)path;
    return false;
#endif
}

bool PathStatus(const fs::path& path, fs::file_status& status) {
    std::error_code ec;
    status = fs::symlink_status(NativeFilesystemPath(path), ec);
    if (!ec) return true;
    if (ec == std::errc::no_such_file_or_directory ||
        ec == std::errc::not_a_directory) {
        status = fs::file_status(fs::file_type::not_found);
        return true;
    }
    return false;
}

bool IsMissing(fs::file_status status) {
    return status.type() == fs::file_type::not_found ||
           status.type() == fs::file_type::none;
}

bool IsWithin(const fs::path& root, const fs::path& candidate) {
    auto r = root.begin();
    auto c = candidate.begin();
    for (; r != root.end(); ++r, ++c) {
        if (c == candidate.end()) return false;
#ifdef _WIN32
        if (LowerAscii(r->u8string()) != LowerAscii(c->u8string())) return false;
#else
        if (*r != *c) return false;
#endif
    }
    return true;
}

bool GetRoot(const fs::path& root, fs::path& canonical) {
    if (!root.is_absolute()) return false;
    fs::file_status status;
    if (!PathStatus(root, status) || IsMissing(status) ||
        !fs::is_directory(status) || fs::is_symlink(status) || IsReparsePoint(root))
        return false;
    std::error_code ec;
    canonical = LogicalFilesystemPath(fs::canonical(NativeFilesystemPath(root), ec));
    return !ec && canonical.is_absolute();
}

bool CheckPath(const fs::path& root, const std::string& relative,
               bool create_parents, fs::path& leaf) {
    fs::path current = root;
    std::size_t start = 0;
    for (;;) {
        const std::size_t slash = relative.find('/', start);
        const bool last = slash == std::string::npos;
        const std::string component = relative.substr(
            start, last ? std::string::npos : slash - start);
        current /= fs::u8path(component);
        if (last) {
            leaf = current;
            fs::file_status status;
            if (!PathStatus(leaf, status)) return false;
            if (!IsMissing(status) && (fs::is_symlink(status) || IsReparsePoint(leaf)))
                return false;
            if (!IsMissing(status)) {
                std::error_code ec;
                const fs::path resolved = LogicalFilesystemPath(
                    fs::canonical(NativeFilesystemPath(leaf), ec));
                if (ec || !IsWithin(root, resolved)) return false;
            }
            return true;
        }
        fs::file_status status;
        if (!PathStatus(current, status)) return false;
        if (IsMissing(status) && create_parents) {
            std::error_code ec;
            fs::create_directory(NativeFilesystemPath(current), ec);
            if (ec && ec != std::errc::file_exists) return false;
            if (!PathStatus(current, status)) return false;
        }
        if (IsMissing(status) || !fs::is_directory(status) ||
            fs::is_symlink(status) || IsReparsePoint(current)) return false;
        std::error_code ec;
        const fs::path resolved = LogicalFilesystemPath(
            fs::canonical(NativeFilesystemPath(current), ec));
        if (ec || !IsWithin(root, resolved)) return false;
        start = slash + 1;
    }
}

bool SingleLinkedRegularFile(const fs::path& path, bool allow_missing,
                             bool& exists, std::uint64_t& size) {
    fs::file_status status;
    if (!PathStatus(path, status)) return false;
    exists = !IsMissing(status);
    size = 0;
    if (!exists) return allow_missing;
    if (!fs::is_regular_file(status) || fs::is_symlink(status) ||
        IsReparsePoint(path)) return false;
    std::error_code ec;
    const std::uintmax_t links =
        fs::hard_link_count(NativeFilesystemPath(path), ec);
    if (ec || links != 1) return false;
    const std::uintmax_t bytes = fs::file_size(NativeFilesystemPath(path), ec);
    if (ec) return false;
    size = static_cast<std::uint64_t>(bytes);
    return true;
}

bool EnsureStageDirectory(const fs::path& root, fs::path& stage) {
    stage = root / fs::u8path(kStageDir);
    fs::file_status status;
    if (!PathStatus(stage, status)) return false;
    if (IsMissing(status)) {
        std::error_code ec;
        fs::create_directory(NativeFilesystemPath(stage), ec);
        if (ec && ec != std::errc::file_exists) return false;
        if (!PathStatus(stage, status)) return false;
    }
    if (!fs::is_directory(status) || fs::is_symlink(status) ||
        IsReparsePoint(stage)) return false;
    std::error_code ec;
    const fs::path resolved = LogicalFilesystemPath(
        fs::canonical(NativeFilesystemPath(stage), ec));
    return !ec && IsWithin(root, resolved);
}

bool ReplaceFile(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
    return MoveFileExW(NativeFilesystemPath(from).c_str(),
                       NativeFilesystemPath(to).c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code ec;
    fs::rename(NativeFilesystemPath(from), NativeFilesystemPath(to), ec);
    return !ec;
#endif
}

bool RemoveFile(const fs::path& path) {
    std::error_code ec;
    fs::remove(NativeFilesystemPath(path), ec);
    return !ec;
}

bool ExistingMatches(const fs::path& root, const std::string& relative,
                     const TransferFile& file, bool& matches) {
    fs::path path;
    if (!CheckPath(root, relative, false, path)) return false;
    bool exists = false;
    std::uint64_t size = 0;
    if (!SingleLinkedRegularFile(path, true, exists, size)) return false;
    matches = false;
    if (!exists || size != file.size) return true;
    std::string digest;
    if (!HashFile(path, file.size, digest)) return false;
    matches = digest == file.sha256;
    return true;
}

std::string PartialName(const TransferFile& file) {
    std::string key;
    const std::string material = file.path + '\0' + file.sha256;
    if (!HashString(material, key)) return {};
    return key + ".part";
}

TransferResult CheckStagingQuota(const fs::path& root,
                                 const std::vector<TransferFile>& files,
                                 std::uint64_t budget) {
    std::uint64_t reservation = 0;
    if (!ComputeManifestDiskReservation(files, budget, reservation))
        return Failure(TransferError::QuotaExceeded, ErrorMessage(TransferError::QuotaExceeded));
    std::map<std::string, std::uint64_t> known_partials;
    for (const TransferFile& file : files) {
        const std::string name = PartialName(file);
        if (name.empty())
            return Failure(TransferError::Filesystem, ErrorMessage(TransferError::Filesystem));
        known_partials.emplace(name, file.size);
    }

    const fs::path stage = root / fs::u8path(kStageDir);
    fs::file_status status;
    if (!PathStatus(stage, status))
        return Failure(TransferError::Filesystem, ErrorMessage(TransferError::Filesystem));
    if (IsMissing(status)) return {};
    if (!fs::is_directory(status) || fs::is_symlink(status) ||
        IsReparsePoint(stage))
        return Failure(TransferError::Filesystem, ErrorMessage(TransferError::Filesystem));
    std::error_code ec;
    const fs::path resolved = LogicalFilesystemPath(
        fs::canonical(NativeFilesystemPath(stage), ec));
    if (ec || !IsWithin(root, resolved))
        return Failure(TransferError::Filesystem, ErrorMessage(TransferError::Filesystem));

    std::vector<fs::path> oversized_known_partials;
    fs::directory_iterator current(NativeFilesystemPath(stage), ec), end;
    if (ec)
        return Failure(TransferError::Filesystem, ErrorMessage(TransferError::Filesystem));
    while (current != end) {
        const fs::path path = current->path();
        const std::string name = path.filename().u8string();
        bool valid_name = name.size() == 69 && name.compare(64, 5, ".part") == 0;
        for (std::size_t i = 0; valid_name && i < 64; ++i)
            valid_name = (name[i] >= '0' && name[i] <= '9') ||
                         (name[i] >= 'a' && name[i] <= 'f');
        bool exists = false;
        std::uint64_t size = 0;
        if (!valid_name || !SingleLinkedRegularFile(path, false, exists, size) ||
            !exists)
            return Failure(TransferError::Filesystem, ErrorMessage(TransferError::Filesystem));
        const auto known = known_partials.find(name);
        if (known != known_partials.end()) {
            if (size > known->second) oversized_known_partials.push_back(path);
        } else if (!AddDiskReservation(size, budget, reservation) ||
                   !AddDiskReservation(kMetadataReservationBytes, budget, reservation)) {
            return Failure(TransferError::QuotaExceeded, ErrorMessage(TransferError::QuotaExceeded));
        }
        current.increment(ec);
        if (ec)
            return Failure(TransferError::Filesystem, ErrorMessage(TransferError::Filesystem));
    }
    for (const fs::path& path : oversized_known_partials)
        if (!RemoveFile(path))
            return Failure(TransferError::Filesystem, ErrorMessage(TransferError::Filesystem));
    return {};
}

std::string Decimal(std::uint64_t value) { return std::to_string(value); }

std::string ManifestFrame(const std::vector<TransferFile>& files) {
    JsonWriter json;
    json.object().field("v", static_cast<int>(kProtocolVersion))
        .field("op", "manifest").key("files").array();
    for (const TransferFile& file : files) {
        json.object().field("path", file.path).field("size", Decimal(file.size))
            .field("sha256", file.sha256).end();
    }
    json.end().end();
    return json.str();
}

std::string SimpleFrame(const char* op) {
    JsonWriter json;
    json.object().field("v", static_cast<int>(kProtocolVersion)).field("op", op).end();
    return json.str();
}

std::string FileFrame(const char* op, const std::string& path) {
    JsonWriter json;
    json.object().field("v", static_cast<int>(kProtocolVersion))
        .field("op", op).field("path", path).end();
    return json.str();
}

std::string ReadyFrame(std::uint64_t offset) {
    JsonWriter json;
    json.object().field("v", static_cast<int>(kProtocolVersion))
        .field("op", "ready").field("offset", Decimal(offset)).end();
    return json.str();
}

std::string ErrorFrame(TransferError error) {
    JsonWriter json;
    json.object().field("v", static_cast<int>(kProtocolVersion))
        .field("op", "error").field("code", ErrorCode(error)).end();
    return json.str();
}

bool SendBytes(detail::FrameTransport transport, const std::uint8_t* data,
               std::size_t size) {
    return transport.send && size <= TlsChannel::kMaxFrameBytes &&
           transport.send(transport.context, data, size, nullptr);
}

bool SendRaw(detail::FrameTransport transport, std::string_view frame) {
    return SendBytes(transport,
        reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size());
}

TransferResult SendError(detail::FrameTransport transport, TransferError error) {
    const std::string frame = ErrorFrame(error);
    SendRaw(transport, frame);
    return Failure(error, ErrorMessage(error));
}

bool ReadFrame(detail::FrameTransport transport, std::vector<std::uint8_t>& frame) {
    if (!transport.receive || !transport.receive(transport.context, frame, nullptr))
        return false;
    return frame.size() <= TlsChannel::kMaxFrameBytes;
}

const JsonValue* Field(const JsonValue& value, const char* key) {
    return value.find(key);
}

bool ExactFields(const JsonValue& value,
                 std::initializer_list<const char*> names) {
    if (!value.is_object() || value.obj.size() != names.size()) return false;
    std::set<std::string> found;
    for (const auto& pair : value.obj) {
        bool known = false;
        for (const char* name : names)
            if (pair.first == name) { known = true; break; }
        if (!known || !found.insert(pair.first).second) return false;
    }
    return true;
}

bool ValidVersion(const JsonValue& value) {
    const JsonValue* version = Field(value, "v");
    return version && version->type == JsonValue::Type::Number &&
           version->num == kProtocolVersion;
}

bool ReadDecimal(const JsonValue* value, std::uint64_t& result) {
    if (!value || value->type != JsonValue::Type::String || value->str.empty())
        return false;
    std::uint64_t parsed = 0;
    for (char ch : value->str) {
        if (ch < '0' || ch > '9') return false;
        const unsigned digit = static_cast<unsigned>(ch - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
            return false;
        parsed = parsed * 10 + digit;
    }
    result = parsed;
    return true;
}

bool ParseControl(const std::vector<std::uint8_t>& bytes, JsonValue& value) {
    if (bytes.empty() || bytes.size() > kMaxControlBytes || bytes[0] != '{')
        return false;
    try {
        value = json_parse(std::string(reinterpret_cast<const char*>(bytes.data()),
                                       bytes.size()));
        return value.is_object() && ValidVersion(value);
    } catch (...) {
        return false;
    }
}

bool GetOperation(const JsonValue& value, const char* expected) {
    const JsonValue* op = Field(value, "op");
    return op && op->type == JsonValue::Type::String && op->str == expected;
}

TransferResult PeerError(const JsonValue& value) {
    if (!ExactFields(value, {"v", "op", "code"}) || !ValidVersion(value) ||
        !GetOperation(value, "error"))
        return Failure(TransferError::Protocol, ErrorMessage(TransferError::Protocol));
    const JsonValue* code = Field(value, "code");
    if (!code || code->type != JsonValue::Type::String)
        return Failure(TransferError::Protocol, ErrorMessage(TransferError::Protocol));
    const TransferError error = DecodeErrorCode(code->str);
    return Failure(error, ErrorMessage(error));
}

bool ParseManifestFrame(const JsonValue& value, std::vector<TransferFile>& files) {
    if (!ExactFields(value, {"v", "op", "files"}) || !ValidVersion(value) ||
        !GetOperation(value, "manifest")) return false;
    const JsonValue* list = Field(value, "files");
    if (!list || !list->is_array() || list->arr.size() > kMaxManifestFiles)
        return false;
    files.clear();
    files.reserve(list->arr.size());
    for (const JsonValue& item : list->arr) {
        if (!ExactFields(item, {"path", "size", "sha256"})) return false;
        const JsonValue* path = Field(item, "path");
        const JsonValue* digest = Field(item, "sha256");
        TransferFile file;
        if (!path || path->type != JsonValue::Type::String ||
            !digest || digest->type != JsonValue::Type::String ||
            !ReadDecimal(Field(item, "size"), file.size)) return false;
        file.path = path->str;
        file.sha256 = digest->str;
        files.push_back(std::move(file));
    }
    return true;
}

TransferResult ReadReply(detail::FrameTransport transport, const char* expected,
                         JsonValue& reply) {
    std::vector<std::uint8_t> frame;
    if (!ReadFrame(transport, frame))
        return Failure(TransferError::Interrupted, ErrorMessage(TransferError::Interrupted));
    if (!ParseControl(frame, reply))
        return Failure(TransferError::Protocol, ErrorMessage(TransferError::Protocol));
    if (GetOperation(reply, "error")) return PeerError(reply);
    if (!ExactFields(reply, {"v", "op"}) || !GetOperation(reply, expected))
        return Failure(TransferError::Protocol, ErrorMessage(TransferError::Protocol));
    return {};
}

bool SourceFileReady(const fs::path& root, const TransferFile& file,
                     fs::path& path) {
    if (!CheckPath(root, file.path, false, path)) return false;
    bool exists = false;
    std::uint64_t size = 0;
    return SingleLinkedRegularFile(path, false, exists, size) && exists &&
           size == file.size;
}

TransferResult SendArtifactsImpl(detail::FrameTransport transport,
                                 const fs::path& supplied_root,
                                 std::vector<TransferFile> files,
                                 std::uint64_t disk_budget_bytes) {
    if (!transport.send || !transport.receive)
        return Failure(TransferError::InvalidArgument, ErrorMessage(TransferError::InvalidArgument));
    TransferResult valid = ValidateManifest(files, disk_budget_bytes);
    if (!valid) return valid;
    fs::path root;
    if (!GetRoot(supplied_root, root))
        return Failure(TransferError::InvalidArgument, ErrorMessage(TransferError::InvalidArgument));
    for (const TransferFile& file : files) {
        fs::path source;
        if (!SourceFileReady(root, file, source))
            return Failure(TransferError::Filesystem, ErrorMessage(TransferError::Filesystem));
    }
    const std::string manifest = ManifestFrame(files);
    if (manifest.size() > kMaxControlBytes)
        return Failure(TransferError::InvalidManifest,
                       ErrorMessage(TransferError::InvalidManifest));
    if (!SendRaw(transport, manifest))
        return Failure(TransferError::Interrupted, ErrorMessage(TransferError::Interrupted));
    JsonValue reply;
    TransferResult received = ReadReply(transport, "manifest-ok", reply);
    if (!received) return received;

    std::vector<std::uint8_t> chunk(kChunkHeaderBytes + kChunkBytes);
    for (const TransferFile& file : files) {
        fs::path source;
        if (!SourceFileReady(root, file, source))
            return SendError(transport, TransferError::Filesystem);
        const std::string announcement = FileFrame("file", file.path);
        if (!SendRaw(transport, announcement))
            return Failure(TransferError::Interrupted, ErrorMessage(TransferError::Interrupted));
        std::vector<std::uint8_t> ready_frame;
        if (!ReadFrame(transport, ready_frame))
            return Failure(TransferError::Interrupted, ErrorMessage(TransferError::Interrupted));
        if (!ParseControl(ready_frame, reply))
            return Failure(TransferError::Protocol, ErrorMessage(TransferError::Protocol));
        if (GetOperation(reply, "error")) return PeerError(reply);
        std::uint64_t offset = 0;
        if (!ExactFields(reply, {"v", "op", "offset"}) ||
            !GetOperation(reply, "ready") ||
            !ReadDecimal(Field(reply, "offset"), offset) || offset > file.size)
            return Failure(TransferError::Protocol, ErrorMessage(TransferError::Protocol));

        std::ifstream input(NativeFilesystemPath(source), std::ios::binary);
        if (!input) return SendError(transport, TransferError::Filesystem);
        std::string source_digest;
        if (!HashStream(input, file.size, source_digest))
            return SendError(transport, TransferError::Filesystem);
        if (source_digest != file.sha256)
            return SendError(transport, TransferError::Integrity);
        input.clear();
        input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!input) return SendError(transport, TransferError::Filesystem);
        std::uint64_t position = offset;
        while (position < file.size) {
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uint64_t>(kChunkBytes, file.size - position));
            input.read(reinterpret_cast<char*>(chunk.data() + kChunkHeaderBytes),
                       static_cast<std::streamsize>(want));
            if (input.gcount() != static_cast<std::streamsize>(want))
                return SendError(transport, TransferError::Filesystem);
            chunk[0] = kChunkTag;
            for (unsigned i = 0; i < 8; ++i)
                chunk[1 + i] = static_cast<std::uint8_t>(position >> (56 - 8 * i));
            if (!SendBytes(transport, chunk.data(), kChunkHeaderBytes + want))
                return Failure(TransferError::Interrupted, ErrorMessage(TransferError::Interrupted));
            position += want;
        }
        const std::string commit = FileFrame("commit", file.path);
        if (!SendRaw(transport, commit))
            return Failure(TransferError::Interrupted, ErrorMessage(TransferError::Interrupted));
        received = ReadReply(transport, "complete", reply);
        if (!received) return received;
    }
    if (!SendRaw(transport, SimpleFrame("done")))
        return Failure(TransferError::Interrupted, ErrorMessage(TransferError::Interrupted));
    return ReadReply(transport, "done", reply);
}

bool CreateEmptyFile(const fs::path& path) {
    std::ofstream output(NativeFilesystemPath(path),
                         std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.flush();
    output.close();
    return !output.fail();
}

TransferResult ReceiveArtifactsImpl(detail::FrameTransport transport,
                                    const fs::path& supplied_root,
                                    std::vector<TransferFile> approved,
                                    std::uint64_t disk_budget_bytes) {
    if (!transport.send || !transport.receive)
        return Failure(TransferError::InvalidArgument, ErrorMessage(TransferError::InvalidArgument));
    TransferResult valid = ValidateManifest(approved, disk_budget_bytes);
    if (!valid) return valid;
    fs::path root;
    if (!GetRoot(supplied_root, root))
        return Failure(TransferError::InvalidArgument, ErrorMessage(TransferError::InvalidArgument));

    std::vector<std::uint8_t> frame;
    JsonValue control;
    if (!ReadFrame(transport, frame))
        return Failure(TransferError::Interrupted, ErrorMessage(TransferError::Interrupted));
    if (!ParseControl(frame, control))
        return SendError(transport, TransferError::Protocol);
    if (GetOperation(control, "error")) return PeerError(control);
    std::vector<TransferFile> incoming;
    if (!ParseManifestFrame(control, incoming))
        return SendError(transport, TransferError::InvalidManifest);
    valid = ValidateManifest(incoming, disk_budget_bytes);
    if (!valid) return SendError(transport, valid.error);
    if (!SameManifest(approved, incoming))
        return SendError(transport, TransferError::InvalidManifest);
    valid = CheckStagingQuota(root, approved, disk_budget_bytes);
    if (!valid) return SendError(transport, valid.error);
    if (!SendRaw(transport, SimpleFrame("manifest-ok")))
        return Failure(TransferError::Interrupted, ErrorMessage(TransferError::Interrupted));

    for (const TransferFile& file : approved) {
        if (!ReadFrame(transport, frame))
            return Failure(TransferError::Interrupted, ErrorMessage(TransferError::Interrupted));
        if (!ParseControl(frame, control))
            return SendError(transport, TransferError::Protocol);
        if (GetOperation(control, "error")) return PeerError(control);
        const JsonValue* path_value = Field(control, "path");
        if (!ExactFields(control, {"v", "op", "path"}) ||
            !GetOperation(control, "file") || !path_value ||
            path_value->type != JsonValue::Type::String || path_value->str != file.path)
            return SendError(transport, TransferError::Protocol);

        fs::path destination;
        if (!CheckPath(root, file.path, true, destination))
            return SendError(transport, TransferError::Filesystem);
        bool published = false;
        if (!ExistingMatches(root, file.path, file, published))
            return SendError(transport, TransferError::Filesystem);

        fs::path stage, partial;
        std::uint64_t offset = file.size;
        std::ofstream output;
        if (!published) {
            if (!EnsureStageDirectory(root, stage))
                return SendError(transport, TransferError::Filesystem);
            const std::string name = PartialName(file);
            if (name.empty()) return SendError(transport, TransferError::Filesystem);
            partial = stage / fs::u8path(name);
            bool exists = false;
            std::uint64_t partial_size = 0;
            if (!SingleLinkedRegularFile(partial, true, exists, partial_size))
                return SendError(transport, TransferError::Filesystem);
            if (exists && partial_size > file.size) {
                if (!RemoveFile(partial)) return SendError(transport, TransferError::Filesystem);
                exists = false;
                partial_size = 0;
            }
            if (exists && partial_size == file.size) {
                std::string digest;
                if (!HashFile(partial, file.size, digest))
                    return SendError(transport, TransferError::Filesystem);
                if (digest != file.sha256) {
                    if (!RemoveFile(partial)) return SendError(transport, TransferError::Filesystem);
                    exists = false;
                    partial_size = 0;
                }
            }
            if (!exists) {
                if (!CreateEmptyFile(partial))
                    return SendError(transport, TransferError::Filesystem);
                partial_size = 0;
            }
            offset = partial_size;
            if (offset < file.size) {
                output.open(NativeFilesystemPath(partial),
                            std::ios::binary | std::ios::app);
                if (!output) return SendError(transport, TransferError::Filesystem);
            }
        }
        if (!SendRaw(transport, ReadyFrame(offset)))
            return Failure(TransferError::Interrupted, ErrorMessage(TransferError::Interrupted));

        std::uint64_t position = offset;
        while (position < file.size) {
            if (!ReadFrame(transport, frame))
                return Failure(TransferError::Interrupted, ErrorMessage(TransferError::Interrupted));
            if (!frame.empty() && frame[0] == '{') {
                if (!ParseControl(frame, control))
                    return SendError(transport, TransferError::Protocol);
                if (GetOperation(control, "error")) return PeerError(control);
                return SendError(transport, TransferError::Protocol);
            }
            if (frame.size() <= kChunkHeaderBytes ||
                frame.size() > kChunkHeaderBytes + kChunkBytes ||
                frame[0] != kChunkTag)
                return SendError(transport, TransferError::Protocol);
            std::uint64_t chunk_offset = 0;
            for (unsigned i = 0; i < 8; ++i)
                chunk_offset = (chunk_offset << 8) | frame[1 + i];
            const std::size_t bytes = frame.size() - kChunkHeaderBytes;
            const std::size_t expected = static_cast<std::size_t>(
                std::min<std::uint64_t>(kChunkBytes, file.size - position));
            if (chunk_offset != position || bytes != expected)
                return SendError(transport, TransferError::Protocol);
            output.write(reinterpret_cast<const char*>(frame.data() + kChunkHeaderBytes),
                         static_cast<std::streamsize>(bytes));
            output.flush();
            if (!output) return SendError(transport, TransferError::Filesystem);
            position += bytes;
        }
        if (output.is_open()) {
            output.close();
            if (output.fail()) return SendError(transport, TransferError::Filesystem);
        }

        if (!ReadFrame(transport, frame))
            return Failure(TransferError::Interrupted, ErrorMessage(TransferError::Interrupted));
        if (!ParseControl(frame, control))
            return SendError(transport, TransferError::Protocol);
        if (GetOperation(control, "error")) return PeerError(control);
        const JsonValue* committed_path = Field(control, "path");
        if (!ExactFields(control, {"v", "op", "path"}) ||
            !GetOperation(control, "commit") || !committed_path ||
            committed_path->type != JsonValue::Type::String ||
            committed_path->str != file.path)
            return SendError(transport, TransferError::Protocol);

        if (published) {
            bool still_matches = false;
            if (!ExistingMatches(root, file.path, file, still_matches))
                return SendError(transport, TransferError::Filesystem);
            if (!still_matches) return SendError(transport, TransferError::Integrity);
        } else {
            bool partial_matches = false;
            const std::string name = PartialName(file);
            if (!ExistingMatches(root, std::string(kStageDir) + "/" + name,
                                 file, partial_matches))
                return SendError(transport, TransferError::Filesystem);
            if (!partial_matches) {
                if (!RemoveFile(partial))
                    return SendError(transport, TransferError::Filesystem);
                return SendError(transport, TransferError::Integrity);
            }
            if (!CheckPath(root, file.path, true, destination))
                return SendError(transport, TransferError::Filesystem);
            bool destination_exists = false;
            std::uint64_t destination_size = 0;
            if (!SingleLinkedRegularFile(destination, true, destination_exists,
                                         destination_size))
                return SendError(transport, TransferError::Filesystem);
            if (!ReplaceFile(partial, destination))
                return SendError(transport, TransferError::Filesystem);
            bool final_matches = false;
            if (!ExistingMatches(root, file.path, file, final_matches))
                return SendError(transport, TransferError::Filesystem);
            if (!final_matches) return SendError(transport, TransferError::Integrity);
        }
        if (!SendRaw(transport, SimpleFrame("complete")))
            return Failure(TransferError::Interrupted, ErrorMessage(TransferError::Interrupted));
    }

    if (!ReadFrame(transport, frame))
        return Failure(TransferError::Interrupted, ErrorMessage(TransferError::Interrupted));
    if (!ParseControl(frame, control))
        return SendError(transport, TransferError::Protocol);
    if (GetOperation(control, "error")) return PeerError(control);
    if (!ExactFields(control, {"v", "op"}) || !GetOperation(control, "done"))
        return SendError(transport, TransferError::Protocol);
    if (!SendRaw(transport, SimpleFrame("done")))
        return Failure(TransferError::Interrupted, ErrorMessage(TransferError::Interrupted));
    return {};
}

bool TlsSend(void* context, const std::uint8_t* bytes, std::size_t size,
             std::string* error) {
    return static_cast<TlsChannel*>(context)->SendFrame(bytes, size, error);
}

bool TlsReceive(void* context, std::vector<std::uint8_t>& bytes,
                std::string* error) {
    return static_cast<TlsChannel*>(context)->ReceiveFrame(bytes, error);
}

}  // namespace

TransferResult SendArtifacts(TlsChannel& channel, const fs::path& source_root,
                             const std::vector<TransferFile>& approved_manifest,
                             std::uint64_t disk_budget_bytes) {
    return detail::SendArtifacts({&channel, TlsSend, TlsReceive}, source_root,
                                 approved_manifest, disk_budget_bytes);
}

TransferResult ReceiveArtifacts(TlsChannel& channel, const fs::path& staging_root,
                                const std::vector<TransferFile>& approved_manifest,
                                std::uint64_t disk_budget_bytes) {
    return detail::ReceiveArtifacts({&channel, TlsSend, TlsReceive}, staging_root,
                                    approved_manifest, disk_budget_bytes);
}

namespace detail {

TransferResult SendArtifacts(FrameTransport transport, const fs::path& source_root,
                             const std::vector<TransferFile>& approved_manifest,
                             std::uint64_t disk_budget_bytes) {
    try {
        return SendArtifactsImpl(transport, source_root, approved_manifest,
                                 disk_budget_bytes);
    } catch (...) {
        return Failure(TransferError::Filesystem, ErrorMessage(TransferError::Filesystem));
    }
}

TransferResult ReceiveArtifacts(FrameTransport transport,
                                const fs::path& staging_root,
                                const std::vector<TransferFile>& approved_manifest,
                                std::uint64_t disk_budget_bytes) {
    try {
        return ReceiveArtifactsImpl(transport, staging_root, approved_manifest,
                                    disk_budget_bytes);
    } catch (...) {
        return Failure(TransferError::Filesystem, ErrorMessage(TransferError::Filesystem));
    }
}

}  // namespace detail
}  // namespace app::agent
