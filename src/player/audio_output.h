#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

struct AVFrame;
struct SDL_AudioStream;

namespace rtsp {

struct AudioOutputStats {
    std::uint64_t samples = 0;
    std::uint64_t overflows = 0;
    std::uint64_t discontinuities = 0;
    std::uint64_t underruns = 0;
    int queue_peak_ms = 0;
};

// 主线程构造/销毁；工作线程提交，主线程查询位置。join 后才能销毁。
// 内部锁只保护 SDL 流和位置，不调用外部回调。
class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    void push(const AVFrame& frame);
    int queued_ms() const;
    // 音频 RTP 时基（1/48000），输入队列消费位置的估算，不是硬件时钟。
    std::optional<std::int64_t> playback_position() const;
    void clear();
    void drain(const std::function<bool()>& stop_requested);
    AudioOutputStats stats() const;

private:
    // 以下辅助函数由持锁的调用方使用。
    int queued_bytes() const;
    std::optional<std::int64_t> position() const;
    void clear_stream();

    mutable std::mutex mutex_;
    SDL_AudioStream* stream_ = nullptr;
    AudioOutputStats stats_;
    std::optional<std::int64_t> end_pts_;
    mutable std::optional<std::int64_t> last_position_;
};

}  // namespace rtsp
