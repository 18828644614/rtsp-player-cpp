# C++ RTSP 配套播放器

配套服务器：[rtsp-cpp](../rtsp-cpp/README.md)。独立的 Windows / C++17 项目。

**P3 已实现：建立 RTSP/TCP 双轨会话，解码 H.264，并在 SDL3 窗口按视频 PTS 显示。**
主线程处理窗口事件和画面，一个工作线程负责网络、重组和解码。视频解码链接 `avcodec`、`avutil`，TCP 使用 Winsock。
音频 RTP/RTCP 继续接收和统计；音频输出与跨轨同步留到 P4 / P5。

## 构建和测试

需要 Visual Studio C++ x64 工具链、CMake、SDL3 3.2+ 的 VC x64 开发包，以及包含 H.264 解码器的 FFmpeg x64 shared SDK：

```text
<FFMPEG_ROOT>/
  include/libavcodec/avcodec.h
  include/libavutil/avutil.h
  lib/avcodec.lib
  lib/avutil.lib
  bin/*.dll
```

把以下 `C:/SDK/ffmpeg` 替换为本机 SDK 路径，也可以复用配套服务器已有的 SDK。
SDL3 开发包需包含 `cmake/SDL3Config.cmake`、头文件及 x64 库。以下 `C:/SDK/SDL3` 同样替换为本机路径。
SDK 路径只保存在本机 CMake 缓存，不写入项目源码。

```powershell
Set-Location E:\MarkDown\rtsp-player-cpp
cmake -S . -B build -A x64 -DFFMPEG_ROOT=C:/SDK/ffmpeg -DSDL3_DIR=C:/SDK/SDL3/cmake
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

构建后会把 FFmpeg SDK `bin` 中的 DLL 和 SDL3.dll 复制到程序输出目录。SDK 的运行库另放时，可设置
`-DFFMPEG_RUNTIME_DIR=<DLL目录>`。现有构建目录已经配置 SDK 时，后续直接构建即可。

默认构建程序和 C++ 测试；找到 Python 3 时，额外运行回环会话测试。Python 仅用于测试。
只构建程序可加 `-DBUILD_TESTING=OFF`。Release 使用 `--config Release` 和 `ctest -C Release`。

## 运行

先按服务端 README 启动 `rtsp_server.exe --camera 0 --microphone 0`，再执行：

```powershell
.\build\Debug\rtsp_player.exe rtsp://127.0.0.1:8554/live --seconds 10
```

省略 `--seconds` 会持续播放，关闭窗口、按 Ctrl+C 或 Ctrl+Break 停止。
`--timeout-ms 3000` 设置连接和控制请求超时；退出时 TEARDOWN 最多等待 500 毫秒。
`--seconds` 从 PLAY 成功后开始计时，0 表示持续接收。
主动关闭或定时结束的退出码为 0。服务端断开、控制协议失败、SDL 或解码器资源/API 错误时退出码为 1；码流损坏则丢弃并等待完整 IDR 恢复。

支持数字 IPv4、`localhost`、RTSP 1.0 / RTP over TCP，以及配套服务器的一路 H.264 和一路 MPEG4-GENERIC AAC。
默认端口 554；SETUP 返回的通道、SDP 的 payload type 均在运行时读取。

## 处理流程

```text
TCP → RTSP/交织帧拆分 → RtspClient
                          ├─ 视频 RTP → 校验和统计 → H264Depacketizer
                          │                              ↓ 完整 AU 回调
                          │                         VideoDecoder → 有界帧队列 → SDL3（主线程）
                          ├─ 音频 RTP → 校验和统计
                          └─ RTCP     → 校验和统计
```

`main` 只处理参数、控制台停止和顶层错误。`Player` 连接工作线程与 SDL 主线程；`VideoRenderer` 管理窗口和绘制。
`src/rtsp/`、`src/rtp/` 不依赖 FFmpeg 或 SDL。控制请求等待和正常接收共用分发入口；等待保活响应时媒体仍继续处理。

解码回调用 `av_frame_clone` 保留引用后入队；队列最多 12 帧且 PTS 跨度不超过 300 ms。超限丢弃最旧的已解码帧，不阻塞收包。
第一帧实际准备显示时建立视频 PTS 与 `steady_clock` 的对应关系，后续按 90 kHz PTS 换算目标时间；早到等待，积压时只显示最新的到期帧。
缺失或不递增的 PTS 丢弃；调度遇到超过 2 秒的时间戳跳变时重新锚定。主线程等待事件最多 10 ms。

支持 YUV420P，按帧的 601/709 矩阵与有限/全范围上传；未指定矩阵时按 601。尺寸变化重建纹理，缩放保持显示宽高比（含像素宽高比），空白处留黑边。
其他像素格式明确报错，暂不引入 swscale。

退出时先停止新 AU、丢弃未完成 AU，有限尝试 TEARDOWN，再排空解码器。定时结束会显示完有界队列；主动关闭或断线丢弃待显示帧。
工作线程结束后才销毁 SDL。连接和发送等待每 50 ms 检查取消；TEARDOWN 有独立的最多 500 ms 清理期限。

## H.264 支持范围

- 支持单 NALU、FU-A，以及同一 AU 内的多个 NALU；不支持 STAP-A、交织模式或包重排。
- SDP `packetization-mode` 支持 0 和 1，未指定时按 0；模式 0 拒绝 FU-A。
- SDP `sprop-parameter-sets` 支持一组 SPS/PPS，也支持流内完整 SPS/PPS；参数集变更后等待 IDR。
- 按 RTP 时间戳、序号和 marker 收齐 AU。FU-A 的结束位只表示 NALU 结束。
- 首次接入的时间戳可能从 AU 中间开始，因此首个 AU 不提交；参数集仍可从完整 NALU 中保存。
- 序号断裂、分片缺失、时间戳提前变化、非法负载或容量超限时丢弃损坏 AU，等待完整 IDR。
- AU 默认上限 8 MiB，单个 SPS/PPS 上限 64 KiB；IDR 前补入参数集时也检查总容量。
- FFmpeg packet 使用带尾部 padding 的独立缓冲；每次提交后循环取帧，退出时排空延迟帧。

视频 PTS 使用 1/90000 秒为单位，以首个提交给解码器的 AU 为原点，处理 RTP 回绕和 B 帧重排。
P3 使用这个时间戳安排视频显示，不用于跨轨同步。RTCP Sender Report 仍只解析和统计。

## 输出与统计

以下节选自 Debug 播放器与合成服务端的四秒联调：

```text
[VIDEO] frame=1 pts=0 width=160 height=96 format=yuv420p
[VIDEO] frame=2 pts=4302 width=160 height=96 format=yuv420p
...
[RTSP] TEARDOWN 200
[FINAL] h264 au=75 dropped=7 invalid=0
[FINAL] decode frames=75 errors=0 resets=1
[FINAL] display frames=75 dropped=0 queue_peak=1 clock_resets=0
```

每秒保留视频、音频 RTP/RTCP 与 H.264 重组统计。`[VIDEO]` 仍记录解码输出；结束时分别打印解码与显示汇总。

| 字段 | 含义 |
| --- | --- |
| `rtp` / `bytes` / `payload` | 有效 RTP 包数、总字节数、去除 RTP 头/扩展/填充后的负载字节数。 |
| `ssrc` / `seq` / `ts` | 轨道 SSRC、最近有效 RTP 包的原始序号和时间戳。 |
| `rtcp_frames` / `rtcp_packets` | 交织 RTCP 帧数、其中的 RTCP 子包总数。 |
| `sr` / `rr` / `sdes` / `bye` | 各类 RTCP 子包数量；`sr_ntp` / `sr_rtp` 保存最新 SR 对应点。 |
| 轨道 `bad` | RTP/RTCP 校验失败次数，例如长度错误、PT 不匹配、SSRC 改变。 |
| `unknown_channels` | 无法对应到已协商轨道的交织帧数。 |
| H.264 `au` | 通过完整 AU 回调提交的数量，包括解码器随后拒绝的 AU。 |
| H.264 `dropped` | 初始边界不明、损坏或等待 IDR 期间丢弃的 AU 数，不等同于 RTP 丢包数。 |
| H.264 `invalid` | 不支持或格式错误的 H.264 负载次数；与轨道 `bad` 分开。 |
| 解码 `frames` / `errors` / `resets` | 实际输出帧数、码流错误数、清理解码状态的次数；初次 IDR 也会 reset。 |
| 显示 `frames` / `dropped` | 实际呈现帧数，以及因队列超限、积压、无效 PTS 或退出而未呈现的帧数。 |
| 显示 `queue_peak` / `clock_resets` | 队列帧数峰值，以及时间戳跳变后的重新锚定次数。 |

H.264 `dropped` 包括首次接入和等待 IDR 的丢弃，不能据此认定网络丢包；显示 `dropped` 另统计未呈现的解码帧。
协议校验、H.264 重组、FFmpeg 解码各自报告错误；资源故障会进入会话清理流程。

## 无设备验证

CTest 包含九项测试（Python 不存在时运行六项 C++ 测试）：

| 测试 | 覆盖内容 |
| --- | --- |
| `player_help` | 命令行帮助。 |
| `protocol` | TCP 边界、SDP/URL/Transport/Session、RTP 扩展/填充、RTCP 复合包及统计。 |
| `socket_cancel` | 对端不读时持续发送，验证大超时下仍可主动取消。 |
| `h264` | 单 NALU、FU-A、多 NALU AU、参数集、容量、序号回绕、缺片、坏负载和 IDR 恢复。 |
| `video_decoder` | 固定合成流的实际像素、B 帧延迟、排空、时间戳回绕、损坏恢复及回调异常。 |
| `video_display` | PTS 换算、早到/晚到、跳变、YUV 白色及红蓝像素、黑边、尺寸/SAR 变化和关闭事件。 |
| `session` | 现有 12 个会话场景及命令行错误。 |
| `video_session` | 真实 TCP 会话中的正常视频、FU-A 缺片、非法 RTP、损坏码流和断连排空。 |
| `player_session` | 等待控制响应/保活时关闭、外部停止、播放中关闭、TEARDOWN 超时、SDL 初始化失败、队列丢帧和断线。 |

涉及 SDL 的自动测试使用 dummy 视频驱动和软件渲染，不弹窗口；`player_test_driver` 仅为测试注入关闭事件，正式程序没有测试专用参数。

合成测试样本及生成方式见 [tests/data/README.md](tests/data/README.md)。测试运行不需要编码器或摄像头。

配套服务端已有 `camera_rtsp_tests.exe --serve`，可提供约 12 秒合成双轨流：

```powershell
python tests/server_integration.py --server E:\MarkDown\rtsp-cpp\build-live-learning\Debug\camera_rtsp_tests.exe --player build/Debug/rtsp_player.exe --log build/p3-server-integration-debug.log
```

脚本打开实际 SDL 窗口，验证四秒内连续解码并显示至少 20 帧，尺寸为 160×96、格式为 YUV420P，双轨 RTP/RTCP 正常且 TEARDOWN 成功。
如改用其他合成源，可用 `--expect-width` / `--expect-height` 设置预期尺寸。

P3 验收结果见 [task_plan.md](task_plan.md)，渲染测试同时在构建目录输出 `p3-render-qa.bmp`。

## 代码阅读顺序

| 顺序 | 文件 | 职责 |
| --- | --- | --- |
| 1 | [main.cpp](src/main.cpp) | 参数、退出信号和顶层错误报告。 |
| 2 | [player.cpp](src/player/player.cpp)、[video_clock.h](src/player/video_clock.h) | 工作线程、帧队列、PTS 调度和退出编排。 |
| 3 | [video_renderer.cpp](src/player/video_renderer.cpp) | SDL 资源、事件、YUV 上传和等比绘制。 |
| 4 | [rtsp_client.cpp](src/rtsp/rtsp_client.cpp) | 建会话、接收分发、保活、协议统计和退出。 |
| 5 | [h264_depacketizer.cpp](src/rtp/h264_depacketizer.cpp) | 包连续性、FU-A 拼接、AU 完成、丢弃与 IDR 恢复。 |
| 6 | [h264_parameters.cpp](src/rtp/h264_parameters.cpp) | fmtp、Base64、SPS/PPS 的纯解析。 |
| 7 | [video_decoder.cpp](src/media/video_decoder.cpp) | FFmpeg 资源、提交、取帧、重置和排空。 |
| 8 | [packet_stats.cpp](src/rtp/packet_stats.cpp) | RTP/RTCP 校验、payload 位置和按轨统计。 |
| 9 | [rtsp_stream_parser.cpp](src/rtsp/rtsp_stream_parser.cpp)、[sdp.cpp](src/rtsp/sdp.cpp)、[tcp_socket.cpp](src/net/tcp_socket.cpp) | TCP 字节流、控制协议和连接基础。 |

完整设计见 [DESIGN.md](DESIGN.md)，阶段及验收记录见 [task_plan.md](task_plan.md)。
