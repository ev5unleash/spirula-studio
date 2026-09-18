#include "app/tests/SceneFixture.h"
#include "data/DatasetParser.h"

#include <cmath>
#include <cstdio>
#include <set>

namespace fs = std::filesystem;

template<class T> void binary(std::ostream& out, T value) {
    const uint16_t endian = 1;
    if (*reinterpret_cast<const unsigned char*>(&endian) != 1)
        throw std::runtime_error("fixture requires little-endian host");
    out.write(reinterpret_cast<const char*>(&value), sizeof value);
}

void other_formats(const fs::path& root, bool bin, bool xml) {
    test::write_scene(root);
    fs::remove(root / "transforms.json");
    if (xml) {
        std::ofstream f(root / "scene.xml");
        f << "<document><chunk><sensors><sensor id=\"0\" type=\"frame\">"
             "<resolution width=\"64\" height=\"64\"/><calibration class=\"adjusted\" type=\"frame\">"
             "<f>50</f></calibration></sensor></sensors><cameras>";
        for (int i = 0; i < 6; ++i)
            f << "<camera id=\"" << i << "\" label=\"view" << i << "\" sensor_id=\"0\">"
              << "<transform>1 0 0 " << (i - 2.5) * 0.1
              << " 0 1 0 0 0 0 1 0 0 0 0 1</transform></camera>";
        f << "</cameras></chunk></document>";
        return;
    }
    fs::create_directories(root / "sparse/0");
    const auto model = root / "sparse/0";
    if (!bin) {
        std::ofstream(model / "cameras.txt") << "1 PINHOLE 64 64 50 50 32 32\n";
        std::ofstream images(model / "images.txt"), points(model / "points3D.txt");
        for (int i = 0; i < 6; ++i)
            images << i + 1 << " 1 0 0 0 " << -(i - 2.5) * 0.1 << " 0 0 1 view" << i << ".png\n\n";
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                points << y * 8 + x + 1 << ' ' << (x - 3.5) * 0.08 << ' ' << (y - 3.5) * 0.08
                       << ' ' << 2.0 + 0.03 * ((x + y) % 3) << " 128 160 192 0\n";
    } else {
        std::ofstream cameras(model / "cameras.bin", std::ios::binary);
        binary<uint64_t>(cameras, 1); binary<int32_t>(cameras, 1); binary<int32_t>(cameras, 1);
        binary<uint64_t>(cameras, 64); binary<uint64_t>(cameras, 64);
        for (double v : {50., 50., 32., 32.}) binary(cameras, v);
        std::ofstream images(model / "images.bin", std::ios::binary);
        binary<uint64_t>(images, 6);
        for (int i = 0; i < 6; ++i) {
            binary<int32_t>(images, i + 1);
            for (double v : {1., 0., 0., 0., -(i - 2.5) * 0.1, 0., 0.}) binary(images, v);
            binary<int32_t>(images, 1);
            const std::string name = "view" + std::to_string(i) + ".png";
            images.write(name.c_str(), name.size() + 1);
            binary<uint64_t>(images, 0);
        }
        std::ofstream points(model / "points3D.bin", std::ios::binary);
        binary<uint64_t>(points, 64);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                binary<uint64_t>(points, y * 8 + x + 1);
                binary(points, (x - 3.5) * 0.08); binary(points, (y - 3.5) * 0.08);
                binary(points, 2.0 + 0.03 * ((x + y) % 3));
                for (uint8_t c : {128, 160, 192}) binary(points, c);
                binary(points, 0.0); binary<uint64_t>(points, 0);
            }
    }
}

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL %s\n", name); ++failures; }
    };
    auto near = [&](double a, double b) { return std::isfinite(a) && std::abs(a - b) < 1e-5; };
    try {
        const fs::path root = fs::current_path() / "parser-fixtures";
        test::write_scene(root / "nerfstudio");
        other_formats(root / "text", false, false);
        other_formats(root / "binary", true, false);
        other_formats(root / "metashape", false, true);
        ParsedDataset reference;
        for (const char* name : {"nerfstudio", "text", "binary", "metashape"}) {
            DatasetParserConfig cfg;
            const auto full = parse_dataset((root / name).u8string(), cfg, "");
            check(near(full.train_frame_scale, 0.25), "scale uses all six camera positions");
            for (float coefficient : full.dist_coeffs)
                check(near(coefficient, 0.0), "pinhole has no distortion");
            check(full.num_cameras == 6 && full.points.num() == 64, "canonical camera and seed counts");
            for (int i = 0; i < full.num_cameras; ++i) {
                check(fs::path(full.image_filenames[i]).filename() == "view" + std::to_string(i) + ".png",
                      "frame identity and ordering");
                const double expected[12] = {1, 0, 0, (i - 2.5) * 0.1, 0, -1, 0, 0, 0, 0, -1, 0};
                for (int k = 0; k < 12; ++k) check(near(full.c2w[i * 12 + k], expected[k]), "canonical camera pose");
                for (int k = 0; k < 4; ++k) check(near(full.intrins[i * 4 + k], k < 2 ? 50 : 32), "canonical intrinsics");
                check(full.widths[i] == 64 && full.heights[i] == 64, "image dimensions");
            }
            for (int i = 0; i < full.points.num(); ++i) {
                check(near(full.points.xyz[i * 3], (i % 8 - 3.5) * 0.08) &&
                      near(full.points.xyz[i * 3 + 1], (i / 8 - 3.5) * 0.08) &&
                      near(full.points.xyz[i * 3 + 2], 2.0 + 0.03 * ((i % 8 + i / 8) % 3)), "seed coordinates");
                check(full.points.rgb.at(i * 3) == 128 && full.points.rgb.at(i * 3 + 1) == 160 &&
                      full.points.rgb.at(i * 3 + 2) == 192, "seed colors");
            }
            if (std::string(name) == "nerfstudio") reference = full;
            else {
                check(full.camera_models == reference.camera_models && full.camera_distortions == reference.camera_distortions,
                      "equivalent camera models and distortion tiers");
                for (size_t i = 0; i < full.dist_coeffs.size(); ++i)
                    check(near(full.dist_coeffs[i], reference.dist_coeffs.at(i)), "equivalent distortion coefficients");
                check(near(full.train_frame_scale, reference.train_frame_scale), "equivalent train-frame scale");
            }
            for (const char* split : {"interval", "fraction"}) {
                cfg.eval_mode = split; cfg.eval_interval = 3; cfg.train_split_fraction = 0.5f;
                cfg.split = "train";
                const auto train = parse_dataset((root / name).u8string(), cfg, "");
                cfg.split = "eval";
                const auto eval = parse_dataset((root / name).u8string(), cfg, "");
                std::set<std::string> expected_train = std::string(split) == "interval"
                    ? std::set<std::string>{"view1.png", "view2.png", "view4.png", "view5.png"}
                    : std::set<std::string>{"view0.png", "view2.png", "view5.png"};
                std::set<std::string> train_names;
                for (const auto& image : train.image_filenames)
                    train_names.insert(fs::path(image).filename().u8string());
                check(train_names == expected_train, "configured split selects expected frames");
                std::set<std::string> frames(train.image_filenames.begin(), train.image_filenames.end());
                const auto train_size = frames.size();
                frames.insert(eval.image_filenames.begin(), eval.image_filenames.end());
                check(frames.size() == 6 && frames.size() == train_size + eval.image_filenames.size(), "train/eval partition all source frames");
                check(train.train_to_normalized == eval.train_to_normalized &&
                      near(train.train_frame_scale, eval.train_frame_scale), "normalization is computed before splitting");
            }
            std::printf("Checked %s scene and both split modes\n", name);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL parser fixture: %s\n", e.what()); ++failures;
    }
    return failures ? 1 : 0;
}
