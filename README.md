# ProcInvoker

**English** | [中文](README.zh-CN.md)

<p>
  <img src="https://img.shields.io/badge/Qt-5.15-41CD52?logo=qt&logoColor=white" alt="Qt 5.15">
  <img src="https://img.shields.io/badge/C%2B%2B-17-00599C?logo=c%2B%2B&logoColor=white" alt="C++17">
  <img src="https://img.shields.io/badge/platforms-Linux%20%7C%20Windows-lightgrey" alt="Platforms">
  <a href="https://github.com/Nekoycode/ProcInvoker/actions/workflows/ci.yml"><img src="https://github.com/Nekoycode/ProcInvoker/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <img src="https://img.shields.io/badge/license-MIT-blue" alt="License: MIT">
</p>

**Drive any stdin/stdout REPL-style program as a child process — Tcl shells, embedded Tcl hosts, or your own command-line tools — with ordered commands, attributed results, and zero UI blocking.**

ProcInvoker launches a program as a child process, feeds it commands through stdin, and attributes every line of output back to the command that produced it. Commands are registered in order, carry flags and callbacks, and complete deterministically thanks to a unique per-command end marker injected into the wire protocol.

---

## Why ProcInvoker

- **Ordered command queue** — FIFO registration; at most one `ExpectResult` command in flight, so output always belongs to the right command
- **Unique marker framing** — every command ends with `marker + commandId`; stale markers from timed-out commands can never poison the next command
- **Dual callback channels** — per-command `std::function` callbacks (`onResult` / `onMessage`) plus Qt signals for global monitoring
- **Never blocks the UI** — the core lives on a dedicated worker thread; callbacks are posted back to the thread that registered the command
- **Crash-safe** — pending commands all receive `ProcessDied` callbacks, the process auto-restarts, timeouts never kill the process
- **Cross-platform** — Linux / Windows, Qt 5.15, C++17, no platform-specific APIs

## Quick Start

```cpp
#include <procinvoker/ProcInvoker.h>

auto *inv = new ProcInvoker(this);
inv->setProgram("tclsh");          // default protocol speaks Tcl out of the box
inv->start();

ProcInvoker::Command cmd;          // C++17: no designated initializers — assign fields
cmd.text  = "expr {sqrt(2) * 10}";
cmd.flags = ProcInvoker::ExpectResult | ProcInvoker::CollectAll;
cmd.onResult = [](const ProcInvoker::Result &r) {
    qDebug() << r.text;            // "14.142135623730951" — back on your thread
};

inv->registerCommand(cmd);         // callable from any thread
```

That's it. No protocol work needed on the Tcl side — the injected probe is itself valid Tcl.

## See It Run

A real end-to-end session against [`examples/calc.tcl`](examples/calc.tcl) (five commands registered at once, executed in order):

```console
$ ./build/examples/tcl_demo
[ add     ] status= Ok  text= 7
[ mul     ] status= Ok  text= 42
[fib 流式] fib(0) = 0
[fib 流式] fib(1) = 1
...
[ fib     ] status= Ok  text= 21      ← streaming onMessage + final onResult
[stderr ] ERROR: divide by zero       ← Tcl errors on the stderr channel
[ div     ] status= Ok  text=
[ ff      ] status= Ok  text=         ← FireAndForget: written, never waited on
```

`./build/examples/tcl_demo --embedded` runs the **same** commands against `examples/embedded_host.c` — a C program with an embedded `Tcl_Interp` — with byte-identical output. The caller cannot tell the difference.

## How It Works

```
ProcInvoker            public API — thread-safe entry, callback re-dispatch
 └── ProcInvokerCore   dedicated worker thread
      ├── QProcess        child-process channel (stdin / stdout / stderr)
      ├── MarkerFramer    unique-marker framing        (default mode)
      ├── PromptFramer    prompt-regex framing         (opt-in fallback)
      ├── FIFO queue      ordered command dispatch & state machine
      └── QTimer          per-command timeout / restart delay
```

Every `ExpectResult` command is written to stdin as:

```
<command text>\n
<probeCommand with %1 replaced by marker+commandId>\n
```

The callee only has to honor one contract: **when it receives the probe line, print the substituted string verbatim to stdout.** For Tcl that probe is simply `puts "%1"` (the default). For anything else, adapt two setters:

```cpp
inv->setMarker("@@DONE_9f3c@@");            // default "\x1dDONE\x1d"
inv->setProbeCommand("print(\"%1\")");      // Python REPL
inv->setProbeCommand("printf '%s\\n' \"%1\"");  // bash / sh
```

No way to inject a marker (closed third-party REPL)? Fall back to prompt-regex framing:

```cpp
inv->setPromptPattern("% ");   // opt-in; see docs for its three inherent limitations
```

## Feature Matrix

| Capability | API |
|---|---|
| Program, args, working directory | `setProgram()` / `setWorkingDirectory()` |
| Protocol adaptation | `setMarker()` / `setProbeCommand()` / `setPromptPattern()` |
| Encoding (ASCII-compatible codecs) | `setCodec()` — default UTF-8 |
| Crash auto-restart | `setRestartDelayMs()` — default 1000 ms, `-1` disables; gives up after 5 consecutive restarts and stays `Faulted` |
| Fire-and-forget commands | `CommandFlag::FireAndForget` |
| Aggregated vs streaming results | `CommandFlag::CollectAll`, `onMessage` |
| Per-command timeout (never kills the process) | `Command::timeoutMs` — default 30 s |
| Cancel queued commands | `cancelCommand(id)` |
| Graceful shutdown with `Cancelled` callbacks | `stop()` — closes stdin first so the REPL exits on EOF, kills only as fallback |
| Global monitoring | `commandFinished` / `messageReceived` / `stderrReceived` / `processDied` / `restarted` / `stateChanged` |

Failure semantics worth knowing: a dying process hands the in-flight command its partial output collected so far (aggregated for `CollectAll`, last line otherwise) with its `ProcessDied` result, and the `processDied` reason carries exit code and exit status (`NormalExit` / `CrashExit`). Commands registered while `Faulted` with no restart pending fail immediately with `ProcessDied` rather than hanging; while `Stopped` they intentionally queue (with a one-time warning) until `start()`. Runtime `setMarker()` / `setCodec()` / `setPromptPattern()` changes are latched: the in-flight command finishes under its original settings, new values apply from the next command on.

## Building & Testing

Requirements: Qt ≥ 5.15 (Core; Test for the suite), a C++17 compiler, CMake ≥ 3.16.

```bash
cmake -G Ninja -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Installing & Consuming Downstream

```bash
cmake --install build          # or: DESTDIR=/path/to/staging cmake --install build
```

Then in your `CMakeLists.txt`:

```cmake
find_package(ProcInvoker 1.0 REQUIRED)
target_link_libraries(app PRIVATE ProcInvoker::procinvoker)
```

The exported config pulls in `Qt5::Core` via `find_dependency`; version compatibility follows SemVer (`SameMajorVersion`). When ProcInvoker is vendored via `add_subdirectory`, tests and examples are off by default (`PROCINVOKER_BUILD_TESTS` / `PROCINVOKER_BUILD_EXAMPLES`) — library consumers don't need Qt5Test or Tcl.

The suite needs no installed Tcl: a purpose-built line-protocol fixture child process drives 74 unit & integration tests (framer edge cases, ordering, streaming, timeouts, crash/restart, callback threading, prompt mode). Examples need `tclsh`; the embedded-host example additionally needs the Tcl dev package (`apt install tcl tcl-dev`).

## Embedding Tcl? Three Rules

`examples/embedded_host.c` shows a host that evals stdin lines with `Tcl_Eval` and pumps events manually. The contract:

1. **Line buffering, twice** — C stdio (`setvbuf`) *and* the Tcl channel (`-buffering line`); pipes default to block buffering on both layers
2. **Results to stdout, errors to stderr**
3. **Pump Tcl events** (`Tcl_DoOneEvent`) after each command, or `after`/`fileevent` starve

A host that calls `Tcl_Main` with no script argument works out of the box — it *is* tclsh's own shape.

## Known Limitations

- Output lines arriving late from a timed-out command may surface as the *next* command's `onMessage` intermediates (final `onResult` attribution is always correct)
- Prompt mode: prompts cannot be unique-ified — FireAndForget mixing and timed-out stale prompts can mis-attribute; tail text matching the prompt regex causes false positives (all documented in `ProcInvoker.h`)
- `ExpectResult` commands run serially — stdin protocols carry no request ID; this design trades concurrency for attribution
- Socket-based callees are a recognized evolution (transport abstraction seam identified in [SPEC.md](SPEC.md)) but intentionally not implemented without a concrete use case

## Project Layout

```
.github/workflows/ci.yml          # manual-trigger CI: Linux + Windows build & test
include/procinvoker/ProcInvoker.h   # public API
src/                                # ProcInvoker entry · ProcInvokerCore · MarkerFramer · PromptFramer
cmake/ProcInvokerConfig.cmake.in    # package config template (install/export)
tests/                              # fixture child process · 74 unit & integration tests (ctest)
examples/                           # calc.tcl · calc_procs.tcl · embedded_host.c · tcl_demo.cpp
SPEC.md                             # design contract (pinned decisions, evolution directions)
CHANGELOG.md                        # release history (Keep a Changelog)
```

## Contributing

Keep this file and [`README.zh-CN.md`](README.zh-CN.md) in sync, and update both whenever the public API, protocol configuration, build, or test workflow changes. Design-level decisions belong in [SPEC.md](SPEC.md).

## License

[MIT](LICENSE) © 2026 nekoycode
