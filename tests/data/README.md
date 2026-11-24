# H.264 解码测试样本

`video_64x48.h264` 是本项目用 FFmpeg 的 `testsrc2` 合成的八帧 Annex B 流，无摄像头素材。
尺寸 64×48、10 fps、YUV420P，每四帧一个 IDR，含两个 B 帧；每个 AU 由 AUD 开始。
解码输入顺序为 I0、P3、B1、B2、I4、P7、B5、B6，用于验证帧重排、排空和时间戳。
测试运行仅需 H.264 解码器，不依赖 ffmpeg 命令行或 libx264 编码器。

重新生成（需要带 libx264 的 ffmpeg）：

```powershell
ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=size=64x48:rate=10 -frames:v 8 -c:v libx264 -preset veryfast -pix_fmt yuv420p -x264-params "keyint=4:min-keyint=4:scenecut=0:bframes=2:b-adapt=0:aud=1:repeat-headers=1:open-gop=0" -f h264 video_64x48.h264
```

## AAC 解码测试样本

`audio_48k_mono.aac` 是 FFmpeg sine 合成的 1 kHz 正弦波，48 kHz、单声道、AAC-LC、64 kbit/s，无麦克风素材。文件使用 ADTS 方便检查和重现；测试读取时去掉每帧 ADTS 头，只把裸 AAC AU 和 ASC=1188 交给播放器解码器。

以下命令指定 12 帧输入，编码器延迟及排空后实际得到 13 个 AU，解码为 13312 个样本。样本覆盖非静音 PCM、采样率/声道、时间戳回绕、排空与坏 AU 后恢复。测试运行仅需要 AAC 解码器，不依赖 ffmpeg 命令行。

```powershell
ffmpeg -hide_banner -loglevel error -f lavfi -i sine=frequency=1000:sample_rate=48000 -ac 1 -frames:a 12 -c:a aac -b:a 64k -f adts audio_48k_mono.aac
```
