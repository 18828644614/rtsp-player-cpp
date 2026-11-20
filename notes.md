# 设计核对记录（2026-11-20）

- 服务端 README：一个 /live，Windows RTSP/TCP，H.264 + AAC；现有 CTest 合成入口可做无设备联调。
- rtsp_server_session.cpp：DESCRIBE 返回 Content-Base，SETUP 协商交织通道；GET_PARAMETER 和 TEARDOWN 可用于客户端保活与关停。
- camera_rtsp_tests.cpp：现有测试客户端做两次 SETUP；AAC 轨为 MPEG4-GENERIC、48 kHz、单声道。测试程序的 --serve 会输出测试 URL 并短时运行。
- h264_packetizer.cpp / aac_packetizer.cpp：现有链路只需要单个 H.264 NALU、FU-A，以及单 AU 的 AAC-hbr 接收格式。
- rtsp_stream_parser.cpp / rtsp_message.cpp / session_description.cpp：可借鉴独立的解析代码；播放器不能把服务端的会话/连接对象当成客户端用。

协议参考方向：RFC 2326（RTSP 1.0 交织传输）、RFC 3550（RTP/RTCP）、RFC 6184（H.264 RTP）、RFC 3640（MPEG-4 音频 RTP）。

## P1 实现核对（2026-11-20）

- 服务端 DESCRIBE 以请求 URL 生成带末尾斜杠的 Content-Base；聚合操作仍使用原始 `/live`。
- 第二次 SETUP 必须带第一次响应中的 Session；Transport 响应决定实际通道。
- 服务端合成测试入口已有编码 DLL，播放器联调只启动已有程序，不修改、重编译服务器。
- 首轮实际联调 4 秒：video RTP=216，audio RTP=184，两轨 SR=64，bad=0，unknown_channels=0；TEARDOWN=200。
- Release 可执行文件经 dumpbin /dependents 检查：只有 Winsock、Windows 系统库和 MSVC/UCRT，无 FFmpeg 或 SDL3。
- 增量解析以完整 Content-Length 或交织帧长度为边界；不搜索负载内部的 `$`。
- 单轨只接受一个 SSRC；统计提交前先校验整个数据包/RTCP 复合包，非法数据不部分累加。
- 复制 SDP 数字解析时补充末位上限检查，防止 channels=9 通过最大值 8 的检查；有对应回归用例。

- 最终 Release 联调 4 秒：video RTP=211、audio RTP=183，两轨 SR=64、BYE=1、bad=0；TEARDOWN=200。Debug / Release CTest 各 3/3 通过，session 含 12 个回环场景。
