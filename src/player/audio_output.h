#pragma once

#include <cstdint>
#include <functional>

struct AVFrame;
struct SDL_AudioStream;

namespace rtsp {

struct AudioOutputStats {
    std::uint64_t samples = 0;
    std::uint64_t overflows = 0;
    int queue_peak_ms = 0;
};

// 在主线程构造/销毁；工作线程独占 push、clear、drain，join 后才能销毁。
class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    void push(const AVFrame& frame);
    int queued_ms() const;
    void clear();
    void drain(const std::function<bool()>& stop_requested);
    const AudioOutputStats& stats() const;

private:
    int queued_bytes() const;
    SDL_AudioStream* stream_ = nullptr;
    AudioOutputStats stats_;
};

}  // namespace rtsp
