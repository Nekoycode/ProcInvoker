#include "ProcInvokerCore.h"

#include <QRegularExpression>
#include <QTextCodec>
#include <QTimer>

namespace {
// 连续自动重启上限：启动即退出的程序按固定周期重启 kMaxAutoRestarts 次后停留 Faulted
constexpr int kMaxAutoRestarts = 5;
// stderr 行缓冲上限：无换行残余超过 64KB 强制冲刷，防内存 DoS
constexpr int kMaxStderrBuffer = 64 * 1024;
} // namespace

ProcInvokerCore::ProcInvokerCore(QObject *parent)
    : QObject(parent)
{
    // 子对象随 moveToThread 一起迁入工作线程
    m_cmdTimer = new QTimer(this);
    m_cmdTimer->setSingleShot(true);
    connect(m_cmdTimer, &QTimer::timeout, this, [this] {
        if (m_hasCurrent)
            finishCurrent(ProcInvoker::Status::Timeout, QString());
    });
}

void ProcInvokerCore::setProgram(const QString &program, const QStringList &args)
{
    m_program = program;
    m_args = args;
}

void ProcInvokerCore::setMarker(const QString &marker)
{
    m_marker = marker;
    if (m_hasCurrent)
        return; // 在途命令的期望标记按旧值固定：推迟到 finishCurrent 再应用到 framer
    ensureCodec();
    m_framer.setMarker(m_codec->fromUnicode(m_marker));
}

void ProcInvokerCore::setPromptPattern(const QString &pattern)
{
    if (!pattern.isEmpty() && !QRegularExpression(pattern).isValid()) {
        // 无效正则：保持原模式，不进入"提示符模式但永不命中"的半开状态
        qWarning("ProcInvoker: invalid prompt pattern, keeping current framing mode");
        return;
    }
    m_promptPattern = pattern;
    if (m_hasCurrent && m_current.promptMode)
        return; // 在途命令闩锁提示符模式：其判定正则保持不变，finishCurrent 时再同步
    m_promptFramer.setPattern(m_promptPattern);
    if (pattern.isEmpty() && m_awaitingFirstPrompt) { // 启动等待期切回：取消等待，直接转 Idle
        m_awaitingFirstPrompt = false;
        if (m_process && m_process->state() == QProcess::Running) {
            setState(ProcInvoker::Idle);
            tryStartNext();
        }
    }
}

void ProcInvokerCore::setCodec(const QByteArray &name)
{
    m_codecName = name;
    if (m_hasCurrent) {
        m_codecDirty = true; // 在途命令按旧编码收发：推迟到命令结束再重建
        return;
    }
    m_codec = nullptr; // 强制按新名称重建
    ensureCodec();
}

void ProcInvokerCore::applyPendingCodec()
{
    if (!m_codecDirty)
        return;
    m_codecDirty = false;
    m_codec = nullptr;
    ensureCodec();
}

void ProcInvokerCore::ensureCodec()
{
    if (m_codec)
        return;
    m_codec = QTextCodec::codecForName(m_codecName);
    if (!m_codec) {
        qWarning("ProcInvoker: unknown codec '%s', falling back to UTF-8", m_codecName.constData());
        m_codec = QTextCodec::codecForName("UTF-8");
    }
    m_promptFramer.setCodec(m_codec);
}

void ProcInvokerCore::ensureProcess()
{
    ensureCodec();
    if (m_process)
        return;
    m_process = new QProcess(this);
    if (!m_workingDir.isEmpty())
        m_process->setWorkingDirectory(m_workingDir);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &ProcInvokerCore::onReadyRead);
    connect(m_process, &QProcess::readyReadStandardError, this, &ProcInvokerCore::onReadyReadStderr);
    connect(m_process, &QProcess::started, this, &ProcInvokerCore::onStarted);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus status) { onFinished(exitCode, status); });
    connect(m_process, &QProcess::errorOccurred, this, &ProcInvokerCore::onProcessError);
}

void ProcInvokerCore::startProcess(bool manual)
{
    if (m_program.isEmpty())
        return;
    if (m_process && m_process->state() != QProcess::NotRunning)
        return;
    if (manual)
        m_restartCount = 0; // 人工 start()：连续自动重启计数清零
    m_stopping = false;
    m_deadHandled = false;
    applyPendingCodec(); // 死亡/停止路径可能跳过了 finishCurrent 的应用
    ensureProcess();
    m_framer.reset();
    m_framer.setMarker(m_codec->fromUnicode(m_marker));
    m_promptFramer.reset();
    m_promptFramer.setPattern(m_promptPattern); // 死亡/停止路径可能跳过了 finishCurrent 的同步
    m_stderrBuffer.clear();
    m_awaitingFirstPrompt = promptMode(); // 提示符模式：先吸收启动提示符再转 Idle
    m_process->start(m_program, m_args);
}

void ProcInvokerCore::stopAll()
{
    m_stopping = true;
    m_restarting = false;
    m_restartCount = 0; // 人工 stop()：连续自动重启计数清零
    m_cmdTimer->stop();
    m_framer.reset();
    m_promptFramer.reset();
    m_stderrBuffer.clear();
    m_awaitingFirstPrompt = false;

    // 不静默丢弃：在途与排队命令逐条收到 Cancelled 结果
    if (m_hasCurrent) {
        failEntry(m_current, ProcInvoker::Status::Cancelled);
        m_hasCurrent = false;
        m_current = Entry();
    }
    while (!m_pending.isEmpty())
        failEntry(m_pending.dequeue(), ProcInvoker::Status::Cancelled);

    if (m_process && m_process->state() != QProcess::NotRunning) {
        // 先关 stdin 让 REPL 读到 EOF 自然退出；超时未退再强杀
        // （工作线程内最多阻塞约 1.5s：500ms 优雅等待 + 1000ms 强杀等待）
        m_process->closeWriteChannel();
        if (!m_process->waitForFinished(500)) {
            m_process->kill();
            m_process->waitForFinished(1000); // kill 是异步的，避免 QProcess 销毁时进程仍在运行
        }
    }
    setState(ProcInvoker::Stopped);
}

void ProcInvokerCore::shutdown()
{
    stopAll();
}

void ProcInvokerCore::enqueue(qint64 id, const ProcInvoker::Command &cmd, QThread *cbThread)
{
    Entry e;
    e.id = id;
    e.cmd = cmd;
    e.cbThread = cbThread;
    if (m_state == ProcInvoker::Faulted && !m_restarting) {
        // 进程已死且无重启计划（FailedToStart 或连续重启达上限）：立即失败，不无声悬死
        failEntry(e, ProcInvoker::Status::ProcessDied);
        return;
    }
    // 未启动/已停止时滞留排队是有意设计（register-before-start），仅首批告警一次。
    // 判据在 Stopped 之外叠加进程真实状态：QProcess::start() 同步进入 Starting，
    // start() 后立刻注册命令时 onStarted 尚未触发、m_state 仍是 Stopped，不得误报；
    // Faulted 重启窗口（m_restarting）也不告警——命令将在自动重启后执行
    if (m_state == ProcInvoker::Stopped
        && (!m_process || m_process->state() == QProcess::NotRunning)
        && m_pending.isEmpty())
        qWarning("ProcInvoker: process not running; command queued until start()");
    m_pending.enqueue(e);
    tryStartNext();
}

bool ProcInvokerCore::cancelQueued(qint64 id)
{
    for (int i = 0; i < m_pending.size(); ++i) {
        if (m_pending[i].id == id) {
            m_pending.removeAt(i);
            return true;
        }
    }
    return false;
}

void ProcInvokerCore::tryStartNext()
{
    if (m_hasCurrent || m_stopping || m_awaitingFirstPrompt)
        return;
    if (!m_process || m_process->state() != QProcess::Running)
        return; // Faulted 等状态下排队，待重启后推进
    if (m_pending.isEmpty()) {
        setState(ProcInvoker::Idle);
        return;
    }

    m_current = m_pending.dequeue();
    m_hasCurrent = true;
    m_current.promptMode = promptMode(); // 闩锁启动时的分帧模式
    setState(ProcInvoker::Busy);

    const auto flags = m_current.cmd.flags;
    const bool expect = flags.testFlag(ProcInvoker::ExpectResult)
                        && !flags.testFlag(ProcInvoker::FireAndForget);

    QByteArray data = m_codec->fromUnicode(m_current.cmd.text);
    data += '\n';
    if (expect && !m_current.promptMode) {
        // 标记唯一化：期望标记 = marker + commandId，陈旧标记无法错位结束本命令
        // （提示符模式下不注入探针）
        const QString expected = m_marker + QString::number(m_current.id);
        m_framer.setExpectedMarker(m_codec->fromUnicode(expected));
        data += m_codec->fromUnicode(m_probeCommand.arg(expected));
        data += '\n';
    }

    const qint64 written = m_process->write(data);
    if (written < 0 || written != data.size()) {
        finishCurrent(ProcInvoker::Status::WriteError, QString());
        return;
    }

    if (flags.testFlag(ProcInvoker::FireAndForget)) {
        finishCurrent(ProcInvoker::Status::Ok, QString()); // 写完即完成，立即推进队列
        return;
    }
    if (m_current.cmd.timeoutMs >= 0)
        m_cmdTimer->start(m_current.cmd.timeoutMs);
}

void ProcInvokerCore::onReadyRead()
{
    const QByteArray data = m_process->readAllStandardOutput();

    // 分帧模式按在途命令启动时的闩锁选用，无在途命令时按当前模式
    const bool usePrompt = m_hasCurrent ? m_current.promptMode : promptMode();

    QList<QByteArray> frames;
    bool commandDone = false;
    if (usePrompt) {
        const auto res = m_promptFramer.feed(data);
        if (m_awaitingFirstPrompt) {
            // 启动期：banner 与首提示符全部丢弃，见到首提示符才转 Idle 并推进队列
            if (res.promptFound) {
                m_awaitingFirstPrompt = false;
                setState(ProcInvoker::Idle);
                tryStartNext();
            }
            return;
        }
        frames = res.frames;
        commandDone = res.promptFound;
    } else {
        const auto res = m_framer.feed(data);
        frames = res.frames;
        commandDone = res.markerFound;
    }

    for (const QByteArray &frame : frames) {
        if (m_hasCurrent)
            deliverIntermediate(m_codec->toUnicode(frame));
        // 无在途命令的输出（含陈旧提示符/标记）无归属，丢弃
    }
    if (commandDone && m_hasCurrent) {
        const QString text = m_current.cmd.flags.testFlag(ProcInvoker::CollectAll)
                                 ? m_current.collected.join(QLatin1Char('\n'))
                                 : m_current.lastLine;
        finishCurrent(ProcInvoker::Status::Ok, text);
    }
}

void ProcInvokerCore::deliverIntermediate(const QString &line)
{
    m_current.lastLine = line;
    if (m_current.cmd.flags.testFlag(ProcInvoker::CollectAll)) {
        m_current.collected.append(line);
        return;
    }
    dispatchResult(m_current, ProcInvoker::Status::Ok, line, true);
}

void ProcInvokerCore::dispatchResult(const Entry &e, ProcInvoker::Status status,
                                     const QString &text, bool intermediate)
{
    ProcInvoker::Result r;
    r.commandId = e.id;
    r.text = text;
    r.status = status;
    r.isIntermediate = intermediate;
    if (intermediate) {
        if (e.cmd.onMessage) {
            const auto cb = e.cmd.onMessage;
            const QPointer<QThread> target = e.cbThread;
            ProcInvoker::postToThread(target, [cb, r] { cb(r); });
        }
        emit messageReady(r.commandId, r);
    } else {
        if (e.cmd.onResult) {
            const auto cb = e.cmd.onResult;
            const QPointer<QThread> target = e.cbThread;
            ProcInvoker::postToThread(target, [cb, r] { cb(r); });
        }
        emit resultReady(r.commandId, r);
    }
}

void ProcInvokerCore::finishCurrent(ProcInvoker::Status status, const QString &text)
{
    m_cmdTimer->stop();
    if (status == ProcInvoker::Status::Ok)
        m_restartCount = 0; // 命令成功完成：进程健康，连续自动重启计数清零
    // 不变量：命令结束时立即清空期望标记，陈旧标记不会错位结束后续命令
    m_framer.setExpectedMarker(QByteArray());
    applyPendingCodec();
    // 应用被闩锁推迟的 setMarker/setCodec（无变更时为重放同值，无副作用）
    m_framer.setMarker(m_codec->fromUnicode(m_marker));
    dispatchResult(m_current, status, text, false);
    m_hasCurrent = false;
    m_current = Entry();
    m_promptFramer.setPattern(m_promptPattern); // 应用被闩锁推迟的模式切换
    tryStartNext();
}

void ProcInvokerCore::failEntry(const Entry &e, ProcInvoker::Status status)
{
    dispatchResult(e, status, QString(), false);
}

void ProcInvokerCore::handleDeath(const QString &reason, bool allowRestart)
{
    if (m_stopping || m_deadHandled)
        return;
    m_deadHandled = true;
    m_restarting = false; // 若重启中的进程启动即死（FailedToStart），该重启计划已终结
    m_cmdTimer->stop();
    m_framer.reset();
    m_promptFramer.reset();
    m_awaitingFirstPrompt = false;
    setState(ProcInvoker::Faulted);

    // stderr 缓冲中未换行的残余也转发出去
    if (!m_stderrBuffer.isEmpty()) {
        emit stderrLine(m_codec ? m_codec->toUnicode(m_stderrBuffer)
                                : QString::fromUtf8(m_stderrBuffer));
        m_stderrBuffer.clear();
    }

    // 在途命令与所有排队命令逐条收到 ProcessDied，不无声丢失；
    // 在途命令带上已收到的部分输出（text 规则与正常完成一致：聚合 join 或最后一条）
    if (m_hasCurrent) {
        const QString partial = m_current.cmd.flags.testFlag(ProcInvoker::CollectAll)
                                    ? m_current.collected.join(QLatin1Char('\n'))
                                    : m_current.lastLine;
        dispatchResult(m_current, ProcInvoker::Status::ProcessDied, partial, false);
        m_hasCurrent = false;
        m_current = Entry();
    }
    while (!m_pending.isEmpty())
        failEntry(m_pending.dequeue(), ProcInvoker::Status::ProcessDied);

    emit died(reason);

    // FailedToStart 不自动重启（程序路径错误需人工干预），运行期崩溃才重启；
    // 连续自动重启有上限（kMaxAutoRestarts）：启动即退出的程序不会无限重启洪泛，
    // 超限后停留 Faulted，等人工 start()（计数清零）
    if (allowRestart && m_restartDelayMs >= 0 && m_restartCount < kMaxAutoRestarts) {
        ++m_restartCount;
        m_restarting = true;
        QTimer::singleShot(m_restartDelayMs, this, [this] {
            if (!m_stopping)
                startProcess();
        });
    }
}

void ProcInvokerCore::onStarted()
{
    if (m_restarting) {
        m_restarting = false;
        emit restartedSig();
    }
    if (m_awaitingFirstPrompt)
        return; // 提示符模式：等核心吸收首个提示符后再转 Idle、推进队列
    setState(ProcInvoker::Idle);
    tryStartNext();
}

void ProcInvokerCore::onFinished(int exitCode, QProcess::ExitStatus status)
{
    handleDeath(QStringLiteral("process exited, exitCode=%1, status=%2")
                    .arg(exitCode)
                    .arg(status == QProcess::NormalExit ? QStringLiteral("NormalExit")
                                                        : QStringLiteral("CrashExit")),
                true);
}

void ProcInvokerCore::onProcessError(QProcess::ProcessError error)
{
    const bool allowRestart = (error != QProcess::FailedToStart);
    handleDeath(QStringLiteral("process error=%1").arg(int(error)), allowRestart);
}

void ProcInvokerCore::onReadyReadStderr()
{
    m_stderrBuffer += m_process->readAllStandardError();
    int pos = 0;
    while (true) {
        const int nl = m_stderrBuffer.indexOf('\n', pos);
        if (nl < 0)
            break;
        QByteArray line = m_stderrBuffer.mid(pos, nl - pos);
        if (line.endsWith('\r'))
            line.chop(1);
        if (!line.isEmpty())
            emit stderrLine(m_codec ? m_codec->toUnicode(line) : QString::fromUtf8(line));
        pos = nl + 1;
    }
    m_stderrBuffer = m_stderrBuffer.mid(pos);
    // 无换行残余超过上限：冲刷为一条并清空，防对端永不换行导致内存膨胀
    if (m_stderrBuffer.size() > kMaxStderrBuffer) {
        emit stderrLine(m_codec ? m_codec->toUnicode(m_stderrBuffer)
                                : QString::fromUtf8(m_stderrBuffer));
        m_stderrBuffer.clear();
    }
}

void ProcInvokerCore::setState(ProcInvoker::State s)
{
    if (m_state == s)
        return;
    m_state = s;
    emit stateSig(s);
}
