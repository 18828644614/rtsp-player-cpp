#define SDL_MAIN_HANDLED
#include "player/audio_output.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void check(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void test_output() {
    rtsp::AudioOutput output;
    check(output.queued_ms() == 0, "new audio output is not empty");
    std::vector<float> samples(1024, 0.1f);
    AVFrame frame{};
    frame.sample_rate = 48000;
    frame.nb_samples = 1024;
    frame.format = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&frame.ch_layout, 1);
    frame.data[0] = reinterpret_cast<std::uint8_t*>(samples.data());
    for (int i = 0; i < 1000; ++i) {
        output.push(frame);
        check(output.queued_ms() <= 300, "audio input queue exceeded 300 ms");
    }
    check(
        output.stats().samples == 1024 * 1000 && output.stats().overflows > 0
            && output.stats().queue_peak_ms <= 300,
        "audio overflow policy failed"
    );
    // push 必须已复制数据，随后修改源缓冲不应影响流的生命周期。
    std::fill(samples.begin(), samples.end(), 0.0f);
    output.clear();
    check(output.queued_ms() == 0, "audio clear failed");
    frame.format = AV_SAMPLE_FMT_FLT;
    output.push(frame);
    const auto started = std::chrono::steady_clock::now();
    output.drain([] { return false; });
    check(output.queued_ms() == 0, "normal audio drain left input data");
    check(
        std::chrono::steady_clock::now() - started < std::chrono::seconds(1),
        "audio drain exceeded its deadline"
    );
    output.push(frame);
    const auto cancelled = std::chrono::steady_clock::now();
    output.drain([] { return true; });
    check(
        output.queued_ms() == 0
            && std::chrono::steady_clock::now() - cancelled < std::chrono::milliseconds(100),
        "audio drain ignored cancellation"
    );
    frame.format = AV_SAMPLE_FMT_S16;
    bool rejected = false;
    try {
        output.push(frame);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "unsupported PCM format was accepted");
    av_channel_layout_uninit(&frame.ch_layout);
}

void test_device_failure() {
    rtsp::AudioOutput output;
    std::vector<float> samples(1024, 0.0f);
    AVFrame frame{};
    frame.sample_rate = 48000;
    frame.nb_samples = 1024;
    frame.format = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&frame.ch_layout, 1);
    frame.data[0] = reinterpret_cast<std::uint8_t*>(samples.data());
    SDL_QuitSubSystem(SDL_INIT_AUDIO);  // 模拟输出设备不可用。
    bool rejected = false;
    try {
        output.push(frame);
    } catch (const std::runtime_error& error) {
        rejected = std::string(error.what()).find("open audio device") != std::string::npos;
    }
    av_channel_layout_uninit(&frame.ch_layout);
    check(rejected && output.stats().samples == 0, "audio device failure was swallowed");
}

}  // namespace

int main() {
    SDL_SetMainReady();
    try {
        test_output();
        test_device_failure();
        check(!SDL_WasInit(SDL_INIT_AUDIO), "audio subsystem leaked after output destruction");
        SDL_Quit();
        std::cout << "PASS: SDL audio submission, bounds, clear, drain and cancellation\n";
        return 0;
    } catch (const std::exception& error) {
        SDL_Quit();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
