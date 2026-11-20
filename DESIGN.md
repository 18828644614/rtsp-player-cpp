# 配套 RTSP 播放器框架设计

## 目标与范围

在 Windows 上做一个独立的 C++17 桌面播放器，播放同级项目 [rtsp-cpp](../rtsp-cpp/README.md) 的 /live。一个程序、一个输入 URL、一个 SDL3 播放窗口。不修改服务器，不在播放器里链接服务器目标，也不要求两边放在同一个构建目录。

- 自己处理 Winsock TCP、RTSP 请求与响应、交织帧分离、RTP 拆包、RTCP 时间映射和播放调度。
- FFmpeg 只负责 H.264/AAC 解码、必要的像素转换与音频重采样，不通过 libavformat 打开 RTSP URL。
- SDL3 只负责窗口事件、画面呈现和音频输出。不引入 Qt、Web 前端或独立 UI 线程框架。
- 首版只支持配套服务器的 RTSP 1.0 + RTP over TCP、一路 H.264 和一路 AAC-LC。不做 UDP、认证、自动重连、拖动进度条、录像、多窗口或通用播放器兼容性。

## 一条主链路

~~~text
                       RTSP 响应 ─→ 会话状态（SDP、Session、通道）
TCP → 字节流拆分 ─┼─→ RTP 视频  ─→ H.264 AU ─→ 解码 ─→ 视频帧队列 ─→ SDL3 画面
                  ├─→ RTP 音频  ─→ AAC AU   ─→ 解码/重采样 ─→ PCM ─→ SDL3 音频
                  └─→ RTCP      ─→ RTP/NTP 映射 ─→ 同一播放时间轴
~~~

## 模块与依赖

目录按阶段引入，不先创建一排空类。P1 已实现 src/net/、src/rtsp/、src/rtp/；src/media/ 和 src/player/ 留到后续阶段。

| 位置 | 职责 |
| --- | --- |
| src/main.cpp | 读取一个 RTSP URL，启动播放器，报告错误。 |
| src/net/ | 一个 TCP 连接，有限读写等待，支持唤醒接收并退出。 |
| src/rtsp/ | RTSP 文本和交织帧解析、SDP、控制 URL 和客户端会话。不能依赖 FFmpeg/SDL3。 |
| src/rtp/ | 读取 RTP/RTCP，按 SETUP 返回的通道分发；将 RTP payload 拆成 H.264/AAC AU。不能解码或绘制。 |
| src/media/ | 两个 FFmpeg 解码器、音频重采样、播放时钟和队列。复用统一的带时间戳数据结构，避免再加通用 Source/Plugin 框架。 |
| src/player/ | 一个 Player 串起处理流程；SDL3 主线程负责事件循环和视频呈现，音频交给 SDL3 设备流。 |
| tests/ | 纯 C++ 协议测试、回环假服务器，以及对配套服务器的集成验收。 |

正式构建只生成 rtsp_player。P1 加 ws2_32；P2 加 FFmpeg avcodec/avutil，按需加 swscale；播放阶段引入 SDL3，需要重采样时加 swresample。各阶段只加实际需要的依赖。包路径由本机构建配置指定，不提交机器专属路径。

## 关键设计

### 1. RTSP 和 TCP 是两层

TCP 接收的数据先增量拆成 RTSP 消息或以美元符号开头的交织帧，不能假定一次 recv 就是一条完整回复。RTSP 回复用 CSeq 匹配；交织帧按协商通道号分派。等待 GET_PARAMETER 或 TEARDOWN 的回复时也要继续拆分收到的媒体数据，不能把它错当成回复。

正常流程：OPTIONS → DESCRIBE → SETUP 视频 → SETUP 音频 → PLAY。DESCRIBE 读取 SDP，结合 Content-Base 解析两个 control URI；SETUP 核对响应中的 Transport，缓存 Session 及其超时；PLAY 走聚合 URL。播放中发带 Session 的 GET_PARAMETER 保活。窗口关闭时在有限时间内尝试 TEARDOWN，服务端断开时退出，不无限等响应。

基础解析对照服务端已有实现，优先复用无状态、无服务端依赖的代码。客户端状态机、两次 SETUP 和媒体分发仍由本项目实现。用服务端日志及测试抓到的字节验证行为，不硬编码动态 payload type 或交织通道。

### 2. 协议包不等于解码帧

H.264 首版支持现有服务器送出的单 NALU 和 FU-A。按 RTP 时间戳和 marker 收齐 AU，并处理 SDP 的 SPS/PPS 或流内参数集；只送完整 AU 给解码器。序号不连续、FU-A 截断或重组超出容量时丢弃损坏的 AU，等待可解码的 IDR。

AAC 按当前服务器的 AAC-hbr 格式读取 AU 头长度和 AU 大小，首版只支持每个 RTP 包携带一个完整 AU；不把 RTP 或 AU 头交给解码器。用 SDP fmtp 中的 config 作为 AudioSpecificConfig，不额外拼 ADTS 头。

### 3. 区分媒体时间、接收时间和播放时间

视频时钟 90 kHz、音频时钟 48 kHz，两者不能直接比较。按轨道和 SSRC 保存 RTCP Sender Report 的 RTP/NTP 对应点，将各轨 RTP 时间戳扩展后换算到同一时间轴。两路不能各自从首包时间归零，也不能用网络到达顺序代替采集时间。等待两路 SR 时预缓冲有上限；超时则报告无法建立同步，不静默伪造时间差。

正常播放时，用带 PTS 的 PCM 和输出缓冲估算音频播放位置；明确这不是精确的硬件时钟。视频早到就等，晚太多则跳过已解码的视频帧。音频欠载时不把缺音的时段当成正常播放进度；缓冲持续失衡时重新预缓冲并记录状态。以仿真时间和可重复音视频信号验证，不只凭“看起来差不多”。

### 4. 先用两段执行流程

P1/P2 可用单线程验证。接窗口后，主线程处理 SDL3 事件和渲染；一个工作线程串行处理网络、拆包和解码，避免为每个模块各起一个线程。视频帧队列与音频缓冲按时长/容量设上限，不允许无限累积延迟。主线程关闭时发停止信号，工作线程在有限时间内停止并释放 Socket，随后再销毁播放器资源。

## 里程碑与验收

| 阶段 | 这次只做什么 | 怎么知道它好了 |
| --- | --- | --- |
| P1 协议 | 会话 + TCP 拆分 + RTP/RTCP 计数；不做解码 | 看到 OPTIONS、DESCRIBE、两个 SETUP、PLAY；分别收到两轨包；测试 TCP 半包/粘包、错 CSeq、超时、保活、断连和 TEARDOWN。 |
| P2 解码 | H.264 重组、FFmpeg 解码；只打印帧信息 | 连续解出视频，尺寸与服务端一致；分片或 AU 丢失不提交坏帧，下一 IDR 可恢复。 |
| P3 出画 | SDL3 窗口、视频单轨调度 | 窗口持续出画、缩放不拉伸、关闭不挂住。此阶段仍协商并接收双轨，暂不播音频。 |
| P4 出声 | AAC 拆包、解码、PCM 输出 | 听到声音；连续输出不卡死；音频队列和设备缓冲有上限。此阶段不宣称双轨已经同步。 |
| P5 同步 | RTCP SR + 音频时钟 + 视频等帧/丢帧 | 模拟两轨起始相差 120 ms 且 RTP 回绕，仍得到正确的相对时间；用拍手或可控同步信号检查实际播放。 |

从 P1 开始，可以用服务端现有的 camera_rtsp_tests.exe --serve 提供短时双轨测试流，不需要摄像头或麦克风；启动后用它打印的 URL。保活测试另用可控超时的回环假服务器。最终接真实双设备，并以 ffplay 作为对照以区分服务端问题和播放器问题。

## P1 实际落地（2026-11-20）

- 三个主要类：`TcpSocket`、`RtspStreamParser`、`RtspClient`；其他协议解析和统计使用普通函数与结构体。
- `RtspClient::run()` 顺序执行 OPTIONS、DESCRIBE、两次 SETUP、PLAY 和接收循环。
- `request_and_wait()` 与接收循环都调用 `receive_and_dispatch()`；等待控制回复时继续统计交织媒体。
- SDP、Transport、Session 和控制 URL 的纯解析集中在 `src/rtsp/sdp.h/.cpp`。
- RTP/RTCP 头校验、复合 RTCP 遍历及统计集中在 `src/rtp/packet_stats.h/.cpp`。
- 每次只允许一个控制请求等待回复；有 CSeq、Session、通道冲突、长度及缓冲上限检查。
- 默认请求超时 3 秒，接收等待以 100 毫秒为单位返回；按 Session 超时的一半周期发 GET_PARAMETER，TEARDOWN 最多等 500 毫秒。
- `a=control:*` 的 PLAY / TEARDOWN 使用输入的聚合 URL；Content-Base 用于解析轨道的相对 control，避免把 `/live/` 当作 `/live`。
- 当前支持数字 IPv4 / localhost、一轨一个 SSRC，不实现 DNS、认证、重连、解码和 RTCP Receiver Report 发送。
- `tests/protocol_tests.cpp` 验证纯协议；`tests/session_tests.py` 验证真实可执行程序与回环假服务器；`tests/server_integration.py` 负责配套服务端联调。
