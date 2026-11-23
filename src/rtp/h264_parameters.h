#pragma once

#include <string>
#include <string_view>

namespace rtsp {

struct H264Parameters {
    int packetization_mode = 0;  // SDP 未指定时，RFC 6184 默认为单 NALU 模式。
    std::string sps;
    std::string pps;
};

// 只解析本播放器需要的 fmtp 参数；不依赖 FFmpeg。
H264Parameters parse_h264_parameters(std::string_view fmtp);

}  // namespace rtsp
