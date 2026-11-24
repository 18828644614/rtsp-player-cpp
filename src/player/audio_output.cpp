#include "player/audio_output.h"

#include <SDL3/SDL.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

namespace rtsp {
namespace {

constexpr int sample_rate = 48000;
constexpr int bytes_per_sample = sizeof(float);  // 单声道 F32，FLTP 的唯一平面也是连续 PCM。
constexpr int max_queue_bytes = sample_rate * bytes_per_sample * 300 / 1000;

void require_sdl(bool result, const char* operation) {
    if (!result) {
        throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
    }
}

}  // namespace

AudioOutput::AudioOutput() {
    require_sdl(SDL_InitSubSystem(SDL_INIT_AUDIO), "initialize SDL audio");
}

AudioOutput::~AudioOutput() {
    SDL_DestroyAudioStream(stream_);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

int AudioOutput::queued_bytes() const {
    const int bytes = stream_ ? SDL_GetAudioStreamQueued(stream_) : 0;
    require_sdl(bytes >= 0, "query queued audio");
    return bytes;
}

int AudioOutput::queued_ms() const {
    return (queued_bytes() * 1000 + sample_rate * bytes_per_sample - 1)
           / (sample_rate * bytes_per_sample);
}

const AudioOutputStats& AudioOutput::stats() const {
    return stats_;
}

void AudioOutput::clear() {
    if (stream_) {
        require_sdl(SDL_ClearAudioStream(stream_), "clear audio stream");
    }
}

void AudioOutput::push(const AVFrame& frame) {
    if (frame.sample_rate != sample_rate || frame.ch_layout.nb_channels != 1
        || (frame.format != AV_SAMPLE_FMT_FLTP && frame.format != AV_SAMPLE_FMT_FLT)
        || frame.nb_samples <= 0 || frame.nb_samples > max_queue_bytes / bytes_per_sample
        || !frame.data[0]) {
        throw std::runtime_error("SDL audio requires 48000 Hz mono float PCM within 300 ms");
    }
    const bool opening = stream_ == nullptr;
    if (opening) {
        const SDL_AudioSpec spec{SDL_AUDIO_F32, 1, sample_rate};
        stream_ =
            SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        require_sdl(stream_ != nullptr, "open audio device");
    }
    const int bytes = frame.nb_samples * bytes_per_sample;
    if (queued_bytes() + bytes > max_queue_bytes) {
        clear();
        ++stats_.overflows;
    }
    // SDL 在返回前复制数据，解码回调结束后可以立即释放 AVFrame。
    require_sdl(SDL_PutAudioStreamData(stream_, frame.data[0], bytes), "submit audio PCM");
    stats_.samples += frame.nb_samples;
    stats_.queue_peak_ms = std::max(stats_.queue_peak_ms, queued_ms());
    if (opening) {
        require_sdl(SDL_ResumeAudioStreamDevice(stream_), "start audio device");
    }
}

void AudioOutput::drain(const std::function<bool()>& stop_requested) {
    if (!stream_) {
        return;
    }
    require_sdl(SDL_FlushAudioStream(stream_), "flush audio stream");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (!stop_requested() && std::chrono::steady_clock::now() < deadline) {
        const int available = SDL_GetAudioStreamAvailable(stream_);
        require_sdl(available >= 0, "query available audio");
        if (queued_bytes() == 0 && available == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    clear();
}

}  // namespace rtsp
