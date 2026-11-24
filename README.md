# C++ RTSP 配套播放器

配套服务器：[rtsp-cpp](../rtsp-cpp/README.md)。独立的 Windows / C++17 项目。

**P5 已实现：使用 RTCP SR 对齐 H.264 / AAC 两轨，以估算音频播放位置调度视频。**
主线程处理窗口事件和画面，一个工作线程负责网络、重组和解码。媒体解码链接 `avcodec`、`avutil`，TCP 使用 Winsock。
音频通过 SDL3 设备流输出，输入缓冲限制为 300 ms；视频提前等待，落后过多则丢弃。仍只有两个应用线程，不增加 PCM 队列或重采样依赖。

## 构建和测试

需要 Visual Studio C++ x64 工具链、CMake、SDL3 3.2+ 的 VC x64 开发包，以及包含 H.264 / AAC 解码器的 FFmpeg x64 shared SDK：

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
主动关闭或正常定时结束的退出码为 0；定时结束时仍缺少任意一轨 SR，则报告同步失败。服务端断开、控制协议失败、SDL 或解码器资源/API 错误时退出码为 1；码流损坏则丢弃并等待完整 IDR 恢复。

支持数字 IPv4、`localhost`、RTSP 1.0 / RTP over TCP，以及配套服务器的一路 H.264 和一路 MPEG4-GENERIC AAC。
默认端口 554；SETUP 返回的通道、SDP 的 payload type 均在运行时读取。

## 处理流程

```text
TCP → RTSP/交织帧拆分 → RtspClient
                          ├─ 视频 RTP → 校验和统计 → H264Depacketizer
                          │                              ↓ 完整 AU 回调
                          │                         VideoDecoder → 有界帧队列 → SDL3（主线程）
                          ├─ 音频 RTP → 校验和统计 → AacDepacketizer
                          │                              ↓ 完整 AU 回调
                          │                         AudioDecoder → AudioOutput → SDL3 音频设备
                          └─ RTCP SR  → 校验和统计 → SyncClock（公共时间轴）
```

`main` 只处理参数、控制台停止和顶层错误。`Player` 连接工作线程与 SDL 主线程；`VideoRenderer` 管理窗口和绘制，`AudioOutput` 管理音频设备流。
`src/rtsp/`、`src/rtp/` 不依赖 FFmpeg 或 SDL。控制请求等待和正常接收共用分发入口；等待保活响应时媒体仍继续处理。

解码回调用 `av_frame_clone` 保留引用后入队；队列最多 12 帧且 PTS 跨度不超过 300 ms。超限丢弃最旧的已解码帧，不阻塞收包。
视频和音频都保留 RTP 起点并扩展时间戳，分别用 90 kHz / 48 kHz 的 SR 对应点转换到相同的 NTP 相对时间轴。
AudioOutput 用连续 PCM 段的末尾 PTS 减去 SDL 输入队列剩余样本，估算音频当前位置；空队列不继续随本机时间前进。
视频超前超过 10 ms 时等待，落后超过 80 ms 时丢弃；一次循环只呈现最新的合适帧。缺失或不递增的 PTS 丢弃，主线程等待事件最多 10 ms。

支持 YUV420P，按帧的 601/709 矩阵与有限/全范围上传；未指定矩阵时按 601。尺寸变化重建纹理，缩放保持显示宽高比（含像素宽高比），空白处留黑边。
其他像素格式明确报错，暂不引入 swscale。

退出时先停止新 AU、丢弃未完成视频 AU，有限尝试 TEARDOWN，再排空两个解码器。定时结束时音频排空最多等待 500 ms，保留最后估算位置，剩余超出音频终点的视频丢弃；主动关闭或断线清空剩余音频并丢弃待显示帧。
主线程初始化音频子系统；工作线程结束后先销毁音频流，再由 VideoRenderer 销毁 SDL。连接和发送等待每 50 ms 检查取消；TEARDOWN 有独立的最多 500 ms 清理期限。

## 同步策略与边界

- 从 PLAY 成功起等待两轨有效 SR，最多 3 秒；定时结束时仍缺 SR 也报错，不退回各轨首包归零。
- 等待期间视频队列仍为 12 帧 / 300 ms；音频 PCM 丢弃并计数，映射齐备后从后续 PCM 开始输出。
- 公共时间为 `SR_NTP 相对公共原点 + RTP 与 SR 的有符号回绕差 / 时钟频率`；不使用网络到达时间推测轨道偏移。SR 应保持常规周期，RTP 相对差按半个 32 位周期内解释。
- 重复或较旧 SR 忽略；新 SR 与上一映射相差超过 100 ms 时清空旧缓冲，等待另一轨新 SR 后恢复。首版不做时钟漂移拟合或音频变速。
- 音频 PTS 缺口和超过 300 ms 的输入积压都会清空旧段并重新估算；音频断供时视频等待，恢复后再比较公共时间。
- 估算反映 SDL 输入数据被消费的位置，未补偿转换器及硬件缓冲。日志里的呈现差值是调度时的视频时间与估算音频时间之差，不能作为物理声画误差测量。
- 当前仍固定为一个会话的一路 H.264、一路 48 kHz 单声道 AAC-LC；SSRC 改变按原协议校验拒绝。

## AAC 支持范围

- 配套服务端的 48 kHz、单声道 AAC-LC，每个 RTP 包一个完整 AU，最多 8191 字节。
- 校验 SDP 的 AAC-hbr、SizeLength=13、IndexLength=3、IndexDeltaLength=3；config 解码为 AudioSpecificConfig，核对采样率、声道及 1024 样本配置。参数名忽略大小写；未知或重复参数明确报错。
- 去除 RTP 与 AU 头后提交裸 AAC，不添加 ADTS。重复/旧包丢弃，序号缺口计数；时间戳扩展支持 32 位回绕，损坏 AU 丢弃后继续处理。
- FFmpeg 输出的单声道 FLTP/FLT 直接作为 F32 PCM 提交 SDL，由 SDL 适配设备格式；不新增 swresample 依赖。
- SDL 输入缓冲最多 300 ms，超限清空积压再提交最新帧，不阻塞收包；设备端仍有少量缓冲，不把输入缓冲当作精确播放时钟。
- 日志新增 aac、audio_decode、audio_output 统计；记录解码样本数、错误数、输入队列峰值及溢出次数。

## H.264 支持范围

- 支持单 NALU、FU-A，以及同一 AU 内的多个 NALU；不支持 STAP-A、交织模式或包重排。
- SDP `packetization-mode` 支持 0 和 1，未指定时按 0；模式 0 拒绝 FU-A。
- SDP `sprop-parameter-sets` 支持一组 SPS/PPS，也支持流内完整 SPS/PPS；参数集变更后等待 IDR。
- 按 RTP 时间戳、序号和 marker 收齐 AU。FU-A 的结束位只表示 NALU 结束。
- 首次接入的时间戳可能从 AU 中间开始，因此首个 AU 不提交；参数集仍可从完整 NALU 中保存。
- 序号断裂、分片缺失、时间戳提前变化、非法负载或容量超限时丢弃损坏 AU，等待完整 IDR。
- AU 默认上限 8 MiB，单个 SPS/PPS 上限 64 KiB；IDR 前补入参数集时也检查总容量。
- FFmpeg packet 使用带尾部 padding 的独立缓冲；每次提交后循环取帧，退出时排空延迟帧。

视频 PTS 使用 1/90000 秒为单位，处理 RTP 回绕和 B 帧重排。
P5 保留首次实际输入 AU 的原始 RTP 起点，后续继续扩展；解码恢复不重新归零。SyncClock 使用 SR 将视频和音频映射到公共时间轴。

## 输出与统计

以下节选自 Debug 播放器与合成服务端的四秒联调：

```text
[SYNC] ready video_rate=90000 audio_rate=48000
[VIDEO] frame=1 pts=25462 width=160 height=96 format=yuv420p
...
[RTSP] TEARDOWN 200
[FINAL] decode frames=76 errors=0 resets=1
[FINAL] audio_decode frames=181 samples=185344 errors=0
[FINAL] audio_output samples=184320 queue_peak_ms=78 overflows=0 discontinuities=0 underruns=0
[FINAL] display frames=76 dropped=0 queue_peak=2 clock_resets=0
[FINAL] sync ready=1 resets=0 late_dropped=0 tail_dropped=0 max_present_error_us=9814 waiting_audio_samples=1024
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
| 显示 `queue_peak` / `clock_resets` | 队列帧数峰值，以及 SR 映射跳变后的重新同步次数。 |
| 同步 `ready` / `resets` | 会话是否建立过双轨映射，以及 SR 跳变后重新缓冲次数。 |
| 同步 `late_dropped` / `tail_dropped` | 超出迟到阈值的丢帧数，以及正常结束时超出音频终点的丢帧数。 |
| 同步 `max_present_error_us` | 呈现调度时视频与估算音频位置的最大绝对差；不代表硬件声画误差。 |
| 同步 `waiting_audio_samples` | 等待 SR 期间解码后未输出的音频样本数。 |
| 音频 `discontinuities` / `underruns` | PCM 时间戳不连续次数，以及下一段连续 PCM 到达前输入队列已空的次数。 |

H.264 `dropped` 包括首次接入和等待 IDR 的丢弃，不能据此认定网络丢包；显示 `dropped` 另统计未呈现的解码帧。
协议校验、H.264 重组、FFmpeg 解码各自报告错误；资源故障会进入会话清理流程。

## 无设备验证

CTest 包含 15 项测试（Python 不存在时运行 10 项 C++ 测试）：

| 测试 | 覆盖内容 |
| --- | --- |
| `player_help` | 命令行帮助。 |
| `protocol` | TCP 边界、SDP/URL/Transport/Session、RTP 扩展/填充、RTCP 复合包及统计。 |
| `socket_cancel` | 对端不读时持续发送，验证大超时下仍可主动取消。 |
| `h264` | 单 NALU、FU-A、多 NALU AU、参数集、容量、序号回绕、缺片、坏负载和 IDR 恢复。 |
| `aac` | AAC-hbr AU 头、ASC、序号与时间戳回绕、坏包恢复。 |
| `audio_decoder` | AAC 真实解码、PTS、损坏恢复、排空及回调异常。 |
| `audio_output` | PCM 输出、输入上限、位置估算、欠载冻结、PTS 缺口、清空和取消。 |
| `sync_clock` | 两轨 120 ms 偏移、不同 SR 时刻、RTP 回绕、重复/过期 SR、跳变及视频调度。 |
| `video_decoder` | 固定合成流的实际像素、B 帧延迟、排空、时间戳回绕、损坏恢复及回调异常。 |
| `video_display` | YUV 白色及红蓝像素、黑边、尺寸/SAR 变化和关闭事件。 |
| `session` | 现有 12 个会话场景及命令行错误。 |
| `video_session` | 真实 TCP 会话中的正常视频、FU-A 缺片、非法 RTP、损坏码流和断连排空。 |
| `player_session` | 等待控制响应/保活时关闭、外部停止、播放中关闭、TEARDOWN 超时、SDL 初始化失败、队列丢帧和断线。 |

| `audio_session` | 双轨播放、AAC 坏包/丢包/损坏恢复、突发溢出、关闭及设备初始化失败。 |
| `sync_session` | 120 ms 偏移、延迟/缺失 SR、重新同步、复合 RTCP 校验、音频提前结束及 SR 回调异常。 |

涉及 SDL 的自动测试使用 dummy 视频驱动和软件渲染，不弹窗口；`player_test_driver` 仅为测试注入关闭事件，正式程序没有测试专用参数。

合成测试样本及生成方式见 [tests/data/README.md](tests/data/README.md)。测试运行不需要编码器或摄像头。

配套服务端已有 `camera_rtsp_tests.exe --serve`，可提供约 12 秒合成双轨流：

```powershell
python tests/server_integration.py --server E:\MarkDown\rtsp-cpp\build-live-learning\Debug\camera_rtsp_tests.exe --player build/Debug/rtsp_player.exe --log build/p5-server-integration-debug.log
```

脚本打开实际 SDL 窗口和音频设备，验证四秒内连续解码并显示至少 20 帧，尺寸为 160×96、格式为 YUV420P；同时验证至少 100 个 AAC 帧、样本数、零解码错误及音频缓冲上限，两轨 SR 映射建立、正常源无重新同步、调度差值处于策略范围，双轨 RTP/RTCP 正常且 TEARDOWN 成功。自动测试使用 dummy 音频驱动，不代替人工听音。
如改用其他合成源，可用 `--expect-width` / `--expect-height` 设置预期尺寸。

P5 验收结果见 [task_plan.md](task_plan.md)，渲染测试同时在构建目录输出 `p3-render-qa.bmp`。

## 代码阅读顺序

| 顺序 | 文件 | 职责 |
| --- | --- | --- |
| 1 | [main.cpp](src/main.cpp) | 参数、退出信号和顶层错误报告。 |
| 2 | [player.cpp](src/player/player.cpp)、[sync_clock.h](src/player/sync_clock.h) | 工作线程、帧队列、SR 映射、音频主时钟调度和退出编排。 |
| 3 | [video_renderer.cpp](src/player/video_renderer.cpp) | SDL 资源、事件、YUV 上传和等比绘制。 |
| 4 | [rtsp_client.cpp](src/rtsp/rtsp_client.cpp) | 建会话、接收分发、保活、协议统计和退出。 |
| 5 | [h264_depacketizer.cpp](src/rtp/h264_depacketizer.cpp) | 包连续性、FU-A 拼接、AU 完成、丢弃与 IDR 恢复。 |
| 6 | [h264_parameters.cpp](src/rtp/h264_parameters.cpp) | fmtp、Base64、SPS/PPS 的纯解析。 |
| 7 | [video_decoder.cpp](src/media/video_decoder.cpp) | FFmpeg 资源、提交、取帧、重置和排空。 |
| 8 | [packet_stats.cpp](src/rtp/packet_stats.cpp) | RTP/RTCP 校验、payload 位置和按轨统计。 |
| 9 | [rtsp_stream_parser.cpp](src/rtsp/rtsp_stream_parser.cpp)、[sdp.cpp](src/rtsp/sdp.cpp)、[tcp_socket.cpp](src/net/tcp_socket.cpp) | TCP 字节流、控制协议和连接基础。 |
| 10 | [audio_output.cpp](src/player/audio_output.cpp)、[audio_decoder.cpp](src/media/audio_decoder.cpp)、[aac_depacketizer.cpp](src/rtp/aac_depacketizer.cpp) | 音频拆包、解码、输出和位置估算。 |

完整设计见 [DESIGN.md](DESIGN.md)，阶段及验收记录见 [task_plan.md](task_plan.md)。
