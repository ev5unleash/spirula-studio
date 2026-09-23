#include "data/DatasetParser.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

int main() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path path = fs::temp_directory_path() /
                          ("spirula-ply-reader-" + std::to_string(suffix) + ".ply");
    struct Remove { fs::path path; ~Remove() { std::error_code ec; fs::remove(path, ec); } } remove{path};
    const auto parse = [&](const std::string& contents) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << contents;
        out.close();
        if (!out) throw std::runtime_error("cannot write PLY fixture");
        return read_ply_points(path.string());
    };
    const auto rejects = [&](const std::string& contents) {
        try { parse(contents); }
        catch (const std::runtime_error&) { return; }
        throw std::runtime_error("malformed PLY was accepted");
    };

    const auto points = parse("ply\nformat ascii 1.0\nelement vertex 1\n"
                              "property float x\nproperty float y\nproperty float z\n"
                              "property uchar red\nproperty uchar green\n"
                              "property uchar blue\nend_header\n1 2 3 4 5 6\n");
    if (points.num() != 1 || points.xyz[0] != 1 || points.rgb[2] != 6)
        throw std::runtime_error("valid PLY contents were changed");

    rejects("ply\nformat binary_little_endian 1.0\nelement vertex 100000000000\n"
            "property float x\nproperty float y\nproperty float z\n"
            "property uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n");
    rejects("ply\nformat binary_little_endian 1.0\nelement face 100000000000\n"
            "property uchar flag\nelement vertex 1\n"
            "property float x\nproperty float y\nproperty float z\n"
            "property uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n");
    rejects("ply\nformat ascii 1.0\nelement vertex 1\nproperty\nend_header\n");
}
