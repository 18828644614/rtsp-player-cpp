# C++ RTSP 配套播放器

配套服务器：[rtsp-cpp](../rtsp-cpp/README.md)。独立的 Windows / C++17 项目。

**P2 已实现：建立一个 RTSP/TCP 双轨会话，重组 H.264 并用 FFmpeg 连续解帧，在终端打印帧信息。**
应用逻辑保持单线程。视频解码直接链接 `avcodec`、`avutil`，TCP 使用 Winsock。
音频 RTP/RTCP 继续接收和统计；窗口显示、音频解码与音视频同步留到 P3–P5。

## 构建和测试

需要 Visual Studio C++ x64 工具链、CMake，以及包含 H.264 解码器的 FFmpeg x64 shared SDK：

```text
<FFMPEG_ROOT>/
  include/libavcodec/avcodec.h
  include/libavutil/avutil.h
  lib/avcodec.lib
  lib/avutil.lib
  bin/*.dll
```

把以下 `C:/SDK/ffmpeg` 替换为本机 SDK 路径，也可以复用配套服务器已有的 SDK。
SDK 路径只保存在本机 CMake 缓存，不写入项目源码。

```powershell
Set-Location E:\MarkDown\rtsp-player-cpp
cmake -S . -B build -A x64 -DFFMPEG_ROOT=C:/SDK/ffmpeg
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

构建后会把 SDK `bin` 中的 DLL 复制到程序输出目录。SDK 的运行库另放时，可设置
`-DFFMPEG_RUNTIME_DIR=<DLL目录>`。现有构建目录已经配置 SDK 时，后续直接构建即可。

默认构建程序和 C++ 测试；找到 Python 3 时，额外运行回环会话测试。Python 仅用于测试。
只构建程序可加 `-DBUILD_TESTING=OFF`。Release 使用 `--config Release` 和 `ctest -C Release`。

## 运行

先按服务端 README 启动 `rtsp_server.exe --camera 0 --microphone 0`，再执行：

```powershell
.\build\Debug\rtsp_player.exe rtsp://127.0.0.1:8554/live --seconds 10
```

省略 `--seconds` 会持续接收，按 Ctrl+C 或 Ctrl+Break 停止。
`--timeout-ms 3000` 设置连接和控制请求超时；退出时 TEARDOWN 最多等待 500 毫秒。
`--seconds` 从 PLAY 成功后开始计时，0 表示持续接收。
服务端断开、控制协议失败或解码器资源/API 错误时退出码为 1；码流损坏则丢弃并等待完整 IDR 恢复。

支持数字 IPv4、`localhost`、RTSP 1.0 / RTP over TCP，以及配套服务器的一路 H.264 和一路 MPEG4-GENERIC AAC。
默认端口 554；SETUP 返回的通道、SDP 的 payload type 均在运行时读取。

## 处理流程

```text
TCP → RTSP/交织帧拆分 → RtspClient
                          ├─ 视频 RTP → 校验和统计 → H264Depacketizer
                          │                              ↓ 完整 AU 回调
                          │                         VideoDecoder → 帧信息
                          ├─ 音频 RTP → 校验和统计
                          └─ RTCP     → 校验和统计
```

`main` 创建解码器并注册同步回调。`src/rtsp/`、`src/rtp/` 不依赖 FFmpeg。
控制请求等待和正常接收共用分发入口；等待保活响应时，媒体仍继续处理。
退出时停止接收新视频 AU，丢弃未完成 AU，执行 TEARDOWN，再排空解码器已经接收的数据。

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
这只是视频解码时间戳，不用于跨轨同步。RTCP Sender Report 仍只解析和统计。

## 输出与统计

以下节选自 Debug 播放器与合成服务端的四秒联调：

```text
[VIDEO] frame=1 pts=0 width=160 height=96 format=yuv420p
[VIDEO] frame=2 pts=4297 width=160 height=96 format=yuv420p
...
[RTSP] TEARDOWN 200
[FINAL] h264 au=79 dropped=6 invalid=0
[FINAL] decode frames=79 errors=0 resets=1
```

每秒保留视频、音频 RTP/RTCP 统计，并增加 H.264 重组统计。结束时打印解码汇总。

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

上例 `dropped=6` 来自首次接入及等待 IDR，不能据此认定网络丢包。
协议校验、H.264 重组、FFmpeg 解码各自报告错误；资源故障会进入会话清理流程。

## 无设备验证

CTest 包含六项测试（Python 不存在时只运行前四项）：

| 测试 | 覆盖内容 |
| --- | --- |
| `player_help` | 命令行帮助。 |
| `protocol` | TCP 边界、SDP/URL/Transport/Session、RTP 扩展/填充、RTCP 复合包及统计。 |
| `h264` | 单 NALU、FU-A、多 NALU AU、参数集、容量、序号回绕、缺片、坏负载和 IDR 恢复。 |
| `video_decoder` | 固定合成流的实际像素、B 帧延迟、排空、时间戳回绕、损坏恢复及回调异常。 |
| `session` | 现有 12 个会话场景及命令行错误。 |
| `video_session` | 真实 TCP 会话中的正常视频、FU-A 缺片、非法 RTP、损坏码流和断连排空。 |

合成测试样本及生成方式见 [tests/data/README.md](tests/data/README.md)。测试运行不需要编码器或摄像头。

配套服务端已有 `camera_rtsp_tests.exe --serve`，可提供约 12 秒合成双轨流：

```powershell
python tests/server_integration.py --server E:\MarkDown\rtsp-cpp\build-live-learning\Debug\camera_rtsp_tests.exe --player build/Debug/rtsp_player.exe --log build/p2-server-integration-debug.log
```

脚本验证四秒内连续解出至少 20 帧，尺寸为 160×96、格式为 YUV420P，双轨 RTP/RTCP 正常且 TEARDOWN 成功。
如改用其他合成源，可用 `--expect-width` / `--expect-height` 设置预期尺寸。

2026-11-23 验收：Debug / Release 构建无警告，CTest 各 6/6 通过；配套服务端联调分别解出 79 / 78 帧，解码错误均为 0。

## 代码阅读顺序

| 顺序 | 文件 | 职责 |
| --- | --- | --- |
| 1 | [main.cpp](src/main.cpp) | 参数、退出信号、连接 AU 回调、帧日志和结束排空。 |
| 2 | [rtsp_client.cpp](src/rtsp/rtsp_client.cpp) | 建会话、接收分发、保活、协议统计和退出。 |
| 3 | [h264_depacketizer.cpp](src/rtp/h264_depacketizer.cpp) | 包连续性、FU-A 拼接、AU 完成、丢弃与 IDR 恢复。 |
| 4 | [h264_parameters.cpp](src/rtp/h264_parameters.cpp) | fmtp、Base64、SPS/PPS 的纯解析。 |
| 5 | [video_decoder.cpp](src/media/video_decoder.cpp) | FFmpeg 资源、提交、取帧、重置和排空。 |
| 6 | [packet_stats.cpp](src/rtp/packet_stats.cpp) | RTP/RTCP 校验、payload 位置和按轨统计。 |
| 7 | [rtsp_stream_parser.cpp](src/rtsp/rtsp_stream_parser.cpp)、[sdp.cpp](src/rtsp/sdp.cpp)、[tcp_socket.cpp](src/net/tcp_socket.cpp) | TCP 字节流、控制协议和连接基础。 |

完整设计见 [DESIGN.md](DESIGN.md)，阶段及验收记录见 [task_plan.md](task_plan.md)。
