#include "media/audio_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include <cstring>
#include <stdexcept>
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

AudioDecoder::AudioDecoder(FrameHandler on_frame) : on_frame_(std::move(on_frame)) {
    if (!on_frame_) {
        throw std::invalid_argument("audio frame handler is required");
    }
}

AudioDecoder::~AudioDecoder() {
    close();
}

void AudioDecoder::close() {
    av_frame_free(&frame_);
    av_packet_free(&packet_);
    avcodec_free_context(&codec_);
}

void AudioDecoder::configure(const AacParameters& parameters) {
    if (codec_) {
        throw std::logic_error("AAC decoder is already configured");
    }
    if (parameters.sample_rate != 48000 || parameters.channels != 1 || parameters.config.size() < 2
        || parameters.config.size() > 64) {
        throw std::invalid_argument("invalid AAC decoder configuration");
    }

    try {
        const auto* decoder = avcodec_find_decoder(AV_CODEC_ID_AAC);
        if (!decoder) {
            throw std::runtime_error("FFmpeg AAC decoder is unavailable");
        }

        codec_ = avcodec_alloc_context3(decoder);
        packet_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        if (!codec_ || !packet_ || !frame_) {
            throw std::runtime_error("cannot allocate FFmpeg audio decoder resources");
        }

        // 用来保存解码所需的额外配置
        // extradata_size
        //   = 配置本身的有效字节数

        // 实际分配大小
        //   = 配置字节数 + FFmpeg 要求的尾部 padding
        codec_->extradata_size = static_cast<int>(parameters.config.size());
        codec_->extradata = static_cast<std::uint8_t*>(
            av_mallocz(parameters.config.size() + AV_INPUT_BUFFER_PADDING_SIZE)
        );
        if (!codec_->extradata) {
            throw std::runtime_error("cannot allocate AAC AudioSpecificConfig");
        }
        std::memcpy(codec_->extradata, parameters.config.data(), parameters.config.size());

        codec_->sample_rate = parameters.sample_rate;
        av_channel_layout_default(&codec_->ch_layout, parameters.channels);
        codec_->pkt_timebase = {1, parameters.sample_rate};

        // 解码选项
        codec_->thread_count = 1;
        codec_->err_recognition = AV_EF_CAREFUL | AV_EF_EXPLODE;
        require_success(avcodec_open2(codec_, decoder, nullptr), "open AAC decoder");
    } catch (...) {
        close();
        throw;
    }
}

const AudioDecodeStats& AudioDecoder::stats() const {
    return stats_;
}

void AudioDecoder::reset() {
    avcodec_flush_buffers(codec_);
    av_packet_unref(packet_);
    av_frame_unref(frame_);
}

bool AudioDecoder::check_result(int result, const char* operation) {
    if (result == AVERROR_INVALIDDATA) {
        ++stats_.errors;
        return false;
    }
    require_success(result, operation);
    return true;
}

bool AudioDecoder::receive_frames() {
    while (true) {
        const int result = avcodec_receive_frame(codec_, frame_);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            return true;
        }
        if (!check_result(result, "receive AAC frame")) {
            return false;
        }
        if ((frame_->flags & AV_FRAME_FLAG_CORRUPT) != 0 || frame_->decode_error_flags != 0) {
            ++stats_.errors;
            av_frame_unref(frame_);
            return false;
        }
        if (frame_->sample_rate != 48000 || frame_->ch_layout.nb_channels != 1
            || frame_->nb_samples != 1024) {
            throw std::runtime_error("AAC output must be 48000 Hz mono, 1024 samples");
        }
        ++stats_.frames;
        stats_.samples += frame_->nb_samples;
        on_frame_(*frame_);
        av_frame_unref(frame_);
    }
}

bool AudioDecoder::decode(const AacAccessUnit& au) {
    if (!codec_ || drained_) {
        throw std::logic_error("AAC decoder is not configured or has been drained");
    }
    if (au.data.empty() || au.data.size() > 8191) {
        throw std::invalid_argument("invalid AAC access unit size");
    }
    av_packet_unref(packet_);
    require_success(
        av_new_packet(packet_, static_cast<int>(au.data.size())), "allocate AAC packet"
    );
    std::memcpy(packet_->data, au.data.data(), au.data.size());
    packet_->pts = au.pts;
    packet_->duration = 1024;
    int result = avcodec_send_packet(codec_, packet_);
    if (result == AVERROR(EAGAIN)) {
        if (!receive_frames()) {
            reset();
            return false;
        }
        result = avcodec_send_packet(codec_, packet_);
    }
    av_packet_unref(packet_);
    if (!check_result(result, "send AAC access unit") || !receive_frames()) {
        reset();
        return false;
    }
    return true;
}

bool AudioDecoder::drain() {
    if (!codec_ || drained_) {
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
    return result == AVERROR_EOF || (check_result(result, "drain AAC decoder") && receive_frames());
}

}  // namespace rtsp
