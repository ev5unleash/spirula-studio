// What a re-run may pick up, and what it must not (D76).
//
// The two halves that decide correctness: a stage signature moves exactly when
// a flag that stage reads moves, and the verification journal reads back what
// it wrote -- including from a tail its writer never flushed, which is how
// every killed run leaves it.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "sfm/SfmConfig.h"
#include "sfm/core/Features.h"
#include "sfm/core/Resume.h"
#include "sfm/tests/TestMain.h"

namespace fs = std::filesystem;
using namespace sfm;

template <typename F> static bool runtimeError(F&& fn) {
    try {
        fn();
    } catch (const std::runtime_error&) {
        return true;
    } catch (...) {
    }
    return false;
}

static void putU32(std::vector<char>& bytes, uint32_t value) {
    for (int i = 0; i < 4; i++) bytes.push_back((char)(value >> (8 * i)));
}

static void writeRaw(const fs::path& path, const std::vector<char>& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), (std::streamsize)bytes.size());
}

static void patchU32(const fs::path& path, uintmax_t offset, uint32_t value) {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp((std::streamoff)offset);
    f.write((const char*)&value, 4);
}


static void check(bool condition, const char* message, int& fails) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    fails++;
}

static fs::path tempDir() {
    const fs::path d = fs::temp_directory_path() / "spirula_sfm_resume_selftest";
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

// ---------------------------------------------------------------------------
// Signatures
// ---------------------------------------------------------------------------

static void testSignatures(int& fails) {
    SfmConfig base;
    base.finalize(CMD_AUTO);

    const std::string ext0 = stageSignature(base, CMD_EXTRACT);
    const std::string mat0 = stageSignature(base, CMD_MATCH);

    // A flag the stage reads moves its signature.
    {
        SfmConfig c = base;
        c.sift.max_num_features = base.sift.max_num_features / 2;
        c.finalize(CMD_AUTO);
        check(stageSignature(c, CMD_EXTRACT) != ext0, "--max-features moves extraction", fails);
    }
    {
        SfmConfig c = base;
        c.twoview.min_num_inliers = base.twoview.min_num_inliers + 5;
        c.finalize(CMD_AUTO);
        check(stageSignature(c, CMD_MATCH) != mat0, "--min-inliers moves matching", fails);
        check(stageSignature(c, CMD_EXTRACT) == ext0, "--min-inliers leaves extraction", fails);
    }
    // A per-group lens is no table row and still reaches the camera setup.
    {
        SfmConfig c = base;
        parseCameraOverride("cam0=opencv-fisheye", OverrideKind::Model, c.camera.overrides);
        check(stageSignature(c, CMD_MATCH) != mat0, "a per-group lens moves matching", fails);
    }
    // How fast the stage runs is not what it produces.
    {
        SfmConfig c = base;
        c.threads = 3;
        c.decode_threads = 3;
        c.device = 1;
        c.quiet = true;
        c.finalize(CMD_AUTO);
        check(stageSignature(c, CMD_EXTRACT) == ext0, "runtime flags leave extraction", fails);
        check(stageSignature(c, CMD_MATCH) == mat0, "runtime flags leave matching", fails);
    }
    // Mapping-only flags leave both earlier stages alone, which is the whole
    // point: re-running the mapper must not cost the matching.
    {
        SfmConfig c = base;
        c.mapper.min_tri_angle_deg = 2.0;
        c.mapper_mode = "bottom-up";
        c.finalize(CMD_AUTO);
        check(stageSignature(c, CMD_EXTRACT) == ext0, "mapper flags leave extraction", fails);
        check(stageSignature(c, CMD_MATCH) == mat0, "mapper flags leave matching", fails);
    }
}

// ---------------------------------------------------------------------------
// The journal
// ---------------------------------------------------------------------------

static resume::PairDependency dependency(
    uint32_t image1, uint32_t image2, const char* key1, const char* digest1,
    const char* key2, const char* digest2, const char* name1, const char* name2,
    const char* recipe) {
    resume::PairDependency value;
    value.image1 = image1;
    value.image2 = image2;
    value.image_name1 = name1;
    value.image_name2 = name2;
    value.feature_key1 = key1;
    value.feature_digest1 = digest1;
    value.feature_key2 = key2;
    value.feature_digest2 = digest2;
    value.recipe_digest = recipe;
    value.dependency_key = resume::pairDependencyKey(value);
    return value;
}

static void testJournal(const fs::path& dir, int& fails) {
    const fs::path file = dir / "matches.part";
    const std::string sig = "extract=1\nmatch=2\n";
    const auto ab = dependency(0, 1, "A", "ad", "B", "bd", "old-a", "old-b", "recipe");
    const auto ac = dependency(0, 2, "A", "ad", "C", "cd", "old-a", "old-c", "recipe");
    const auto bc = dependency(1, 2, "B", "bd", "C", "cd", "old-b", "old-c", "recipe");
    const std::vector<resume::PairDependency> old_dependencies = {ab, ac, bc};

    const uint32_t idx1[] = {1, 4, 9}, idx2[] = {2, 5, 7};
    {
        resume::MatchJournal j;
        check(j.open(file, sig, false, old_dependencies), "journal opens", fails);
        j.record(0, 1, 3, 40, idx1, idx2, 4, 3);
        j.record(0, 2, 0, 12, nullptr, nullptr, 4, 0);
        j.record(1, 2, 2, 30, idx1, idx2, 4, 2);
        j.close();
    }

    const std::vector<ImageEntry> current_images = {
        {"renamed-b", 100}, {"renamed-c", 100}, {"renamed-a", 100}, {"added-d", 100}};
    const auto remapped_ab =
        dependency(2, 0, "A", "ad", "B", "bd", "renamed-a", "renamed-b", "recipe");
    const auto remapped_ac =
        dependency(2, 1, "A", "ad", "C", "cd", "renamed-a", "renamed-c", "recipe");
    const auto remapped_bc =
        dependency(0, 1, "B", "bd", "C", "cd", "renamed-b", "renamed-c", "recipe");
    const auto added_ad =
        dependency(2, 3, "A", "ad", "D", "dd", "renamed-a", "added-d", "recipe");
    const std::vector<resume::PairDependency> remapped = {
        remapped_ab, remapped_ac, remapped_bc, added_ad};

    std::unordered_map<uint64_t, TwoViewMatches> kept;
    std::vector<uint64_t> done;
    uint64_t putative = 0;
    check(resume::readJournal(file, sig, remapped, current_images, kept, done, putative),
          "journal reads after rename and addition", fails);
    check(done.size() == 3 && kept.size() == 2, "unaffected pairs survive remapping", fails);
    check(putative == 82, "the putative count survives", fails);
    const auto it = kept.find(resume::pairKey(2, 0));
    check(it != kept.end() && it->second.config == 3 &&
              it->second.matches[2].idx1 == 9 && it->second.matches[2].idx2 == 7,
          "a pair remaps to current indices", fails);
    check(kept.count(resume::pairKey(2, 1)) == 0,
          "a refused pair keeps no matches", fails);
    check(kept.count(resume::pairKey(2, 3)) == 0, "an added pair has no stale result", fails);

    std::unordered_map<uint64_t, TwoViewMatches> other_kept;
    std::vector<uint64_t> other_done;
    uint64_t other_putative = 0;
    check(!resume::readJournal(file, sig + "matcher-changed", remapped, current_images,
                               other_kept, other_done, other_putative),
          "a matcher setting mismatch refuses the journal", fails);

    const auto changed_ac =
        dependency(2, 1, "A", "ad", "C2", "cd2", "renamed-a", "renamed-c", "recipe");
    const auto changed_bc =
        dependency(0, 1, "B", "bd", "C2", "cd2", "renamed-b", "renamed-c", "recipe");
    const std::vector<resume::PairDependency> incident = {
        remapped_ab, changed_ac, changed_bc, added_ad};
    kept.clear();
    done.clear();
    putative = 0;
    check(resume::readJournal(file, sig, incident, current_images, kept, done, putative) &&
              done.size() == 1 && kept.size() == 1 &&
              kept.count(resume::pairKey(2, 0)) == 1,
          "changing one feature invalidates only incident pairs", fails);

    // A tail a killed writer never finished: everything before it survives.
    {
        const uintmax_t whole = fs::file_size(file);
        std::vector<char> bytes(whole);
        std::ifstream(file, std::ios::binary).read(bytes.data(), (std::streamsize)whole);
        std::ofstream torn(file, std::ios::binary | std::ios::trunc);
        torn.write(bytes.data(), (std::streamsize)whole - 6);
    }
    kept.clear();
    done.clear();
    putative = 0;
    check(resume::readJournal(file, sig, remapped, current_images, kept, done, putative),
          "a torn journal reads", fails);
    check(done.size() == 2 && kept.size() == 1, "an incomplete tail is dropped", fails);

    {
        resume::MatchJournal j;
        check(j.open(file, sig, true, old_dependencies), "journal reopens", fails);
        j.record(1, 2, 2, 30, idx1, idx2, 4, 2);
        j.close();
    }
    kept.clear();
    done.clear();
    putative = 0;
    check(resume::readJournal(file, sig, remapped, current_images, kept, done, putative) &&
              done.size() == 3 && kept.size() == 2,
          "appending keeps what was there", fails);

    const fs::path tampered = dir / "tampered.part";
    {
        std::vector<char> bytes((size_t)fs::file_size(file));
        std::ifstream(file, std::ios::binary).read(bytes.data(), (std::streamsize)bytes.size());
        bytes.back() ^= 1;
        writeRaw(tampered, bytes);
    }
    kept.clear();
    done.clear();
    putative = 0;
    check(resume::readJournal(tampered, sig, remapped, current_images, kept, done, putative) &&
              done.size() == 2 && kept.size() == 1,
          "a tampered pair payload is refused", fails);

    const fs::path legacy = dir / "legacy.part";
    {
        std::vector<char> bytes((size_t)fs::file_size(file));
        std::ifstream(file, std::ios::binary).read(bytes.data(), (std::streamsize)bytes.size());
        bytes[0] = 'V';
        bytes[1] = 'K';
        bytes[2] = 'R';
        bytes[3] = 'J';
        writeRaw(legacy, bytes);
    }
    check(!resume::readJournal(legacy, sig, remapped, current_images, kept, done, putative),
          "legacy journal records are rejected", fails);
}

// ---------------------------------------------------------------------------
// The pair list and the feature-file probe
// ---------------------------------------------------------------------------

static void testPairsAndFeatures(const fs::path& dir, int& fails) {
    const fs::path file = dir / "pairs.bin";
    const std::vector<std::pair<uint32_t, uint32_t>> pairs = {{0, 1}, {0, 2}, {1, 2}};
    resume::writePairs(file, "sig", pairs);
    std::vector<std::pair<uint32_t, uint32_t>> back;
    check(resume::readPairs(file, "sig", back) && back == pairs, "the pair list round-trips",
          fails);
    back.clear();
    check(!resume::readPairs(file, "other", back) && back.empty(),
          "a signature mismatch refuses the pair list", fails);

    // peekFeatures is what says a feature file may be reused without reading
    // the descriptors back, so a truncated one has to fail it.
    FeatureSet fs_;
    fs_.width = fs_.extract_width = 640;
    fs_.height = fs_.extract_height = 480;
    fs_.dim = 8;
    fs_.dtype = DType::U8;
    fs_.keypoints.resize(50);
    fs_.descriptors.assign(50 * 8, 7);
    const fs::path feat = dir / "one.bin";
    writeFeatures(feat.string(), fs_);
    uint32_t count = 0;
    check(peekFeatures(feat.string(), count) && count == 50, "a whole feature file probes", fails);
    check(!fs::exists(feat.string() + ".part"), "the write leaves no part file", fails);
    {
        const uintmax_t whole = fs::file_size(feat);
        std::vector<char> bytes(whole);
        std::ifstream(feat, std::ios::binary).read(bytes.data(), (std::streamsize)whole);
        std::ofstream torn(feat, std::ios::binary | std::ios::trunc);
        torn.write(bytes.data(), (std::streamsize)whole - 100);
    }
    count = 0;
    check(!peekFeatures(feat.string(), count), "a truncated feature file is refused", fails);
}

static void testMatches(const fs::path& dir, int& fails) {
    MatchesDatabase db;
    db.images = {{"a", 3}, {"b", 4}};
    db.pairs = {{0, 1, 3, {{1, 2, 0}}}};
    db.cameras = {Camera::defaultFor(7, 640, 480, 400, CamModel::Radial)};
    db.camera_ids = {7, 7};
    db.focal_prior = {1};
    db.focal_measured = {1};
    const fs::path file = dir / "matches.bin";
    writeMatches(file.string(), db);
    const MatchesDatabase back = readMatches(file.string());
    check(back.images.size() == 2 && back.pairs.size() == 1 &&
              back.pairs[0].matches[0].idx1 == 1 && back.focal_measured == db.focal_measured,
          "matches round-trip", fails);
    check(!runtimeError([&] { writeMatches(file.string(), db); }),
          "identical matches reuse", fails);

    MatchesDatabase conflict = db;
    conflict.pairs[0].config = 4;
    check(runtimeError([&] { writeMatches(file.string(), conflict); }),
          "conflicting matches do not overwrite", fails);
    check(readMatches(file.string()).pairs[0].config == 3, "conflict leaves destination", fails);

    MatchesDatabase bad = db;
    bad.images[0].name = std::string("a\0x", 3);
    check(runtimeError([&] { writeMatches((dir / "nul.bin").string(), bad); }),
          "NUL image name is rejected", fails);
    bad = db;
    bad.pairs[0].matches[0].idx1 = 3;
    check(runtimeError([&] { writeMatches((dir / "feature.bin").string(), bad); }),
          "out-of-range feature index is rejected", fails);
    bad = db;
    bad.pairs[0].image1 = 1;
    bad.pairs[0].image2 = 0;
    check(runtimeError([&] { writeMatches((dir / "reordered-write.bin").string(), bad); }),
          "reordered writer endpoints are rejected", fails);
    bad = db;
    bad.pairs.push_back(bad.pairs[0]);
    check(runtimeError([&] { writeMatches((dir / "duplicate.bin").string(), bad); }),
          "duplicate image pair is rejected", fails);
    bad = db;
    bad.camera_ids[0] = 8;
    check(runtimeError([&] { writeMatches((dir / "camera-id.bin").string(), bad); }),
          "unknown camera id is rejected", fails);
    bad = db;
    bad.cameras[0].fx = std::numeric_limits<double>::quiet_NaN();
    check(runtimeError([&] { writeMatches((dir / "nan-camera.bin").string(), bad); }),
          "non-finite camera is rejected", fails);

    const fs::path reordered = dir / "reordered.bin";
    fs::copy_file(file, reordered);
    patchU32(reordered, 34, 1);
    patchU32(reordered, 38, 0);
    check(runtimeError([&] { readMatches(reordered.string()); }),
          "reordered pair endpoints are rejected", fails);

    const fs::path truncated = dir / "truncated.bin";
    fs::copy_file(file, truncated);
    fs::resize_file(truncated, fs::file_size(truncated) - 1);
    check(runtimeError([&] { readMatches(truncated.string()); }),
          "incomplete measured section is rejected", fails);

    const fs::path trailing = dir / "trailing.bin";
    fs::copy_file(file, trailing);
    {
        std::ofstream f(trailing, std::ios::binary | std::ios::app);
        f.put('x');
    }
    check(runtimeError([&] { readMatches(trailing.string()); }),
          "trailing bytes are rejected", fails);

    const fs::path old = dir / "v2.bin";
    fs::copy_file(file, old);
    fs::resize_file(old, 58);
    patchU32(old, 4, 2);
    check(readMatches(old.string()).pairs.size() == 1, "v2 matches remain readable", fails);

    const fs::path oldCamera = dir / "v3.bin";
    fs::copy_file(file, oldCamera);
    fs::resize_file(oldCamera, fs::file_size(oldCamera) - 1);
    patchU32(oldCamera, 4, 3);
    check(readMatches(oldCamera.string()).focal_measured.empty(),
          "v3 camera metadata remains readable", fails);

    std::vector<char> overflow = {'V', 'K', 'M', 'T'};
    putU32(overflow, 4);
    putU32(overflow, std::numeric_limits<uint32_t>::max());
    const fs::path overflowFile = dir / "overflow.bin";
    writeRaw(overflowFile, overflow);
    check(runtimeError([&] { readMatches(overflowFile.string()); }),
          "overflow count is rejected before allocation", fails);

    std::vector<char> unknown = {'V', 'K', 'M', 'T'};
    putU32(unknown, 1);
    putU32(unknown, 0);
    const fs::path unknownFile = dir / "unknown.bin";
    writeRaw(unknownFile, unknown);
    check(runtimeError([&] { readMatches(unknownFile.string()); }),
          "unknown VKMT version is rejected", fails);
}

static int cmdResumeTest(int, char**) {
    int fails = 0;
    const fs::path dir = tempDir();
    testSignatures(fails);
    testJournal(dir, fails);
    testPairsAndFeatures(dir, fails);
    std::error_code ec;
    testMatches(dir, fails);
    fs::remove_all(dir, ec);
    std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}

int main(int argc, char** argv) { return sfmTestMain(argc, argv, cmdResumeTest); }
