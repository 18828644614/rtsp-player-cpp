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
- [x] P3：用 SDL3 创建窗口，按视频自身时间戳显示，验证关闭和断线收尾。
- [x] P4：实现 AAC-hbr 拆包、AAC 解码和 SDL3 音频输出。
- [x] P5：基于 RTCP SR 对齐两轨，用估算的音频播放位置调度视频，验证端到端播放。

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

P2 验收时状态：单线程、两个新增主要类，独立 FFmpeg 解码目标；后续 P3 进展见下文。

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

## P3 实施进度（2026-11-23）

目标：两个主要类（Player、VideoRenderer），SDL 主线程与一个接收解码线程，用有界帧队列按视频 PTS 显示。

- [x] 接入 SDL3 和 YUV420P 渲染。
- [x] 实现帧引用、有界队列、PTS 调度及工作线程。
- [x] 补齐网络取消、主动关闭和异常收尾。
- [x] 验证调度、SDL 绘制、关闭、断线及原有回归。
- [x] 完成 Debug / Release 与配套服务端联调，更新文档。

P3 验收时状态：窗口播放已完成；音频输出与跨轨同步留到后续阶段。P4 进展见下文。

### P3 验收结果

- MSVC x64 Debug / Release 构建无警告，CTest 各 9/9 通过；clang-format 检查与 git diff --check 通过。
- 保留 P1/P2 六项回归；新增 socket_cancel、video_display、player_session。
- 验证 90 kHz PTS 换算、早到/晚到及跳变；软件渲染检查白色/红色/蓝色像素、黑边、动态尺寸、SAR 和不支持格式。QA 图片在忽略目录 build/p3-render-qa.bmp。
- 回环验证等待 OPTIONS 时关闭、等待保活时关闭、外部停止、真实 H264 播放时关闭、TEARDOWN 超时、SDL 初始化失败、有界队列与服务端断线。
- 发送测试在对端不读、超时设为十分钟的情况下仍能主动取消；连接/发送的等待均通过同一分片等待检查停止。
- 配套 --serve 四秒实际窗口联调：Debug 解码/显示 75/75 帧，Release 76/76 帧；均 160×96 / YUV420P、display dropped=0、queue_peak=1、clock_resets=0、decode errors=0、H264 invalid=0。
- 两次最终联调均两轨 bad=0、unknown_channels=0、TEARDOWN=200；日志为 build/p3-server-integration-debug.log 和 build/p3-server-integration-release.log。
- Release dumpbin 直接依赖 avcodec-63.dll、avutil-61.dll、SDL3.dll、Winsock 和系统/MSVC 运行库；未引入 avformat、swscale 或音频解码。
- SDK 路径仅保存在本机 CMake 缓存；SDL3 3.4.20 VC 包保存在忽略的 build/deps，未修改配套服务端源码。实体摄像头/麦克风不属于本次无设备验收。

## P4 实施进度（2026-11-24）

目标：新增 AacDepacketizer、AudioDecoder、AudioOutput 三个主要类，沿用主线程和一个接收解码工作线程，完成配套服务端 AAC 音频播放。

- [x] 实现 AAC-hbr 参数校验、单 AU 拆包、序号检查及 RTP 时间戳扩展。
- [x] 实现 AudioSpecificConfig 初始化、FFmpeg AAC 解码、损坏 AU 恢复及排空。
- [x] 实现 SDL3 F32 单声道音频输出、300 ms 输入缓冲上限及有限排空。
- [x] 接入双轨回调，验证关闭、断线、设备失败和资源释放。
- [x] 完成 Debug / Release 回归、配套服务端联调及文档更新。

P4 验收时状态：音频输出已实现，视频仍使用自身 PTS；后续 P5 进展见下文。

### P4 验收结果

- MSVC x64 Debug / Release 构建成功，无编译警告；CTest 各 13/13 通过。最后整理帮助文字和测试代码后，Debug 相关三项亦重新通过。clang-format 检查与 git diff --check 通过。
- 保留原有九项回归；新增 aac、audio_decoder、audio_output、audio_session。协议测试不依赖 FFmpeg/SDL；自动播放测试使用 SDL dummy 音频驱动。
- 覆盖 ASC 和 13/3/3 位 AU 参数、空包/截断/声明长度不符、多 AU/分片拒绝、8191 字节上限、重复/旧包、丢包恢复及序号/时间戳回绕。
- 固定合成 AAC 样本解码为 13 帧、13312 个样本，验证非静音浮点 PCM、采样率/声道、PTS、排空、损坏 AU 后恢复以及回调异常传播。
- SDL 测试验证实际设备流提交、输入缓冲不超过 300 ms、突发积压清空、有限排空、停止取消、不支持的 PCM 格式、设备打开失败及子系统释放。双轨回环还验证实际音频播放期间关闭窗口/外部停止、断线和 SDP 配置不兼容。
- 配套 --serve 四秒实际窗口和默认音频设备联调：Debug 视频解码/显示 79/79 帧，AAC 解码 186 帧、190464 个样本，音频输入峰值 80 ms；Release 视频 76/76 帧，AAC 180 帧、184320 个样本，输入峰值 65 ms。两次均音频 decode errors=0、AAC invalid/lost/dropped=0、audio overflows=0。
- 两次联调均两轨 RTP bad=0、unknown_channels=0、TEARDOWN=200，视频显示无丢帧。日志保存在忽略目录 build/p4-server-integration-debug.log 和 build/p4-server-integration-release.log。
- Release dumpbin 直接依赖仍为 avcodec-63.dll、avutil-61.dll、SDL3.dll、Winsock 和系统/MSVC 运行库，没有新增 swresample、swscale 或 avformat。未修改配套服务端源码。
- 本次验证合成音频的解码和实际设备流输出；未进行人工听音、实体摄像头/麦克风采集或音视频同步验收。

## P5 实施进度（2026-11-24）

目标：保持主线程与一个接收解码线程，用两轨 SR 和估算音频位置调度视频，只增加小型同步时钟结构体。

- [x] 统一视频 RTP PTS 语义，接入有效 SR 回调和公共时间映射。
- [x] 扩展 AudioOutput 播放位置、缓冲不连续及线程保护。
- [x] 接入 Player 等待 SR、音频主时钟和有限收尾。
- [x] 更新回环媒体时间，补齐同步边界测试并运行 Debug / Release 回归。
- [x] 完成配套服务端联调，更新设计、使用说明和验收记录。

当前状态：P5 代码与自动化验收已完成。真实窗口/音频设备端到端播放已联调，物理声画误差尚未进行人工拍手或采集回录测量。

### P5 验收结果

- MSVC x64 Debug / Release 构建成功，无编译警告；CTest 各 15/15 通过，最终完整测试分别耗时约 49 / 48 秒。clang-format --dry-run --Werror 与 git diff --check 通过。
- 保留 P4 各项协议、拆包、解码、渲染、取消和故障恢复回归；新增 sync_clock、sync_session。媒体回环按 PTS 时长发送并预送 B 帧，不再以等包数代替等媒体时长。
- 确定性测试验证两轨首样本相差 120 ms、两轨 SR 不同时刻/不同先后顺序、RTP 回绕、后续 SR 更新、重复/过期 SR、映射跳变和视频等帧/丢帧阈值。
- 实际 TCP 双轨测试验证 120 ms 偏移、延迟 SR、缺一轨 SR 约 3 秒报错、SR 跳变后重新同步、坏复合 RTCP 不提交、音频先结束的有限收尾，以及无 SR 的 RTCP 不重放回调、SR 回调异常正确传播。
- AudioOutput 测试验证带 PTS 的 PCM 输出、300 ms 上限、位置处于已提交样本范围、排空保留终点、空队列不外推、PTS 缺口重建、clear 使位置失效，以及设备失败和主动取消。
- 配套 camera_rtsp_tests.exe --serve 与真实 SDL 窗口/默认音频设备四秒联调：Debug 解码/显示 76/76 帧，AAC 181 帧，音频输入峰值 78 ms，最大呈现调度差值 9814 us；Release 解码/显示 77/77 帧，AAC 180 帧，峰值 76 ms，差值 9958 us。
- 两次联调均 SR 映射建立、sync resets=0、音频 overflows/discontinuities/underruns=0，两轨 bad=0、AAC invalid/lost/dropped=0、解码 errors=0、unknown_channels=0、TEARDOWN=200。各丢弃 1024 个等待首组 SR 的音频样本，日志独立统计。
- 成功日志位于忽略目录 build/p5-server-integration-debug-retry.log、build/p5-server-integration-release.log；首次提前结束、未满足样本数断言的 Debug 日志保留于 build/p5-server-integration-debug.log，未降低验收下限。
- Release dumpbin 直接依赖仍为 avcodec-63.dll、avutil-61.dll、SDL3.dll、Winsock 和系统/MSVC 运行库；没有新增 avformat、swresample 或 swscale。未修改配套服务端。
- 差值反映播放器调度与 SDL 输入消费位置的关系，未补偿设备缓冲；未进行实体摄像头/麦克风拍手或采集回录，不能将约 10 ms 调度差值解释为物理声画误差。
