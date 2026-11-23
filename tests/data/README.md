# H.264 解码测试样本

`video_64x48.h264` 是本项目用 FFmpeg 的 `testsrc2` 合成的八帧 Annex B 流，无摄像头素材。
尺寸 64×48、10 fps、YUV420P，每四帧一个 IDR，含两个 B 帧；每个 AU 由 AUD 开始。
解码输入顺序为 I0、P3、B1、B2、I4、P7、B5、B6，用于验证帧重排、排空和时间戳。
测试运行仅需 H.264 解码器，不依赖 ffmpeg 命令行或 libx264 编码器。

重新生成（需要带 libx264 的 ffmpeg）：

```powershell
ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=size=64x48:rate=10 -frames:v 8 -c:v libx264 -preset veryfast -pix_fmt yuv420p -x264-params "keyint=4:min-keyint=4:scenecut=0:bframes=2:b-adapt=0:aud=1:repeat-headers=1:open-gop=0" -f h264 video_64x48.h264
```
