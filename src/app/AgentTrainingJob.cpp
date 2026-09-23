#include "app/AgentTrainingJob.h"
#include "core/FilesystemPath.h"

#include "checkpoint/Resume.h"
#include "checkpoint/SplatPly.h"
#include "config/TrainConfigJson.h"
#include "core/CheckpointIO.h"
#include "core/Sha256.h"
#include "data/DatasetParser.h"
#include "data/ImageProbe.h"
#include "data/JsonWrite.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <cstdlib>

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
using spirula::LogicalAbsoluteFilesystemPath;
using spirula::LogicalFilesystemPath;
using spirula::NativeFilesystemPath;
namespace {
namespace fs = std::filesystem;

constexpr std::size_t kMaxFiles = 4096;
constexpr std::size_t kMaxPathBytes = 4096;
constexpr std::size_t kMaxRecordBytes = 1u << 20;
constexpr std::uint64_t kMaxDatasetJsonBytes = 64u << 20;
constexpr std::size_t kMaxEntries = kMaxFiles * 4;
constexpr std::size_t kChunkBytes = 64u << 10;
constexpr char kInputDir[] = "input";
constexpr char kDatasetDir[] = "dataset";
constexpr char kConfigFile[] = "config.json";
constexpr char kTrainingFile[] = "training.json";
constexpr char kTransferStage[] = ".agent-transfer-v1";
constexpr char kOutputLock[] = ".spirula-output.lock";

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("agent training: " + message);
}

std::string lower_ascii(std::string value) {
    for (char& c : value)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return value;
}

bool reserved_windows_name(const std::string& component) {
    const std::string stem =
        lower_ascii(component.substr(0, component.find('.')));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul")
        return true;
    return stem.size() == 4 &&
           (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) &&
           stem[3] >= '1' && stem[3] <= '9';
}

void safe_relative_path(const std::string& value) {
    if (value.empty() || value.size() > kMaxPathBytes || value.front() == '/' ||
        value.back() == '/' || value.find('\\') != std::string::npos ||
        value.find(':') != std::string::npos)
        fail("path is not normalized and relative");
    std::size_t begin = 0;
    while (begin < value.size()) {
        const std::size_t end = value.find('/', begin);
        const std::string component = value.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if (component.empty() || component == "." || component == ".." ||
            component.size() > 255 || component.back() == '.' ||
            component.back() == ' ' || reserved_windows_name(component))
            fail("path contains an unsafe component: " + value);
        for (const unsigned char c : component)
            if (c < 0x20 || c == 0x7f || c == '<' || c == '>' || c == '"' ||
                c == '|' || c == '?' || c == '*')
                fail("path contains an unsafe character: " + value);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
}

bool valid_digest(const std::string& digest) {
    if (digest.size() != 64) return false;
    for (const char c : digest)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

#ifdef _WIN32
bool is_reparse_point(const fs::path& path) {
    const DWORD attributes = GetFileAttributesW(
        NativeFilesystemPath(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}
#else
bool is_reparse_point(const fs::path&) { return false; }
#endif

fs::path canonical_path(const fs::path& path, std::error_code& ec) {
    return LogicalFilesystemPath(fs::canonical(NativeFilesystemPath(path), ec));
}

fs::path canonical_path(const fs::path& path) {
    return LogicalFilesystemPath(fs::canonical(NativeFilesystemPath(path)));
}

fs::file_status path_status(const fs::path& path) {
    std::error_code ec;
    const fs::file_status status =
        fs::symlink_status(NativeFilesystemPath(path), ec);
    if (!ec) return status;
    if (ec == std::errc::no_such_file_or_directory ||
        ec == std::errc::not_a_directory)
        return fs::file_status(fs::file_type::not_found);
    fail("cannot inspect a filesystem path: " + path.u8string());
}

bool missing(fs::file_status status) {
    return status.type() == fs::file_type::not_found ||
           status.type() == fs::file_type::none;
}

bool within(const fs::path& root, const fs::path& candidate) {
    auto r = root.begin();
    auto c = candidate.begin();
    for (; r != root.end(); ++r, ++c) {
        if (c == candidate.end()) return false;
#ifdef _WIN32
        if (lower_ascii(r->u8string()) != lower_ascii(c->u8string())) return false;
#else
        if (*r != *c) return false;
#endif
    }
    return true;
}

bool same_path(const fs::path& a, const fs::path& b) {
    return within(a, b) && within(b, a);
}

fs::path safe_root(const fs::path& supplied, const char* what) {
    if (supplied.empty()) fail(std::string(what) + " is empty");
    std::error_code ec;
    const fs::path absolute = LogicalAbsoluteFilesystemPath(supplied, ec);
    if (ec || !absolute.is_absolute())
        fail(std::string(what) + " cannot be resolved");
    const fs::file_status status = path_status(absolute);
    if (!fs::is_directory(status) || fs::is_symlink(status) ||
        is_reparse_point(absolute))
        fail(std::string(what) + " is missing, not a directory, or linked");
    const fs::path canonical = canonical_path(absolute, ec);
    if (ec || !canonical.is_absolute())
        fail(std::string(what) + " cannot be resolved");
    return canonical;
}

std::uint64_t regular_file_size(const fs::path& path, const fs::path& root,
                                const std::string& relative) {
    const fs::file_status status = path_status(path);
    if (!fs::is_regular_file(status) || fs::is_symlink(status) ||
        is_reparse_point(path))
        fail("not an unlinked regular file: " + relative);
    std::error_code ec;
    if (fs::hard_link_count(NativeFilesystemPath(path), ec) != 1 || ec)
        fail("hard-linked file is not allowed: " + relative);
    const fs::path resolved = canonical_path(path, ec);
    if (ec || !within(root, resolved)) fail("file escapes its root: " + relative);
    const std::uintmax_t size = fs::file_size(NativeFilesystemPath(path), ec);
    if (ec || size > static_cast<std::uintmax_t>(
                       std::numeric_limits<std::streamoff>::max()))
        fail("cannot size file: " + relative);
    return static_cast<std::uint64_t>(size);
}

std::string file_digest(const fs::path& path, std::uint64_t size,
                        const std::string& relative) {
    std::ifstream input(NativeFilesystemPath(path), std::ios::binary);
    if (!input) fail("cannot read file: " + relative);
    spirula::Sha256 hash;
    std::array<char, kChunkBytes> buffer{};
    std::uint64_t total = 0;
    while (total < size) {
        const std::size_t want = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), size - total));
        input.read(buffer.data(), static_cast<std::streamsize>(want));
        if (input.gcount() != static_cast<std::streamsize>(want))
            fail("file changed or could not be read: " + relative);
        hash.update(reinterpret_cast<const std::uint8_t*>(buffer.data()), want);
        total += want;
    }
    char extra = 0;
    input.read(&extra, 1);
    if (input.gcount() != 0 || input.bad())
        fail("file changed while reading: " + relative);
    return hash.hex();
}

std::string read_bounded_file(const fs::path& path, const fs::path& root,
                              const std::string& relative,
                              std::uint64_t max_bytes = kMaxRecordBytes) {
    const std::uint64_t size = regular_file_size(path, root, relative);
    if (size > max_bytes) fail("record file is too large: " + relative);
    std::ifstream input(NativeFilesystemPath(path), std::ios::binary);
    if (!input) fail("cannot read record file: " + relative);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (size && !input.read(bytes.data(), static_cast<std::streamsize>(size)))
        fail("cannot read record file: " + relative);
    char extra = 0;
    input.read(&extra, 1);
    if (input.gcount() != 0 || input.bad())
        fail("record file changed while reading: " + relative);
    return bytes;
}

std::vector<TransferFile> validate_manifest(std::vector<TransferFile> files,
                                            std::uint64_t budget) {
    if (files.empty() || files.size() > kMaxFiles)
        fail("manifest has an invalid file count");
    std::sort(files.begin(), files.end(), [](const TransferFile& a,
                                             const TransferFile& b) {
        return a.path < b.path;
    });
    std::set<std::string> paths;
    std::uint64_t total = 0;
    for (const TransferFile& file : files) {
        safe_relative_path(file.path);
        if (!valid_digest(file.sha256)) fail("manifest has an invalid digest");
        if (file.size > static_cast<std::uint64_t>(
                            std::numeric_limits<std::streamoff>::max()) ||
            file.size > std::numeric_limits<std::uint64_t>::max() - total)
            fail("manifest size overflows");
        total += file.size;
        if (total > budget) fail("bundle exceeds the disk quota");
        if (!paths.insert(lower_ascii(file.path)).second)
            fail("manifest paths collide");
        for (std::size_t slash = file.path.find('/'); slash != std::string::npos;
             slash = file.path.find('/', slash + 1))
            if (paths.count(lower_ascii(file.path.substr(0, slash))))
                fail("manifest file is also a parent directory");
    }
    return files;
}

void hash_u64(spirula::Sha256& hash, std::uint64_t value) {
    std::uint8_t bytes[8];
    for (int i = 0; i < 8; ++i)
        bytes[i] = static_cast<std::uint8_t>(value >> (56 - i * 8));
    hash.update(bytes, sizeof(bytes));
}

std::string manifest_identity(const std::vector<TransferFile>& manifest) {
    spirula::Sha256 hash;
    static constexpr char domain[] = "spirula-training-input-v1";
    hash.update(reinterpret_cast<const std::uint8_t*>(domain), sizeof(domain));
    for (const TransferFile& file : manifest) {
        hash_u64(hash, file.path.size());
        hash.update(reinterpret_cast<const std::uint8_t*>(file.path.data()),
                    file.path.size());
        hash_u64(hash, file.size);
        hash.update(reinterpret_cast<const std::uint8_t*>(file.sha256.data()),
                    file.sha256.size());
    }
    return hash.hex();
}

void validate_exact_tree(const fs::path& supplied_root,
                         const std::vector<TransferFile>& approved,
                         std::uint64_t budget, bool allow_transfer_stage,
                         bool allow_output_lock) {
    const fs::path root = safe_root(supplied_root, "artifact root");
    const std::vector<TransferFile> expected = validate_manifest(approved, budget);
    std::set<std::string> allowed_dirs;
    for (const TransferFile& file : expected) {
        for (std::size_t slash = file.path.find('/'); slash != std::string::npos;
             slash = file.path.find('/', slash + 1))
            allowed_dirs.insert(file.path.substr(0, slash));
    }

    std::vector<TransferFile> actual;
    std::set<std::string> actual_dirs;
    std::size_t entries = 0;
    std::uint64_t total = 0;
    std::error_code ec;
    fs::recursive_directory_iterator it(
        NativeFilesystemPath(root), fs::directory_options::none, ec), end;
    if (ec) fail("cannot enumerate artifact tree");
    for (; it != end; it.increment(ec)) {
        if (++entries > kMaxEntries) fail("artifact tree has too many entries");
        if (ec) fail("cannot enumerate artifact tree");
        const fs::path path = LogicalFilesystemPath(it->path());
        const std::string relative = path.lexically_relative(root).generic_u8string();
        const fs::file_status status = path_status(path);
        if (fs::is_symlink(status) || is_reparse_point(path))
            fail("artifact tree contains a linked path: " + relative);
        const fs::path resolved = canonical_path(path, ec);
        if (ec || !within(root, resolved))
            fail("artifact path escapes its root: " + relative);

        if (relative == kTransferStage && fs::is_directory(status) &&
            allow_transfer_stage) {
            if (!fs::is_empty(NativeFilesystemPath(path), ec) || ec)
                fail("transfer staging directory is not empty");
            it.disable_recursion_pending();
            continue;
        }
        if (relative == kOutputLock && allow_output_lock) {
            if (!fs::is_regular_file(status) ||
                regular_file_size(path, root, relative) != 0)
                fail("output lease marker is not an empty regular file");
            continue;
        }
        safe_relative_path(relative);
        if (fs::is_directory(status)) {
            actual_dirs.insert(relative);
            continue;
        }
        if (!fs::is_regular_file(status))
            fail("artifact tree contains a non-regular entry: " + relative);
        const std::uint64_t size = regular_file_size(path, root, relative);
        if (size > budget - total) fail("artifact tree exceeds the disk quota");
        total += size;
        actual.push_back({relative, size, file_digest(path, size, relative)});
        if (actual.size() > kMaxFiles) fail("artifact tree has too many files");
    }
    for (const std::string& directory : actual_dirs)
        if (!allowed_dirs.count(directory))
            fail("artifact tree has an unexpected directory: " + directory);
    const std::vector<TransferFile> observed = validate_manifest(
        std::move(actual), budget);
    if (observed.size() != expected.size())
        fail("artifact tree does not match its approved manifest");
    for (std::size_t i = 0; i < expected.size(); ++i)
        if (observed[i].path != expected[i].path ||
            observed[i].size != expected[i].size ||
            observed[i].sha256 != expected[i].sha256)
            fail("artifact digest or size mismatch: " + expected[i].path);
}

fs::path checked_relative(const fs::path& root, const std::string& relative,
                          bool must_exist, bool directory) {
    safe_relative_path(relative);
    fs::path current = root;
    std::size_t begin = 0;
    for (;;) {
        const std::size_t slash = relative.find('/', begin);
        const bool last = slash == std::string::npos;
        current /= fs::u8path(relative.substr(
            begin, last ? std::string::npos : slash - begin));
        const fs::file_status status = path_status(current);
        if (last) {
            if (missing(status)) {
                if (must_exist) fail("required artifact is missing: " + relative);
                return current;
            }
            if (fs::is_symlink(status) || is_reparse_point(current) ||
                (directory ? !fs::is_directory(status) : !fs::is_regular_file(status)))
                fail("artifact has the wrong type or is linked: " + relative);
        } else if (missing(status) || !fs::is_directory(status) ||
                   fs::is_symlink(status) || is_reparse_point(current)) {
            fail("artifact parent is missing or linked: " + relative);
        }
        std::error_code ec;
        const fs::path resolved = canonical_path(current, ec);
        if (ec || !within(root, resolved))
            fail("artifact path escapes its root: " + relative);
        if (last) return current;
        begin = slash + 1;
    }
}


std::string normalized_source_path(const fs::path& root,
                                   const std::string& value,
                                   const char* what, bool directory,
                                   bool allow_missing) {
    if (value.empty()) return {};
    if (value.find('\0') != std::string::npos)
        fail(std::string(what) + " contains an embedded NUL");
    fs::path specified = fs::u8path(value);
    if (specified.has_root_name() || specified.has_root_directory()) {
        if (!specified.is_absolute())
            fail(std::string(what) + " has an incomplete absolute path");
    } else {
        for (const fs::path& part : specified)
            if (part == fs::path(".."))
                fail(std::string(what) + " contains a parent traversal");
    }
    fs::path candidate = specified.is_absolute() ? specified : root / specified;
    candidate = candidate.lexically_normal();
    if (!within(root, candidate))
        fail(std::string(what) + " is outside the dataset root");
    fs::file_status status = path_status(candidate);
    if (missing(status)) {
        if (!allow_missing || specified.is_absolute())
            fail(std::string(what) + " does not exist");
        return candidate.lexically_relative(root).generic_u8string();
    }
    if (fs::is_symlink(status) || is_reparse_point(candidate) ||
        (directory ? !fs::is_directory(status) : !fs::is_regular_file(status)))
        fail(std::string(what) + " has the wrong type or is linked");
    std::error_code ec;
    const fs::path canonical = canonical_path(candidate, ec);
    if (ec || !within(root, canonical))
        fail(std::string(what) + " resolves outside the dataset root");
    return canonical.lexically_relative(root).generic_u8string();
}

fs::path resolve_data_root(const std::string& value) {
    if (value.empty() || value.find('\0') != std::string::npos)
        fail("training data path is empty or malformed");
    const fs::path supplied = fs::u8path(value);
    std::error_code ec;
    const fs::path absolute = LogicalAbsoluteFilesystemPath(supplied, ec);
    if (ec || !absolute.is_absolute()) fail("cannot resolve training data path");
    return safe_root(absolute, "training data root");
}
fs::path resolve_checkpoint_data_root(const std::string& value,
                                      const fs::path& checkpoint,
                                      const fs::path& expected_root) {
    if (value.empty() || value.find('\0') != std::string::npos)
        fail("resume checkpoint has an empty or malformed data path");
    const fs::path supplied = fs::u8path(value);
    if (supplied.is_absolute()) {
        const fs::path resolved = resolve_data_root(value);
        if (!same_path(resolved, expected_root))
            fail("resume checkpoint is bound to a different dataset root");
        return resolved;
    }
    if (supplied.has_root_name() || supplied.has_root_directory())
        fail("resume checkpoint has an incomplete data path");

    std::error_code ec;
    fs::path candidate = LogicalAbsoluteFilesystemPath(supplied, ec);
    if (!ec) {
        const fs::file_status status = path_status(candidate);
        if (fs::is_directory(status) && !fs::is_symlink(status) &&
            !is_reparse_point(candidate)) {
            const fs::path resolved = safe_root(candidate, "checkpoint data root");
            if (same_path(resolved, expected_root)) return resolved;
        }
    }
    for (fs::path base = checkpoint; !base.empty(); base = base.parent_path()) {
        candidate = (base / supplied).lexically_normal();
        const fs::file_status status = path_status(candidate);
        if (!fs::is_directory(status) || fs::is_symlink(status) ||
            is_reparse_point(candidate))
            continue;
        const fs::path resolved = safe_root(candidate, "checkpoint data root");
        if (same_path(resolved, expected_root)) return resolved;
        if (base == base.root_path()) break;
    }
    fail("resume checkpoint is bound to a different or unavailable dataset root");
}


void normalize_dataset_paths(TrainConfig& config, const fs::path& root) {
    config.data = root.u8string();
    config.image_dir = normalized_source_path(root, config.image_dir,
                                               "image directory", true, true);
    config.mask_dir = normalized_source_path(root, config.mask_dir,
                                              "mask directory", true, true);
    config.depth_dir = normalized_source_path(root, config.depth_dir,
                                               "depth directory", true, true);
    config.normal_dir = normalized_source_path(root, config.normal_dir,
                                                "normal directory", true, true);
    config.colmap_recon_dir = normalized_source_path(
        root, config.colmap_recon_dir, "COLMAP reconstruction directory", true,
        false);
    config.metashape_xml = normalized_source_path(
        root, config.metashape_xml, "Metashape XML", false, false);
    config.metashape_ply = normalized_source_path(
        root, config.metashape_ply, "Metashape point cloud", false, false);
    config.metashape_psx = normalized_source_path(
        root, config.metashape_psx, "Metashape project", false, false);
}

bool has_metashape_project(const fs::path& root) {
    std::error_code ec;
    for (fs::directory_iterator it(NativeFilesystemPath(root), ec), end;
         it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (lower_ascii(it->path().extension().u8string()) == ".psx") return true;
    }
    if (ec) fail("cannot inspect dataset format markers");
    return false;
}

void validate_finite_json(const JsonValue& value, const std::string& label,
                          bool allow_positive_infinity = false);
void validate_finite_dataset_values(const ParsedDataset& parsed);
std::string validate_nerfstudio_reference(const fs::path& root,
                                          const std::string& value,
                                          const char* field, bool required,
                                          bool must_exist) {
    if (value.empty()) {
        if (required) fail(std::string("Nerfstudio ") + field + " is empty");
        return {};
    }
    const fs::path relative = fs::u8path(value);
    if (relative.has_root_name() || relative.has_root_directory() ||
        relative.is_absolute() || value.find('\\') != std::string::npos ||
        value.find('\0') != std::string::npos)
        fail(std::string("Nerfstudio ") + field +
             " is absolute or not portable; use a dataset-relative path");
    for (const fs::path& part : relative)
        if (part == fs::path(".."))
            fail(std::string("Nerfstudio ") + field +
                 " contains a parent traversal");
    const fs::path candidate = (root / relative).lexically_normal();
    if (!within(root, candidate))
        fail(std::string("Nerfstudio ") + field + " escapes the dataset root");
    const fs::path canonical_root = safe_root(root, "dataset root");
    const std::string name =
        candidate.lexically_relative(canonical_root).generic_u8string();
    if (name.empty())
        fail(std::string("Nerfstudio ") + field + " names the dataset root");
    if (must_exist) {
        const fs::path checked = checked_relative(
            canonical_root, name, true, false);
        regular_file_size(checked, canonical_root, name);
    } else {
        fs::path current = canonical_root;
        const fs::path relative_path = fs::u8path(name);
        std::error_code ec;
        for (auto part = relative_path.begin();
             part != relative_path.end(); ++part) {
            current /= *part;
            const fs::file_status status = path_status(current);
            if (missing(status)) return name;
            auto next = part;
            ++next;
            const bool last = next == relative_path.end();
            if (fs::is_symlink(status) || is_reparse_point(current) ||
                (last ? !fs::is_regular_file(status)
                      : !fs::is_directory(status)))
                fail(std::string("Nerfstudio ") + field +
                     " has a linked path or wrong type");
            const fs::path resolved = canonical_path(current, ec);
            if (ec || !within(canonical_root, resolved))
                fail(std::string("Nerfstudio ") + field +
                     " escapes the dataset root");
            if (last) regular_file_size(current, canonical_root, name);
        }
    }
    return name;
}

std::string json_string_field(const JsonValue& object, const char* key,
                              bool required) {
    const JsonValue* value = object.find(key);
    if (!value) {
        if (required) fail(std::string("config is missing '") + key + "'");
        return {};
    }
    if (value->type != JsonValue::Type::String)
        fail(std::string("config field '") + key + "' is not a string");
    return value->str;
}

void validate_nerfstudio_references(const TrainConfig& config,
                                   const fs::path& root) {
    const fs::path transforms = root / "transforms.json";
    const std::string text = read_bounded_file(
        transforms, root, "transforms.json", kMaxDatasetJsonBytes);
    const JsonValue meta = json_parse(text);
    if (!meta.is_object()) fail("Nerfstudio transforms.json is not an object");
    validate_finite_json(meta, "transforms.json");
    const JsonValue* ply = meta.find("ply_file_path");
    if (ply) {
        if (ply->type != JsonValue::Type::String)
            fail("Nerfstudio ply_file_path is not a string");
        if (!ply->str.empty())
            validate_nerfstudio_reference(
                root, ply->str, "ply_file_path", false, true);
    } else {
        for (const char* candidate : {"sparse_pc.ply", "pointcloud.ply"}) {
            const fs::path path = root / candidate;
            if (!missing(path_status(path))) {
                validate_nerfstudio_reference(
                    root, candidate, "initial point cloud", false, true);
                break;
            }
        }
    }
    const JsonValue* frames = meta.find("frames");
    if (!frames || !frames->is_array())
        fail("Nerfstudio transforms.json has no frame array");
    for (const JsonValue& frame : frames->arr) {
        if (!frame.is_object()) fail("Nerfstudio frame is not an object");
        if (frame.find("file_path")) {
            const std::string path = json_string_field(frame, "file_path", true);
            const bool supported =
                fs::u8path(path).extension().u8string() != ".webp";
            validate_nerfstudio_reference(
                root, path, "file_path", true, supported);
        }
        const std::pair<const char*, bool> fields[] = {
            {"mask_path", config.load_masks},
            {"depth_file_path",
             config.load_depths && config.depth_supervision_weight > 0.0f},
            {"normal_file_path",
             config.load_normals &&
                 (config.normal_supervision_weight > 0.0f ||
                  config.median_normal_supervision_weight > 0.0f)}};
        for (const auto& [field, needed] : fields)
            if (frame.find(field))
                validate_nerfstudio_reference(
                    root, json_string_field(frame, field, true),
                    field, false, needed);
    }
}

DatasetParserConfig parser_config(const TrainConfig& config) {
    DatasetParserConfig parser;
    parser.recon_dir = config.colmap_recon_dir;
    parser.image_dir = config.image_dir;
    parser.mask_dir = config.mask_dir;
    parser.depth_dir = config.depth_dir;
    parser.normal_dir = config.normal_dir;
    parser.validation_fraction = config.validation_fraction;
    parser.eval_mode = config.eval_mode;
    parser.eval_interval = config.eval_interval;
    parser.train_split_fraction = config.train_split_fraction;
    parser.outlier_threshold = config.outlier_threshold;
    parser.center_mode = config.scene_center;
    parser.exif_orientation = config.exif_orientation;
    parser.probe_image_size = probe_image_size;
    parser.train_resolution_divisor = config.train_resolution_divisor;
    parser.downscale_rounding_mode = config.downscale_rounding_mode;
    parser.metashape_xml = config.metashape_xml;
    parser.metashape_ply = config.metashape_ply;
    parser.metashape_psx = config.metashape_psx;
    return parser;
}

void logicalize_parsed_paths(ParsedDataset& parsed) {
#ifdef _WIN32
    const auto logicalize = [](std::vector<std::string>& paths) {
        for (std::string& path : paths)
            if (path.rfind("\\\\?\\", 0) == 0)
                path = LogicalFilesystemPath(fs::u8path(path)).u8string();
    };
    logicalize(parsed.image_filenames);
    logicalize(parsed.mask_filenames);
    logicalize(parsed.depth_filenames);
    logicalize(parsed.normal_filenames);
#endif
}

void validate_parsed_path(const fs::path& root, const std::string& value,
                          const char* what) {
    if (value.empty()) return;
    const fs::path path = LogicalFilesystemPath(fs::u8path(value));
    std::error_code ec;
    const fs::path absolute = LogicalAbsoluteFilesystemPath(path, ec);
    if (ec || !within(root, absolute.lexically_normal()))
        fail(std::string("parsed ") + what + " escapes the staged dataset");
    const fs::path normalized = absolute.lexically_normal();
    const std::string relative = normalized.lexically_relative(root).generic_u8string();
    if (relative.empty()) fail(std::string("parsed ") + what + " names the root");
    for (const fs::path& part : fs::u8path(relative))
        if (part == fs::path(".."))
            fail(std::string("parsed ") + what + " contains a parent traversal");
    const fs::path checked = checked_relative(root, relative, true, false);
    regular_file_size(checked, root, relative);
}

std::string dataset_relative_name(const fs::path& root,
                                 const fs::path& path) {
    const fs::path normalized = LogicalFilesystemPath(path).lexically_normal();
    if (!within(root, normalized))
        fail("dataset dependency escapes the dataset root");
    const std::string relative =
        normalized.lexically_relative(root).generic_u8string();
    safe_relative_path(relative);
    return relative;
}

fs::path safe_dataset_file(const fs::path& root, const fs::path& supplied) {
    const fs::path path = (supplied.is_absolute() ? supplied : root / supplied)
                              .lexically_normal();
    const std::string relative = dataset_relative_name(root, path);
    const fs::path checked = checked_relative(root, relative, true, false);
    regular_file_size(checked, root, relative);
    return checked;
}

struct ColmapModel {
    fs::path directory;
    fs::path cameras;
    fs::path images_path;
    fs::path points;
    fs::path gauge;
};

fs::path colmap_file(const fs::path& root, const fs::path& directory,
                     const char* base) {
    for (const char* extension : {".bin", ".txt"}) {
        const fs::path path = directory / (std::string(base) + extension);
        if (!missing(path_status(path))) return safe_dataset_file(root, path);
    }
    return {};
}

int64_t colmap_model_num_images(const fs::path& root,
                                const fs::path& directory) {
    const fs::path binary = directory / "images.bin";
    if (!missing(path_status(binary))) {
        const fs::path checked = safe_dataset_file(root, binary);
        std::ifstream input(NativeFilesystemPath(checked), std::ios::binary);
        std::uint64_t count = 0;
        if (input.read(reinterpret_cast<char*>(&count), sizeof(count)))
            return static_cast<int64_t>(count);
    }
    const fs::path text = directory / "images.txt";
    if (missing(path_status(text))) return -1;
    const fs::path checked = safe_dataset_file(root, text);
    std::ifstream input(NativeFilesystemPath(checked));
    std::string line;
    int64_t rows = 0;
    while (std::getline(input, line)) {
        if (!line.empty() && line[0] == '#') {
            const std::size_t marker = line.find("Number of images:");
            if (marker != std::string::npos)
                return std::strtoll(line.c_str() + marker + 17, nullptr, 10);
            continue;
        }
        if (!line.empty()) ++rows;
    }
    if (!input.eof()) fail("cannot read COLMAP image metadata");
    return rows / 2;
}

std::optional<ColmapModel> selected_colmap_model(
    const TrainConfig& config, const fs::path& root) {
    std::vector<fs::path> probes;
    if (!config.colmap_recon_dir.empty()) {
        probes.push_back(root / fs::u8path(config.colmap_recon_dir));
    } else {
        struct RankedModel {
            int64_t count;
            fs::path path;
            std::string relative;
        };
        std::vector<RankedModel> ranked;
        for (const char* parent : {"sparse", "colmap/sparse"}) {
            const fs::path path = root / parent;
            const fs::file_status parent_status = path_status(path);
            if (missing(parent_status)) continue;
            if (fs::is_symlink(parent_status) || is_reparse_point(path) ||
                !fs::is_directory(parent_status))
                fail("COLMAP model parent is not an unlinked directory");
            std::error_code ec;
            for (fs::directory_iterator it(NativeFilesystemPath(path), ec), end;
                 it != end && !ec; it.increment(ec)) {
                const fs::path candidate = LogicalFilesystemPath(it->path());
                const fs::file_status status = path_status(candidate);
                if (fs::is_symlink(status) || is_reparse_point(candidate))
                    fail("COLMAP model path is linked");
                if (!fs::is_directory(status)) continue;
                const fs::path model = safe_root(candidate, "COLMAP model");
                const int64_t count = colmap_model_num_images(root, model);
                if (count >= 0)
                    ranked.push_back({
                        count, model,
                        model.lexically_relative(root).generic_u8string()});
            }
            if (ec) fail("cannot enumerate COLMAP model directories");
        }
        std::sort(ranked.begin(), ranked.end(),
                  [](const RankedModel& a, const RankedModel& b) {
                      return a.count != b.count ? a.count > b.count
                                                : a.relative < b.relative;
                  });
        for (const RankedModel& model : ranked) probes.push_back(model.path);
        for (const char* relative :
             {"sparse/0", "colmap/sparse/0", "sparse", "colmap", ""})
            probes.push_back(relative[0] ? root / relative : root);
    }

    const fs::path image_root = config.image_dir.empty()
        ? root : safe_root(root / fs::u8path(config.image_dir),
                           "dataset image directory");
    for (const fs::path& supplied : probes) {
        const fs::file_status status = path_status(supplied);
        if (missing(status)) continue;
        if (fs::is_symlink(status) || is_reparse_point(supplied) ||
            !fs::is_directory(status))
            fail("COLMAP model path is not an unlinked directory");
        const fs::path directory = safe_root(supplied, "COLMAP model");
        ColmapModel model;
        model.directory = directory;
        model.cameras = colmap_file(root, directory, "cameras");
        model.images_path = colmap_file(root, directory, "images");
        model.points = colmap_file(root, directory, "points3D");
        if (model.cameras.empty() || model.images_path.empty() ||
            model.points.empty())
            continue;
        const fs::path gauge = directory / "gauge.txt";
        if (!missing(path_status(gauge)))
            model.gauge = safe_dataset_file(root, gauge);
        const std::map<int32_t, ColmapImage> images =
            model.images_path.extension() == ".txt"
                ? read_images_text(directory.u8string())
                : read_images_binary(directory.u8string());
        for (const auto& [id, image] : images) {
            (void)id;
            safe_relative_path(image.name);
            const fs::path image_path =
                checked_relative(image_root, image.name, true, false);
            regular_file_size(image_path, image_root, image.name);
        }
        return model;
    }
    return std::nullopt;
}

struct MetashapeInputs {
    fs::path xml;
    fs::path ply;
    std::vector<fs::path> images;
};

fs::path metashape_input(const fs::path& root, const std::string& configured,
                         const char* extension, bool required) {
    if (!configured.empty()) {
        const fs::path path = safe_dataset_file(root, fs::u8path(configured));
        if (lower_ascii(path.extension().u8string()) != extension)
            fail(std::string("Metashape input must have a ") + extension +
                 " extension");
        return path;
    }
    std::vector<fs::path> found;
    std::error_code ec;
    for (fs::directory_iterator it(NativeFilesystemPath(root), ec), end;
         it != end && !ec; it.increment(ec)) {
        const fs::path path = LogicalFilesystemPath(it->path());
        if (lower_ascii(path.extension().u8string()) != extension) continue;
        const fs::file_status status = path_status(path);
        if (fs::is_symlink(status) || is_reparse_point(path))
            fail("Metashape input is linked");
        if (fs::is_regular_file(status))
            found.push_back(safe_dataset_file(root, path));
    }
    if (ec) fail("cannot inspect Metashape input files");
    if (found.size() > 1)
        fail(std::string("multiple Metashape ") + extension +
             " files require an explicit path");
    if (found.empty()) {
        if (required)
            fail(std::string("Metashape ") + extension + " file is missing");
        return {};
    }
    return found.front();
}

MetashapeInputs validate_metashape_inputs(const TrainConfig& config,
                                          const fs::path& root) {
    if (!config.metashape_psx.empty() || has_metashape_project(root))
        fail("Metashape .psx projects are not portable: the existing parser reads "
             "photo paths inside the project archive but cannot rebase them");
    MetashapeInputs inputs{
        metashape_input(root, config.metashape_xml, ".xml", true),
        metashape_input(root, config.metashape_ply, ".ply", true)};
    const fs::path image_root = config.image_dir.empty()
        ? root : safe_root(root / fs::u8path(config.image_dir),
                           "dataset image directory");
    std::error_code ec;
    fs::recursive_directory_iterator it(
        NativeFilesystemPath(image_root), fs::directory_options::none, ec), end;
    if (ec) fail("cannot enumerate Metashape image directory");
    for (; it != end; it.increment(ec)) {
        if (ec) fail("cannot enumerate Metashape image directory");
        const fs::path path = LogicalFilesystemPath(it->path());
        const std::string relative = dataset_relative_name(root, path);
        const fs::file_status status = path_status(path);
        if (fs::is_symlink(status) || is_reparse_point(path))
            fail("Metashape image directory contains a linked path: " + relative);
        if (fs::is_directory(status)) {
            const fs::path resolved = canonical_path(path, ec);
            if (ec || !within(root, resolved))
                fail("Metashape image directory escapes the dataset root");
        } else if (fs::is_regular_file(status)) {
            regular_file_size(path, root, relative);
            inputs.images.push_back(path);
        } else {
            fail("Metashape image directory contains a non-file entry: " +
                 relative);
        }
    }
    if (ec) fail("cannot enumerate Metashape image directory");
    return inputs;
}
ParsedDataset validate_dataset(
    const TrainConfig& config, const fs::path& staged_root,
    std::string* selected_format = nullptr,
    std::optional<ColmapModel>* selected_colmap = nullptr) {
    const fs::path root = safe_root(staged_root, "staged dataset root");
    if (selected_colmap) selected_colmap->reset();
    TrainConfig local = config;
    normalize_dataset_paths(local, root);
    if (local.data_format != "" && local.data_format != "colmap" &&
        local.data_format != "nerfstudio" && local.data_format != "metashape")
        fail("unsupported training data format");

    const DatasetParserConfig parser = parser_config(local);
    ParsedDataset parsed;
    std::string format = local.data_format;
    const bool auto_nerfstudio = format.empty() &&
        !missing(path_status(root / "transforms.json"));
    const bool auto_colmap = format.empty() && !auto_nerfstudio;
    if ((format == "metashape" || auto_colmap) &&
        (!local.metashape_psx.empty() || has_metashape_project(root)))
        fail("Metashape .psx projects are not portable: the existing parser reads "
             "photo paths inside the project archive but cannot rebase them");
    if (format == "nerfstudio" || auto_nerfstudio) {
        validate_nerfstudio_references(local, root);
        parsed = parse_nerfstudio_dataset(
            root.u8string(), parser);
        format = "nerfstudio";
    } else if (format == "colmap") {
        std::optional<ColmapModel> model =
            selected_colmap_model(local, root);
        parsed = parse_colmap_dataset(root.u8string(), parser);
        if (selected_colmap) *selected_colmap = std::move(model);
    } else if (format == "metashape") {
        (void)validate_metashape_inputs(local, root);
        parsed = parse_metashape_dataset(
            root.u8string(), parser);
    } else {
        std::optional<ColmapModel> model =
            selected_colmap_model(local, root);
        if (model) {
            try {
                parsed = parse_colmap_dataset(
                    root.u8string(), parser);
                if (selected_colmap) *selected_colmap = std::move(model);
                format = "colmap";
            } catch (const std::exception&) {
                (void)validate_metashape_inputs(local, root);
                parsed = parse_metashape_dataset(
                    root.u8string(), parser);
                format = "metashape";
            }
        } else {
            (void)validate_metashape_inputs(local, root);
            parsed = parse_metashape_dataset(
                root.u8string(), parser);
            format = "metashape";
        }
    }
    logicalize_parsed_paths(parsed);
    if (parsed.num_cameras <= 0 ||
        parsed.image_filenames.size() != static_cast<std::size_t>(parsed.num_cameras))
        fail("dataset parser accepted no training images");
    validate_finite_dataset_values(parsed);
    for (const std::string& path : parsed.image_filenames)
        validate_parsed_path(root, path, "image path");
    if (local.load_masks)
        for (const std::string& path : parsed.mask_filenames)
            validate_parsed_path(root, path, "mask path");
    if (local.load_depths && local.depth_supervision_weight > 0.0f)
        for (const std::string& path : parsed.depth_filenames)
            validate_parsed_path(root, path, "depth path");
    if (local.load_normals &&
        (local.normal_supervision_weight > 0.0f ||
         local.median_normal_supervision_weight > 0.0f))
        for (const std::string& path : parsed.normal_filenames)
            validate_parsed_path(root, path, "normal path");
    if (selected_format) *selected_format = format;
    return parsed;
}
ParsedDataset parse_all_dataset_frames(const TrainConfig& config,
                                       const fs::path& supplied_root,
                                       const std::string& format) {
    const fs::path root = safe_root(supplied_root, "dataset root");
    TrainConfig local = config;
    normalize_dataset_paths(local, root);
    DatasetParserConfig parser = parser_config(local);
    parser.eval_mode = "all";
    parser.split = "train";
    ParsedDataset parsed;
    if (format == "nerfstudio")
        parsed = parse_nerfstudio_dataset(
            root.u8string(), parser);
    else if (format == "colmap")
        parsed = parse_colmap_dataset(root.u8string(), parser);
    else if (format == "metashape")
        parsed = parse_metashape_dataset(
            root.u8string(), parser);
    else
        fail("unsupported parser format for all-frame selection");
    logicalize_parsed_paths(parsed);
    if (parsed.num_cameras <= 0 ||
        parsed.image_filenames.size() != static_cast<std::size_t>(parsed.num_cameras))
        fail("dataset parser accepted no usable images");
    validate_finite_dataset_values(parsed);
    for (const std::string& path : parsed.image_filenames)
        validate_parsed_path(root, path, "image path");
    if (local.load_masks)
        for (const std::string& path : parsed.mask_filenames)
            validate_parsed_path(root, path, "mask path");
    if (local.load_depths && local.depth_supervision_weight > 0.0f)
        for (const std::string& path : parsed.depth_filenames)
            validate_parsed_path(root, path, "depth path");
    if (local.load_normals &&
        (local.normal_supervision_weight > 0.0f ||
         local.median_normal_supervision_weight > 0.0f))
        for (const std::string& path : parsed.normal_filenames)
            validate_parsed_path(root, path, "normal path");
    return parsed;
}

void validate_finite_json(const JsonValue& value, const std::string& label,
                          bool allow_positive_infinity) {
    if (value.type == JsonValue::Type::Number) {
        if (!std::isfinite(value.num) &&
            !(allow_positive_infinity && value.num > 0 &&
              std::isinf(value.num)))
            fail(label + " contains a non-finite number");
    } else if (value.type == JsonValue::Type::Array) {
        for (const JsonValue& child : value.arr)
            validate_finite_json(child, label, allow_positive_infinity);
    } else if (value.type == JsonValue::Type::Object) {
        std::set<std::string> keys;
        for (const auto& [key, child] : value.obj) {
            if (!keys.insert(key).second)
                fail(label + " contains a duplicate key");
            validate_finite_json(child, label + "." + key,
                                 allow_positive_infinity);
        }
    }
}

void validate_finite_config(const TrainConfig& config) {
    for (const auto& [key, encoded] : train_config_json_pairs(config)) {
        const std::string name(key);
        const bool infinity_sentinel =
            name == "outlier_threshold" || name == "max_screen_size" ||
            name == "max_world_size";
        validate_finite_json(json_parse(encoded), std::string("config.") + key,
                             infinity_sentinel);
    }
}

template <typename Range>
void require_finite_values(const Range& values, const char* label) {
    for (const auto& value : values)
        if (!std::isfinite(static_cast<double>(value)))
            fail(std::string("dataset has a non-finite ") + label);
}

void validate_finite_dataset_values(const ParsedDataset& parsed) {
    require_finite_values(parsed.points.xyz, "point coordinate");
    require_finite_values(parsed.c2w, "camera transform");
    require_finite_values(parsed.intrins, "camera intrinsic");
    require_finite_values(parsed.dist_coeffs, "camera distortion");
    require_finite_values(parsed.center, "scene center");
    require_finite_values(parsed.train_to_normalized, "scene transform");
    require_finite_values(parsed.normalized_rotation, "scene rotation");
    if (!std::isfinite(parsed.train_frame_scale))
        fail("dataset has a non-finite scene scale");
    for (const RedistortSource& source : parsed.redistort) {
        if (!std::isfinite(source.fit_max_px))
            fail("dataset has a non-finite redistortion error");
        require_finite_values(source.params, "redistortion parameter");
    }
    for (const int32_t width : parsed.widths)
        if (width <= 0) fail("dataset has a non-positive image width");
    for (const int32_t height : parsed.heights)
        if (height <= 0) fail("dataset has a non-positive image height");
}


std::vector<fs::path> dataset_dependencies(
    const TrainConfig& config, const fs::path& supplied_root,
    const std::string& format,
    const std::optional<ColmapModel>& selected_colmap,
    const ParsedDataset& all_frames) {
    const fs::path root = safe_root(supplied_root, "dataset root");
    TrainConfig local = config;
    normalize_dataset_paths(local, root);
    std::map<std::string, fs::path> dependencies;
    auto add_file = [&](const fs::path& supplied) {
        const fs::path path = safe_dataset_file(root, supplied);
        const std::string relative = dataset_relative_name(root, path);
        const std::string key = lower_ascii(relative);
        const auto [it, inserted] = dependencies.emplace(key, path);
        if (!inserted &&
            dataset_relative_name(root, it->second) != relative)
            fail("required dataset paths collide on a portable filesystem");
    };
    auto add_paths = [&](const std::vector<std::string>& paths) {
        for (const std::string& path : paths)
            if (!path.empty()) add_file(fs::u8path(path));
    };

    for (const std::string& image : all_frames.image_filenames)
        add_file(fs::u8path(image));
    if (local.load_masks) add_paths(all_frames.mask_filenames);
    if (local.load_depths && local.depth_supervision_weight > 0.0f)
        add_paths(all_frames.depth_filenames);
    if (local.load_normals &&
        (local.normal_supervision_weight > 0.0f ||
         local.median_normal_supervision_weight > 0.0f))
        add_paths(all_frames.normal_filenames);

    if (format == "nerfstudio") {
        add_file(root / "transforms.json");
        const JsonValue meta = json_parse(read_bounded_file(
            root / "transforms.json", root, "transforms.json",
            kMaxDatasetJsonBytes));
        if (const JsonValue* ply = meta.find("ply_file_path")) {
            const std::string path =
                json_string_field(meta, "ply_file_path", false);
            if (!path.empty())
                add_file(fs::u8path(validate_nerfstudio_reference(
                    root, path, "ply_file_path", false, true)));
        } else {
            for (const char* candidate : {"sparse_pc.ply", "pointcloud.ply"}) {
                if (!missing(path_status(root / candidate))) {
                    add_file(root / candidate);
                    break;
                }
            }
        }
    } else if (format == "colmap") {
        if (!selected_colmap)
            fail("selected COLMAP reconstruction is unavailable");
        add_file(selected_colmap->cameras);
        add_file(selected_colmap->images_path);
        add_file(selected_colmap->points);
        if (!selected_colmap->gauge.empty())
            add_file(selected_colmap->gauge);
    } else if (format == "metashape") {
        const MetashapeInputs inputs = validate_metashape_inputs(local, root);
        add_file(inputs.xml);
        add_file(inputs.ply);
        for (const fs::path& image : inputs.images) add_file(image);
    } else {
        fail("unsupported parser format for training dependencies");
    }

    if (dependencies.empty()) fail("dataset has no parser dependencies");
    std::vector<fs::path> files;
    files.reserve(dependencies.size());
    for (const auto& [key, path] : dependencies) {
        (void)key;
        files.push_back(path);
    }
    return files;
}

std::string config_json(const TrainConfig& config, const std::string& preset) {
    JsonWriter writer;
    writer.object().field("preset", preset);
    for (const auto& [key, value] : train_config_json_pairs(config))
        writer.field_raw(key, value);
    return writer.end().str();
}

void write_bytes(const fs::path& path, const std::string& bytes,
                 const std::string& description) {
    std::ofstream output(NativeFilesystemPath(path),
                         std::ios::binary | std::ios::trunc);
    if (!output || (!bytes.empty() &&
                    !output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))))
        fail("cannot write " + description);
    output.flush();
    if (!output) fail("cannot finish " + description);
}

std::string training_metadata_json(TrainingMode mode, const std::string& preset,
                                   const std::string& resume_path) {
    JsonWriter writer;
    writer.object()
        .field("schema", 1)
        .field("mode", mode == TrainingMode::Fresh ? "fresh" : "resume")
        .field("preset", preset)
        .field("resume_checkpoint", resume_path)
        .end();
    return writer.str();
}

std::vector<std::pair<std::string, std::string>> required_config_fields(
    const std::string& preset) {
    TrainConfig empty;
    const auto pairs = train_config_json_pairs(empty);
    std::vector<std::pair<std::string, std::string>> expected;
    expected.reserve(pairs.size() + 1);
    expected.emplace_back("preset", preset);
    for (const auto& pair : pairs) expected.emplace_back(pair.first, pair.second);
    return expected;
}

TrainConfig parse_config_bytes(const std::string& bytes,
                               const std::string& expected_preset) {
    const JsonValue root = json_parse(bytes);
    if (!root.is_object() || !train_config_json_has_fields(root))
        fail("training config is missing or malformed");
    const std::string preset = json_string_field(root, "preset", true);
    if (preset != expected_preset) fail("training config preset identity mismatch");
    TrainConfig config;
    train_config_from_json(root, config);
    validate_finite_config(config);

    const auto fields = required_config_fields(expected_preset);
    std::set<std::string> expected_keys;
    for (const auto& field : fields) expected_keys.insert(field.first);
    std::set<std::string> actual_keys;
    for (const auto& field : root.obj) {
        if (!actual_keys.insert(field.first).second)
            fail("training config contains a duplicate key");
    }
    if (actual_keys != expected_keys)
        fail("training config has missing or unexpected fields");
    return config;
}

bool same_config(const TrainConfig& a, const TrainConfig& b) {
    const auto left = train_config_json_pairs(a);
    const auto right = train_config_json_pairs(b);
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i)
        if (std::string(left[i].first) != right[i].first ||
            left[i].second != right[i].second)
            return false;
    return true;
}

std::string config_preset(const fs::path& path, const fs::path& root,
                          const std::string& relative) {
    const std::string bytes = read_bounded_file(path, root, relative);
    const JsonValue json = json_parse(bytes);
    if (!json.is_object()) fail("training config is not an object");
    return json_string_field(json, "preset", true);
}

TransferFile copy_file(const fs::path& source_root, const fs::path& source,
                       const fs::path& destination_root,
                       const std::string& relative,
                       std::uint64_t& remaining_budget) {
    safe_relative_path(relative);
    const std::uint64_t size = regular_file_size(source, source_root, relative);
    if (size > remaining_budget) fail("bundle exceeds the disk quota");
    const fs::path destination = destination_root / fs::u8path(relative);
    std::error_code ec;
    fs::create_directories(NativeFilesystemPath(destination.parent_path()), ec);
    if (ec) fail("cannot create staged dataset directory");
    if (!missing(path_status(destination)))
        fail("staging destination already exists: " + relative);
    std::ifstream input(NativeFilesystemPath(source), std::ios::binary);
    std::ofstream output(NativeFilesystemPath(destination),
                         std::ios::binary | std::ios::trunc);
    if (!input || !output) fail("cannot stage file: " + relative);
    spirula::Sha256 hash;
    std::array<char, kChunkBytes> buffer{};
    std::uint64_t total = 0;
    while (total < size) {
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), size - total));
        input.read(buffer.data(), static_cast<std::streamsize>(count));
        if (input.gcount() != static_cast<std::streamsize>(count))
            fail("source changed while staging: " + relative);
        output.write(buffer.data(), static_cast<std::streamsize>(count));
        if (!output) fail("cannot stage file: " + relative);
        hash.update(reinterpret_cast<const std::uint8_t*>(buffer.data()), count);
        total += count;
    }
    char extra = 0;
    input.read(&extra, 1);
    if (input.gcount() != 0 || input.bad())
        fail("source changed while staging: " + relative);
    output.flush();
    if (!output || total != size) fail("cannot finish staged file: " + relative);
    remaining_budget -= size;
    return {relative, size, hash.hex()};
}

TransferFile write_record(const fs::path& root, const std::string& relative,
                          const std::string& bytes,
                          std::uint64_t& remaining_budget) {
    if (bytes.size() > kMaxRecordBytes || bytes.size() > remaining_budget)
        fail("training record exceeds its limit");
    safe_relative_path(relative);
    const fs::path destination = root / fs::u8path(relative);
    std::error_code ec;
    fs::create_directories(NativeFilesystemPath(destination.parent_path()), ec);
    if (ec || !missing(path_status(destination)))
        fail("cannot create training record: " + relative);
    write_bytes(destination, bytes, relative);
    spirula::Sha256 hash;
    hash.update(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    remaining_budget -= bytes.size();
    return {relative, static_cast<std::uint64_t>(bytes.size()), hash.hex()};
}

std::string relative_data_path(const std::string& value,
                               const fs::path& workspace,
                               const char* what) {
    if (value.empty() || value.find('\0') != std::string::npos)
        fail(std::string(what) + " is empty or malformed");
    const fs::path path = fs::u8path(value);
    if (path.has_root_name() || path.has_root_directory() || path.is_absolute())
        fail(std::string(what) + " must be a worker-relative path");
    for (const fs::path& part : path)
        if (part == fs::path(".."))
            fail(std::string(what) + " contains a parent traversal");
    const fs::path candidate = (workspace / path).lexically_normal();
    if (!within(workspace, candidate))
        fail(std::string(what) + " escapes the worker workspace");
    return candidate.u8string();
}

void check_bundle_integrity(const TrainingInputBundle& inputs,
                            std::uint64_t budget) {
    if (inputs.root.empty() || inputs.workspace_root.empty())
        fail("training input bundle has an invalid workspace binding");
    const fs::path workspace = safe_root(inputs.workspace_root, "attempt workspace");
    const fs::path root = safe_root(inputs.root, "input directory");
    if (!same_path(workspace, inputs.workspace_root) ||
        !same_path(root, inputs.root) ||
        inputs.root.filename().u8string() != kInputDir ||
        !same_path(inputs.root.parent_path(), inputs.workspace_root) ||
        !same_path(inputs.dataset_root, inputs.root / kDatasetDir) ||
        !same_path(inputs.config_path, inputs.root / kConfigFile) ||
        !same_path(inputs.output_root,
                   inputs.workspace_root / "output" / "run"))
        fail("training input bundle has an invalid workspace binding");
    validate_exact_tree(inputs.root, inputs.files, budget, true, false);
    const std::vector<TransferFile> manifest =
        validate_manifest(inputs.files, budget);
    const std::string identity = manifest_identity(manifest);
    if (!valid_digest(inputs.identity_sha256) ||
        identity != inputs.identity_sha256)
        fail("training input identity mismatch");

    const TrainConfig frozen = parse_config_bytes(
        read_bounded_file(inputs.config_path, inputs.root, kConfigFile),
        inputs.preset);
    if (!same_config(frozen, inputs.config) ||
        frozen.data != "input/dataset" || frozen.output_dir_prefix != "output" ||
        frozen.output_dir_name != "run" || !frozen.save_full_checkpoint ||
        !frozen.disable_viewer)
        fail("training input bundle differs from its frozen config");
    const std::string metadata_bytes = read_bounded_file(
        inputs.root / kTrainingFile, inputs.root, kTrainingFile);
    const JsonValue metadata = json_parse(metadata_bytes);
    if (!metadata.is_object()) fail("training metadata is malformed");
    const JsonValue* schema = metadata.find("schema");
    if (!schema || schema->type != JsonValue::Type::Number || schema->num != 1)
        fail("training metadata schema is unsupported");
    const std::string expected_mode =
        inputs.mode == TrainingMode::Fresh ? "fresh" : "resume";
    const std::string mode = json_string_field(metadata, "mode", true);
    const std::string preset = json_string_field(metadata, "preset", true);
    const std::string resume = json_string_field(
        metadata, "resume_checkpoint", true);
    TrainConfig preset_check;
    if (!train_apply_preset(preset_check, inputs.preset))
        fail("training input bundle has an unknown preset");
    if (mode != expected_mode || preset != inputs.preset ||
        resume != frozen.resume ||
        (inputs.mode == TrainingMode::Fresh) != resume.empty())
        fail("training input bundle differs from its frozen metadata");
    const std::set<std::string> metadata_keys = {
        "schema", "mode", "preset", "resume_checkpoint"};
    std::set<std::string> actual_metadata_keys;
    for (const auto& item : metadata.obj)
        if (!actual_metadata_keys.insert(item.first).second)
            fail("training metadata contains duplicate fields");
    if (actual_metadata_keys != metadata_keys)
        fail("training metadata contains missing or unexpected fields");
    if (resume.empty()) {
        if (!inputs.resume_checkpoint.empty())
            fail("fresh training input has a resume checkpoint");
    } else if (resume.rfind("input/resume/", 0) != 0 ||
               !same_path(inputs.resume_checkpoint,
                          inputs.workspace_root / fs::u8path(resume))) {
        fail("resume checkpoint differs from its frozen metadata");
    }
}

std::string checkpoint_name_for_step(std::uint64_t step) {
    std::string digits = std::to_string(step);
    if (digits.size() < 9) digits.insert(0, 9 - digits.size(), '0');
    return "step-" + digits + ".ckpt";
}

std::uint64_t parse_checkpoint_name(const std::string& name) {
    if (name.size() < 5 + 9 || name.compare(0, 5, "step-") != 0 ||
        name.compare(name.size() - 5, 5, ".ckpt") != 0)
        fail("checkpoint name is not an explicit step-*.ckpt directory");
    const std::string digits = name.substr(5, name.size() - 10);
    if (!std::all_of(digits.begin(), digits.end(), [](char c) {
            return c >= '0' && c <= '9';
        }))
        fail("checkpoint name has an invalid step");
    std::uint64_t step = 0;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), step);
    if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() ||
        checkpoint_name_for_step(step) != name)
        fail("checkpoint name has a non-canonical step");
    return step;
}

void validate_checkpoint(const fs::path& root, const std::string& relative,
                         const TrainConfig& expected_config,
                         const std::string& preset) {
    const std::uint64_t named_step = parse_checkpoint_name(
        fs::u8path(relative).filename().u8string());
    const fs::path directory = checked_relative(root, relative, true, true);
    const std::set<std::string> expected_files = {
        relative + "/config.json", relative + "/splat.ply", relative + "/state.tar"};
    std::set<std::string> actual_files;
    std::error_code ec;
    for (fs::directory_iterator it(NativeFilesystemPath(directory), ec), end;
         it != end && !ec; it.increment(ec)) {
        const std::string name = it->path().filename().u8string();
        const fs::file_status status = path_status(it->path());
        if (fs::is_symlink(status) || is_reparse_point(it->path()) ||
            !fs::is_regular_file(status))
            fail("checkpoint contains a linked or non-file artifact");
        actual_files.insert(relative + "/" + name);
    }
    if (ec || actual_files != expected_files)
        fail("checkpoint has missing or unexpected files: " + relative);

    const fs::path config_path = directory / kConfigFile;
    const std::string config_text = read_bounded_file(
        config_path, root, relative + "/config.json");
    const TrainConfig checkpoint_config = parse_config_bytes(config_text, preset);
    if (!same_config(checkpoint_config, expected_config))
        fail("checkpoint config does not match frozen training inputs");
    const fs::path splat = directory / "splat.ply";
    (void)regular_file_size(splat, root, relative + "/splat.ply");
    if (!spirula::is_splat_ply(NativeFilesystemPath(splat).u8string()))
        fail("checkpoint splat.ply is not loadable");
    (void)regular_file_size(directory / "state.tar", root,
                            relative + "/state.tar");
    const JsonValue state = ckpt::read_state_json(
        directory);
    const JsonValue* step = state.find("step");
    if (named_step > 9007199254740991ULL ||
        !step || step->type != JsonValue::Type::Number ||
        !std::isfinite(step->num) || std::trunc(step->num) != step->num ||
        step->num != static_cast<double>(named_step))
        fail("checkpoint state step does not match its directory identity");
    ckpt::check_resumable(directory);
}

bool eval_image_name(const std::string& name) {
    for (const char* prefix : {"eval-gt-", "eval-render-"}) {
        const std::string start(prefix);
        if (name.compare(0, start.size(), start) != 0 ||
            name.size() < start.size() + 5 + 4 ||
            name.compare(name.size() - 4, 4, ".png") != 0)
            continue;
        const std::string digits = name.substr(
            start.size(), name.size() - start.size() - 4);
        if (digits.size() < 5 ||
            !std::all_of(digits.begin(), digits.end(), [](char c) {
                return c >= '0' && c <= '9';
            }))
            continue;
        return true;
    }
    return false;
}

std::vector<TransferFile> inspect_output_tree(
    const fs::path& supplied_root, const TrainConfig& config,
    std::uint64_t budget, bool worker_source) {
    const fs::path root = safe_root(supplied_root, "training output root");
    std::vector<TransferFile> files;
    std::set<std::string> checkpoint_dirs;
    std::size_t entries = 0;
    std::uint64_t total = 0;
    std::error_code ec;
    fs::recursive_directory_iterator it(
        NativeFilesystemPath(root), fs::directory_options::none, ec), end;
    if (ec) fail("cannot enumerate training outputs");
    for (; it != end; it.increment(ec)) {
        if (++entries > kMaxEntries) fail("training output has too many entries");
        if (ec) fail("cannot enumerate training outputs");
        const fs::path path = LogicalFilesystemPath(it->path());
        const std::string relative = path.lexically_relative(root).generic_u8string();
        const fs::file_status status = path_status(path);
        if (fs::is_symlink(status) || is_reparse_point(path))
            fail("training outputs contain a linked path: " + relative);
        const fs::path resolved = canonical_path(path, ec);
        if (ec || !within(root, resolved))
            fail("training output escapes its root: " + relative);

        if (relative == kOutputLock && worker_source) {
            if (!fs::is_regular_file(status) ||
                regular_file_size(path, root, relative) != 0)
                fail("output lease marker is not empty");
            continue;
        }
        safe_relative_path(relative);
        if (fs::is_directory(status)) {
            if (relative.find('/') == std::string::npos) {
                (void)parse_checkpoint_name(relative);
                checkpoint_dirs.insert(relative);
                if (checkpoint_dirs.size() > kMaxFiles)
                    fail("training output has too many checkpoints");
                continue;
            }
            fail("training outputs contain an unexpected directory: " + relative);
        }
        if (!fs::is_regular_file(status))
            fail("training outputs contain a non-regular entry: " + relative);
        const std::size_t slash = relative.find('/');
        if (slash == std::string::npos) {
            const bool fixed = relative == "config.json" ||
                               relative == "scene_transform.json" ||
                               relative == "metrics.json";
            if (!fixed && !(config.save_eval_images && eval_image_name(relative)))
                fail("training outputs contain an unexpected file: " + relative);
        } else {
            const std::string checkpoint = relative.substr(0, slash);
            if (!checkpoint_dirs.count(checkpoint) ||
                relative.find('/', slash + 1) != std::string::npos)
                fail("training outputs contain an unexpected checkpoint file: " + relative);
            const std::string leaf = relative.substr(slash + 1);
            if (leaf != "config.json" && leaf != "splat.ply" && leaf != "state.tar")
                fail("training checkpoint has an unexpected file: " + relative);
        }
        const std::uint64_t size = regular_file_size(path, root, relative);
        if (size > budget - total)
            fail("training output exceeds the disk quota");
        total += size;
        files.push_back({relative, size, file_digest(path, size, relative)});
        if (files.size() > kMaxFiles) fail("training outputs have too many files");
    }
    if (ec) fail("cannot enumerate training outputs");
    const std::vector<TransferFile> manifest = validate_manifest(
        std::move(files), budget);
    if (!checkpoint_dirs.size()) fail("training output has no checkpoint");
    const std::set<std::string> seen_files = [&] {
        std::set<std::string> result;
        for (const TransferFile& file : manifest) result.insert(file.path);
        return result;
    }();
    if (!seen_files.count("config.json") ||
        !seen_files.count("scene_transform.json"))
        fail("training output is missing config.json or scene_transform.json");

    for (const std::string& checkpoint : checkpoint_dirs) {
        for (const char* leaf : {"config.json", "splat.ply", "state.tar"})
            if (!seen_files.count(checkpoint + "/" + leaf))
                fail("checkpoint is missing a required output: " + checkpoint);
    }
    return manifest;
}

void validate_output_semantics(const TrainingInputBundle& inputs,
                               const fs::path& root,
                               const std::vector<TransferFile>& files,
                               const std::string& returned_checkpoint) {
    const std::string root_config = read_bounded_file(
        root / kConfigFile, root, kConfigFile);
    const TrainConfig produced = parse_config_bytes(root_config, inputs.preset);
    if (!same_config(produced, inputs.config))
        fail("returned run config differs from frozen training inputs");
    const std::set<std::string> paths = [&] {
        std::set<std::string> result;
        for (const TransferFile& file : files) result.insert(file.path);
        return result;
    }();
    if (!paths.count(returned_checkpoint + "/state.tar") ||
        !paths.count(returned_checkpoint + "/config.json") ||
        !paths.count(returned_checkpoint + "/splat.ply"))
        fail("explicit returned checkpoint is not in the approved output manifest");
    std::set<std::string> checkpoints;
    for (const TransferFile& file : files) {
        const std::size_t slash = file.path.find('/');
        if (slash == std::string::npos) continue;
        checkpoints.insert(file.path.substr(0, slash));
    }
    if (checkpoints.empty()) fail("training output has no checkpoints");
    for (const std::string& checkpoint : checkpoints)
        validate_checkpoint(root, checkpoint, inputs.config, inputs.preset);
    auto validate_json_record = [&](const char* name) {
        if (!paths.count(name)) return;
        const JsonValue value = json_parse(
            read_bounded_file(root / name, root, name));
        if (!value.is_object()) fail(std::string(name) + " is malformed");
        validate_finite_json(value, name);
    };
    validate_json_record("scene_transform.json");
    validate_json_record("metrics.json");
}

TrainingInputBundle input_bundle(const fs::path& workspace,
                                 const std::string& preset,
                                 const TrainConfig& config,
                                 const std::vector<TransferFile>& files,
                                 const std::string& identity) {
    TrainingInputBundle bundle;
    bundle.workspace_root = workspace;
    bundle.root = workspace / kInputDir;
    bundle.dataset_root = bundle.root / kDatasetDir;
    bundle.config_path = bundle.root / kConfigFile;
    bundle.resume_checkpoint = config.resume.empty()
        ? fs::path() : workspace / fs::u8path(config.resume);
    bundle.output_root = workspace / "output" / "run";
    bundle.mode = config.resume.empty() ? TrainingMode::Fresh : TrainingMode::Resume;
    bundle.preset = preset;
    bundle.config = config;
    bundle.identity_sha256 = identity;
    bundle.files = files;
    return bundle;
}

void validate_workspace_layout(const TrainingInputBundle& inputs,
                               bool output_expected) {
    const fs::path workspace = safe_root(inputs.workspace_root, "attempt workspace");
    if (!same_path(workspace, inputs.workspace_root))
        fail("attempt workspace is not canonical");
    const std::set<std::string> expected = output_expected
        ? std::set<std::string>{"input", "output"}
        : std::set<std::string>{"input"};
    std::set<std::string> actual;
    std::error_code ec;
    for (fs::directory_iterator it(NativeFilesystemPath(workspace), ec), end;
         it != end && !ec; it.increment(ec)) {
        const fs::file_status status = path_status(it->path());
        const std::string name = it->path().filename().u8string();
        if (fs::is_symlink(status) || is_reparse_point(it->path()) ||
            !fs::is_directory(status) || !actual.insert(name).second)
            fail("attempt workspace contains an unsafe entry: " + name);
    }
    if (ec || actual != expected)
        fail("attempt workspace contains missing or unexpected entries");
}

fs::path checked_output_root(const TrainingInputBundle& inputs,
                             const fs::path& supplied, bool must_exist) {
    validate_workspace_layout(inputs, must_exist);
    const fs::path expected = inputs.workspace_root / "output" / "run";
    std::error_code ec;
    const fs::path actual =
        LogicalAbsoluteFilesystemPath(supplied, ec).lexically_normal();
    if (ec || !same_path(expected, actual))
        fail("output directory differs from the frozen worker workspace");
    if (!must_exist) return expected;

    const fs::path parent = safe_root(inputs.workspace_root / "output",
                                      "training output parent");
    const fs::path root = safe_root(expected, "training output root");
    if (!within(inputs.workspace_root, parent) ||
        !within(inputs.workspace_root, root))
        fail("training output path escapes the attempt workspace");
    std::error_code iter_error;
    fs::directory_iterator it(NativeFilesystemPath(parent), iter_error), end;
    if (iter_error || it == end || it->path().filename().u8string() != "run")
        fail("training output parent contains an unexpected entry");
    it.increment(iter_error);
    if (iter_error || it != end)
        fail("training output parent contains an unexpected entry");
    return root;
}

}  // namespace

TrainingInputBundle StageTrainingInputs(
    const TrainConfig& source_config, const std::string& preset,
    const fs::path& resume_checkpoint, const fs::path& attempt_root,
    std::uint64_t disk_budget_bytes) {
    if (disk_budget_bytes == 0) fail("disk budget must be positive");
    validate_finite_config(source_config);
    TrainConfig preset_check;
    if (!train_apply_preset(preset_check, preset)) fail("unknown training preset");
    if (!source_config.save_full_checkpoint)
        fail("portable training requires save_full_checkpoint=1 for resumable outputs");
    if (!source_config.disable_viewer)
        fail("portable training requires disable_viewer=1 to avoid a network listener");
    if (!source_config.resume.empty())
        fail("resume path must be supplied as the explicit checkpoint argument");

    const fs::path workspace = safe_root(attempt_root, "attempt root");
    if (!fs::is_empty(NativeFilesystemPath(workspace)))
        fail("attempt root must be empty");
    const fs::path source_data = resolve_data_root(source_config.data);
    if (within(source_data, workspace) || within(workspace, source_data))
        fail("attempt root and dataset root must not overlap");

    TrainConfig config = source_config;
    normalize_dataset_paths(config, source_data);
    if (config.data_format != "" && config.data_format != "colmap" &&
        config.data_format != "nerfstudio" && config.data_format != "metashape")
        fail("unsupported training data format");

    fs::path source_checkpoint;
    std::string checkpoint_leaf;
    if (!resume_checkpoint.empty()) {
        source_checkpoint = safe_root(resume_checkpoint, "resume checkpoint");
        checkpoint_leaf = source_checkpoint.filename().u8string();
        (void)parse_checkpoint_name(checkpoint_leaf);
        const ckpt::ResolvedCheckpoint resolved =
            ckpt::resolve_checkpoint(source_checkpoint);
        if (!same_path(LogicalFilesystemPath(resolved.ckpt_dir), source_checkpoint))
            fail("resume input must name one exact checkpoint directory");
        ckpt::check_resumable(source_checkpoint);

        const fs::path source_config_path = source_checkpoint / kConfigFile;
        const std::string old_preset = config_preset(
            source_config_path, source_checkpoint, kConfigFile);
        TrainConfig checkpoint_config = parse_config_bytes(
            read_bounded_file(source_config_path, source_checkpoint, kConfigFile),
            old_preset);
        TrainConfig old_preset_check;
        if (!train_apply_preset(old_preset_check, old_preset))
            fail("resume checkpoint uses an unknown preset");
        (void)resolve_checkpoint_data_root(
            checkpoint_config.data, source_checkpoint, source_data);
        normalize_dataset_paths(checkpoint_config, source_data);
#define SS_TRAIN_DATA_FIELD(member) \
        if (!(checkpoint_config.member == config.member)) \
            fail("resume checkpoint dataset configuration differs at " #member);
        SS_DATASET_PARSE_FIELDS(SS_TRAIN_DATA_FIELD)
#undef SS_TRAIN_DATA_FIELD

        const std::set<std::string> required = {"config.json", "splat.ply", "state.tar"};
        std::set<std::string> present;
        std::error_code ec;
        for (fs::directory_iterator it(NativeFilesystemPath(source_checkpoint), ec), end;
             it != end && !ec; it.increment(ec)) {
            const fs::file_status status = path_status(it->path());
            if (fs::is_symlink(status) || is_reparse_point(it->path()) ||
                !fs::is_regular_file(status))
                fail("resume checkpoint contains a linked or non-file artifact");
            present.insert(it->path().filename().u8string());
        }
        if (ec || present != required)
            fail("resume checkpoint has missing or unexpected files");
    }
    std::string source_format;
    std::optional<ColmapModel> source_colmap;
    (void)validate_dataset(config, source_data, &source_format, &source_colmap);
    const ParsedDataset all_frames =
        parse_all_dataset_frames(config, source_data, source_format);
    const std::vector<fs::path> sources = dataset_dependencies(
        config, source_data, source_format, source_colmap, all_frames);
    const std::size_t reserved_files = source_checkpoint.empty() ? 2 : 5;
    if (sources.size() > kMaxFiles - reserved_files)
        fail("training package has too many required files");

    config.data = "input/dataset";
    config.resume = resume_checkpoint.empty()
        ? std::string() : "input/resume/" + checkpoint_leaf;
    config.output_dir_prefix = "output";
    config.output_dir_name = "run";
    std::uint64_t remaining = disk_budget_bytes;
    const fs::path input_root = workspace / kInputDir;
    std::vector<TransferFile> files;
    std::error_code ec;
    try {
        if (!fs::create_directory(NativeFilesystemPath(input_root), ec) || ec)
            fail("cannot create isolated input directory");
        const fs::path dataset_root = input_root / kDatasetDir;
        if (!fs::create_directory(NativeFilesystemPath(dataset_root), ec) || ec)
            fail("cannot create staged dataset directory");
        for (const fs::path& source : sources) {
            const std::string relative = source.lexically_relative(source_data).generic_u8string();
            files.push_back(copy_file(
                source_data, source, input_root, "dataset/" + relative, remaining));
        }

        if (!source_checkpoint.empty()) {
            const std::string relative = "resume/" + checkpoint_leaf;
            for (const char* leaf : {"splat.ply", "state.tar"})
                files.push_back(copy_file(
                    source_checkpoint, source_checkpoint / leaf, input_root,
                    relative + "/" + leaf, remaining));
        }
        files.push_back(write_record(input_root, kConfigFile,
                                     config_json(config, preset), remaining));
        files.push_back(write_record(
            input_root, kTrainingFile,
            training_metadata_json(
                resume_checkpoint.empty() ? TrainingMode::Fresh : TrainingMode::Resume,
                preset, config.resume), remaining));

        if (!source_checkpoint.empty()) {
            const std::string relative = "resume/" + checkpoint_leaf + "/config.json";
            files.push_back(write_record(input_root, relative,
                                         config_json(config, preset), remaining));
        }
        files = validate_manifest(std::move(files), disk_budget_bytes);
        TrainConfig validation_config = config;
        validation_config.data = (workspace / "input" / "dataset").u8string();
        validate_dataset(validation_config, dataset_root);
        if (!source_checkpoint.empty())
            validate_checkpoint(input_root, "resume/" + checkpoint_leaf,
                                config, preset);
        validate_exact_tree(input_root, files, disk_budget_bytes, false, false);
        const std::string identity = manifest_identity(files);
        return input_bundle(workspace, preset, config, files, identity);
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(NativeFilesystemPath(input_root), ignored);
        throw;
    }
}

TrainingInputBundle VerifyTrainingInputs(
    const fs::path& input_root, const std::vector<TransferFile>& approved_files,
    const std::string& expected_identity_sha256,
    std::uint64_t disk_budget_bytes) {
    if (disk_budget_bytes == 0) fail("disk budget must be positive");
    const fs::path root = safe_root(input_root, "input directory");
    if (root.filename().u8string() != kInputDir)
        fail("input directory must be named 'input'");
    const fs::path workspace = safe_root(root.parent_path(), "attempt workspace");
    const std::vector<TransferFile> files = validate_manifest(
        approved_files, disk_budget_bytes);
    validate_exact_tree(root, files, disk_budget_bytes, true, false);
    const std::string identity = manifest_identity(files);
    if (!valid_digest(expected_identity_sha256) ||
        identity != expected_identity_sha256)
        fail("input manifest identity mismatch");

    const std::string metadata_bytes = read_bounded_file(
        root / kTrainingFile, root, kTrainingFile);
    const JsonValue metadata = json_parse(metadata_bytes);
    if (!metadata.is_object()) fail("training metadata is malformed");
    const JsonValue* schema = metadata.find("schema");
    if (!schema || schema->type != JsonValue::Type::Number || schema->num != 1)
        fail("training metadata schema is unsupported");
    const std::string preset = json_string_field(metadata, "preset", true);
    TrainConfig preset_check;
    if (!train_apply_preset(preset_check, preset)) fail("unknown training preset");
    const std::string mode_text = json_string_field(metadata, "mode", true);
    const std::string resume_text = json_string_field(
        metadata, "resume_checkpoint", true);
    const std::set<std::string> metadata_keys = {
        "schema", "preset", "mode", "resume_checkpoint"};
    std::set<std::string> actual_metadata_keys;
    for (const auto& item : metadata.obj)
        if (!actual_metadata_keys.insert(item.first).second)
            fail("training metadata contains duplicate fields");
    if (actual_metadata_keys != metadata_keys)
        fail("training metadata contains missing or unexpected fields");

    TrainConfig config = parse_config_bytes(
        read_bounded_file(root / kConfigFile, root, kConfigFile), preset);
    if (!config.save_full_checkpoint || !config.disable_viewer ||
        config.output_dir_prefix != "output" || config.output_dir_name != "run" ||
        config.data != "input/dataset" || config.resume != resume_text)
        fail("frozen config has invalid paths or remote-safety options");
    const TrainingMode mode = mode_text == "fresh" ? TrainingMode::Fresh
        : mode_text == "resume" ? TrainingMode::Resume
                                  : throw std::runtime_error("agent training: invalid mode");
    if ((mode == TrainingMode::Fresh) != resume_text.empty())
        fail("training mode and explicit resume checkpoint disagree");
    if (!resume_text.empty() && resume_text.rfind("input/resume/", 0) != 0)
        fail("resume checkpoint path is outside the staged input root");

    const fs::path dataset_root = root / kDatasetDir;
    TrainConfig validation_config = config;
    validation_config.data = dataset_root.u8string();
    validate_dataset(validation_config, dataset_root);
    if (mode == TrainingMode::Resume) {
        const std::string checkpoint = resume_text.substr(std::string("input/").size());
        (void)parse_checkpoint_name(fs::u8path(checkpoint).filename().u8string());
        validate_checkpoint(root, checkpoint, config, preset);
    }
    return input_bundle(workspace, preset, config, files, identity);
}

TrainingInvocationOptions MakeTrainingInvocation(
    const TrainingInputBundle& inputs) {
    check_bundle_integrity(inputs, std::numeric_limits<std::uint64_t>::max());
    TrainingInvocationOptions options;
    options.mode = inputs.mode;
    options.preset = inputs.preset;
    options.config = inputs.config;
    options.working_directory = inputs.workspace_root;
    options.output_directory = inputs.output_root;
    options.resume_checkpoint = inputs.resume_checkpoint;
    (void)checked_output_root(inputs, options.output_directory, false);
    return options;
}

TrainingOutputPackage TrainingOutputManifest(
    const TrainingInputBundle& inputs, const fs::path& output_root,
    const std::string& returned_checkpoint,
    std::uint64_t disk_budget_bytes) {
    if (disk_budget_bytes == 0) fail("disk budget must be positive");
    check_bundle_integrity(inputs, disk_budget_bytes);
    const fs::path output = checked_output_root(inputs, output_root, true);
    (void)parse_checkpoint_name(returned_checkpoint);
    const std::vector<TransferFile> files = inspect_output_tree(
        output, inputs.config, disk_budget_bytes, true);
    validate_output_semantics(inputs, output, files, returned_checkpoint);
    return {files, inputs.identity_sha256, returned_checkpoint};
}

TrainingOutputBundle VerifyTrainingOutputs(
    const TrainingInputBundle& inputs, const fs::path& result_root,
    const TrainingOutputPackage& approved_output,
    std::uint64_t disk_budget_bytes) {
    if (disk_budget_bytes == 0) fail("disk budget must be positive");
    check_bundle_integrity(inputs, disk_budget_bytes);
    const fs::path result = checked_output_root(inputs, result_root, true);
    if (approved_output.input_identity_sha256 != inputs.identity_sha256)
        fail("returned output is bound to a different input identity");
    (void)parse_checkpoint_name(approved_output.returned_checkpoint);
    const std::vector<TransferFile> files = validate_manifest(
        approved_output.files, disk_budget_bytes);
    validate_exact_tree(result, files, disk_budget_bytes, true, false);
    validate_output_semantics(inputs, result, files,
                              approved_output.returned_checkpoint);
    TrainingOutputBundle output;
    output.root = result;
    output.returned_checkpoint = approved_output.returned_checkpoint;
    output.checkpoint_dir = output.root / fs::u8path(output.returned_checkpoint);
    output.config_path = output.root / kConfigFile;
    output.splat_path = output.checkpoint_dir / "splat.ply";
    output.preset = inputs.preset;
    output.input_identity_sha256 = inputs.identity_sha256;
    output.worker_files = files;

    const fs::path transfer_stage = output.root / kTransferStage;
    if (!missing(path_status(transfer_stage))) {
        std::error_code ec;
        if (!fs::is_empty(NativeFilesystemPath(transfer_stage), ec) || ec ||
            !fs::remove(NativeFilesystemPath(transfer_stage), ec) || ec)
            fail("cannot remove empty output transfer staging directory");
    }
    TrainConfig local_config = inputs.config;
    local_config.data = inputs.dataset_root.u8string();
    write_bytes(output.config_path, config_json(local_config, inputs.preset),
                "locally bound run config");
    std::set<std::string> checkpoints;
    for (const TransferFile& file : files) {
        const std::size_t slash = file.path.find('/');
        if (slash != std::string::npos)
            checkpoints.insert(file.path.substr(0, slash));
    }
    for (const std::string& checkpoint : checkpoints)
        write_bytes(output.root / fs::u8path(checkpoint) / kConfigFile,
                    config_json(local_config, inputs.preset),
                    "locally bound checkpoint config");
    output.files = inspect_output_tree(output.root, inputs.config,
                                       disk_budget_bytes, false);
    output.resume_config = local_config;
    output.resume_config.resume =
        LogicalFilesystemPath(output.checkpoint_dir).u8string();
    output.resume_config.output_dir_prefix =
        LogicalFilesystemPath(output.root.parent_path()).u8string();
    output.resume_config.output_dir_name = output.root.filename().u8string();
    return output;
}

}  // namespace app::agent
