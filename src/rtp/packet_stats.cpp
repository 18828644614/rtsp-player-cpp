#include "rtp/packet_stats.h"
#include <stdexcept>

namespace rtsp {
namespace {

unsigned int byte(const std::string& data, std::size_t pos) {
    return static_cast<unsigned char>(data[pos]);
}

unsigned int u16(const std::string& data, std::size_t pos) {
    return (byte(data, pos) << 8) | byte(data, pos + 1);
}

std::uint32_t u32(const std::string& data, std::size_t pos) {
    return (static_cast<std::uint32_t>(u16(data, pos)) << 16) | u16(data, pos + 2);
}

void require(bool valid, const char* error) {
    if (!valid) {
        throw std::runtime_error(error);
    }
}

void check_source(const PacketStats& stats, std::uint32_t source) {
    require(!stats.ssrc || *stats.ssrc == source, "SSRC changed within one track");
}

}  // namespace

RtpHeader parse_rtp_header(const std::string& wire) {
    require(wire.size() >= 12 && byte(wire, 0) >> 6 == 2, "invalid RTP header/version");
    std::size_t offset = 12 + (byte(wire, 0) & 15) * 4;
    require(offset <= wire.size(), "truncated RTP CSRC list");
    if ((byte(wire, 0) & 16) != 0) {
        require(wire.size() - offset >= 4, "truncated RTP extension header");
        const auto size = static_cast<std::size_t>(u16(wire, offset + 2)) * 4;
        offset += 4;
        require(size <= wire.size() - offset, "truncated RTP extension data");
        offset += size;
    }
    std::size_t end = wire.size();
    if ((byte(wire, 0) & 32) != 0) {
        require(end > offset, "missing RTP padding");
        const auto padding = byte(wire, end - 1);
        require(padding != 0 && padding <= end - offset, "invalid RTP padding");
        end -= padding;
    }
    return {
        byte(wire, 1) & 127,
        (byte(wire, 1) & 128) != 0,
        static_cast<std::uint16_t>(u16(wire, 2)),
        u32(wire, 4),
        u32(wire, 8),
        end - offset,
        offset
    };
}

RtcpInfo parse_rtcp(const std::string& wire) {
    require(!wire.empty(), "empty RTCP frame");
    RtcpInfo info;
    std::size_t offset = 0;
    while (offset < wire.size()) {
        require(
            wire.size() - offset >= 4 && byte(wire, offset) >> 6 == 2, "invalid RTCP header/version"
        );
        const auto count = byte(wire, offset) & 31;
        const auto type = byte(wire, offset + 1);
        require(type >= 192 && type <= 223, "invalid RTCP packet type");
        const std::size_t size = (static_cast<std::size_t>(u16(wire, offset + 2)) + 1) * 4;
        require(size <= wire.size() - offset, "truncated RTCP packet");
        std::size_t end = offset + size;
        if ((byte(wire, offset) & 32) != 0) {
            const auto padding = byte(wire, end - 1);
            require(
                end == wire.size() && padding != 0 && padding <= size - 4, "invalid RTCP padding"
            );
            end -= padding;
        }
        const auto body = offset + 4;
        if (type == 200 || type == 201) {
            const std::size_t fixed = type == 200 ? 24 : 4;
            require(end - body == fixed + count * 24, "invalid RTCP SR/RR length");
            const auto source = u32(wire, body);
            info.sources.push_back(source);
            if (type == 200) {
                ++info.sr;
                info.latest_sr = SenderReport{
                    source,
                    (static_cast<std::uint64_t>(u32(wire, body + 4)) << 32) | u32(wire, body + 8),
                    u32(wire, body + 12), u32(wire, body + 16), u32(wire, body + 20)
                };
            } else {
                ++info.rr;
            }
        } else if (type == 202) {
            std::size_t pos = body;
            for (unsigned int i = 0; i < count; ++i) {
                require(end - pos >= 4, "truncated RTCP SDES source");
                info.sources.push_back(u32(wire, pos));
                pos += 4;
                bool ended = false;
                while (pos < end) {
                    const auto item = byte(wire, pos++);
                    if (item == 0) {
                        while ((pos - offset) % 4 != 0) {
                            require(
                                pos < end && byte(wire, pos) == 0, "invalid RTCP SDES alignment"
                            );
                            ++pos;
                        }
                        ended = true;
                        break;
                    }
                    require(pos < end, "truncated RTCP SDES item");
                    const auto length = byte(wire, pos++);
                    require(length <= end - pos, "truncated RTCP SDES value");
                    pos += length;
                }
                require(ended, "missing RTCP SDES END");
            }
            require(pos == end, "invalid RTCP SDES length");
            ++info.sdes;
        } else if (type == 203) {
            require(count != 0 && end - body >= count * 4, "invalid RTCP BYE sources");
            for (unsigned int i = 0; i < count; ++i) {
                info.sources.push_back(u32(wire, body + i * 4));
            }
            const auto pos = body + count * 4;
            if (pos < end) {
                const auto length = byte(wire, pos);
                require(
                    length <= end - pos - 1 && end - pos - 1 - length <= 3,
                    "invalid RTCP BYE reason"
                );
                for (auto pad = pos + 1 + length; pad < end; ++pad) {
                    require(byte(wire, pad) == 0, "invalid RTCP BYE alignment");
                }
            }
            ++info.bye;
        }
        ++info.packets;
        offset += size;
    }
    return info;
}

RtpHeader count_rtp(PacketStats& stats, const std::string& wire, int expected_pt) {
    const auto header = parse_rtp_header(wire);
    require(
        static_cast<int>(header.payload_type) == expected_pt, "RTP payload type differs from SDP"
    );
    check_source(stats, header.ssrc);
    stats.ssrc = header.ssrc;
    ++stats.rtp_packets;
    stats.rtp_bytes += wire.size();
    stats.payload_bytes += header.payload_size;
    stats.last_sequence = header.sequence;
    stats.last_timestamp = header.timestamp;
    return header;
}

void count_rtcp(PacketStats& stats, const std::string& wire) {
    const auto info = parse_rtcp(wire);
    // 首个 SR 可能先于 RTP 到达；所有子包校验成功后才提交。
    auto source = stats.ssrc;
    for (auto ssrc : info.sources) {
        require(!source || *source == ssrc, "RTCP SSRC differs from track");
        source = ssrc;
    }
    stats.ssrc = source;
    ++stats.rtcp_frames;
    stats.rtcp_packets += info.packets;
    stats.sr += info.sr;
    stats.rr += info.rr;
    stats.sdes += info.sdes;
    stats.bye += info.bye;
    if (info.latest_sr) {
        stats.latest_sr = info.latest_sr;
    }
}

}  // namespace rtsp
