// Keypoints + descriptors and the flat-file interchange format.
//
// The layout is deliberately dtype/dim-agnostic (docs/notes/sfm-design.md D1): SIFT is
// 128-D uint8 today, a learned frontend later may be F32 and a different width,
// and nothing downstream may assume otherwise. `features.bin` is our own format
// (D4); a converter to/from COLMAP's SQLite database is a separate host tool.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/FilesystemPath.h"

namespace sfm {

// Similarity-covariant keypoint in *original*-image pixel coordinates (top-left
// origin, +x right, +y down), matching COLMAP's FeatureKeypoint(x,y,scale,orn).
// "Original" is the source file's resolution, not the possibly-downscaled one
// SIFT ran on: the extractor scales its output back (see scaleKeypoints), so a
// camera built from a FeatureSet describes the images on disk (D46).
struct Keypoint {
    float x = 0, y = 0;         // subpixel location
    float scale = 0;            // sigma in original-image pixels
    float orientation = 0;      // radians, CCW from +x
    // SIFT: |DoG| at the refined extremum. A learned detector: its detection
    // score. Persisted from v5 -- ALIKED has neither scale nor orientation, so
    // this is the only ranking signal its keypoints carry, and anything that
    // used to rank by scale has to fall back to it (Pairing / PairSelection).
    float response = 0;
};

enum class DType : uint32_t { U8 = 0, F32 = 1 };

inline uint32_t dtypeSize(DType t) { return t == DType::U8 ? 1u : 4u; }

// One image's features. Descriptors are row-major: feature i occupies
// data[i*dim .. i*dim+dim) as `dtype` elements.
struct FeatureSet {
    int width = 0, height = 0;          // source image size features refer to
    // The size SIFT actually ran at, which is smaller than (width,height)
    // whenever the loader downscaled to --max-image-size. 0 means "the same",
    // which is what a file written before this field says. Keypoint coordinates
    // are in the *source* frame (see scaleKeypoints), but their localization
    // noise is a property of *this* frame, so it is what pixel thresholds are
    // measured in (D47).
    int extract_width = 0, extract_height = 0;
    // The focal length EXIF claims for this image, in pixels of (width,height),
    // and the camera identity EXIF gives it (sfm/core/Exif.h). 0 / empty when the
    // file had no usable EXIF. Recorded at extraction because that is the last
    // stage that sees the image file; matching and mapping only see features.
    double exif_focal = 0;
    std::string exif_camera;
    // The Orientation tag, 1..8, of the pixels as the mapper will see them --
    // so 1 once `--exif-orientation apply` has turned them (sfm/core/Exif.h).
    // The gauge fix reads it: a portrait capture's up is not the image's.
    uint8_t exif_orientation = 1;
    uint32_t dim = 128;
    DType dtype = DType::U8;
    std::vector<Keypoint> keypoints;
    std::vector<uint8_t> descriptors;   // count*dim*dtypeSize bytes
    // Optional per-keypoint RGB sampled from the source image (count*3 bytes, or
    // empty). Written by `spirula-sfm extract`; the mapper averages them over each 3D
    // point's track to color the point cloud for 3DGS init. Empty for a feature
    // set produced without an image (e.g. selftest) or read from a v1 file.
    std::vector<uint8_t> colors;

    uint32_t count() const { return (uint32_t)keypoints.size(); }
    bool hasColors() const { return colors.size() == (size_t)count() * 3; }
    // Whether any keypoint carries a detection score worth persisting. SIFT
    // leaves response at 0; a learned detector fills it in.
    bool hasScores() const {
        for (const Keypoint& k : keypoints)
            if (k.response != 0) return true;
        return false;
    }
    // What a subset selection should rank by. Scale for SIFT (D16: the largest
    // scales are the most repeatable), the detection score for a detector that
    // has no scale. Never both -- an extractor fills in one of them.
    float rank(uint32_t i) const {
        const Keypoint& k = keypoints[i];
        return k.scale > 0 ? k.scale : k.response;
    }
    // How many source pixels one extraction pixel is worth: >= 1, and exactly 1
    // when nothing was downscaled. The two axes agree up to the rounding in
    // the size clamp, so their mean is the isotropic answer.
    double pixelScale() const {
        if (extract_width <= 0 || extract_height <= 0 || width <= 0 || height <= 0) return 1.0;
        return 0.5 * ((double)width / extract_width + (double)height / extract_height);
    }
};

// Scale keypoints from the coordinates of a (dw,dh) image to those of a (w,h)
// one, and record the new size. The extractor runs SIFT on a downscaled copy
// when the source is larger than --max-image-size; everything downstream --
// cameras.bin above all -- should still be in the coordinates of the file the
// user has (COLMAP's ScaleKeypoints does the same, feature/utils.cc).
//
// The mapping is the exact inverse of the bilinear resample in sfm/core/Image.h: a
// destination pixel center (x+0.5) came from source (x+0.5)*sx. Sigma scales by
// the geometric mean, the only isotropic choice when the two axes round
// differently.
inline void scaleKeypoints(FeatureSet& fs, int w, int h) {
    if (w <= 0 || h <= 0 || fs.width <= 0 || fs.height <= 0) return;
    fs.extract_width = fs.width;    // where the keypoints were actually measured
    fs.extract_height = fs.height;
    if (w == fs.width && h == fs.height) return;
    const float sx = (float)w / fs.width, sy = (float)h / fs.height;
    const float ss = std::sqrt(sx * sy);
    for (Keypoint& k : fs.keypoints) {
        k.x = (k.x + 0.5f) * sx - 0.5f;
        k.y = (k.y + 0.5f) * sy - 0.5f;
        k.scale *= ss;
    }
    fs.width = w;
    fs.height = h;
}

// ---- features.bin -------------------------------------------------------

namespace feature_detail {

constexpr uint64_t kMaxFeatureFileBytes = 4ull << 30;
constexpr uint64_t kMaxFeatureAllocation = 1ull << 30;
constexpr uint32_t kMaxDimension = 100000000;
constexpr uint32_t kMaxDescriptorDimension = 1u << 20;

inline void requireHost() {
    const uint16_t one = 1;
    if (*reinterpret_cast<const uint8_t*>(&one) != 1 || sizeof(int) != 4 ||
        sizeof(float) != 4 || sizeof(double) != 8)
        throw std::runtime_error("VKFT requires little-endian 32-bit scalar host types");
}
[[noreturn]] inline void bad(const std::string& path, const char* why) {
    throw std::runtime_error("invalid VKFT " + path + ": " + why);
}

inline uint64_t add(uint64_t a, uint64_t b, const std::string& path) {
    if (b > std::numeric_limits<uint64_t>::max() - a) bad(path, "size overflow");
    return a + b;
}

inline uint64_t mul(uint64_t a, uint64_t b, const std::string& path) {
    if (a && b > std::numeric_limits<uint64_t>::max() / a) bad(path, "size overflow");
    return a * b;
}

inline size_t toSize(uint64_t n, const std::string& path) {
    if (n > (uint64_t)std::numeric_limits<size_t>::max() || n > kMaxFeatureAllocation)
        bad(path, "allocation bound exceeded");
    return (size_t)n;
}

inline void finite(float value, const std::string& path, const char* what) {
    if (!std::isfinite(value)) bad(path, what);
}

inline void finite(double value, const std::string& path, const char* what) {
    if (!std::isfinite(value)) bad(path, what);
}

inline std::string tempSibling(const std::string& path) {
    static std::atomic<uint64_t> serial{0};
    const uint64_t stamp = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
    for (uint64_t i = 0; i != 1000; ++i) {
        const std::string candidate = path + ".part." + std::to_string(stamp) + "." +
                                      std::to_string(serial.fetch_add(1));
        std::error_code ec;
        if (!std::filesystem::exists(
                spirula::NativeFilesystemPath(std::filesystem::path(candidate)), ec) && !ec)
            return candidate;
    }
    throw std::runtime_error("cannot create temporary VKFT sibling for " + path);
}

struct Reader {
    std::ifstream file;
    std::string path;
    uint64_t size = 0;
    uint64_t offset = 0;

    explicit Reader(const std::string& p)
        : file(spirula::NativeFilesystemPath(std::filesystem::path(p)),
               std::ios::binary), path(p) {
        if (!file) throw std::runtime_error("cannot read " + p);
        std::error_code ec;
        size = (uint64_t)std::filesystem::file_size(
            spirula::NativeFilesystemPath(std::filesystem::path(p)), ec);
        if (ec) throw std::runtime_error("cannot stat " + p);
        if (size > kMaxFeatureFileBytes) bad(path, "file-size bound exceeded");
    }

    void read(void* dst, uint64_t n) {
        if (n > size - offset) bad(path, "truncated");
        if (n > (uint64_t)std::numeric_limits<std::streamsize>::max())
            bad(path, "stream-size overflow");
        if (n) file.read((char*)dst, (std::streamsize)n);
        if (!file) bad(path, "truncated");
        offset += n;
    }

    void skip(uint64_t n) {
        if (n > size - offset) bad(path, "truncated");
        if (n > (uint64_t)std::numeric_limits<std::streamoff>::max())
            bad(path, "stream-offset overflow");
        file.seekg((std::streamoff)n, std::ios::cur);
        if (!file) bad(path, "truncated");
        offset += n;
    }

    template <typename T> T value() {
        T out{};
        read(&out, sizeof out);
        return out;
    }
};

inline void validateFloatBytes(const uint8_t* bytes, size_t n, const std::string& path) {
    if ((n % sizeof(float)) != 0) bad(path, "unaligned descriptor bytes");
    for (size_t i = 0; i < n / sizeof(float); ++i) {
        float value = 0;
        std::memcpy(&value, bytes + i * sizeof(float), sizeof value);
        finite(value, path, "non-finite descriptor");
    }
}

inline void validateFeatureSet(const std::string& path, const FeatureSet& fs) {
    requireHost();
    if (fs.width <= 0 || fs.height <= 0 || (uint32_t)fs.width > kMaxDimension ||
        (uint32_t)fs.height > kMaxDimension)
        bad(path, "invalid image dimensions");
    if ((fs.extract_width == 0) != (fs.extract_height == 0) ||
        fs.extract_width < 0 || fs.extract_height < 0 ||
        (fs.extract_width && (fs.extract_width > fs.width || fs.extract_height > fs.height)))
        bad(path, "invalid extraction dimensions");
    if (fs.dim == 0 || fs.dim > kMaxDescriptorDimension ||
        (fs.dtype != DType::U8 && fs.dtype != DType::F32))
        bad(path, "invalid descriptor type or dimension");
    if (fs.keypoints.size() > std::numeric_limits<uint32_t>::max())
        bad(path, "feature count overflow");
    const uint64_t count = (uint64_t)fs.keypoints.size();
    const uint64_t descriptorBytes = mul(mul(count, fs.dim, path), dtypeSize(fs.dtype), path);
    if (descriptorBytes > kMaxFeatureAllocation ||
        descriptorBytes != fs.descriptors.size())
        bad(path, "descriptor vector size mismatch");
    if (!fs.colors.empty() && fs.colors.size() != toSize(mul(count, 3, path), path))
        bad(path, "color vector size mismatch");
    if (fs.exif_camera.size() > (1u << 20)) bad(path, "EXIF camera string too long");
    if (!std::isfinite(fs.exif_focal) || fs.exif_focal < 0) bad(path, "invalid EXIF focal");
    if (fs.exif_orientation < 1 || fs.exif_orientation > 8) bad(path, "invalid EXIF orientation");
    for (const Keypoint& k : fs.keypoints) {
        finite(k.x, path, "non-finite keypoint");
        finite(k.y, path, "non-finite keypoint");
        finite(k.scale, path, "non-finite keypoint");
        finite(k.orientation, path, "non-finite keypoint");
        finite(k.response, path, "non-finite score");
        if (k.scale < 0) bad(path, "negative keypoint scale");
    }
    if (fs.dtype == DType::F32)
        validateFloatBytes(fs.descriptors.data(), fs.descriptors.size(), path);
}

inline FeatureSet parse(const std::string& path, bool withDescriptors) {
    requireHost();
    Reader r(path);
    char magic[4] = {};
    r.read(magic, sizeof magic);
    if (std::memcmp(magic, "VKFT", sizeof magic) != 0) bad(path, "bad magic");
    const uint32_t version = r.value<uint32_t>();
    if (version < 1 || version > 6) bad(path, "unsupported version");
    const int32_t width = r.value<int32_t>();
    const int32_t height = r.value<int32_t>();
    const uint32_t count = r.value<uint32_t>();
    const uint32_t dim = r.value<uint32_t>();
    const uint32_t rawDtype = r.value<uint32_t>();
    if (width <= 0 || height <= 0 || (uint32_t)width > kMaxDimension ||
        (uint32_t)height > kMaxDimension)
        bad(path, "invalid image dimensions");
    if (dim == 0 || dim > kMaxDescriptorDimension || rawDtype > (uint32_t)DType::F32)
        bad(path, "invalid descriptor type or dimension");
    const uint64_t keypointBytes = mul(count, 16, path);
    const uint64_t descriptorBytes =
        mul(mul(count, dim, path), dtypeSize((DType)rawDtype), path);
    const uint64_t minimum = add(28, add(keypointBytes, descriptorBytes, path), path);
    uint64_t allocation = add(keypointBytes, mul(count, sizeof(Keypoint), path), path);
    if (withDescriptors) allocation = add(allocation, descriptorBytes, path);
    if (version >= 2) allocation = add(allocation, mul(count, 3, path), path);
    if (version >= 5) allocation = add(allocation, mul(count, sizeof(float), path), path);
    if (minimum > r.size || minimum > kMaxFeatureAllocation || allocation > kMaxFeatureAllocation)
        bad(path, "payload bound exceeded");
    const size_t countSize = toSize(count, path);
    FeatureSet fs;
    fs.width = width;
    fs.height = height;
    fs.dim = dim;
    fs.dtype = (DType)rawDtype;
    fs.keypoints.resize(countSize);
    std::vector<float> rawKeypoints(toSize(mul(count, 16, path) / sizeof(float), path));
    r.read(rawKeypoints.data(), keypointBytes);
    for (uint32_t i = 0; i < count; ++i) {
        const float* v = &rawKeypoints[(size_t)i * 4];
        finite(v[0], path, "non-finite keypoint");
        finite(v[1], path, "non-finite keypoint");
        finite(v[2], path, "non-finite keypoint");
        finite(v[3], path, "non-finite keypoint");
        if (v[2] < 0) bad(path, "negative keypoint scale");
        fs.keypoints[i] = {v[0], v[1], v[2], v[3], 0.0f};
    }
    if (withDescriptors) fs.descriptors.resize(toSize(descriptorBytes, path));
    if (fs.dtype == DType::F32) {
        if (withDescriptors) {
            r.read(fs.descriptors.data(), descriptorBytes);
            validateFloatBytes(fs.descriptors.data(), fs.descriptors.size(), path);
        } else {
            std::vector<uint8_t> block(1 << 20);
            uint64_t left = descriptorBytes;
            while (left) {
                const size_t take = (size_t)std::min<uint64_t>(left, block.size());
                r.read(block.data(), take);
                validateFloatBytes(block.data(), take, path);
                left -= take;
            }
        }
    } else if (withDescriptors) {
        r.read(fs.descriptors.data(), descriptorBytes);
    } else {
        r.skip(descriptorBytes);
    }
    if (version >= 2) {
        const uint8_t hasColors = r.value<uint8_t>();
        if (hasColors > 1) bad(path, "invalid colors flag");
        if (hasColors) {
            const uint64_t bytes = mul(count, 3, path);
            fs.colors.resize(toSize(bytes, path));
            r.read(fs.colors.data(), bytes);
        }
    }
    if (version >= 3) {
        fs.exif_focal = r.value<double>();
        if (!std::isfinite(fs.exif_focal) || fs.exif_focal < 0)
            bad(path, "invalid EXIF focal");
        const uint32_t cameraBytes = r.value<uint32_t>();
        if (cameraBytes > (1u << 20) || cameraBytes > r.size - r.offset)
            bad(path, "invalid EXIF camera length");
        fs.exif_camera.resize(cameraBytes);
        r.read(fs.exif_camera.data(), cameraBytes);
    }
    if (version >= 4) {
        const int32_t ew = r.value<int32_t>();
        const int32_t eh = r.value<int32_t>();
        if ((ew == 0) != (eh == 0) || ew < 0 || eh < 0 ||
            (ew && (ew > width || eh > height)))
            bad(path, "invalid extraction dimensions");
        fs.extract_width = ew;
        fs.extract_height = eh;
    }
    if (version >= 5) {
        const uint8_t hasScores = r.value<uint8_t>();
        if (hasScores > 1) bad(path, "invalid scores flag");
        if (hasScores) {
            std::vector<float> scores(countSize);
            r.read(scores.data(), mul(count, sizeof(float), path));
            for (uint32_t i = 0; i < count; ++i) {
                finite(scores[i], path, "non-finite score");
                fs.keypoints[i].response = scores[i];
            }
        }
    }
    if (version >= 6) {
        fs.exif_orientation = r.value<uint8_t>();
        if (fs.exif_orientation < 1 || fs.exif_orientation > 8)
            bad(path, "invalid EXIF orientation");
    }
    if (r.offset != r.size) bad(path, "trailing bytes");
    return fs;
}

}  // namespace feature_detail

inline void writeFeatures(const std::string& path, const FeatureSet& fs) {
    feature_detail::validateFeatureSet(path, fs);
    const std::string tmp = feature_detail::tempSibling(path);
    try {
        {
            std::ofstream f(spirula::NativeFilesystemPath(std::filesystem::path(tmp)),
                            std::ios::binary);
            if (!f) throw std::runtime_error("cannot write " + path);
            const uint32_t version = 6, count = fs.count(), dtype = (uint32_t)fs.dtype;
            f.write("VKFT", 4);
            f.write((const char*)&version, 4);
            f.write((const char*)&fs.width, 4);
            f.write((const char*)&fs.height, 4);
            f.write((const char*)&count, 4);
            f.write((const char*)&fs.dim, 4);
            f.write((const char*)&dtype, 4);
            for (const Keypoint& k : fs.keypoints) {
                const float v[4] = {k.x, k.y, k.scale, k.orientation};
                f.write((const char*)v, sizeof v);
            }
            f.write((const char*)fs.descriptors.data(), (std::streamsize)fs.descriptors.size());
            const uint8_t hasColors = fs.hasColors() ? 1 : 0;
            f.write((const char*)&hasColors, 1);
            if (hasColors)
                f.write((const char*)fs.colors.data(), (std::streamsize)fs.colors.size());
            const uint32_t cameraBytes = (uint32_t)fs.exif_camera.size();
            f.write((const char*)&fs.exif_focal, 8);
            f.write((const char*)&cameraBytes, 4);
            f.write(fs.exif_camera.data(), (std::streamsize)cameraBytes);
            f.write((const char*)&fs.extract_width, 4);
            f.write((const char*)&fs.extract_height, 4);
            const uint8_t hasScores = fs.hasScores() ? 1 : 0;
            f.write((const char*)&hasScores, 1);
            if (hasScores)
                for (const Keypoint& k : fs.keypoints)
                    f.write((const char*)&k.response, sizeof k.response);
            f.write((const char*)&fs.exif_orientation, 1);
            f.flush();
            if (!f) throw std::runtime_error("cannot write " + path);
        }
        std::error_code ec;
        std::filesystem::rename(
            spirula::NativeFilesystemPath(std::filesystem::path(tmp)),
            spirula::NativeFilesystemPath(std::filesystem::path(path)), ec);
        if (ec) {
            std::error_code removeError;
            std::filesystem::remove(
                spirula::NativeFilesystemPath(std::filesystem::path(path)), removeError);
            if (removeError) throw std::runtime_error("cannot write " + path);
            std::filesystem::rename(
                spirula::NativeFilesystemPath(std::filesystem::path(tmp)),
                spirula::NativeFilesystemPath(std::filesystem::path(path)), ec);
        }
        if (ec) throw std::runtime_error("cannot write " + path);
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(
            spirula::NativeFilesystemPath(std::filesystem::path(tmp)), ec);
        throw;
    }
}

inline bool peekFeatures(const std::string& path, uint32_t& count) {
    try {
        const FeatureSet fs = feature_detail::parse(path, false);
        count = fs.count();
        return true;
    } catch (...) {
        count = 0;
        return false;
    }
}

inline FeatureSet readFeatures(const std::string& path, bool with_descriptors = true) {
    return feature_detail::parse(path, with_descriptors);
}

}  // namespace sfm
