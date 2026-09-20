// Resume.cpp -- see Resume.h.

#include "sfm/core/Resume.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <tuple>

#include "sfm/core/FeatureWork.h"

namespace fs = std::filesystem;

namespace sfm {
namespace resume {

namespace {

constexpr size_t kFlushBytes = 4u << 20;
constexpr double kFlushSeconds = 5.0;
constexpr uint32_t kJournalVersion = 2;
constexpr uint32_t kRecordVersion = 1;
constexpr uint32_t kMaxRecordString = 1u << 20;
constexpr uint32_t kMaxRecordMatches = 1u << 22;

double steadyNow() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void put_u32(std::string& b, uint32_t v) { b.append((const char*)&v, 4); }
void put_i32(std::string& b, int32_t v) { b.append((const char*)&v, 4); }

bool read_u32(std::istream& f, uint32_t& v) {
    f.read((char*)&v, 4);
    return f.gcount() == 4;
}

void put_string(std::string& b, const std::string& value) {
    put_u32(b, (uint32_t)value.size());
    b += value;
}

bool read_string(std::istream& f, std::string& value) {
    uint32_t size = 0;
    if (!read_u32(f, size) || size > kMaxRecordString) return false;
    value.resize(size);
    if (!size) return true;
    f.read(value.data(), (std::streamsize)size);
    return f.gcount() == (std::streamsize)size;
}

// The signature block every file here opens with: u32 length, then the text.
void put_signature(std::string& b, const std::string& sig) {
    put_u32(b, (uint32_t)sig.size());
    b += sig;
}

bool check_signature(std::istream& f, const std::string& sig) {
    uint32_t len = 0;
    if (!read_u32(f, len) || len > kMaxRecordString) return false;
    std::string seen(len, '\0');
    if (len) f.read(seen.data(), (std::streamsize)len);
    return (!len || f.gcount() == (std::streamsize)len) && seen == sig;
}

bool magic_is(std::istream& f, const char* want) {
    char m[4];
    f.read(m, 4);
    return f.gcount() == 4 && std::memcmp(m, want, 4) == 0;
}

bool canonicalSwap(const PairDependency& dependency) {
    return std::tie(dependency.feature_key2, dependency.feature_digest2) <
           std::tie(dependency.feature_key1, dependency.feature_digest1);
}

void appendCanonicalProducts(std::string& bytes, const PairDependency& dependency) {
    const bool swap = canonicalSwap(dependency);
    if (swap) {
        put_string(bytes, dependency.feature_key2);
        put_string(bytes, dependency.feature_digest2);
        put_string(bytes, dependency.feature_key1);
        put_string(bytes, dependency.feature_digest1);
    } else {
        put_string(bytes, dependency.feature_key1);
        put_string(bytes, dependency.feature_digest1);
        put_string(bytes, dependency.feature_key2);
        put_string(bytes, dependency.feature_digest2);
    }
}

std::string canonicalPairDigest(const PairDependency& dependency, int32_t config,
                                const std::vector<FeatureMatch>& matches) {
    std::string bytes;
    bytes.reserve(12 + matches.size() * 8);
    put_i32(bytes, config);
    put_u32(bytes, (uint32_t)matches.size());
    const bool swap = canonicalSwap(dependency);
    for (const FeatureMatch& match : matches) {
        if (swap) {
            put_u32(bytes, match.idx2);
            put_u32(bytes, match.idx1);
        } else {
            put_u32(bytes, match.idx1);
            put_u32(bytes, match.idx2);
        }
    }
    return feature_work::sha256Text(bytes);
}

std::string recordPairDigest(int32_t config,
                             const std::vector<std::pair<uint32_t, uint32_t>>& matches) {
    std::string bytes;
    bytes.reserve(12 + matches.size() * 8);
    put_i32(bytes, config);
    put_u32(bytes, (uint32_t)matches.size());
    for (const auto& match : matches) {
        put_u32(bytes, match.first);
        put_u32(bytes, match.second);
    }
    return feature_work::sha256Text(bytes);
}

PairDependency canonicalRecordDependency(const std::string& key,
                                         const std::string& feature_key1,
                                         const std::string& feature_digest1,
                                         const std::string& feature_key2,
                                         const std::string& feature_digest2,
                                         const std::string& recipe_digest,
                                         uint32_t image1, uint32_t image2,
                                         const std::string& image_name1,
                                         const std::string& image_name2) {
    PairDependency dependency;
    dependency.image1 = image1;
    dependency.image2 = image2;
    dependency.image_name1 = image_name1;
    dependency.image_name2 = image_name2;
    dependency.feature_key1 = feature_key1;
    dependency.feature_digest1 = feature_digest1;
    dependency.feature_key2 = feature_key2;
    dependency.feature_digest2 = feature_digest2;
    dependency.recipe_digest = recipe_digest;
    dependency.dependency_key = key;
    return dependency;
}

bool sameProducts(const PairDependency& a, const PairDependency& b) {
    const bool as = canonicalSwap(a);
    const bool bs = canonicalSwap(b);
    const std::string& ak1 = as ? a.feature_key2 : a.feature_key1;
    const std::string& ad1 = as ? a.feature_digest2 : a.feature_digest1;
    const std::string& ak2 = as ? a.feature_key1 : a.feature_key2;
    const std::string& ad2 = as ? a.feature_digest1 : a.feature_digest2;
    const std::string& bk1 = bs ? b.feature_key2 : b.feature_key1;
    const std::string& bd1 = bs ? b.feature_digest2 : b.feature_digest1;
    const std::string& bk2 = bs ? b.feature_key1 : b.feature_key2;
    const std::string& bd2 = bs ? b.feature_digest1 : b.feature_digest2;
    return ak1 == bk1 && ad1 == bd1 && ak2 == bk2 && ad2 == bd2 &&
           a.recipe_digest == b.recipe_digest;
}

bool trimJournalTail(const fs::path& file, const std::string& signature) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(file, ec);
    if (ec) return false;
    std::ifstream f(file, std::ios::binary);
    uint32_t version = 0;
    if (!f || !magic_is(f, "VKR2") || !check_signature(f, signature) ||
        !read_u32(f, version) || version != kJournalVersion)
        return false;
    const std::streamoff header_end = f.tellg();
    if (header_end < 0) return false;
    uintmax_t valid = (uintmax_t)header_end;
    for (;;) {
        const std::streamoff start = f.tellg();
        if (start < 0) break;
        uint32_t record_version = 0, image1 = 0, image2 = 0;
        int32_t config = 0;
        uint32_t putative = 0, complete = 0, count = 0;
        std::string ignored;
        if (!read_u32(f, record_version) || record_version != kRecordVersion ||
            !read_string(f, ignored) || !read_string(f, ignored) ||
            !read_string(f, ignored) || !read_string(f, ignored) ||
            !read_string(f, ignored) || !read_string(f, ignored) ||
            !read_string(f, ignored) || !read_u32(f, image1) ||
            !read_u32(f, image2) || !read_string(f, ignored) ||
            !read_string(f, ignored) || !f.read((char*)&config, 4) ||
            !read_u32(f, putative) || !read_u32(f, complete) ||
            !read_u32(f, count) || count > kMaxRecordMatches)
            break;
        const std::streamoff payload = f.tellg();
        if (payload < 0 || (uintmax_t)payload > size ||
            count > (size - (uintmax_t)payload) / 8)
            break;
        f.seekg((std::streamoff)count * 8, std::ios::cur);
        if (!f) break;
        const std::streamoff end = f.tellg();
        if (end < 0) break;
        valid = (uintmax_t)end;
    }
    if (valid == size) return true;
    fs::resize_file(file, valid, ec);
    return !ec;
}

}  // namespace

fs::path dir(const std::string& workspace) { return fs::path(workspace) / kDir; }

std::string recorded(const fs::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return "";
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

void store(const fs::path& file, const std::string& signature) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (f) f << signature;
}

void forget(const fs::path& file) {
    std::error_code ec;
    fs::remove(file, ec);
}

void clear(const std::string& workspace) {
    if (workspace.empty()) return;
    std::error_code ec;
    fs::remove_all(dir(workspace), ec);
}

// ---------------------------------------------------------------------------
// The pair list
// ---------------------------------------------------------------------------

bool readPairs(const fs::path& file, const std::string& signature,
               std::vector<std::pair<uint32_t, uint32_t>>& pairs) {
    std::ifstream f(file, std::ios::binary);
    if (!f || !magic_is(f, "VKRP") || !check_signature(f, signature)) return false;
    uint32_t n = 0;
    if (!read_u32(f, n)) return false;
    std::vector<std::pair<uint32_t, uint32_t>> out(n);
    f.read((char*)out.data(), (std::streamsize)n * 8);
    if (f.gcount() != (std::streamsize)n * 8) return false;
    pairs.swap(out);
    return true;
}

void writePairs(const fs::path& file, const std::string& signature,
                const std::vector<std::pair<uint32_t, uint32_t>>& pairs) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::string head;
    head += "VKRP";
    put_signature(head, signature);
    put_u32(head, (uint32_t)pairs.size());
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f.write(head.data(), (std::streamsize)head.size());
    f.write((const char*)pairs.data(), (std::streamsize)pairs.size() * 8);
}

// ---------------------------------------------------------------------------
// The verification journal
// ---------------------------------------------------------------------------

std::string pairDependencyKey(const PairDependency& dependency) {
    std::string bytes = "sfm-pair-dependency-v2";
    appendCanonicalProducts(bytes, dependency);
    put_string(bytes, dependency.recipe_digest);
    return feature_work::sha256Text(bytes);
}

std::string pairPayloadDigest(const PairDependency& dependency,
                              const TwoViewMatches& pair) {
    std::vector<FeatureMatch> oriented;
    oriented.reserve(pair.matches.size());
    const bool reverse = pair.image1 != dependency.image1;
    for (const FeatureMatch& match : pair.matches)
        oriented.push_back(reverse ? FeatureMatch{match.idx2, match.idx1, 0} : match);
    return canonicalPairDigest(dependency, pair.config, oriented);
}

bool MatchJournal::open(const fs::path& file, const std::string& signature,
                        bool append,
                        const std::vector<PairDependency>& dependencies) {
    close();
    _dependencies.clear();
    for (const PairDependency& source : dependencies) {
        PairDependency dependency = source;
        const std::string computed = pairDependencyKey(dependency);
        if (!dependency.dependency_key.empty() && dependency.dependency_key != computed)
            continue;
        dependency.dependency_key = computed;
        _dependencies[pairKey(dependency.image1, dependency.image2)] = std::move(dependency);
    }
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (append) {
        std::ifstream existing(file, std::ios::binary);
        uint32_t version = 0;
        if (!existing || !magic_is(existing, "VKR2") ||
            !check_signature(existing, signature) || !read_u32(existing, version) ||
            version != kJournalVersion)
            return false;
        if (!trimJournalTail(file, signature)) return false;
    }
    _f.open(file, std::ios::binary |
                      (append ? std::ios::app : std::ios::trunc));
    if (!_f) return false;
    if (!append) {
        std::string head;
        head += "VKR2";
        put_signature(head, signature);
        put_u32(head, kJournalVersion);
        _f.write(head.data(), (std::streamsize)head.size());
    }
    _flushed_at = steadyNow();
    _armed = (bool)_f;
    return _armed;
}

void MatchJournal::record(uint32_t a, uint32_t b, int32_t config, uint32_t putative,
                          const uint32_t* idx1, const uint32_t* idx2,
                          size_t stride, uint32_t count) {
    if (!_armed || count > kMaxRecordMatches || (count && (!idx1 || !idx2 || stride < 4)))
        return;
    const auto found = _dependencies.find(pairKey(a, b));
    if (found == _dependencies.end()) return;
    const PairDependency& dependency = found->second;

    std::vector<FeatureMatch> matches;
    matches.reserve(count);
    const bool endpoint_swap = a != dependency.image1;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t x = 0, y = 0;
        std::memcpy(&x, (const char*)idx1 + i * stride, 4);
        std::memcpy(&y, (const char*)idx2 + i * stride, 4);
        matches.push_back(endpoint_swap ? FeatureMatch{y, x, 0}
                                        : FeatureMatch{x, y, 0});
    }
    const std::string payload_digest =
        canonicalPairDigest(dependency, config, matches);
    const bool swap = canonicalSwap(dependency);
    std::string rec;
    rec.reserve(128 + (size_t)count * 8);
    put_u32(rec, kRecordVersion);
    put_string(rec, dependency.dependency_key);
    if (swap) {
        put_string(rec, dependency.feature_key2);
        put_string(rec, dependency.feature_digest2);
        put_string(rec, dependency.feature_key1);
        put_string(rec, dependency.feature_digest1);
    } else {
        put_string(rec, dependency.feature_key1);
        put_string(rec, dependency.feature_digest1);
        put_string(rec, dependency.feature_key2);
        put_string(rec, dependency.feature_digest2);
    }
    put_string(rec, dependency.recipe_digest);
    put_string(rec, payload_digest);
    put_u32(rec, dependency.image1);
    put_u32(rec, dependency.image2);
    put_string(rec, dependency.image_name1);
    put_string(rec, dependency.image_name2);
    put_i32(rec, config);
    put_u32(rec, putative);
    put_u32(rec, 1);  // completed
    put_u32(rec, count);
    for (const FeatureMatch& match : matches) {
        if (swap) {
            put_u32(rec, match.idx2);
            put_u32(rec, match.idx1);
        } else {
            put_u32(rec, match.idx1);
            put_u32(rec, match.idx2);
        }
    }
    std::lock_guard<std::mutex> lk(_mu);
    _buf += rec;
    write_locked(false);
}

void MatchJournal::write_locked(bool force) {
    const double t = steadyNow();
    if (_buf.empty() ||
        (!force && _buf.size() < kFlushBytes && t - _flushed_at < kFlushSeconds))
        return;
    _f.write(_buf.data(), (std::streamsize)_buf.size());
    _f.flush();
    _buf.clear();
    _flushed_at = t;
}

void MatchJournal::flush() {
    std::lock_guard<std::mutex> lk(_mu);
    if (_armed) write_locked(true);
}

void MatchJournal::close() {
    std::lock_guard<std::mutex> lk(_mu);
    if (_armed) write_locked(true);
    _f.close();
    _f.clear();
    _buf.clear();
    _dependencies.clear();
    _armed = false;
}

bool readJournal(const fs::path& file, const std::string& signature,
                 const std::vector<PairDependency>& dependencies,
                 const std::vector<ImageEntry>& images,
                 std::unordered_map<uint64_t, TwoViewMatches>& kept,
                 std::vector<uint64_t>& done, uint64_t& putative) {
    std::ifstream f(file, std::ios::binary);
    uint32_t version = 0;
    if (!f || !magic_is(f, "VKR2") || !check_signature(f, signature) ||
        !read_u32(f, version) || version != kJournalVersion)
        return false;

    std::unordered_map<std::string, std::vector<PairDependency>> candidates;
    for (const PairDependency& source : dependencies) {
        PairDependency dependency = source;
        const std::string computed = pairDependencyKey(dependency);
        if (!dependency.dependency_key.empty() && dependency.dependency_key != computed)
            continue;
        dependency.dependency_key = computed;
        candidates[computed].push_back(std::move(dependency));
    }

    struct RawRecord {
        std::string dependency_key;
        std::string feature_key1, feature_digest1;
        std::string feature_key2, feature_digest2;
        std::string recipe_digest, payload_digest;
        uint32_t image1 = 0, image2 = 0;
        std::string image_name1, image_name2;
        int32_t config = 0;
        uint32_t putative = 0, complete = 0, count = 0;
        std::vector<std::pair<uint32_t, uint32_t>> matches;
    };
    std::unordered_map<uint64_t, TwoViewMatches> out_kept;
    std::vector<uint64_t> out_done;
    std::unordered_map<uint64_t, char> seen_done;
    uint64_t out_putative = 0;
    for (;;) {
        RawRecord record;
        if (!read_u32(f, version)) break;
        if (version != kRecordVersion) break;
        if (!read_string(f, record.dependency_key) ||
            !read_string(f, record.feature_key1) ||
            !read_string(f, record.feature_digest1) ||
            !read_string(f, record.feature_key2) ||
            !read_string(f, record.feature_digest2) ||
            !read_string(f, record.recipe_digest) ||
            !read_string(f, record.payload_digest) ||
            !read_u32(f, record.image1) || !read_u32(f, record.image2) ||
            !read_string(f, record.image_name1) ||
            !read_string(f, record.image_name2))
            break;
        if (!f.read((char*)&record.config, 4) ||
            !read_u32(f, record.putative) || !read_u32(f, record.complete) ||
            !read_u32(f, record.count))
            break;
        if (record.count > kMaxRecordMatches) break;
        record.matches.resize(record.count);
        bool torn = false;
        for (auto& match : record.matches)
            if (!read_u32(f, match.first) || !read_u32(f, match.second)) {
                torn = true;
                break;
            }
        if (torn) break;

        {
            PairDependency recorded = canonicalRecordDependency(
                record.dependency_key, record.feature_key1, record.feature_digest1,
                record.feature_key2, record.feature_digest2, record.recipe_digest,
                record.image1, record.image2, record.image_name1, record.image_name2);
            if (pairDependencyKey(recorded) != record.dependency_key ||
                record.complete != 1 ||
                recordPairDigest(record.config, record.matches) != record.payload_digest)
                continue;
            const auto found = candidates.find(record.dependency_key);
            if (found == candidates.end()) continue;
            for (const PairDependency& dependency : found->second) {
                if (!sameProducts(recorded, dependency)) continue;
                TwoViewMatches pair;
                pair.image1 = dependency.image1;
                pair.image2 = dependency.image2;
                pair.config = record.config;
                pair.matches.reserve(record.matches.size());
                const bool swap = canonicalSwap(dependency);
                for (const auto& match : record.matches)
                    pair.matches.push_back(swap
                                              ? FeatureMatch{match.second, match.first, 0}
                                              : FeatureMatch{match.first, match.second, 0});
                bool valid = dependency.image1 < images.size() &&
                             dependency.image2 < images.size();
                for (const FeatureMatch& match : pair.matches)
                    valid = valid && match.idx1 < images[dependency.image1].num_features &&
                             match.idx2 < images[dependency.image2].num_features;
                if (!valid || pairPayloadDigest(dependency, pair) != record.payload_digest)
                    continue;
                const uint64_t key = pairKey(dependency.image1, dependency.image2);
                if (seen_done.emplace(key, 1).second) {
                    out_done.push_back(key);
                    out_putative += record.putative;
                }
                if (!pair.matches.empty()) out_kept[key] = std::move(pair);
            }
        }
    }
    kept.swap(out_kept);
    done.swap(out_done);
    putative = out_putative;
    return true;
}

}  // namespace resume
}  // namespace sfm
