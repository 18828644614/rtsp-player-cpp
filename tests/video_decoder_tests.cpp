#include "media/video_decoder.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

namespace {

void check(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

struct StartCode {
    std::size_t offset = 0;
    std::size_t size = 0;
};

StartCode find_start(const std::string& wire, std::size_t from) {
    for (auto i = from; i + 3 <= wire.size(); ++i) {
        if (wire[i] == 0 && wire[i + 1] == 0) {
            if (wire[i + 2] == 1) {
                return {i, 3};
            }
            if (i + 4 <= wire.size() && wire[i + 2] == 0 && wire[i + 3] == 1) {
                return {i, 4};
            }
        }
    }
    return {wire.size(), 0};
}

std::vector<std::string> read_fixture(const char* path) {
    std::ifstream input(path, std::ios::binary);
    check(input.good(), "cannot open H264 fixture");
    const std::string wire((std::istreambuf_iterator<char>(input)), {});
    std::vector<std::string> units;
    std::string current;
    auto start = find_start(wire, 0);
    while (start.size != 0) {
        const auto nal = start.offset + start.size;
        check(nal < wire.size(), "fixture ends in a start code");
        const auto next = find_start(wire, nal);
        if ((static_cast<unsigned char>(wire[nal]) & 31) == 9 && !current.empty()) {
            units.push_back(std::move(current));
            current.clear();
        }
        current.append(wire, start.offset, next.offset - start.offset);
        start = next;
    }
    if (!current.empty()) {
        units.push_back(std::move(current));
    }
    check(units.size() == 8, "expected eight access units in fixture");
    return units;
}

void check_frame(const AVFrame& frame) {
    check(
        frame.width == 64 && frame.height == 48 && frame.format == AV_PIX_FMT_YUV420P,
        "decoded dimensions or format changed"
    );
    check(frame.data[0] != nullptr && frame.linesize[0] >= 64, "decoded frame has no pixels");
}

void test_decode_and_drain(const std::vector<std::string>& units) {
    std::vector<std::int64_t> timestamps;
    rtsp::VideoDecoder decoder([&](const AVFrame& frame) {
        check_frame(frame);
        timestamps.push_back(frame.pts);
    });
    // 固定 fixture 的解码顺序为 I0 P3 B1 B2 I4 P7 B5 B6。
    // 同时覆盖 B 帧的负时间差以及 32 位 RTP 时间戳回绕。
    const std::uint32_t presentation_order[] = {0, 3, 1, 2, 4, 7, 5, 6};
    for (std::size_t i = 0; i < units.size(); ++i) {
        const auto timestamp = 0xffffe000U + presentation_order[i] * 9000;
        const rtsp::H264AccessUnit au{units[i], timestamp, i % 4 == 0, i == 0};
        check(decoder.decode(au), "valid fixture rejected");
    }
    check(timestamps.size() < 8, "fixture did not exercise delayed output");
    check(decoder.drain() && decoder.drain(), "drain failed or was not idempotent");
    check(timestamps.size() == 8, "drain lost delayed frames");
    for (std::size_t i = 0; i < timestamps.size(); ++i) {
        check(
            timestamps[i] == 0xffffe000LL + static_cast<std::int64_t>(i * 9000),
            "frame PTS/reordering changed"
        );
    }

    const rtsp::H264AccessUnit restarted{units[0], 0xffffe000U + 8 * 9000, true, true};
    check(decoder.decode(restarted) && decoder.drain(), "decoder could not reset after drain");
    check(
        timestamps.size() == 9 && timestamps.back() == 0xffffe000LL + 72000,
        "reset restarted timestamp origin"
    );
}

void test_corruption_and_recovery(const std::vector<std::string>& units) {
    std::size_t frames = 0;
    rtsp::VideoDecoder decoder([&](const AVFrame& frame) {
        check_frame(frame);
        ++frames;
    });
    const rtsp::H264AccessUnit corrupt{std::string("\0\0\0\1\x65\x80", 6), 0, true, true};
    check(!decoder.decode(corrupt) && decoder.stats().errors > 0, "corrupt bitstream not reported");
    check(frames == 0, "corrupt frame reached handler");
    check(decoder.decode({units[0], 9000, true, true}) && decoder.drain(), "IDR recovery failed");
    check(frames == 1, "valid frame missing after recovery");
}

void test_reset_discards_delayed_frames(const std::vector<std::string>& units) {
    std::vector<std::int64_t> timestamps;
    rtsp::VideoDecoder decoder([&](const AVFrame& frame) { timestamps.push_back(frame.pts); });
    check(decoder.decode({units[0], 0, true, true}), "first IDR rejected");
    check(decoder.decode({units[4], 36000, true, true}), "recovery IDR rejected");
    check(decoder.drain(), "recovery drain failed");
    check(timestamps.size() == 1 && timestamps[0] == 36000, "reset leaked old delayed frame");
}

void test_callback_exception(const std::vector<std::string>& units) {
    rtsp::VideoDecoder decoder([](const AVFrame&) {
        throw std::runtime_error("frame handler failed");
    });
    bool propagated = false;
    try {
        decoder.decode({units[0], 0, true, true});
        decoder.drain();
    } catch (const std::runtime_error& error) {
        propagated = std::string(error.what()) == "frame handler failed";
    }
    check(propagated, "frame handler exception was swallowed");
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        check(argc == 2, "expected H264 fixture path");
        const auto units = read_fixture(argv[1]);
        test_decode_and_drain(units);
        test_corruption_and_recovery(units);
        test_reset_discards_delayed_frames(units);
        test_callback_exception(units);
        std::cout << "PASS: H264 pixels, B-frame drain, RTP timestamps, corruption and reset\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
