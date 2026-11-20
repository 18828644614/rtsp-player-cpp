#include "rtsp/rtsp_message.h"
#include "rtsp/rtsp_stream_parser.h"
#include "rtsp/sdp.h"
#include "rtp/packet_stats.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void check(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

template <class F>
void rejects(F action, const char* message) {
    try {
        action();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

void put32(std::string& data, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        data += static_cast<char>(value >> shift);
    }
}

std::string rtp(unsigned int pt = 104, std::uint32_t ssrc = 123) {
    std::string wire("\xb1\xe8\xff\xff", 4);  // P、X、一项 CSRC、marker、seq=65535。
    wire[1] = static_cast<char>(128 | pt);
    put32(wire, 0xffffffff);
    put32(wire, ssrc);
    put32(wire, 456);
    wire.append("\xbe\xde\0\x01", 4);  // 一组 4 字节扩展。
    wire += "ext!";
    wire += "abc";
    wire.append("\0\x02", 2);
    return wire;
}

std::string rtcp() {
    std::string wire("\x80\xc8\0\x06", 4);
    put32(wire, 123);
    put32(wire, 4);
    put32(wire, 5);
    put32(wire, 6);
    put32(wire, 7);
    put32(wire, 8);
    wire.append("\x81\xca\0\x02", 4);
    put32(wire, 123);
    wire.append("\x01\x01x\0", 4);
    wire.append("\x81\xcb\0\x01", 4);
    put32(wire, 123);
    return wire;
}

void test_stream() {
    rtsp::RtspMessage response;
    response.is_request = false;
    response.set_header("cSeQ", "9");
    response.body = std::string("a$\0b", 4);
    const auto text = rtsp::serialize_rtsp_message(response);
    const auto frame = rtsp::serialize_interleaved_frame(255, std::string("$\0\xff", 3));
    const auto combined = text + frame + text;
    // 每种切分点都必须得到相同的三条消息。
    for (std::size_t split = 0; split <= combined.size(); ++split) {
        rtsp::RtspStreamParser parser;
        rtsp::RtspStreamItem item;
        unsigned int count = 0;
        auto drain = [&] {
            while (parser.next(item)) {
                if (count == 1) {
                    check(
                        item.is_interleaved && item.channel == 255 && item.payload.size() == 3,
                        "binary frame changed"
                    );
                } else {
                    check(
                        !item.is_interleaved && item.message.body == response.body
                            && item.message.header("CSeq") == "9",
                        "response body changed"
                    );
                }
                ++count;
            }
        };
        parser.append(combined.data(), split);
        drain();
        parser.append(combined.data() + split, combined.size() - split);
        drain();
        check(count == 3, "half/sticky packets lost a message");
        parser.finish();
    }
    rtsp::RtspStreamParser bytewise;
    rtsp::RtspStreamItem item;
    for (std::size_t i = 0; i < frame.size(); ++i) {
        bytewise.append(frame.data() + i, 1);
        check(bytewise.next(item) == (i + 1 == frame.size()), "bytewise frame boundary failed");
    }
    rtsp::RtspStreamParser partial;
    partial.append(text.data(), text.size() - 1);
    check(!partial.next(item), "partial response completed");
    rejects([&] { partial.finish(); }, "partial EOF accepted");
    for (const auto& bad :
         {"RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx",
          "RTSP/1.0 200 OK\r\nCSeq: 1\r\nCSeq: 2\r\n\r\n", "RTSP/1.0 200 OK\r\nCSeq: -1\r\n\r\n",
          "RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 999999999999999\r\n\r\n"}) {
        rejects(
            [&] {
                rtsp::RtspStreamParser p;
                p.append(bad, std::string(bad).size());
                p.next(item);
            },
            "invalid RTSP accepted"
        );
    }
    rejects(
        [&] {
            rtsp::RtspStreamParser p;
            const std::string huge(2 * 1024 * 1024 + 1, 'a');
            p.append(huge.data(), huge.size());
        },
        "unbounded stream buffer"
    );
}

void test_sdp() {
    const auto base = std::string("rtsp://127.0.0.1:8554/live/");
    check(rtsp::resolve_control_url(base, "v") == base + "v", "relative control failed");
    check(
        rtsp::resolve_control_url(base, "/v") == "rtsp://127.0.0.1:8554/v", "root control failed"
    );
    check(
        rtsp::resolve_control_url(base, "rtsp://localhost:554/v") == "rtsp://localhost:554/v",
        "absolute control failed"
    );
    check(
        rtsp::resolve_control_url("rtsp://localhost/live", "v") == "rtsp://localhost/v",
        "file base failed"
    );
    check(rtsp::resolve_control_url(base, "*") == base, "aggregate resolution failed");
    rejects([&] { rtsp::resolve_control_url(base, "../v"); }, "unsupported dot segment accepted");
    check(rtsp::parse_rtsp_url("rtsp://localhost/live").port == 554, "default port failed");
    rejects([] { rtsp::parse_rtsp_url("rtsp://user@localhost/live"); }, "auth URL accepted");
    rejects([] { rtsp::parse_rtsp_url("rtsp://localhost:65536/live"); }, "bad port accepted");
    const std::string sdp = "v=0\r\na=control:*\r\n"
                            "m=audio 0 RTP/AVP 111\r\na=rtpmap:111 MPEG4-GENERIC/48000/1\r\n"
                            "a=fmtp:111 config=1188\r\na=control:a\r\n"
                            "m=video 0 RTP/AVP 104\r\na=rtpmap:104 H264/90000\r\na=control:v\r\n";
    const auto parsed = rtsp::parse_sdp(sdp);
    check(
        parsed.tracks.size() == 2 && parsed.tracks[0].payload_type == 111
            && parsed.tracks[1].control == "v",
        "SDP lost dynamic track information"
    );
    auto bad_channels = sdp;
    bad_channels.replace(bad_channels.find("48000/1"), 7, "48000/9");
    rejects([&] { rtsp::parse_sdp(bad_channels); }, "audio channel overflow accepted");
    rejects(
        [&] { rtsp::parse_sdp(sdp + "a=control:other\r\n"); }, "duplicate SDP control accepted"
    );
    rtsp::TcpTransport transport;
    check(
        rtsp::parse_tcp_transport("RTP/AVP/TCP;unicast;interleaved=6-7", transport)
            && transport.rtp_channel == 6,
        "transport parse failed"
    );
    check(
        !rtsp::parse_tcp_transport("RTP/AVP/TCP;unicast;interleaved=6-6", transport),
        "overlapping channel accepted"
    );
    const auto session = rtsp::parse_session_header("abc;timeout=2");
    check(session.id == "abc" && session.timeout_seconds == 2U, "Session parse failed");
    rejects([] { rtsp::parse_session_header("abc;timeout=0"); }, "zero timeout accepted");
    rejects(
        [] { rtsp::parse_session_header("abc;timeout=1;timeout=2"); }, "duplicate timeout accepted"
    );
}

void test_packets() {
    const auto header = rtsp::parse_rtp_header(rtp());
    check(
        header.payload_size == 3 && header.marker && header.sequence == 65535
            && header.timestamp == 0xffffffff,
        "RTP extension/padding decode failed"
    );
    rejects([] { rtsp::parse_rtp_header(std::string(11, '\0')); }, "short RTP accepted");
    auto bad = rtp();
    bad.back() = static_cast<char>(255);
    rejects([&] { rtsp::parse_rtp_header(bad); }, "bad RTP padding accepted");
    bad = rtp();
    bad[18] = static_cast<char>(255);
    rejects([&] { rtsp::parse_rtp_header(bad); }, "bad RTP extension accepted");
    rtsp::PacketStats stats;
    rtsp::count_rtcp(stats, rtcp());  // SR 可先于首个 RTP 到达。
    rtsp::count_rtp(stats, rtp(), 104);
    auto wrapped = rtp();
    wrapped[2] = wrapped[3] = 0;
    rtsp::count_rtp(stats, wrapped, 104);
    check(
        stats.rtp_packets == 2 && stats.payload_bytes == 6 && stats.last_sequence == 0,
        "RTP wrap/count failed"
    );
    check(
        stats.rtcp_frames == 1 && stats.rtcp_packets == 3 && stats.sr == 1 && stats.sdes == 1
            && stats.bye == 1 && stats.latest_sr->rtp_timestamp == 6
            && stats.latest_sr->ntp == ((std::uint64_t{4} << 32) | 5),
        "RTCP compound count failed"
    );
    rejects([&] { rtsp::count_rtp(stats, rtp(105), 104); }, "wrong PT accepted");
    rejects([&] { rtsp::count_rtp(stats, rtp(104, 456), 104); }, "wrong SSRC accepted");
    rejects(
        [&] { rtsp::count_rtcp(stats, rtcp().substr(0, rtcp().size() - 1)); },
        "truncated RTCP accepted"
    );
    check(
        stats.rtp_packets == 2 && stats.rtcp_frames == 1, "invalid packet partially committed stats"
    );
    auto wrong_source = rtcp();
    wrong_source[7] = 124;
    rejects([&] { rtsp::count_rtcp(stats, wrong_source); }, "wrong RTCP SSRC accepted");
    check(stats.rtcp_frames == 1, "bad compound partially counted");
}

}  // namespace

int main() {
    try {
        test_stream();
        test_sdp();
        test_packets();
        std::cout
            << "PASS: stream boundaries, SDP/control, transport/session, RTP/RTCP statistics\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
