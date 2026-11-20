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
