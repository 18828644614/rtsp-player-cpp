# C++ RTSP 配套播放器

配套服务器：[rtsp-cpp](../rtsp-cpp/README.md)。独立的 Windows / C++17 项目。

**P1 已实现：建立一个 RTSP/TCP 双轨会话，每秒打印视频、音频的 RTP/RTCP 统计。**
播放器只链接 Windows Winsock（`ws2_32`）和 C++ 运行库，不需要 FFmpeg、SDL3。
目前不解码、不显示画面、不播放声音。

## 构建和测试

需要 Visual Studio C++ x64 工具链和 CMake：

~~~powershell
Set-Location E:\MarkDown\rtsp-player-cpp
cmake -S . -B build -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
~~~

默认构建 `rtsp_player` 和纯 C++ 协议测试；找到 Python 3 时，额外运行标准库实现的回环会话测试。
Python 只用于测试。仅构建正式程序可在配置时加 `-DBUILD_TESTING=OFF`。

## 连接你的服务端

先按服务端 README 启动 `rtsp_server.exe --camera 0 --microphone 0`，再执行：

~~~powershell
.\build\Debug\rtsp_player.exe rtsp://127.0.0.1:8554/live --seconds 10
~~~

省略 `--seconds` 会持续接收，按 Ctrl+C 或 Ctrl+Break 停止。
`--timeout-ms 3000` 设置连接和每次控制请求的超时；退出时 TEARDOWN 最多等待 500 毫秒。
`--seconds` 从 PLAY 成功后开始计时，0 表示持续接收；服务端断开或协议失败时返回退出码 1。

P1 的地址只支持数字 IPv4 和 `localhost`，默认端口为 554。只支持配套服务器的
RTSP 1.0 / RTP over TCP、一路 H.264 和一路 MPEG4-GENERIC AAC。

正常控制流程：

~~~text
TCP connect → OPTIONS → DESCRIBE → SETUP video → SETUP audio → PLAY
            → 收包统计 + GET_PARAMETER 保活 → TEARDOWN → close
~~~

## 输出

以下为一次 4 秒合成流联调的最终统计节选；每次运行的数量会变化：

~~~text
[RTSP] PLAY 200
[RTSP] TEARDOWN 200
[FINAL] video rtp=216 bytes=31621 payload=29029 ... rtcp_frames=64 rtcp_packets=129 sr=64 ... bye=1 bad=0 ...
[FINAL] audio rtp=184 bytes=33642 payload=31434 ... rtcp_frames=64 rtcp_packets=129 sr=64 ... bye=1 bad=0 ...
[FINAL] unknown_channels=0
~~~

- `rtp`：有效 RTP 包数；`bytes` 包含 RTP 头，`payload` 不含 RTP 头、扩展和填充；二者都不含 TCP 交织头。
- `ssrc`、`seq`、`ts`：当前轨道 SSRC 和最近 RTP 包的原始序号、时间戳；不把时间戳解释为播放时间。
- `rtcp_frames`：有效 RTCP 交织帧数；`rtcp_packets`：这些帧内的 RTCP 子包总数。
- `sr` / `rr` / `sdes` / `bye`：对应 RTCP 子包数量；`sr_ntp` / `sr_rtp` 保存最新 SR 的时间对应值。
- `bad`：已知通道上被拒绝的数据包，包括长度错误、PT 不匹配、SSRC 改变；`unknown_channels` 单独计数。

统计为本地接收累计值，不等同于服务器发送总数，也不计算丢包率或执行音视频同步。
P1 接收并解析 RTCP，不发送 Receiver Report。

## 无设备联调

配套服务端已有 `camera_rtsp_tests.exe --serve`，提供约 12 秒的合成双轨流。
脚本会自动启动它、读取随机端口 URL、让播放器接收 4 秒，验证两轨 RTP、SR、零非法包和 TEARDOWN。
按你的实际构建目录调整 `--server`：

~~~powershell
python tests/server_integration.py --server E:\MarkDown\rtsp-cpp\build-live-learning\Debug\camera_rtsp_tests.exe --player build/Debug/rtsp_player.exe
~~~

服务端自身的编码依赖沿用它已有的构建环境；播放器没有链接这些依赖。

## 代码阅读顺序

| 顺序 | 文件 | 职责 |
| --- | --- | --- |
| 1 | [main.cpp](src/main.cpp) | 参数、退出信号、错误报告。 |
| 2 | [rtsp_client.cpp](src/rtsp/rtsp_client.cpp) | 顺序建会话、统一接收分发、保活、退出。 |
| 3 | [rtsp_stream_parser.cpp](src/rtsp/rtsp_stream_parser.cpp) | TCP 半包/粘包，拆出 RTSP 消息或交织帧。 |
| 4 | [sdp.cpp](src/rtsp/sdp.cpp) | SDP、控制 URL、Transport 和 Session 字段。 |
| 5 | [packet_stats.cpp](src/rtp/packet_stats.cpp) | RTP/RTCP 校验和按轨统计。 |
| 6 | [tcp_socket.cpp](src/net/tcp_socket.cpp) | Winsock 生命周期、非阻塞连接和有限读写等待。 |

应用逻辑使用单线程；Windows 控制台回调仅设置原子停止标志。主要类只有
`RtspClient`、`RtspStreamParser`、`TcpSocket`，其余使用结构体和普通函数。
RTSP/SDP 纯解析实现参考配套服务器，保存在本项目内独立编译。

完整设计见 [DESIGN.md](DESIGN.md)，阶段和验收记录见 [task_plan.md](task_plan.md)。
