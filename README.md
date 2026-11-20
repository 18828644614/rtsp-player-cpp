# C++ RTSP 配套播放器

配套服务器：[rtsp-cpp](../rtsp-cpp/README.md)。本项目独立构建，首版面向 Windows。

**当前只有设计和可构建入口，还不能连接或播放。** 实现前先看 [框架设计](DESIGN.md)，开发顺序和验收见 [阶段计划](task_plan.md)。

在项目根目录，用 Visual Studio 的 C++ 工具链构建骨架：

~~~powershell
cmake -S . -B build -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
.\build\Debug\rtsp_player.exe --help
~~~

骨架不需要 FFmpeg 或 SDL3。第一项开发任务是让终端打印 DESCRIBE → SETUP（视频）→ SETUP（音频）→ PLAY 和两轨收包统计，再加解码、窗口与声音。
