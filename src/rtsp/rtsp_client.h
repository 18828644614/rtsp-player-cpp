#pragma once

#include "net/tcp_socket.h"
#include "rtp/packet_stats.h"
#include "rtp/h264_depacketizer.h"
#include "rtp/aac_depacketizer.h"
#include "rtsp/rtsp_stream_parser.h"
#include "rtsp/sdp.h"

#include <chrono>
#include <functional>
#include <exception>
#include <optional>
#include <ostream>
#include <vector>

namespace rtsp {

struct ClientOptions {
    int timeout_ms = 3000;
    int seconds = 0;  // 从 PLAY 成功开始计时；0 为持续接收。
};

struct TrackState {
    MediaTrack media;
    std::string url;
    TcpTransport channels;
    bool setup = false;
    PacketStats stats;
};

// 单线程、一次会话、每次只等待一个 RTSP 响应。
class RtspClient {
public:
    using PlayHandler = std::function<void()>;
    using SenderReportHandler = std::function<void(const MediaTrack&, const SenderReport&)>;
    using VideoHandler = std::function<bool(const H264AccessUnit&)>;
    using AudioConfigHandler = std::function<void(const AacParameters&)>;
    using AudioHandler = std::function<void(const AacAccessUnit&)>;

    RtspClient(
        ClientOptions options, std::ostream& output, std::function<bool()> stop_requested = {}
    );

    // PLAY 成功及有效 SR 的同步通知；必须在 run 前设置。
    void set_play_handler(PlayHandler handler);
    void set_sender_report_handler(SenderReportHandler handler);

    // 同步回调；返回 false 要求丢弃后续依赖帧并等待 IDR。必须在 run 前设置。
    void set_video_handler(VideoHandler handler);
    // 配置在 DESCRIBE 后回调，完整 AU 在收包时同步回调；必须在 run 前设置。
    void set_audio_handlers(AudioConfigHandler on_config, AudioHandler on_audio);

    // 它从一个 RTSP URL 开始，依次完成连接、协商双轨、开始接收，最后负责结束会话和清理资源
    void run(const std::string& url);

    const std::vector<TrackState>& tracks() const {
        return tracks_;
    }

private:
    using Clock = std::chrono::steady_clock;

    RtspMessage request_and_wait(
        const std::string& method,
        const std::string& url,
        const std::vector<RtspHeader>& headers = {},
        bool stopping = false
    );
    std::optional<RtspMessage> receive_and_dispatch(int timeout_ms);
    void dispatch_media(const RtspStreamItem& item);
    void dispatch_rtp(TrackState& track, const std::string& wire);
    void dispatch_video(const RtpHeader& header, const std::string& wire);
    void dispatch_audio(const RtpHeader& header, const std::string& wire);
    void report_bad_packet(TrackState& track, const std::exception& error);
    void finish_media();
    void describe();
    void setup_track(TrackState& track, int first_channel);
    void receive_loop();
    void teardown();
    void print_stats(const char* label);
    bool should_stop() const;
    void check_endpoint(const std::string& url) const;

    PlayHandler play_handler_;
    SenderReportHandler sender_report_handler_;
    ClientOptions options_;
    std::ostream& out_;
    std::function<bool()> stop_requested_;

    TcpSocket socket_;
    RtspStreamParser parser_;
    H264Depacketizer video_;
    VideoHandler video_handler_;
    bool video_active_ = false;
    AacDepacketizer audio_;
    AudioConfigHandler audio_config_handler_;
    AudioHandler audio_handler_;
    bool audio_active_ = false;

    std::string input_url_;
    std::string aggregate_url_;
    std::string session_;
    RtspUrl endpoint_;
    int cseq_ = 0;
    unsigned int session_timeout_seconds_ = 60;
    std::vector<TrackState> tracks_;

    Clock::time_point next_print_;
    Clock::time_point next_keepalive_;
    std::optional<Clock::time_point> play_started_;
    std::uint64_t unknown_channels_ = 0;
};

}  // namespace rtsp
