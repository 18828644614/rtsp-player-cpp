# 错误记录（Errors）

## [ERR-20261120-001] session_test_fixture

**Logged**: 2026-11-20
**Priority**: low
**Status**: resolved
**Area**: tests

### 摘要（Summary）
首轮 session 测试中，重复控制 URL 用例在 SDP 原文检查时提前被拒绝，未覆盖预期的 URL 解析后重复检查。

### 原始错误（Error）
`AssertionError: duplicate_url: expected duplicate resolved track URL`；实际为 `duplicate SDP control or payload type`。

### 详情（Details）
将视频控制改为相对路径 `same`，音频保持绝对 URL，形成原文不同而解析结果相同的输入。测试比较使用无符号常量以消除 MSVC C4389 警告。

### 建议行动（Suggested Action）
边界用例应构造能通过前置校验的输入，明确验证目标层。

### 元数据（Metadata）
- Related Files: tests/session_tests.py, tests/protocol_tests.cpp

### 解决情况（Resolution）
- **Resolved**: 2026-11-20
- **Notes**: 已修正测试输入，重新执行完整 CTest。

## [ERR-20261120-002] release_build

**Logged**: 2026-11-20
**Priority**: medium
**Status**: resolved
**Area**: build

### 摘要（Summary）
排版前试编译发现 `single_header` 中的变量声明末尾有多余字符。

### 原始错误（Error）
```text
error C2065: “c”: 未声明的标识符
error C2144: 语法错误:“bool”的前面应有“;”
```

### 解决情况（Resolution）
删除 `std::string value;` 后多余的 `c`，重新编译通过。

### 元数据（Metadata）
- Related Files: src/rtsp/rtsp_client.cpp

## [ERR-20261120-003] windows_apply_patch

**Logged**: 2026-11-20
**Priority**: low
**Status**: resolved
**Area**: infra

### 原始错误（Error）
```text
Error: --codex-run-as-apply-patch requires a UTF-8 PATCH argument.
Invalid patch: The last line of the patch must be '*** End Patch'
```

### 解决情况（Resolution）
在 PowerShell 中直接调用 `codex.exe --codex-run-as-apply-patch`，把多行补丁作为单个参数传入，避免批处理转发截断。

## [ERR-20261123-001] object-library-transitive-link

**Logged**: 2026-11-23
**Priority**: low
**Status**: resolved
**Area**: config

嵌套 OBJECT 目标仅传播依赖使用要求，不会自动把间接 OBJECT 文件链接进最终程序，导致 P3 首次链接找不到协议/解码符号。播放层改用 STATIC，直接收纳链接的协议和媒体对象；协议测试仍不链接 SDL。

## [ERR-20261123-002] sdl-readback-viewport

**Logged**: 2026-11-23
**Priority**: low
**Status**: resolved
**Area**: tests

SDL_RenderReadPixels 默认裁剪到当前逻辑视口，初次测试把内容边缘误当窗口黑边。软件渲染测试先切换到整个窗口视口，再检查视频像素及黑边；SDK 头文件明确说明读回的视口裁剪行为。

## [ERR-20261123-003] windows-send-buffer-test

**Logged**: 2026-11-23
**Priority**: low
**Status**: resolved
**Area**: tests

Windows 可在对端不读时暂存完整 16 MiB 发送，单次发送不一定进入等待。取消测试改为持续发送至取消，避免假设固定大小必然填满系统缓冲；本机 Python 缺少 Pillow，QA 图片转换改用打包运行时。

## [ERR-20261124-001] p4-edit-and-aac-fixture

**Logged**: 2026-11-24
**Priority**: low
**Status**: resolved
**Area**: tests

### 摘要（Summary）
精确替换脚本初次未匹配 CRLF 多行文本（missing stats tail / missing stats flush），改为先统一换行再替换，未匹配的内存修改未写回。AAC 测试样本生成指定 12 帧后，实际 ADTS 包含编码器延迟/排空产生的 13 个 AU，初次测试报 expected twelve AAC AUs。

### 解决情况（Resolution）
修改工具统一读取换行；解析样本实际 AU 数并将验收改为 13 帧、13312 个样本，保留合成命令及延迟说明。
