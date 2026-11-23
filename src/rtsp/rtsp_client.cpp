#include "rtsp/rtsp_client.h"

#include <algorithm>
#include <climits>
#include <stdexcept>
#include <utility>

namespace rtsp {
namespace {

struct StopRequested {};

std::string lower(std::string value) {
    for (auto& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return value;
}

std::string single_header(const RtspMessage& message, const std::string& name) {
    std::string value;
    bool found = false;

    for (const auto& header : message.headers) {
        if (lower(header.name) != lower(name)) {
            continue;
        }
        if (found) {
            throw std::runtime_error("duplicate RTSP " + name);
        }
        value = header.value;
        found = true;
    }
    return value;
}

}  // namespace

RtspClient::RtspClient(
    ClientOptions options, std::ostream& output, std::function<bool()> stop_requested
)
    : options_(options), out_(output), stop_requested_(std::move(stop_requested)) {
    if (options_.timeout_ms <= 0 || options_.seconds < 0) {
        throw std::invalid_argument("invalid client timeout or duration");
    }
}

void RtspClient::set_video_handler(VideoHandler handler) {
    video_handler_ = std::move(handler);
}

void RtspClient::finish_video() {
    if (video_active_) {
        video_.finish();
        video_active_ = false;
    }
}

bool RtspClient::should_stop() const {
    return (stop_requested_ && stop_requested_())
           || (play_started_ && options_.seconds > 0
               && Clock::now() - *play_started_ >= std::chrono::seconds(options_.seconds));
}

void RtspClient::check_endpoint(const std::string& url) const {
    const auto parsed = parse_rtsp_url(url);
    const auto ip = [](const std::string& host) {
        return host == "localhost" ? "127.0.0.1" : host;
    };
    if (ip(parsed.host) != ip(endpoint_.host) || parsed.port != endpoint_.port) {
        throw std::runtime_error("control URL points outside this TCP session");
    }
}

void RtspClient::run(const std::string& url) {
    // 保存用户最初输入的地址，OPTIONS 和 DESCRIBE 使用它
    input_url_ = url;
    // 表示控制整个音视频会话的地址，PLAY、保活和 TEARDOWN 使用它
    aggregate_url_ = url;
    endpoint_ = parse_rtsp_url(url);
    next_print_ = Clock::now() + std::chrono::seconds(1);

    try {
        if (should_stop()) {
            throw StopRequested{};
        }
        socket_.connect(endpoint_.host, endpoint_.port, options_.timeout_ms);
        out_ << "[TCP] connected " << endpoint_.host << ':' << endpoint_.port << '\n';

        request_and_wait("OPTIONS", input_url_);
        describe();
        setup_track(tracks_[0], 0);
        setup_track(tracks_[1], 2);

        request_and_wait("PLAY", aggregate_url_);
        play_started_ = Clock::now();
        next_keepalive_ =
            *play_started_
            + std::chrono::milliseconds(static_cast<std::int64_t>(session_timeout_seconds_) * 500);
        receive_loop();
    } catch (const StopRequested&) {
        out_ << "[RTSP] stop requested\n";
    } catch (...) {
        finish_video();
        teardown();
        socket_.close();
        print_stats("FINAL");
        throw;
    }

    finish_video();
    teardown();
    socket_.close();
    print_stats("FINAL");
}

// method: RTSP 方法，例如 DESCRIBE。
// url: 整个会话或单条轨道的地址。
// headers: 额外头字段，例如 Accept 或 Transport。
// stopping: 退出清理时传 true，用于发送 TEARDOWN。
RtspMessage RtspClient::request_and_wait(
    const std::string& method,
    const std::string& url,
    const std::vector<RtspHeader>& headers,
    bool stopping
) {
    if (!stopping && should_stop()) {
        throw StopRequested{};
    }
    if (cseq_ == INT_MAX) {
        throw std::runtime_error("CSeq exhausted");
    }

    RtspMessage request;
    request.method = method;
    request.uri = url;
    request.headers = headers;
    request.set_header("CSeq", std::to_string(++cseq_));
    request.set_header("User-Agent", "rtsp-player-cpp/P2");
    if (!session_.empty()) {
        request.set_header("Session", session_);
    }

    const int timeout = stopping ? std::min(options_.timeout_ms, 500) : options_.timeout_ms;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout);
    socket_.send_all(serialize_rtsp_message(request), timeout);

    // 等待服务器对刚才那条请求作出回复
    while (true) {
        if (!stopping && should_stop()) {
            throw StopRequested{};
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining <= 0) {
            throw std::runtime_error(method + " response timeout");
        }

        const auto response =
            receive_and_dispatch(static_cast<int>(std::min<std::int64_t>(remaining, 100)));
        if (!response) {
            continue;
        }

        if (response->is_request
            || std::stoul(response->header("CSeq")) != static_cast<unsigned long>(cseq_)) {
            throw std::runtime_error(method + " received wrong CSeq or a server request");
        }
        out_ << "[RTSP] " << method << ' ' << response->status_code << '\n';
        if (response->status_code != 200) {
            throw std::runtime_error(
                method + " failed: " + std::to_string(response->status_code) + " "
                + response->reason
            );
        }

        const auto session = single_header(*response, "Session");
        if (!session.empty()) {
            const auto parsed = parse_session_header(session);
            if (!session_.empty() && parsed.id != session_) {
                throw std::runtime_error("Session id changed");
            }
            if (parsed.timeout_seconds) {
                session_timeout_seconds_ = *parsed.timeout_seconds;
            }
        } else if (!session_.empty()) {
            throw std::runtime_error(method + " response is missing Session");
        }
        return *response;
    }
}

std::optional<RtspMessage> RtspClient::receive_and_dispatch(int timeout_ms) {
    // 先处理遗留的完整消息，再 recv。返回一条响应后，剩余消息仍在解析器内。
    bool received = false;
    while (true) {
        RtspStreamItem item;
        while (parser_.next(item)) {
            // rtsp 消息
            if (!item.is_interleaved) {
                return item.message;
            }
            // rtp 消息
            dispatch_media(item);
        }

        if (Clock::now() >= next_print_) {
            if (play_started_) {
                print_stats("STATS");
            }
            next_print_ = Clock::now() + std::chrono::seconds(1);
        }

        // 让本次函数调用最多调用一次 socket_.receive()
        // 这样处理完一批数据后就能回到外层
        if (received) {
            return std::nullopt;
        }

        char buffer[16384];
        const auto result = socket_.receive(buffer, sizeof(buffer), timeout_ms);
        if (result.status == ReadStatus::Timeout) {
            return std::nullopt;
        }
        if (result.status == ReadStatus::Eof) {
            socket_.close();
            parser_.finish();
            throw std::runtime_error("server disconnected");
        }
        parser_.append(buffer, result.size);
        received = true;
    }
}

void RtspClient::describe() {
    const auto response = request_and_wait("DESCRIBE", input_url_, {{"Accept", "application/sdp"}});
    const auto type = lower(single_header(response, "Content-Type"));
    if (type.substr(0, type.find(';')) != "application/sdp") {
        throw std::runtime_error("DESCRIBE did not return application/sdp");
    }

    const auto description = parse_sdp(response.body);
    if (description.tracks.size() != 2) {
        throw std::runtime_error("requires one video and one audio track");
    }

    auto base = single_header(response, "Content-Base");
    if (base.empty()) {
        base = input_url_;
    }
    check_endpoint(base);
    // '*' 表示整个描述的聚合控制；配套服务端 Content-Base 末尾带 '/'，
    // PLAY 仍使用最初的 /live，不能误发为 /live/。
    aggregate_url_ = description.control.empty() || description.control == "*"
                         ? input_url_
                         : resolve_control_url(base, description.control);
    check_endpoint(aggregate_url_);

    for (const auto& kind : {"video", "audio"}) {
        const auto track = std::find_if(
            description.tracks.begin(), description.tracks.end(),
            [kind](const MediaTrack& media) { return media.media_type == kind; }
        );
        if (track == description.tracks.end()) {
            throw std::runtime_error("missing video or audio track");
        }

        TrackState state;
        state.media = *track;
        state.url = resolve_control_url(base, track->control);
        check_endpoint(state.url);
        for (const auto& previous : tracks_) {
            if (previous.url == state.url) {
                throw std::runtime_error("duplicate resolved track URL");
            }
        }
        out_ << "[SDP] " << kind << ' ' << track->encoding << " pt=" << track->payload_type
             << " clock=" << track->clock_rate << " control=" << state.url << '\n';
        tracks_.push_back(std::move(state));
    }
    if (video_handler_) {
        video_.configure(tracks_[0].media.fmtp);
        video_active_ = true;
    }
}

void RtspClient::setup_track(TrackState& track, int first_channel) {
    const auto response = request_and_wait(
        "SETUP", track.url, {{"Transport", make_tcp_transport({first_channel, first_channel + 1})}}
    );
    const auto header = single_header(response, "Session");
    if (header.empty()) {
        throw std::runtime_error("SETUP response is missing Session");
    }
    const auto parsed = parse_session_header(header);
    session_ = parsed.id;  // 即使 Transport 校验失败，也尽量释放服务端已创建的会话。

    TcpTransport transport;
    if (!parse_tcp_transport(single_header(response, "Transport"), transport)) {
        throw std::runtime_error("SETUP did not negotiate RTP/AVP/TCP interleaved transport");
    }
    for (const auto& previous : tracks_) {
        if (previous.setup
            && (transport.rtp_channel == previous.channels.rtp_channel
                || transport.rtp_channel == previous.channels.rtcp_channel
                || transport.rtcp_channel == previous.channels.rtp_channel
                || transport.rtcp_channel == previous.channels.rtcp_channel)) {
            throw std::runtime_error("SETUP returned overlapping channels");
        }
    }

    track.channels = transport;
    track.setup = true;
    out_ << "[SETUP] " << track.media.media_type << " channels=" << transport.rtp_channel << '/'
         << transport.rtcp_channel << " session=" << session_
         << " timeout=" << session_timeout_seconds_ << '\n';
}

void RtspClient::dispatch_media(const RtspStreamItem& item) {
    for (auto& track : tracks_) {
        if (!track.setup) {
            continue;
        }
        const bool rtp = item.channel == static_cast<unsigned int>(track.channels.rtp_channel);
        if (!rtp && item.channel != static_cast<unsigned int>(track.channels.rtcp_channel)) {
            continue;
        }
        if (rtp) {
            dispatch_rtp(track, item.payload);
        } else {
            try {
                count_rtcp(track.stats, item.payload);
            } catch (const std::runtime_error& error) {
                report_bad_packet(track, error);
            }
        }
        return;
    }
    ++unknown_channels_;
    if (unknown_channels_ <= 3) {
        out_ << "[WARN] unknown interleaved channel=" << item.channel << '\n';
    }
}

void RtspClient::report_bad_packet(TrackState& track, const std::exception& error) {
    ++track.stats.bad;
    if (track.stats.bad <= 3) {
        out_ << "[WARN] " << track.media.media_type << " bad packet: " << error.what() << '\n';
    }
}

void RtspClient::dispatch_rtp(TrackState& track, const std::string& wire) {
    RtpHeader header;
    try {
        header = count_rtp(track.stats, wire, track.media.payload_type);
    } catch (const std::runtime_error& error) {
        report_bad_packet(track, error);
        if (video_active_ && track.media.media_type == "video") {
            video_.discard();
        }
        return;
    }
    if (video_active_ && track.media.media_type == "video") {
        dispatch_video(header, wire);
    }
}

void RtspClient::dispatch_video(const RtpHeader& header, const std::string& wire) {
    std::optional<H264AccessUnit> au;
    try {
        const std::string_view payload(wire.data() + header.payload_offset, header.payload_size);
        au = video_.push(header, payload);
    } catch (const std::runtime_error& error) {
        if (video_.stats().invalid <= 3) {
            out_ << "[WARN] H264 reassembly: " << error.what() << '\n';
        }
        return;
    }
    // 回调中的解码/资源异常必须交给会话清理流程，不能误算为协议坏包。
    if (au && !video_handler_(*au)) {
        video_.discard();
    }
}

void RtspClient::receive_loop() {
    while (!should_stop()) {
        if (Clock::now() >= next_keepalive_) {
            request_and_wait("GET_PARAMETER", aggregate_url_);
            next_keepalive_ = Clock::now()
                              + std::chrono::milliseconds(
                                  static_cast<std::int64_t>(session_timeout_seconds_) * 500
                              );
        } else if (receive_and_dispatch(100)) {
            throw std::runtime_error("unexpected RTSP response while streaming");
        }
    }
}

void RtspClient::teardown() {
    if (session_.empty() || !socket_.is_open()) {
        return;
    }
    try {
        request_and_wait("TEARDOWN", aggregate_url_, {}, true);
    } catch (const std::exception& error) {
        out_ << "[WARN] TEARDOWN: " << error.what() << '\n';
    }
    session_.clear();
}

void RtspClient::print_stats(const char* label) {
    for (const auto& track : tracks_) {
        const auto& s = track.stats;
        out_ << '[' << label << "] " << track.media.media_type << " rtp=" << s.rtp_packets
             << " bytes=" << s.rtp_bytes << " payload=" << s.payload_bytes;
        if (s.ssrc) {
            out_ << " ssrc=" << *s.ssrc << " seq=" << s.last_sequence << " ts=" << s.last_timestamp;
        }
        out_ << " rtcp_frames=" << s.rtcp_frames << " rtcp_packets=" << s.rtcp_packets
             << " sr=" << s.sr << " rr=" << s.rr << " sdes=" << s.sdes << " bye=" << s.bye
             << " bad=" << s.bad;
        if (s.latest_sr) {
            out_ << " sr_ntp=" << s.latest_sr->ntp << " sr_rtp=" << s.latest_sr->rtp_timestamp;
        }
        out_ << '\n';
    }
    if (!tracks_.empty()) {
        out_ << '[' << label << "] unknown_channels=" << unknown_channels_ << '\n';
    }
    if (video_handler_) {
        const auto& stats = video_.stats();
        out_ << '[' << label << "] h264 au=" << stats.access_units << " dropped=" << stats.dropped
             << " invalid=" << stats.invalid << '\n';
    }
    out_.flush();
}

}  // namespace rtsp
