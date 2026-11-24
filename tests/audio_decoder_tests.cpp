#include "media/audio_decoder.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {

void check(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

std::vector<std::string> read_units(const char* path) {
    std::ifstream input(path, std::ios::binary);
    check(input.good(), "cannot open AAC fixture");
    const std::string wire((std::istreambuf_iterator<char>(input)), {});
    std::vector<std::string> units;
    std::size_t offset = 0;
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(wire[i]); };
    while (offset < wire.size()) {
        check(
            wire.size() - offset >= 7 && byte(offset) == 255 && byte(offset + 1) == 241,
            "invalid AAC fixture ADTS header"
        );
        const std::size_t size =
            ((byte(offset + 3) & 3) << 11) | (byte(offset + 4) << 3) | (byte(offset + 5) >> 5);
        check(size > 7 && size <= wire.size() - offset, "truncated AAC fixture");
        units.push_back(wire.substr(offset + 7, size - 7));
        offset += size;
    }
    check(units.size() == 13, "expected thirteen AAC AUs");
    return units;
}

rtsp::AacParameters parameters() {
    return {std::string("\x11\x88", 2), 48000, 1};
}

void test_decode(const std::vector<std::string>& units) {
    std::vector<std::int64_t> timestamps;
    double energy = 0;
    rtsp::AudioDecoder decoder([&](const AVFrame& frame) {
        check(
            frame.sample_rate == 48000 && frame.ch_layout.nb_channels == 1
                && frame.nb_samples == 1024 && frame.format == AV_SAMPLE_FMT_FLTP,
            "AAC decoded format changed"
        );
        timestamps.push_back(frame.pts);
        const auto* samples = reinterpret_cast<const float*>(frame.data[0]);
        for (int i = 0; i < frame.nb_samples; ++i) {
            check(std::isfinite(samples[i]), "AAC produced non-finite PCM");
            energy += samples[i] * samples[i];
        }
    });
    decoder.configure(parameters());
    for (std::size_t i = 0; i < units.size(); ++i) {
        check(
            decoder.decode({units[i], 0xfffffc00LL + static_cast<std::int64_t>(i) * 1024}),
            "AAC fixture decode failed"
        );
    }
    check(decoder.drain() && decoder.drain(), "AAC drain is not repeatable");
    check(timestamps.size() == 13 && energy > 1, "AAC frame count or signal energy failed");
    for (std::size_t i = 0; i < timestamps.size(); ++i) {
        check(
            timestamps[i] == 0xfffffc00LL + static_cast<std::int64_t>(i) * 1024,
            "AAC timestamp was lost"
        );
    }
    check(
        decoder.stats().samples == 13 * 1024 && decoder.stats().errors == 0,
        "AAC decoder statistics failed"
    );
}

void test_recovery(const std::vector<std::string>& units) {
    int frames = 0;
    rtsp::AudioDecoder decoder([&](const AVFrame&) { ++frames; });
    decoder.configure(parameters());
    check(!decoder.decode({std::string(1, '\0'), 0}), "corrupt AAC was accepted");
    for (std::size_t i = 0; i < units.size(); ++i) {
        check(
            decoder.decode({units[i], static_cast<std::int64_t>(i) * 1024}),
            "AAC failed after corrupt AU"
        );
    }
    check(
        decoder.drain() && frames == 13 && decoder.stats().errors == 1,
        "AAC did not recover after a corrupt AU"
    );

    rtsp::AudioDecoder throwing([](const AVFrame&) { throw std::runtime_error("frame callback"); });
    throwing.configure(parameters());
    bool propagated = false;
    try {
        for (const auto& data : units) {
            throwing.decode({data, 0});
        }
        throwing.drain();
    } catch (const std::runtime_error& error) {
        propagated = std::string(error.what()) == "frame callback";
    }
    check(propagated, "audio callback failure was swallowed");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 2, "usage: audio_decoder_tests fixture.aac");
        const auto units = read_units(argv[1]);
        test_decode(units);
        test_recovery(units);
        std::cout << "PASS: AAC PCM, sample count, timestamps, drain and corruption recovery\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
