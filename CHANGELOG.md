# Changelog

本文件遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 格式，
版本号遵循[语义化版本](https://semver.org/lang/zh-CN/)。

## [1.0.0] - 2026-09-07

首个发布版本。

### 新增

- 子进程 REPL 命令调用器：stdin 进命令、stdout 出消息，FIFO 队列，回调投递回注册线程
- 标记唯一化分帧（marker + commandId）：超时命令的陈旧标记不会错位结束后续命令
- 提示符正则分帧兜底模式（`setPromptPattern`，opt-in），启动提示符由核心层吸收
- 命令标志位：`ExpectResult` / `FireAndForget` / `CollectAll`（聚合输出）
- 双回调通道：每条命令的 `onResult` / `onMessage` + 全局 Qt signals
- 单命令超时（不杀进程）、排队命令取消（`cancelCommand`）
- 崩溃自动重启（`setRestartDelayMs`，默认 1000ms，-1 关闭）
- 编码可配置（`setCodec`，默认 UTF-8）、stderr 转发（`stderrReceived`）
- CMake install/export：`find_package(ProcInvoker 1.0 REQUIRED)` 后链接
  `ProcInvoker::procinvoker`；作为子项目引入时 tests/examples 默认不构建

### 修复（发布前审查）

- D1：运行期 `setMarker()` / `setCodec()` 不再杀死在途命令——与提示符模式闩锁
  对称，在途命令期间只记录新值，命令结束后才应用（此前在途命令会因标记永不
  匹配而超时）
- D2：能启动但启动即退出的程序不再无限重启洪泛——连续自动重启上限 5 次，
  超限后停留 Faulted；计数在命令成功完成或人工 `start()`/`stop()` 后清零；
  `processDied` 的 reason 现在带 exitCode 与 ExitStatus（区分正常退出与崩溃）
- D3：进程 Faulted 且无重启计划时（FailedToStart 或重启达上限），新注册命令
  立即收到 `ProcessDied`，不再无声悬死；Stopped 状态入队滞留仍是有意设计，
  现在会告警提示。告警判据是 QProcess 真实状态而非内部状态机——`start()`
  后立即注册命令不再误报（W1）

### 加固

- 进程死亡时，在途命令的 `ProcessDied` 结果带已收到的部分输出
  （CollectAll 为聚合，否则为最后一条），不再丢弃
- `stop()` 优雅退出：先关闭 stdin 让 REPL 读到 EOF 自然退出（500ms），
  未退出再强杀
- stderr 行缓冲设 64KB 上限：无换行残余超限即强制冲刷，防内存 DoS
- PromptFramer 尾段设 1MB 上限：无匹配时冲刷为帧并清空，防内存 DoS 与
  反复全量匹配的 O(n²)
- 头文件补充使用契约：配置 setter 应在 `start()` 前同线程调用；析构须在
  无并发注册的时点进行
