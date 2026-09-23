#include "app/AgentReconstructionJob.h"
#include "core/FilesystemPath.h"

#include "core/Sha256.h"
#include "data/DatasetParser.h"
#include "data/Json.h"
#include "data/Yaml.h"
#include "data/JsonWrite.h"
#include "sfm/Pipeline.h"
#include "sfm/core/Camera.h"
#include "sfm/core/FeatureWork.h"
#include "sfm/core/Mask.h"
#include "sfm/core/Model.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <atomic>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <system_error>
#include <utility>
#include <cstring>

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
using spirula::LogicalFilesystemPath;
using spirula::NativeFilesystemPath;
namespace {
namespace fs = std::filesystem;

constexpr std::size_t kMaxManifestFiles = 4096;
constexpr std::size_t kMaxPathBytes = 4096;
constexpr std::size_t kMaxRecordBytes = 16 * 1024 * 1024;
constexpr std::size_t kChunkBytes = 64 * 1024;
constexpr char kTransferStage[] = ".agent-transfer-v1";
constexpr char kPortableManifest[] = "manifest.json";
constexpr char kBundleRecord[] = "bundle.json";
constexpr char kPortableRequest[] = "request.json";

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("agent reconstruction: " + message);
}

std::string lower_ascii(std::string value) {
    for (char& c : value)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return value;
}

bool windows_reserved_name(const std::string& component) {
    const std::string stem = lower_ascii(component.substr(0, component.find('.')));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul")
        return true;
    return stem.size() == 4 &&
           (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) &&
           stem[3] >= '1' && stem[3] <= '9';
}

void safe_relative_path(const std::string& value) {
    if (value.empty() || value.size() > kMaxPathBytes ||
        sfm::feature_work::normalizeRelativePath(value) != value ||
        value.front() == '/' || value.back() == '/')
        fail("path is not normalized and relative");
    std::size_t begin = 0;
    while (begin < value.size()) {
        const std::size_t end = value.find('/', begin);
        const std::string component = value.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if (component.empty() || component == "." || component == ".." ||
            component.size() > 255 || component.back() == '.' ||
            component.back() == ' ' || windows_reserved_name(component) ||
            lower_ascii(component) == kTransferStage)
            fail("path contains an unsafe component: " + value);
        for (const unsigned char c : component)
            if (c < 0x20 || c == 0x7f || c == '\\' || c == ':' || c == '<' ||
                c == '>' || c == '"' || c == '|' || c == '?' || c == '*')
                fail("path contains an unsafe character: " + value);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
}

bool valid_digest(const std::string& digest) {
    if (digest.size() != 64) return false;
    for (char c : digest)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
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

fs::path safe_root(const fs::path& supplied, const char* what) {
    if (supplied.empty()) fail(std::string(what) + " is empty");
    std::error_code ec;
    const fs::path absolute = fs::absolute(supplied, ec);
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

fs::path checked_path(const fs::path& root, const std::string& relative,
                      bool create_parents = false,
                      std::vector<fs::path>* created_dirs = nullptr) {
    safe_relative_path(relative);
    fs::path current = root;
    std::size_t begin = 0;
    for (;;) {
        const std::size_t slash = relative.find('/', begin);
        const bool last = slash == std::string::npos;
        current /= fs::u8path(relative.substr(
            begin, last ? std::string::npos : slash - begin));
        fs::file_status status = path_status(current);
        if (missing(status) && !last && create_parents) {
            std::error_code ec;
            const bool made = fs::create_directory(NativeFilesystemPath(current), ec);
            if (made && created_dirs) created_dirs->push_back(current);
            if (ec && ec != std::errc::file_exists)
                fail("cannot create a bundle directory");
            status = path_status(current);
        }
        if (last) {
            if (missing(status)) fail("required file is missing: " + relative);
            if (!fs::is_regular_file(status) || fs::is_symlink(status) ||
                is_reparse_point(current))
                fail("path is not an unlinked regular file: " + relative);
            std::error_code ec;
            if (fs::hard_link_count(NativeFilesystemPath(current), ec) != 1 || ec)
                fail("hard-linked file is not allowed: " + relative);
            const fs::path resolved = canonical_path(current, ec);
            if (ec || !within(root, resolved))
                fail("file escapes its root: " + relative);
            return current;
        }
        if (missing(status) || !fs::is_directory(status) || fs::is_symlink(status) ||
            is_reparse_point(current))
            fail("required directory is missing or linked: " + relative);
        std::error_code ec;
        const fs::path resolved = canonical_path(current, ec);
        if (ec || !within(root, resolved))
            fail("directory escapes its root: " + relative);
        begin = slash + 1;
    }
}

std::uint64_t regular_file_size(const fs::path& path, const fs::path& root,
                                const std::string& relative) {
    const fs::file_status status = path_status(path);
    if (!fs::is_regular_file(status) || fs::is_symlink(status) ||
        is_reparse_point(path))
        fail("not a regular file: " + relative);
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

std::string hash_bytes(std::string_view bytes) {
    spirula::Sha256 hash;
    hash.update(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    return hash.hex();
}

std::string read_digest(const fs::path& path, std::uint64_t size,
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
        fail("file changed or could not be read: " + relative);
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

bool is_image_extension(const fs::path& path) {
    std::string extension = lower_ascii(path.extension().u8string());
    return extension == ".jpg" || extension == ".jpeg" || extension == ".png" ||
           extension == ".bmp" || extension == ".tga" || extension == ".ppm" ||
           extension == ".pgm" || extension == ".exr";
}

bool is_sidecar(const fs::path& path) {
    const std::string name = path.filename().u8string();
    return name.size() > 2 && name[0] == '.' && name[1] == '_';
}

std::vector<fs::path> regular_tree_files(const fs::path& supplied_root,
                                         const char* what) {
    const fs::path root = safe_root(supplied_root, what);
    std::vector<fs::path> files;
    std::error_code ec;
    fs::recursive_directory_iterator it(
        NativeFilesystemPath(root), fs::directory_options::none, ec), end;
    if (ec) fail(std::string("cannot enumerate ") + what);
    for (; it != end; it.increment(ec)) {
        if (ec) fail(std::string("cannot enumerate ") + what);
        const fs::path path = LogicalFilesystemPath(it->path());
        const fs::file_status status = path_status(path);
        if (fs::is_symlink(status) || is_reparse_point(path))
            fail(std::string(what) + " contains a linked path");
        const fs::path resolved = canonical_path(path, ec);
        if (ec || !within(root, resolved))
            fail(std::string(what) + " contains a path outside its root");
        if (fs::is_directory(status)) continue;
        if (!fs::is_regular_file(status))
            fail(std::string(what) + " contains a non-regular entry");
        files.push_back(path);
    }
    return files;
}

std::vector<fs::path> image_files_under(const std::vector<fs::path>& tree,
                                        const fs::path& root) {
    std::vector<fs::path> files;
    for (const fs::path& path : tree)
        if (within(root, path) && is_image_extension(path) && !is_sidecar(path))
            files.push_back(path);
    return files;
}

fs::path selected_image_root(const fs::path& supplied_root,
                             const std::vector<fs::path>& all_tree_files) {
    const fs::path root = safe_root(supplied_root, "image root");
    const fs::path nested = root / "images";
    const fs::file_status status = path_status(nested);
    if (missing(status)) return root;
    if (!fs::is_directory(status) || fs::is_symlink(status) ||
        is_reparse_point(nested))
        fail("nested images path is not a real directory");
    std::error_code ec;
    const fs::path canonical_nested = canonical_path(nested, ec);
    if (ec || !within(root, canonical_nested))
        fail("nested images path escapes its input root");
    for (const fs::path& path : all_tree_files)
        if (is_image_extension(path) && !is_sidecar(path) &&
            !within(canonical_nested, path))
            return root;
    return canonical_nested;
}

std::string relative_name(const fs::path& path, const fs::path& root) {
    const fs::path relative = path.lexically_relative(root);
    if (relative.empty()) fail("cannot make an input path relative");
    const std::string name = relative.generic_u8string();
    safe_relative_path(name);
    return name;
}

std::vector<fs::path> allowed_source_roots(const fs::path& image_root,
                                           const fs::path& mask_root,
                                           const fs::path& manifest_root) {
    std::vector<fs::path> roots{image_root};
    if (!mask_root.empty()) roots.push_back(mask_root);
    if (!manifest_root.empty()) roots.push_back(manifest_root);
    return roots;
}

std::pair<fs::path, std::string> source_under_allowed_root(
    const fs::path& supplied, const std::vector<fs::path>& roots,
    const char* what) {
    if (supplied.empty()) fail(std::string(what) + " path is empty");
    std::error_code ec;
    const fs::path absolute = fs::absolute(supplied, ec);
    if (ec || !absolute.is_absolute())
        fail(std::string(what) + " path cannot be resolved");
    const fs::path normalized = absolute.lexically_normal();
    for (const fs::path& root : roots) {
        const fs::path relative = normalized.lexically_relative(root);
        if (relative.empty() || relative.is_absolute()) continue;
        const std::string name = relative.generic_u8string();
        try {
            safe_relative_path(name);
        } catch (const std::exception&) {
            continue;
        }
        const fs::path checked = checked_path(root, name);
        return {checked, name};
    }
    fail(std::string(what) + " must be inside the selected image, mask, or manifest root");
}

struct SourceSpec {
    std::string path;
    fs::path source;
    std::string bytes;
    bool bundle_metadata = false;
};

std::vector<TransferFile> validate_manifest(std::vector<TransferFile> files,
                                            std::uint64_t budget) {
    if (files.empty() || files.size() > kMaxManifestFiles)
        fail("manifest has an invalid file count");
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
    }
    std::sort(files.begin(), files.end(), [](const TransferFile& a, const TransferFile& b) {
        return a.path < b.path;
    });
    std::set<std::string> paths;
    for (const TransferFile& file : files) {
        const std::string folded = lower_ascii(file.path);
        if (!paths.insert(folded).second) fail("manifest paths collide");
        for (std::size_t slash = folded.find('/'); slash != std::string::npos;
             slash = folded.find('/', slash + 1))
            if (paths.count(folded.substr(0, slash)))
                fail("manifest file is also a parent directory");
    }
    return files;
}

void validate_source_specs(const std::vector<SourceSpec>& files,
                           std::uint64_t budget) {
    if (files.empty() || files.size() + 2 > kMaxManifestFiles)
        fail("input tree has an invalid file count");
    std::set<std::string> paths;
    std::uint64_t total = 0;
    for (const SourceSpec& file : files) {
        safe_relative_path(file.path);
        const std::string folded = lower_ascii(file.path);
        if (!paths.insert(folded).second) fail("input paths collide");
        for (std::size_t slash = folded.find('/'); slash != std::string::npos;
             slash = folded.find('/', slash + 1))
            if (paths.count(folded.substr(0, slash)))
                fail("input file is also a parent directory");
        if (!file.bundle_metadata &&
            (folded == kBundleRecord || folded == kPortableManifest ||
             folded == kPortableRequest))
            fail("input path collides with bundle metadata");
        if (file.source.empty()) {
            if (file.bytes.size() > std::numeric_limits<std::uint64_t>::max() - total)
                fail("input size overflows");
            total += file.bytes.size();
        } else {
            const std::uint64_t size = fs::file_size(NativeFilesystemPath(file.source));
            if (size > std::numeric_limits<std::uint64_t>::max() - total)
                fail("input size overflows");
            total += size;
        }
        if (total > budget) fail("bundle exceeds the disk quota");
    }
}

void create_parent_dirs(const fs::path& root, const std::string& relative,
                        std::vector<fs::path>& created_dirs) {
    safe_relative_path(relative);
    std::size_t begin = 0;
    fs::path current = root;
    for (;;) {
        const std::size_t slash = relative.find('/', begin);
        if (slash == std::string::npos) return;
        current /= fs::u8path(relative.substr(begin, slash - begin));
        fs::file_status status = path_status(current);
        if (missing(status)) {
            std::error_code ec;
            const bool made = fs::create_directory(NativeFilesystemPath(current), ec);
            if (made) created_dirs.push_back(current);
            if (ec && ec != std::errc::file_exists)
                fail("cannot create a staged input directory");
            status = path_status(current);
        }
        if (!fs::is_directory(status) || fs::is_symlink(status) ||
            is_reparse_point(current))
            fail("staging parent is missing or linked");
        std::error_code ec;
        const fs::path resolved = canonical_path(current, ec);
        if (ec || !within(root, resolved)) fail("staging parent escapes the attempt root");
        begin = slash + 1;
    }
}

std::atomic<std::uint64_t> g_stage_serial{0};

TransferFile stage_file(const fs::path& root, const SourceSpec& file,
                        const std::vector<fs::path>& allowed_roots,
                        std::uint64_t& remaining_budget,
                        std::vector<fs::path>& created_files,
                        std::vector<fs::path>& created_dirs) {
    const fs::path source_path = file.source.empty() ? fs::path{} :
        source_under_allowed_root(file.source, allowed_roots, "selected input").first;
    const std::uint64_t expected_size = source_path.empty()
        ? static_cast<std::uint64_t>(file.bytes.size())
        : regular_file_size(source_path, canonical_path(source_path.parent_path()),
                            source_path.filename().u8string());
    if (expected_size > remaining_budget) fail("bundle exceeds the disk quota");
    create_parent_dirs(root, file.path, created_dirs);
    const fs::path destination = root / fs::u8path(file.path);
    if (!missing(path_status(destination))) fail("staging destination already exists");
    const fs::path temporary = destination.parent_path() /
        fs::u8path(".recon-job-" +
                   std::to_string(g_stage_serial.fetch_add(1)) + ".part");
    if (!missing(path_status(temporary))) fail("staging temporary path already exists");
    created_files.push_back(temporary);
    std::ofstream output(NativeFilesystemPath(temporary),
                         std::ios::binary | std::ios::trunc);
    if (!output) fail("cannot create a staged file: " + file.path);
    spirula::Sha256 hash;
    std::uint64_t total = 0;
    if (file.source.empty()) {
        std::size_t offset = 0;
        while (offset < file.bytes.size()) {
            const std::size_t count = std::min(kChunkBytes, file.bytes.size() - offset);
            output.write(file.bytes.data() + offset, static_cast<std::streamsize>(count));
            if (!output) fail("cannot write staged metadata: " + file.path);
            hash.update(reinterpret_cast<const std::uint8_t*>(file.bytes.data() + offset), count);
            total += count;
            offset += count;
        }
    } else {
        const std::uint64_t expected = expected_size;
        std::ifstream input(NativeFilesystemPath(source_path), std::ios::binary);
        std::array<char, kChunkBytes> buffer{};
        while (total < expected) {
            const std::size_t count = static_cast<std::size_t>(
                std::min<std::uint64_t>(buffer.size(), expected - total));
            input.read(buffer.data(), static_cast<std::streamsize>(count));
            if (input.gcount() != static_cast<std::streamsize>(count))
                fail("selected source changed or could not be read: " + file.path);
            output.write(buffer.data(), static_cast<std::streamsize>(count));
            if (!output) fail("cannot stage selected source: " + file.path);
            hash.update(reinterpret_cast<const std::uint8_t*>(buffer.data()), count);
            total += count;
        }
        char extra = 0;
        input.read(&extra, 1);
        if (input.gcount() != 0 || input.bad())
            fail("selected source changed while staging: " + file.path);
    }
    if (total != expected_size) fail("staged file size differs: " + file.path);
    output.flush();
    if (!output) fail("cannot flush staged file: " + file.path);
    output.close();
    if (output.fail()) fail("cannot close staged file: " + file.path);
    const std::string digest = hash.hex();
    const std::string temporary_name = temporary.lexically_relative(root).generic_u8string();
    if (regular_file_size(temporary, root, temporary_name) != total)
        fail("staged file size differs: " + file.path);
    if (!missing(path_status(destination))) fail("staging destination appeared");
    std::error_code ec;
    fs::rename(NativeFilesystemPath(temporary),
               NativeFilesystemPath(destination), ec);
    if (ec) fail("cannot atomically publish staged file: " + file.path);
    remaining_budget -= total;
    created_files.push_back(destination);
    return {file.path, total, digest};
}

void cleanup(const std::vector<fs::path>& files,
             const std::vector<fs::path>& directories) noexcept {
    std::error_code ec;
    for (auto it = files.rbegin(); it != files.rend(); ++it) {
        fs::remove(NativeFilesystemPath(*it), ec);
        ec.clear();
    }
    for (auto it = directories.rbegin(); it != directories.rend(); ++it) {
        fs::remove(NativeFilesystemPath(*it), ec);
        ec.clear();
    }
}

void require_empty_root(const fs::path& root) {
    std::error_code ec;
    fs::directory_iterator it(NativeFilesystemPath(root), ec), end;
    if (ec) fail("cannot inspect the attempt root");
    if (it != end) fail("attempt root is not empty");
}

void verify_tree(const fs::path& root, const std::vector<TransferFile>& files) {
    std::set<std::string> expected_files;
    std::set<std::string> expected_dirs;
    for (const TransferFile& file : files) {
        expected_files.insert(file.path);
        for (std::size_t slash = file.path.find('/'); slash != std::string::npos;
             slash = file.path.find('/', slash + 1))
            expected_dirs.insert(file.path.substr(0, slash));
    }
    std::set<std::string> seen;
    std::error_code ec;
    fs::recursive_directory_iterator it(
        NativeFilesystemPath(root), fs::directory_options::none, ec), end;
    if (ec) fail("cannot enumerate the staged tree");
    for (; it != end; it.increment(ec)) {
        if (ec) fail("cannot enumerate the staged tree");
        const fs::path path = LogicalFilesystemPath(it->path());
        const std::string relative = path.lexically_relative(root).generic_u8string();
        const fs::file_status status = path_status(path);
        if (fs::is_symlink(status) || is_reparse_point(path))
            fail("staged tree contains a linked path: " + relative);
        if (relative == kTransferStage) {
            if (!fs::is_directory(status))
                fail("staged transfer namespace is not a directory");
            std::error_code canonical_error;
            const fs::path resolved = canonical_path(path, canonical_error);
            if (canonical_error || !within(root, resolved))
                fail("staged transfer namespace escapes its root");
            continue;
        }
        safe_relative_path(relative);
        if (fs::is_directory(status)) {
            if (!expected_dirs.count(relative))
                fail("staged tree contains an unauthorized directory: " + relative);
            std::error_code canonical_error;
            const fs::path resolved = canonical_path(path, canonical_error);
            if (canonical_error || !within(root, resolved))
                fail("staged directory escapes its root: " + relative);
            continue;
        }
        if (!fs::is_regular_file(status) || !expected_files.count(relative) ||
            !seen.insert(relative).second)
            fail("staged tree contains an unauthorized file: " + relative);
        regular_file_size(path, root, relative);
    }
    if (seen.size() != expected_files.size())
        fail("staged tree is missing an approved file");
}

std::vector<TransferFile> verify_files(const fs::path& root,
                                       const std::vector<TransferFile>& supplied,
                                       std::uint64_t budget) {
    const fs::path canonical_root = safe_root(root, "artifact root");
    const std::vector<TransferFile> files = validate_manifest(supplied, budget);
    verify_tree(canonical_root, files);
    for (const TransferFile& file : files) {
        const fs::path path = checked_path(canonical_root, file.path);
        const std::uint64_t size = regular_file_size(path, canonical_root, file.path);
        if (size != file.size || read_digest(path, size, file.path) != file.sha256)
            fail("artifact integrity mismatch: " + file.path);
    }
    return files;
}

std::string bundle_identity(const std::string& build_id,
                            const std::string& source_manifest_digest,
                            const std::string& portable_manifest_digest,
                            const std::vector<TransferFile>& files) {
    std::string material = "spirula-reconstruction-input-v2\n" + build_id + "\n" +
                           source_manifest_digest + "\n" + portable_manifest_digest + "\n";
    for (const TransferFile& file : files) {
        if (file.path == kBundleRecord) continue;
        material += file.path;
        material.push_back('\0');
        material += std::to_string(file.size);
        material.push_back('\0');
        material += file.sha256;
        material.push_back('\n');
    }
    return hash_bytes(material);
}

std::string bundle_record(const std::string& build_id,
                          const std::string& source_manifest_digest,
                          const std::string& portable_manifest_digest,
                          const std::string& identity,
                          const std::string& metric_positions) {
    JsonWriter json;
    json.object().field("version", 2).field("build_id", build_id)
        .field("source_manifest_sha256", source_manifest_digest)
        .field("portable_manifest_sha256", portable_manifest_digest)
        .field("identity_sha256", identity)
        .field("metric_positions", metric_positions).end();
    return json.str();
}

std::string string_field(const JsonValue& value, const char* name) {
    const JsonValue* field = value.find(name);
    if (!field || field->type != JsonValue::Type::String)
        fail(std::string("bundle record has an invalid ") + name);
    return field->str;
}

struct BundleRecord {
    std::string build_id;
    std::string source_manifest_sha256;
    std::string portable_manifest_sha256;
    std::string identity_sha256;
    std::string metric_positions;
};

BundleRecord parse_bundle_record(const std::string& bytes) {
    JsonValue value;
    try {
        value = json_parse(bytes);
    } catch (const std::exception& e) {
        fail(std::string("bundle record is malformed: ") + e.what());
    }
    const std::set<std::string> expected = {
        "version", "build_id", "source_manifest_sha256",
        "portable_manifest_sha256", "identity_sha256", "metric_positions"};
    if (!value.is_object() || value.obj.size() != expected.size())
        fail("bundle record has an unexpected shape");
    std::set<std::string> actual;
    for (const auto& entry : value.obj)
        if (!actual.insert(entry.first).second || !expected.count(entry.first))
            fail("bundle record has an unexpected field");
    const JsonValue* version = value.find("version");
    if (!version || version->type != JsonValue::Type::Number || version->num != 2)
        fail("bundle record version is unsupported");
    BundleRecord record;
    record.build_id = string_field(value, "build_id");
    record.source_manifest_sha256 = string_field(value, "source_manifest_sha256");
    record.portable_manifest_sha256 = string_field(value, "portable_manifest_sha256");
    record.identity_sha256 = string_field(value, "identity_sha256");
    record.metric_positions = string_field(value, "metric_positions");
    if (record.build_id.empty() || !valid_digest(record.portable_manifest_sha256) ||
        !valid_digest(record.identity_sha256) ||
        (!record.source_manifest_sha256.empty() &&
         !valid_digest(record.source_manifest_sha256)) ||
        (!record.metric_positions.empty() &&
         record.metric_positions != "references/metric_positions.txt"))
        fail("bundle record identity is invalid");
    return record;
}

bool has_prefix(const std::string& value, const char* prefix) {
    const std::string p(prefix);
    return value.size() > p.size() && value.compare(0, p.size(), p) == 0;
}

bool indexed_telemetry_path(const std::string& path) {
    if (!has_prefix(path, "telemetry/") || path.size() != 22 ||
        path.compare(path.size() - 4, 4, ".bin") != 0)
        return false;
    for (std::size_t i = 10; i < 18; ++i)
        if (path[i] < '0' || path[i] > '9') return false;
    return true;
}

JsonValue json_string(const std::string& value) {
    JsonValue result;
    result.type = JsonValue::Type::String;
    result.str = value;
    return result;
}

template <class T>
JsonValue config_json_value(const T& value) {
    JsonValue result;
    if constexpr (std::is_same_v<T, std::string>) {
        result.type = JsonValue::Type::String;
        result.str = value;
    } else if constexpr (std::is_same_v<T, bool>) {
        result.type = JsonValue::Type::Bool;
        result.b = value;
    } else if constexpr (std::is_integral_v<T> || std::is_floating_point_v<T>) {
        result.type = JsonValue::Type::Number;
        result.num = static_cast<double>(value);
        if (!std::isfinite(result.num)) fail("request contains a non-finite option");
    } else {
        static_assert(std::is_same_v<T, void>, "unsupported SfmConfig field type");
    }
    return result;
}

template <class T>
void restore_config_value(const JsonValue& value, T& target, const char* name) {
    if constexpr (std::is_same_v<T, std::string>) {
        if (value.type != JsonValue::Type::String || value.str.size() > kMaxRecordBytes ||
            value.str.find('\0') != std::string::npos)
            fail(std::string("request has an invalid option: ") + name);
        target = value.str;
    } else if constexpr (std::is_same_v<T, bool>) {
        if (value.type != JsonValue::Type::Bool)
            fail(std::string("request has an invalid option: ") + name);
        target = value.b;
    } else if constexpr (std::is_integral_v<T>) {
        if (value.type != JsonValue::Type::Number || !std::isfinite(value.num) ||
            std::trunc(value.num) != value.num ||
            value.num < static_cast<double>(std::numeric_limits<T>::lowest()) ||
            value.num > static_cast<double>(std::numeric_limits<T>::max()))
            fail(std::string("request has an invalid option: ") + name);
        target = static_cast<T>(value.num);
    } else if constexpr (std::is_floating_point_v<T>) {
        if (value.type != JsonValue::Type::Number || !std::isfinite(value.num) ||
            value.num < -static_cast<double>(std::numeric_limits<T>::max()) ||
            value.num > static_cast<double>(std::numeric_limits<T>::max()))
            fail(std::string("request has an invalid option: ") + name);
        target = static_cast<T>(value.num);
    } else {
        static_assert(std::is_same_v<T, void>, "unsupported SfmConfig field type");
    }
}

JsonValue portable_config_value(
    const char* name, const std::string& value, const sfm::AutoRequest& request,
    bool has_masks, const std::map<std::string, std::string>& telemetry_paths,
    const std::string& metric_positions) {
    if (std::strcmp(name, "mask_dir") == 0)
        return json_string(has_masks ? "masks" : "");
    if (std::strcmp(name, "metric_positions") == 0) {
        if (value.empty()) return json_string("");
        if (metric_positions.empty())
            fail("metric-position input was not staged");
        return json_string(metric_positions);
    }
    if (std::strcmp(name, "telemetry") == 0) {
        if (value.empty()) return json_string("");
        const auto path = telemetry_paths.find(value);
        if (path == telemetry_paths.end())
            fail("telemetry option was not staged");
        return json_string(path->second);
    }
    if (std::strcmp(name, "device_request") == 0) return json_string("");
    const bool model_path = std::strcmp(name, "aliked.model") == 0 ||
        std::strcmp(name, "lightglue.model") == 0 ||
        std::strcmp(name, "loma.detector_model") == 0 ||
        std::strcmp(name, "loma.descriptor_model") == 0 ||
        std::strcmp(name, "loma_match.model") == 0;
    if (model_path && !value.empty()) {
        for (const unsigned char c : value)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-'))
                fail("external learned-model paths are not portable");
    }
    (void)request;
    return json_string(value);
}

template <class T>
JsonValue portable_config_value(
    const char* name, const T& value, const sfm::AutoRequest&,
    bool, const std::map<std::string, std::string>&, const std::string&) {
    (void)name;
    return config_json_value(value);
}

bool camera_mode_is_pinned(const sfm::AutoRequest& request) {
    return request.cfg.camera_mode_pinned ||
           request.in.explicit_flags.count("camera-mode") != 0;
}

std::string portable_request_bytes(
    const sfm::AutoRequest& request, bool has_masks,
    const std::map<std::string, std::string>& telemetry_paths,
    const std::string& metric_positions) {
    if (request.wants_help || !request.progress_dir.empty())
        fail("help and progress-directory requests are not portable reconstructions");
    if (request.cfg.device_request_set || !request.cfg.device_request.empty() ||
        request.cfg.device >= 0)
        fail("an explicit source-device selection cannot be applied to a worker");
    if (request.cfg.sift.profile || !request.cfg.sift.spv_path.empty())
        fail("SIFT profiling and external shader paths are not portable auto options");
    if (!request.cfg.verify || !request.cfg.merge_ba || request.cfg.in_place ||
        request.cfg.check || !request.cfg.image_dir.empty() ||
        !request.cfg.feature_dir.empty() || !request.cfg.resume.empty())
        fail("non-auto map or merge options are not portable reconstruction inputs");

    JsonValue fields;
    fields.type = JsonValue::Type::Object;
    std::set<std::string> written;
    using namespace sfm;
#define APP_AGENT_WRITE_CONFIG(member, flag, cmds, tier, group, lo, hi, choices, key) \
    do { \
        if (((cmds) & CMD_AUTO) && written.insert(#member).second) \
            fields.obj.emplace_back( \
                #member, portable_config_value(#member, request.cfg.member, request, \
                    has_masks, telemetry_paths, metric_positions)); \
    } while (false);
    SFM_CONFIG_FIELDS(APP_AGENT_WRITE_CONFIG)
#undef APP_AGENT_WRITE_CONFIG

    JsonValue result;
    result.type = JsonValue::Type::Object;
    JsonValue version;
    version.type = JsonValue::Type::Number;
    version.num = 1;
    result.obj.emplace_back("version", std::move(version));
    result.obj.emplace_back("config", std::move(fields));
    result.obj.emplace_back("camera_mode_pinned",
                            config_json_value(camera_mode_is_pinned(request)));
    result.obj.emplace_back("mask_dir_explicit",
                            config_json_value(request.in.mask_dir_explicit));

    JsonValue explicit_flags;
    explicit_flags.type = JsonValue::Type::Array;
    for (const std::string& flag : request.in.explicit_flags) {
        if (flag.size() > 256 || flag.find('\0') != std::string::npos)
            fail("request has an invalid explicit-option name");
        explicit_flags.arr.push_back(json_string(flag));
    }
    result.obj.emplace_back("explicit_flags", std::move(explicit_flags));

    JsonValue preset_changes;
    preset_changes.type = JsonValue::Type::Array;
    for (const sfm::PresetChange& change : request.in.preset_changes) {
        JsonValue item;
        item.type = JsonValue::Type::Object;
        item.obj.emplace_back("flag", json_string(change.flag));
        item.obj.emplace_back("from", json_string(change.from));
        item.obj.emplace_back("to", json_string(change.to));
        preset_changes.arr.push_back(std::move(item));
    }
    result.obj.emplace_back("preset_changes", std::move(preset_changes));
    const std::string bytes = json_write(result);
    if (bytes.size() > kMaxRecordBytes) fail("portable request is too large");
    return bytes;
}

std::set<std::string> portable_config_field_names() {
    std::set<std::string> result;
    using namespace sfm;
#define APP_AGENT_NAME_CONFIG(member, flag, cmds, tier, group, lo, hi, choices, key) \
    do { if ((cmds) & CMD_AUTO) result.insert(#member); } while (false);
    SFM_CONFIG_FIELDS(APP_AGENT_NAME_CONFIG)
#undef APP_AGENT_NAME_CONFIG
    return result;
}

void restore_portable_config(const JsonValue& fields, sfm::SfmConfig& config) {
    const std::set<std::string> expected = portable_config_field_names();
    if (!fields.is_object() || fields.obj.size() != expected.size())
        fail("portable request config has an unexpected shape");
    std::set<std::string> actual;
    for (const auto& [name, value] : fields.obj) {
        (void)value;
        if (!actual.insert(name).second || !expected.count(name))
            fail("portable request config has an unknown or duplicate option");
    }
    using namespace sfm;
#define APP_AGENT_RESTORE_CONFIG(member, flag, cmds, tier, group, lo, hi, choices, key) \
    do { \
        if ((cmds) & CMD_AUTO) { \
            const JsonValue* value = fields.find(#member); \
            if (!value) fail("portable request config omits an option: " #member); \
            restore_config_value(*value, config.member, #member); \
        } \
    } while (false);
    SFM_CONFIG_FIELDS(APP_AGENT_RESTORE_CONFIG)
#undef APP_AGENT_RESTORE_CONFIG
}
std::string config_number_argument(double value) {
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), value,
        std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (result.ec != std::errc{}) fail("cannot encode a numeric config option");
    return std::string(buffer.data(), result.ptr);
}

void append_portable_option(std::vector<std::string>& args, const JsonValue& value,
                            const char* flag, const char* member,
                            const ReconstructionInputBundle& inputs) {
    const std::string field(member);
    if (field == "camera_model" || field == "focal" || field == "distortion" ||
        field == "device_request")
        return;
    std::string text;
    if (value.type == JsonValue::Type::Bool) {
        args.push_back(std::string(value.b ? "--" : "--no-") + flag);
        return;
    }
    if (value.type == JsonValue::Type::String) {
        text = value.str;
        if (field == "mask_dir") {
            if (text == "masks") text = inputs.mask_root.u8string();
            else if (!text.empty()) fail("portable request contains an unrooted mask path");
        } else if (field == "metric_positions") {
            if (text == "references/metric_positions.txt")
                text = inputs.metric_positions_path.u8string();
            else if (!text.empty())
                fail("portable request contains an unrooted metric-position path");
        } else if (field == "telemetry" && !text.empty()) {
            if (!indexed_telemetry_path(text))
                fail("portable request contains an unrooted telemetry path");
            text = checked_path(inputs.root, text).u8string();
        }
    } else if (value.type == JsonValue::Type::Number && std::isfinite(value.num)) {
        text = config_number_argument(value.num);
    } else {
        fail(std::string("portable request option has an invalid type: ") + member);
    }
    args.push_back("--" + std::string(flag));
    args.push_back(std::move(text));
}

void append_portable_config_args(const JsonValue& fields,
                                 const ReconstructionInputBundle& inputs,
                                 std::vector<std::string>& args) {
    std::set<std::string> appended;
    using namespace sfm;
#define APP_AGENT_APPEND_CONFIG(member, flag, cmds, tier, group, lo, hi, choices, key) \
    do { \
        if (((cmds) & CMD_AUTO) && appended.insert(#member).second) { \
            const JsonValue* value = fields.find(#member); \
            if (!value) fail("portable request omits config option: " #member); \
            append_portable_option(args, *value, flag, #member, inputs); \
        } \
    } while (false);
    SFM_CONFIG_FIELDS(APP_AGENT_APPEND_CONFIG)
#undef APP_AGENT_APPEND_CONFIG
}


std::string request_string_field(const JsonValue& value, const char* name) {
    const JsonValue* field = value.find(name);
    if (!field || field->type != JsonValue::Type::String ||
        field->str.size() > kMaxRecordBytes || field->str.find('\0') != std::string::npos)
        fail(std::string("portable request has an invalid ") + name);
    return field->str;
}

bool request_bool_field(const JsonValue& value, const char* name) {
    const JsonValue* field = value.find(name);
    if (!field || field->type != JsonValue::Type::Bool)
        fail(std::string("portable request has an invalid ") + name);
    return field->b;
}

bool portable_model_identifier(const std::string& value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

JsonValue parse_portable_request(const std::string& bytes,
                                const sfm::Manifest& manifest,
                                const BundleRecord& record,
                                const std::vector<TransferFile>& files) {
    JsonValue value;
    try {
        value = json_parse(bytes);
    } catch (const std::exception& e) {
        fail(std::string("portable request is malformed: ") + e.what());
    }
    const std::set<std::string> expected = {
        "version", "config", "camera_mode_pinned", "mask_dir_explicit",
        "explicit_flags", "preset_changes"};
    if (!value.is_object() || value.obj.size() != expected.size())
        fail("portable request has an unexpected shape");
    std::set<std::string> actual;
    for (const auto& [name, field] : value.obj) {
        (void)field;
        if (!actual.insert(name).second || !expected.count(name))
            fail("portable request has an unknown or duplicate field");
    }
    const JsonValue* version = value.find("version");
    if (!version || version->type != JsonValue::Type::Number || version->num != 1)
        fail("portable request version is unsupported");
    const JsonValue* config_value = value.find("config");
    if (!config_value) fail("portable request omits its config");
    sfm::SfmConfig config;
    restore_portable_config(*config_value, config);
    const bool camera_mode_pinned = request_bool_field(value, "camera_mode_pinned");
    (void)request_bool_field(value, "mask_dir_explicit");
    if (camera_mode_pinned != !manifest.camera_mode.empty())
        fail("portable request camera mode disagrees with its manifest");

    const auto config_string = [&](const char* name) {
        const JsonValue* field = config_value->find(name);
        if (!field || field->type != JsonValue::Type::String)
            fail(std::string("portable request has an invalid config field: ") + name);
        return field->str;
    };
    const std::string mask_dir = config_string("mask_dir");
    if (mask_dir != (manifest.mask_dir.empty() ? "" : "masks"))
        fail("portable request mask path disagrees with its manifest");
    const std::string metric_positions = config_string("metric_positions");
    if (metric_positions != record.metric_positions)
        fail("portable request metric path disagrees with its bundle");
    const std::string telemetry = config_string("telemetry");
    if (!telemetry.empty()) {
        const auto file = std::find_if(files.begin(), files.end(), [&](const TransferFile& item) {
            return item.path == telemetry;
        });
        if (!indexed_telemetry_path(telemetry) || file == files.end())
            fail("portable request refers to an unstaged telemetry input");
    }
    if (!config_string("device_request").empty())
        fail("portable request contains a source-device selection");
    const std::string* models[] = {
        &config.aliked.model, &config.lightglue.model, &config.loma.detector_model,
        &config.loma.descriptor_model, &config.loma_match.model};
    for (const std::string* model : models)
        if (!model->empty() && !portable_model_identifier(*model))
            fail("portable request contains an external learned-model path");

    const JsonValue* flags = value.find("explicit_flags");
    if (!flags || !flags->is_array() || flags->arr.size() > 1024)
        fail("portable request has invalid explicit options");
    std::set<std::string> unique_flags;
    for (const JsonValue& flag : flags->arr) {
        if (flag.type != JsonValue::Type::String || flag.str.size() > 256 ||
            flag.str.find('\0') != std::string::npos ||
            !unique_flags.insert(flag.str).second)
            fail("portable request has an invalid explicit option");
    }
    const JsonValue* changes = value.find("preset_changes");
    if (!changes || !changes->is_array() || changes->arr.size() > 1024)
        fail("portable request has invalid preset changes");
    for (const JsonValue& item : changes->arr) {
        if (!item.is_object() || item.obj.size() != 3)
            fail("portable request has an invalid preset change");
        std::set<std::string> keys;
        for (const auto& [name, field] : item.obj) {
            (void)field;
            if (!keys.insert(name).second ||
                (name != "flag" && name != "from" && name != "to"))
                fail("portable request has an invalid preset change");
        }
        for (const char* name : {"flag", "from", "to"})
            (void)request_string_field(item, name);
    }
    return value;
}

JsonValue read_portable_request(const fs::path& root,
                                const sfm::Manifest& manifest,
                                const BundleRecord& record,
                                const std::vector<TransferFile>& files) {
    const auto entry = std::find_if(files.begin(), files.end(), [](const TransferFile& file) {
        return file.path == kPortableRequest;
    });
    if (entry == files.end()) fail("approved input list omits the portable request");
    return parse_portable_request(
        read_bounded_file(root / kPortableRequest, root, kPortableRequest),
        manifest, record, files);
}
void validate_portable_manifest(const sfm::Manifest& manifest,
                                const BundleRecord& record,
                                const std::vector<TransferFile>& files) {
    if (manifest.image_dir != "images" ||
        (!manifest.mask_dir.empty() && manifest.mask_dir != "masks"))
        fail("portable manifest contains an external image or mask path");
    std::set<std::string> paths;
    std::size_t images = 0;
    bool has_metric_positions = false;
    for (const TransferFile& file : files) {
        paths.insert(file.path);
        if (has_prefix(file.path, "images/")) ++images;
        else if (has_prefix(file.path, "masks/")) {}
        else if (has_prefix(file.path, "telemetry/")) {
            if (!indexed_telemetry_path(file.path))
                fail("portable bundle contains a non-canonical telemetry path");
        } else if (file.path == "references/metric_positions.txt") {
            has_metric_positions = true;
        } else if (file.path != kPortableManifest && file.path != kPortableRequest &&
                   file.path != kBundleRecord) {
            fail("portable bundle contains an unexpected path: " + file.path);
        }
    }
    if (images < 2 || has_metric_positions != !record.metric_positions.empty())
        fail("portable bundle is missing required reconstruction inputs");
    for (const sfm::ManifestCapture& capture : manifest.captures)
        if (!indexed_telemetry_path(capture.telemetry) || !paths.count(capture.telemetry))
            fail("portable manifest refers to an unstaged telemetry file");
    if (manifest.mask_dir.empty())
        for (const std::string& path : paths)
            if (has_prefix(path, "masks/"))
                fail("portable bundle has masks disabled but contains mask files");
}

ReconstructionInputBundle input_result(const fs::path& root,
                                       const BundleRecord& record,
                                       const std::vector<TransferFile>& files) {
    ReconstructionInputBundle result;
    result.root = root;
    result.manifest_path = root / kPortableManifest;
    result.request_path = root / kPortableRequest;
    result.image_root = root / "images";
    result.mask_root = root / "masks";
    result.build_id = record.build_id;
    result.source_manifest_sha256 = record.source_manifest_sha256;
    result.identity_sha256 = record.identity_sha256;
    result.files = files;
    if (!record.metric_positions.empty())
        result.metric_positions_path = root / record.metric_positions;
    try {
        result.manifest = sfm::manifest_read(
            NativeFilesystemPath(result.manifest_path).u8string());
    } catch (const std::exception& e) {
        fail(std::string("portable manifest cannot be loaded: ") + e.what());
    }
    if (result.manifest.mask_dir.empty()) result.mask_root.clear();
    validate_portable_manifest(result.manifest, record, files);
    (void)read_portable_request(root, result.manifest, record, files);
    return result;
}

void require_empty_attempt(const fs::path& root) {
    const fs::path safe = safe_root(root, "attempt root");
    require_empty_root(safe);
}

std::string mode_name(sfm::CameraMode mode) {
    switch (mode) {
        case sfm::CameraMode::Folder: return "folder";
        case sfm::CameraMode::Image: return "image";
        default: return "single";
    }
}

sfm::Manifest effective_manifest(const sfm::AutoRequest& request,
                                 const fs::path& mask_root,
                                 const std::map<std::string, std::string>& telemetry_paths) {
    const sfm::SfmConfig& config = request.cfg;
    sfm::Manifest manifest;
    manifest.image_dir = "images";
    if (!mask_root.empty()) manifest.mask_dir = "masks";
    manifest.has_mask_flipped = true;
    manifest.mask_flipped = config.flip_mask;
    if (camera_mode_is_pinned(request))
        manifest.camera_mode = mode_name(config.camera.mode);
    manifest.image_gamut = config.image_gamut;
    manifest.image_linear = config.image_is_linear ? 1 : 0;

    sfm::ManifestCamera base;
    base.model = sfm::camInfo(config.camera.model).cli_name;
    base.focal = config.focal;
    base.distortion = config.camera.extra;
    manifest.cameras.push_back(std::move(base));
    for (const sfm::CameraOverride& setting : config.camera.overrides) {
        sfm::ManifestCamera camera;
        camera.prefix = setting.prefix;
        if (setting.has_model) camera.model = sfm::camInfo(setting.model).cli_name;
        if (setting.has_focal) camera.focal = setting.focal;
        if (setting.has_extra) camera.distortion = setting.extra;
        manifest.cameras.push_back(std::move(camera));
    }
    manifest.rigs = config.rigs;
    for (std::size_t i = 0; i < config.telemetry_inputs.size(); ++i) {
        const sfm::TelemetryInput& input = config.telemetry_inputs[i];
        const auto it = telemetry_paths.find(input.path);
        if (it == telemetry_paths.end()) fail("telemetry input was not staged");
        sfm::ManifestCapture capture;
        capture.prefix = input.prefix;
        capture.telemetry = it->second;
        capture.fps = input.fps;
        capture.time_offset = input.time_offset;
        capture.source_export_mapping = input.source_export_mapping;
        capture.timing_estimate = input.timing_estimate;
        capture.synchronization_decision = input.synchronization_decision;
        manifest.captures.push_back(std::move(capture));
    }
    return manifest;
}

bool is_finite_camera(const sfm::Camera& camera) {
    const double values[] = {camera.fx, camera.fy, camera.cx, camera.cy, camera.k1,
        camera.k2, camera.p1, camera.p2, camera.k3, camera.k4, camera.k5,
        camera.k6, camera.sx1, camera.sy1};
    return camera.width > 0 && camera.height > 0 &&
           std::all_of(std::begin(values), std::end(values),
                       [](double value) { return std::isfinite(value); });
}

bool numeric_model_name(const std::string& name, std::uint64_t& index) {
    if (name.empty() || (name.size() > 1 && name.front() == '0')) return false;
    if (!std::all_of(name.begin(), name.end(), [](char c) {
            return c >= '0' && c <= '9';
        })) return false;
    try {
        std::size_t consumed = 0;
        index = std::stoull(name, &consumed);
        return consumed == name.size() && std::to_string(index) == name;
    } catch (const std::exception&) {
        return false;
    }
}

std::vector<TransferFile> inspect_sparse_tree(const fs::path& supplied_root,
                                               std::uint64_t budget) {
    const fs::path root = safe_root(supplied_root, "sparse output root");
    std::map<std::uint64_t, std::set<std::string>> models;
    std::vector<TransferFile> files;
    std::error_code ec;
    fs::recursive_directory_iterator it(
        NativeFilesystemPath(root), fs::directory_options::none, ec), end;
    if (ec) fail("cannot enumerate sparse output");
    for (; it != end; it.increment(ec)) {
        if (ec) fail("cannot enumerate sparse output");
        const fs::path path = LogicalFilesystemPath(it->path());
        const std::string relative = path.lexically_relative(root).generic_u8string();
        safe_relative_path(relative);
        const fs::file_status status = path_status(path);
        if (fs::is_symlink(status) || is_reparse_point(path))
            fail("sparse output contains a linked path: " + relative);
        const fs::path resolved = canonical_path(path, ec);
        if (ec || !within(root, resolved))
            fail("sparse output path escapes its root: " + relative);
        const std::size_t slash = relative.find('/');
        const std::string dirname = relative.substr(0, slash);
        std::uint64_t model_index = 0;
        if (!numeric_model_name(dirname, model_index))
            fail("sparse output has a non-model directory: " + relative);
        if (!models.count(model_index) && models.size() >= kMaxManifestFiles / 3)
            fail("sparse output has too many model directories");
        if (fs::is_directory(status)) {
            if (slash != std::string::npos)
                fail("sparse output has a nested directory: " + relative);
            models[model_index];
            continue;
        }
        if (!fs::is_regular_file(status) || slash == std::string::npos ||
            relative.find('/', slash + 1) != std::string::npos)
            fail("sparse output has a non-regular or nested file: " + relative);
        const std::string filename = relative.substr(slash + 1);
        if (filename != "cameras.bin" && filename != "images.bin" &&
            filename != "points3D.bin" && filename != "gauge.txt" &&
            filename != "rigs.txt")
            fail("sparse output contains an unapproved artifact: " + relative);
        models[model_index].insert(filename);
        const std::uint64_t size = regular_file_size(path, root, relative);
        if (files.size() >= kMaxManifestFiles)
            fail("sparse output has too many files");
        files.push_back({relative, size, read_digest(path, size, relative)});
    }
    if (models.empty() || models.size() > kMaxManifestFiles)
        fail("sparse output has no models or too many models");
    std::uint64_t expected_index = 0;
    for (const auto& [index, names] : models) {
        if (index != expected_index++) fail("sparse model indices are not contiguous");
        for (const char* required : {"cameras.bin", "images.bin", "points3D.bin"})
            if (!names.count(required))
                fail("sparse model is missing " + std::string(required));
    }
    return validate_manifest(std::move(files), budget);
}

std::map<std::uint64_t, std::set<std::string>> validate_sparse_layout(
    const std::vector<TransferFile>& files) {
    std::map<std::uint64_t, std::set<std::string>> models;
    for (const TransferFile& file : files) {
        const std::size_t slash = file.path.find('/');
        if (slash == std::string::npos ||
            file.path.find('/', slash + 1) != std::string::npos)
            fail("output manifest path is not a sparse model file");
        std::uint64_t index = 0;
        const std::string dirname = file.path.substr(0, slash);
        if (!numeric_model_name(dirname, index))
            fail("output manifest has an invalid sparse model index");
        const std::string name = file.path.substr(slash + 1);
        if (name != "cameras.bin" && name != "images.bin" &&
            name != "points3D.bin" && name != "gauge.txt" && name != "rigs.txt")
            fail("output manifest has an unapproved model artifact");
        if (!models[index].insert(name).second)
            fail("output manifest repeats a model artifact");
    }
    if (models.empty()) fail("output manifest contains no sparse models");
    std::uint64_t next = 0;
    for (const auto& [index, names] : models) {
        if (index != next++) fail("sparse model indices are not contiguous");
        for (const char* required : {"cameras.bin", "images.bin", "points3D.bin"})
            if (!names.count(required))
                fail("sparse model is incomplete");
    }
    return models;
}

class BinaryCursor {
public:
    BinaryCursor(const fs::path& path, std::uint64_t size, std::string name)
        : input_(NativeFilesystemPath(path), std::ios::binary),
          remaining_(size), name_(std::move(name)) {
        if (!input_) fail("cannot open model file: " + name_);
    }

    template <typename T>
    T read() {
        T value{};
        if (remaining_ < sizeof(T)) fail("truncated sparse model file: " + name_);
        input_.read(reinterpret_cast<char*>(&value), sizeof(T));
        if (!input_) fail("truncated sparse model file: " + name_);
        remaining_ -= sizeof(T);
        return value;
    }

    void skip(std::uint64_t bytes) {
        if (bytes > remaining_ || bytes > static_cast<std::uint64_t>(
                                         std::numeric_limits<std::streamoff>::max()))
            fail("truncated sparse model file: " + name_);
        input_.seekg(static_cast<std::streamoff>(bytes), std::ios::cur);
        if (!input_) fail("truncated sparse model file: " + name_);
        remaining_ -= bytes;
    }

    std::string cstring() {
        std::string value;
        while (remaining_ && value.size() <= kMaxPathBytes) {
            const char c = read<char>();
            if (!c) return value;
            value.push_back(c);
        }
        fail("invalid or oversized image name in sparse model: " + name_);
    }

    std::uint64_t remaining() const { return remaining_; }
    void finish() {
        if (remaining_ != 0) fail("sparse model file has trailing data: " + name_);
    }

private:
    std::ifstream input_;
    std::uint64_t remaining_;
    std::string name_;
};

void preflight_model(const fs::path& model_root,
                     const std::string& model_relative,
                     const fs::path& input_root,
                     const std::set<std::string>& staged_images) {
    const std::string cameras_path = model_relative + "/cameras.bin";
    const std::string images_path = model_relative + "/images.bin";
    const std::string points_path = model_relative + "/points3D.bin";
    const fs::path cameras_file = checked_path(model_root, cameras_path);
    const fs::path images_file = checked_path(model_root, images_path);
    const fs::path points_file = checked_path(model_root, points_path);
    const std::uint64_t camera_size = regular_file_size(cameras_file, model_root, cameras_path);
    const std::uint64_t image_size = regular_file_size(images_file, model_root, images_path);
    const std::uint64_t points_size = regular_file_size(points_file, model_root, points_path);

    {
        BinaryCursor input(cameras_file, camera_size, cameras_path);
        const std::uint64_t count = input.read<std::uint64_t>();
        if (count == 0 || count > input.remaining() / 40)
            fail("camera count exceeds model file size");
        std::set<std::uint32_t> ids;
        for (std::uint64_t i = 0; i < count; ++i) {
            const std::uint32_t id = input.read<std::uint32_t>();
            const std::int32_t model_id = input.read<std::int32_t>();
            const std::uint64_t width = input.read<std::uint64_t>();
            const std::uint64_t height = input.read<std::uint64_t>();
            if (!ids.insert(id).second || width == 0 || height == 0 ||
                width > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
                height > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
                fail("sparse model has an invalid camera record");
            sfm::CamModel model;
            try {
                model = sfm::camFromColmapId(model_id);
            } catch (const std::exception&) {
                fail("sparse model uses an unsupported camera model");
            }
            for (int k = 0; k < sfm::camColmapParams(model); ++k)
                if (!std::isfinite(input.read<double>()))
                    fail("sparse model has a non-finite camera parameter");
        }
        input.finish();
    }
    {
        BinaryCursor input(images_file, image_size, images_path);
        const std::uint64_t count = input.read<std::uint64_t>();
        if (count < 2 || count > input.remaining() / 73)
            fail("image count is invalid or exceeds model file size");
        std::set<std::uint32_t> ids;
        std::set<std::string> names;
        for (std::uint64_t i = 0; i < count; ++i) {
            const std::uint32_t id = input.read<std::uint32_t>();
            if (!ids.insert(id).second) fail("sparse model repeats an image id");
            for (int k = 0; k < 7; ++k)
                if (!std::isfinite(input.read<double>()))
                    fail("sparse model has a non-finite image pose");
            input.read<std::uint32_t>();  // camera id; checked by the loaders
            const std::string name = input.cstring();
            safe_relative_path(name);
            if (!names.insert(name).second || !staged_images.count(name))
                fail("sparse model image is not in the frozen input bundle: " + name);
            const std::uint64_t points = input.read<std::uint64_t>();
            if (points > std::numeric_limits<std::uint32_t>::max() ||
                points > input.remaining() / 24)
                fail("image point count exceeds model limits");
            for (std::uint64_t k = 0; k < points; ++k) {
                if (!std::isfinite(input.read<double>()) ||
                    !std::isfinite(input.read<double>()))
                    fail("sparse model has a non-finite image point");
                input.read<std::uint64_t>();
            }
        }
        input.finish();
    }
    {
        BinaryCursor input(points_file, points_size, points_path);
        const std::uint64_t count = input.read<std::uint64_t>();
        if (count == 0) fail("sparse model contains no 3D points");
        if (count > input.remaining() / 51) fail("point count exceeds model file size");
        std::set<std::uint64_t> ids;
        for (std::uint64_t i = 0; i < count; ++i) {
            const std::uint64_t id = input.read<std::uint64_t>();
            if (!ids.insert(id).second) fail("sparse model repeats a point id");
            for (int k = 0; k < 3; ++k)
                if (!std::isfinite(input.read<double>()))
                    fail("sparse model has a non-finite point coordinate");
            input.skip(3);  // RGB
            if (!std::isfinite(input.read<double>()))
                fail("sparse model has a non-finite reprojection error");
            const std::uint64_t track = input.read<std::uint64_t>();
            if (track < 2 || track > input.remaining() / 8)
                fail("sparse model has an invalid point track");
            input.skip(track * 8);
        }
        input.finish();
    }

    // Require both production loaders to accept a useful COLMAP model.
    DatasetParserConfig parser;
    parser.recon_dir =
        (model_root / fs::u8path(model_relative)).string();
    parser.image_dir = "images";
    parser.mask_dir = "masks";
    parser.require_image_files = true;
    parser.exif_orientation = "none";
    const ParsedDataset loaded =
        parse_colmap_dataset(input_root.string(), parser);
    if (loaded.num_cameras < 1 || loaded.points.xyz.size() < 3)
        fail("sparse model loads without a usable reconstruction");

    const sfm::Reconstruction model =
        sfm::Reconstruction::readBinary(
            (model_root / fs::u8path(model_relative)).string());
    if (model.cameras.empty() || model.numRegistered() < 2 || model.points3D.empty())
        fail("SfM model loader found no usable sparse reconstruction");
    std::map<std::uint64_t, std::set<std::pair<std::uint32_t, std::uint32_t>>>
        point_observations;
    for (const auto& [image_id, image] : model.images) {
        const auto camera = model.cameras.find(image.camera_id);
        if (!image.registered || image.id != image_id || camera == model.cameras.end() ||
            !is_finite_camera(camera->second) ||
            image.points2D.size() != image.point3D_ids.size())
            fail("sparse model image references an invalid camera or observation");
        for (double value : image.pose.R)
            if (!std::isfinite(value)) fail("sparse model has a non-finite image pose");
        if (!std::isfinite(image.pose.t.x) || !std::isfinite(image.pose.t.y) ||
            !std::isfinite(image.pose.t.z))
            fail("sparse model has a non-finite image pose");
        for (std::size_t point = 0; point < image.point3D_ids.size(); ++point) {
            const std::uint64_t point_id = image.point3D_ids[point];
            if (point_id == sfm::kInvalidPoint3D) continue;
            if (!model.points3D.count(point_id))
                fail("sparse model image references a missing 3D point");
            point_observations[point_id].emplace(
                image.id, static_cast<std::uint32_t>(point));
        }
    }
    for (const auto& [point_id, point] : model.points3D) {
        if (point.track.size() < 2) fail("sparse model has an incomplete 3D track");
        std::set<std::pair<std::uint32_t, std::uint32_t>> track;
        for (const sfm::TrackElement& observation : point.track) {
            const auto image = model.images.find(observation.image_id);
            if (image == model.images.end() || !image->second.registered ||
                observation.point2D_idx >= image->second.point3D_ids.size() ||
                image->second.point3D_ids[observation.point2D_idx] != point_id ||
                !track.emplace(observation.image_id, observation.point2D_idx).second)
                fail("sparse model contains an invalid 3D track reference");
        }
        if (point_observations[point_id] != track)
            fail("sparse model image observations disagree with 3D tracks");
    }
}

void validate_output_models(const fs::path& sparse_root,
                            const fs::path& input_root,
                            const std::vector<TransferFile>& output_files,
                            const std::vector<TransferFile>& input_files) {
    const auto models = validate_sparse_layout(output_files);
    std::set<std::string> image_names;
    for (const TransferFile& file : input_files)
        if (has_prefix(file.path, "images/")) image_names.insert(file.path.substr(7));
    const fs::path model_root = safe_root(sparse_root, "sparse output root");
    for (const auto& [index, names] : models) {
        (void)names;
        preflight_model(model_root, std::to_string(index), input_root, image_names);
    }
}

void verify_bundle_identity(const fs::path& root,
                            const std::vector<TransferFile>& files,
                            const std::string& expected_identity,
                            BundleRecord& record) {
    if (!valid_digest(expected_identity)) fail("expected bundle identity is malformed");
    const auto metadata = std::find_if(files.begin(), files.end(), [](const TransferFile& file) {
        return file.path == kBundleRecord;
    });
    const auto manifest = std::find_if(files.begin(), files.end(), [](const TransferFile& file) {
        return file.path == kPortableManifest;
    });
    const auto request = std::find_if(files.begin(), files.end(), [](const TransferFile& file) {
        return file.path == kPortableRequest;
    });
    if (metadata == files.end() || manifest == files.end() || request == files.end())
        fail("approved input list omits bundle metadata");
    record = parse_bundle_record(read_bounded_file(root / kBundleRecord, root, kBundleRecord));
    if (record.build_id != ReconstructionBuildIdentity())
        fail("worker build is incompatible with the staged reconstruction");
    if (record.identity_sha256 != expected_identity)
        fail("input bundle identity differs from the approved job");
    if (record.portable_manifest_sha256 != manifest->sha256)
        fail("portable manifest identity differs from the approved file list");
    if (bundle_identity(record.build_id, record.source_manifest_sha256,
                        record.portable_manifest_sha256, files) != record.identity_sha256)
        fail("input bundle identity digest is invalid");
}

}  // namespace

const std::string& ReconstructionBuildIdentity() {
#ifdef SS_VERSION
    static const std::string identity = SS_VERSION;
#else
    static const std::string identity = "dev";
#endif
    return identity;
}

ReconstructionInputBundle StageReconstructionInputs(
    const sfm::AutoRequest& request, const fs::path& source_manifest,
    const fs::path& attempt_root, std::uint64_t disk_budget_bytes) {
    if (!request.in.feature_plan.empty())
        fail("auto feature-plan jobs are not portable whole-reconstruction inputs");
    require_empty_attempt(attempt_root);
    const fs::path root = safe_root(attempt_root, "attempt root");

    fs::path source_image_root = request.in.image_dir.empty()
        ? fs::path("images") : fs::u8path(request.in.image_dir);
    const fs::path canonical_source_images = safe_root(source_image_root, "image root");
    const std::vector<fs::path> image_tree =
        regular_tree_files(canonical_source_images, "image root");
    const fs::path selected_images = selected_image_root(canonical_source_images, image_tree);
    std::vector<fs::path> images = image_files_under(image_tree, selected_images);
    if (images.size() < 2) fail("at least two source images are required");
    std::sort(images.begin(), images.end(), [&](const fs::path& a, const fs::path& b) {
        return relative_name(a, selected_images) < relative_name(b, selected_images);
    });

    fs::path selected_masks;
    if (!request.cfg.mask_dir.empty()) {
        selected_masks = safe_root(fs::u8path(request.cfg.mask_dir), "mask root");
    } else if (!request.in.mask_dir_explicit) {
        fs::path p = selected_images;
        const fs::path sibling = p.parent_path() / "masks";
        const fs::file_status status = path_status(sibling);
        if (!missing(status)) selected_masks = safe_root(sibling, "mask root");
    }


    fs::path manifest_root;
    std::string source_manifest_digest;
    if (!source_manifest.empty()) {
        const fs::path manifest_file = fs::absolute(source_manifest).lexically_normal();
        manifest_root = safe_root(manifest_file.parent_path(), "source manifest directory");
        const std::string leaf = manifest_file.filename().u8string();
        safe_relative_path(leaf);
        const fs::path safe_file = checked_path(manifest_root, leaf);
        const std::uint64_t size = regular_file_size(safe_file, manifest_root, leaf);
        if (size > kMaxRecordBytes) fail("source manifest is too large");
        source_manifest_digest = read_digest(safe_file, size, leaf);
        if (manifest_root == manifest_root.root_path())
            fail("source manifest directory is not a scoped project root");
        const sfm::Manifest original = sfm::manifest_read(
            NativeFilesystemPath(safe_file).u8string());
        for (const sfm::ManifestCapture& capture : original.captures) {
            const fs::path telemetry = fs::u8path(capture.telemetry);
            if (telemetry.is_absolute() || telemetry.has_root_name() ||
                telemetry.has_root_directory())
                fail("manifest telemetry references must be relative");
            safe_relative_path(telemetry.generic_u8string());
        }
    }

    const std::vector<fs::path> allowed_roots = allowed_source_roots(
        selected_images, selected_masks, manifest_root);
    std::vector<SourceSpec> source_files;
    source_files.reserve(images.size() + request.cfg.telemetry_inputs.size() + 3);
    for (const fs::path& image : images) {
        const std::string name = relative_name(image, selected_images);
        (void)regular_file_size(image, selected_images, name);
        source_files.push_back({"images/" + name, image, {}});
    }

    std::map<std::string, std::string> telemetry_paths;
    std::map<std::string, std::string> source_to_portable;
    for (const sfm::TelemetryInput& input : request.cfg.telemetry_inputs) {
        const auto mapped = source_to_portable.find(input.path);
        if (mapped != source_to_portable.end()) {
            telemetry_paths.emplace(input.path, mapped->second);
            continue;
        }
        const auto source = source_under_allowed_root(
            fs::u8path(input.path), allowed_roots, "telemetry input");
        std::string number = std::to_string(source_to_portable.size());
        if (number.size() < 8) number.insert(0, 8 - number.size(), '0');
        const std::string portable = "telemetry/" + number + ".bin";
        source_files.push_back({portable, source.first, {}});
        source_to_portable.emplace(input.path, portable);
        telemetry_paths.emplace(input.path, portable);
    }

    bool has_staged_masks = false;
    if (!selected_masks.empty()) {
        (void)regular_tree_files(selected_masks, "mask root");
        sfm::MaskIndex masks(NativeFilesystemPath(selected_masks).u8string());
        if (!masks.valid()) fail("mask root is not readable");
        std::set<std::string> staged_masks;
        for (const fs::path& image : images) {
            const std::string image_name = relative_name(image, selected_images);
            const std::string found = masks.find(image_name);
            if (found.empty()) continue;
            const auto source = source_under_allowed_root(
                LogicalFilesystemPath(fs::u8path(found)), {selected_masks}, "mask input");
            const std::string target = "masks/" + source.second;
            if (staged_masks.insert(target).second)
                source_files.push_back({target, source.first, {}});
                has_staged_masks = true;
        }
    }

    std::string metric_positions_target;
    if (!request.cfg.metric_positions.empty()) {
        const auto source = source_under_allowed_root(
            fs::u8path(request.cfg.metric_positions), allowed_roots,
            "metric-position reference");
        metric_positions_target = "references/metric_positions.txt";
        source_files.push_back({metric_positions_target, source.first, {}});
    }

    std::map<std::string, std::string> telemetry_source_targets;
    for (const auto& [source, target] : telemetry_paths)
        telemetry_source_targets.emplace(source, target);
    sfm::Manifest portable = effective_manifest(
        request, has_staged_masks ? selected_masks : fs::path(), telemetry_source_targets);
    const std::string portable_manifest_bytes = sfm::manifest_write(portable, true);
    if (portable_manifest_bytes.size() > kMaxRecordBytes)
        fail("portable manifest is too large");
    source_files.push_back({kPortableManifest, {}, portable_manifest_bytes, true});
    const std::string request_bytes = portable_request_bytes(
        request, has_staged_masks, telemetry_paths, metric_positions_target);
    source_files.push_back({kPortableRequest, {}, request_bytes, true});
    validate_source_specs(source_files, disk_budget_bytes);

    std::vector<fs::path> created_files;
    std::vector<fs::path> created_dirs;
    std::uint64_t remaining_budget = disk_budget_bytes;
    try {
        std::vector<TransferFile> files;
        files.reserve(source_files.size() + 1);
        for (const SourceSpec& source : source_files)
            files.push_back(stage_file(root, source, allowed_roots, remaining_budget,
                                       created_files, created_dirs));
        files = validate_manifest(std::move(files), disk_budget_bytes);
        const auto portable_file = std::find_if(files.begin(), files.end(), [](const TransferFile& file) {
            return file.path == kPortableManifest;
        });
        if (portable_file == files.end()) fail("portable manifest was not staged");
        const std::string portable_manifest_digest = portable_file->sha256;
        const std::string identity = bundle_identity(
            ReconstructionBuildIdentity(), source_manifest_digest,
            portable_manifest_digest, files);
        const std::string record_bytes = bundle_record(
            ReconstructionBuildIdentity(), source_manifest_digest,
            portable_manifest_digest, identity, metric_positions_target);
        files.push_back(stage_file(root, {kBundleRecord, {}, record_bytes, true},
                                   allowed_roots, remaining_budget,
                                   created_files, created_dirs));
        files = validate_manifest(std::move(files), disk_budget_bytes);
        BundleRecord record{ReconstructionBuildIdentity(), source_manifest_digest,
                            portable_manifest_digest, identity, metric_positions_target};
        verify_files(root, files, disk_budget_bytes);
        verify_bundle_identity(root, files, identity, record);
        ReconstructionInputBundle result = input_result(root, record, files);
        if (!result.metric_positions_path.empty())
            (void)checked_path(root, metric_positions_target);
        return result;
    } catch (...) {
        cleanup(created_files, created_dirs);
        throw;
    }
}

ReconstructionInputBundle VerifyReconstructionInputs(
    const fs::path& attempt_root, const std::vector<TransferFile>& approved_files,
    const std::string& expected_identity_sha256,
    std::uint64_t disk_budget_bytes) {
    const fs::path root = safe_root(attempt_root, "attempt root");
    const std::vector<TransferFile> files = verify_files(root, approved_files,
                                                        disk_budget_bytes);
    BundleRecord record;
    verify_bundle_identity(root, files, expected_identity_sha256, record);
    return input_result(root, record, files);
}

sfm::AutoRequest DecodeReconstructionRequest(
    const ReconstructionInputBundle& inputs, const fs::path& workspace,
    std::uint64_t disk_budget_bytes) {
    const ReconstructionInputBundle verified = VerifyReconstructionInputs(
        inputs.root, inputs.files, inputs.identity_sha256, disk_budget_bytes);
    const fs::path workspace_root = safe_root(workspace, "worker workspace");
    require_empty_root(workspace_root);
    if (within(verified.root, workspace_root) || within(workspace_root, verified.root))
        fail("worker workspace overlaps the immutable input tree");

    const auto manifest_file = std::find_if(
        verified.files.begin(), verified.files.end(), [](const TransferFile& file) {
            return file.path == kPortableManifest;
        });
    if (manifest_file == verified.files.end())
        fail("verified input omits its portable manifest");
    const BundleRecord record{
        verified.build_id, verified.source_manifest_sha256, manifest_file->sha256,
        verified.identity_sha256,
        verified.metric_positions_path.empty() ? "" : "references/metric_positions.txt"};
    const JsonValue portable = read_portable_request(
        verified.root, verified.manifest, record, verified.files);
    const JsonValue* config = portable.find("config");
    if (!config) fail("portable request omits its config");
    std::vector<std::string> args = {
        verified.image_root.u8string(), "-o", workspace_root.u8string(),
        "--manifest", verified.manifest_path.u8string()};
    append_portable_config_args(*config, verified, args);

    sfm::AutoRequest request;
    const std::string parse_error = sfm::parse_auto_args(args, request, false);
    if (!parse_error.empty())
        fail("typed worker options are rejected by the SfM config loader: " + parse_error);
    request.cfg.camera_mode_pinned = request_bool_field(portable, "camera_mode_pinned");
    request.cfg.device_request.clear();
    request.cfg.device_request_set = false;
    request.cfg.device_selector.clear();
    request.cfg.device = -1;

    request.in.explicit_flags.clear();
    const JsonValue* flags = portable.find("explicit_flags");
    for (const JsonValue& flag : flags->arr) request.in.explicit_flags.insert(flag.str);
    request.in.preset_changes.clear();
    const JsonValue* changes = portable.find("preset_changes");
    for (const JsonValue& item : changes->arr)
        request.in.preset_changes.push_back(
            {request_string_field(item, "flag"), request_string_field(item, "from"),
             request_string_field(item, "to")});
    request.in.mask_dir_explicit = request_bool_field(portable, "mask_dir_explicit");
    request.in.image_dir = verified.image_root.u8string();
    request.in.workspace = workspace_root.u8string();
    request.in.feature_plan.clear();
    request.progress_dir.clear();
    request.wants_help = false;
    const std::string finalize_error = request.cfg.finalize(sfm::CMD_AUTO);
    if (!finalize_error.empty())
        fail("portable worker config is invalid: " + finalize_error);
    return request;
}

std::vector<TransferFile> ReconstructionOutputManifest(
    const ReconstructionInputBundle& inputs, const fs::path& sparse_root,
    std::uint64_t disk_budget_bytes) {
    const ReconstructionInputBundle verified = VerifyReconstructionInputs(
        inputs.root, inputs.files, inputs.identity_sha256, disk_budget_bytes);
    if (verified.build_id != ReconstructionBuildIdentity())
        fail("output build is incompatible with the input bundle");
    const fs::path root = safe_root(sparse_root, "sparse output root");
    const std::vector<TransferFile> files = inspect_sparse_tree(root, disk_budget_bytes);
    verify_files(root, files, disk_budget_bytes);
    validate_output_models(root, verified.root, files, verified.files);
    return files;
}

void VerifyReconstructionOutputs(
    const ReconstructionInputBundle& inputs, const fs::path& sparse_root,
    const std::vector<TransferFile>& approved_files,
    std::uint64_t disk_budget_bytes) {
    const ReconstructionInputBundle verified = VerifyReconstructionInputs(
        inputs.root, inputs.files, inputs.identity_sha256, disk_budget_bytes);
    if (verified.build_id != ReconstructionBuildIdentity())
        fail("output build is incompatible with the input bundle");
    const fs::path root = safe_root(sparse_root, "sparse output root");
    const std::vector<TransferFile> files = verify_files(root, approved_files,
                                                        disk_budget_bytes);
    validate_output_models(root, verified.root, files, verified.files);
}

}  // namespace app::agent
