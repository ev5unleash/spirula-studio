#include <checkpoint/B1Merge.h>
#include <checkpoint/B1Contract.h>
#include <checkpoint/SplatPly.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

namespace fs = std::filesystem;
constexpr uint64_t kB1BlockRows = 256;

struct Property {
    std::string type;
    std::string name;
};

struct PlyHeader {
    uint64_t vertices = 0;
    uint64_t data_offset = 0;
    size_t row_bytes = 0;
    std::vector<Property> properties;
};

struct PlyFile {
    std::string path;
    PlyHeader header;
    uint64_t bytes = 0;
};

size_t scalar_size(const std::string& type) {
    if (type == "char" || type == "uchar" || type == "int8" || type == "uint8")
        return 1;
    if (type == "short" || type == "ushort" || type == "int16" || type == "uint16")
        return 2;
    if (type == "int" || type == "uint" || type == "float" ||
        type == "int32" || type == "uint32" || type == "float32")
        return 4;
    if (type == "double" || type == "float64") return 8;
    return 0;
}

[[noreturn]] void bad(const std::string& path, const std::string& detail) {
    throw std::runtime_error(path + ": " + detail);
}

uint64_t stream_length(std::ifstream& in, const std::string& path) {
    in.seekg(0, std::ios::end);
    const std::streampos end = in.tellg();
    if (end < 0) bad(path, "cannot determine byte length");
    in.seekg(0, std::ios::beg);
    if (!in) bad(path, "cannot rewind");
    return (uint64_t)end;
}

void read_bytes(std::istream& in, uint8_t* bytes, std::streamsize count,
                const std::string& path) {
    in.read(reinterpret_cast<char*>(bytes), count);
    if (in.gcount() != count) bad(path, "truncated sidecar");
}

uint32_t read_le32(std::istream& in, const std::string& path) {
    uint8_t bytes[4];
    read_bytes(in, bytes, 4, path);
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

uint64_t read_le64(std::istream& in, const std::string& path) {
    uint8_t bytes[8];
    read_bytes(in, bytes, 8, path);
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) value |= (uint64_t)bytes[i] << (8 * i);
    return value;
}

struct B1Blocks {
    uint64_t active_n = 0;
    uint64_t source_id = 0;
    uint64_t step = 0;
    uint32_t owner = 0;
    std::vector<uint32_t> mask;
};

struct B1Ids {
    uint64_t vertices = 0;
    std::vector<uint32_t> ids;
};

B1Blocks read_b1_blocks(const fs::path& path) {
    const std::string name = path.string();
    std::ifstream in(path, std::ios::binary);
    if (!in) bad(name, "cannot open");
    const uint64_t bytes = stream_length(in, name);
    if (bytes < 32) bad(name, "truncated header");

    const uint64_t active_n = read_le64(in, name);
    const uint64_t source_id = read_le64(in, name);
    const uint64_t step = read_le64(in, name);
    const uint32_t block_count = read_le32(in, name);
    const uint32_t owner = read_le32(in, name);
    if (owner > 1) bad(name, "owner must be 0 or 1");

    const uint64_t expected_blocks =
        active_n == 0 ? 0 : (active_n - 1) / kB1BlockRows + 1;
    if (expected_blocks != block_count)
        bad(name, "block count does not match active N");
    if (!source_id) bad(name, "source checkpoint ID is zero");
    const uint64_t expected_bytes = 32 + (uint64_t)block_count * sizeof(uint32_t);
    if (bytes != expected_bytes) bad(name, "byte length does not match block count");
    if ((uint64_t)block_count > std::numeric_limits<size_t>::max())
        bad(name, "block count is out of range");

    B1Blocks result;
    result.active_n = active_n;
    result.source_id = source_id;
    result.step = step;
    result.owner = owner;
    result.mask.resize((size_t)block_count);
    for (uint32_t& value : result.mask) {
        value = read_le32(in, name);
        if (value > 1) bad(name, "mask value must be 0 or 1");
    }
    return result;
}

B1Ids read_b1_ids(const fs::path& path) {
    const std::string name = path.string();
    std::ifstream in(path, std::ios::binary);
    if (!in) bad(name, "cannot open");
    const uint64_t bytes = stream_length(in, name);
    if (bytes < sizeof(uint64_t)) bad(name, "truncated header");

    const uint64_t vertices = read_le64(in, name);
    if (vertices > (std::numeric_limits<uint64_t>::max() - sizeof(uint64_t)) /
                       sizeof(uint32_t))
        bad(name, "vertex count overflows byte length");
    const uint64_t expected_bytes = sizeof(uint64_t) + vertices * sizeof(uint32_t);
    if (bytes != expected_bytes) bad(name, "byte length does not match vertex count");
    if (vertices > std::numeric_limits<size_t>::max())
        bad(name, "vertex count is out of range");

    B1Ids result;
    result.vertices = vertices;
    result.ids.resize((size_t)vertices);
    for (uint32_t& id : result.ids) id = read_le32(in, name);
    return result;
}

fs::path sidecar_path(const std::string& ply_path, const char* name) {
    return fs::path(ply_path).parent_path() / name;
}

bool path_exists(const fs::path& path) {
    std::error_code error;
    const bool exists = fs::exists(path, error);
    if (error) bad(path.string(), "cannot inspect path: " + error.message());
    return exists;
}

fs::path fresh_sibling_temp(const fs::path& output) {
    const fs::path parent = output.parent_path().empty() ? fs::path(".")
                                                         : output.parent_path();
    const uint64_t nonce = (uint64_t)std::chrono::high_resolution_clock::now()
                               .time_since_epoch()
                               .count();
    const std::string prefix = output.filename().string() + ".tmp-" +
                               std::to_string(nonce) + "-";
    for (uint32_t attempt = 0; attempt < 1000; attempt++) {
        const fs::path candidate = parent / (prefix + std::to_string(attempt));
        if (!path_exists(candidate)) return candidate;
    }
    bad(output.string(), "cannot find a fresh sibling temporary path");
}

void validate_ids(const std::string& path, uint64_t expected, const B1Blocks& blocks,
                   const B1Ids& ids, std::unordered_set<uint32_t>& seen) {
    if (ids.vertices != expected)
        bad(path, "B1 sidecar ID count mismatch");
    uint32_t previous = 0;
    for (size_t row = 0; row < ids.ids.size(); row++) {
        const uint32_t id = ids.ids[row];
        if (row != 0 && id <= previous)
            bad(path, "b1-owned-ids.bin row IDs are not increasing");
        previous = id;
        if ((uint64_t)id >= blocks.active_n)
            bad(path, "b1-owned-ids.bin row ID is outside active N");
        const uint64_t block = (uint64_t)id / kB1BlockRows;
        if (block >= blocks.mask.size() || blocks.mask[(size_t)block] != 1)
            bad(path, "b1-owned-ids.bin row ID is not owned by its mask");
        if (!seen.insert(id).second)
            bad(path, "duplicate original row ID in merged outputs");
    }
}

PlyHeader read_header(std::istream& in, const std::string& path) {
    PlyHeader h;
    std::string line;
    if (!std::getline(in, line)) bad(path, "missing header");
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != "ply") bad(path, "missing ply magic");

    bool format_seen = false;
    bool vertex_seen = false;
    bool in_vertex = false;
    for (;;) {
        if (!std::getline(in, line)) bad(path, "missing end_header");
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream fields(line);
        std::string token;
        fields >> token;
        if (token == "format") {
            std::string format, version, extra;
            if (format_seen || !(fields >> format >> version) || fields >> extra ||
                format != "binary_little_endian" || version != "1.0")
                bad(path, "format must be binary_little_endian 1.0");
            format_seen = true;
        } else if (token == "element") {
            std::string name, extra;
            int64_t count;
            if (!(fields >> name >> count) || fields >> extra || count < 0 ||
                name != "vertex" || vertex_seen)
                bad(path, "only one nonnegative vertex element is supported");
            vertex_seen = true;
            in_vertex = true;
            h.vertices = (uint64_t)count;
        } else if (token == "property") {
            std::string type, name, extra;
            if (!in_vertex || !(fields >> type >> name) || fields >> extra ||
                type == "list")
                bad(path, "vertex properties must be scalar");
            const size_t bytes = scalar_size(type);
            if (bytes == 0 || h.row_bytes > std::numeric_limits<size_t>::max() - bytes)
                bad(path, "unsupported or oversized vertex property");
            for (const Property& p : h.properties)
                if (p.name == name) bad(path, "duplicate vertex property " + name);
            h.properties.push_back({type, name});
            h.row_bytes += bytes;
        } else if (token == "end_header") {
            if (!format_seen || !vertex_seen || h.properties.empty())
                bad(path, "incomplete header");
            const std::streampos pos = in.tellg();
            if (pos < 0) bad(path, "invalid data offset");
            h.data_offset = (uint64_t)pos;
            return h;
        } else if (token != "comment" && token != "obj_info" && !token.empty()) {
            bad(path, "unsupported header line");
        }
    }
}

bool has_property(const PlyHeader& h, const char* name) {
    for (const Property& p : h.properties)
        if (p.name == name) return true;
    return false;
}

PlyFile inspect(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) bad(path, "cannot open");
    PlyFile file{path, read_header(in, path), 0};
    const std::streampos end = [&]() {
        in.seekg(0, std::ios::end);
        return in.tellg();
    }();
    if (end < 0) bad(path, "cannot determine byte length");
    file.bytes = (uint64_t)end;
    if (file.header.row_bytes == 0 ||
        file.header.vertices >
            (std::numeric_limits<uint64_t>::max() - file.header.data_offset) /
                file.header.row_bytes)
        bad(path, "vertex data length overflows");
    const uint64_t expected = file.header.data_offset +
                              file.header.vertices * file.header.row_bytes;
    if (file.bytes != expected)
        bad(path, "byte length does not match vertex count and schema");

    const char* required[] = {
        "x", "y", "z", "opacity", "scale_0", "scale_1", "scale_2",
        "rot_0", "rot_1", "rot_2", "rot_3", "f_dc_0", "f_dc_1", "f_dc_2",
    };
    for (const char* name : required)
        if (!has_property(file.header, name)) bad(path, "missing splat property " + std::string(name));
    return file;
}

bool same_schema(const PlyHeader& a, const PlyHeader& b) {
    if (a.row_bytes != b.row_bytes || a.properties.size() != b.properties.size())
        return false;
    for (size_t i = 0; i < a.properties.size(); i++)
        if (a.properties[i].type != b.properties[i].type ||
            a.properties[i].name != b.properties[i].name)
            return false;
    return true;
}

std::string output_header(const PlyHeader& h, uint64_t vertices,
                          const std::string& appearance) {
    std::ostringstream out;
    out << "ply\nformat binary_little_endian 1.0\n" << appearance
        << "\nelement vertex " << vertices << "\n";
    for (const Property& p : h.properties)
        out << "property " << p.type << ' ' << p.name << "\n";
    out << "end_header\n";
    return out.str();
}

void copy_body(const PlyFile& file, std::ofstream& out) {
    std::ifstream in(file.path, std::ios::binary);
    if (!in) bad(file.path, "cannot reopen");
    in.seekg((std::streamoff)file.header.data_offset);
    if (!in) bad(file.path, "cannot seek to vertex data");
    std::vector<char> buffer(1 << 20);
    uint64_t remaining = file.bytes - file.header.data_offset;
    while (remaining != 0) {
        const std::streamsize chunk = (std::streamsize)std::min<uint64_t>(
            remaining, buffer.size());
        in.read(buffer.data(), chunk);
        if (in.gcount() != chunk) bad(file.path, "truncated vertex data");
        out.write(buffer.data(), chunk);
        if (!out) bad(file.path, "output write failed");
        remaining -= (uint64_t)chunk;
    }
}

}  // namespace

namespace ckpt {

void merge_b1_ply(const fs::path& left_path, const fs::path& right_path,
                  const fs::path& output_path) {
    fs::path temp_path;
    bool owns_temp = false;
    try {
        if (path_exists(output_path))
            throw std::runtime_error("output PLY already exists");
        const auto left_contract = read_b1_contract(left_path.parent_path());
        const auto right_contract = read_b1_contract(right_path.parent_path());
        if (!left_contract || !right_contract)
            throw std::runtime_error("merge requires marked native B1 checkpoints");
        validate_b1_pair(*left_contract, *right_contract);
        if (left_contract->version == 3 &&
            left_contract->refresh_generation != left_contract->completed_step)
            throw std::runtime_error("live-context merge requires a completed refresh boundary");
        if (!fs::equivalent(left_path, left_path.parent_path() / "splat.ply") ||
            !fs::equivalent(right_path, right_path.parent_path() / "splat.ply"))
            throw std::runtime_error("merge inputs must be native checkpoint splat.ply files");
        const PlyFile left = inspect(left_path.string());
        const PlyFile right = inspect(right_path.string());
        if (!same_schema(left.header, right.header))
            throw std::runtime_error("input PLY schemas differ");

        const B1Blocks left_blocks = read_b1_blocks(
            sidecar_path(left_path.string(), "b1-owned-blocks.bin"));
        const B1Blocks right_blocks = read_b1_blocks(
            sidecar_path(right_path.string(), "b1-owned-blocks.bin"));
        const auto validate_block_contract = [](const B1Blocks& blocks,
                                                const B1Contract& contract) {
            if (blocks.active_n != (uint64_t)contract.rows ||
                blocks.step != (uint64_t)contract.completed_step ||
                blocks.owner != (uint32_t)contract.owner)
                throw std::runtime_error("B1 blocks do not match native contract");
        };
        validate_block_contract(left_blocks, *left_contract);
        validate_block_contract(right_blocks, *right_contract);
        if (left_blocks.active_n != right_blocks.active_n)
            throw std::runtime_error("B1 sidecars have different active N");
        if (left_blocks.source_id != right_blocks.source_id ||
            left_blocks.step != right_blocks.step)
            throw std::runtime_error("B1 sidecars have different source or step");
        if (left_blocks.owner == right_blocks.owner)
            throw std::runtime_error("B1 sidecars must have different owners");
        if (left_blocks.mask.size() != right_blocks.mask.size())
            throw std::runtime_error("B1 sidecars have different block counts");
        for (size_t i = 0; i < left_blocks.mask.size(); i++)
            if (left_blocks.mask[i] + right_blocks.mask[i] != 1)
                throw std::runtime_error("B1 sidecar masks are not complementary");

        const B1Ids left_ids = read_b1_ids(sidecar_path(left_path.string(), "b1-owned-ids.bin"));
        const B1Ids right_ids = read_b1_ids(sidecar_path(right_path.string(), "b1-owned-ids.bin"));
        const B1Ids left_excluded = read_b1_ids(sidecar_path(left_path.string(), "b1-excluded-ids.bin"));
        const B1Ids right_excluded = read_b1_ids(sidecar_path(right_path.string(), "b1-excluded-ids.bin"));
        std::unordered_set<uint32_t> seen_ids;
        validate_ids(left_path.string(), left.header.vertices, left_blocks, left_ids, seen_ids);
        validate_ids(right_path.string(), right.header.vertices, right_blocks, right_ids, seen_ids);
        validate_ids(left_path.string(), left_excluded.vertices, left_blocks, left_excluded, seen_ids);
        validate_ids(right_path.string(), right_excluded.vertices, right_blocks, right_excluded, seen_ids);
        if (seen_ids.size() != left_blocks.active_n)
            throw std::runtime_error("B1 owned and excluded IDs do not cover active N");

        if (left.header.vertices > std::numeric_limits<uint64_t>::max() - right.header.vertices)
            throw std::runtime_error("merged vertex count overflows");
        const uint64_t vertices = left.header.vertices + right.header.vertices;
        if (vertices > (uint64_t)std::numeric_limits<int64_t>::max())
            throw std::runtime_error("merged vertex count exceeds reader range");

        temp_path = fresh_sibling_temp(output_path);
        if (!fs::create_directory(temp_path))
            bad(temp_path.string(), "cannot reserve temporary directory");
        owns_temp = true;
        const fs::path staged_ply = temp_path / "payload.ply";
        {
            std::ofstream out(staged_ply, std::ios::binary | std::ios::trunc);
            if (!out) bad(staged_ply.string(), "cannot open for write");
            const std::string header = output_header(left.header, vertices,
                b1_appearance_comment(*left_contract, std::string(64, '0')));
            out.write(header.data(), (std::streamsize)header.size());
            if (!out) bad(staged_ply.string(), "header write failed");
            copy_body(left, out);
            copy_body(right, out);
            out.close();
            if (!out) bad(staged_ply.string(), "close failed");
        }
        {
            const std::string header = output_header(left.header, vertices,
                b1_appearance_comment(*left_contract, b1_ply_body_hash(staged_ply)));
            if (header.size() != inspect(staged_ply.string()).header.data_offset)
                bad(staged_ply.string(), "appearance binding changed header length");
            std::fstream out(staged_ply, std::ios::binary | std::ios::in | std::ios::out);
            if (!out) bad(staged_ply.string(), "cannot reopen binding for write");
            out.write(header.data(), (std::streamsize)header.size());
            out.close();
            if (!out) bad(staged_ply.string(), "binding write failed");
        }

        const spirula::SplatCloud merged = spirula::read_splat_ply(staged_ply.string());
        if (merged.num != (int64_t)vertices)
            throw std::runtime_error("read-back vertex count mismatch");

        std::error_code publish_error;
        fs::create_hard_link(staged_ply, output_path, publish_error);
        if (publish_error)
            bad(output_path.string(), "atomic no-replace publish failed: " + publish_error.message());
        std::error_code cleanup_error;
        fs::remove_all(temp_path, cleanup_error);
        owns_temp = false;
        temp_path.clear();
    } catch (...) {
        if (owns_temp) {
            std::error_code cleanup_error;
            fs::remove_all(temp_path, cleanup_error);
        }
        throw;
    }
}

}  // namespace ckpt
