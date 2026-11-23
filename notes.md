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

## P2 实现核对（2026-11-23）

- 配套服务器 h264_packetizer.cpp 只输出单 NALU / FU-A；FU-A E 位表示 NALU 结束，整个 AU 的 marker 由发送链路另设。
- 配套服务器 make_h264_fmtp() 输出 packetization-mode=1 和 Base64 SPS/PPS；测试入口默认视频尺寸 160×96，GOP 为 6。
- 本机构建复用服务器已有的 shared SDK，通过 FFMPEG_ROOT 配置，播放器没有链接服务器目标或修改服务器源码。
- 先实现纯 RTP/H.264，再接独立 FFmpeg 目标。参数解析、FU-A、AU 完成、解码提交/取帧各有独立函数。
- Debug 首轮五项 CTest 通过；补充五个 TCP 视频故障场景后，Debug / Release 最终 CTest 均 6/6 通过。
- 固定合成样本含八帧，解码输入顺序 I0 P3 B1 B2 I4 P7 B5 B6；用于验证 B 帧重排、padding、排空和时间戳回绕。生成方式在 tests/data/README.md。
- Debug 四秒实际联调：video RTP=216、audio RTP=184，解出 79 帧；H264 dropped=6、invalid=0，decode errors=0、resets=1。
- Release 四秒实际联调：video RTP=211、audio RTP=182，解出 78 帧；H264 dropped=6、invalid=0，decode errors=0、resets=1。
- dropped=6 为首次接入/等待 IDR 的丢弃，不代表六个 RTP 包丢失。
- 两次实际联调均为 160×96 / YUV420P，两轨 bad=0、unknown_channels=0，TEARDOWN=200。
- Release dumpbin 确认程序直接链接 avcodec-63.dll、avutil-61.dll，无 avformat、SDL3。
- 联调日志：忽略目录 build/p2-server-integration-debug.log、build/p2-server-integration-release.log。

## P3 实现核对（2026-11-23）

- 使用官方 SDL3 3.4.20 VC 开发包，保存于忽略目录 build/deps，通过 SDL3_DIR 配置，不提交 SDK。
- Player 编排两个执行流程；VideoRenderer 的所有 SDL 窗口和绘制调用都在主线程。
- 帧回调使用 av_frame_clone 保留引用；队列最多 12 帧且跨度不超过 300 ms，满时丢弃旧的已解码帧，不阻塞网络。
- 保留 P2 的 [VIDEO] 解码日志；新增 [FINAL] display 区分解码帧与实际呈现帧。
- TcpSocket 等待按 50 ms 分片检查取消，TEARDOWN 忽略主动停止但仍受最多 500 ms 期限限制。
- SDL_RenderReadPixels 会裁剪到逻辑视口；QA 先切回全窗口视口，再验证黑边。已检查红/蓝测试图，U/V 顺序、有限范围、矩阵与等比黑边均正常。
- 新增 socket_cancel、video_display、player_session；播放测试驱动定时注入真实 SDL 关闭事件，正式程序不增加测试开关。
- 最终 Debug / Release CTest 各 9/9；实际窗口四秒联调分别解码并显示 75 / 76 帧，显示无丢帧、队列峰值 1。Release 直接依赖新增 SDL3.dll，没有 avformat/swscale；未修改配套服务端。
