#include "app/AgentFeatureJob.h"
#include "core/FilesystemPath.h"

#include "core/Sha256.h"
#include "sfm/Pipeline.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <system_error>
#include <stdexcept>
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
using spirula::LogicalAbsoluteFilesystemPath;
using spirula::LogicalFilesystemPath;
using spirula::NativeFilesystemPath;
namespace {
namespace fs = std::filesystem;
namespace fw = sfm::feature_work;

constexpr std::size_t kMaxManifestFiles = 4096;
constexpr std::size_t kMaxPathBytes = 4096;
constexpr std::size_t kChunkBytes = 64 * 1024;
constexpr char kTransferStage[] = ".agent-transfer-v1";
constexpr char kOutputLeaseFile[] = ".spirula-output.lock";

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::string lower_ascii(std::string value) {
    for (char& c : value)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return value;
}

bool reserved_windows_name(const std::string& component) {
    const std::string stem = lower_ascii(component.substr(0, component.find('.')));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul")
        return true;
    return stem.size() == 4 &&
        (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) &&
        stem[3] >= '1' && stem[3] <= '9';
}

void safe_relative_path(const std::string& value) {
    if (value.empty() || value.size() > kMaxPathBytes || value.front() == '/' ||
        value.back() == '/' || fw::normalizeRelativePath(value) != value)
        fail("path is not normalized and relative");
    std::size_t begin = 0;
    while (begin < value.size()) {
        const std::size_t end = value.find('/', begin);
        const std::string component = value.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if (component.empty() || component == "." || component == ".." ||
            component.size() > 255 || component.back() == '.' ||
            component.back() == ' ' || reserved_windows_name(component) ||
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

#ifdef _WIN32
bool is_reparse_point(const fs::path& path) {
    const DWORD attributes =
        GetFileAttributesW(NativeFilesystemPath(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}
#else
bool is_reparse_point(const fs::path&) { return false; }
#endif

fs::file_status path_status(const fs::path& path) {
    std::error_code ec;
    const fs::file_status status =
        fs::symlink_status(NativeFilesystemPath(path), ec);
    if (!ec) return status;
    if (ec == std::errc::no_such_file_or_directory ||
        ec == std::errc::not_a_directory)
        return fs::file_status(fs::file_type::not_found);
    fail("cannot inspect filesystem path");
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
        fail(std::string(what) + " cannot be resolved to an absolute path");
    const fs::file_status status = path_status(absolute);
    if (!fs::is_directory(status) || fs::is_symlink(status) ||
        is_reparse_point(absolute))
        fail(std::string(what) + " is missing, not a directory, or linked");
    const fs::path canonical = LogicalFilesystemPath(
        fs::canonical(NativeFilesystemPath(absolute), ec));
    if (ec || !canonical.is_absolute())
        fail(std::string(what) + " cannot be resolved");
    return canonical;
}

fs::path checked_path(const fs::path& root, const std::string& relative,
                      bool create_parents, std::vector<fs::path>* created_dirs,
                      bool must_exist) {
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
            if (created_dirs) created_dirs->push_back(current);
            std::error_code ec;
            const bool made = fs::create_directory(NativeFilesystemPath(current), ec);
            if (!made && created_dirs) created_dirs->pop_back();
            if (ec && ec != std::errc::file_exists)
                fail("cannot create bundle directory for " + relative);
            status = path_status(current);
        }
        if (last) {
            if (missing(status)) {
                if (must_exist) fail("required file is missing: " + relative);
            } else {
                if (fs::is_symlink(status) || is_reparse_point(current))
                    fail("linked file is not allowed: " + relative);
                std::error_code ec;
                const fs::path resolved = LogicalFilesystemPath(
                    fs::canonical(NativeFilesystemPath(current), ec));
                if (ec || !within(root, resolved))
                    fail("path escapes its root: " + relative);
                if (!must_exist)
                    fail("bundle destination already exists: " + relative);
            }
            return current;
        }
        if (missing(status) || !fs::is_directory(status) || fs::is_symlink(status) ||
            is_reparse_point(current))
            fail("required directory is missing or linked: " + relative);
        std::error_code ec;
        const fs::path resolved = LogicalFilesystemPath(
            fs::canonical(NativeFilesystemPath(current), ec));
        if (ec || !within(root, resolved))
            fail("path escapes its root: " + relative);
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
    const fs::path resolved = LogicalFilesystemPath(
        fs::canonical(NativeFilesystemPath(path), ec));
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

std::string read_bounded_file(const fs::path& path, std::uint64_t max_bytes) {
    std::ifstream input(NativeFilesystemPath(path), std::ios::binary);
    if (!input) fail("cannot read record file");
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > max_bytes)
        fail("record file is too large");
    input.seekg(0, std::ios::beg);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (size && !input.read(bytes.data(), size))
        fail("cannot read record file");
    return bytes;
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

struct FileSpec {
    std::string path;
    std::string sha256;
    std::uint64_t size = 0;
    fs::path source;
    std::string bytes;
};

bool has_source(const FileSpec& file) { return !file.source.empty(); }

std::vector<TransferFile> manifest_for(const std::vector<FileSpec>& files,
                                       std::uint64_t quota) {
    if (files.size() > kMaxManifestFiles) fail("manifest has too many files");
    std::vector<TransferFile> manifest;
    manifest.reserve(files.size());
    for (const FileSpec& file : files)
        manifest.push_back({file.path, file.size, file.sha256});
    std::sort(manifest.begin(), manifest.end(), [](const TransferFile& a,
                                                   const TransferFile& b) {
        return a.path < b.path;
    });
    std::set<std::string> paths;
    std::uint64_t total = 0;
    for (const TransferFile& file : manifest) {
        safe_relative_path(file.path);
        if (file.sha256.size() != 64 ||
            !std::all_of(file.sha256.begin(), file.sha256.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }))
            fail("manifest has an invalid SHA-256 digest: " + file.path);
        if (file.size > static_cast<std::uint64_t>(
                            std::numeric_limits<std::streamoff>::max()) ||
            file.size > std::numeric_limits<std::uint64_t>::max() - total)
            fail("manifest size overflows: " + file.path);
        total += file.size;
        if (total > quota) fail("bundle exceeds the disk quota");
        const std::string folded = lower_ascii(file.path);
        if (!paths.insert(folded).second)
            fail("manifest paths collide: " + file.path);
        for (std::size_t slash = folded.find('/'); slash != std::string::npos;
             slash = folded.find('/', slash + 1))
            if (paths.count(folded.substr(0, slash)))
                fail("manifest file is also a parent directory: " + file.path);
    }
    return manifest;
}

fw::request portable_request(const fw::FeaturePlan& plan,
                             const fw::FeatureRequest& request) {
    fw::request result = request;
    result.plan_path = "plan.json";
    if (fw::requestDigest(result) != fw::requestDigest(request)) result = request;
    fw::validateRequest(plan, result);
    return result;
}

fs::path resolve_file(const fs::path& root, const std::string& relative) {
    const fs::path path = checked_path(root, relative, false, nullptr, true);
    regular_file_size(path, root, relative);
    return path;
}

struct SourceRecords {
    fw::FeaturePlan plan;
    fw::FeatureRequest request;
};

SourceRecords read_source_records(const fs::path& supplied_plan_file,
                                  const fs::path& supplied_request_file) {
    if (supplied_plan_file.empty() || supplied_request_file.empty())
        fail("plan and request files are required");
    std::error_code ec;
    const fs::path plan_file =
        LogicalAbsoluteFilesystemPath(supplied_plan_file, ec);
    if (ec) fail("plan file path cannot be resolved");
    const fs::path request_file =
        LogicalAbsoluteFilesystemPath(supplied_request_file, ec);
    if (ec) fail("request file path cannot be resolved");
    const fs::path request_root = safe_root(request_file.parent_path(), "request directory");
    const std::string request_name = request_file.filename().u8string();
    const fs::path safe_request = resolve_file(request_root, request_name);
    const fw::FeatureRequest request = fw::readRequest(
        read_bounded_file(safe_request, 64ull << 20));
    const fs::path linked_plan = resolve_file(request_root, request.plan_path);
    const fs::path plan_root = safe_root(plan_file.parent_path(), "plan directory");
    const fs::path safe_plan = resolve_file(plan_root, plan_file.filename().u8string());
    const fs::path linked_canonical =
        LogicalFilesystemPath(fs::canonical(NativeFilesystemPath(linked_plan), ec));
    if (ec) fail("request plan_path cannot be resolved");
    const fs::path supplied_canonical =
        LogicalFilesystemPath(fs::canonical(NativeFilesystemPath(safe_plan), ec));
    if (ec || !same_path(linked_canonical, supplied_canonical))
        fail("request plan_path does not identify the supplied plan file");
    const fw::FeaturePlan plan = fw::readPlan(
        read_bounded_file(safe_plan, 64ull << 20));
    fw::validateRequest(plan, request);
    return {plan, request};
}

SourceRecords read_output_source_records(const fs::path& supplied_request_file) {
    if (supplied_request_file.empty()) fail("request file is required");
    std::error_code ec;
    const fs::path request_file =
        LogicalAbsoluteFilesystemPath(supplied_request_file, ec);
    if (ec) fail("request file path cannot be resolved");
    const fs::path root = safe_root(request_file.parent_path(), "request directory");
    const fs::path safe_request =
        resolve_file(root, request_file.filename().u8string());
    const fw::FeatureRequest request =
        fw::readRequest(read_bounded_file(safe_request, 64ull << 20));
    const fs::path plan_file = resolve_file(root, request.plan_path);
    const fw::FeaturePlan plan =
        fw::readPlan(read_bounded_file(plan_file, 64ull << 20));
    fw::validateRequest(plan, request);
    return {plan, request};
}

std::string receipt_relative_path(std::uint32_t index) {
    std::string digits = std::to_string(index);
    if (digits.size() < 8) digits.insert(0, 8 - digits.size(), '0');
    return "receipts/" + digits + ".json";
}


std::vector<FileSpec> expected_files(const fw::FeaturePlan& plan,
                                     const fw::FeatureRequest& request,
                                     const fs::path& image_root,
                                     const fs::path& mask_root,
                                     bool include_sources) {
    const fw::request portable = portable_request(plan, request);
    const std::string plan_bytes = fw::writePlan(plan);
    const std::string request_bytes = fw::writeRequest(portable);
    std::vector<FileSpec> files;
    files.push_back({portable.plan_path, hash_bytes(plan_bytes), plan_bytes.size(), {},
                     plan_bytes});
    files.push_back({"request.json", hash_bytes(request_bytes), request_bytes.size(), {},
                     request_bytes});

    for (const std::uint32_t index : request.image_indices) {
        if (index >= plan.images.size() || plan.images[index].global_index != index)
            fail("request image index is outside the plan");
        const fw::PlanImage& image = plan.images[index];
        const std::string image_path = "images/" + image.logical_name;
        safe_relative_path(image_path);
        FileSpec source{image_path, image.source_digest};
        if (include_sources) {
            const fs::path path = resolve_file(image_root, image.logical_name);
            source.source = path;
            source.size = regular_file_size(path, image_root, image.logical_name);
        }
        files.push_back(std::move(source));
        if (!image.mask_path.empty()) {
            if (include_sources && mask_root.empty())
                fail("selected masks require a mask root");
            const std::string mask_path = "masks/" + image.mask_path;
            safe_relative_path(mask_path);
            FileSpec mask{mask_path, image.mask_digest};
            if (include_sources) {
                const fs::path path = resolve_file(mask_root, image.mask_path);
                mask.source = path;
                mask.size = regular_file_size(path, mask_root, image.mask_path);
            }
            files.push_back(std::move(mask));
        }
    }
    return files;
}

void add_size(std::uint64_t& total, std::uint64_t size, std::uint64_t quota) {
    if (size > std::numeric_limits<std::uint64_t>::max() - total ||
        (total += size) > quota)
        fail("bundle exceeds the disk quota");
}

void write_bytes(std::ofstream& output, const std::string& bytes,
                 spirula::Sha256& hash) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t count = std::min(kChunkBytes, bytes.size() - offset);
        output.write(bytes.data() + offset, static_cast<std::streamsize>(count));
        if (!output) fail("cannot write staged metadata");
        hash.update(reinterpret_cast<const std::uint8_t*>(bytes.data() + offset), count);
        offset += count;
    }
}

void write_source(std::ofstream& output, const FileSpec& file,
                  spirula::Sha256& hash) {
    std::ifstream input(NativeFilesystemPath(file.source), std::ios::binary);
    if (!input) fail("cannot open selected source: " + file.path);
    std::array<char, kChunkBytes> buffer{};
    std::uint64_t total = 0;
    while (total < file.size) {
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), file.size - total));
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

void write_atomic(const fs::path& root, const FileSpec& file,
                  std::vector<fs::path>& created_dirs,
                  std::vector<fs::path>& created_files) {
    const fs::path destination = checked_path(
        root, file.path, true, &created_dirs, false);
    static std::atomic<std::uint64_t> serial{0};
    const auto stamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path temporary = destination.parent_path() /
        fs::u8path(".feature-job-" + std::to_string(stamp) + "-" +
                   std::to_string(serial.fetch_add(1)) + ".part");
    if (!missing(path_status(temporary)))
        fail("temporary staging path already exists");
    created_files.push_back(temporary);
    std::ofstream output(NativeFilesystemPath(temporary),
                         std::ios::binary | std::ios::trunc);
    if (!output) fail("cannot create temporary file for " + file.path);
    spirula::Sha256 hash;
    if (has_source(file)) write_source(output, file, hash);
    else write_bytes(output, file.bytes, hash);
    output.flush();
    if (!output) fail("cannot flush staged file: " + file.path);
    output.close();
    if (output.fail()) fail("cannot close staged file: " + file.path);
    if (hash.hex() != file.sha256)
        fail("digest mismatch for selected input: " + file.path);
    if (regular_file_size(temporary, root, file.path) != file.size)
        fail("staged file size differs: " + file.path);
    if (!missing(path_status(destination)))
        fail("bundle destination appeared during staging: " + file.path);
    std::error_code ec;
    created_files.push_back(destination);
    fs::rename(NativeFilesystemPath(temporary),
               NativeFilesystemPath(destination), ec);
    if (ec) {
        created_files.pop_back();
        fail("cannot atomically publish staged file: " + file.path);
    }
}

void replace_binding_file(const fs::path& path,
                          const fw::WorkerBinding& binding) {
    const std::string bytes = fw::writeWorkerBinding(binding);
    if (read_bounded_file(path, 64ull << 20) == bytes) return;
    static std::atomic<std::uint64_t> serial{0};
    const auto stamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path temporary = path.parent_path() /
        fs::u8path(".feature-output-binding-" + std::to_string(stamp) + "-" +
                   std::to_string(serial.fetch_add(1)) + ".part");
    if (!missing(path_status(temporary)))
        fail("temporary output binding path already exists");
    std::ofstream output(NativeFilesystemPath(temporary),
                         std::ios::binary | std::ios::trunc);
    if (!output) {
        std::error_code ec;
        fs::remove(NativeFilesystemPath(temporary), ec);
        fail("cannot create temporary output binding");
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    output.close();
    if (output.fail()) {
        std::error_code ec;
        fs::remove(NativeFilesystemPath(temporary), ec);
        fail("cannot write temporary output binding");
    }
#ifdef _WIN32
    if (!MoveFileExW(NativeFilesystemPath(temporary).c_str(),
                     NativeFilesystemPath(path).c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec;
        fs::remove(NativeFilesystemPath(temporary), ec);
        fail("cannot atomically publish sanitized worker binding");
    }
#else
    std::error_code ec;
    fs::rename(NativeFilesystemPath(temporary),
               NativeFilesystemPath(path), ec);
    if (ec) {
        fs::remove(NativeFilesystemPath(temporary), ec);
        fail("cannot atomically publish sanitized worker binding");
    }
#endif
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
    if (ec) fail("cannot inspect attempt root");
    if (it != end) fail("attempt root is not empty");
}

FeatureInputBundle result_bundle(const fs::path& root,
                                 const fw::FeaturePlan& plan,
                                 const fw::FeatureRequest& request,
                                 std::vector<TransferFile> manifest) {
    FeatureInputBundle result;
    result.plan = plan;
    result.request = request;
    result.bundle_root = root;
    result.plan_path = root / fs::u8path(request.plan_path);
    result.request_path = root / "request.json";
    result.image_root = root / "images";
    result.mask_root = root / "masks";
    result.manifest = std::move(manifest);
    return result;
}

std::map<std::string, const TransferFile*> manifest_map(
    const std::vector<TransferFile>& manifest, std::uint64_t quota) {
    const std::vector<FileSpec> specs = [&] {
        std::vector<FileSpec> out;
        out.reserve(manifest.size());
        for (const TransferFile& file : manifest)
            out.push_back({file.path, file.sha256, file.size});
        return out;
    }();
    const auto canonical = manifest_for(specs, quota);
    if (canonical.size() != manifest.size()) fail("approved manifest differs");
    std::map<std::string, const TransferFile*> result;
    for (const TransferFile& file : manifest)
        result.emplace(file.path, &file);
    return result;
}

void verify_tree(const fs::path& root,
                 const std::set<std::string>& files,
                 bool allow_output_lease = false) {
    std::set<std::string> directories;
    for (const std::string& path : files) {
        for (std::size_t slash = path.find('/'); slash != std::string::npos;
             slash = path.find('/', slash + 1))
            directories.insert(path.substr(0, slash));
    }
    std::set<std::string> seen;
    std::error_code ec;
    const fs::path native_root = NativeFilesystemPath(root);
    fs::recursive_directory_iterator it(
        native_root, fs::directory_options::none, ec), end;
    if (ec) fail("cannot enumerate attempt bundle");
    for (; it != end; it.increment(ec)) {
        if (ec) fail("cannot enumerate attempt bundle");
        const fs::path path = it->path();
        const std::string relative =
            path.lexically_relative(native_root).generic_u8string();
        const fs::file_status status = path_status(path);
        if (fs::is_symlink(status) || is_reparse_point(path))
            fail("bundle contains a linked path: " + relative);
        if (fs::is_directory(status)) {
            if (relative != kTransferStage && !directories.count(relative))
                fail("bundle contains an unauthorized directory: " + relative);
            continue;
        }
        if (!fs::is_regular_file(status))
            fail("bundle contains a non-regular file: " + relative);
        if (!files.count(relative)) {
            if (allow_output_lease && relative == kOutputLeaseFile) {
                if (regular_file_size(path, root, relative) != 0)
                    fail("worker output lease marker is not empty");
                continue;
            }
            fail("bundle contains an unauthorized file: " + relative);
        }
        if (!seen.insert(relative).second)
            fail("bundle repeats a file path: " + relative);
    }
    for (const std::string& path : files)
        if (!seen.count(path)) fail("approved bundle file is missing: " + path);
}

void verify_tree(const fs::path& root,
                 const std::map<std::string, const TransferFile*>& files) {
    std::set<std::string> paths;
    for (const auto& [path, ignored] : files) paths.insert(path);
    verify_tree(root, paths);
}

void add_output_file(const fs::path& root, const std::string& relative,
                     std::vector<FileSpec>& files) {
    safe_relative_path(relative);
    if (files.size() >= kMaxManifestFiles)
        fail("manifest has too many files");
    const fs::path path = checked_path(root, relative, false, nullptr, true);
    const std::uint64_t size = regular_file_size(path, root, relative);
    files.push_back({relative, read_digest(path, size, relative), size, path, {}});
}
std::vector<TransferFile> inspect_feature_outputs(
    const fs::path& request_path, const fs::path& result_root,
    std::uint64_t disk_budget_bytes, bool redact_worker_paths) {
    const SourceRecords records = read_output_source_records(request_path);
    const fw::FeaturePlan& plan = records.plan;
    const fw::FeatureRequest& request = records.request;
    const fs::path root = safe_root(result_root, "feature output root");
    std::uint64_t total = 0;

    const fs::path request_record = resolve_file(root, "request.json");
    add_size(total, regular_file_size(request_record, root, "request.json"),
             disk_budget_bytes);
    const fw::FeatureRequest output_request =
        fw::readRequest(read_bounded_file(request_record, 64ull << 20));
    if (fw::requestDigest(output_request) != fw::requestDigest(request))
        fail("output request identity differs from the leader request");

    const fs::path binding_path = resolve_file(root, "binding.json");
    fw::WorkerBinding binding =
        fw::readWorkerBinding(read_bounded_file(binding_path, 64ull << 20));
    fw::validateWorkerBinding(plan, binding);
    if (fw::requestDigest(binding.extraction_request) != fw::requestDigest(request))
        fail("worker binding names another request");
    if (redact_worker_paths) {
        binding.request_path = "request.json";
        binding.image_root.clear();
        binding.mask_root.clear();
        binding.model_root.clear();
        binding.device.clear();
        binding.output_dir.clear();
        binding.digest = fw::workerBindingDigest(binding);
        fw::validateWorkerBinding(plan, binding);
    } else if (binding.request_path != "request.json" ||
               !binding.image_root.empty() || !binding.mask_root.empty() ||
               !binding.model_root.empty() || !binding.device.empty() ||
               !binding.output_dir.empty()) {
        fail("worker binding contains unapproved worker-local paths");
    }
    add_size(total, static_cast<std::uint64_t>(
                        fw::writeWorkerBinding(binding).size()),
             disk_budget_bytes);

    const fs::path result_path = resolve_file(root, "result.json");
    add_size(total, regular_file_size(result_path, root, "result.json"),
             disk_budget_bytes);
    const fw::ShardResult result =
        fw::readShardResult(read_bounded_file(result_path, 64ull << 20));
    fw::validateShardResult(plan, request, result);
    if (!result.complete)
        fail("feature result is incomplete");
    if (result.outcomes.size() > (kMaxManifestFiles - 3) / 2)
        fail("manifest has too many files");

    std::vector<std::string> paths = {
        "binding.json", "result.json", "request.json"};
    paths.reserve(3 + result.outcomes.size() * 2);
    for (const fw::ImageOutcome& outcome : result.outcomes) {
        if (!fw::successful(outcome.status))
            fail("feature result contains a failed image");
        const std::string receipt_path = receipt_relative_path(outcome.global_index);
        const fs::path receipt_file = resolve_file(root, receipt_path);
        add_size(total, regular_file_size(receipt_file, root, receipt_path),
                 disk_budget_bytes);
        const fw::ImageReceipt receipt =
            fw::readReceipt(read_bounded_file(receipt_file, 64ull << 20));
        fw::validateReceipt(plan, request, receipt);
        if (receipt.digest != outcome.receipt_digest)
            fail("result and receipt digests disagree");
        const std::string payload_path = "payload/" + receipt.feature_path;
        safe_relative_path(payload_path);
        const fs::path payload_file = resolve_file(root, payload_path);
        add_size(total, regular_file_size(payload_file, root, payload_path),
                 disk_budget_bytes);
        paths.push_back(receipt_path);
        paths.push_back(payload_path);
    }
    verify_tree(root, std::set<std::string>(paths.begin(), paths.end()),
                 redact_worker_paths);

    if (redact_worker_paths) replace_binding_file(binding_path, binding);
    // Validate the redacted output against the leader's unchanged request.
    sfm::validateFeatureResult(
        LogicalAbsoluteFilesystemPath(request_path).u8string(),
        LogicalFilesystemPath(root));

    std::vector<FileSpec> files;
    files.reserve(paths.size());
    for (const std::string& path : paths) add_output_file(root, path, files);
    const std::vector<TransferFile> manifest =
        manifest_for(files, disk_budget_bytes);
    verify_tree(root, std::set<std::string>(paths.begin(), paths.end()),
                 redact_worker_paths);
    return manifest;
}


}  // namespace
std::vector<TransferFile> FeatureOutputManifest(
    const std::filesystem::path& request_path,
    const std::filesystem::path& result_root,
    std::uint64_t disk_budget_bytes) {
    try {
        return inspect_feature_outputs(request_path, result_root,
                                       disk_budget_bytes, true);
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string("feature output: ") + error.what());
    }
}

void VerifyFeatureOutputs(
    const std::filesystem::path& request_path,
    const std::filesystem::path& result_root,
    const std::vector<TransferFile>& approved_manifest,
    std::uint64_t disk_budget_bytes) {
    try {
        const std::vector<TransferFile> actual = inspect_feature_outputs(
            request_path, result_root, disk_budget_bytes, false);
        const auto expected_files = manifest_map(actual, disk_budget_bytes);
        const auto approved_files =
            manifest_map(approved_manifest, disk_budget_bytes);
        if (expected_files.size() != approved_files.size())
            fail("approved output manifest differs");
        for (const auto& [path, expected] : expected_files) {
            const auto found = approved_files.find(path);
            if (found == approved_files.end() ||
                found->second->size != expected->size ||
                found->second->sha256 != expected->sha256)
                fail("approved output manifest differs: " + path);
        }
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string("feature output: ") + error.what());
    }
}

FeatureInputBundle StageFeatureInputs(
    const fs::path& plan_file, const fs::path& request_file,
    const fs::path& image_root, const fs::path& mask_root,
    const fs::path& attempt_root, std::uint64_t disk_budget_bytes) {
    std::vector<fs::path> created_dirs;
    std::vector<fs::path> created_files;
    try {
        SourceRecords records = read_source_records(plan_file, request_file);
        const fw::FeatureRequest request = portable_request(records.plan, records.request);
        const fs::path images = safe_root(image_root, "image root");
        const bool needs_masks = std::any_of(
            request.image_indices.begin(), request.image_indices.end(),
            [&](std::uint32_t index) {
                return !records.plan.images[index].mask_path.empty();
            });
        const fs::path masks = needs_masks ? safe_root(mask_root, "mask root") : fs::path{};
        const fs::path root = safe_root(attempt_root, "attempt root");
        require_empty_root(root);

        std::vector<FileSpec> files = expected_files(
            records.plan, request, images, masks, true);
        std::uint64_t total = 0;
        for (const FileSpec& file : files) add_size(total, file.size, disk_budget_bytes);
        const std::vector<TransferFile> manifest = manifest_for(files, disk_budget_bytes);
        for (const FileSpec& file : files)
            write_atomic(root, file, created_dirs, created_files);
        return result_bundle(root, records.plan, request, manifest);
    } catch (const std::exception& error) {
        cleanup(created_files, created_dirs);
        throw std::runtime_error(std::string("feature bundle: ") + error.what());
    } catch (...) {
        cleanup(created_files, created_dirs);
        throw;
    }
}

FeatureInputBundle VerifyFeatureInputs(
    const fs::path& attempt_root, const fw::FeaturePlan& expected_plan,
    const fw::FeatureRequest& expected_request,
    const std::vector<TransferFile>& approved_manifest,
    std::uint64_t disk_budget_bytes) {
    try {
        fw::validateRequest(expected_plan, expected_request);
        const fw::FeatureRequest request = portable_request(expected_plan, expected_request);
        const fs::path root = safe_root(attempt_root, "attempt root");
        const std::map<std::string, const TransferFile*> approved =
            manifest_map(approved_manifest, disk_budget_bytes);

        const std::vector<FileSpec> expected = expected_files(
            expected_plan, request, {}, {}, false);
        if (expected.size() != approved.size())
            fail("approved manifest does not match selected feature inputs");
        std::map<std::string, std::string> expected_digests;
        std::map<std::string, std::uint64_t> exact_sizes;
        for (const FileSpec& file : expected) {
            expected_digests.emplace(file.path, file.sha256);
            if (file.path == request.plan_path || file.path == "request.json")
                exact_sizes.emplace(file.path, file.size);
        }
        for (const auto& [path, file] : approved) {
            const auto digest = expected_digests.find(path);
            if (digest == expected_digests.end() || digest->second != file->sha256)
                fail("approved manifest has an unauthorized path or digest: " + path);
            const auto size = exact_sizes.find(path);
            if (size != exact_sizes.end() && size->second != file->size)
                fail("approved metadata size differs: " + path);
        }
        verify_tree(root, approved);
        for (const auto& [relative, file] : approved) {
            const fs::path path = checked_path(root, relative, false, nullptr, true);
            const std::uint64_t size = regular_file_size(path, root, relative);
            if (size != file->size)
                fail("bundle file size differs: " + relative);
            if (read_digest(path, size, relative) != file->sha256)
                fail("bundle file digest mismatch: " + relative);
        }

        const fs::path request_file = resolve_file(root, "request.json");
        const fw::FeatureRequest received_request = fw::readRequest(
            read_bounded_file(request_file, 64ull << 20));
        if (fw::requestDigest(received_request) != fw::requestDigest(request))
            fail("received request identity differs");
        const fs::path plan_file = resolve_file(root, received_request.plan_path);
        const fw::FeaturePlan received_plan = fw::readPlan(
            read_bounded_file(plan_file, 64ull << 20));
        if (fw::planDigest(received_plan) != fw::planDigest(expected_plan))
            fail("received plan identity differs");
        fw::validateRequest(received_plan, received_request);
        return result_bundle(root, received_plan, received_request, approved_manifest);
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string("feature bundle: ") + error.what());
    }
}

}  // namespace app::agent
