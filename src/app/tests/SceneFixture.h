#pragma once

#include "external/stb_image_write.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace test {
namespace fs = std::filesystem;

inline void write_scene(const fs::path& root) {
    fs::create_directories(root / "images");
    std::vector<unsigned char> rgb(64 * 64 * 3);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const int k = 3 * (y * 64 + x);
            rgb[k] = static_cast<unsigned char>(x * 4);
            rgb[k + 1] = static_cast<unsigned char>(y * 4);
            rgb[k + 2] = static_cast<unsigned char>(((x / 8 + y / 8) % 2) * 180 + 30);
        }
    std::ofstream json(root / "transforms.json");
    json << "{\"camera_model\":\"PINHOLE\",\"w\":64,\"h\":64,"
            "\"fl_x\":50,\"fl_y\":50,\"cx\":32,\"cy\":32,"
            "\"ply_file_path\":\"points.ply\",\"frames\":[";
    for (int i = 0; i < 6; ++i) {
        const std::string name = "view" + std::to_string(i) + ".png";
        if (!stbi_write_png((root / "images" / name).u8string().c_str(), 64, 64, 3,
                            rgb.data(), 64 * 3))
            throw std::runtime_error("cannot write fixture image");
        if (i) json << ',';
        json << "{\"file_path\":\"images/" << name << "\",\"transform_matrix\":["
             << "[1,0,0," << (i - 2.5) * 0.1 << "],[0,-1,0,0],[0,0,-1,0],[0,0,0,1]]}";
    }
    json << "]}";
    std::ofstream ply(root / "points.ply");
    ply << "ply\nformat ascii 1.0\nelement vertex 64\nproperty float x\nproperty float y\n"
           "property float z\nproperty uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n";
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            ply << (x - 3.5) * 0.08 << ' ' << (y - 3.5) * 0.08 << ' '
                << 2.0 + 0.03 * ((x + y) % 3) << " 128 160 192\n";
    if (!json || !ply) throw std::runtime_error("cannot write fixture metadata");
}

}  // namespace test
