#include "rtp/h264_depacketizer.h"

#include <stdexcept>
#include <utility>

namespace rtsp {
namespace {

constexpr char start_code[] = "\0\0\0\1";
constexpr std::size_t start_code_size = 4;
constexpr std::size_t max_parameter_size = 64 * 1024;

unsigned int byte(char value) {
    return static_cast<unsigned char>(value);
}

}  // namespace

H264Depacketizer::H264Depacketizer(std::size_t max_au_size) : max_au_size_(max_au_size) {
    if (max_au_size < 16) {
        throw std::invalid_argument("H264 AU size limit is too small");
    }
}

void H264Depacketizer::configure(std::string_view fmtp) {
    auto parameters = parse_h264_parameters(fmtp);
    *this = H264Depacketizer(max_au_size_);
    parameters_ = std::move(parameters);
}

const H264Stats& H264Depacketizer::stats() const {
    return stats_;
}

void H264Depacketizer::begin_au(std::uint32_t timestamp, bool boundary_known) {
    timestamp_ = timestamp;
    boundary_known_ = boundary_known;
    ended_ = false;
    damaged_ = false;
    has_vcl_ = false;
    is_idr_ = false;
    au_.clear();
    fragment_.clear();
}

void H264Depacketizer::damage_au() {
    if (!damaged_) {
        ++stats_.dropped;
    }
    damaged_ = true;
    waiting_for_idr_ = true;
    reset_decoder_ = true;
    au_.clear();
    fragment_.clear();
}

void H264Depacketizer::discard() {
    waiting_for_idr_ = true;
    reset_decoder_ = true;
    if (timestamp_ && !ended_) {
        damage_au();
    }
}

void H264Depacketizer::finish() {
    discard();
    ended_ = true;
}

void H264Depacketizer::begin_packet(const RtpHeader& header) {
    const bool continuous =
        !last_sequence_ || header.sequence == static_cast<std::uint16_t>(*last_sequence_ + 1);
    if (!timestamp_) {
        // 中途加入可能从某个 AU 的中间开始；首个时间戳只收参数集，不交给解码器。
        begin_au(header.timestamp, false);
    } else if (header.timestamp != *timestamp_) {
        if (!ended_) {
            damage_au();
        }
        begin_au(header.timestamp, continuous);
    } else if (ended_) {
        damage_au();
    }
    if (!continuous) {
        damage_au();
    }
    last_sequence_ = header.sequence;
}

void H264Depacketizer::check_size(std::size_t current, std::size_t extra) const {
    if (current > max_au_size_ || extra > max_au_size_ - current) {
        throw std::runtime_error("H264 access unit exceeds size limit");
    }
}

void H264Depacketizer::append_nalu(std::string_view nalu) {
    const auto type = byte(nalu.front()) & 31;
    check_size(au_.size(), start_code_size);
    check_size(au_.size() + start_code_size, nalu.size());

    if (type == 7 || type == 8) {
        if (nalu.size() > max_parameter_size || (type == 7 && nalu.size() < 4)) {
            throw std::runtime_error("invalid H264 parameter set size");
        }
        auto& saved = type == 7 ? parameters_.sps : parameters_.pps;
        if (!saved.empty() && saved != nalu) {
            waiting_for_idr_ = true;
            reset_decoder_ = true;
        }
        saved.assign(nalu);
    }
    has_vcl_ = has_vcl_ || (type >= 1 && type <= 5);
    is_idr_ = is_idr_ || type == 5;
    au_.append(start_code, start_code_size);
    au_.append(nalu);
}

void H264Depacketizer::unpack_fu_a(std::string_view payload) {
    if (parameters_.packetization_mode != 1 || payload.size() < 2) {
        throw std::runtime_error("unexpected or truncated H264 FU-A");
    }
    const auto header = byte(payload[1]);
    const auto type = header & 31;
    const bool start = (header & 128) != 0;
    const bool end = (header & 64) != 0;
    const auto nal_header = (byte(payload[0]) & 0xe0) | type;
    // RFC 6184 要求接收端忽略 FU header 的 R 位。
    if (type == 0 || type > 23 || (start && end)) {
        throw std::runtime_error("invalid H264 FU-A header");
    }
    if (start) {
        if (!fragment_.empty()) {
            throw std::runtime_error("H264 FU-A start before previous NAL ended");
        }
        fragment_ += static_cast<char>(nal_header);
    } else if (fragment_.empty() || byte(fragment_[0]) != nal_header) {
        throw std::runtime_error("H264 FU-A continuation without matching start");
    }
    check_size(au_.size(), start_code_size);
    check_size(au_.size() + start_code_size, fragment_.size());
    check_size(au_.size() + start_code_size + fragment_.size(), payload.size() - 2);
    fragment_.append(payload.substr(2));
    if (end) {
        append_nalu(fragment_);
        fragment_.clear();
    }
}

void H264Depacketizer::unpack_payload(std::string_view payload) {
    if (payload.empty() || (byte(payload[0]) & 128) != 0) {
        throw std::runtime_error("empty H264 payload or forbidden_zero_bit");
    }
    const auto type = byte(payload[0]) & 31;
    if (type == 28) {
        unpack_fu_a(payload);
    } else if (type >= 1 && type <= 23 && fragment_.empty()) {
        append_nalu(payload);
    } else {
        throw std::runtime_error("unsupported H264 RTP type or unfinished FU-A");
    }
}

std::optional<H264AccessUnit> H264Depacketizer::complete_au() {
    if (!fragment_.empty()) {
        throw std::runtime_error("H264 marker before FU-A end");
    }
    if (damaged_ || !boundary_known_) {
        damage_au();
        return std::nullopt;
    }
    if (!has_vcl_) {
        return std::nullopt;
    }
    if (waiting_for_idr_ && (!is_idr_ || parameters_.sps.empty() || parameters_.pps.empty())) {
        ++stats_.dropped;
        return std::nullopt;
    }

    if (is_idr_) {
        // 在初次解码和丢包恢复时均保证参数集先于图像数据。
        std::string prefix;
        for (const auto* parameter : {&parameters_.sps, &parameters_.pps}) {
            check_size(au_.size() + prefix.size(), start_code_size);
            check_size(au_.size() + prefix.size() + start_code_size, parameter->size());
            prefix.append(start_code, start_code_size);
            prefix += *parameter;
        }
        au_.insert(0, prefix);
    }
    H264AccessUnit result{std::move(au_), *timestamp_, is_idr_, reset_decoder_};
    waiting_for_idr_ = false;
    reset_decoder_ = false;
    ++stats_.access_units;
    return result;
}

std::optional<H264AccessUnit>
H264Depacketizer::push(const RtpHeader& header, std::string_view payload) {
    begin_packet(header);
    try {
        if (!damaged_) {
            unpack_payload(payload);
        }
        if (!header.marker) {
            return std::nullopt;
        }
        auto result = complete_au();
        ended_ = true;
        au_.clear();
        fragment_.clear();
        return result;
    } catch (const std::runtime_error&) {
        ++stats_.invalid;
        damage_au();
        ended_ = header.marker;
        throw;
    }
}

}  // namespace rtsp
