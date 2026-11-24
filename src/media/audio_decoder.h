#pragma once

#include "rtp/aac_depacketizer.h"

#include <functional>

struct AVCodecContext;
struct AVPacket;
struct AVFrame;

namespace rtsp {

struct AudioDecodeStats {
    std::uint64_t frames = 0;
    std::uint64_t samples = 0;
    std::uint64_t errors = 0;
};

// 同步解码；AVFrame 仅在回调期间有效。
class AudioDecoder {
public:
    using FrameHandler = std::function<void(const AVFrame&)>;

    explicit AudioDecoder(FrameHandler on_frame);
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    void configure(const AacParameters& parameters);
    bool decode(const AacAccessUnit& au);
    bool drain();
    const AudioDecodeStats& stats() const;

private:
    bool receive_frames();
    bool check_result(int result, const char* operation);
    void reset();
    void close();

    FrameHandler on_frame_;
    AVCodecContext* codec_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    AudioDecodeStats stats_;
    bool drained_ = false;
};

}  // namespace rtsp
