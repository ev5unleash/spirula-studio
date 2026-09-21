#include "core/Env.h"
#include "core/Sha256.h"
#include "nn/Device.h"
#include "video/Demuxer.h"
#include "video/VideoPipeline.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
    std::printf("ok   %s\n", message.c_str());
}

std::string sha256(const std::vector<uint8_t>& bytes) {
    spirula::Sha256 hash;
    hash.update(bytes.data(), bytes.size());
    return hash.hex();
}

std::vector<video::Packet> readTrack(const fs::path& path, int track) {
    std::string error;
    auto demux = video::open_demuxer(path.u8string(), error);
    require(demux != nullptr, "open " + path.filename().u8string() + ": " + error);
    require(demux->selectTrack(track, error),
            "select track " + std::to_string(track) + ": " + error);
    std::vector<video::Packet> packets;
    video::Packet packet;
    while (demux->next(packet, error)) packets.push_back(std::move(packet));
    require(error.empty(), "read track " + std::to_string(track) + ": " + error);
    return packets;
}

template <size_t N>
void checkTrack(const std::vector<video::Packet>& packets,
                const std::array<int64_t, N>& pts_ms,
                const std::array<uint64_t, N>& presentation, uint32_t stream_index) {
    require(packets.size() == N, "packet count");
    for (size_t i = 0; i < N; ++i) {
        const video::FrameTiming& timing = packets[i].timing;
        require(timing.kind == video::TimingKind::SourceExact, "source-exact timing");
        require(timing.decode_ordinal == i, "decode ordinal " + std::to_string(i));
        require(timing.presentation_ordinal == presentation[i],
                "presentation ordinal " + std::to_string(i));
        require(timing.stream_index == stream_index && timing.discontinuity_segment == 0,
                "stream and segment identity");
        require(timing.has_pts && timing.pts == pts_ms[i] * 1000000,
                "PTS " + std::to_string(i));
        require(!timing.has_dts, "DTS remains unavailable");
        require(timing.has_duration && timing.duration == 200000000,
                "default duration");
        require(timing.time_base_num == 1 && timing.time_base_den == 1000000000,
                "nanosecond time base");
    }
}

void checkDemux(const fs::path& fixtures) {
    const fs::path gap = fixtures / "bframes_gap.mkv";
    const fs::path multi = fixtures / "equal_pts_two_tracks.mkv";
    const fs::path invalid = fixtures / "zero_timestamp_scale.mkv";
    require(spirula::sha256_file(gap.u8string()) ==
                "0d8341087fb31530520e4cb7fda70f78c7ea572df7c53f37899908c3a15a87b0",
            "gap fixture hash");
    require(spirula::sha256_file(multi.u8string()) ==
                "dc7b90408d23ce1b69d4628704626482d5ea266fe97baff6ff17d844dd870d2d",
            "multi-track fixture hash");
    require(spirula::sha256_file(invalid.u8string()) ==
                "a40654b153ee2998760e3e2da85c798ed5856a230eb94ef2f98f8cd1cd21b49f",
            "invalid fixture hash");

    const auto gap_packets = readTrack(gap, 0);
    checkTrack(gap_packets,
               std::array<int64_t, 8>{0, 1000, 200, 400, 1600, 1200, 1400, 1800},
               std::array<uint64_t, 8>{0, 3, 1, 2, 6, 4, 5, 7}, 0);

    std::string error;
    auto demux = video::open_demuxer(multi.u8string(), error);
    require(demux != nullptr, "open multi-track fixture: " + error);
    require(demux->tracks().size() == 2, "two video tracks");
    require(demux->tracks()[0].stream_id == "matroska:track:1" &&
                demux->tracks()[1].stream_id == "matroska:track:2",
            "stable Matroska track identities");

    checkTrack(readTrack(multi, 0), std::array<int64_t, 6>{0, 200, 0, 200, 400, 400},
               std::array<uint64_t, 6>{0, 2, 1, 3, 4, 5}, 0);
    checkTrack(readTrack(multi, 1), std::array<int64_t, 6>{0, 200, 400, 600, 800, 1000},
               std::array<uint64_t, 6>{0, 1, 2, 3, 4, 5}, 1);

    error.clear();
    require(!video::open_demuxer(invalid.u8string(), error) && !error.empty(),
            "zero TimestampScale is rejected");
}

template <size_t N>
void checkPipelineOrder(video::VideoPipeline& pipeline,
                         const std::array<int64_t, N>& pts_ms,
                         const std::array<const char*, N>* luma_hashes = nullptr) {
    std::string error;
    std::string luma_errors;
    for (size_t i = 0; i < N; ++i) {
        video::FrameHandle frame;
        require(pipeline.next(frame, error), "decode frame " + std::to_string(i) + ": " + error);
        require(frame.timing.presentation_ordinal == i,
                "pipeline presentation ordinal " + std::to_string(i));
        require(frame.timing.has_pts && frame.timing.pts == pts_ms[i] * 1000000,
                "pipeline PTS " + std::to_string(i));
        if (luma_hashes) {
            std::vector<uint8_t> gray;
            require(pipeline.toGray(frame, 64, 48, gray, error),
                    "download luma " + std::to_string(i) + ": " + error);
            const std::string got = sha256(gray);
            if (got == (*luma_hashes)[i]) {
                require(true, "decoded content " + std::to_string(i));
            } else {
                luma_errors += "\n  frame " + std::to_string(i) + ": expected " +
                               (*luma_hashes)[i] + ", got " + got;
            }
        }
        pipeline.release(frame);
    }
    video::FrameHandle extra;
    require(!pipeline.next(extra, error) && error.empty(), "pipeline EOF");
    require(luma_errors.empty(), "decoded content mismatches:" + luma_errors);
}

void checkPipeline(const fs::path& fixtures) {
    const char* selector = spirula::env("TEST_DEVICE");
    require(selector && *selector, "SS_TEST_DEVICE explicitly selects the test GPU");
    nn::configure_device(selector);

    const std::array<const char*, 8> luma_hashes = {
        "ed4400cfdcf3fb8909dd2adc847a8e6b3bdb196b1840b384dc9c8bc4feb924a9",
        "2c485ee4b3fb8d84e0aacac75e9891a23f857682558803f7b6fc9f9880390226",
        "23d75a14738069b6ec27af0ec08b25f5ca861f229e2198fda2de7377d0d2a672",
        "0c907b4c3e219860965f6c654b1f8030d70fbc4bde235a87342c8fe51111ebdd",
        "00b47e8c818033ee29a107e54fc81ca9aee2f4c0078e37f4dca8bbf13474d29b",
        "27a2611e5018cd527893659182e9246c3e4a0ff776699689bf74c110f7b716b2",
        "b9bab2a3e787fdae2e135dd16a1d1b1b6f59327a9a798a1c492faad474f5ecd5",
        "e2884291859509f113bc43570c83fde61560b0dc30d6383d0e1b84cd35e64210",
    };
    std::string error;
    video::VideoPipeline gap;
    require(gap.open((fixtures / "bframes_gap.mkv").u8string(), 0, 0, error),
            "open gap pipeline: " + error);
    checkPipelineOrder(gap, std::array<int64_t, 8>{0, 200, 400, 1000, 1200, 1400, 1600, 1800},
                       &luma_hashes);

    video::VideoPipeline equal;
    require(equal.open((fixtures / "equal_pts_two_tracks.mkv").u8string(), 0, 0, error),
            "open equal-PTS pipeline: " + error);
    checkPipelineOrder(equal, std::array<int64_t, 6>{0, 0, 200, 200, 400, 400});
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        require(argc == 2 || (argc == 3 && std::string(argv[2]) == "--demux-only"),
                "usage: mkv_timing_test <fixture-directory> [--demux-only]");
        const fs::path fixtures = fs::u8path(argv[1]);
        checkDemux(fixtures);
        if (argc == 2) checkPipeline(fixtures);
        std::puts("Matroska timing fixture passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
