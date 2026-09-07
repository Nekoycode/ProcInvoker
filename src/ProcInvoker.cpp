#include "procinvoker/ProcInvoker.h"

#include "ProcInvokerCore.h"

#include <QAbstractEventDispatcher>
#include <QMetaObject>

ProcInvoker::ProcInvoker(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<ProcInvoker::Result>("ProcInvoker::Result");
    qRegisterMetaType<ProcInvoker::State>("ProcInvoker::State");

    m_thread = new QThread();
    m_thread->setObjectName(QStringLiteral("ProcInvokerWorker"));
    m_core = new ProcInvokerCore();
    m_core->moveToThread(m_thread);
    m_thread->start();

    // 核心信号统一排队回本线程再转发为公开信号
    connect(m_core, &ProcInvokerCore::stateSig, this, [this](ProcInvoker::State s) {
        m_state.storeRelaxed(int(s));
        emit stateChanged(s);
    });
    connect(m_core, &ProcInvokerCore::resultReady, this,
            [this](qint64 id, const ProcInvoker::Result &r) { emit commandFinished(id, r); });
    connect(m_core, &ProcInvokerCore::messageReady, this,
            [this](qint64 id, const ProcInvoker::Result &r) { emit messageReceived(id, r); });
    connect(m_core, &ProcInvokerCore::stderrLine, this,
            [this](const QString &line) { emit stderrReceived(line); });
    connect(m_core, &ProcInvokerCore::died, this,
            [this](const QString &reason) { emit processDied(reason); });
    connect(m_core, &ProcInvokerCore::restartedSig, this, [this] { emit restarted(); });
}

ProcInvoker::~ProcInvoker()
{
    // 在工作线程内做完清理（杀进程），随后退出线程并销毁核心对象
    if (QThread::currentThread() == m_core->thread())
        m_core->shutdown(); // 同线程直接调用，避免 BlockingQueuedConnection 自死锁
    else
        QMetaObject::invokeMethod(m_core, [this] { m_core->shutdown(); }, Qt::BlockingQueuedConnection);
    m_thread->quit();
    m_thread->wait();
    delete m_core;
    delete m_thread;
}

void ProcInvoker::postToThread(const QPointer<QThread> &target, std::function<void()> fn)
{
    if (target.isNull()) {
        qWarning("ProcInvoker: callback thread already destroyed, callback dropped");
        return;
    }
    QThread *t = target.data();
    if (!t->isRunning() && t != QThread::currentThread()) {
        qWarning("ProcInvoker: callback thread not running, callback dropped");
        return;
    }
    if (QObject *ctx = QAbstractEventDispatcher::instance(t)) {
        QMetaObject::invokeMethod(ctx, std::move(fn), Qt::QueuedConnection);
        return;
    }
    qWarning("ProcInvoker: callback thread has no event loop, callback dropped");
}

// 所有 setter 与 start()/stop()/registerCommand() 都按调用顺序 queued 投递到同一个
// 工作线程接收者，Qt 保证同一接收者的 queued 调用按投递顺序执行（保序隐含假设）。

void ProcInvoker::setProgram(const QString &program, const QStringList &args)
{
    m_program = program;
    postToCore([core = m_core, program, args] { core->setProgram(program, args); });
}

void ProcInvoker::setWorkingDirectory(const QString &dir)
{
    postToCore([core = m_core, dir] { core->setWorkingDirectory(dir); });
}

void ProcInvoker::setMarker(const QString &marker)
{
    postToCore([core = m_core, marker] { core->setMarker(marker); });
}

void ProcInvoker::setProbeCommand(const QString &probe)
{
    postToCore([core = m_core, probe] { core->setProbeCommand(probe); });
}

void ProcInvoker::setCodec(const QByteArray &codecName)
{
    postToCore([core = m_core, codecName] { core->setCodec(codecName); });
}

void ProcInvoker::setRestartDelayMs(int ms)
{
    postToCore([core = m_core, ms] { core->setRestartDelayMs(ms); });
}

void ProcInvoker::setPromptPattern(const QString &regex)
{
    postToCore([core = m_core, regex] { core->setPromptPattern(regex); });
}

void ProcInvoker::clearPromptPattern()
{
    postToCore([core = m_core] { core->setPromptPattern(QString()); });
}

bool ProcInvoker::start()
{
    if (m_program.isEmpty())
        return false;
    ProcInvokerCore *core = m_core;
    QMetaObject::invokeMethod(core, [core] { core->startProcess(true); }, Qt::QueuedConnection);
    return true;
}

void ProcInvoker::stop()
{
    ProcInvokerCore *core = m_core;
    QMetaObject::invokeMethod(core, [core] { core->stopAll(); }, Qt::QueuedConnection);
}

ProcInvoker::State ProcInvoker::state() const
{
    return State(m_state.loadRelaxed());
}

qint64 ProcInvoker::registerCommand(const Command &cmd)
{
    const qint64 id = m_nextId.fetchAndAddRelaxed(1);

    Command c = cmd;
    // 都未设置时默认 ExpectResult；两者互斥时 ExpectResult 优先
    if (!c.flags.testFlag(ExpectResult) && !c.flags.testFlag(FireAndForget))
        c.flags |= ExpectResult;
    if (c.flags.testFlag(ExpectResult) && c.flags.testFlag(FireAndForget))
        c.flags &= ~FireAndForget;

    QThread *cbThread = c.callbackThread ? c.callbackThread : QThread::currentThread();
    ProcInvokerCore *core = m_core;
    QMetaObject::invokeMethod(core, [core, id, c, cbThread]() mutable { core->enqueue(id, c, cbThread); },
                              Qt::QueuedConnection);
    return id;
}

bool ProcInvoker::cancelCommand(qint64 id)
{
    if (QThread::currentThread() == m_core->thread())
        return m_core->cancelQueued(id); // 同线程直接调用
    bool cancelled = false;
    QMetaObject::invokeMethod(m_core, [this, id, &cancelled] { cancelled = m_core->cancelQueued(id); },
                              Qt::BlockingQueuedConnection);
    return cancelled;
}
