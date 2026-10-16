#include "app/tests/SceneFixture.h"
#include "data/DataManager.h"
#include "checkpoint/B1Contract.h"

#include <algorithm>
#include <cstdio>
#include <memory>

namespace fs = std::filesystem;
using StepIds = std::vector<std::vector<int32_t>>;
constexpr uint64_t seed = ckpt::b1_sampler_seed;

std::unique_ptr<DataManager> manager(const fs::path& root, CacheMode mode,
                                   int batch, uint64_t offset, bool val,
                                   int prefetch = 2, int workers = 1,
                                   bool full_faces = false) {
    DataManagerConfig cfg;
    cfg.cache_mode = mode;
    cfg.seed = seed;
    cfg.independent_train_rng = true;
    cfg.initial_train_step = offset;
    cfg.train_batch_size = batch;
    cfg.load_masks = cfg.load_depths = cfg.load_normals = false;
    cfg.prefetch_batches = prefetch;
    cfg.workers_rgb = workers;
    std::vector<std::string> images;
    for (int i = 0; i < 6; ++i)
        images.push_back((root / "images" / ("view" + std::to_string(i) + ".png")).u8string());
    const std::vector<int32_t> all{0, 1, 2, 3, 4, 5};
    const std::vector<int32_t> training = full_faces ? std::vector<int32_t>{0, 1, 2, 3, 4} : all;
    const std::vector<int32_t> validation = full_faces ? std::vector<int32_t>{5} : all;
    const std::vector<int32_t> factors = full_faces
        ? std::vector<int32_t>{1, 1, 2, 1, 1, 1} : std::vector<int32_t>{};
    const std::vector<int32_t> offsets = full_faces
        ? std::vector<int32_t>{0, 1, 2, 4, 5, 6} : std::vector<int32_t>{};
    const int post_count = full_faces ? 7 : 6;
    return std::make_unique<DataManager>(cfg,
        std::vector<int32_t>(6, (int32_t)CameraModelType::PINHOLE),
        std::vector<int32_t>(6, 0), images,
        std::vector<std::string>{}, std::vector<std::string>{}, std::vector<std::string>{},
        std::vector<int32_t>{64, 64, 64, 32, 24, 16}, std::vector<int32_t>(6, 64),
        factors, offsets,
        std::vector<float>(post_count * 16), std::vector<float>(post_count * 4),
        std::vector<float>(post_count * 8),
        full_faces ? std::vector<int32_t>{64, 64, 32, 16, 32, 24, 16} : std::vector<int32_t>{},
        full_faces ? std::vector<int32_t>(7, 64) : std::vector<int32_t>{},
        full_faces ? std::vector<float>(7 * 9) : std::vector<float>{},
        std::vector<float>{}, std::vector<float>{}, std::vector<int32_t>{},
        std::vector<float>{}, training, val ? validation : std::vector<int32_t>{});
}

StepIds ids(const TrainStep& step) {
    StepIds result;
    for (const auto& sub : step.subs) result.push_back(sub->indices);
    return result;
}

StepIds schedule(const TrainStep& step) {
    StepIds result;
    for (const auto& sub : step.subs) {
        result.push_back(sub->indices);
        std::vector<int32_t> faces{sub->width, sub->height, sub->num};
        faces.insert(faces.end(), sub->post_offsets.begin(), sub->post_offsets.end());
        for (const auto& pass : sub->face_passes) {
            faces.push_back(pass.k0);
            faces.push_back(pass.k1);
            faces.push_back(pass.width);
            faces.push_back(pass.height);
        }
        result.push_back(std::move(faces));
    }
    return result;
}

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL %s\n", name); ++failures; }
    };
    const auto root = fs::current_path() / "sampler-fixture";
    try {
        test::write_scene(root);
        for (int i = 3; i < 6; ++i) {
            const int width = i == 3 ? 32 : i == 4 ? 24 : 16;
            std::vector<unsigned char> pixels(width * 64 * 3, (unsigned char)i);
            if (!stbi_write_png((root / "images" / ("view" + std::to_string(i) + ".png")).u8string().c_str(),
                                width, 64, 3, pixels.data(), width * 3))
                throw std::runtime_error("cannot write sampler fixture");
        }
        for (int batch : {1, 3}) {
            const int epoch_steps = batch == 1 ? 6 : 2;
            auto uninterrupted = manager(root, CacheMode::CPU, batch, 0, false);
            std::vector<StepIds> expected;
            for (int i = 0; i < 32; ++i) expected.push_back(ids(uninterrupted->next_train_step()));
            for (int epoch = 0; epoch < 4; ++epoch) {
                std::vector<int32_t> seen;
                bool heterogeneous = false;
                for (int step = 0; step < epoch_steps; ++step) {
                    const auto& scheduled = expected[epoch * epoch_steps + step];
                    heterogeneous |= scheduled.size() > 1;
                    for (const auto& sub : scheduled) seen.insert(seen.end(), sub.begin(), sub.end());
                }
                std::sort(seen.begin(), seen.end());
                check(seen == std::vector<int32_t>({0, 1, 2, 3, 4, 5}), "each epoch covers every input once");
                check(batch == 1 || heterogeneous, "native cross-group remainder packing");
            }
            for (uint64_t offset : {0ULL, 1ULL, 2ULL, 5ULL, 6ULL, 7ULL, 12ULL, 19ULL}) {
                for (CacheMode mode : {CacheMode::CPU, CacheMode::DISK}) {
                    for (int prefetch : {1, 4}) {
                        auto resumed = manager(root, mode, batch, offset, true, prefetch, prefetch);
                        for (int step = 0; step < 8; ++step) {
                            resumed->next_val_batch();
                            if (mode == CacheMode::CPU && step % 2) resumed->next_val_batch();
                            check(ids(resumed->next_train_step()) == expected[offset + step],
                                  "resume matches uninterrupted optimizer sequence across epochs");
                        }
                    }
                }
            }
        }
        {
            auto native = manager(root, CacheMode::CPU, 3, 0, true, 2, 1, true);
            std::vector<StepIds> expected;
            for (int epoch = 0; epoch < 4; ++epoch) {
                std::vector<int32_t> seen;
                bool split_faces = false, packed = false;
                for (int step = 0; step < 3; ++step) {
                    const auto& actual = native->next_train_step();
                    expected.push_back(schedule(actual));
                    packed |= actual.subs.size() > 1;
                    for (const auto& sub : actual.subs) {
                        seen.insert(seen.end(), sub->indices.begin(), sub->indices.end());
                        if (sub->indices == std::vector<int32_t>{2}) {
                            const auto& passes = sub->face_passes;
                            split_faces |= passes.size() == 2 &&
                                passes[0].k0 == 0 && passes[0].k1 == 1 &&
                                passes[0].width == 32 && passes[0].height == 64 &&
                                passes[1].k0 == 1 && passes[1].k1 == 2 &&
                                passes[1].width == 16 && passes[1].height == 64;
                        }
                    }
                }
                std::sort(seen.begin(), seen.end());
                check(seen == std::vector<int32_t>({0, 1, 2, 3, 4}),
                      "full B1 epoch covers parser training IDs and excludes held-out camera");
                check(split_faces && packed, "full B1 preserves split faces and cross-group subbatches");
            }
            for (uint64_t offset : {1ULL, 2ULL, 3ULL, 4ULL, 6ULL}) {
                auto resumed = manager(root, CacheMode::DISK, 3, offset, true, 2, 1, true);
                for (int step = 0; step < 5; ++step) {
                    resumed->next_val_batch();
                    check(schedule(resumed->next_train_step()) == expected[offset + step],
                          "full B1 resumed input face subbatch schedule matches across epochs");
                }
            }
        }
        auto oracle = manager(root, CacheMode::CPU, 1, 0, false);
        const auto first = ids(oracle->next_train_step());
        const auto second = ids(oracle->next_train_step());
        const auto missing = root / "images" / ("view" + std::to_string(first[0][0]) + ".png");
        const auto parked = root / "parked.png";
        fs::rename(missing, parked);
        {
            auto skipped = manager(root, CacheMode::DISK, 1, 1, false, 1, 1);
            check(ids(skipped->next_train_step()) == second,
                  "offset skips missing discarded image without decoding it");
        }
        {
            auto disk = manager(root, CacheMode::DISK, 1, 0, false);
            bool failed = false;
            try { disk->next_train_step(); }
            catch (const DataDecodeError&) { failed = true; }
            check(failed, "ordered decode failure is reported");
            fs::rename(parked, missing);
            disk->resolve_data_error(true);
            check(disk->data_error().empty(), "retry clears active fault before pending decode completes");
            check(ids(disk->next_train_step()) == first, "decode retry preserves the pending schedule step");
            check(ids(disk->next_train_step()) == second, "retry consumes exactly one scheduled step");
        }
    } catch (const std::exception& e) {
        std::printf("FAIL exception: %s\n", e.what());
        ++failures;
    }
    fs::remove_all(root);
    return failures ? 1 : 0;
}
