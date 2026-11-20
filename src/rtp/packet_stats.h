#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rtsp {

struct RtpHeader {
    unsigned int payload_type = 0;
    bool marker = false;
    std::uint16_t sequence = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t ssrc = 0;
    std::size_t payload_size = 0;
};

struct SenderReport {
    std::uint32_t ssrc = 0;
    std::uint64_t ntp = 0;
    std::uint32_t rtp_timestamp = 0;
    std::uint32_t packet_count = 0;
    std::uint32_t octet_count = 0;
};

struct RtcpInfo {
    std::uint64_t packets = 0;
    std::uint64_t sr = 0;
    std::uint64_t rr = 0;
    std::uint64_t sdes = 0;
    std::uint64_t bye = 0;
    std::vector<std::uint32_t> sources;
    std::optional<SenderReport> latest_sr;
};

struct PacketStats {
    std::optional<std::uint32_t> ssrc;

    std::uint64_t rtp_packets = 0;
    std::uint64_t rtp_bytes = 0;
    std::uint64_t payload_bytes = 0;

    std::uint64_t rtcp_frames = 0;
    std::uint64_t rtcp_packets = 0;
    std::uint64_t sr = 0;
    std::uint64_t rr = 0;
    std::uint64_t sdes = 0;
    std::uint64_t bye = 0;
    std::uint64_t bad = 0;

    std::uint16_t last_sequence = 0;
    std::uint32_t last_timestamp = 0;
    std::optional<SenderReport> latest_sr;
};

// 先完整校验，再更新统计；一条轨道仅支持一个 SSRC。
RtpHeader parse_rtp_header(const std::string& wire);
RtcpInfo parse_rtcp(const std::string& wire);
void count_rtp(PacketStats& stats, const std::string& wire, int expected_pt);
void count_rtcp(PacketStats& stats, const std::string& wire);

}  // namespace rtsp
