#include "media/video_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

#include <climits>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace rtsp {
namespace {

void require_success(int result, const char* operation) {
    if (result < 0) {
        char message[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(result, message, sizeof(message));
        throw std::runtime_error(std::string(operation) + ": " + message);
    }
}

}  // namespace

VideoDecoder::VideoDecoder(FrameHandler on_frame) : on_frame_(std::move(on_frame)) {
    if (!on_frame_) {
        throw std::invalid_argument("video frame handler is required");
    }
    try {
        const auto* decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!decoder) {
            throw std::runtime_error("FFmpeg H264 decoder is unavailable");
        }
        codec_ = avcodec_alloc_context3(decoder);
        packet_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        if (!codec_ || !packet_ || !frame_) {
            throw std::runtime_error("cannot allocate FFmpeg video decoder resources");
        }
        codec_->pkt_timebase = {1, 90000};
        codec_->thread_count = 1;
        codec_->err_recognition = AV_EF_CAREFUL | AV_EF_EXPLODE;
        require_success(avcodec_open2(codec_, decoder, nullptr), "open H264 decoder");
    } catch (...) {
        close();
        throw;
    }
}

VideoDecoder::~VideoDecoder() {
    close();
}

void VideoDecoder::close() {
    av_frame_free(&frame_);
    av_packet_free(&packet_);
    avcodec_free_context(&codec_);
}

const VideoDecodeStats& VideoDecoder::stats() const {
    return stats_;
}

void VideoDecoder::reset() {
    avcodec_flush_buffers(codec_);
    av_packet_unref(packet_);
    av_frame_unref(frame_);
    drained_ = false;
    ++stats_.resets;
    // 保留时间戳展开状态；恢复解码不会让时间轴重新归零。
}

std::int64_t VideoDecoder::extend_timestamp(std::uint32_t timestamp) {
    if (last_timestamp_) {
        const auto delta = static_cast<std::uint32_t>(timestamp - *last_timestamp_);
        pts_ += delta <= 0x7fffffffU ? static_cast<std::int64_t>(delta)
                                     : static_cast<std::int64_t>(delta) - 0x100000000LL;
    }
    last_timestamp_ = timestamp;
    return pts_;
}

bool VideoDecoder::check_decode_result(int result, const char* operation) {
    if (result == AVERROR_INVALIDDATA) {
        ++stats_.errors;
        return false;
    }
    require_success(result, operation);
    return true;
}

bool VideoDecoder::receive_frames() {
    while (true) {
        const int result = avcodec_receive_frame(codec_, frame_);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            return true;
        }
        if (!check_decode_result(result, "receive H264 frame")) {
            return false;
        }
        if ((frame_->flags & AV_FRAME_FLAG_CORRUPT) != 0 || frame_->decode_error_flags != 0) {
            av_frame_unref(frame_);
            ++stats_.errors;
            return false;
        }
        ++stats_.frames;
        on_frame_(*frame_);
        av_frame_unref(frame_);
    }
}

bool VideoDecoder::decode(const H264AccessUnit& au) {
    if (au.data.empty() || au.data.size() > INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE) {
        throw std::invalid_argument("invalid H264 access unit size");
    }
    if (au.reset_decoder) {
        reset();
    }
    if (drained_) {
        throw std::logic_error("reset video decoder before decoding after drain");
    }

    av_packet_unref(packet_);
    // av_new_packet 分配并清零 FFmpeg 要求的尾部 padding，不能直接借用 std::string。
    require_success(
        av_new_packet(packet_, static_cast<int>(au.data.size())), "allocate H264 packet"
    );
    std::memcpy(packet_->data, au.data.data(), au.data.size());
    packet_->pts = extend_timestamp(au.rtp_timestamp);
    if (au.is_idr) {
        packet_->flags |= AV_PKT_FLAG_KEY;
    }

    int result = avcodec_send_packet(codec_, packet_);
    if (result == AVERROR(EAGAIN)) {
        if (!receive_frames()) {
            reset();
            return false;
        }
        result = avcodec_send_packet(codec_, packet_);
    }
    av_packet_unref(packet_);
    if (!check_decode_result(result, "send H264 access unit") || !receive_frames()) {
        reset();
        return false;
    }
    return true;
}

bool VideoDecoder::drain() {
    if (drained_) {
        return true;
    }
    int result = avcodec_send_packet(codec_, nullptr);
    if (result == AVERROR(EAGAIN)) {
        if (!receive_frames()) {
            reset();
            drained_ = true;
            return false;
        }
        result = avcodec_send_packet(codec_, nullptr);
    }
    drained_ = true;
    if (result == AVERROR_EOF) {
        return true;
    }
    return check_decode_result(result, "drain H264 decoder") && receive_frames();
}

}  // namespace rtsp
