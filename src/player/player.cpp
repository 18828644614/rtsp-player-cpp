#include "player/player.h"
#include "player/video_renderer.h"
#include "media/video_decoder.h"
#include "media/audio_decoder.h"
#include "player/audio_output.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
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

void Player::update_sender_report(
    const MediaTrack& track, const SenderReport& report, AudioOutput& audio_output
) {
    const auto kind =
        track.media_type == "video" ? SyncClock::Track::Video : SyncClock::Track::Audio;
    std::lock_guard<std::mutex> lock(mutex_);
    const bool was_ready = clock_.ready();
    if (clock_.update(kind, track.clock_rate, report)) {
        dropped_ += frames_.size();
        frames_.clear();
        last_pts_.reset();
        audio_output.clear();
        sync_wait_started_ = std::chrono::steady_clock::now();
        std::cout << "[SYNC] SR discontinuity; buffering again\n";
    }
    if (!was_ready && clock_.ready()) {
        synchronized_ = true;
        std::cout << "[SYNC] ready video_rate=90000 audio_rate=48000\n";
    }
}

Player::Frame Player::take_due_frame(std::optional<std::int64_t> audio_pts) {
    // 调用方持锁。FFmpeg 已重排 B 帧，音频位置耗尽后不会随本机时间前进。
    Frame due;
    const auto audio_us = audio_pts && clock_.ready()
                              ? clock_.time(SyncClock::Track::Audio, *audio_pts)
                              : std::nullopt;
    if (!audio_us) {
        if (done_) {
            tail_dropped_ += frames_.size();
            dropped_ += frames_.size();
            frames_.clear();
        }
        return due;
    }
    while (!frames_.empty()) {
        const auto video_us = *clock_.time(SyncClock::Track::Video, frames_.front()->pts);
        const auto action = SyncClock::action(video_us, *audio_us);
        if (action == SyncClock::Action::Wait) {
            if (done_) {
                // 音频已经有限排空，余下超出音频终点的视频不能继续等。
                tail_dropped_ += frames_.size();
                dropped_ += frames_.size();
                frames_.clear();
            }
            break;
        }
        if (action == SyncClock::Action::Drop) {
            frames_.pop_front();
            ++dropped_;
            ++late_dropped_;
            continue;
        }
        if (due) {
            ++dropped_;
        }
        due = std::move(frames_.front());
        frames_.pop_front();
    }
    if (due) {
        const auto delta = *clock_.time(SyncClock::Track::Video, due->pts) - *audio_us;
        const auto error = delta < 0 ? -delta : delta;
        max_present_error_us_ = std::max(max_present_error_us_, error);
    }
    return due;
}

void Player::decode_media(const std::string& url, AudioOutput& audio_output) {
    std::uint64_t decoded_frames = 0;
    VideoDecoder decoder([this, &decoded_frames](const AVFrame& frame) {
        print_frame(frame, ++decoded_frames);
        enqueue_frame(frame);
    });
    AudioDecoder audio_decoder([this, &audio_output](const AVFrame& frame) {
        if (should_stop()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!clock_.ready()) {
                waiting_audio_samples_ += frame.nb_samples;
                return;
            }
        }
        audio_output.push(frame);
    });
    RtspClient client(options_, std::cout, [this] { return should_stop(); });
    client.set_play_handler([this] {
        std::lock_guard<std::mutex> lock(mutex_);
        sync_wait_started_ = std::chrono::steady_clock::now();
    });
    client.set_sender_report_handler([this, &audio_output](
                                         const MediaTrack& track, const SenderReport& report
                                     ) { update_sender_report(track, report, audio_output); });
    client.set_video_handler([&decoder](const H264AccessUnit& au) {
        const bool decoded = decoder.decode(au);
        if (!decoded) {
            std::cerr << "[WARN] H264 decoder rejected AU; waiting for IDR\n";
        }
        return decoded;
    });

    client.set_audio_handlers(
        [&audio_decoder](const AacParameters& parameters) { audio_decoder.configure(parameters); },
        [&audio_decoder](const AacAccessUnit& au) {
            if (!audio_decoder.decode(au) && audio_decoder.stats().errors <= 3) {
                std::cerr << "[WARN] AAC decoder rejected AU; continuing with next AU\n";
            }
        }
    );

    std::exception_ptr failure;
    try {
        client.run(url);
    } catch (...) {
        failure = std::current_exception();
    }
    // 两个解码器都尝试排空；后续清理失败时保留最初的异常。
    const auto finish = [&failure](auto operation) {
        try {
            operation();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
    };
    finish([&] {
        if (!decoder.drain()) {
            std::cerr << "[WARN] corrupt H264 data while draining decoder\n";
        }
    });
    finish([&] {
        if (!audio_decoder.drain()) {
            std::cerr << "[WARN] corrupt AAC data while draining decoder\n";
        }
    });
    finish([&] {
        if (failure || should_stop()) {
            audio_output.clear();
        } else {
            audio_output.drain([this] { return should_stop(); });
        }
    });
    const auto& stats = decoder.stats();
    std::cout << "[FINAL] decode frames=" << stats.frames << " errors=" << stats.errors
              << " resets=" << stats.resets << std::endl;
    const auto& audio_stats = audio_decoder.stats();
    std::cout << "[FINAL] audio_decode frames=" << audio_stats.frames
              << " samples=" << audio_stats.samples << " errors=" << audio_stats.errors
              << std::endl;
    const auto& output_stats = audio_output.stats();
    std::cout << "[FINAL] audio_output samples=" << output_stats.samples
              << " queue_peak_ms=" << output_stats.queue_peak_ms
              << " overflows=" << output_stats.overflows
              << " discontinuities=" << output_stats.discontinuities
              << " underruns=" << output_stats.underruns << std::endl;
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void Player::receive_media(const std::string& url, AudioOutput& audio_output) {
    std::exception_ptr failure;
    try {
        decode_media(url, audio_output);
    } catch (...) {
        failure = std::current_exception();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    failure_ = failure;
    done_ = true;
}

void Player::run(const std::string& url) {
    // 创建的是局部对象，它的构造函数会初始化 SDL、创建窗口和 renderer
    VideoRenderer renderer;
    // renderer 最后销毁；音频初始化/释放在主线程，设备流由工作线程使用。
    AudioOutput audio_output;
    std::thread worker(&Player::receive_media, this, url, std::ref(audio_output));
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
            // 取到期帧
            // 工作线程：向 frames_ 入队
            // 主线程：从 frames_ 出队
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (failure_) {
                    break;
                }
                if (!clock_.ready() && sync_wait_started_
                    && (done_
                        || std::chrono::steady_clock::now() - *sync_wait_started_
                               >= std::chrono::seconds(3))) {
                    throw std::runtime_error("cannot establish A/V sync: waiting for both RTCP SR");
                }
                // 锁顺序始终为 Player -> AudioOutput；音频内部从不回调 Player。
                frame = take_due_frame(audio_output.playback_position());
                finished = done_ && frames_.empty();
            }

            // 显示视频帧
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
    std::cout << "[FINAL] sync ready=" << synchronized_ << " resets=" << clock_.resets
              << " late_dropped=" << late_dropped_ << " tail_dropped=" << tail_dropped_
              << " max_present_error_us=" << max_present_error_us_
              << " waiting_audio_samples=" << waiting_audio_samples_ << std::endl;
    if (display_failure) {
        std::rethrow_exception(display_failure);
    }
    if (failure_) {
        std::rethrow_exception(failure_);
    }
}

}  // namespace rtsp
