#include "player/audio_output.h"

#include <SDL3/SDL.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <chrono>
#include <limits>
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
    std::lock_guard<std::mutex> lock(mutex_);
    return (queued_bytes() * 1000 + sample_rate * bytes_per_sample - 1)
           / (sample_rate * bytes_per_sample);
}

AudioOutputStats AudioOutput::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

std::optional<std::int64_t> AudioOutput::position() const {
    if (!end_pts_) {
        return std::nullopt;
    }
    const auto estimate = *end_pts_ - queued_bytes() / bytes_per_sample;
    // SDL 以块消费，位置会阶梯前进；空队列最多到已提交 PCM 的末尾。
    last_position_ = last_position_ ? std::max(*last_position_, estimate) : estimate;
    return last_position_;
}

std::optional<std::int64_t> AudioOutput::playback_position() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return position();
}

void AudioOutput::clear_stream() {
    if (stream_) {
        require_sdl(SDL_ClearAudioStream(stream_), "clear audio stream");
    }
    end_pts_.reset();
    last_position_.reset();
}

void AudioOutput::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    clear_stream();
}

void AudioOutput::push(const AVFrame& frame) {
    if (frame.sample_rate != sample_rate || frame.ch_layout.nb_channels != 1
        || (frame.format != AV_SAMPLE_FMT_FLTP && frame.format != AV_SAMPLE_FMT_FLT)
        || frame.nb_samples <= 0 || frame.nb_samples > max_queue_bytes / bytes_per_sample
        || !frame.data[0] || frame.pts == AV_NOPTS_VALUE
        || frame.pts > std::numeric_limits<std::int64_t>::max() - frame.nb_samples) {
        throw std::runtime_error(
            "SDL audio requires timestamped 48000 Hz mono float PCM within 300 ms"
        );
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const bool opening = stream_ == nullptr;
    if (opening) {
        const SDL_AudioSpec spec{SDL_AUDIO_F32, 1, sample_rate};
        stream_ =
            SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        require_sdl(stream_ != nullptr, "open audio device");
    }
    if (end_pts_ && frame.pts != *end_pts_) {
        clear_stream();  // 缺包/解码失败后的 PCM 不能跨缺口反推位置。
        ++stats_.discontinuities;
    } else if (end_pts_ && queued_bytes() == 0) {
        ++stats_.underruns;
    }
    const int bytes = frame.nb_samples * bytes_per_sample;
    if (queued_bytes() + bytes > max_queue_bytes) {
        clear_stream();
        ++stats_.overflows;
    }
    // SDL 在返回前复制数据，解码回调结束后可以立即释放 AVFrame。
    require_sdl(SDL_PutAudioStreamData(stream_, frame.data[0], bytes), "submit audio PCM");
    end_pts_ = frame.pts + frame.nb_samples;
    stats_.samples += frame.nb_samples;
    const int milliseconds = (queued_bytes() * 1000 + sample_rate * bytes_per_sample - 1)
                             / (sample_rate * bytes_per_sample);
    stats_.queue_peak_ms = std::max(stats_.queue_peak_ms, milliseconds);
    if (opening) {
        require_sdl(SDL_ResumeAudioStreamDevice(stream_), "start audio device");
    }
}

void AudioOutput::drain(const std::function<bool()>& stop_requested) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stream_) {
            return;
        }
        require_sdl(SDL_FlushAudioStream(stream_), "flush audio stream");
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (!stop_requested() && std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const int available = SDL_GetAudioStreamAvailable(stream_);
            require_sdl(available >= 0, "query available audio");
            if (queued_bytes() == 0 && available == 0) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const bool stopped = stop_requested();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto final_position = stopped ? std::nullopt : position();
    clear_stream();
    // 正常结束保留实际估算到的位置，让主线程能有限收尾；取消则使时钟失效。
    end_pts_ = final_position;
    last_position_ = final_position;
}

}  // namespace rtsp
