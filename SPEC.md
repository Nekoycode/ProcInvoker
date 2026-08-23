# ProcInvoker 设计规格

Qt 5.15.2 / C++17，跨平台（Linux / Windows / Kylin）。基于 QProcess 的子进程命令调用器，
支持 Tcl 交互式程序及其他一切"stdin 进命令、stdout 出消息"的协议。

## 架构

```
ProcInvoker（公开 API，线程安全入口，QObject）
 └─ ProcInvokerCore（活在专用 QThread 工作线程）
     ├─ QProcess            子进程通道
     ├─ MarkerFramer        分帧 + 结束标记识别（可独立单测，不依赖进程）
     ├─ PromptFramer        提示符正则分帧（兜底模式，可独立单测）
     ├─ QQueue<Command>     FIFO 命令队列
     ├─ 状态机              Idle → Writing → Awaiting → Idle / Faulted
     └─ QTimer              单命令超时 / 重启延迟
```

## 关键决策（已定稿，不得偏离）

1. **消息定界：注入分隔标记 + 标记唯一化。** 每条 ExpectResult 命令写入 stdin 时，自动在其后
   追加一行探针命令（probe），探针会让子进程打印**带命令 id 的结束标记**：期望标记 =
   `marker + commandId`。读到匹配的期望标记即判定当前命令结束，之前的 stdout 输出归给
   该命令；不匹配的标记（超时命令的陈旧标记等）一律丢弃，不会错位结束后续命令。
   - `marker`（默认 `"\x1dDONE\x1d"`）与 `probeCommand`（模板，默认 Tcl 形式
     `puts "%1"`，`%1` 为完整期望标记）均可配置，以适配非 Tcl 程序。
   - FireAndForget 命令不注入探针、不等待输出。
   - 协议固有限制：超时命令迟到的输出行（非标记）可能被归入下一条命令的
     onMessage 中间消息；onResult 最终归属不受影响。
   - **兜底（opt-in）：提示符正则匹配分帧模式**（`setPromptPattern`/`clearPromptPattern`），
     用于无法注入标记的封闭 REPL。不注入探针（marker/probe 不生效），以输出流尾部匹配
     提示符正则判定 ExpectResult 命令结束，提示符文本剥除、不投递；跨分片到达正确处理；
     启动期提示符由核心层吸收（见首个提示符前不转 Idle、不推进队列）；无在途命令时的
     提示符一律丢弃。无效正则不进入提示符模式（告警并保持原模式）；在途命令按启动时
     闩锁的分帧模式结束，运行期切换仅影响后续命令；假定被调程序无 stdin 回显。
     已知限制（原理性）：FireAndForget 产生的
     提示符会误归属给随后在途命令、超时命令的陈旧提示符无法唯一化会提前结束当前命令、
     输出尾部恰好匹配正则会误判——文档（README 与头文件注释）明确警告。

2. **命令抽象。** `Command` 为值类型：`text`、`flags`、`onResult`、`onMessage`、
   `timeoutMs`（默认 30000，-1 表示不超时）。
   标志位（QFlags 风格）：
   - `ExpectResult`：等待返回并回调（与 FireAndForget 互斥，都未设置时默认 ExpectResult）
   - `FireAndForget`：写完即完成，立即推进队列
   - `CollectAll`：聚合命令期间所有输出，结束时一次性经 onResult 回调；
     未设置时 onMessage 逐条流式回调，onResult 只带最终归属摘要
   回调签名：`std::function<void(const Result&)>`。
   `Result`：`commandId`、`text`（聚合或单条）、`status`
   （`Ok / Timeout / ProcessDied / WriteError / Cancelled`）、`isIntermediate`。

3. **回调双通道。**
   - 每条命令挂 `onResult` / `onMessage`（std::function），随命令走，天然有归属。
   - ProcInvoker 同时暴露 Qt signals：`commandFinished(qint64 id, Result)`、
     `messageReceived(qint64 id, Result)`、`processDied(QString)`、`restarted()`、
     `stateChanged(...)`，供全局监控。

4. **线程模型：工作线程 + 回调回投（不阻塞 UI）。**
   - ProcInvokerCore moveToThread 到专用 QThread；进程读写、等待、超时全在工作线程
     事件循环中执行。
   - `registerCommand()` 可从任意线程调用，内部用
     `QMetaObject::invokeMethod(core, ..., Qt::QueuedConnection)` 投递到工作线程。
   - 回调执行线程：注册时捕获调用方线程为回调目标线程（允许显式覆盖），
     触发时 queued 投递回该线程。UI 线程注册的回调就在 UI 线程执行。
   - 跨线程传递的类型需 Q_DECLARE_METATYPE + qRegisterMetaType。

5. **失败处理：错误回调 + 清空 + 自动重启。**
   - 子进程运行期异常退出：在途命令与所有排队命令逐条收到 `ProcessDied` 错误结果
     （onResult 照常触发，不无声丢失），队列清空。
   - `FailedToStart`（程序路径无效等）**不自动重启**：直接进 Faulted、发 processDied，
     等人工干预，避免无限重启洪泛。
   - 运行期崩溃按可配置延迟自动重启（默认 1000ms，-1 表示不自动重启），
     重启成功发 `restarted()`。
   - `stop()` 时对在途+排队命令逐条发 `Cancelled` 错误结果，再终止进程。
   - 单条命令超时：该命令以 `Timeout` 回调并出队，**不杀进程**；
     标记唯一化保证其迟到的陈旧标记不会错位影响后续命令，队列立即推进。

6. **其他。**
   - stdin/stdout 编码默认 UTF-8，可配置（Qt5 用 QTextCodec）。
   - stderr 通过 signal `stderrReceived(QString)` 转发，不参与命令归属。
   - stdin 写入异步进行，不阻塞事件循环。
   - Kylin 与 Linux 行为一致，无特殊分支；不得使用任何平台特定 API。

## 公开 API 草案（可按实现需要微调，语义不变）

```cpp
class ProcInvoker : public QObject {
    Q_OBJECT
public:
    explicit ProcInvoker(QObject *parent = nullptr);
    ~ProcInvoker() override;

    void setProgram(const QString &program, const QStringList &args = {});
    void setWorkingDirectory(const QString &dir);
    void setMarker(const QString &marker);          // 默认 "\x1dDONE\x1d"
    void setProbeCommand(const QString &probe);     // 默认 "puts \"\\x1dDONE\\x1d\""
    void setCodec(const QByteArray &codecName);     // 默认 "UTF-8"
    void setRestartDelayMs(int ms);                 // 默认 1000，-1 不自动重启

    bool start();
    void stop();                                    // 清空队列，终止进程
    State state() const;                            // Idle/Busy/Faulted/Stopped

    qint64 registerCommand(const Command &cmd);     // 返回 commandId，任意线程可调
    bool cancelCommand(qint64 id);                  // 仅排队中可取消

signals:
    void commandFinished(qint64 id, const ProcInvoker::Result &r);
    void messageReceived(qint64 id, const ProcInvoker::Result &r);
    void stderrReceived(const QString &line);
    void processDied(const QString &reason);
    void restarted();
    void stateChanged(ProcInvoker::State s);
};
```

## 测试要求（Qt Test / QTest，ctest 驱动）

测试不依赖系统安装 tclsh。自带一个 CMake 构建的 fixture 子进程
（纯 C++ 小程序，行协议 REPL），支持指令：
- `print <text>`：原样打印一行
- `printmulti <n> <text>`：打印 n 行（用于流式 onMessage）
- `emitmark <text>`：原样打印 text（probeCommand 在测试中配置为 `emitmark %1`，
  `%1` 即完整期望标记 marker+commandId）
- `err <text>`：向 stderr 打印一行（用于 stderr 转发测试）
- `nop`：不产生任何输出（避免无归属回显串入下一条命令）
- `slow <ms> <text>`：延迟 ms 后打印（用于超时测试）
- `crash`：立即 exit(1)（用于进程死亡/重启测试）
- 其他输入：原样回显一行
- 以 `--prompt <str>` 启动时进入提示符模式：启动即打印一次提示符（不换行），
  之后每处理完一行命令再打印一次提示符

必须覆盖的场景：
1. MarkerFramer 纯单元测试（分帧、标记内嵌、跨缓冲区分片）
2. 启动/停止生命周期
3. ExpectResult 命令完整往返，结果内容正确
4. 多条命令顺序注册，结果按注册顺序归属
5. FireAndForget 不阻塞队列推进
6. onMessage 流式中间消息（printmulti）
7. CollectAll 聚合
8. 超时：slow 命令 → Timeout 回调，后续命令仍正常执行，进程不被杀
9. 进程死亡：crash → 在途+排队命令全收到 ProcessDied，自动重启后 restarted()，
   新命令可正常执行
10. 回调线程正确性：UI 线程注册的回调在 UI 线程执行
11. cancelCommand 取消排队命令
12. stderr 转发

## 工程结构

```
CMakeLists.txt            # 顶层，C++17，Qt5::Core Qt5::Test，enable_testing
SPEC.md
include/procinvoker/ProcInvoker.h
src/ProcInvoker.cpp
src/ProcInvokerCore.h / .cpp
src/MarkerFramer.h / .cpp
src/PromptFramer.h / .cpp
tests/CMakeLists.txt
tests/fixture/repl_fixture.cpp
tests/tst_framer.cpp
tests/tst_promptframer.cpp
tests/tst_invoker.cpp
examples/CMakeLists.txt     # PROCINVOKER_BUILD_EXAMPLES（默认 ON）
examples/calc.tcl           # 演示用被调 Tcl 程序（source calc_procs.tcl）
examples/calc_procs.tcl     # 演示命令集（calc.tcl 与 embedded_host 共用）
examples/embedded_host.c    # 嵌入式 Tcl 宿主示例（形态 B，需 tcl 开发包）
examples/tcl_demo.cpp       # 端到端示例（--embedded 切换被调方为 embedded_host）
```

## 演进方向（已识别，未实现，勿提前抽象）

- **传输层抽象（socket 支持）**：当前唯一与"子进程"绑死的是 ProcInvokerCore 直接使用
  QProcess。队列/分帧/回调/超时均传输无关。若未来出现 socket 形态被调方（TCP daemon、
  QLocalSocket 本地 IPC），正确做法是在 ProcInvokerCore 与 QProcess 之间抽窄接口
  IChannel（write/readAll/readyRead/died），派生 ProcessChannel / TcpChannel /
  LocalChannel。注意语义重映射：start→connect、崩溃→断线检测、自动重启→退避重连、
  stderr 无对应物。**在有具体场景前不实现**——避免投机抽象。

