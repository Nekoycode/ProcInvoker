# ProcInvoker

Qt 5.15 / C++17 的子进程命令调用器。以子进程方式启动被调程序，通过其 **stdin 写入命令、stdout 接收消息**，支持 Tcl 交互式程序（tclsh/wish）及其他一切行协议 REPL。跨平台：Linux / Windows / Kylin，无平台特定 API。

> **维护约定**：本文件是项目的门面文档，任何对公开 API、协议配置、构建方式、测试方式的修改都必须同步更新 README，保持其为最新事实源（与 `SPEC.md` 设计契约并列）。

## 特性

- **命令顺序注册**：FIFO 队列，任一时刻最多一条 ExpectResult 命令在途，输出按注册顺序归属
- **标记定界协议**：注入带 commandId 的唯一结束标记判定命令完成，陈旧标记不会错位影响后续命令
- **提示符兜底模式**：`setPromptPattern()` 一行切换到提示符正则分帧，适配无法注入标记的封闭 REPL
- **双回调通道**：每条命令可挂 `std::function` 回调（`onResult` / `onMessage`），同时暴露 Qt signals 供全局监控
- **不阻塞 UI**：核心运行在专用工作线程；回调投递回注册命令时调用方所在线程
- **健壮失败处理**：进程崩溃时在途+排队命令逐条收到 `ProcessDied` 错误回调并自动重启；`stop()` 逐条发 `Cancelled`；单命令超时不杀进程、不影响后续命令

## 要求

- Qt >= 5.15（Qt5::Core，测试需要 Qt5::Test）
- 支持 C++17 的编译器（gcc / clang / MSVC / MinGW）
- CMake >= 3.16

## 构建与测试

```bash
cmake -G Ninja -B build          # 或不指定 -G 使用默认生成器
cmake --build build
ctest --test-dir build --output-on-failure
```

测试不依赖系统安装 tclsh：自带行协议 fixture 子进程（`tests/fixture/repl_fixture.cpp`），
由 `tst_framer`（分帧器单元测试）与 `tst_invoker`（集成测试）两个 ctest 目标驱动。

## 快速开始（Tcl，默认协议）

```cpp
#include <procinvoker/ProcInvoker.h>

auto *inv = new ProcInvoker(this);
inv->setProgram("tclsh");          // 默认协议即适配 Tcl
inv->start();

// C++17 无指定初始化器（.text = ... 是 C++20），逐字段赋值：
ProcInvoker::Command cmd;
cmd.text  = "expr {1+2}";
cmd.flags = ProcInvoker::ExpectResult | ProcInvoker::CollectAll;
cmd.onResult = [](const ProcInvoker::Result &r) {
    // 在注册命令时调用方所在线程执行（UI 线程注册即在 UI 线程回调）
    qDebug() << r.text;            // "3"
};

qint64 id = inv->registerCommand(cmd);   // 可从任意线程调用
```

`Command` 字段一览（`include/procinvoker/ProcInvoker.h`）：

| 字段 | 默认 | 说明 |
|---|---|---|
| `text` | — | 写入被调程序 stdin 的命令文本 |
| `flags` | `ExpectResult` | `ExpectResult` / `FireAndForget` / `CollectAll`，可组合 |
| `onResult` | 空 | 命令终结回调（Ok / Timeout / ProcessDied / WriteError / Cancelled） |
| `onMessage` | 空 | 命令执行期间每条中间输出回调（`Result::isIntermediate == true`） |
| `timeoutMs` | 30000 | 单命令超时，-1 表示不超时；超时**不杀进程** |
| `callbackThread` | nullptr | 回调目标线程，nullptr = 注册调用方所在线程 |

标志位语义：

- `ExpectResult`：等待被调程序返回，收到结束标记后回调（默认）
- `FireAndForget`：写完即完成，不注入探针、不等待输出，立即推进队列
- `CollectAll`：聚合命令期间全部输出，结束时经 `onResult` 一次性回调；未设置时 `onMessage` 逐条流式回调，`onResult` 的 `text` 为最后一条输出

## 协议配置（接入非 Tcl 程序）

每条 `ExpectResult` 命令实际写入子进程 stdin 的内容为：

```
<命令文本>\n
<probeCommand 将 %1 替换为 marker+commandId 后的结果>\n
```

接入契约只有一条：**被调程序收到探针行后，把 `%1` 替换进来的完整字符串原样打印到 stdout**。
通过两个 setter 适配任意程序：

```cpp
inv->setMarker("...");         // 默认 "\x1dDONE\x1d"；期望标记 = marker + commandId
inv->setProbeCommand("...");   // 默认 "puts \"%1\""（Tcl）
```

常见被调程序配置示例：

```cpp
// Tcl（默认，无需配置）
// probe = "puts \"%1\""

// Python 交互式解释器
inv->setProbeCommand("print(\"%1\")");

// bash / sh
inv->setProbeCommand("printf '%s\\n' \"%1\"");

// 自有程序：约定一条"原样回显参数"的命令即可（测试 fixture 的 emitmark 即此模式）
inv->setProbeCommand("emitmark %1");
```

注意：

- `%1` 是 `QString::arg` 纯文本替换，引号需写在模板里（如 `"%1"`），保证替换后对被调程序是合法的一行命令。
- 默认 marker 含 GS 控制字符（`\x1d`）以避免与正常输出撞车；若被调程序不便打印控制字符，换成不可能出现在正常输出中的普通字符串（不要以数字结尾，数字后缀留给 commandId）：
  `inv->setMarker("@@PROCINVOKER_DONE_9f3c@@");`
- 标记唯一化保证：超时/迟到命令的陈旧标记一律被丢弃，不会错位结束后续命令。

### 提示符正则匹配模式（opt-in 兜底）

用于无法注入标记的封闭 REPL（没有任何"打印指定字符串"手段的第三方 REPL）：

```cpp
inv->setPromptPattern("% ");   // 设置即切换到提示符分帧模式
// inv->clearPromptPattern();  // 切回默认标记模式
```

语义：

- 不注入探针（`marker`/`probeCommand` 不生效）；以输出流**尾部**匹配提示符正则判定
  ExpectResult 命令结束，提示符文本从输出中剥除、不作为消息投递。
- 提示符通常不以换行结尾（如 tclsh 的 `% `）；跨 readyRead 分片到达可正确处理，
  完整行内类似提示符的文本不会误判。
- 启动期提示符由核心层吸收：见到首个提示符前不转 `Idle`、不推进队列，
  首条命令不会被启动提示符误杀或污染。**前提：被调程序启动时必须会打印一次
  提示符**——若程序启动时不打印提示符（配置错误），队列将不会推进、状态停在
  `Stopped`，此场景无超时兜底。
- 建议在 `start()` 前配置；运行期切换仅影响后续命令，在途命令按启动时闩锁的模式结束。
- 无效正则不进入提示符模式：告警并保持原（标记）模式。
- 提示符正则的匹配遵循 `setCodec()` 配置的编码；pattern 不得能匹配实际提示符的
  真前缀（如 `% ?` 能匹配 `%`），否则提示符跨分片到达时会提前误判。
- 假定被调程序无 stdin 回显（或回显已关闭），否则回显的命令文本会污染归属。

**提示符模式的三段警告（原理性限制，无法修）：**

1. **FireAndForget 混用误归属**：FF 命令同样会产生提示符，该提示符到达时若已有
   ExpectResult 命令在途会被误归属（使其提前结束）。提示符模式下避免 FF 与
   ExpectResult 紧邻混用。
2. **超时陈旧提示符错位**：超时命令迟到的提示符无法唯一化区分（与标记模式不同，
   提示符没有 commandId），会提前结束当前命令。
3. **假阳性**：输出尾部恰好匹配提示符正则的文本会被误判为提示符。

## 其他配置

```cpp
inv->setProgram(program, args);   // 程序路径与启动参数（start 前调用）
inv->setWorkingDirectory(dir);    // 子进程工作目录
inv->setCodec("GBK");             // stdin/stdout 编码，默认 UTF-8；
                                  // 需为 ASCII 兼容、单字节换行的编码（不支持 UTF-16 等有状态编码）
inv->setRestartDelayMs(2000);     // 崩溃后自动重启延迟，默认 1000ms；-1 关闭自动重启
inv->state();                     // Stopped / Idle / Busy / Faulted
inv->cancelCommand(id);           // 取消排队中的命令（在途不可取消）
inv->stop();                      // 在途+排队命令逐条收到 Cancelled，终止进程
```

Qt signals（全局监控通道）：`commandFinished`、`messageReceived`、`stderrReceived`、
`processDied`、`restarted`、`stateChanged`。

## 线程模型

- `ProcInvokerCore`（QProcess、队列、状态机、定时器）运行在专用工作线程，进程读写不阻塞调用方。
- `registerCommand()` / `cancelCommand()` 可从任意线程调用（内部 queued 投递）。
- 回调投递回注册命令时调用方所在线程（或 `Command::callbackThread` 指定线程）。
  **回调目标线程必须比 ProcInvoker 存活更久**；目标线程已销毁或无事件循环时回调被丢弃并告警。

## 失败语义

| 事件 | 行为 |
|---|---|
| 单命令超时 | 该命令以 `Timeout` 回调并出队，**不杀进程**，队列立即推进 |
| 运行期崩溃 | 在途+排队命令逐条 `ProcessDied` 回调，清空队列，按 `restartDelayMs` 自动重启，成功发 `restarted()` |
| `FailedToStart`（程序不存在等） | 进 `Faulted`，发 `processDied`，**不自动重启**，等人工干预 |
| `stop()` | 在途+排队命令逐条 `Cancelled` 回调，终止进程（工作线程内最多阻塞约 1s 等待进程退出） |

## 已知限制

1. **协议固有限制**：超时命令迟到的输出行（非标记）可能被归入下一条命令的 `onMessage`
   中间消息；`onResult` 最终归属不受影响（标记按 commandId 唯一化）。
2. ~~封闭 REPL 不可接入~~ **提示符正则匹配模式已实现**（`setPromptPattern`，见上节），
   作为无法注入标记的封闭 REPL 的 opt-in 兜底，但带三条原理性限制
   （FF 混用误归属、超时陈旧提示符错位、尾部假阳性）；默认标记模式不受这些限制。
3. `ExpectResult` 命令串行执行（stdin 协议无请求 ID，靠顺序归因），高吞吐场景不适用。

## 项目结构

```
CMakeLists.txt              # 顶层构建：procinvoker 库 + 测试
SPEC.md                     # 设计契约（定稿决策）
include/procinvoker/
    ProcInvoker.h           # 公开 API
src/
    ProcInvoker.cpp         # 线程安全入口 + 回调回投
    ProcInvokerCore.h/.cpp  # 工作线程核心：QProcess + FIFO 队列 + 状态机
    MarkerFramer.h/.cpp     # 唯一化标记分帧器（独立可测）
    PromptFramer.h/.cpp     # 提示符正则分帧器（独立可测）
tests/
    fixture/repl_fixture.cpp  # 行协议测试子进程（--prompt <str> 进入提示符模式）
    tst_framer.cpp            # MarkerFramer 单元测试
    tst_promptframer.cpp      # PromptFramer 单元测试
    tst_invoker.cpp           # 集成测试（标记模式 + 提示符模式）
examples/
    calc.tcl                  # 演示用被调 Tcl 程序（stdin REPL，source calc_procs.tcl）
    calc_procs.tcl            # 演示命令集 add/mul/div/fib（calc.tcl 与 embedded_host 共用）
    embedded_host.c           # 嵌入式 Tcl 宿主示例（Tcl_Eval 循环，需 tcl 开发包）
    tcl_demo.cpp              # 端到端示例：ProcInvoker 驱动 calc.tcl（构建生成 tcl_demo）
```

## 示例（真实 Tcl 程序端到端）

需要系统装有 `tclsh`（如 `sudo apt install tcl`）。构建后运行：

```bash
./build/examples/tcl_demo              # 驱动 tclsh 跑 calc.tcl
./build/examples/tcl_demo --embedded   # 驱动嵌入式 Tcl 宿主（需 tcl-dev 才会构建）
```

演示内容：ExpectResult 往返、CollectAll 聚合、流式 onMessage、Tcl 错误经 stderr
转发、FireAndForget、五条命令顺序注册按序归属。被调程序 `examples/calc.tcl` 是一个
逐行 eval stdin 的 Tcl REPL——它展示了接入 ProcInvoker 对被调方的唯一要求：
stdout 行缓冲（`fconfigure stdout -buffering line`），其余无需任何协议配合。

### 嵌入式 Tcl 宿主（Tcl_Eval / 手动事件泵形态）

`examples/embedded_host.c` 演示把 Tcl 解释器集成进 C 程序（非 tclsh、自建
stdin eval 循环 + `Tcl_DoOneEvent` 事件泵）时的接入要点，三条缺一不可：

1. **双层行缓冲**：C stdio（`setvbuf`）与 Tcl channel（`-buffering line`）各一层，
   管道下默认都是块缓冲，漏一层调用器就会等不到输出直到超时；
2. **结果走 stdout、错误走 stderr**；
3. **每条命令后泵 Tcl 事件**，否则 `after`/`fileevent`/`vwait` 类命令饿死。

满足后调用器侧零改动：`tcl_demo --embedded` 与默认形态跑同一套命令，输出一致。
若宿主直接以无脚本参数调用 `Tcl_Main`（形态 A），则开箱即用等同 tclsh；
若 stdin 根本没有接入解释器（命令入口在 GUI/socket），则超出本库职责范围。
