# RTSP 播放器实施计划

## 本次交付：框架（2026-11-20）

- [x] 核对现有服务端的协议、轨道和测试入口。
- [x] 创建独立项目目录，不修改服务端。
- [x] 写框架设计、阅读入口和可构建骨架。
- [x] 验证 CMake 构建与命令行帮助。

验收结果：MSVC x64 Debug 构建成功，CTest 1/1 通过；rtsp_player.exe --help 正常返回。未安装 FFmpeg 或 SDL3，未改动同级服务端项目。

## 后续实施

- [x] P1：实现一个 RTSP/TCP 会话，打印收到的两轨 RTP/RTCP 统计。不引入 FFmpeg、SDL3。
- [x] P2：实现 H.264 拆包与访问单元重组，用 FFmpeg 在终端连续解帧。
- [ ] P3：用 SDL3 创建窗口，按视频自身时间戳显示，验证关闭和断线收尾。
- [ ] P4：实现 AAC-hbr 拆包、AAC 解码和 SDL3 音频输出。
- [ ] P5：基于 RTCP SR 对齐两轨，用估算的音频播放位置调度视频，验证端到端播放。

P1 阶段仅实现连接和协议统计；P2 在此基础上增加 FFmpeg 视频解码。

## P1 实施进度（2026-11-20）

- [x] 核对配套服务端的会话、解析器和合成双轨测试入口。
- [x] 实现 TCP、RTSP/SDP 解析、双轨会话和 RTP/RTCP 统计。
- [x] 验证协议边界、保活、超时、断连和 TEARDOWN。
- [x] 与配套服务端联调，更新使用文档和验收结果。

当前状态：P1 已完成。保持单线程、一个连接，纯解析代码参考配套服务端并复制为项目内独立源码。

### P1 验收结果

- MSVC x64 Debug / Release 构建成功，无编译警告；两种配置 CTest 均 3/3 通过。
- protocol：覆盖 TCP 全切分点、逐字节交织头、混合输入、截断、重复头、长度上限、SDP/URL/Session/Transport、RTP 扩展及填充、序号回绕、复合 RTCP 及统计提交。
- session：12 个回环场景，包括实际通道改分配、SDP 音频在前、保活时媒体持续到达、错误 CSeq/Session、通道重叠、UDP 拒绝、解析后重复 URL、断连、截断、保活错误/超时、TEARDOWN 超时；另覆盖命令行错误。
- 配套 `build-live-learning/Debug/camera_rtsp_tests.exe --serve`，Debug 播放器接收 4 秒：video RTP=216、audio RTP=184，两轨各 SR=64、BYE=1、bad=0，unknown_channels=0，TEARDOWN=200。
- 同一服务端与 Release 播放器联调 4 秒：video RTP=211、audio RTP=183，两轨各 SR=64、BYE=1、bad=0，unknown_channels=0，TEARDOWN=200。
- Release `dumpbin /dependents`：仅 Winsock、Windows 系统库、MSVC/UCRT；无 FFmpeg、SDL3。
- 联调日志保存在忽略的构建目录 `build/p1-server-integration.log` 和 `build/p1-server-integration-release.log`。
- 未修改配套服务器。P1 验收时 P2–P5 尚未实施；实体设备画面/声音不属于 P1 验收。

## P2 实施进度（2026-11-23）

目标：沿用单线程接收链路，用两个主要类完成 H.264 重组和 FFmpeg 解码；按职责分文件、分短函数，不引入窗口、音频解码或线程队列。

- [x] 扩展 RTP 解析结果并实现 H.264 单 NALU / FU-A / AU 重组。
- [x] 实现 SPS/PPS、容量限制、丢包后等待完整 IDR 恢复。
- [x] 封装 FFmpeg 解码，接入 RTSP AU 回调和控制台输出。
- [x] 验证协议回归、重组边界、实际解码和配套服务端连续解帧。
- [x] 更新设计、构建说明及最终验收结果。

当前状态：P2 已完成。单线程、两个新增主要类，独立 FFmpeg 解码目标；P3–P5 尚未实施。

### P2 验收结果

- MSVC x64 Debug / Release 构建成功，无编译警告；两种配置 CTest 均 6/6 通过。
- 保留原有协议和 12 个会话场景；新增纯 H.264 重组、实际 FFmpeg 解码、五个完整 TCP 视频场景。
- 覆盖单 NALU、多 NALU AU、FU-A、参数集、容量限制、序号和时间戳回绕、缺片、坏包、码流损坏和完整 IDR 恢复。
- 用含 B 帧的固定合成流验证延迟帧排空和时间戳；回调异常正常向外传播。
- 配套 --serve 四秒联调：Debug 解出 79 帧、Release 解出 78 帧，均为 160×96 / YUV420P、decode errors=0、H264 invalid=0。
- 两次实际联调均两轨 bad=0、unknown_channels=0，TEARDOWN=200；初始等待 IDR 丢弃 6 个 AU。
- Release 直接依赖 avcodec-63.dll、avutil-61.dll、Winsock 和系统/MSVC 运行库。
- 联调日志保存在 build/p2-server-integration-debug.log 和 build/p2-server-integration-release.log。
- 复用已有 FFmpeg SDK，本机缓存保存路径；未修改配套服务器源码。
