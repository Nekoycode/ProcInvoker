# ProcInvoker

[English](README.md) | **中文**

<p>
  <img src="https://img.shields.io/badge/Qt-5.15-41CD52?logo=qt&logoColor=white" alt="Qt 5.15">
  <img src="https://img.shields.io/badge/C%2B%2B-17-00599C?logo=c%2B%2B&logoColor=white" alt="C++17">
  <img src="https://img.shields.io/badge/platforms-Linux%20%7C%20Windows-lightgrey" alt="支持平台">
  <a href="https://github.com/Nekoycode/ProcInvoker/actions/workflows/ci.yml"><img src="https://github.com/Nekoycode/ProcInvoker/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <img src="https://img.shields.io/badge/license-MIT-blue" alt="许可证: MIT">
</p>

**把一切"stdin 进命令、stdout 出消息"的程序当作子进程驱动——Tcl 解释器、嵌入式 Tcl 宿主、或你自己的命令行工具——命令有序、结果归属正确、UI 零阻塞。**

ProcInvoker 以子进程方式启动被调程序，经 stdin 写入命令，并把每一行输出准确归因到产生它的命令。命令顺序注册、携带标志位与回调，借助注入协议的唯一结束标记（marker + commandId）确定性地完成。

---

## 为什么选择 ProcInvoker

- **有序命令队列** — FIFO 注册，任一时刻最多一条 `ExpectResult` 命令在途，输出永远归属正确的命令
- **唯一化标记分帧** — 每条命令以 `marker + commandId` 结束；超时命令的陈旧标记永远不会错位污染下一条命令
- **双回调通道** — 每条命令可挂 `std::function` 回调（`onResult` / `onMessage`），同时暴露 Qt signals 供全局监控
- **不阻塞 UI** — 核心运行在专用工作线程；回调投递回注册命令时调用方所在线程
- **崩溃安全** — 排队命令全部收到 `ProcessDied` 回调、进程自动重启、超时绝不杀进程
- **跨平台** — Linux / Windows，Qt 5.15，C++17，无平台特定 API

## 快速开始

```cpp
#include <procinvoker/ProcInvoker.h>

auto *inv = new ProcInvoker(this);
inv->setProgram("tclsh");          // 默认协议即适配 Tcl，零配置
inv->start();

ProcInvoker::Command cmd;          // C++17 无指定初始化器，逐字段赋值
cmd.text  = "expr {sqrt(2) * 10}";
cmd.flags = ProcInvoker::ExpectResult | ProcInvoker::CollectAll;
cmd.onResult = [](const ProcInvoker::Result &r) {
    qDebug() << r.text;            // "14.142135623730951"——在注册线程执行
};

inv->registerCommand(cmd);         // 可从任意线程调用
```

如此而已。Tcl 侧无需任何协议配合——注入的探针本身就是合法 Tcl。

## 实际运行

对 [`examples/calc.tcl`](examples/calc.tcl) 的真实端到端会话（五条命令一次性注册，按序执行）：

```console
$ ./build/examples/tcl_demo
[ add     ] status= Ok  text= 7
[ mul     ] status= Ok  text= 42
[fib 流式] fib(0) = 0
[fib 流式] fib(1) = 1
...
[ fib     ] status= Ok  text= 21      ← 流式 onMessage + 终结 onResult
[stderr ] ERROR: divide by zero       ← Tcl 错误走 stderr 通道
[ div     ] status= Ok  text=
[ ff      ] status= Ok  text=         ← FireAndForget：写完即完成，不等返回
```

`./build/examples/tcl_demo --embedded` 用**同一套**命令驱动 `examples/embedded_host.c`——一个内嵌 `Tcl_Interp` 的 C 程序——输出逐字节一致。调用器无法区分两者。

## 工作原理

```
ProcInvoker            公开 API——线程安全入口，回调回投
 └── ProcInvokerCore   专用工作线程
      ├── QProcess        子进程通道（stdin / stdout / stderr）
      ├── MarkerFramer    唯一化标记分帧      （默认模式）
      ├── PromptFramer    提示符正则分帧      （opt-in 兜底）
      ├── FIFO 队列       有序命令调度与状态机
      └── QTimer          单命令超时 / 重启延迟
```

每条 `ExpectResult` 命令写入 stdin 的实际内容：

```
<命令文本>\n
<probeCommand 将 %1 替换为 marker+commandId 后的结果>\n
```

被调方只需满足一条契约：**收到探针行后，把替换进来的字符串原样打印到 stdout**。Tcl 的探针就是 `puts "%1"`（默认值）。其他程序只需调整两个 setter：

```cpp
inv->setMarker("@@DONE_9f3c@@");                // 默认 "\x1dDONE\x1d"
inv->setProbeCommand("print(\"%1\")");          // Python REPL
inv->setProbeCommand("printf '%s\\n' \"%1\"");  // bash / sh
```

被调方是封闭第三方 REPL、无法注入标记？退回提示符正则分帧：

```cpp
inv->setPromptPattern("% ");   // opt-in；三条原理性限制见文档
```

## 能力矩阵

| 能力 | API |
|---|---|
| 程序、参数、工作目录 | `setProgram()` / `setWorkingDirectory()` |
| 协议适配 | `setMarker()` / `setProbeCommand()` / `setPromptPattern()` |
| 编码（ASCII 兼容编码） | `setCodec()`——默认 UTF-8 |
| 崩溃自动重启 | `setRestartDelayMs()`——默认 1000ms，-1 关闭；连续 5 次重启仍失败则停留 Faulted |
| 即发即弃命令 | `CommandFlag::FireAndForget` |
| 聚合 vs 流式结果 | `CommandFlag::CollectAll`、`onMessage` |
| 单命令超时（不杀进程） | `Command::timeoutMs`——默认 30s |
| 取消排队命令 | `cancelCommand(id)` |
| 优雅停止（逐条 `Cancelled` 回调） | `stop()`——先关闭 stdin 让 REPL 读到 EOF 自然退出，未退出再强杀 |
| 全局监控 | `commandFinished` / `messageReceived` / `stderrReceived` / `processDied` / `restarted` / `stateChanged` |

值得了解的失败语义：进程死亡时，在途命令的 `ProcessDied` 结果带已收到的部分输出（CollectAll 为聚合，否则为最后一条），`processDied` 的 reason 带 exitCode 与 ExitStatus（`NormalExit` / `CrashExit`）。Faulted 且无重启计划时注册的命令立即收到 `ProcessDied`，不会悬死；Stopped 时注册则有意滞留排队（告警一次）直到 `start()`。运行期 `setMarker()` / `setCodec()` / `setPromptPattern()` 修改均为闩锁式：在途命令按原配置结束，新值从下一条命令起生效。

## 构建与测试

要求：Qt ≥ 5.15（Core；测试需要 Test）、支持 C++17 的编译器、CMake ≥ 3.16。

```bash
cmake -G Ninja -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## 安装与下游消费

```bash
cmake --install build          # 或：DESTDIR=/path/to/staging cmake --install build
```

然后在你的 `CMakeLists.txt` 中：

```cmake
find_package(ProcInvoker 1.0 REQUIRED)
target_link_libraries(app PRIVATE ProcInvoker::procinvoker)
```

导出的 config 经 `find_dependency` 自动引入 `Qt5::Core`；版本兼容遵循 SemVer（`SameMajorVersion`）。以 `add_subdirectory` 方式引入时，tests 与 examples 默认不构建（`PROCINVOKER_BUILD_TESTS` / `PROCINVOKER_BUILD_EXAMPLES`）——库消费者无需安装 Qt5Test 或 Tcl。

测试套件不依赖已安装的 Tcl：自带的行协议 fixture 子进程驱动 73 个单元与集成测试（分帧边界、顺序归属、流式、超时、崩溃重启、回调线程、提示符模式）。示例需要 `tclsh`；嵌入式宿主示例另需 Tcl 开发包（`apt install tcl tcl-dev`）。

## 嵌入 Tcl？三条规则

`examples/embedded_host.c` 演示了用 `Tcl_Eval` 逐行 eval stdin、手动泵事件的宿主程序。接入契约：

1. **行缓冲，两层都要** — C stdio（`setvbuf`）*和* Tcl channel（`-buffering line`）；管道下两层默认都是块缓冲
2. **结果走 stdout，错误走 stderr**
3. **每条命令后泵 Tcl 事件**（`Tcl_DoOneEvent`），否则 `after`/`fileevent` 会饿死

以无脚本参数调用 `Tcl_Main` 的宿主开箱即用——那正是 tclsh 自己的形态。

## 已知限制

- 超时命令迟到的输出行可能成为*下一条*命令的 `onMessage` 中间消息（`onResult` 的最终归属永远正确）
- 提示符模式：提示符无法唯一化——FireAndForget 混用与超时陈旧提示符可能误归属；输出尾部匹配提示符正则的文本会造成假阳性（均已写入 `ProcInvoker.h` 注释）
- `ExpectResult` 命令串行执行——stdin 协议没有请求 ID，本设计以并发换归属
- socket 形态被调方是已识别的演进方向（传输抽象接缝见 [SPEC.md](SPEC.md)），无具体场景前有意不实现

## 项目结构

```
.github/workflows/ci.yml          # 手动触发 CI：Linux + Windows 构建与测试
include/procinvoker/ProcInvoker.h   # 公开 API
src/                                # ProcInvoker 入口 · ProcInvokerCore · MarkerFramer · PromptFramer
cmake/ProcInvokerConfig.cmake.in    # 包配置模板（install/export）
tests/                              # fixture 子进程 · 73 个单元与集成测试（ctest）
examples/                           # calc.tcl · calc_procs.tcl · embedded_host.c · tcl_demo.cpp
SPEC.md                             # 设计契约（定稿决策、演进方向）
CHANGELOG.md                        # 发布历史（Keep a Changelog）
```

## 贡献

本文件与 [`README.md`](README.md)（英文）互为镜像，任何对公开 API、协议配置、构建或测试流程的修改必须同步更新两者。设计级决策请写入 [SPEC.md](SPEC.md)。

## 许可证

[MIT](LICENSE) © 2026 nekoycode
