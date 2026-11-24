#include "rtp/aac_depacketizer.h"

#include <map>
#include <stdexcept>

namespace rtsp {
namespace {

std::string trim_lower(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    std::string result(text.substr(first, text.find_last_not_of(" \t") - first + 1));
    for (auto& c : result) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return result;
}

std::map<std::string, std::string> read_fields(std::string_view fmtp) {
    std::map<std::string, std::string> fields;
    while (!fmtp.empty()) {
        const auto end = fmtp.find(';');
        const auto field = fmtp.substr(0, end);
        if (!trim_lower(field).empty()) {
            const auto equal = field.find('=');
            const auto name = trim_lower(field.substr(0, equal));
            if (equal == std::string_view::npos || name.empty()
                || !fields.emplace(name, trim_lower(field.substr(equal + 1))).second) {
                throw std::runtime_error("invalid or duplicate AAC fmtp field");
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        fmtp.remove_prefix(end + 1);
    }
    return fields;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    throw std::runtime_error("invalid AAC config hex digit");
}

AacParameters read_parameters(std::string_view fmtp, int clock_rate, int channels) {
    if (clock_rate != 48000 || channels != 1) {
        throw std::runtime_error("AAC requires 48000 Hz mono");
    }
    auto fields = read_fields(fmtp);
    // 只接受已实现的打包参数，避免把分片或交织流误当作单 AU。
    const auto take = [&](const char* name, const char* expected, bool required) {
        const auto field = fields.find(name);
        if (field == fields.end()) {
            if (required) {
                throw std::runtime_error(std::string("missing AAC ") + name);
            }
            return;
        }
        if (field->second != expected) {
            throw std::runtime_error(std::string("unsupported AAC ") + name);
        }
        fields.erase(field);
    };
    take("mode", "aac-hbr", true);
    take("sizelength", "13", true);
    take("indexlength", "3", true);
    take("indexdeltalength", "3", true);
    take("streamtype", "5", false);
    take("profile-level-id", "1", false);
    take("constantduration", "1024", false);

    const auto config = fields.find("config");
    if (config == fields.end() || config->second.size() < 4 || config->second.size() > 128
        || config->second.size() % 2 != 0) {
        throw std::runtime_error("invalid AAC AudioSpecificConfig length");
    }
    AacParameters parameters;
    parameters.sample_rate = clock_rate;
    parameters.channels = channels;
    for (std::size_t i = 0; i < config->second.size(); i += 2) {
        parameters.config += static_cast<char>(
            (hex_digit(config->second[i]) << 4) | hex_digit(config->second[i + 1])
        );
    }
    fields.erase(config);
    if (!fields.empty()) {
        throw std::runtime_error("unsupported AAC fmtp field: " + fields.begin()->first);
    }
    const auto a = static_cast<unsigned char>(parameters.config[0]);
    const auto b = static_cast<unsigned char>(parameters.config[1]);
    // objectType=2、frequencyIndex=3、channelConfiguration=1、标准 1024 样本 GASpecificConfig。
    if ((a >> 3) != 2 || (((a & 7) << 1) | (b >> 7)) != 3 || ((b >> 3) & 15) != 1 || (b & 7) != 0) {
        throw std::runtime_error("AAC config must describe 48000 Hz mono AAC-LC, 1024 samples");
    }
    return parameters;
}

}  // namespace

const AacParameters&
AacDepacketizer::configure(std::string_view fmtp, int clock_rate, int channels) {
    parameters_ = read_parameters(fmtp, clock_rate, channels);
    stats_ = {};
    sequence_.reset();
    timestamp_.reset();
    pts_ = 0;
    return parameters_;
}

const AacStats& AacDepacketizer::stats() const {
    return stats_;
}

std::optional<AacAccessUnit>
AacDepacketizer::push(const RtpHeader& header, std::string_view payload) {
    if (parameters_.config.empty()) {
        throw std::logic_error("configure AAC depacketizer before receiving RTP");
    }
    const auto byte = [&](std::size_t offset) {
        return static_cast<unsigned char>(payload[offset]);
    };
    // AU-headers-length=16，13 bit 大小（字节）+ 3 bit AU-index=0。
    if (payload.size() < 5 || !header.marker || byte(0) != 0 || byte(1) != 16 || (byte(3) & 7) != 0
        || ((byte(2) << 5) | (byte(3) >> 3)) != payload.size() - 4) {
        ++stats_.invalid;
        ++stats_.dropped;
        throw std::runtime_error("expected one complete AAC-hbr AU");
    }
    const auto sequence_delta =
        sequence_ ? static_cast<std::uint16_t>(header.sequence - *sequence_) : 1;
    const auto timestamp_delta = timestamp_ ? header.timestamp - *timestamp_ : 0;
    if (sequence_
        && (sequence_delta == 0 || sequence_delta > 0x7fff || timestamp_delta == 0
            || timestamp_delta > 0x7fffffffU)) {
        ++stats_.dropped;
        return std::nullopt;
    }
    stats_.lost += sequence_delta - 1;
    pts_ = timestamp_ ? pts_ + timestamp_delta : header.timestamp;
    sequence_ = header.sequence;
    timestamp_ = header.timestamp;
    ++stats_.access_units;
    return AacAccessUnit{std::string(payload.substr(4)), pts_};
}

}  // namespace rtsp
