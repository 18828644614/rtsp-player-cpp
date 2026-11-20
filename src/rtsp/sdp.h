#pragma once

#include <optional>
#include <string>
#include <vector>

namespace rtsp {

// 每个 m= 段对应一条轨道。P1 接受一条 H.264 和一条 MPEG4-GENERIC 音频。
struct MediaTrack {
    std::string media_type;
    std::string control;
    int payload_type = -1;
    std::string encoding;
    int clock_rate = 0;
    int channels = 0;
    std::string fmtp;
};

struct SessionDescription {
    std::string control;
    std::vector<MediaTrack> tracks;
};

struct TcpTransport {
    int rtp_channel = 0;
    int rtcp_channel = 1;
};

struct SessionHeader {
    std::string id;
    std::optional<unsigned int> timeout_seconds;
};

struct RtspUrl {
    std::string host;
    unsigned short port = 554;
    std::string origin;
};

// 纯解析函数：非法输入抛异常；Transport 不支持时返回 false。
SessionDescription parse_sdp(const std::string& text);
bool parse_tcp_transport(const std::string& text, TcpTransport& transport);
std::string make_tcp_transport(const TcpTransport& transport);
SessionHeader parse_session_header(const std::string& text);
RtspUrl parse_rtsp_url(const std::string& url);

// 支持绝对 URL、根路径、相对路径和 '*'；相对路径按 base 的目录解析。
std::string resolve_control_url(const std::string& base, const std::string& control);

}  // namespace rtsp
