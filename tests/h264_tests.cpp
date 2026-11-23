#include "rtp/h264_depacketizer.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

const std::string fmtp = "packetization-mode=1;sprop-parameter-sets=Z0IAHg==,aM4G4g==";
const std::string sps("\x67\x42\0\x1e", 4);
const std::string pps("\x68\xce\x06\xe2", 4);
const std::string idr("\x65\x11\x22\x33", 4);
const std::string predicted("\x41\x11", 2);
const std::string aud("\x09\xf0", 2);

void check(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

template <class F>
void rejects(F action, const char* message) {
    try {
        action();
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error(message);
}

std::string annex_b(const std::string& nalu) {
    return std::string("\0\0\0\1", 4) + nalu;
}

struct Stream {
    rtsp::H264Depacketizer depacketizer;
    std::uint16_t sequence = 1;

    explicit Stream(std::size_t limit = 8 * 1024 * 1024) : depacketizer(limit) {
        depacketizer.configure(fmtp);
    }

    std::optional<rtsp::H264AccessUnit>
    send(std::uint32_t timestamp, const std::string& payload, bool marker = true) {
        rtsp::RtpHeader header;
        header.sequence = sequence++;
        header.timestamp = timestamp;
        header.marker = marker;
        return depacketizer.push(header, payload);
    }

    void boundary() {
        check(!send(0, aud), "first observed AU was submitted");
    }

    void start_decoding() {
        boundary();
        check(send(3000, idr).has_value(), "initial complete IDR missing");
    }
};

void test_parameters() {
    const auto parameters = rtsp::parse_h264_parameters(
        " profile-level-id=42001e ; sprop-parameter-sets=Z0IAHg==,aM4G4g== ; packetization-mode=1 "
    );
    check(
        parameters.packetization_mode == 1 && parameters.sps == sps && parameters.pps == pps,
        "SDP parameter sets changed"
    );
    check(rtsp::parse_h264_parameters("").packetization_mode == 0, "default mode is not zero");
    for (const auto& invalid :
         {"packetization-mode=2", "packetization-mode=1;packetization-mode=1",
          "sprop-parameter-sets=Z0IAHg==", "sprop-parameter-sets=Z0IAHg==,aM4G4g==,aA==",
          "sprop-parameter-sets=Z0IAHx==,aM4G4g==", "sprop-parameter-sets=Z0IAHg=!,aM4G4g==",
          "sprop-parameter-sets=aM4G4g==,Z0IAHg==", "sprop-parameter-sets=,aM4G4g==",
          "sprop-parameter-sets=Z0IAHg==,aM4G4g==;sprop-parameter-sets=Z0IAHg==,aM4G4g=="}) {
        rejects([&] { rtsp::parse_h264_parameters(invalid); }, "invalid fmtp accepted");
    }
    const std::string oversized = "sprop-parameter-sets=" + std::string(100000, 'A') + ",aA==";
    rejects([&] { rtsp::parse_h264_parameters(oversized); }, "oversized SDP parameter accepted");
}

void test_access_units() {
    Stream stream;
    // 即便首包看似 IDR，也可能只是多 slice 图像的最后一个 slice。
    check(!stream.send(0, idr), "partial initial IDR was submitted");
    check(!stream.send(3000, predicted), "predicted frame decoded before IDR");
    check(!stream.send(6000, aud, false), "AUD became a frame");
    check(!stream.send(6000, idr, false), "AU completed before marker");
    const auto au = stream.send(6000, idr);
    check(
        au && au->is_idr && au->reset_decoder && au->rtp_timestamp == 6000,
        "complete multi-NAL IDR metadata missing"
    );
    check(
        au->data == annex_b(sps) + annex_b(pps) + annex_b(aud) + annex_b(idr) + annex_b(idr),
        "Annex B NAL order changed"
    );
    const auto next = stream.send(9000, predicted);
    check(
        next && !next->is_idr && !next->reset_decoder && next->data == annex_b(predicted),
        "ordinary predicted AU changed"
    );
    check(stream.depacketizer.stats().access_units == 2, "AU count incorrect");
}

void test_fu_a_and_wrap() {
    Stream stream;
    stream.sequence = 65533;
    stream.boundary();
    check(!stream.send(3000, std::string("\x7c\x85\x11", 3), false), "FU start emitted AU");
    // R 位必须忽略；16 位序号在中间片和结束片之间回绕。
    check(!stream.send(3000, std::string("\x7c\x25\x22", 3), false), "FU middle emitted AU");
    check(!stream.send(3000, std::string("\x7c\x45\x33", 3), false), "FU end acted as marker");
    const auto au = stream.send(3000, aud);
    check(
        au && au->data == annex_b(sps) + annex_b(pps) + annex_b(idr) + annex_b(aud),
        "FU-A reconstruction or sequence wrap failed"
    );
}

void test_loss_and_recovery() {
    Stream stream;
    stream.start_decoding();
    stream.send(6000, std::string("\x7c\x81\x11", 3), false);
    ++stream.sequence;  // 缺失一个分片。
    check(!stream.send(6000, std::string("\x7c\x41\x33", 3)), "incomplete FU reached decoder");
    check(!stream.send(9000, predicted), "dependent frame submitted after loss");
    const auto recovered = stream.send(12000, idr);
    check(recovered && recovered->reset_decoder, "next complete IDR did not recover");

    ++stream.sequence;  // 丢掉整个 AU，下一 AU 也不能假定从头收齐。
    check(!stream.send(18000, idr), "ambiguous IDR accepted across sequence gap");
    check(stream.send(21000, idr)->reset_decoder, "recovery after whole-AU loss failed");
}

void test_missing_marker_and_finish() {
    Stream stream;
    stream.start_decoding();
    stream.send(6000, predicted, false);
    check(!stream.send(9000, predicted), "new timestamp did not discard unfinished AU");
    check(stream.send(12000, idr)->reset_decoder, "IDR after missing marker did not recover");
    stream.send(15000, std::string("\x7c\x81\x11", 3), false);
    const auto before = stream.depacketizer.stats().dropped;
    stream.depacketizer.finish();
    check(stream.depacketizer.stats().dropped == before + 1, "unfinished final AU not dropped");
    stream.depacketizer.finish();
    check(stream.depacketizer.stats().dropped == before + 1, "finish counted AU twice");
}

void test_invalid_payloads() {
    for (const auto& payload :
         {std::string{}, std::string("\xe5\x11", 2), std::string("\x78\x11", 2),
          std::string("\x7c", 1), std::string("\x7c\xc5\x11", 3), std::string("\x7c\x80\x11", 3),
          std::string("\x7c\x45\x11", 3)}) {
        Stream stream;
        stream.start_decoding();
        rejects([&] { stream.send(6000, payload); }, "malformed H264 payload accepted");
        check(stream.depacketizer.stats().invalid == 1, "invalid H264 count missing");
        check(!stream.send(9000, predicted), "bad payload did not invalidate references");
        check(stream.send(12000, idr)->reset_decoder, "bad payload prevented IDR recovery");
    }
}

void test_unfinished_fragments() {
    for (const auto& continuation :
         {idr, std::string("\x7c\x85\x11", 3), std::string("\x5c\x45\x11", 3)}) {
        Stream stream;
        stream.start_decoding();
        stream.send(6000, std::string("\x7c\x85\x11", 3), false);
        rejects([&] { stream.send(6000, continuation); }, "FU-A boundary/header mismatch accepted");
        check(stream.send(9000, idr)->reset_decoder, "FU mismatch prevented recovery");
    }
    Stream stream;
    stream.boundary();
    rejects(
        [&] { stream.send(3000, std::string("\x7c\x85\x11", 3)); }, "marker before FU end accepted"
    );
}

void test_inband_parameters() {
    Stream stream;
    stream.depacketizer.configure("");
    stream.send(0, sps, false);
    stream.send(0, pps);
    const auto initial = stream.send(3000, idr);
    check(
        initial && initial->data == annex_b(sps) + annex_b(pps) + annex_b(idr),
        "in-band parameter sets not used"
    );

    const auto changed_sps = sps + std::string("\x80", 1);
    stream.send(6000, changed_sps, false);
    check(!stream.send(6000, predicted), "parameter change did not wait for IDR");
    const auto changed = stream.send(9000, idr);
    check(
        changed && changed->reset_decoder
            && changed->data == annex_b(changed_sps) + annex_b(pps) + annex_b(idr),
        "new SPS not applied at IDR"
    );

    stream.depacketizer.discard();  // 外部 RTP 校验/解码错误也必须触发恢复。
    check(!stream.send(12000, predicted), "external discard ignored");
    check(stream.send(15000, idr)->reset_decoder, "external discard prevented IDR recovery");
}

void test_limits_and_mode() {
    Stream stream(64);
    stream.start_decoding();
    const std::string large_nalu = std::string("\x41", 1) + std::string(64, 'x');
    rejects([&] { stream.send(6000, large_nalu); }, "oversized AU accepted");
    check(stream.send(9000, idr)->reset_decoder, "size limit prevented recovery");

    stream.send(12000, std::string("\x7c\x81", 2) + std::string(40, 'x'), false);
    rejects(
        [&] { stream.send(12000, std::string("\x7c\x41", 2) + std::string(40, 'x')); },
        "oversized FU-A accepted"
    );

    Stream single_only;
    single_only.depacketizer.configure("packetization-mode=0");
    single_only.boundary();
    rejects(
        [&] { single_only.send(3000, std::string("\x7c\x85\x11", 3), false); },
        "FU-A accepted in single-NAL mode"
    );

    Stream no_parameters;
    no_parameters.depacketizer.configure("packetization-mode=1");
    no_parameters.boundary();
    check(!no_parameters.send(3000, idr), "IDR submitted without SPS/PPS");
}

}  // namespace

int main() {
    try {
        test_parameters();
        test_access_units();
        test_fu_a_and_wrap();
        test_loss_and_recovery();
        test_missing_marker_and_finish();
        test_invalid_payloads();
        test_unfinished_fragments();
        test_inband_parameters();
        test_limits_and_mode();
        std::cout << "PASS: H264 single NAL/FU-A/AU, parameters, limits and IDR recovery\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
