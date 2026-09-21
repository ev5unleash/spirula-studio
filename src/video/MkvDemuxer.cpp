#include "video/MkvDemuxer.h"

#include "nn/core/Log.h"
#include "video/Mp4Demuxer.h"

#include <algorithm>
#include <cstring>
#include <limits>
namespace video {

namespace {

// Element ids, kept with their marker bits so they compare against what readId
// returns verbatim.
enum : uint32_t {
    kEBML            = 0x1A45DFA3,
    kSegment         = 0x18538067,
    kSeekHead        = 0x114D9B74,
    kInfo            = 0x1549A966,
    kTimestampScale  = 0x2AD7B1,
    kDuration        = 0x4489,
    kTracks          = 0x1654AE6B,
    kTrackEntry      = 0xAE,
    kTrackNumber     = 0xD7,
    kTrackType       = 0x83,
    kCodecID         = 0x86,
    kCodecPrivate    = 0x63A2,
    kDefaultDuration = 0x23E383,
    kVideo           = 0xE0,
    kPixelWidth      = 0xB0,
    kPixelHeight     = 0xBA,
    kCluster         = 0x1F43B675,
    kClusterTs       = 0xE7,
    kSimpleBlock     = 0xA3,
    kBlockGroup      = 0xA0,
    kBlock           = 0xA1,
    kCues            = 0x1C53BB6B,
};

bool scale_timestamp(int64_t timestamp, uint64_t scale, int64_t& result) {
    if (scale == 0) return false;
    if (timestamp == 0) {
        result = 0;
        return true;
    }
    if (timestamp > 0) {
        if (scale > (uint64_t)std::numeric_limits<int64_t>::max() /
                        (uint64_t)timestamp)
            return false;
        result = (int64_t)((uint64_t)timestamp * scale);
        return true;
    }
    const uint64_t magnitude = (uint64_t)(-(timestamp + 1)) + 1;
    const uint64_t limit = (uint64_t)std::numeric_limits<int64_t>::max() + 1;
    if (scale > limit / magnitude) return false;
    const uint64_t product = magnitude * scale;
    result = product == limit ? std::numeric_limits<int64_t>::min()
                              : -(int64_t)product;
    return true;
}

}  // namespace

// ================
// EBML primitives
// ================

void MkvDemuxer::seek(uint64_t off) {
    pos_ = off;
    file_.clear();
    file_.seekg((std::streamoff)off, std::ios::beg);
}

bool MkvDemuxer::readBytes(void* dst, uint64_t len) {
    if (pos_ > file_size_ || len > file_size_ - pos_) return false;
    file_.read((char*)dst, (std::streamsize)len);
    if ((uint64_t)file_.gcount() != len) return false;
    pos_ += len;
    return true;
}

bool MkvDemuxer::readId(uint32_t& id) {
    uint8_t b0;
    if (!readBytes(&b0, 1)) return false;
    int len = 1;
    if (b0 & 0x80) len = 1;
    else if (b0 & 0x40) len = 2;
    else if (b0 & 0x20) len = 3;
    else if (b0 & 0x10) len = 4;
    else return false;  // reserved / corrupt
    id = b0;
    for (int i = 1; i < len; ++i) {
        uint8_t b;
        if (!readBytes(&b, 1)) return false;
        id = (id << 8) | b;
    }
    return true;
}

bool MkvDemuxer::readSize(uint64_t& size, bool& unknown) {
    uint8_t b0;
    if (!readBytes(&b0, 1)) return false;
    int len = 0;
    for (int i = 0; i < 8; ++i) {
        if (b0 & (0x80 >> i)) { len = i + 1; break; }
    }
    if (len == 0) return false;
    uint64_t v = b0 & (0xFFu >> len);
    uint64_t all_ones = (0xFFu >> len);
    for (int i = 1; i < len; ++i) {
        uint8_t b;
        if (!readBytes(&b, 1)) return false;
        v = (v << 8) | b;
        all_ones = (all_ones << 8) | 0xFF;
    }
    unknown = (v == all_ones);
    size = v;
    return true;
}

uint64_t MkvDemuxer::readUInt(uint64_t len) {
    uint64_t v = 0;
    uint8_t buf[8] = {};
    if (len > 8 || !readBytes(buf, len)) return 0;
    for (uint64_t i = 0; i < len; ++i) v = (v << 8) | buf[i];
    return v;
}

double MkvDemuxer::readFloat(uint64_t len) {
    uint8_t buf[8] = {};
    if ((len != 4 && len != 8) || !readBytes(buf, len)) return 0.0;
    if (len == 4) {
        uint32_t bits = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                        ((uint32_t)buf[2] << 8) | buf[3];
        float f;
        std::memcpy(&f, &bits, 4);
        return (double)f;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) bits = (bits << 8) | buf[i];
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
}

// ================
// Header parsing
// ================

bool MkvDemuxer::open(const std::string& path, std::string& error) {
    error.clear();
    path_ = path;
    file_.open(path, std::ios::binary | std::ios::ate);
    if (!file_) {
        error = "cannot open '" + path + "'";
        return false;
    }
    file_size_ = (uint64_t)file_.tellg();
    seek(0);

    uint32_t id = 0;
    uint64_t size = 0;
    bool unknown = false;
    if (!readId(id) || id != kEBML) {
        error = "'" + path + "' is not a Matroska/WebM file (no EBML header)";
        return false;
    }
    if (!readSize(size, unknown) || unknown || pos_ > file_size_ ||
        size > file_size_ - pos_) {
        error = "malformed EBML header";
        return false;
    }
    skip(size);

    // Find the Segment.
    while (pos_ < file_size_) {
        if (!readId(id) || !readSize(size, unknown)) {
            error = "corrupt EBML structure before the Segment element";
            return false;
        }
        if (id == kSegment) break;
        if (unknown || pos_ > file_size_ || size > file_size_ - pos_) {
            error = "corrupt EBML structure before the Segment element";
            return false;
        }
        skip(size);
    }
    if (pos_ >= file_size_) {
        error = "'" + path + "' has no Segment element";
        return false;
    }
    if (!unknown && (pos_ > file_size_ || size > file_size_ - pos_)) {
        error = "Matroska Segment extends past the file";
        return false;
    }
    segment_end_ = unknown ? file_size_ : pos_ + size;

    // Walk the Segment's children until Tracks has been seen and a Cluster is
    // in sight. Well-formed files put Info and Tracks first; we do not require
    // it, we just stop looking once both conditions hold.
    while (pos_ < segment_end_) {
        const uint64_t elem_start = pos_;
        if (!readId(id) || !readSize(size, unknown)) break;
        if (pos_ > segment_end_ || (!unknown && size > segment_end_ - pos_)) {
            error = "Matroska element extends past the Segment";
            return false;
        }
        if (unknown && id != kCluster && id != kSegment) break;
        const uint64_t end = unknown ? segment_end_ : pos_ + size;

        if (id == kInfo) {
            if (!parseInfo(end, error)) return false;
        } else if (id == kTracks) {
            if (!parseTracks(end, error)) return false;
        } else if (id == kCluster) {
            if (first_cluster_ == 0) first_cluster_ = elem_start;
            if (!tracks_.empty()) break;
        }
        if (end <= elem_start) break;  // zero-length or corrupt: no progress
        seek(end);
    }

    if (tracks_.empty()) {
        error = "'" + path +
                "' has no supported video track (looked for V_MPEG4/ISO/AVC, "
                "V_MPEGH/ISO/HEVC, V_AV1)";
        return false;
    }
    if (first_cluster_ == 0) {
        error = "'" + path + "' has no Cluster element";
        return false;
    }

    infos_.clear();
    for (size_t i = 0; i < tracks_.size(); ++i) {
        Track& tk = tracks_[i];
        tk.info.index = (int)i;
        tk.info.stream_index = (uint32_t)i;
        tk.info.stream_id = "matroska:track:" + std::to_string(tk.number);
        tk.info.time_base_num = 1;
        tk.info.time_base_den = 1000000000;
        tk.info.duration_sec = duration_;
        if (tk.info.fps > 0.0 && duration_ > 0.0)
            tk.info.frame_count = (int64_t)(duration_ * tk.info.fps + 0.5);
        infos_.push_back(tk.info);
    }
    return selectTrack(0, error);
}

bool MkvDemuxer::parseInfo(uint64_t end, std::string& error) {
    while (pos_ < end) {
        uint32_t id;
        uint64_t size;
        bool unknown;
        if (!readId(id) || !readSize(size, unknown) || unknown || pos_ > end ||
            size > end - pos_) {
            error = "malformed Matroska Info element";
            return false;
        }
        const uint64_t next = pos_ + size;
        if (id == kTimestampScale) {
            if (size == 0 || size > 8) {
                error = "invalid Matroska TimestampScale";
                return false;
            }
            const uint64_t scale = readUInt(size);
            if (scale == 0) {
                error = "Matroska TimestampScale must be nonzero";
                return false;
            }
            timestamp_scale_ns_ = scale;
        } else if (id == kDuration) {
            duration_ = readFloat(size);
        }
        seek(next);
    }
    if (duration_ > 0.0)
        duration_ *= (double)timestamp_scale_ns_ * 1e-9;
    return true;
}

bool MkvDemuxer::parseTracks(uint64_t end, std::string& error) {
    while (pos_ < end) {
        uint32_t id;
        uint64_t size;
        bool unknown;
        if (!readId(id) || !readSize(size, unknown) || unknown || pos_ > end ||
            size > end - pos_) {
            error = "malformed Matroska Tracks element";
            return false;
        }
        const uint64_t next = pos_ + size;
        if (id == kTrackEntry) parseTrackEntry(next);
        seek(next);
    }
    (void)error;
    return true;
}

bool MkvDemuxer::parseTrackEntry(uint64_t end) {
    Track tk;
    uint64_t track_type = 0;
    std::string codec_id;
    while (pos_ < end) {
        uint32_t id;
        uint64_t size;
        bool unknown;
        if (!readId(id) || !readSize(size, unknown) || unknown || pos_ > end ||
            size > end - pos_)
            return false;
        const uint64_t next = pos_ + size;
        switch (id) {
            case kTrackNumber: tk.number = readUInt(size); break;
            case kTrackType:   track_type = readUInt(size); break;
            case kDefaultDuration: tk.default_duration_ns = readUInt(size); break;
            case kCodecID: {
                codec_id.resize((size_t)size);
                readBytes(codec_id.data(), size);
                while (!codec_id.empty() && codec_id.back() == '\0') codec_id.pop_back();
                break;
            }
            case kCodecPrivate:
                tk.info.codec_config.resize((size_t)size);
                readBytes(tk.info.codec_config.data(), size);
                break;
            case kVideo: {
                while (pos_ < next) {
                    uint32_t vid;
                    uint64_t vsize;
                    bool vunknown;
                    if (!readId(vid) || !readSize(vsize, vunknown) || vunknown ||
                        pos_ > next || vsize > next - pos_)
                        return false;
                    const uint64_t vnext = pos_ + vsize;
                    if (vid == kPixelWidth) tk.info.width = (int)readUInt(vsize);
                    else if (vid == kPixelHeight) tk.info.height = (int)readUInt(vsize);
                    seek(vnext);
                }
                break;
            }
            default: break;
        }
        seek(next);
    }

    if (track_type != 1) return false;  // 1 == video
    if (codec_id == "V_MPEG4/ISO/AVC") {
        tk.info.codec = Codec::H264;
        tk.info.nal_length_size =
            tk.info.codec_config.size() >= 5 ? (tk.info.codec_config[4] & 3) + 1 : 4;
    } else if (codec_id == "V_MPEGH/ISO/HEVC") {
        tk.info.codec = Codec::H265;
        tk.info.nal_length_size =
            tk.info.codec_config.size() >= 22 ? (tk.info.codec_config[21] & 3) + 1 : 4;
    } else if (codec_id == "V_AV1") {
        tk.info.codec = Codec::AV1;
        tk.info.nal_length_size = 0;
    } else {
        return false;
    }
    if (tk.default_duration_ns > 0)
        tk.info.fps = 1e9 / (double)tk.default_duration_ns;
    tracks_.push_back(std::move(tk));
    return true;
}


bool MkvDemuxer::selectTrack(int index, std::string& error) {
    error.clear();
    if (index < 0 || index >= (int)tracks_.size()) {
        error = "video track index out of range";
        return false;
    }
    selected_ = index;
    Track& tk = tracks_[(size_t)selected_];
    if (tk.presentation_ranks.empty()) {
        rewindPackets();
        std::vector<int64_t> pts;
        Block block;
        while (nextBlock(block, error)) pts.push_back(block.pts_ns);
        if (!error.empty()) return false;

        std::vector<size_t> order(pts.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(),
                         [&pts](size_t a, size_t b) { return pts[a] < pts[b]; });
        tk.presentation_ranks.resize(order.size());
        for (size_t rank = 0; rank < order.size(); ++rank)
            tk.presentation_ranks[order[rank]] = rank;
    }
    rewindPackets();
    return true;
}

void MkvDemuxer::rewindPackets() {
    in_cluster_ = false;
    packet_index_ = 0;
    discontinuity_segment_ = 0;
    seek(first_cluster_);
}

bool MkvDemuxer::nextCluster(std::string& error) {
    while (pos_ < segment_end_) {
        uint32_t id;
        uint64_t size;
        bool unknown;
        if (!readId(id) || !readSize(size, unknown)) {
            error = "malformed Matroska element between clusters";
            return false;
        }
        if (id == kCluster) {
            if (unknown) {
                error = "unknown-size Cluster (live stream capture); remux the file";
                return false;
            }
            if (pos_ > segment_end_ || size > segment_end_ - pos_) {
                error = "Matroska Cluster extends past the Segment";
                return false;
            }
            cluster_end_ = pos_ + size;
            in_cluster_ = true;
            const uint64_t payload_start = pos_;
            while (pos_ < cluster_end_) {
                uint32_t cid;
                uint64_t csize;
                bool cunknown;
                if (!readId(cid) || !readSize(csize, cunknown) || cunknown ||
                    pos_ > cluster_end_ || csize > cluster_end_ - pos_) {
                    error = "malformed Matroska Cluster";
                    return false;
                }
                const uint64_t child_end = pos_ + csize;
                if (cid == kClusterTs) {
                    if (csize == 0 || csize > 8) {
                        error = "Matroska Cluster has no valid Timestamp";
                        return false;
                    }
                    const uint64_t timestamp = readUInt(csize);
                    if (timestamp > (uint64_t)std::numeric_limits<int64_t>::max()) {
                        error = "Matroska Cluster Timestamp is not representable";
                        return false;
                    }
                    cluster_ts_ = (int64_t)timestamp;
                    seek(payload_start);
                    return true;
                }
                seek(child_end);
            }
            error = "Matroska Cluster has no Timestamp";
            return false;
        }
        if (unknown || pos_ > segment_end_ || size > segment_end_ - pos_) {
            error = "malformed Matroska element between clusters";
            return false;
        }
        skip(size);
    }
    return false;
}

bool MkvDemuxer::nextBlock(Block& block, std::string& error) {
    const Track& tk = tracks_[(size_t)selected_];

    while (true) {
        if (!in_cluster_ || pos_ >= cluster_end_) {
            if (!nextCluster(error)) return false;  // EOF or corrupt
            continue;
        }

        uint32_t id;
        uint64_t size;
        bool unknown;
        const uint64_t elem_pos = pos_;
        if (!readId(id) || !readSize(size, unknown) || unknown) {
            error = "malformed Matroska Cluster element";
            return false;
        }
        if (elem_pos >= cluster_end_ || pos_ > cluster_end_ ||
            size > cluster_end_ - pos_) {
            error = "Matroska element extends past its Cluster";
            return false;
        }
        const uint64_t next_elem = pos_ + size;

        // A BlockGroup wraps exactly one Block plus reference metadata; we only
        // need the Block, so descend and let the loop pick it up next.
        if (id == kBlockGroup) continue;
        if (id != kSimpleBlock && id != kBlock) {
            seek(next_elem);
            continue;
        }

        // Block header: track number (vint), int16 relative timestamp, flags.
        uint64_t track_num;
        bool tn_unknown;
        if (!readSize(track_num, tn_unknown) || tn_unknown || pos_ + 3 > next_elem) {
            error = "malformed Matroska block header";
            return false;
        }
        uint8_t hdr[3];
        if (!readBytes(hdr, 3)) {
            error = "truncated Matroska block header";
            return false;
        }
        const int16_t rel_ts = (int16_t)((hdr[0] << 8) | hdr[1]);
        const uint8_t flags = hdr[2];

        if (track_num != tk.number) {
            seek(next_elem);
            continue;
        }
        if ((flags & 0x06) != 0) {
            error = "laced Matroska block in a video track; this demuxer reads "
                    "unlaced blocks only (remux with `mkvmerge`)";
            return false;
        }

        const int64_t cluster = cluster_ts_;
        const int64_t relative = rel_ts;
        if ((relative > 0 && cluster > std::numeric_limits<int64_t>::max() - relative) ||
            (relative < 0 && cluster < std::numeric_limits<int64_t>::min() - relative)) {
            error = "Matroska block timestamp is not representable";
            return false;
        }
        int64_t pts_ns = 0;
        if (!scale_timestamp(cluster + relative, timestamp_scale_ns_, pts_ns)) {
            error = "Matroska block timestamp is not representable in nanoseconds";
            return false;
        }

        block.payload_offset = pos_;
        block.payload_size = next_elem - pos_;
        block.decode_ordinal = packet_index_++;
        block.pts_ns = pts_ns;
        block.is_sync = (id == kSimpleBlock) && ((flags & 0x80) != 0);
        seek(next_elem);
        return true;
    }
}

bool MkvDemuxer::next(Packet& out, std::string& error) {
    error.clear();
    const Track& tk = tracks_[(size_t)selected_];
    Block block;
    if (!nextBlock(block, error)) return false;
    if (block.decode_ordinal >= tk.presentation_ranks.size()) {
        error = "Matroska presentation index does not match the selected track";
        return false;
    }

    out = Packet{};
    out.data.resize((size_t)block.payload_size);
    seek(block.payload_offset);
    if (!readBytes(out.data.data(), block.payload_size)) {
        error = "truncated Matroska block";
        return false;
    }

    FrameTiming& timing = out.timing;
    timing.kind = TimingKind::SourceExact;
    timing.stream_index = tk.info.stream_index;
    timing.discontinuity_segment = discontinuity_segment_;
    timing.decode_ordinal = block.decode_ordinal;
    timing.presentation_ordinal = tk.presentation_ranks[(size_t)block.decode_ordinal];
    timing.time_base_num = 1;
    timing.time_base_den = 1000000000;
    timing.pts = block.pts_ns;
    timing.has_pts = true;
    if (tk.default_duration_ns > 0) {
        if (tk.default_duration_ns > (uint64_t)std::numeric_limits<int64_t>::max()) {
            error = "Matroska DefaultDuration is not representable";
            return false;
        }
        timing.duration = (int64_t)tk.default_duration_ns;
        timing.has_duration = true;
    }
    out.is_sync = block.is_sync;
    return true;
}

// ================
// Factory
// ================

std::unique_ptr<Demuxer> open_demuxer(const std::string& path, std::string& error) {
    // By content, not extension: .insv is an MP4 and .webm is a Matroska.
    std::ifstream probe(path, std::ios::binary);
    if (!probe) {
        error = "cannot open '" + path + "'";
        return nullptr;
    }
    uint8_t head[12] = {};
    probe.read((char*)head, 12);
    const std::streamsize got = probe.gcount();
    probe.close();

    if (got >= 4 && head[0] == 0x1A && head[1] == 0x45 && head[2] == 0xDF && head[3] == 0xA3) {
        auto d = std::make_unique<MkvDemuxer>();
        if (!d->open(path, error)) return nullptr;
        return d;
    }
    if (got >= 8 && std::memcmp(head + 4, "ftyp", 4) == 0) {
        auto d = std::make_unique<Mp4Demuxer>();
        if (!d->open(path, error)) return nullptr;
        return d;
    }
    // Some MOV files lead with `moov`, `wide`, `free` or `mdat` instead of
    // `ftyp`; try MP4 anyway before giving up.
    {
        auto d = std::make_unique<Mp4Demuxer>();
        std::string mp4_error;
        if (d->open(path, mp4_error)) return d;
        error = "'" + path +
                "' is not a recognized container (expected ISO-BMFF or Matroska): " +
                mp4_error;
    }
    return nullptr;
}

}  // namespace video
