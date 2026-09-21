// Feature matches and their flat-file interchange format.
//
// A match is a pair of feature indices into two images' FeatureSets. A
// MatchesDatabase collects the two-view match lists for a whole dataset (the
// input to phase-3 verification and the phase-4 correspondence graph). Like
// features.bin this is our own format (D4), self-describing and trivial to
// parse; it is not COLMAP's SQLite database.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "sfm/core/Camera.h"

namespace sfm {

// One putative correspondence: feature idx1 in image1 <-> idx2 in image2.
struct FeatureMatch {
    uint32_t idx1 = 0, idx2 = 0;
    float distance = 0;   // L2 descriptor distance (not persisted)
};

// All matches between one ordered image pair. When geometric verification has
// run, `matches` holds only the inliers and `config` is the two-view config
// (0 = not verified / raw; otherwise the sfm/geometry/TwoView.h TwoViewConfig).
struct TwoViewMatches {
    uint32_t image1 = 0, image2 = 0;      // indices into MatchesDatabase::images
    int32_t config = 0;                   // 0 = unverified
    std::vector<FeatureMatch> matches;
};

// One image's identity in the match database.
struct ImageEntry {
    std::string name;         // feature-file stem (== image name)
    uint32_t num_features = 0;
};

struct MatchesDatabase {
    std::vector<ImageEntry> images;
    std::vector<TwoViewMatches> pairs;
    // The camera setup verification actually used (D47): which images share
    // intrinsics, what those intrinsics were, and whether the focal was a prior
    // or a guess. Recorded so the mapper does not have to reconstruct it -- its
    // only other option is to re-search the focal on the *verified inliers*,
    // which are biased towards whatever focal produced them. Empty in a file
    // written before this, or by `--no-verify`.
    std::vector<Camera> cameras;         // one per distinct camera id
    std::vector<uint32_t> camera_ids;    // per image, parallel to `images`
    std::vector<uint8_t> focal_prior;    // parallel to `cameras`; 1 = not a guess
    // 1 where THIS stage measured the focal rather than being told it. Kept
    // apart from `focal_prior` because the mapper treats the two differently:
    // it re-refines a measured focal and leaves a given one alone (D45).
    std::vector<uint8_t> focal_measured;
    bool hasCameras() const {
        return !cameras.empty() && camera_ids.size() == images.size();
    }
};

// Verified inlier evidence grouped by canonical image-group pair.
struct GroupOverlap {
    std::string group1, group2;
    uint64_t verified_pairs = 0;
    uint64_t verified_inliers = 0;
};

inline std::vector<GroupOverlap> verifiedGroupOverlap(
    const MatchesDatabase& db, const std::vector<std::string>& image_groups) {
    if (image_groups.size() != db.images.size())
        throw std::invalid_argument("image-group vector size does not match images");
    for (const std::string& group : image_groups)
        if (group.empty()) throw std::invalid_argument("image group is empty");

    std::vector<GroupOverlap> overlaps;
    const uint64_t max = std::numeric_limits<uint64_t>::max();
    for (const TwoViewMatches& pair : db.pairs) {
        if (pair.image1 >= db.images.size() || pair.image2 >= db.images.size())
            throw std::out_of_range("match pair endpoint out of range");
        if (pair.config == 0 || pair.matches.empty()) continue;

        const std::string* group1 = &image_groups[pair.image1];
        const std::string* group2 = &image_groups[pair.image2];
        if (*group1 == *group2) continue;
        if (*group2 < *group1) std::swap(group1, group2);
        const uint64_t inliers = (uint64_t)pair.matches.size();
        auto found = std::find_if(
            overlaps.begin(), overlaps.end(), [&](const GroupOverlap& overlap) {
                return overlap.group1 == *group1 && overlap.group2 == *group2;
            });
        if (found == overlaps.end()) {
            overlaps.push_back({*group1, *group2, 1, inliers});
            continue;
        }
        if (found->verified_pairs == max ||
            inliers > max - found->verified_inliers)
            throw std::overflow_error("verified overlap count overflow");
        found->verified_pairs++;
        found->verified_inliers += inliers;
    }
    std::sort(overlaps.begin(), overlaps.end(), [](const GroupOverlap& a,
                                                   const GroupOverlap& b) {
        if (a.group1 != b.group1) return a.group1 < b.group1;
        return a.group2 < b.group2;
    });
    return overlaps;
}

namespace matches_detail {

constexpr uint32_t kOldestVersion = 2;
constexpr uint32_t kCurrentVersion = 4;
constexpr uint64_t kMaxFileBytes = 4ull << 30;
constexpr uint64_t kMaxAllocation = 1ull << 30;

[[noreturn]] inline void bad(const std::string& path, const char* why) {
    throw std::runtime_error("invalid VKMT " + path + ": " + why);
}

inline uint64_t add(uint64_t a, uint64_t b, const std::string& path) {
    if (b > std::numeric_limits<uint64_t>::max() - a) bad(path, "size overflow");
    return a + b;
}

inline uint64_t mul(uint64_t a, uint64_t b, const std::string& path) {
    if (a && b > std::numeric_limits<uint64_t>::max() / a) bad(path, "size overflow");
    return a * b;
}

struct Budget {
    uint64_t left = kMaxAllocation;
    void take(uint64_t bytes, const std::string& path) {
        if (bytes > left) bad(path, "allocation bound exceeded");
        left -= bytes;
    }
};

inline size_t checkedSize(uint64_t value, const std::string& path) {
    if (value > (uint64_t)std::numeric_limits<size_t>::max())
        bad(path, "allocation size overflow");
    return (size_t)value;
}

struct Reader {
    std::ifstream file;
    std::string path;
    uint64_t size = 0;
    uint64_t offset = 0;

    explicit Reader(const std::string& p) : file(p, std::ios::binary), path(p) {
        if (!file) throw std::runtime_error("cannot read " + p);
        std::error_code ec;
        size = (uint64_t)std::filesystem::file_size(p, ec);
        if (ec) throw std::runtime_error("cannot stat " + p);
        if (size > kMaxFileBytes) bad(path, "file-size bound exceeded");
    }

    uint64_t remaining() const { return size - offset; }

    void read(void* dst, uint64_t bytes) {
        if (bytes > remaining()) bad(path, "truncated");
        if (bytes > (uint64_t)std::numeric_limits<std::streamsize>::max())
            bad(path, "stream-size overflow");
        if (bytes) file.read((char*)dst, (std::streamsize)bytes);
        if (!file) bad(path, "truncated");
        offset += bytes;
    }

    template <typename T> T value() {
        T out{};
        read(&out, sizeof out);
        return out;
    }
};

inline uint32_t count32(size_t value, const std::string& path, const char* what) {
    if (value > std::numeric_limits<uint32_t>::max()) bad(path, what);
    return (uint32_t)value;
}

inline void finite(double value, const std::string& path, const char* what) {
    if (!std::isfinite(value)) bad(path, what);
}

inline uint32_t cameraParams(const Camera& cam, const std::string& path) {
    int np = 0;
    try {
        (void)camColmapId(cam.model);
        np = camColmapParams(cam.model);
    } catch (...) {
        bad(path, "unsupported camera model");
    }
    if (np < 0 || np > 16) bad(path, "invalid camera parameter count");
    return (uint32_t)np;
}

inline void validateCamera(const Camera& cam, const std::string& path) {
    const uint32_t np = cameraParams(cam, path);
    if (cam.width <= 0 || cam.height <= 0) bad(path, "invalid camera dimensions");
    if (!std::isfinite(cam.pixel_scale) || cam.pixel_scale <= 0)
        bad(path, "invalid camera pixel scale");
    const double values[] = {cam.fx, cam.fy, cam.cx, cam.cy, cam.k1, cam.k2, cam.p1,
                             cam.p2, cam.k3, cam.k4, cam.k5, cam.k6, cam.sx1, cam.sy1};
    for (double value : values) finite(value, path, "non-finite camera value");
    double params[16] = {};
    try {
        packColmap(cam, params);
    } catch (...) {
        bad(path, "unsupported camera model");
    }
    for (uint32_t i = 0; i < np; i++) finite(params[i], path, "non-finite camera parameter");
}

inline uint64_t pairKey(uint32_t image1, uint32_t image2) {
    return ((uint64_t)image1 << 32) | image2;
}

inline void validateDatabase(const MatchesDatabase& db, const std::string& path) {
    const uint32_t nimg = count32(db.images.size(), path, "image count overflow");
    count32(db.pairs.size(), path, "pair count overflow");
    for (const ImageEntry& image : db.images) {
        count32(image.name.size(), path, "image name too long");
        if (image.name.empty() || image.name.find('\0') != std::string::npos)
            bad(path, "invalid image name");
    }

    std::vector<uint64_t> keys;
    keys.reserve(db.pairs.size());
    for (const TwoViewMatches& pair : db.pairs) {
        if (pair.image1 >= nimg || pair.image2 >= nimg)
            bad(path, "pair endpoint out of range");
        if (pair.image1 >= pair.image2)
            bad(path, "pair endpoints are not an ordered unordered pair");
        count32(pair.matches.size(), path, "match count overflow");
        for (const FeatureMatch& match : pair.matches) {
            if (match.idx1 >= db.images[pair.image1].num_features ||
                match.idx2 >= db.images[pair.image2].num_features)
                bad(path, "feature index out of range");
        }
        keys.push_back(pairKey(pair.image1, pair.image2));
    }
    std::sort(keys.begin(), keys.end());
    for (size_t i = 1; i < keys.size(); i++)
        if (keys[i] == keys[i - 1]) bad(path, "duplicate image pair");

    if (db.cameras.empty()) {
        if (!db.camera_ids.empty() || !db.focal_prior.empty() || !db.focal_measured.empty())
            bad(path, "camera metadata shape mismatch");
        return;
    }
    if (db.camera_ids.size() != db.images.size())
        bad(path, "camera id count does not match images");
    if (!db.focal_prior.empty() && db.focal_prior.size() != db.cameras.size())
        bad(path, "focal-prior vector shape mismatch");
    if (!db.focal_measured.empty() && db.focal_measured.size() != db.cameras.size())
        bad(path, "focal-measured vector shape mismatch");
    std::vector<uint32_t> cameraIds;
    cameraIds.reserve(db.cameras.size());
    for (size_t i = 0; i < db.cameras.size(); i++) {
        validateCamera(db.cameras[i], path);
        cameraIds.push_back(db.cameras[i].id);
        if (!db.focal_prior.empty() && db.focal_prior[i] > 1)
            bad(path, "invalid focal-prior flag");
        if (!db.focal_measured.empty() && db.focal_measured[i] > 1)
            bad(path, "invalid focal-measured flag");
    }
    std::sort(cameraIds.begin(), cameraIds.end());
    for (size_t i = 1; i < cameraIds.size(); i++)
        if (cameraIds[i] == cameraIds[i - 1]) bad(path, "duplicate camera id");
    for (uint32_t id : db.camera_ids)
        if (!std::binary_search(cameraIds.begin(), cameraIds.end(), id))
            bad(path, "camera id has no camera record");
}

inline uint64_t encodedSize(const MatchesDatabase& db, const std::string& path) {
    uint64_t bytes = 12;
    for (const ImageEntry& image : db.images)
        bytes = add(bytes, add(8, image.name.size(), path), path);
    bytes = add(bytes, 4, path);
    for (const TwoViewMatches& pair : db.pairs)
        bytes = add(bytes, add(16, mul(pair.matches.size(), 8, path), path), path);
    bytes = add(bytes, 4, path);
    for (const Camera& cam : db.cameras) {
        const uint32_t np = cameraParams(cam, path);
        bytes = add(bytes, add(29, mul(np, 8, path), path), path);
    }
    if (!db.cameras.empty()) {
        bytes = add(bytes, add(4, mul(db.camera_ids.size(), 4, path), path), path);
        bytes = add(bytes, db.cameras.size(), path);
    }
    if (bytes > kMaxFileBytes) bad(path, "file-size bound exceeded");
    return bytes;
}

inline std::string tempSibling(const std::string& path) {
    static std::atomic<uint64_t> serial{0};
    const uint64_t stamp =
        (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
    for (uint64_t i = 0; i != 1000; i++) {
        const std::string candidate = path + ".part." + std::to_string(stamp) + "." +
                                      std::to_string(serial.fetch_add(1));
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec) && !ec) return candidate;
    }
    throw std::runtime_error("cannot create temporary VKMT sibling for " + path);
}

inline bool sameBytes(const std::string& a, const std::string& b) {
    std::error_code ea, eb;
    const uint64_t as = (uint64_t)std::filesystem::file_size(a, ea);
    const uint64_t bs = (uint64_t)std::filesystem::file_size(b, eb);
    if (ea || eb || as != bs) return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    std::array<char, 1 << 16> ba{}, bb{};
    uint64_t left = as;
    while (left) {
        const std::streamsize take =
            (std::streamsize)std::min<uint64_t>(left, ba.size());
        fa.read(ba.data(), take);
        fb.read(bb.data(), take);
        if (fa.gcount() != take || fb.gcount() != take ||
            std::memcmp(ba.data(), bb.data(), (size_t)take) != 0)
            return false;
        left -= (uint64_t)take;
    }
    return true;
}

inline void publish(const std::string& tmp, const std::string& path) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) throw std::runtime_error("cannot publish " + path);
    if (exists) {
        if (sameBytes(tmp, path)) {
            std::filesystem::remove(tmp, ec);
            return;
        }
        std::filesystem::remove(tmp, ec);
        throw std::runtime_error("refusing to overwrite " + path);
    }

    std::filesystem::create_hard_link(tmp, path, ec);
    if (!ec) {
        std::filesystem::remove(tmp, ec);
        return;
    }
    ec.clear();
    if (std::filesystem::exists(path, ec)) {
        if (!ec && sameBytes(tmp, path)) {
            std::filesystem::remove(tmp, ec);
            return;
        }
        std::filesystem::remove(tmp, ec);
        throw std::runtime_error("refusing to overwrite " + path);
    }
    if (ec) {
        std::filesystem::remove(tmp, ec);
        throw std::runtime_error("cannot publish " + path);
    }
    ec.clear();
    std::filesystem::rename(tmp, path, ec);
    if (!ec) return;
    std::filesystem::remove(tmp, ec);
    throw std::runtime_error("cannot publish " + path);
}

inline MatchesDatabase parse(const std::string& path) {
    try {
        Reader r(path);
        Budget budget;
        char magic[4] = {};
        r.read(magic, sizeof magic);
        if (std::memcmp(magic, "VKMT", sizeof magic) != 0) bad(path, "bad magic");
        const uint32_t version = r.value<uint32_t>();
        if (version < kOldestVersion || version > kCurrentVersion)
            bad(path, "unsupported version");
        const uint32_t nimg = r.value<uint32_t>();
        if (nimg > r.remaining() / 8) bad(path, "image count exceeds file");

        MatchesDatabase db;
        budget.take(mul(nimg, sizeof(ImageEntry), path), path);
        db.images.resize(checkedSize(nimg, path));
        for (uint32_t i = 0; i < nimg; i++) {
            const uint32_t len = r.value<uint32_t>();
            if (r.remaining() < 4 || len > r.remaining() - 4)
                bad(path, "image name exceeds file");
            budget.take(len, path);
            db.images[i].name.resize(checkedSize(len, path));
            if (len) r.read(db.images[i].name.data(), len);
            if (db.images[i].name.empty() ||
                db.images[i].name.find('\0') != std::string::npos)
                bad(path, "invalid image name");
            db.images[i].num_features = r.value<uint32_t>();
        }

        const uint32_t npairs = r.value<uint32_t>();
        if (npairs > r.remaining() / 16) bad(path, "pair count exceeds file");
        budget.take(mul(npairs, sizeof(TwoViewMatches), path), path);
        budget.take(mul(npairs, sizeof(uint64_t), path), path);
        db.pairs.reserve(checkedSize(npairs, path));
        std::vector<uint64_t> keys;
        keys.reserve(checkedSize(npairs, path));
        for (uint32_t i = 0; i < npairs; i++) {
            TwoViewMatches& pair = db.pairs.emplace_back();
            pair.image1 = r.value<uint32_t>();
            pair.image2 = r.value<uint32_t>();
            pair.config = r.value<int32_t>();
            const uint32_t nm = r.value<uint32_t>();
            if (pair.image1 >= nimg || pair.image2 >= nimg)
                bad(path, "pair endpoint out of range");
            if (pair.image1 >= pair.image2)
                bad(path, "pair endpoints are not an ordered unordered pair");
            if (nm > r.remaining() / 8) bad(path, "match count exceeds file");
            budget.take(mul(nm, sizeof(FeatureMatch), path), path);
            pair.matches.resize(checkedSize(nm, path));
            for (uint32_t j = 0; j < nm; j++) {
                FeatureMatch& match = pair.matches[j];
                match.idx1 = r.value<uint32_t>();
                match.idx2 = r.value<uint32_t>();
                if (match.idx1 >= db.images[pair.image1].num_features ||
                    match.idx2 >= db.images[pair.image2].num_features)
                    bad(path, "feature index out of range");
            }
            keys.push_back(pairKey(pair.image1, pair.image2));
        }
        std::sort(keys.begin(), keys.end());
        for (size_t i = 1; i < keys.size(); i++)
            if (keys[i] == keys[i - 1]) bad(path, "duplicate image pair");

        if (version >= 3) {
            const uint32_t ncam = r.value<uint32_t>();
            if (ncam > r.remaining() / 45) bad(path, "camera count exceeds file");
            budget.take(mul(ncam, sizeof(Camera), path), path);
            budget.take(ncam, path);
            if (version >= 4) budget.take(ncam, path);
            budget.take(mul(ncam, sizeof(uint32_t), path), path);
            db.cameras.resize(checkedSize(ncam, path));
            db.focal_prior.resize(checkedSize(ncam, path));
            std::vector<uint32_t> cameraRecordIds;
            cameraRecordIds.reserve(checkedSize(ncam, path));
            for (uint32_t i = 0; i < ncam; i++) {
                Camera& cam = db.cameras[i];
                cam.id = r.value<uint32_t>();
                const int32_t width = r.value<int32_t>();
                const int32_t height = r.value<int32_t>();
                const int32_t readModelId = r.value<int32_t>();
                const uint8_t prior = r.value<uint8_t>();
                cam.width = width;
                cam.height = height;
                cam.pixel_scale = r.value<double>();
                const uint32_t np = r.value<uint32_t>();
                if (prior > 1) bad(path, "invalid focal-prior flag");
                if (width <= 0 || height <= 0) bad(path, "invalid camera dimensions");
                if (!std::isfinite(cam.pixel_scale) || cam.pixel_scale <= 0)
                    bad(path, "invalid camera pixel scale");
                try {
                    cam.model = camFromColmapId(readModelId);
                } catch (...) {
                    bad(path, "unsupported camera model");
                }
                if (np != cameraParams(cam, path)) bad(path, "invalid camera parameter count");
                double params[16] = {};
                const uint64_t paramBytes = mul(np, sizeof(double), path);
                if (paramBytes > r.remaining()) bad(path, "camera parameters exceed file");
                r.read(params, paramBytes);
                for (uint32_t j = 0; j < np; j++)
                    finite(params[j], path, "non-finite camera parameter");
                unpackColmap(cam, params);
                const double values[] = {cam.fx, cam.fy, cam.cx, cam.cy, cam.k1, cam.k2,
                                         cam.p1, cam.p2, cam.k3, cam.k4, cam.k5, cam.k6,
                                         cam.sx1, cam.sy1};
                for (double value : values) finite(value, path, "non-finite camera value");
                db.focal_prior[i] = prior;
                cameraRecordIds.push_back(cam.id);
            }
            std::sort(cameraRecordIds.begin(), cameraRecordIds.end());
            for (size_t i = 1; i < cameraRecordIds.size(); i++)
                if (cameraRecordIds[i] == cameraRecordIds[i - 1])
                    bad(path, "duplicate camera id");

            if (ncam) {
                const uint32_t nid = r.value<uint32_t>();
                if (nid != nimg) bad(path, "camera id count does not match images");
                const uint64_t idBytes = mul(nimg, sizeof(uint32_t), path);
                if (idBytes > r.remaining()) bad(path, "camera ids exceed file");
                budget.take(idBytes, path);
                db.camera_ids.resize(checkedSize(nimg, path));
                r.read(db.camera_ids.data(), idBytes);
                for (uint32_t id : db.camera_ids)
                    if (!std::binary_search(cameraRecordIds.begin(), cameraRecordIds.end(), id))
                        bad(path, "camera id has no camera record");
                if (version >= 4) {
                    if (ncam > r.remaining()) bad(path, "measured section exceeds file");
                    db.focal_measured.resize(checkedSize(ncam, path));
                    r.read(db.focal_measured.data(), ncam);
                    for (uint8_t measured : db.focal_measured)
                        if (measured > 1) bad(path, "invalid focal-measured flag");
                }
            }
        }
        if (r.remaining() != 0) bad(path, "trailing bytes");
        return db;
    } catch (const std::bad_alloc&) {
        bad(path, "allocation failed");
    } catch (const std::length_error&) {
        bad(path, "allocation size overflow");
    }
}

}  // namespace matches_detail

// ---------------------------------------------------------------------------
// matches.bin -- "VKMT", u32 version=4; v2 stops after the pairs, v3 adds
// cameras, and v4 adds one measured-focal byte per camera.
// ---------------------------------------------------------------------------

inline void writeMatches(const std::string& path, const MatchesDatabase& db) {
    matches_detail::encodedSize(db, path);
    matches_detail::validateDatabase(db, path);
    const std::string tmp = matches_detail::tempSibling(path);
    try {
        {
            std::ofstream f(tmp, std::ios::binary);
            if (!f) throw std::runtime_error("cannot write " + path);
            const uint32_t version = 4;
            const uint32_t nimg = (uint32_t)db.images.size();
            f.write("VKMT", 4);
            f.write((const char*)&version, 4);
            f.write((const char*)&nimg, 4);
            for (const ImageEntry& image : db.images) {
                const uint32_t len = (uint32_t)image.name.size();
                f.write((const char*)&len, 4);
                f.write(image.name.data(), (std::streamsize)len);
                f.write((const char*)&image.num_features, 4);
            }
            const uint32_t npairs = (uint32_t)db.pairs.size();
            f.write((const char*)&npairs, 4);
            for (const TwoViewMatches& pair : db.pairs) {
                const uint32_t nm = (uint32_t)pair.matches.size();
                f.write((const char*)&pair.image1, 4);
                f.write((const char*)&pair.image2, 4);
                f.write((const char*)&pair.config, 4);
                f.write((const char*)&nm, 4);
                for (const FeatureMatch& match : pair.matches) {
                    f.write((const char*)&match.idx1, 4);
                    f.write((const char*)&match.idx2, 4);
                }
            }
            const uint32_t ncam = (uint32_t)db.cameras.size();
            f.write((const char*)&ncam, 4);
            for (uint32_t i = 0; i < ncam; i++) {
                const Camera& cam = db.cameras[i];
                const int32_t modelId = camColmapId(cam.model);
                const uint32_t np = (uint32_t)camColmapParams(cam.model);
                const uint8_t prior = db.focal_prior.empty() ? 0 : db.focal_prior[i];
                const int32_t width = cam.width, height = cam.height;
                double params[16] = {};
                packColmap(cam, params);
                f.write((const char*)&cam.id, 4);
                f.write((const char*)&width, 4);
                f.write((const char*)&height, 4);
                f.write((const char*)&modelId, 4);
                f.write((const char*)&prior, 1);
                f.write((const char*)&cam.pixel_scale, 8);
                f.write((const char*)&np, 4);
                f.write((const char*)params, (std::streamsize)np * 8);
            }
            if (ncam) {
                const uint32_t nid = (uint32_t)db.camera_ids.size();
                f.write((const char*)&nid, 4);
                f.write((const char*)db.camera_ids.data(), (std::streamsize)nid * 4);
                for (uint32_t i = 0; i < ncam; i++) {
                    const uint8_t measured =
                        db.focal_measured.empty() ? 0 : db.focal_measured[i];
                    f.write((const char*)&measured, 1);
                }
            }
            f.flush();
            if (!f) throw std::runtime_error("cannot write " + path);
            f.close();
            if (!f) throw std::runtime_error("cannot write " + path);
        }
        matches_detail::publish(tmp, path);
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        throw;
    }
}

inline MatchesDatabase readMatches(const std::string& path) {
    return matches_detail::parse(path);
}


// ---- one pair at a time -------------------------------------------------
//
// The pair table without the matches themselves, for a reader that wants to
// draw one pair out of a file whose match arrays are most of a gigabyte. The
// GUI's match map is the caller: it needs every pair's size to draw, and one
// pair's contents only when the cursor is over it.

// A pair count of this means "pairs until the end of the file": the writer was
// still appending when the file was made (sfm/core/Progress.h, live_matches.bin).
inline constexpr uint32_t kStreamingPairs = 0xFFFFFFFFu;

struct MatchesIndex {
    struct Entry {
        uint32_t image1 = 0, image2 = 0, count = 0;
        uint64_t offset = 0;      // first idx1 of this pair's array
    };
    std::vector<ImageEntry> images;
    std::vector<Entry> pairs;
};

inline bool indexMatches(const std::string& path, MatchesIndex& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    // A file still being written hands back nonsense counts, and this one is
    // read while a run is going. The smallest a pair can be on disk is 16
    // bytes, so the file's own size is what bounds them.
    f.seekg(0, std::ios::end);
    const uint64_t bytes = (uint64_t)f.tellg();
    f.seekg(0);
    char magic[4];
    f.read(magic, 4);
    uint32_t version = 0, nimg = 0;
    f.read((char*)&version, 4);
    f.read((char*)&nimg, 4);
    if (!f || std::memcmp(magic, "VKMT", 4) != 0) return false;
    MatchesIndex idx;
    idx.images.resize(nimg);
    for (uint32_t i = 0; i < nimg; i++) {
        uint32_t len = 0;
        f.read((char*)&len, 4);
        if (!f || len > (1u << 20)) return false;
        idx.images[i].name.resize(len);
        f.read(&idx.images[i].name[0], len);
        f.read((char*)&idx.images[i].num_features, 4);
    }
    uint32_t npairs = 0;
    f.read((char*)&npairs, 4);
    const bool streaming = npairs == kStreamingPairs;
    if (!f || (!streaming && (uint64_t)npairs * 16 > bytes)) return false;
    if (!streaming) idx.pairs.reserve(npairs);
    for (uint32_t i = 0; streaming || i < npairs; i++) {
        MatchesIndex::Entry e;
        int32_t config = 0;
        f.read((char*)&e.image1, 4);
        f.read((char*)&e.image2, 4);
        f.read((char*)&config, 4);
        f.read((char*)&e.count, 4);
        // Streaming stops at the tail the writer has not finished; a fixed
        // count that runs out is a truncated file and stays an error.
        if (!f) return streaming ? (out = std::move(idx), true) : false;
        e.offset = (uint64_t)f.tellg();
        // seekg past the end does not fail until something is read, so the
        // bound is checked here for both shapes: a short streaming file is a
        // tail the writer has not finished, a short fixed one is truncated.
        if (e.offset + (uint64_t)e.count * 8 > bytes)
            return streaming ? (out = std::move(idx), true) : false;
        idx.pairs.push_back(e);
        f.seekg((std::streamoff)e.count * 8, std::ios::cur);
        if (!f) return streaming ? (out = std::move(idx), true) : false;
    }
    out = std::move(idx);
    return true;
}

inline bool readPairMatches(const std::string& path,
                            const MatchesIndex::Entry& e,
                            std::vector<FeatureMatch>& out) {
    out.clear();
    if (!e.count) return true;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg((std::streamoff)e.offset);
    out.resize(e.count);
    for (uint32_t i = 0; i < e.count && f; i++) {
        f.read((char*)&out[i].idx1, 4);
        f.read((char*)&out[i].idx2, 4);
    }
    if (!f) { out.clear(); return false; }
    return true;
}

}  // namespace sfm
