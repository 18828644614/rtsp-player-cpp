#pragma once

#include "rtp/h264_parameters.h"
#include "rtp/packet_stats.h"

#include <optional>
#include <string>
#include <string_view>

namespace rtsp {

struct H264AccessUnit {
    std::string data;  // Annex B：每个 NALU 前带 00 00 00 01。
    std::uint32_t rtp_timestamp = 0;
    bool is_idr = false;
    bool reset_decoder = false;
};

struct H264Stats {
    std::uint64_t access_units = 0;
    std::uint64_t dropped = 0;
    std::uint64_t invalid = 0;
};

// 一路已校验的 RTP 视频；只支持单 NALU / FU-A，不重排包。
class H264Depacketizer {
public:
    explicit H264Depacketizer(std::size_t max_au_size = 8 * 1024 * 1024);

    void configure(std::string_view fmtp);
    std::optional<H264AccessUnit> push(const RtpHeader& header, std::string_view payload);

    // RTP 无效或解码失败时通知重组器：丢弃当前 AU，等待新的完整 IDR。
    void discard();
    void finish();
    const H264Stats& stats() const;

private:
    void begin_packet(const RtpHeader& header);
    void begin_au(std::uint32_t timestamp, bool boundary_known);
    void damage_au();
    void unpack_payload(std::string_view payload);
    void unpack_fu_a(std::string_view payload);
    void append_nalu(std::string_view nalu);
    void check_size(std::size_t current, std::size_t extra) const;
    std::optional<H264AccessUnit> complete_au();

    std::size_t max_au_size_;
    H264Parameters parameters_;
    H264Stats stats_;

    std::optional<std::uint16_t> last_sequence_;
    std::optional<std::uint32_t> timestamp_;
    bool boundary_known_ = false;
    bool ended_ = false;
    bool damaged_ = false;
    bool has_vcl_ = false;
    bool is_idr_ = false;
    bool waiting_for_idr_ = true;
    bool reset_decoder_ = true;

    std::string au_;
    std::string fragment_;  // 非空表示 FU-A 正在重组，第一个字节是恢复的 NAL 头。
};

}  // namespace rtsp
