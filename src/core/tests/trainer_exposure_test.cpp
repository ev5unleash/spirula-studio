// The trainer's EXIF seed path must center brightness multipliers, then copy
// each source seed to every post-split camera slot.

#include "app/TrainerCore.h"
#include "engine/EngineState.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

std::vector<float> seeded_exposures(const std::vector<float>& seeds,
                                    bool arithmetic_mean) {
    engine_reset();
    engine_init_ppisp((int)seeds.size(), "no_crf", true, arithmetic_mean, seeds);
    backend::device_synchronize();
    const int p = engine().ppisp.num_params;
    std::vector<float> params((size_t)engine().ppisp.params.numel());
    backend::memcpy_sync(params.data(), engine().ppisp.params.data_ptr(),
                         params.size() * sizeof(float),
                         backend::MemcpyKind::DeviceToHost);
    std::vector<float> out(seeds.size());
    for (size_t i = 0; i < out.size(); i++) out[i] = params[i * p];
    return out;
}

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf(condition ? "ok   %s\n" : "FAIL %s\n", message);
    if (!condition) failures++;
}

std::vector<uint8_t> exposure_tiff(uint32_t time_num, uint32_t time_den,
                                   uint32_t f_num, uint32_t f_den,
                                   uint16_t iso) {
    std::vector<uint8_t> out;
    auto u16 = [&](uint16_t v) {
        out.push_back((uint8_t)v);
        out.push_back((uint8_t)(v >> 8));
    };
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; i++) out.push_back((uint8_t)(v >> (8 * i)));
    };
    auto entry = [&](uint16_t tag, uint16_t type, uint32_t value) {
        u16(tag);
        u16(type);
        u32(1);
        if (type == 3) {
            u16((uint16_t)value);
            u16(0);
        } else {
            u32(value);
        }
    };

    out.push_back('I');
    out.push_back('I');
    u16(42);
    u32(8);
    u16(1);
    entry(0x8769, 4, 26);
    u32(0);
    u16(3);
    entry(0x829A, 5, 68);
    entry(0x829D, 5, 76);
    entry(0x8827, 3, iso);
    u32(0);
    u32(time_num);
    u32(time_den);
    u32(f_num);
    u32(f_den);
    return out;
}

void write_exif_jpeg(const fs::path& path, const std::vector<uint8_t>& tiff) {
    std::vector<uint8_t> bytes{0xFF, 0xD8, 0xFF, 0xE1};
    const uint16_t length = (uint16_t)(2 + 6 + tiff.size());
    bytes.push_back((uint8_t)(length >> 8));
    bytes.push_back((uint8_t)length);
    const uint8_t tag[6] = {'E', 'x', 'i', 'f', 0, 0};
    bytes.insert(bytes.end(), tag, tag + 6);
    bytes.insert(bytes.end(), tiff.begin(), tiff.end());
    bytes.push_back(0xFF);
    bytes.push_back(0xD9);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
}

}  // namespace

int main() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
                          ("spirula_trainer_exposure_" + std::to_string(nonce));
    fs::create_directories(root);
    const fs::path a = root / "a.jpg";
    const fs::path b = root / "b.jpg";
    write_exif_jpeg(a, exposure_tiff(1, 1, 1, 1, 1));
    write_exif_jpeg(b, exposure_tiff(4, 1, 1, 1, 1));

    ParsedDataset ds;
    ds.num_cameras = 3;
    ds.image_filenames = {a.u8string(), b.u8string(), (root / "missing.jpg").u8string()};
    PostSplitCameras post;
    post.n_post = 4;
    post.K_per_camera = {2, 1, 1};
    post.post_offsets = {0, 2, 3};

    int found = 0;
    const std::vector<float> arithmetic =
        spirula::trainer_exif_exposure_evs(ds, post, true, found);
    const double center = std::log2(1.5);
    const std::vector<float> expected_arithmetic = {
        (float)-center, (float)-center, (float)(1.0 - center), 0.0f};
    check(found == 2 && arithmetic.size() == 4,
          "EXIF seed path reads both tagged cameras");
    check(arithmetic == expected_arithmetic,
          "arithmetic gain center matches independent expected seeds");
    check(std::fabs((std::exp2(arithmetic[0]) + std::exp2(arithmetic[2])) * 0.5 - 1.0) < 1e-6,
          "centered brightness multipliers average to one");
    check(seeded_exposures(arithmetic, true) == expected_arithmetic,
          "arithmetic seeds cross the engine PPISP boundary");

    const std::vector<float> geometric =
        spirula::trainer_exif_exposure_evs(ds, post, false, found);
    const std::vector<float> expected_geometric = {-0.5f, -0.5f, 0.5f, 0.0f};
    check(geometric == expected_geometric,
          "geometric gain center matches independent expected seeds");
    check(seeded_exposures(geometric, false) == expected_geometric,
          "geometric seeds cross the engine PPISP boundary");

    fs::remove_all(root);
    if (failures == 0) std::printf("All trainer exposure tests passed.\n");
    return failures == 0 ? 0 : 1;
}
