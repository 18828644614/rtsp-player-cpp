# RTSP 播放器实施计划

## 本次交付：框架（2026-11-20）

- [x] 核对现有服务端的协议、轨道和测试入口。
- [x] 创建独立项目目录，不修改服务端。
- [x] 写框架设计、阅读入口和可构建骨架。
- [x] 验证 CMake 构建与命令行帮助。

验收结果：MSVC x64 Debug 构建成功，CTest 1/1 通过；rtsp_player.exe --help 正常返回。未安装 FFmpeg 或 SDL3，未改动同级服务端项目。

## 后续实施

- [x] P1：实现一个 RTSP/TCP 会话，打印收到的两轨 RTP/RTCP 统计。不引入 FFmpeg、SDL3。
- [ ] P2：实现 H.264 拆包与访问单元重组，用 FFmpeg 在终端连续解帧。
- [ ] P3：用 SDL3 创建窗口，按视频自身时间戳显示，验证关闭和断线收尾。
- [ ] P4：实现 AAC-hbr 拆包、AAC 解码和 SDL3 音频输出。
- [ ] P5：基于 RTCP SR 对齐两轨，用估算的音频播放位置调度视频，验证端到端播放。

P1 本次实现连接和协议统计；不实现解码或播放，不安装 FFmpeg、SDL3。

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
- 未修改配套服务器。P2–P5 尚未实施；实体设备画面/声音不属于 P1 验收。
