#include "video/CodecDecoder.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

class BitWriter {
public:
    void bit(uint32_t value) {
        if (used_ == 0) bytes_.push_back(0);
        if (value & 1u) bytes_.back() |= (uint8_t)(1u << (7 - used_));
        used_ = (used_ + 1) & 7;
    }

    void bits(uint64_t value, int count) {
        for (int i = count - 1; i >= 0; --i) bit((uint32_t)(value >> i));
    }

    void ue(uint32_t value) {
        const uint64_t code = (uint64_t)value + 1;
        int width = 0;
        for (uint64_t x = code; x; x >>= 1) ++width;
        for (int i = 1; i < width; ++i) bit(0);
        bits(code, width);
    }

    void se(int32_t value) {
        ue(value <= 0 ? (uint32_t)(-2LL * value) : (uint32_t)(2LL * value - 1));
    }

    std::vector<uint8_t> finish() {
        bit(1);
        while (used_) bit(0);
        return bytes_;
    }

private:
    std::vector<uint8_t> bytes_;
    int used_ = 0;
};

void write_ptl(BitWriter& w, uint8_t level_idc) {
    w.bits(0, 2);  // profile_space
    w.bit(0);      // tier
    w.bits(1, 5);  // Main profile
    w.bits(0, 32); // profile compatibility
    w.bit(1);      // progressive source
    w.bit(0);      // interlaced source
    w.bit(0);      // non-packed constraint
    w.bit(1);      // frame-only constraint
    w.bits(0, 43); // remaining constraint flags
    w.bit(0);      // inbld / reserved
    w.bits(level_idc, 8);
}

std::vector<uint8_t> make_nal(uint8_t type, BitWriter& w) {
    const std::vector<uint8_t> rbsp = w.finish();
    std::vector<uint8_t> nal{(uint8_t)(type << 1), 1};
    int zeros = 0;
    for (uint8_t b : rbsp) {
        if (zeros >= 2 && b <= 3) {
            nal.push_back(3);
            zeros = 0;
        }
        nal.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
    return nal;
}

std::vector<uint8_t> make_vps(uint8_t level_idc) {
    BitWriter w;
    w.bits(0, 4);  // VPS id
    w.bits(0, 2);  // base-layer flags
    w.bits(0, 6);  // max_layers_minus1
    w.bits(0, 3);  // max_sub_layers_minus1
    w.bit(1);      // temporal_id_nesting
    w.bits(0xffff, 16);
    write_ptl(w, level_idc);
    w.bit(1);      // ordering info for every sub-layer
    w.ue(0);
    w.ue(0);
    w.ue(0);
    return make_nal(32, w);
}

std::vector<uint8_t> make_sps(uint32_t id, uint8_t level_idc,
                              uint32_t max_dec_pic_buffering_minus1 = 0) {
    BitWriter w;
    w.bits(0, 4);  // VPS id
    w.bits(0, 3);  // max_sub_layers_minus1
    w.bit(1);      // temporal_id_nesting
    write_ptl(w, level_idc);
    w.ue(id);
    w.ue(1);       // 4:2:0
    w.ue(16);      // width
    w.ue(16);      // height
    w.bit(0);      // no conformance window
    w.ue(0);       // luma bit_depth_minus8
    w.ue(0);       // chroma bit_depth_minus8
    w.ue(0);       // log2_max_pic_order_cnt_lsb_minus4
    w.bit(1);      // ordering info for every sub-layer
    w.ue(max_dec_pic_buffering_minus1); w.ue(0); w.ue(0);
    for (int i = 0; i < 6; ++i) w.ue(0);
    w.bit(0);      // scaling_list_enabled
    w.bit(0);      // amp
    w.bit(0);      // sample_adaptive_offset
    w.bit(0);      // pcm
    w.ue(0);       // short-term reference picture sets
    w.bit(0);      // long-term reference pictures
    w.bit(0);      // temporal_mvp
    w.bit(0);      // strong_intra_smoothing
    w.bit(0);      // VUI
    return make_nal(33, w);
}

std::vector<uint8_t> make_pps(uint32_t id, uint32_t sps_id, int32_t init_qp_delta = 0) {
    BitWriter w;
    w.ue(id);
    w.ue(sps_id);
    w.bit(0);      // dependent_slice_segments_enabled
    w.bit(0);      // output_flag_present
    w.bits(0, 3);  // num_extra_slice_header_bits
    w.bit(0);      // sign_data_hiding
    w.bit(0);      // cabac_init_present
    w.ue(0); w.ue(0);
    w.se(init_qp_delta);
    w.bit(0); w.bit(0); w.bit(0); // constrained intra, transform skip, CU QP delta
    w.se(0); w.se(0);
    w.bit(0); w.bit(0); w.bit(0); w.bit(0); w.bit(0); // chroma QP, weighted, bypass, tiles
    w.bit(0);      // entropy coding sync
    w.bit(0);      // loop filter across slices
    w.bit(0);      // deblocking control
    w.bit(0);      // PPS scaling lists
    w.bit(0);      // list modification
    w.ue(0);       // log2_parallel_merge_level_minus2
    w.bit(0);      // slice header extension
    w.bit(0);      // PPS extension
    return make_nal(34, w);
}

std::vector<uint8_t> make_idr_slice(uint32_t pps_id) {
    BitWriter w;
    w.bit(1);      // first_slice_segment_in_pic_flag
    w.bit(0);      // no_output_of_prior_pics_flag
    w.ue(pps_id);
    w.ue(2);       // I slice
    return make_nal(19, w);
}

void append_array(std::vector<uint8_t>& cfg, uint8_t type,
                  const std::vector<std::vector<uint8_t>>& nals) {
    cfg.push_back(type);
    cfg.push_back((uint8_t)(nals.size() >> 8));
    cfg.push_back((uint8_t)nals.size());
    for (const auto& nal : nals) {
        cfg.push_back((uint8_t)(nal.size() >> 8));
        cfg.push_back((uint8_t)nal.size());
        cfg.insert(cfg.end(), nal.begin(), nal.end());
    }
}

std::vector<uint8_t> make_config(const std::vector<std::vector<uint8_t>>& spss,
                                 const std::vector<std::vector<uint8_t>>& ppss,
                                 uint8_t vps_level = 153) {
    std::vector<uint8_t> cfg(23, 0);
    cfg[0] = 1;
    cfg[22] = 3;
    append_array(cfg, 32, {make_vps(vps_level)});
    append_array(cfg, 33, spss);
    append_array(cfg, 34, ppss);
    return cfg;
}

std::unique_ptr<video::CodecDecoder> make_decoder(
    const std::vector<std::vector<uint8_t>>& spss,
    const std::vector<std::vector<uint8_t>>& ppss,
    std::string& error, uint8_t vps_level = 153) {
    video::TrackInfo track;
    track.codec = video::Codec::H265;
    track.width = 16;
    track.height = 16;
    track.codec_config = make_config(spss, ppss, vps_level);
    auto decoder = video::make_codec_decoder(track.codec);
    if (!decoder || !decoder->init(track, error)) return {};
    return decoder;
}

std::vector<uint8_t> make_access_unit(const std::vector<std::vector<uint8_t>>& nals) {
    std::vector<uint8_t> au;
    for (const auto& nal : nals) {
        const uint32_t size = (uint32_t)nal.size();
        au.push_back((uint8_t)(size >> 24));
        au.push_back((uint8_t)(size >> 16));
        au.push_back((uint8_t)(size >> 8));
        au.push_back((uint8_t)size);
        au.insert(au.end(), nal.begin(), nal.end());
    }
    return au;
}

bool decode(video::CodecDecoder& decoder, const std::vector<std::vector<uint8_t>>& nals,
            video::PictureInfo& picture, std::string& error) {
    std::vector<uint8_t> bitstream;
    std::vector<uint32_t> offsets;
    const std::vector<uint8_t> au = make_access_unit(nals);
    return decoder.decodeFrame(au.data(), au.size(), 4, bitstream, offsets, picture, error);
}

void test_level_encodings() {
    constexpr std::array<std::pair<uint8_t, int>, 13> levels{{
        {30, 10}, {60, 20}, {63, 21}, {90, 30}, {93, 31}, {120, 40}, {123, 41},
        {150, 50}, {153, 51}, {156, 52}, {180, 60}, {183, 61}, {186, 62},
    }};
    for (const auto& level : levels) {
        std::string error;
        auto decoder = make_decoder({make_sps(0, level.first)}, {make_pps(0, 0)}, error,
                                    level.first);
        check(decoder && decoder->format().hevc_level_x10 == level.second,
              "HEVC level IDC maps to normalized level");
    }

    for (uint8_t invalid : {uint8_t(0), uint8_t(31), uint8_t(181), uint8_t(187), uint8_t(255)}) {
        std::string error;
        auto decoder = make_decoder({make_sps(0, invalid)}, {make_pps(0, 0)}, error);
        check(!decoder, "reserved HEVC level IDC is rejected");
    }
    check(video::hevc_level_supported(50, 51), "level below device maximum is admitted");
    check(video::hevc_level_supported(51, 51), "level equal to device maximum is admitted");
    check(!video::hevc_level_supported(52, 51), "level above device maximum is rejected");
    check(!video::hevc_level_supported(0, 62), "missing level is not permission");
}

void test_all_sps_ordering_and_selection() {
    const auto sps0 = make_sps(0, 153);  // 5.1
    const auto sps1 = make_sps(1, 180, 16);  // 6.0, pathological DPB requirement
    for (bool reverse : {false, true}) {
        std::string error;
        std::vector<std::vector<uint8_t>> spss = reverse
            ? std::vector<std::vector<uint8_t>>{sps1, sps0}
            : std::vector<std::vector<uint8_t>>{sps0, sps1};
        auto decoder = make_decoder(spss, {make_pps(0, 0), make_pps(1, 1)}, error);
        check(decoder && decoder->sequenceFormatCount() == 2,
              "both configured SPSs are retained");
        if (!decoder) continue;
        check(video::required_hevc_level_x10(*decoder) == 60,
              "all-SPS required level is independent of hvcC ordering");
        auto above = video::admit_hevc_sequence(*decoder, 51);
        check(above.status == video::HevcAdmissionResult::Status::UnsupportedLevel &&
                  above.required_level_x10 == 60,
              "an unsupported non-final SPS rejects initial admission");
        auto below = video::admit_hevc_sequence(*decoder, 61);
        check(below.status == video::HevcAdmissionResult::Status::Compatible,
              "all configured SPS levels below the capability maximum are admitted");
        auto boundary = video::admit_hevc_sequence(*decoder, 60);
        check(boundary.status == video::HevcAdmissionResult::Status::Compatible,
              "all configured SPS levels at the capability boundary are admitted");
        auto dpb_over = video::admit_hevc_sequence(*decoder, 60, 16, 16);
        check(decoder->format().max_dpb_slots == 17 &&
                  dpb_over.status ==
                      video::HevcAdmissionResult::Status::UnsupportedDpbSlots &&
                  dpb_over.required_dpb_slots == 17 &&
                  dpb_over.supported_dpb_slots == 16,
              "a non-final SPS whose DPB requirement exceeds the device cap is rejected");

        video::PictureInfo picture;
        check(decode(*decoder, {make_idr_slice(0)}, picture, error), "decode picture using SPS 0");
        decoder->commitFrame(); // createParameters() acknowledges initial hvcC sets in production
        check(decode(*decoder, {make_idr_slice(0)}, picture, error), "decode repeated SPS 0 picture");
        auto selected0 = video::admit_hevc_picture(picture, 51);
        check(picture.sequence_format.hevc_level_x10 == 51 &&
                  selected0.status == video::HevcAdmissionResult::Status::Compatible,
              "PPS selects and admits its own SPS level");
        decoder->commitFrame();
        check(decode(*decoder, {make_idr_slice(1)}, picture, error), "decode picture using SPS 1");
        auto selected1 = video::admit_hevc_picture(picture, 51);
        check(picture.sequence_format.hevc_level_x10 == 60 &&
                  selected1.status == video::HevcAdmissionResult::Status::UnsupportedLevel,
              "active PPS-selected SPS is checked before submission");
    }
}

void test_parameter_update_boundary() {
    const auto vps = make_vps(153);
    const auto sps = make_sps(0, 153);
    const auto pps = make_pps(0, 0);
    std::string error;
    auto decoder = make_decoder({sps}, {pps}, error);
    check((bool)decoder, "initial HEVC parameter sets parse");
    if (!decoder) return;

    video::PictureInfo picture;
    check(decode(*decoder, {make_idr_slice(0)}, picture, error), "initial configured picture parses");
    decoder->commitFrame();
    check(decode(*decoder, {vps, sps, pps, make_idr_slice(0)}, picture, error),
          "identical repeated VPS/SPS/PPS parse");
    check(!picture.params_changed &&
              video::admit_hevc_picture(picture, 51).status ==
                  video::HevcAdmissionResult::Status::Compatible,
          "identical repeated parameter headers preserve the session boundary");
    decoder->commitFrame();

    const auto changed_sps = make_sps(0, 150); // 5.0 remains within the 5.1 cap
    check(decode(*decoder, {changed_sps, make_idr_slice(0)}, picture, error),
          "in-band SPS replacement parses");
    auto changed = video::admit_hevc_picture(picture, 51);
    check(picture.params_changed && picture.sequence_format.hevc_level_x10 == 50 &&
              changed.status == video::HevcAdmissionResult::Status::ParametersChanged,
          "changed in-band SPS is rejected until session parameters are updated");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    test_level_encodings();
    test_all_sps_ordering_and_selection();
    test_parameter_update_boundary();
    std::printf("%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
