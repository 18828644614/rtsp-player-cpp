#pragma once

#include "rtp/h264_depacketizer.h"

#include <functional>
#include <optional>

struct AVCodecContext;
struct AVPacket;
struct AVFrame;

namespace rtsp {

struct VideoDecodeStats {
    std::uint64_t frames = 0;
    std::uint64_t errors = 0;
    std::uint64_t resets = 0;
};

// 同步调用；帧只在回调期间有效。下一阶段可在回调中引用或复制帧。
class VideoDecoder {
public:
    using FrameHandler = std::function<void(const AVFrame&)>;

    explicit VideoDecoder(FrameHandler on_frame);
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // false 表示码流损坏，调用方应等待完整 IDR；资源/API 错误抛异常。
    bool decode(const H264AccessUnit& au);
    bool drain();
    void reset();
    const VideoDecodeStats& stats() const;

private:
    bool receive_frames();
    bool check_decode_result(int result, const char* operation);
    std::int64_t extend_timestamp(std::uint32_t timestamp);
    void close();

    FrameHandler on_frame_;
    AVCodecContext* codec_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    VideoDecodeStats stats_;
    bool drained_ = false;
    std::optional<std::uint32_t> last_timestamp_;
    std::int64_t pts_ = 0;
};

}  // namespace rtsp
