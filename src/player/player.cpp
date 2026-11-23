#include "player/player.h"
#include "player/video_renderer.h"
#include "media/video_decoder.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace rtsp {
namespace {

constexpr std::size_t max_frames = 12;
constexpr std::int64_t max_buffer_pts = 27000;  // 300 ms，视频时钟为 90 kHz。

void print_frame(const AVFrame& frame, std::uint64_t number) {
    const auto* format = av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame.format));
    std::cout << "[VIDEO] frame=" << number << " pts=" << frame.pts << " width=" << frame.width
              << " height=" << frame.height << " format=" << (format ? format : "unknown") << '\n';
}

}  // namespace

Player::Player(ClientOptions options, std::function<bool()> stop_requested)
    : options_(options), external_stop_(std::move(stop_requested)) {
}

void Player::FrameDeleter::operator()(AVFrame* frame) const {
    av_frame_free(&frame);
}

bool Player::should_stop() const {
    return stop_.load(std::memory_order_relaxed) || (external_stop_ && external_stop_());
}

void Player::enqueue_frame(const AVFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (should_stop()) {
        ++dropped_;
        return;
    }
    if (frame.pts == AV_NOPTS_VALUE || (last_pts_ && frame.pts <= *last_pts_)) {
        ++dropped_;
        return;
    }
    Frame retained(av_frame_clone(&frame));
    if (!retained) {
        throw std::runtime_error("cannot retain decoded video frame");
    }
    last_pts_ = frame.pts;
    frames_.push_back(std::move(retained));
    while (frames_.size() > max_frames
           || (frames_.size() > 1 && frame.pts - frames_.front()->pts > max_buffer_pts)) {
        frames_.pop_front();
        ++dropped_;
    }
    if (frames_.size() > queue_peak_) {
        queue_peak_ = frames_.size();
    }
}

Player::Frame Player::take_due_frame(VideoClock::Time now) {
    // 调用方持锁；FFmpeg 已重排 B 帧，此处仅从队首按 PTS 调度。
    Frame due;
    while (!frames_.empty() && clock_.deadline(frames_.front()->pts, now) <= now) {
        if (due) {
            ++dropped_;
        }
        due = std::move(frames_.front());
        frames_.pop_front();
    }
    return due;
}

void Player::decode_video(const std::string& url) {
    std::uint64_t decoded_frames = 0;
    VideoDecoder decoder([this, &decoded_frames](const AVFrame& frame) {
        print_frame(frame, ++decoded_frames);
        enqueue_frame(frame);
    });
    RtspClient client(options_, std::cout, [this] { return should_stop(); });
    client.set_video_handler([&decoder](const H264AccessUnit& au) {
        const bool decoded = decoder.decode(au);
        if (!decoded) {
            std::cerr << "[WARN] H264 decoder rejected AU; waiting for IDR\n";
        }
        return decoded;
    });

    std::exception_ptr failure;
    try {
        client.run(url);
    } catch (...) {
        failure = std::current_exception();
    }
    // 已提交的数据仍排空以释放延迟帧；主动关闭时 enqueue_frame 会直接丢弃。
    try {
        if (!decoder.drain()) {
            std::cerr << "[WARN] corrupt H264 data while draining decoder\n";
        }
    } catch (...) {
        if (!failure) {
            failure = std::current_exception();
        }
    }
    const auto& stats = decoder.stats();
    std::cout << "[FINAL] decode frames=" << stats.frames << " errors=" << stats.errors
              << " resets=" << stats.resets << std::endl;
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void Player::receive_video(const std::string& url) {
    std::exception_ptr failure;
    try {
        decode_video(url);
    } catch (...) {
        failure = std::current_exception();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    failure_ = failure;
    done_ = true;
}

void Player::run(const std::string& url) {
    VideoRenderer renderer;
    std::thread worker(&Player::receive_video, this, url);
    std::uint64_t displayed = 0;
    std::exception_ptr display_failure;
    try {
        while (!should_stop()) {
            if (renderer.poll_events() || should_stop()) {
                stop_.store(true, std::memory_order_relaxed);
                break;
            }
            Frame frame;
            bool finished;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (failure_) {
                    break;
                }
                frame = take_due_frame(VideoClock::Clock::now());
                finished = done_ && frames_.empty();
            }
            if (frame) {
                renderer.present(*frame);
                ++displayed;
            } else {
                renderer.redraw();
            }
            if (finished) {
                break;
            }
        }
        renderer.hide();
    } catch (...) {
        display_failure = std::current_exception();
    }
    // 无论窗口/网络哪一侧失败，都先通知停止，再回收线程，最后销毁 SDL。
    stop_.store(true, std::memory_order_relaxed);
    worker.join();
    dropped_ += frames_.size();
    frames_.clear();
    std::cout << "[FINAL] display frames=" << displayed << " dropped=" << dropped_
              << " queue_peak=" << queue_peak_ << " clock_resets=" << clock_.resets << std::endl;
    if (display_failure) {
        std::rethrow_exception(display_failure);
    }
    if (failure_) {
        std::rethrow_exception(failure_);
    }
}

}  // namespace rtsp
