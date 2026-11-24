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

## P4 实现核对（2026-11-24）

- 三个主要类：AacDepacketizer 拆单 AU，AudioDecoder 解码，AudioOutput 输出；RtspClient 通过配置/AU 回调与 Player 连接，继续保持协议层无 FFmpeg/SDL 依赖。
- 范围固定为配套服务端 48 kHz 单声道 AAC-LC；ASC 从 SDP 读取并校验，不固定 config 字符串，不拼 ADTS。完整 AU 携带 64 位音频 PTS，为 P5 保留时间信息。
- 单声道 FLTP 直接作为连续 F32 PCM 提交 SDL，设备适配由 SDL 完成，无需新增 swresample。输入积压最多 300 ms，超限清空积压，统计 overflows；不增加应用层音频队列或线程。
- 主线程创建音频输出对象，工作线程独占设备流操作；join 后销毁流，最后由视频对象销毁 SDL。正常结束音频最多排空 500 ms；主动停止/断线清空，保留最初异常。
- 最终 Debug / Release CTest 各 13/13；四秒配套服务端实际设备流联调分别解码 AAC 186 / 180 帧，输入队列峰值 80 / 65 ms，均零错误、零溢出。实际听音及跨轨同步仍需后续人工/P5 验证。

## P5 实施核对（2026-11-24）

- 视频原先以首个 RTP 时间戳归零，音频保留 RTP 起点；P5 统一为保留起点的扩展时间戳。
- 现有回环 SR 使用固定虚拟字段，需更新媒体测试的时间映射。
- 编辑脚本首次匹配受 CRLF 影响失败，已改为读入时规范化换行；未发生部分业务文件替换。

- 锁顺序统一为 Player → AudioOutput。音频内部不调用外部回调，主线程在 Player 锁内读取位置，以避免 SR 重置与旧位置快照交错。

- 首轮 Debug：11/14 通过。媒体回环原先将视频 100 ms/帧、音频 21.3 ms/包以相同收包间隔快速发送；音频主时钟下会正确丢弃或等待而非独立出画。已将普通媒体测试改为按媒体时长发送、预送 B 帧解码数据，保留独立 burst 场景。
- 初始 H.264 等待 IDR 时可能已经跨 RTP 回绕；解码器保留的是首个实际输入 AU 的 RTP 起点，不推测未接收的回绕周期，测试预期据此修正。

- P5 六个播放场景（正常、延迟 SR、缺 SR、SR 跳变、坏复合 RTCP、音频提前结束）首轮通过。异常回调探针发现 TEARDOWN 期间仍会触发 SR 回调，可能再次抛错；finish_media 现在先停用 SR 回调，收尾阶段继续协议统计。

- 回调异常探针最初在 PLAY 回复之前抛错，未读 PLAY 回复导致 TEARDOWN 读到旧 CSeq；原异常仍保留。探针改为 PLAY 成功后的首次 SR 抛错，以验证正常播放阶段的异常收尾；未改变原有 CSeq 校验。
- 首次真实设备联调提前结束，仅收到 74 个 AAC 帧，未达到四秒验收样本下限；保留失败日志并重新执行完整时长验收，不降低断言。

- 最终 Debug / Release CTest 各 15/15；实际窗口四秒联调视频显示 76 / 77 帧，最大调度差值 9814 / 9958 us；两次音频无溢出、无缺口、无输入欠载，TEARDOWN=200。硬件声画误差不由该统计推断。
- 编辑时包含 CRLF 的文档统一按 LF 读写；常规 git diff --check 通过。core.autocrlf=false 下的额外检查把旧 CR 当作行尾空白，应使用仓库自身换行配置检查。
