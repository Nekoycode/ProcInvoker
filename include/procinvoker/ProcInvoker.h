#ifndef PROCINVOKER_H
#define PROCINVOKER_H

#include <QAtomicInteger>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QThread>

#include <functional>
#include <utility>

class ProcInvokerCore;

// Qt5 子进程命令调用器：stdin 进命令、stdout 出消息，注入分隔标记判定命令结束。
// 公开入口线程安全：registerCommand()/cancelCommand() 可从任意线程调用，
// 回调投递回注册命令时调用方所在线程（可用 Command::callbackThread 显式覆盖）。
class ProcInvoker : public QObject
{
    Q_OBJECT
public:
    enum State {
        Stopped, // 未启动或已停止
        Idle,    // 进程运行，无在途命令
        Busy,    // 有命令在途（写入中/等待结果）
        Faulted  // 进程异常退出，等待自动重启
    };
    Q_ENUM(State)

    enum class Status { Ok, Timeout, ProcessDied, WriteError, Cancelled };
    Q_ENUM(Status)

    enum CommandFlag {
        ExpectResult  = 0x1, // 等待返回并回调（默认）
        FireAndForget = 0x2, // 写完即完成，不注入探针
        CollectAll    = 0x4  // 聚合全部输出，结束时一次性经 onResult 回调
    };
    Q_DECLARE_FLAGS(CommandFlags, CommandFlag)
    Q_FLAG(CommandFlags)

    struct Result {
        qint64 commandId = -1;
        QString text;         // CollectAll 时为聚合输出；否则为最后一条输出（摘要）
        Status status = Status::Ok;
        bool isIntermediate = false; // 流式中间消息为 true
        // 协议固有限制：超时命令迟到的输出行（非标记）可能被归入下一条命令的
        // onMessage 中间消息；onResult 最终归属不受影响（标记按 commandId 唯一化）。
    };

    struct Command {
        QString text;
        CommandFlags flags = ExpectResult;
        std::function<void(const Result &)> onResult;
        std::function<void(const Result &)> onMessage;
        int timeoutMs = 30000;               // -1 表示不超时
        // nullptr = 注册调用方所在线程。回调目标线程必须比 ProcInvoker 存活更久；
        // 目标线程已销毁/无事件循环时回调会被丢弃并告警。
        QThread *callbackThread = nullptr;
    };

    explicit ProcInvoker(QObject *parent = nullptr);
    ~ProcInvoker() override;

    void setProgram(const QString &program, const QStringList &args = {});
    void setWorkingDirectory(const QString &dir);
    void setMarker(const QString &marker);       // 默认 "\x1dDONE\x1d"；实际期望标记为 marker+commandId
    // 默认 "puts \"%1\""；%1 会被替换为完整期望标记（marker+commandId），
    // 探针须让子进程原样打印该完整标记
    void setProbeCommand(const QString &probe);
    // 默认 "UTF-8"；需为 ASCII 兼容、单字节换行的编码（不支持 UTF-16 等有状态编码）
    void setCodec(const QByteArray &codecName);
    void setRestartDelayMs(int ms);              // 默认 1000，-1 不自动重启

    // 提示符分帧模式（opt-in 兜底，用于无法注入标记的封闭 REPL）。
    // 设置后切换到提示符模式：不注入探针（marker/probeCommand 不生效），以输出流
    // 尾部匹配 regex 判定 ExpectResult 命令结束，提示符文本从输出中剥除。
    // clearPromptPattern() 切回默认标记模式。建议在 start() 前配置；
    // 运行期切换仅影响后续命令，在途命令按原模式结束。
    // 前提：被调程序启动时必须打印一次提示符（启动提示符由核心层吸收，
    // 见到首个提示符前不转 Idle；程序启动时不打印提示符则队列不会推进）。
    // 已知限制（原理性，无法修）：
    //  - FireAndForget 命令产生的提示符会被误归属给随后在途的 ExpectResult 命令
    //    （提示符模式下请避免 FF 与 ExpectResult 紧邻混用）；
    //  - 超时命令的陈旧提示符无法唯一化区分（不同于标记模式），会提前结束当前命令；
    //  - 输出尾部恰好匹配提示符正则的文本会被误判为提示符（假阳性）；
    //  - 假定被调程序无 stdin 回显（或回显已关闭），否则回显的命令文本会污染归属。
    void setPromptPattern(const QString &regex);
    void clearPromptPattern();

    bool start();
    // 在途+排队命令逐条收到 Cancelled 后终止进程；工作线程内最多阻塞约 1s 等待进程退出
    void stop();
    State state() const;

    qint64 registerCommand(const Command &cmd); // 返回 commandId，任意线程可调
    bool cancelCommand(qint64 id);              // 仅排队中可取消；阻塞式跨线程调用

signals:
    void commandFinished(qint64 id, const ProcInvoker::Result &r);
    void messageReceived(qint64 id, const ProcInvoker::Result &r);
    void stderrReceived(const QString &line);
    void processDied(const QString &reason);
    void restarted();
    void stateChanged(ProcInvoker::State s);

private:
    friend class ProcInvokerCore;
    // 将 fn 投递到 target 线程事件循环执行；线程已销毁、未运行或无事件循环时丢弃并告警
    static void postToThread(const QPointer<QThread> &target, std::function<void()> fn);
    // 将 fn queued 投递到工作线程的核心对象；同接收者的 queued 调用按投递顺序执行
    template <typename Fn>
    void postToCore(Fn &&fn)
    {
        QMetaObject::invokeMethod(m_core, std::forward<Fn>(fn), Qt::QueuedConnection);
    }

    ProcInvokerCore *m_core = nullptr; // 活在 m_thread 工作线程
    QThread *m_thread = nullptr;
    QString m_program;
    QAtomicInteger<qint64> m_nextId{1};
    QAtomicInteger<int> m_state{Stopped};
};

Q_DECLARE_METATYPE(ProcInvoker::Result)
Q_DECLARE_METATYPE(ProcInvoker::State)
Q_DECLARE_OPERATORS_FOR_FLAGS(ProcInvoker::CommandFlags)

#endif // PROCINVOKER_H
