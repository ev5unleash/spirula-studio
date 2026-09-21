#include "data/DataManager.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {

void write_ppm(const fs::path& path, int width, int height, uint8_t value) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot create PPM fixture");
    out << "P6\n" << width << ' ' << height << "\n255\n";
    for (int i = 0; i < width * height; ++i) {
        const char rgb[3] = {(char)value, (char)(value ^ 0x55), (char)(value ^ 0xaa)};
        out.write(rgb, sizeof(rgb));
    }
    if (!out) throw std::runtime_error("cannot write PPM fixture");
}

std::unique_ptr<DataManager> make_manager(const fs::path& root,
                                          uint64_t seed) {
    const std::vector<int32_t> widths = {2, 2, 2, 3, 3, 3};
    const std::vector<int32_t> heights = {2, 2, 2, 1, 1, 1};
    std::vector<std::string> images;
    images.reserve(widths.size());
    for (size_t i = 0; i < widths.size(); ++i)
        images.push_back((root / ("image-" + std::to_string(i) + ".ppm")).string());

    DataManagerConfig cfg;
    cfg.cache_mode = CacheMode::CPU;
    cfg.load_masks = false;
    cfg.load_depths = false;
    cfg.load_normals = false;
    cfg.train_batch_size = 2;
    cfg.seed = seed;

    std::vector<float> viewmats(widths.size() * 16, 0.0f);
    std::vector<float> intrins(widths.size() * 4, 0.0f);
    std::vector<float> dist_coeffs(widths.size() * kCameraDistortionParams, 0.0f);
    return std::make_unique<DataManager>(
        std::move(cfg),
        std::vector<int32_t>(widths.size(), (int32_t)CameraModelType::PINHOLE),
        std::vector<int32_t>(widths.size(), (int32_t)CameraDistortionType::None),
        std::move(images), std::vector<std::string>{},
        std::vector<std::string>{}, std::vector<std::string>{},
        widths, heights, std::vector<int32_t>{}, std::vector<int32_t>{},
        std::move(viewmats), std::move(intrins), std::move(dist_coeffs),
        std::vector<int32_t>{}, std::vector<int32_t>{}, std::vector<float>{},
        std::vector<float>{}, std::vector<float>{}, std::vector<int32_t>{},
        std::vector<float>{}, std::vector<int32_t>{0, 1, 2, 3, 4, 5},
        std::vector<int32_t>{5});
}

using Sequence = std::vector<std::vector<std::vector<int32_t>>>;

Sequence collect(DataManager& dm, int count, bool sample_val) {
    Sequence out;
    out.reserve((size_t)count);
    for (int i = 0; i < count; ++i) {
        if (sample_val) (void)dm.next_val_batch();
        const TrainStep& step = dm.next_train_step();
        out.emplace_back();
        out.back().reserve(step.subs.size());
        for (const auto& sub : step.subs)
            out.back().push_back(sub->indices);
    }
    return out;
}

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() /
        ("spirula-data-manager-sampler-test-" +
         std::to_string((long long)std::chrono::steady_clock::now()
                            .time_since_epoch().count()));
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) return 1;

    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        if (!ok) {
            std::fprintf(stderr, "FAIL %s\n", name);
            ++failures;
        }
    };

    try {
        const std::vector<int32_t> widths = {2, 2, 2, 3, 3, 3};
        const std::vector<int32_t> heights = {2, 2, 2, 1, 1, 1};
        for (size_t i = 0; i < widths.size(); ++i)
            write_ppm(root / ("image-" + std::to_string(i) + ".ppm"),
                      widths[i], heights[i], (uint8_t)(16 + i));

        constexpr uint64_t seed = 0x123456789abcdef0ull;
        auto uninterrupted = make_manager(root, seed);
        check(uninterrupted->initial_seed() == seed,
              "constructor retains configured sampler seed");

        // Two three-image shape groups at B=2 produce three steps per epoch;
        // four consumed steps therefore crosses the first reshuffle boundary.
        constexpr int prefix_steps = 4;
        (void)collect(*uninterrupted, prefix_steps, true);
        const Sequence expected = collect(*uninterrupted, 8, true);
        check(uninterrupted->consumed_steps() == 12,
              "sampler counts delivered training steps");

        auto restored = make_manager(root, seed);
        restored->restore_sampler(restored->initial_seed(), prefix_steps);
        const Sequence actual = collect(*restored, (int)expected.size(), false);
        check(actual == expected,
              "restored CPU sampler ignores validation sampling history");
        check(restored->consumed_steps() == 12,
              "restored sampler advances its persisted cursor");

        auto nondeterministic = make_manager(root, 0);
        check(nondeterministic->initial_seed() != 0,
              "zero config seed resolves to a retained nonzero seed");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "DataManager sampler test error: %s\n", e.what());
        ++failures;
    }

    fs::remove_all(root, ec);
    return failures == 0 ? 0 : 1;
}
