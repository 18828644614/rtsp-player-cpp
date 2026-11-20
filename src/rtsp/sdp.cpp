// SDP、Transport、Session 和控制 URL 解析参考配套 rtsp-cpp。
#include "rtsp/sdp.h"

#include <sstream>
#include <stdexcept>

namespace rtsp {
namespace {

int read_number(const std::string& text, int maximum) {
    if (text.empty()) {
        throw std::runtime_error("empty SDP number");
    }
    int number = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const int digit = text[i] - '0';
        if (digit < 0 || digit > 9 || number > maximum / 10
            || (number == maximum / 10 && digit > maximum % 10)) {
            throw std::runtime_error("invalid SDP number");
        }
        number = number * 10 + digit;
    }
    return number;
}

void check_line_value(const std::string& value) {
    if (value.find_first_of("\r\n") != std::string::npos) {
        throw std::runtime_error("line break in SDP value");
    }
}

// 校验 SDP 中明确给出的轨道参数。
void check_track(const MediaTrack& track) {
    check_line_value(track.control);
    check_line_value(track.fmtp);
    if (track.payload_type < 96 || track.payload_type > 127 || track.control.empty()
        || track.control == "*" || track.control.find_first_of(" \t") != std::string::npos) {
        throw std::runtime_error("invalid SDP track control or payload type");
    }
    if (track.media_type == "video" && track.encoding == "H264" && track.clock_rate == 90000
        && track.channels == 0) {
        return;
    }
    if (track.media_type == "audio" && track.encoding == "MPEG4-GENERIC" && track.clock_rate > 0
        && track.channels >= 1 && track.channels <= 8 && !track.fmtp.empty()) {
        return;
    }
    throw std::runtime_error("expected H264/90000 or MPEG4-GENERIC audio");
}

void check_description(const SessionDescription& description) {
    check_line_value(description.control);
    if (description.tracks.empty()) {
        throw std::runtime_error("SDP requires at least one track");
    }
    for (std::size_t i = 0; i < description.tracks.size(); ++i) {
        check_track(description.tracks[i]);
        for (std::size_t j = 0; j < i; ++j) {
            if (description.tracks[i].control == description.tracks[j].control
                || description.tracks[i].payload_type == description.tracks[j].payload_type) {
                throw std::runtime_error("duplicate SDP control or payload type");
            }
        }
    }
}

}  // namespace

SessionDescription parse_sdp(const std::string& text) {
    SessionDescription description;
    description.control.clear();
    description.tracks.clear();
    bool has_version = false;
    bool has_session_control = false;
    bool has_rtpmap = false;
    bool has_fmtp = false;
    bool has_track_control = false;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (line.size() < 2 || line[1] != '=' || line.find('\r') != std::string::npos) {
            throw std::runtime_error("invalid SDP line");
        }
        if (line[0] == 'v') {
            if (has_version || line != "v=0" || !description.tracks.empty()) {
                throw std::runtime_error("unsupported SDP version");
            }
            has_version = true;
        } else if (line[0] == 'm') {
            if (!description.tracks.empty() && (!has_rtpmap || !has_track_control)) {
                throw std::runtime_error("incomplete SDP track");
            }
            std::istringstream fields(line.substr(2));
            MediaTrack track;
            std::string port, protocol, pt, extra;
            if (!(fields >> track.media_type >> port >> protocol >> pt) || fields >> extra
                || protocol != "RTP/AVP") {
                throw std::runtime_error("expected one RTP/AVP payload type per track");
            }
            read_number(port, 65535);
            track.payload_type = read_number(pt, 127);
            track.control.clear();
            track.encoding.clear();
            track.clock_rate = 0;
            track.channels = 0;
            track.fmtp.clear();
            description.tracks.push_back(track);
            has_rtpmap = false;
            has_fmtp = false;
            has_track_control = false;
        } else if (line.compare(0, 10, "a=control:") == 0) {
            const std::string control = line.substr(10);
            if (control.empty() || control.find_first_of(" \t") != std::string::npos) {
                throw std::runtime_error("invalid SDP control value");
            }
            if (description.tracks.empty()) {
                if (has_session_control) {
                    throw std::runtime_error("duplicate session control");
                }
                description.control = control;
                has_session_control = true;
            } else {
                if (has_track_control || control == "*") {
                    throw std::runtime_error("invalid or duplicate media control");
                }
                description.tracks.back().control = control;
                has_track_control = true;
            }
        } else if (line.compare(0, 9, "a=rtpmap:") == 0 || line.compare(0, 7, "a=fmtp:") == 0) {
            const bool is_rtpmap = line.compare(0, 9, "a=rtpmap:") == 0;
            const std::size_t begin = is_rtpmap ? 9 : 7;
            const std::size_t space = line.find(' ', begin);
            if (description.tracks.empty() || space == std::string::npos
                || read_number(line.substr(begin, space - begin), 127)
                       != description.tracks.back().payload_type) {
                throw std::runtime_error("SDP attribute does not match the media payload");
            }
            MediaTrack& track = description.tracks.back();
            const std::string value = line.substr(space + 1);
            if (is_rtpmap) {
                const std::size_t slash = value.find('/');
                if (has_rtpmap || slash == std::string::npos) {
                    throw std::runtime_error("invalid or duplicate rtpmap");
                }
                track.encoding = value.substr(0, slash);
                const std::size_t second = value.find('/', slash + 1);
                track.clock_rate = read_number(
                    value.substr(
                        slash + 1,
                        second == std::string::npos ? std::string::npos : second - slash - 1
                    ),
                    192000
                );
                if (second != std::string::npos) {
                    track.channels = read_number(value.substr(second + 1), 8);
                } else if (track.media_type == "audio") {
                    track.channels = 1;
                }
                has_rtpmap = true;
            } else {
                if (has_fmtp || value.empty()) {
                    throw std::runtime_error("invalid or duplicate fmtp");
                }
                track.fmtp = value;
                has_fmtp = true;
            }
        }
    }
    if (!has_version || !has_rtpmap || !has_track_control) {
        throw std::runtime_error("SDP is missing track configuration");
    }
    check_description(description);
    return description;
}

namespace {

std::string trim(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        return "";
    }
    const std::size_t end = text.find_last_not_of(" \t");
    return text.substr(begin, end - begin + 1);
}

std::string lower(const std::string& text) {
    std::string result = text;
    for (std::size_t i = 0; i < result.size(); ++i) {
        if (result[i] >= 'A' && result[i] <= 'Z') {
            result[i] = static_cast<char>(result[i] - 'A' + 'a');
        }
    }
    return result;
}

bool read_channel(const std::string& text, int& channel) {
    if (text.empty()) {
        return false;
    }
    int number = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        number = number * 10 + text[i] - '0';
        if (number > 255) {
            return false;
        }
    }
    channel = number;
    return true;
}

// 本步支持常用的 rtsp://host[:port]/path，不实现认证和 IPv6 URL。
std::string url_origin(const std::string& url) {
    if (url.compare(0, 7, "rtsp://") != 0 || url.find_first_of(" \t\r\n#") != std::string::npos) {
        throw std::runtime_error("expected an absolute RTSP URL");
    }
    //从下标7开始寻找第一个 / 或 ?
    const std::size_t end = url.find_first_of("/?", 7);
    //找到end,截取end-7个字节，没有找到，截取到末尾
    const std::string authority =
        url.substr(7, end == std::string::npos ? std::string::npos : end - 7);
    if (authority.empty() || authority.find_first_of("@[]\\") != std::string::npos) {
        throw std::runtime_error("unsupported RTSP URL authority");
    }
    return "rtsp://" + authority;
}

void check_control_path(const std::string& control) {
    // 本步没有实现路径归一化，遇到独立的 '.' / '..' 段时明确拒绝。
    std::istringstream path(control.substr(0, control.find('?')));
    std::string segment;
    while (std::getline(path, segment, '/')) {
        if (segment == "." || segment == "..") {
            throw std::runtime_error("dot segments in control URLs are not supported yet");
        }
    }
}

}  // namespace

bool parse_tcp_transport(const std::string& text, TcpTransport& transport) {
    // 多个备选 Transport 和 multicast 留到后续互通阶段。
    if (text.find(',') != std::string::npos) {
        return false;
    }
    std::istringstream input(text);
    std::string part;
    if (!std::getline(input, part, ';') || lower(trim(part)) != "rtp/avp/tcp") {
        return false;
    }

    TcpTransport parsed;
    bool has_unicast = false;
    bool has_channels = false;
    bool has_mode = false;
    while (std::getline(input, part, ';')) {
        part = lower(trim(part));
        if (part == "unicast" && !has_unicast) {
            has_unicast = true;
        } else if ((part == "mode=play" || part == "mode=\"play\"") && !has_mode) {
            has_mode = true;
        } else if (part.compare(0, 12, "interleaved=") == 0 && !has_channels) {
            const std::string pair = part.substr(12);
            const std::size_t dash = pair.find('-');
            if (dash == std::string::npos || !read_channel(pair.substr(0, dash), parsed.rtp_channel)
                || !read_channel(pair.substr(dash + 1), parsed.rtcp_channel)
                || parsed.rtp_channel == parsed.rtcp_channel) {
                return false;
            }
            has_channels = true;
        } else {
            return false;
        }
    }
    if (!has_unicast || !has_channels) {
        return false;
    }
    transport = parsed;
    return true;
}

std::string make_tcp_transport(const TcpTransport& transport) {
    if (transport.rtp_channel < 0 || transport.rtp_channel > 255 || transport.rtcp_channel < 0
        || transport.rtcp_channel > 255 || transport.rtp_channel == transport.rtcp_channel) {
        throw std::runtime_error("invalid interleaved channels");
    }
    return "RTP/AVP/TCP;unicast;interleaved=" + std::to_string(transport.rtp_channel) + "-"
           + std::to_string(transport.rtcp_channel);
}

SessionHeader parse_session_header(const std::string& text) {
    SessionHeader parsed;
    std::istringstream input(text);
    std::string part;
    std::getline(input, part, ';');
    parsed.id = trim(part);
    if (parsed.id.empty()) {
        throw std::runtime_error("empty Session id");
    }
    for (unsigned char c : parsed.id) {
        if (c <= 32 || c >= 127) {
            throw std::runtime_error("invalid Session id");
        }
    }
    while (std::getline(input, part, ';')) {
        const auto equal = part.find('=');
        if (lower(trim(part.substr(0, equal))) != "timeout") {
            continue;
        }
        if (parsed.timeout_seconds || equal == std::string::npos) {
            throw std::runtime_error("invalid or duplicate Session timeout");
        }
        const auto value = trim(part.substr(equal + 1));
        unsigned int seconds = 0;
        if (value.empty()) {
            throw std::runtime_error("empty Session timeout");
        }
        for (char c : value) {
            if (c < '0' || c > '9' || seconds > (2147483647U - (c - '0')) / 10) {
                throw std::runtime_error("invalid Session timeout");
            }
            seconds = seconds * 10 + (c - '0');
        }
        if (seconds == 0) {
            throw std::runtime_error("Session timeout must be positive");
        }
        parsed.timeout_seconds = seconds;
    }
    return parsed;
}

std::string resolve_control_url(const std::string& base, const std::string& control) {
    const std::string origin = url_origin(base);
    if (control.empty() || control == "*") {
        return base;
    }
    check_control_path(control);
    if (control.compare(0, 7, "rtsp://") == 0) {
        url_origin(control);  // 先验证绝对地址。
        return control;
    }
    if (control.find_first_of(" \t\r\n#\\") != std::string::npos
        || control.find("://") != std::string::npos || control.compare(0, 2, "//") == 0) {
        throw std::runtime_error("unsupported SDP control URL");
    }
    if (control[0] == '/') {
        return origin + control;
    }

    // 去掉基地址的查询串，再将普通相对路径连接到基地址所在目录。
    const std::string clean_base = base.substr(0, base.find('?'));
    const std::size_t slash = clean_base.find_last_of('/');
    std::string directory = origin + "/";
    if (slash >= origin.size()) {
        directory = clean_base.substr(0, slash + 1);
    }
    return directory + control;
}

// 从完整 RTSP URL 中提取服务器主机、端口和地址前缀，保存到 endpoint_，供后面建立 TCP 连接使用
RtspUrl parse_rtsp_url(const std::string& url) {
    RtspUrl parsed;
    parsed.origin = url_origin(url);
    // substr(7) 表示从下标 7 开始，一直截取到末尾
    const auto authority = parsed.origin.substr(7);
    const auto colon = authority.find(':');
    parsed.host = authority.substr(0, colon);
    if (parsed.host.empty() || parsed.host.find_first_of("?%") != std::string::npos) {
        throw std::runtime_error("invalid RTSP host");
    }
    if (colon != std::string::npos) {
        const auto port = read_number(authority.substr(colon + 1), 65535);
        if (port == 0) {
            throw std::runtime_error("RTSP port must be positive");
        }
        parsed.port = static_cast<unsigned short>(port);
    }
    return parsed;
}

}  // namespace rtsp
