#ifndef PROCINVOKERCORE_H
#define PROCINVOKERCORE_H

#include "MarkerFramer.h"
#include "PromptFramer.h"
#include "procinvoker/ProcInvoker.h"

#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QQueue>
#include <QStringList>

class QTextCodec;
class QTimer;

// 核心对象：活在专用工作线程，进程读写、超时、状态机全在其事件循环内执行。
// 所有 public 方法只允许在本对象所在线程调用（ProcInvoker 通过 queued 投递）。
class ProcInvokerCore : public QObject
{
    Q_OBJECT
public:
    explicit ProcInvokerCore(QObject *parent = nullptr);

    void setProgram(const QString &program, const QStringList &args);
    void setWorkingDirectory(const QString &dir) { m_workingDir = dir; }
    void setMarker(const QString &marker); // 即时同步到 framer，对在途命令之后的命令生效
    void setProbeCommand(const QString &probe) { m_probeCommand = probe; }
    void setCodec(const QByteArray &name);
    void setRestartDelayMs(int ms) { m_restartDelayMs = ms; }
    // 空串 = 切回标记模式；无效正则不进入提示符模式（保持原模式）。
    // 运行期切换仅影响后续命令，在途命令按启动时闩锁的模式结束。
    void setPromptPattern(const QString &pattern);

    void startProcess();
    void stopAll();
    void shutdown();
    void enqueue(qint64 id, const ProcInvoker::Command &cmd, QThread *cbThread);
    bool cancelQueued(qint64 id);

signals:
    void resultReady(qint64 id, const ProcInvoker::Result &r);
    void messageReady(qint64 id, const ProcInvoker::Result &r);
    void stderrLine(const QString &line);
    void died(const QString &reason);
    void restartedSig();
    void stateSig(ProcInvoker::State s);

private:
    struct Entry {
        qint64 id = -1;
        ProcInvoker::Command cmd;
        QPointer<QThread> cbThread;
        QStringList collected;
        QString lastLine;
        bool promptMode = false; // 启动时闩锁的分帧模式
    };

    void ensureCodec();
    void ensureProcess();
    bool promptMode() const { return !m_promptPattern.isEmpty(); }
    void tryStartNext();
    void finishCurrent(ProcInvoker::Status status, const QString &text);
    void failEntry(const Entry &e, ProcInvoker::Status status);
    void deliverIntermediate(const QString &line);
    // 统一的结果分发：构造 Result → postToThread 回投回调 → emit 信号
    // intermediate=true 发 messageReady（走 onMessage），否则发 resultReady（走 onResult）
    void dispatchResult(const Entry &e, ProcInvoker::Status status,
                        const QString &text, bool intermediate);
    void handleDeath(const QString &reason, bool allowRestart);
    void setState(ProcInvoker::State s);

    void onReadyRead();
    void onReadyReadStderr();
    void onStarted();
    void onFinished(int exitCode);
    void onProcessError(QProcess::ProcessError error);

    QString m_program;
    QStringList m_args;
    QString m_workingDir;
    QString m_marker = QStringLiteral("\x1d" "DONE" "\x1d");
    // %1 会被替换为完整期望标记（marker + commandId）
    QString m_probeCommand = QStringLiteral("puts \"%1\"");
    QByteArray m_codecName = "UTF-8";
    QTextCodec *m_codec = nullptr;
    int m_restartDelayMs = 1000;

    QProcess *m_process = nullptr;
    QTimer *m_cmdTimer = nullptr;
    MarkerFramer m_framer;
    PromptFramer m_promptFramer;
    QString m_promptPattern; // 非空 = 提示符分帧模式
    QByteArray m_stderrBuffer; // stderr 行缓冲，避免长行跨 readyRead 被拆分
    QQueue<Entry> m_pending;
    Entry m_current;
    bool m_hasCurrent = false;
    bool m_stopping = false;
    bool m_deadHandled = false;
    bool m_restarting = false;
    // 提示符模式：启动后在核心层见到首个提示符前不转 Idle、不推进队列（吸收启动提示符）
    bool m_awaitingFirstPrompt = false;
    ProcInvoker::State m_state = ProcInvoker::Stopped;
};

#endif // PROCINVOKERCORE_H
