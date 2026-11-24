#pragma once

#include "rtp/packet_stats.h"

#include <optional>
#include <string>
#include <string_view>

namespace rtsp {

struct AacParameters {
    std::string config;  // 二进制 AudioSpecificConfig，不含 ADTS。
    int sample_rate = 0;
    int channels = 0;
};

struct AacAccessUnit {
    std::string data;
    std::int64_t pts = 0;  // 扩展后的 RTP 时间戳，单位为 1 / sample_rate。
};

struct AacStats {
    std::uint64_t access_units = 0;
    std::uint64_t dropped = 0;
    std::uint64_t invalid = 0;
    std::uint64_t lost = 0;
};

// 配套服务端：48 kHz 单声道 AAC-LC，每个 RTP 包一个完整 AU。
class AacDepacketizer {
public:
    const AacParameters& configure(std::string_view fmtp, int clock_rate, int channels);
    std::optional<AacAccessUnit> push(const RtpHeader& header, std::string_view payload);
    const AacStats& stats() const;

private:
    AacParameters parameters_;
    AacStats stats_;
    std::optional<std::uint16_t> sequence_;
    std::optional<std::uint32_t> timestamp_;
    std::int64_t pts_ = 0;
};

}  // namespace rtsp
