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

目录按阶段引入，不先创建一排空类。P3 已实现 src/net/、src/rtsp/、src/rtp/、视频解码所需的 src/media/ 和视频播放所需的 src/player/。

| 位置 | 职责 |
| --- | --- |
| src/main.cpp | 读取一个 RTSP URL，启动播放器，报告错误。 |
| src/net/ | 一个 TCP 连接，有限读写等待，支持唤醒接收并退出。 |
| src/rtsp/ | RTSP 文本和交织帧解析、SDP、控制 URL 和客户端会话。不能依赖 FFmpeg/SDL3。 |
| src/rtp/ | 读取 RTP/RTCP，按 SETUP 返回的通道分发；将 RTP payload 拆成 H.264/AAC AU。不能解码或绘制。 |
| src/media/ | 两个 FFmpeg 解码器，保留帧的 RTP PTS；当前格式无需新增重采样依赖。 |
| src/player/ | Player 串起处理流程，SyncClock 负责 SR 映射；SDL3 主线程处理事件和视频，AudioOutput 输出音频并估算位置。 |
| tests/ | 纯 C++ 协议测试、回环假服务器，以及对配套服务器的集成验收。 |

正式构建只生成 rtsp_player。P1 加 ws2_32；P2 只加 FFmpeg avcodec/avutil；画面格式转换需要时再加 swscale；播放阶段引入 SDL3，需要重采样时加 swresample。各阶段只加实际需要的依赖。包路径由本机构建配置指定，不提交机器专属路径。

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

## P2 实际落地（2026-11-23）

### 模块边界

- 新增两个主要类：`H264Depacketizer`、`VideoDecoder`。沿用单线程，同步调用，不增加队列。
- `RtspClient` 持有重组器；`main` 创建解码器并设置完整 AU 回调。回调返回 false 时，重组器等待下一完整 IDR。
- `src/rtp/h264_parameters.h/.cpp` 用普通结构体和函数处理 fmtp 与 Base64，避免把参数解析挤进重组类。
- `src/rtsp/`、`src/rtp/` 不包含或链接 FFmpeg；`player_media` 单独链接 avcodec/avutil。
- `RtpHeader` 增加 payload_offset，`count_rtp()` 返回已经验证的头；重组器直接使用去除扩展、CSRC、填充后的负载。

### 重组与恢复

重组器支持单 NALU 和 FU-A。相同时间戳内累积 NALU，marker 且 FU-A 已结束时才提交 AU。
首次接入的 AU 边界未知，保守丢弃；完整参数集仍可保留。SDP 支持 mode 0/1 和一组 SPS/PPS，也接收流内参数集。
序号断裂、FU-A 不完整、旧 AU 未结束便切换时间戳，或容量超限时丢弃损坏 AU，等待参数集齐全的完整 IDR。
有序 TCP 不需要重排缓存；序号缺口仍用于检测服务端未发出的包或测试注入的缺片。

AU 使用带起始码的 Annex B 字节，携带原始 RTP 时间戳、is_idr、reset_decoder。
IDR 前补 SPS/PPS；AU 上限 8 MiB，单个参数集上限 64 KiB。

### 解码与退出

解码器拥有 AVCodecContext、AVPacket、AVFrame，使用独立且带 padding 的 packet 缓冲。
使用 send/receive API，每次提交后循环取帧；FFmpeg 解码线程数设为 1。
帧回调只借用当前 AVFrame，调用完成后 unref；后续显示阶段需要保留帧时须另外引用。
32 位 RTP 时间戳展开为 64 位视频 PTS，单位 1/90000，支持回绕及 B 帧的时间戳回退；不建立跨轨同步时间轴。

RTP/RTCP 错误增加轨道 bad；H.264 格式错误增加 invalid；边界不明、损坏和等待 IDR 的 AU 增加 dropped。
FFmpeg INVALIDDATA 或损坏帧触发重置和等待 IDR；资源/API 异常向外传播，不会被当作协议坏包吞掉。
结束时先停用 AU 回调并丢弃未完成 AU，再尽量 TEARDOWN，最后排空已提交的延迟帧。
断连或其他异常退出也尝试排空，同时保留原始会话错误。

### 验证

- Debug / Release 构建无警告，CTest 各 6/6 通过。
- 重组测试覆盖多 NALU、FU-A 缺片/错误头、序号回绕、参数集变更、容量、缺 marker、外部丢弃和 IDR 恢复。
- 固定 64×48 合成码流包含 B 帧，验证实际解码、重排、延迟帧排空、RTP 时间戳回绕和解码状态重置。
- TCP 视频会话覆盖正常链路、缺片、非法 RTP、完整但损坏的码流、断连排空，均验证后续 IDR 可恢复。
- 配套服务器 --serve 四秒联调：Debug 79 帧，Release 78 帧，均为 160×96 / YUV420P，解码错误为 0，TEARDOWN=200。
- Release 直接依赖 avcodec-63.dll、avutil-61.dll、Winsock 和 Windows/MSVC 运行库。

## P3 实际落地（2026-11-23）

- 两个主要类：Player 编排播放、VideoRenderer 管理 SDL；VideoClock 是可传入当前时间的小结构体。没有通用队列或插件框架。
- 主线程拥有 SDL 窗口、renderer、texture，事件等待不超过 10 ms；一个工作线程顺序接收、拆包和解码，不在解码回调中等待显示。
- Player 内部 deque 保存 av_frame_clone 的独立引用，智能指针自动释放。mutex 保护队列及工作线程结束/错误状态；停止标记使用 atomic，所有线程结束后再销毁 SDL。
- 队列最多 12 帧、跨度最多 300 ms；超限丢弃最旧已解码帧，消费者跳过积压的旧到期帧，保留最新到期帧。不阻塞网络及保活。
- 视频相对时间轴以第一帧准备呈现时的 PTS 与 steady_clock 建立基准，目标时间按 1/90000 换算；无有效/不递增 PTS 的帧丢弃，超过 2 秒的跳变重新锚定。VideoClock 的仿真时间测试不依赖 sleep。
- YUV420P 使用 SDL IYUV 纹理，显式选择 601/709 与有限/全范围；未指定矩阵默认 601。尺寸或颜色参数改变重建纹理，logical presentation 保持含 SAR 的显示比例。
- 主动关闭时停止呈现，工作线程仍完成会话清理和解码排空，但不再入队。定时结束显示完有界队列；断线保留原始异常，丢弃剩余显示帧并返回失败。
- TcpSocket 的 connect/send 等待每 50 ms 检查取消，取消异常接入 RtspClient 正常停止分支。TEARDOWN 忽略主动取消，仍最多等待 500 ms。
- player_display 使用 STATIC 收纳已有协议/解码 OBJECT 目标，并链接 SDL3；协议测试仍不依赖 FFmpeg/SDL。SDL 路径使用 SDL3_DIR，本机 SDK 保存在忽略的构建目录。
- 自动测试使用 SDL dummy/software 驱动，检查实际像素与黑边；回环测试注入窗口关闭及外部停止，并保留 P1/P2 协议、解码和故障恢复回归。配套 --serve 联调使用实际窗口。音频输出与跨轨同步留到 P4/P5。

## P4 实际落地（2026-11-24）

- 新增三个主要类：AacDepacketizer 处理协议、AudioDecoder 负责 FFmpeg 解码、AudioOutput 管理 SDL 音频流；没有通用媒体管线或额外应用线程。
- AacParameters 保存二进制 ASC 和音频格式，AacAccessUnit 保存裸 AAC 与扩展后的 RTP PTS。配置在 DESCRIBE 后同步回调给解码器，AU 在收包线程同步回调，RtspClient 仍不依赖 FFmpeg/SDL。
- 首版仅支持配套服务端的 48 kHz 单声道 AAC-LC、1024 样本、每包一个完整 AU；校验 AAC-hbr、13/3/3 位 AU 头参数、ASC 与 rtpmap 一致性。多 AU、分片、非零 AU-index、未知打包参数明确拒绝。
- AU 大小为 1..8191 字节，必须与实际 payload 匹配；不拼接 ADTS。重复/旧包丢弃，序号缺口计数，坏包不阻止后续完整 AU；时间戳在协议层扩展为 64 位。
- AudioDecoder 持有 AVCodecContext/AVPacket/AVFrame，ASC 和输入包分别分配 FFmpeg 要求的零填充尾部；按 send/receive API 解码、排空，损坏码流计数并 flush 后继续。帧仅在同步回调期间有效，资源或回调异常向外传播。
- 单声道 FLTP 的唯一平面直接作为 F32 PCM；SDL_PutAudioStreamData 返回前复制数据，回调结束即可释放帧。设备格式转换交给 SDL，不添加 swresample，不增加应用层 PCM 队列。
- AudioOutput 在主线程初始化音频子系统，首个解码帧到达时由工作线程打开并启动设备流；推送、查询、清空和排空均由工作线程独占。主线程 join 后销毁音频流，VideoRenderer 最后执行 SDL_Quit。
- SDL 输入缓冲上限为 300 ms，提交前超限则清空积压并计数，避免拖住共用的收包线程。这里只限制流输入积压；设备端另有少量缓冲，P4 不据此计算同步时钟。
- 正常定时结束排空两个解码器，并对音频流 Flush 后有限等待输入/输出数据被消费，最多 500 ms，每 5 ms 检查主动停止。主动关闭或断线清空音频；任何清理异常保留原始失败。
- 保留视频时钟和帧队列；P5 再引入 SR 对齐及估算音频播放时钟。

## P5 实际落地（2026-11-24）

- 保持 SDL 主线程与一个接收解码线程，新增一个头文件结构体 SyncClock，替换 VideoClock。未增加应用线程、PCM 队列、通用管线或新依赖。
- VideoDecoder 不再将首个实际送入的 AU 时间戳归零，视频和 AAC PTS 都保留原始 RTP 起点并继续扩展；解码恢复保留时间戳状态。
- RtspClient 增加 PLAY 成功和 Sender Report 同步通知。仅在当前复合 RTCP 完整校验成功且含 SR 时通知；回调异常向会话清理流程传播。finish_media 先停用播放通知，TEARDOWN 期间继续协议统计。
- SyncClock 按轨道保存 SR 和频率，以首个 NTP 为公共原点。NTP 先求相对差再转整数微秒；RTP 取有符号 32 位回绕差，可映射 SR 前后的媒体帧以及不同扩展周期表示的 PTS。依赖周期性 SR，单个 RTP 相对差需在半个回绕周期以内。
- 较旧/重复 SR 不更新映射。新 SR 与上一映射偏差超过 100 ms 时清空两轨映射及旧音视频缓冲，保留当前 SR，等待另一轨新 SR。首版不拟合漂移或调整音频速率。
- 从 PLAY 成功起等待两轨 SR 最多 3 秒；会话提前正常结束时仍缺 SR 也报告同步失败。视频预缓冲维持 12 帧 / 300 ms，音频 PCM 在映射齐备前丢弃并统计，从后续连续音频开始播放。
- AudioOutput 记录连续 PCM 段末尾 PTS，减去 SDL_GetAudioStreamQueued 对应的输入样本，返回估算 RTP 位置。队列空后停在 PCM 末尾；不随本机时间外推，不将缺音算作正常播放。
- 音频 PTS 缺口或 300 ms 输入溢出时清空旧段，建立新位置；不跨缺口反推样本进度。查询、提交、清空共用 AudioOutput 内部锁，锁顺序固定为 Player → AudioOutput，音频锁内不调用外部回调。SDL 后台消费以块进行，位置呈阶梯变化。
- Player 在公共轴上比较音画时间：视频超前超过 10 ms 等待，落后超过 80 ms 丢弃，一次循环只呈现最新的合适帧。音频断供期间不推进视频；恢复后根据新位置调度。
- 正常结束先有限排空音频（最多 500 ms），保留最终估算位置，余下超出音频终点的帧丢弃。主动关闭或异常清空音频并丢弃待显示帧；任何路径先 join 再销毁 SDL。
- 日志报告同步建立/重新缓冲、等待 SR 丢弃的样本、迟到/末尾丢帧、最大调度差值及音频缺口/空队列次数。差值基于 SDL 输入消费位置，未补偿转换和硬件延迟，不当作物理声画误差。
- 回环媒体改为按真实媒体时长发送，普通场景预送 B 帧以供解码，并保留突发溢出测试。新增纯计算 sync_clock 和实际 TCP sync_session，覆盖 120 ms 偏移、RTP 回绕、不同 SR 时刻、延迟/缺失 SR、跳变、坏复合包和音频先结束。
