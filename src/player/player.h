#pragma once

#include "player/video_clock.h"
#include "rtsp/rtsp_client.h"

#include <atomic>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

struct AVFrame;

namespace rtsp {

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

    void receive_video(const std::string& url);
    void decode_video(const std::string& url);
    void enqueue_frame(const AVFrame& frame);
    Frame take_due_frame(VideoClock::Time now);
    bool should_stop() const;

    ClientOptions options_;
    std::function<bool()> external_stop_;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::deque<Frame> frames_;  // 持锁仅做入队/出队，不在锁内解码或绘制。
    bool done_ = false;
    std::exception_ptr failure_;
    std::optional<std::int64_t> last_pts_;
    std::uint64_t dropped_ = 0;
    std::uint64_t queue_peak_ = 0;
    VideoClock clock_;  // 仅主线程使用。
};

}  // namespace rtsp
