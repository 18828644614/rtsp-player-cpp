#include "rtp/aac_depacketizer.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

const std::string fmtp = "streamtype=5;profile-level-id=1;mode=AAC-hbr;config=1188;"
                         "SizeLength=13;IndexLength=3;IndexDeltaLength=3";

void check(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

template <class F>
void rejects(F action) {
    try {
        action();
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error("expected AAC input rejection");
}

std::string payload(const std::string& data) {
    const auto size = static_cast<unsigned int>(data.size());
    std::string result("\0\x10", 2);
    result += static_cast<char>(size >> 5);
    result += static_cast<char>((size & 31) << 3);
    return result + data;
}

void test_parameters() {
    rtsp::AacDepacketizer parser;
    const auto& parameters = parser.configure(fmtp, 48000, 1);
    check(parameters.config == std::string("\x11\x88", 2), "ASC hex decoding failed");
    parser.configure(
        " Mode = AAC-HBR ; CONFIG=118856E500; SizeLength=13; IndexLength=3; "
        "IndexDeltaLength=3;",
        48000, 1
    );
    for (const auto& bad :
         {std::string("config=1188"), fmtp + ";config=1188", fmtp + ";CTSDeltaLength=16",
          std::string("mode=AAC-lbr;config=1188;SizeLength=13;IndexLength=3;IndexDeltaLength=3"),
          std::string("mode=AAC-hbr;config=zz88;SizeLength=13;IndexLength=3;IndexDeltaLength=3"),
          std::string("mode=AAC-hbr;config=118;SizeLength=13;IndexLength=3;IndexDeltaLength=3"),
          std::string("mode=AAC-hbr;config=1190;SizeLength=13;IndexLength=3;IndexDeltaLength=3"),
          std::string("mode=AAC-hbr;config=118c;SizeLength=13;IndexLength=3;IndexDeltaLength=3")}) {
        rejects([&] { parser.configure(bad, 48000, 1); });
    }
    rejects([&] { parser.configure(fmtp, 44100, 1); });
    rejects([&] { parser.configure(fmtp, 48000, 2); });
}

void test_packets() {
    rtsp::AacDepacketizer parser;
    parser.configure(fmtp, 48000, 1);
    rtsp::RtpHeader header;
    header.marker = true;
    header.sequence = 65535;
    header.timestamp = 0xfffffc00U;
    auto au = parser.push(header, payload("abc"));
    check(au && au->data == "abc" && au->pts == 0xfffffc00LL, "AAC extraction failed");
    header.sequence = 0;
    header.timestamp = 0;
    au = parser.push(header, payload("next"));
    check(au && au->pts == 0x100000000LL, "AAC RTP wrap failed");
    check(!parser.push(header, payload("duplicate")), "duplicate AAC was accepted");
    header.sequence = 65535;
    header.timestamp = 0xfffffc00U;
    check(!parser.push(header, payload("old")), "old AAC was accepted");
    header.sequence = 2;
    header.timestamp = 2048;
    au = parser.push(header, payload(std::string(8191, 'x')));
    check(
        au && au->data.size() == 8191 && parser.stats().lost == 1,
        "AAC loss recovery or size limit failed"
    );
    for (auto bad :
         {std::string(), std::string("\0\x10\0\0", 4), payload("abc") + "extra",
          payload(std::string(8192, 'x'))}) {
        rejects([&] { parser.push(header, bad); });
    }
    auto bad = payload("abc");
    bad[1] = 32;  // 多 AU 头。
    rejects([&] { parser.push(header, bad); });
    bad = payload("abc");
    bad[3] |= 1;  // 不支持非零 AU-index。
    rejects([&] { parser.push(header, bad); });
    header.marker = false;
    rejects([&] { parser.push(header, payload("abc")); });
    header.marker = true;
    header.sequence = 3;
    header.timestamp = 3072;
    check(parser.push(header, payload("recovered")).has_value(), "bad packet blocked next AU");
    check(
        parser.stats().access_units == 4 && parser.stats().invalid == 7
            && parser.stats().dropped == 9,
        "AAC counters changed"
    );
}

}  // namespace

int main() {
    try {
        test_parameters();
        test_packets();
        std::cout << "PASS: AAC config, AU validation, loss, duplicates and RTP wrap\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
