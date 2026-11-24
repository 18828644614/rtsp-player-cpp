#pragma once

#include "player/sync_clock.h"
#include "rtsp/rtsp_client.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

struct AVFrame;

namespace rtsp {

class AudioOutput;

// 一个实例运行一个会话；run 在主线程调用。
class Player {
public:
    // 外部停止回调可能从主线程和工作线程调用，调用方应保证线程安全。
    explicit Player(ClientOptions options, std::function<bool()> stop_requested = {});
    void run(const std::string& url);

private:
    struct FrameDeleter {
        void operator()(AVFrame* frame) const;
    };

    using Frame = std::unique_ptr<AVFrame, FrameDeleter>;

    void receive_media(const std::string& url, AudioOutput& audio_output);
    void decode_media(const std::string& url, AudioOutput& audio_output);
    void enqueue_frame(const AVFrame& frame);
    void update_sender_report(
        const MediaTrack& track, const SenderReport& report, AudioOutput& audio_output
    );
    Frame take_due_frame(std::optional<std::int64_t> audio_pts);
    bool should_stop() const;

    ClientOptions options_;
    std::function<bool()> external_stop_;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::deque<Frame> frames_;  // mutex_ 保护队列、同步映射与结束状态；锁内不解码或绘制。
    bool done_ = false;
    std::exception_ptr failure_;
    std::optional<std::int64_t> last_pts_;
    std::uint64_t dropped_ = 0;
    std::uint64_t queue_peak_ = 0;
    SyncClock clock_;  // 与帧队列共用 mutex_。
    std::optional<std::chrono::steady_clock::time_point> sync_wait_started_;
    bool synchronized_ = false;
    std::uint64_t waiting_audio_samples_ = 0;
    std::uint64_t late_dropped_ = 0;
    std::uint64_t tail_dropped_ = 0;
    std::int64_t max_present_error_us_ = 0;
};

}  // namespace rtsp
