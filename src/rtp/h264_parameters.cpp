#include "rtp/h264_parameters.h"

#include <stdexcept>
#include <utility>

namespace rtsp {
namespace {

constexpr std::size_t max_parameter_size = 64 * 1024;

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

int base64_digit(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+' || c == '/') {
        return c == '+' ? 62 : 63;
    }
    throw std::runtime_error("invalid H264 parameter base64");
}

std::string decode_base64(std::string_view text) {
    if (text.empty() || text.size() % 4 != 0 || text.size() > ((max_parameter_size + 2) / 3) * 4) {
        throw std::runtime_error("invalid H264 parameter base64 length");
    }

    std::string result;
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const int a = base64_digit(text[i]);
        const int b = base64_digit(text[i + 1]);
        const bool pad_c = text[i + 2] == '=';
        const bool pad_d = text[i + 3] == '=';
        const int c = pad_c ? 0 : base64_digit(text[i + 2]);
        const int d = pad_d ? 0 : base64_digit(text[i + 3]);
        if ((pad_c && !pad_d) || ((pad_c || pad_d) && i + 4 != text.size())
            || (pad_c && (b & 15) != 0) || (pad_d && !pad_c && (c & 3) != 0)) {
            throw std::runtime_error("invalid H264 parameter base64 padding");
        }
        result += static_cast<char>((a << 2) | (b >> 4));
        if (!pad_c) {
            result += static_cast<char>((b << 4) | (c >> 2));
        }
        if (!pad_d) {
            result += static_cast<char>((c << 6) | d);
        }
    }
    if (result.size() > max_parameter_size) {
        throw std::runtime_error("H264 parameter set exceeds 64 KiB");
    }
    return result;
}

void read_parameter_sets(std::string_view text, H264Parameters& parameters) {
    const auto comma = text.find(',');
    if (comma == std::string_view::npos || text.find(',', comma + 1) != std::string_view::npos) {
        throw std::runtime_error("expected one H264 SPS and one PPS in SDP");
    }
    auto sps = decode_base64(trim(text.substr(0, comma)));
    auto pps = decode_base64(trim(text.substr(comma + 1)));
    if (sps.size() < 4 || pps.empty() || (static_cast<unsigned char>(sps[0]) & 0x9f) != 7
        || (static_cast<unsigned char>(pps[0]) & 0x9f) != 8) {
        throw std::runtime_error("invalid H264 SDP SPS/PPS NAL types");
    }
    parameters.sps = std::move(sps);
    parameters.pps = std::move(pps);
}

}  // namespace

H264Parameters parse_h264_parameters(std::string_view fmtp) {
    H264Parameters parameters;
    bool has_mode = false;
    bool has_parameter_sets = false;
    while (!fmtp.empty()) {
        const auto end = fmtp.find(';');
        const auto field = trim(fmtp.substr(0, end));
        const auto equal = field.find('=');
        const auto name = trim(field.substr(0, equal));
        const auto value =
            equal == std::string_view::npos ? std::string_view{} : trim(field.substr(equal + 1));
        if (name == "packetization-mode") {
            if (has_mode || (value != "0" && value != "1")) {
                throw std::runtime_error("unsupported or duplicate H264 packetization-mode");
            }
            parameters.packetization_mode = value == "1" ? 1 : 0;
            has_mode = true;
        } else if (name == "sprop-parameter-sets") {
            if (has_parameter_sets) {
                throw std::runtime_error("duplicate H264 sprop-parameter-sets");
            }
            read_parameter_sets(value, parameters);
            has_parameter_sets = true;
        }
        if (end == std::string_view::npos) {
            break;
        }
        fmtp.remove_prefix(end + 1);
    }
    return parameters;
}

}  // namespace rtsp
