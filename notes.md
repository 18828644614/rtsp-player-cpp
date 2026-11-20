# 设计核对记录（2026-11-20）

- 服务端 README：一个 /live，Windows RTSP/TCP，H.264 + AAC；现有 CTest 合成入口可做无设备联调。
- rtsp_server_session.cpp：DESCRIBE 返回 Content-Base，SETUP 协商交织通道；GET_PARAMETER 和 TEARDOWN 可用于客户端保活与关停。
- camera_rtsp_tests.cpp：现有测试客户端做两次 SETUP；AAC 轨为 MPEG4-GENERIC、48 kHz、单声道。测试程序的 --serve 会输出测试 URL 并短时运行。
- h264_packetizer.cpp / aac_packetizer.cpp：现有链路只需要单个 H.264 NALU、FU-A，以及单 AU 的 AAC-hbr 接收格式。
- rtsp_stream_parser.cpp / rtsp_message.cpp / session_description.cpp：可借鉴独立的解析代码；播放器不能把服务端的会话/连接对象当成客户端用。

协议参考方向：RFC 2326（RTSP 1.0 交织传输）、RFC 3550（RTP/RTCP）、RFC 6184（H.264 RTP）、RFC 3640（MPEG-4 音频 RTP）。
