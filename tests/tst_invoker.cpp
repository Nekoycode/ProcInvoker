#include "procinvoker/ProcInvoker.h"
#include "fixture_path.h"

#include <QMutex>
#include <QSignalSpy>
#include <QtTest/QtTest>

static const QString TEST_MARKER = QStringLiteral("@@TESTMARK@@");

using Cmd = ProcInvoker::Command;
using Result = ProcInvoker::Result;

// 捕获工作线程发出的 qWarning（QTest::ignoreMessage 不适用跨线程）
class WarningCapture
{
public:
    WarningCapture()
    {
        QMutexLocker lk(&mutex());
        warnings().clear();
        m_prev = qInstallMessageHandler(
            [](QtMsgType, const QMessageLogContext &, const QString &msg) {
                QMutexLocker lk(&mutex());
                warnings().append(msg);
            });
    }
    ~WarningCapture() { qInstallMessageHandler(m_prev); }

    static bool contains(const QString &needle)
    {
        QMutexLocker lk(&mutex());
        for (const QString &w : warnings())
            if (w.contains(needle))
                return true;
        return false;
    }

private:
    static QMutex &mutex()
    {
        static QMutex m;
        return m;
    }
    static QStringList &warnings()
    {
        static QStringList w;
        return w;
    }
    QtMessageHandler m_prev;
};

static Cmd makeCmd(const QString &text,
                   std::function<void(const Result &)> onResult = {},
                   std::function<void(const Result &)> onMessage = {},
                   ProcInvoker::CommandFlags flags = ProcInvoker::ExpectResult,
                   int timeoutMs = 30000)
{
    Cmd c;
    c.text = text;
    c.flags = flags;
    c.onResult = std::move(onResult);
    c.onMessage = std::move(onMessage);
    c.timeoutMs = timeoutMs;
    return c;
}

class TestInvoker : public QObject
{
    Q_OBJECT
private slots:
    void lifecycle();
    void expectResultRoundtrip();
    void orderedAttribution();
    void fireAndForget();
    void streamingMessages();
    void collectAll();
    void timeoutKeepsProcess();
    void timeoutStaleMarkerDoesNotKillNext();
    void stopCancelsPending();
    void failedToStartNoRestart();
    void crashAndRestart();
    void callbackThread();
    void cancelQueued();
    void stderrForwarding();
    void promptRoundtrip();
    void promptOrderedAttribution();
    void promptStreaming();
    void promptLikeTextNotMisjudged();
    void promptStartupAbsorbed();
    void promptModeLatchedMidFlight();
    void invalidPromptPatternStaysMarker();
    void defaultFlagsImplyExpectResult();
    void conflictingFlagsPreferExpectResult();
    void callbackThreadOverride();
    void registerDuringFaultedRunsAfterRestart();
    void noAutoRestartWhenDisabled();
    void stopBeatsRestartTimer();
    void clearPromptPatternDuringStartupWait();
    void markerToPromptSwitchMidFlight();
    void stderrResidualFlushedOnDeath();
    void callbackThreadDestroyedDropsCallback();
    void callbackThreadNotRunningDropsCallback();
    void unknownCodecFallsBackToUtf8();
    void stopRegisterStartCycle();
    void exit0RestartFloodCapped();
    void enqueueWhileFaultedFailsImmediately();
    void enqueueWhileStoppedWarnsButQueues();
    void setMarkerMidFlightDeferred();
    void setCodecMidFlightDeferred();
    void processDiedKeepsPartialOutput();
    void stderrLongLineCap();

private:
    static void startAndWaitIdle(ProcInvoker *inv)
    {
        QVERIFY(inv->start());
        QTRY_COMPARE_WITH_TIMEOUT(inv->state(), ProcInvoker::Idle, 5000);
    }
    // 提示符模式：fixture 以 --prompt 启动；启动提示符可能与首条命令输出粘连，
    // 先跑一条 warmup 命令将其吸收，保证后续断言确定
    static void setupPromptMode(ProcInvoker *inv)
    {
        inv->setProgram(QStringLiteral(FIXTURE_PATH),
                        {QStringLiteral("--prompt"), QStringLiteral("% ")});
        inv->setPromptPattern(QStringLiteral("% "));
        startAndWaitIdle(inv);
        QList<Result> warmup;
        inv->registerCommand(makeCmd(QStringLiteral("print warmup"),
                                     [&](const Result &r) { warmup.append(r); }));
        QTRY_COMPARE_WITH_TIMEOUT(warmup.size(), 1, 5000);
        QCOMPARE(warmup[0].status, ProcInvoker::Status::Ok);
        QCOMPARE(warmup[0].text, QStringLiteral("warmup")); // 启动提示符不得混入首条命令结果
    }
};

void TestInvoker::lifecycle()
{
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    QSignalSpy stateSpy(&inv, &ProcInvoker::stateChanged);
    QCOMPARE(inv.state(), ProcInvoker::Stopped);
    QVERIFY(inv.start());
    QTRY_COMPARE_WITH_TIMEOUT(inv.state(), ProcInvoker::Idle, 5000);
    // 状态转换经 stateChanged 信号发出
    QTRY_VERIFY_WITH_TIMEOUT(stateSpy.count() >= 1, 5000);
    QCOMPARE(qvariant_cast<ProcInvoker::State>(stateSpy.first()[0]), ProcInvoker::Idle);
    inv.stop();
    QTRY_COMPARE_WITH_TIMEOUT(inv.state(), ProcInvoker::Stopped, 5000);
    QCOMPARE(qvariant_cast<ProcInvoker::State>(stateSpy.last()[0]), ProcInvoker::Stopped);
}

void TestInvoker::expectResultRoundtrip()
{
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    startAndWaitIdle(&inv);

    QList<Result> results;
    QSignalSpy finishedSpy(&inv, &ProcInvoker::commandFinished);
    const qint64 id = inv.registerCommand(
        makeCmd(QStringLiteral("print hello"), [&](const Result &r) { results.append(r); }));

    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 5000);
    QCOMPARE(results[0].commandId, id);
    QCOMPARE(results[0].status, ProcInvoker::Status::Ok);
    QCOMPARE(results[0].text, QStringLiteral("hello"));
    QVERIFY(!results[0].isIntermediate);
    QTRY_VERIFY_WITH_TIMEOUT(finishedSpy.count() >= 1, 2000);
}

void TestInvoker::orderedAttribution()
{
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    startAndWaitIdle(&inv);

    QList<Result> results;
    auto collect = [&](const Result &r) { results.append(r); };
    const qint64 id1 = inv.registerCommand(makeCmd(QStringLiteral("print one"), collect));
    const qint64 id2 = inv.registerCommand(makeCmd(QStringLiteral("print two"), collect));
    const qint64 id3 = inv.registerCommand(makeCmd(QStringLiteral("print three"), collect));

    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 3, 5000);
    // FIFO：结果按注册顺序归属
    QCOMPARE(results[0].commandId, id1);
    QCOMPARE(results[1].commandId, id2);
    QCOMPARE(results[2].commandId, id3);
    QCOMPARE(results[0].text, QStringLiteral("one"));
    QCOMPARE(results[1].text, QStringLiteral("two"));
    QCOMPARE(results[2].text, QStringLiteral("three"));
}

void TestInvoker::fireAndForget()
{
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    startAndWaitIdle(&inv);

    QList<qint64> finishOrder;
    QList<Result> results;
    auto collect = [&](const Result &r) {
        finishOrder.append(r.commandId);
        results.append(r);
    };
    // "nop" 不产生输出，避免无归属输出串入下一条命令
    const qint64 id1 = inv.registerCommand(makeCmd(QStringLiteral("nop"), collect, {},
                                                   ProcInvoker::FireAndForget));
    const qint64 id2 = inv.registerCommand(makeCmd(QStringLiteral("print real"), collect));

    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 2, 5000);
    // FireAndForget 写完即完成，不阻塞队列推进
    QCOMPARE(finishOrder[0], id1);
    QCOMPARE(finishOrder[1], id2);
    QCOMPARE(results[1].text, QStringLiteral("real"));
    QCOMPARE(results[1].status, ProcInvoker::Status::Ok);
}

void TestInvoker::streamingMessages()
{
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    startAndWaitIdle(&inv);

    QList<Result> messages;
    QList<Result> results;
    QSignalSpy messageSpy(&inv, &ProcInvoker::messageReceived);
    inv.registerCommand(makeCmd(QStringLiteral("printmulti 3 tick"),
                                [&](const Result &r) { results.append(r); },
                                [&](const Result &r) { messages.append(r); }));

    QTRY_COMPARE_WITH_TIMEOUT(messages.size(), 3, 5000);
    for (const Result &m : messages) {
        QVERIFY(m.isIntermediate);
        QCOMPARE(m.text, QStringLiteral("tick"));
        QCOMPARE(m.status, ProcInvoker::Status::Ok);
    }
    QTRY_VERIFY_WITH_TIMEOUT(messageSpy.count() >= 3, 5000); // 中间消息同时经信号发出
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 5000);
    QVERIFY(!results[0].isIntermediate);
    QCOMPARE(results[0].status, ProcInvoker::Status::Ok);
    QCOMPARE(results[0].text, QStringLiteral("tick")); // 摘要 = 最后一条输出
}

void TestInvoker::collectAll()
{
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    startAndWaitIdle(&inv);

    QList<Result> messages;
    QList<Result> results;
    inv.registerCommand(makeCmd(QStringLiteral("printmulti 3 tick"),
                                [&](const Result &r) { results.append(r); },
                                [&](const Result &r) { messages.append(r); },
                                ProcInvoker::ExpectResult | ProcInvoker::CollectAll));

    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 5000);
    QCOMPARE(results[0].status, ProcInvoker::Status::Ok);
    QCOMPARE(results[0].text, QStringLiteral("tick\ntick\ntick"));
    QCOMPARE(messages.size(), 0); // 聚合时不逐条回调
}

void TestInvoker::timeoutKeepsProcess()
{
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    startAndWaitIdle(&inv);

    QSignalSpy diedSpy(&inv, &ProcInvoker::processDied);

    QList<Result> results;
    inv.registerCommand(makeCmd(QStringLiteral("slow 500 late"),
                                [&](const Result &r) { results.append(r); },
                                {}, ProcInvoker::ExpectResult, 150));

    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 5000);
    QCOMPARE(results[0].status, ProcInvoker::Status::Timeout);
    QCOMPARE(diedSpy.count(), 0); // 超时不杀进程
}

void TestInvoker::timeoutStaleMarkerDoesNotKillNext()
{
    // B1 回归：超时后立即注册下一条命令（不人为等待陈旧输出流过），
    // 陈旧标记（id 不匹配）必须被丢弃，不得错位结束新命令
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    startAndWaitIdle(&inv);

    QList<Result> results1;
    inv.registerCommand(makeCmd(QStringLiteral("slow 500 late"),
                                [&](const Result &r) { results1.append(r); },
                                {}, ProcInvoker::ExpectResult, 150));

    QTRY_COMPARE_WITH_TIMEOUT(results1.size(), 1, 5000);
    QCOMPARE(results1[0].status, ProcInvoker::Status::Timeout);

    QList<Result> results2;
    inv.registerCommand(makeCmd(QStringLiteral("print ok"),
                                [&](const Result &r) { results2.append(r); }));
    QTRY_COMPARE_WITH_TIMEOUT(results2.size(), 1, 5000);
    QCOMPARE(results2[0].status, ProcInvoker::Status::Ok);
    QCOMPARE(results2[0].text, QStringLiteral("ok")); // 摘要为最后一条输出，陈旧 "late" 不影响归属
    QTRY_COMPARE_WITH_TIMEOUT(inv.state(), ProcInvoker::Idle, 5000);
}

void TestInvoker::stopCancelsPending()
{
    // M2：stop() 对在途+排队命令逐条发 Cancelled 结果，不静默丢弃
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    startAndWaitIdle(&inv);

    QList<Result> results;
    auto collect = [&](const Result &r) { results.append(r); };
    const qint64 id1 = inv.registerCommand(makeCmd(QStringLiteral("slow 800 first"), collect));
    const qint64 id2 = inv.registerCommand(makeCmd(QStringLiteral("print second"), collect));

    QSignalSpy diedSpy(&inv, &ProcInvoker::processDied);
    inv.stop();

    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 2, 5000);
    QSet<qint64> cancelledIds;
    for (const Result &r : results) {
        QCOMPARE(r.status, ProcInvoker::Status::Cancelled);
        cancelledIds.insert(r.commandId);
    }
    QVERIFY(cancelledIds.contains(id1));
    QVERIFY(cancelledIds.contains(id2));
    QTRY_COMPARE_WITH_TIMEOUT(inv.state(), ProcInvoker::Stopped, 5000);
    QTest::qWait(200);
    QCOMPARE(results.size(), 2);  // 进程终止后无补发的 ProcessDied 结果
    QCOMPARE(diedSpy.count(), 0); // m_stopping 守卫：kill 引发的 finished 不得再发 processDied
}

void TestInvoker::failedToStartNoRestart()
{
    // M3：程序不存在 → FailedToStart 进 Faulted、发 processDied，不自动重启
    ProcInvoker inv;
    inv.setProgram(QStringLiteral("/nonexistent/definitely-missing-procinvoker-binary"));
    inv.setRestartDelayMs(100);
    QVERIFY(inv.start());

    QSignalSpy diedSpy(&inv, &ProcInvoker::processDied);
    QSignalSpy restartedSpy(&inv, &ProcInvoker::restarted);

    QTRY_COMPARE_WITH_TIMEOUT(inv.state(), ProcInvoker::Faulted, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(diedSpy.count() >= 1, 2000);

    // 等超过两个重启周期，确认没有重启尝试成功
    QTest::qWait(350);
    QCOMPARE(restartedSpy.count(), 0);
    QCOMPARE(inv.state(), ProcInvoker::Faulted);
}

void TestInvoker::crashAndRestart()
{
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    inv.setRestartDelayMs(100);
    startAndWaitIdle(&inv);

    QSignalSpy diedSpy(&inv, &ProcInvoker::processDied);
    QSignalSpy restartedSpy(&inv, &ProcInvoker::restarted);

    QList<Result> results;
    auto collect = [&](const Result &r) { results.append(r); };
    const qint64 id1 = inv.registerCommand(makeCmd(QStringLiteral("crash"), collect));
    const qint64 id2 = inv.registerCommand(makeCmd(QStringLiteral("print queued"), collect));

    // 在途命令与排队命令全部收到 ProcessDied
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 2, 5000);
    QSet<qint64> diedIds;
    for (const Result &r : results) {
        QCOMPARE(r.status, ProcInvoker::Status::ProcessDied);
        diedIds.insert(r.commandId);
    }
    QVERIFY(diedIds.contains(id1));
    QVERIFY(diedIds.contains(id2));
    QTRY_VERIFY_WITH_TIMEOUT(diedSpy.count() >= 1, 2000);

    // 自动重启后发 restarted()，新命令可正常执行
    QTRY_VERIFY_WITH_TIMEOUT(restartedSpy.count() >= 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(inv.state(), ProcInvoker::Idle, 5000);

    QList<Result> results3;
    inv.registerCommand(makeCmd(QStringLiteral("print back"),
                                [&](const Result &r) { results3.append(r); }));
    QTRY_COMPARE_WITH_TIMEOUT(results3.size(), 1, 5000);
    QCOMPARE(results3[0].status, ProcInvoker::Status::Ok);
    QCOMPARE(results3[0].text, QStringLiteral("back"));
}

void TestInvoker::callbackThread()
{
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    startAndWaitIdle(&inv);

    QThread *cbThread = nullptr;
    QList<Result> results;
    inv.registerCommand(makeCmd(QStringLiteral("print th"), [&](const Result &r) {
        cbThread = QThread::currentThread();
        results.append(r);
    }));

    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 5000);
    // 注册调用方在主（UI）线程，回调必须在主线程执行
    QCOMPARE(cbThread, QThread::currentThread());
    QCOMPARE(cbThread, qApp->thread());
}

void TestInvoker::cancelQueued()
{
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    startAndWaitIdle(&inv);

    QList<Result> results1;
    QList<Result> results2;
    // 第一条占住通道 400ms，第二条处于排队状态
    const qint64 id1 = inv.registerCommand(
        makeCmd(QStringLiteral("slow 400 first"), [&](const Result &r) { results1.append(r); }));
    const qint64 id2 = inv.registerCommand(
        makeCmd(QStringLiteral("print second"), [&](const Result &r) { results2.append(r); }));

    QVERIFY(inv.cancelCommand(id2));  // 排队中可取消
    QVERIFY(!inv.cancelCommand(id1)); // 在途不可取消

    QTRY_COMPARE_WITH_TIMEOUT(results1.size(), 1, 5000);
    QCOMPARE(results1[0].status, ProcInvoker::Status::Ok);
    QCOMPARE(results1[0].text, QStringLiteral("first"));

    // 取消的命令不会回调
    QTest::qWait(150);
    QCOMPARE(results2.size(), 0);
}

void TestInvoker::stderrForwarding()
{
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    startAndWaitIdle(&inv);

    QSignalSpy stderrSpy(&inv, &ProcInvoker::stderrReceived);
    inv.registerCommand(makeCmd(QStringLiteral("err boom"), {}, {}, ProcInvoker::FireAndForget));

    QTRY_VERIFY_WITH_TIMEOUT(stderrSpy.count() >= 1, 5000);
    QCOMPARE(stderrSpy[0][0].toString(), QStringLiteral("boom"));
}

void TestInvoker::promptRoundtrip()
{
    // 提示符模式：启动提示符被吸收（warmup），ExpectResult 往返结果不含提示符文本
    ProcInvoker inv;
    setupPromptMode(&inv);

    QList<Result> results;
    const qint64 id = inv.registerCommand(
        makeCmd(QStringLiteral("print hello"), [&](const Result &r) { results.append(r); }));
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 5000);
    QCOMPARE(results[0].commandId, id);
    QCOMPARE(results[0].status, ProcInvoker::Status::Ok);
    QCOMPARE(results[0].text, QStringLiteral("hello"));
}

void TestInvoker::promptOrderedAttribution()
{
    ProcInvoker inv;
    setupPromptMode(&inv);

    QList<Result> results;
    auto collect = [&](const Result &r) { results.append(r); };
    const qint64 id1 = inv.registerCommand(makeCmd(QStringLiteral("print one"), collect));
    const qint64 id2 = inv.registerCommand(makeCmd(QStringLiteral("print two"), collect));
    const qint64 id3 = inv.registerCommand(makeCmd(QStringLiteral("print three"), collect));

    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 3, 5000);
    QCOMPARE(results[0].commandId, id1);
    QCOMPARE(results[1].commandId, id2);
    QCOMPARE(results[2].commandId, id3);
    QCOMPARE(results[0].text, QStringLiteral("one"));
    QCOMPARE(results[1].text, QStringLiteral("two"));
    QCOMPARE(results[2].text, QStringLiteral("three"));
}

void TestInvoker::promptStreaming()
{
    ProcInvoker inv;
    setupPromptMode(&inv);

    QList<Result> messages;
    QList<Result> results;
    QSignalSpy messageSpy(&inv, &ProcInvoker::messageReceived);
    inv.registerCommand(makeCmd(QStringLiteral("printmulti 3 tick"),
                                [&](const Result &r) { results.append(r); },
                                [&](const Result &r) { messages.append(r); }));

    QTRY_COMPARE_WITH_TIMEOUT(messages.size(), 3, 5000);
    for (const Result &m : messages) {
        QVERIFY(m.isIntermediate);
        QCOMPARE(m.text, QStringLiteral("tick"));
    }
    QTRY_VERIFY_WITH_TIMEOUT(messageSpy.count() >= 3, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 5000);
    QCOMPARE(results[0].status, ProcInvoker::Status::Ok);
    QCOMPARE(results[0].text, QStringLiteral("tick"));
}

void TestInvoker::promptLikeTextNotMisjudged()
{
    // 输出中含类似提示符的文本（行中间）不得误判为命令结束
    ProcInvoker inv;
    setupPromptMode(&inv);

    QList<Result> results;
    inv.registerCommand(makeCmd(QStringLiteral("print progress 100% done"),
                                [&](const Result &r) { results.append(r); }));
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 5000);
    QCOMPARE(results[0].status, ProcInvoker::Status::Ok);
    QCOMPARE(results[0].text, QStringLiteral("progress 100% done"));
}

void TestInvoker::promptStartupAbsorbed()
{
    // M3：start 后立即注册（不做任何等待），核心层必须先吸收启动提示符，
    // 首条命令不得被启动提示符误杀或污染（不依赖 QProcess started/readyRead 顺序）
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH),
                   {QStringLiteral("--prompt"), QStringLiteral("% ")});
    inv.setPromptPattern(QStringLiteral("% "));
    QVERIFY(inv.start());

    QList<Result> results;
    inv.registerCommand(makeCmd(QStringLiteral("print hello"),
                                [&](const Result &r) { results.append(r); }));
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 5000);
    QCOMPARE(results[0].status, ProcInvoker::Status::Ok);
    QCOMPARE(results[0].text, QStringLiteral("hello"));
    QTRY_COMPARE_WITH_TIMEOUT(inv.state(), ProcInvoker::Idle, 5000);
}

void TestInvoker::promptModeLatchedMidFlight()
{
    // M1：运行期 clearPromptPattern() 不影响在途命令——其按启动时闩锁的提示符模式结束
    ProcInvoker inv;
    setupPromptMode(&inv);

    QList<Result> results;
    inv.registerCommand(makeCmd(QStringLiteral("slow 300 x"),
                                [&](const Result &r) { results.append(r); },
                                {}, ProcInvoker::ExpectResult, 2000));
    inv.clearPromptPattern(); // queued 于命令之后：命令已在提示符模式下启动

    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 5000);
    QCOMPARE(results[0].status, ProcInvoker::Status::Ok); // 若未闩锁则挂到 Timeout
    QCOMPARE(results[0].text, QStringLiteral("x"));
}

void TestInvoker::invalidPromptPatternStaysMarker()
{
    // M2：无效正则不进入提示符模式，保持标记模式可用。
    // 核心在工作线程发 qWarning，QTest::ignoreMessage 不适用，用自定义 handler 捕获
    static QMutex warnMutex;
    static QStringList warnings;
    {
        QMutexLocker lk(&warnMutex);
        warnings.clear();
    }
    QtMessageHandler prev = qInstallMessageHandler(
        [](QtMsgType, const QMessageLogContext &, const QString &msg) {
            QMutexLocker lk(&warnMutex);
            warnings.append(msg);
        });

    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    inv.setPromptPattern(QStringLiteral("([bad")); // 无效 → 保持标记模式
    startAndWaitIdle(&inv);

    QList<Result> results;
    inv.registerCommand(makeCmd(QStringLiteral("print hello"),
                                [&](const Result &r) { results.append(r); }));
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 5000);
    QCOMPARE(results[0].status, ProcInvoker::Status::Ok);
    QCOMPARE(results[0].text, QStringLiteral("hello"));

    qInstallMessageHandler(prev);
    QMutexLocker lk(&warnMutex);
    QVERIFY2(warnings.contains(QStringLiteral(
                 "ProcInvoker: invalid prompt pattern, keeping current framing mode")),
             qPrintable(warnings.join(QLatin1Char('\n'))));
}

void TestInvoker::defaultFlagsImplyExpectResult()
{
    // 标志位规范化：flags 全缺省时自动补 ExpectResult
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    startAndWaitIdle(&inv);

    QList<Result> results;
    Cmd c = makeCmd(QStringLiteral("print x"), [&](const Result &r) { results.append(r); });
    c.flags = ProcInvoker::CommandFlags(); // 全缺省（QFlags(0) 构造在 Qt5 已弃用）
    inv.registerCommand(c);

    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 5000);
    QCOMPARE(results[0].status, ProcInvoker::Status::Ok);
    QCOMPARE(results[0].text, QStringLiteral("x"));
}

void TestInvoker::conflictingFlagsPreferExpectResult()
{
    // 标志位规范化：ExpectResult 与 FireAndForget 同时设置时剥除 FireAndForget
    // （若 FF 生效会写完即以空文本完成，text=="x" 可区分）
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    startAndWaitIdle(&inv);

    QList<Result> results;
    Cmd c = makeCmd(QStringLiteral("print x"), [&](const Result &r) { results.append(r); });
    c.flags = ProcInvoker::ExpectResult | ProcInvoker::FireAndForget;
    inv.registerCommand(c);

    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 5000);
    QCOMPARE(results[0].status, ProcInvoker::Status::Ok);
    QCOMPARE(results[0].text, QStringLiteral("x"));
}

void TestInvoker::callbackThreadOverride()
{
    // Command::callbackThread 显式覆盖：回调在指定线程执行
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    startAndWaitIdle(&inv);

    QThread t;
    t.start();

    QAtomicInt calls{0};
    QThread *cbThread = nullptr;
    QList<Result> results;
    Cmd c = makeCmd(QStringLiteral("print ov"), [&](const Result &r) {
        cbThread = QThread::currentThread();
        results.append(r);
        calls.ref();
    });
    c.callbackThread = &t;
    inv.registerCommand(c);

    QTRY_COMPARE_WITH_TIMEOUT(calls.loadRelaxed(), 1, 5000);
    t.quit();
    t.wait(); // 回调线程退出后再读取，无数据竞争
    QCOMPARE(cbThread, &t);
    QCOMPARE(results.size(), 1);
    QCOMPARE(results[0].status, ProcInvoker::Status::Ok);
    QCOMPARE(results[0].text, QStringLiteral("ov"));
}

void TestInvoker::registerDuringFaultedRunsAfterRestart()
{
    // Faulted 期间注册的命令滞留排队，自动重启后正常执行
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    inv.setRestartDelayMs(300);
    startAndWaitIdle(&inv);

    QSignalSpy diedSpy(&inv, &ProcInvoker::processDied);
    QSignalSpy restartedSpy(&inv, &ProcInvoker::restarted);

    QList<Result> died;
    inv.registerCommand(makeCmd(QStringLiteral("crash"),
                                [&](const Result &r) { died.append(r); }));
    QTRY_VERIFY_WITH_TIMEOUT(diedSpy.count() >= 1, 5000);
    QCOMPARE(died.size(), 1);
    QCOMPARE(died[0].status, ProcInvoker::Status::ProcessDied);
    QCOMPARE(inv.state(), ProcInvoker::Faulted);

    QList<Result> results;
    inv.registerCommand(makeCmd(QStringLiteral("print back"),
                                [&](const Result &r) { results.append(r); }));
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 5000);
    QCOMPARE(results[0].status, ProcInvoker::Status::Ok);
    QCOMPARE(results[0].text, QStringLiteral("back"));
    QTRY_VERIFY_WITH_TIMEOUT(restartedSpy.count() >= 1, 5000);
}

void TestInvoker::noAutoRestartWhenDisabled()
{
    // setRestartDelayMs(-1)：崩溃后发 processDied 进 Faulted，不自动重启
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    inv.setRestartDelayMs(-1);
    startAndWaitIdle(&inv);

    QSignalSpy diedSpy(&inv, &ProcInvoker::processDied);
    QSignalSpy restartedSpy(&inv, &ProcInvoker::restarted);

    inv.registerCommand(makeCmd(QStringLiteral("crash"), {}, {}, ProcInvoker::FireAndForget));
    QTRY_VERIFY_WITH_TIMEOUT(diedSpy.count() >= 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(inv.state(), ProcInvoker::Faulted, 5000);

    QTest::qWait(500); // 等超过常规重启周期
    QCOMPARE(restartedSpy.count(), 0);
    QCOMPARE(inv.state(), ProcInvoker::Faulted);
}

void TestInvoker::stopBeatsRestartTimer()
{
    // 崩溃后、重启定时器触发前 stop()：不重启，停在 Stopped
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    inv.setRestartDelayMs(400);
    startAndWaitIdle(&inv);

    QSignalSpy diedSpy(&inv, &ProcInvoker::processDied);
    QSignalSpy restartedSpy(&inv, &ProcInvoker::restarted);

    inv.registerCommand(makeCmd(QStringLiteral("crash"), {}, {}, ProcInvoker::FireAndForget));
    QTRY_VERIFY_WITH_TIMEOUT(diedSpy.count() >= 1, 5000);
    inv.stop();

    QTest::qWait(600); // 超过重启延迟
    QCOMPARE(restartedSpy.count(), 0);
    QCOMPARE(inv.state(), ProcInvoker::Stopped);
}

void TestInvoker::clearPromptPatternDuringStartupWait()
{
    // 提示符模式启动但被调程序不打印提示符：队列卡死（已知限制）；
    // clearPromptPattern() 取消启动等待，积压命令按标记模式正常完成
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH)); // 不带 --prompt：永不打印提示符
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    inv.setPromptPattern(QStringLiteral("% "));
    QVERIFY(inv.start());

    QList<Result> results;
    inv.registerCommand(makeCmd(QStringLiteral("print hello"),
                                [&](const Result &r) { results.append(r); }));
    QTest::qWait(200);
    QCOMPARE(results.size(), 0); // 启动等待期队列不推进

    inv.clearPromptPattern();
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 5000);
    QCOMPARE(results[0].status, ProcInvoker::Status::Ok);
    QCOMPARE(results[0].text, QStringLiteral("hello"));
    QTRY_COMPARE_WITH_TIMEOUT(inv.state(), ProcInvoker::Idle, 5000);
}

void TestInvoker::markerToPromptSwitchMidFlight()
{
    // 运行期 marker→prompt 切换：在途命令按闩锁的标记模式结束（提示符文本
    // 不被剥除，作为普通输出归属），后续命令切换到提示符模式
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH),
                   {QStringLiteral("--prompt"), QStringLiteral("% ")});
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    startAndWaitIdle(&inv);

    QList<Result> results1;
    inv.registerCommand(makeCmd(QStringLiteral("slow 300 x"),
                                [&](const Result &r) { results1.append(r); }, {},
                                ProcInvoker::ExpectResult | ProcInvoker::CollectAll, 3000));
    inv.setPromptPattern(QStringLiteral("% ")); // queued 于命令之后：命令已在标记模式下启动

    QTRY_COMPARE_WITH_TIMEOUT(results1.size(), 1, 5000);
    QCOMPARE(results1[0].status, ProcInvoker::Status::Ok);
    // 标记模式语义：fixture 每行命令后打印的 "% " 提示符是普通输出。
    // 启动提示符与 "x" 粘连成 "% x"；若错误切换为提示符模式则提示符被剥除
    QCOMPARE(results1[0].text, QStringLiteral("% x\n% "));

    QList<Result> results2;
    inv.registerCommand(makeCmd(QStringLiteral("print after"),
                                [&](const Result &r) { results2.append(r); }));
    QTRY_COMPARE_WITH_TIMEOUT(results2.size(), 1, 5000);
    QCOMPARE(results2[0].status, ProcInvoker::Status::Ok);
    QCOMPARE(results2[0].text, QStringLiteral("after")); // 提示符模式下提示符被剥除
}

void TestInvoker::stderrResidualFlushedOnDeath()
{
    // stderr 未换行的残余在进程死亡时被冲刷转发
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    inv.setRestartDelayMs(-1);
    startAndWaitIdle(&inv);

    QSignalSpy stderrSpy(&inv, &ProcInvoker::stderrReceived);
    QSignalSpy diedSpy(&inv, &ProcInvoker::processDied);

    inv.registerCommand(makeCmd(QStringLiteral("errnonl partial"), {}, {},
                                ProcInvoker::FireAndForget));
    // 死亡前留 300ms 窗口，确保残余字节已进入核心层行缓冲（避免与 finished 信号竞态）
    inv.registerCommand(makeCmd(QStringLiteral("slow 300 x"), {}, {},
                                ProcInvoker::FireAndForget));
    inv.registerCommand(makeCmd(QStringLiteral("crash"), {}, {},
                                ProcInvoker::FireAndForget));

    QTRY_VERIFY_WITH_TIMEOUT(diedSpy.count() >= 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(stderrSpy.count() >= 1, 5000);
    QCOMPARE(stderrSpy[0][0].toString(), QStringLiteral("partial"));
}

void TestInvoker::callbackThreadDestroyedDropsCallback()
{
    // postToThread 异常分支：回调目标线程已销毁 → 回调丢弃并告警，信号仍发
    WarningCapture cap;
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    startAndWaitIdle(&inv);

    QThread *t = new QThread;
    t->start();

    QSignalSpy finishedSpy(&inv, &ProcInvoker::commandFinished);
    QAtomicInt calls{0};
    Cmd c = makeCmd(QStringLiteral("slow 500 gone"),
                    [&](const Result &) { calls.ref(); });
    c.callbackThread = t;
    const qint64 id = inv.registerCommand(c);

    QTest::qWait(200); // 等 enqueue 在工作线程落地（QPointer 接管），再销毁目标线程
    t->quit();
    t->wait();
    delete t;

    QTRY_VERIFY_WITH_TIMEOUT(finishedSpy.count() >= 1, 5000);
    QCOMPARE(finishedSpy[0][0].toLongLong(), id);
    const Result r = finishedSpy[0][1].value<ProcInvoker::Result>();
    QCOMPARE(r.status, ProcInvoker::Status::Ok);
    QCOMPARE(r.text, QStringLiteral("gone"));
    QCOMPARE(calls.loadRelaxed(), 0); // 回调被丢弃
    QTRY_VERIFY_WITH_TIMEOUT(
        WarningCapture::contains(QStringLiteral("callback thread already destroyed")), 2000);
}

void TestInvoker::callbackThreadNotRunningDropsCallback()
{
    // postToThread 异常分支：回调目标线程从未 start → 回调丢弃并告警，信号仍发
    WarningCapture cap;
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    startAndWaitIdle(&inv);

    QThread t; // 从未启动；存活整个用例，QPointer 保持有效
    QSignalSpy finishedSpy(&inv, &ProcInvoker::commandFinished);
    QAtomicInt calls{0};
    Cmd c = makeCmd(QStringLiteral("print nr"), [&](const Result &) { calls.ref(); });
    c.callbackThread = &t;
    const qint64 id = inv.registerCommand(c);

    QTRY_VERIFY_WITH_TIMEOUT(finishedSpy.count() >= 1, 5000);
    QCOMPARE(finishedSpy[0][0].toLongLong(), id);
    const Result r = finishedSpy[0][1].value<ProcInvoker::Result>();
    QCOMPARE(r.status, ProcInvoker::Status::Ok);
    QCOMPARE(r.text, QStringLiteral("nr"));
    QCOMPARE(calls.loadRelaxed(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(
        WarningCapture::contains(QStringLiteral("callback thread not running")), 2000);
}

void TestInvoker::unknownCodecFallsBackToUtf8()
{
    // 未知 codec：告警并回退 UTF-8，命令往返不受影响
    WarningCapture cap;
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    inv.setCodec("BOGUS");
    startAndWaitIdle(&inv);

    QList<Result> results;
    inv.registerCommand(makeCmd(QStringLiteral("print hello"),
                                [&](const Result &r) { results.append(r); }));
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 5000);
    QCOMPARE(results[0].status, ProcInvoker::Status::Ok);
    QCOMPARE(results[0].text, QStringLiteral("hello"));
    QTRY_VERIFY_WITH_TIMEOUT(
        WarningCapture::contains(QStringLiteral("unknown codec 'BOGUS'")), 2000);
}

void TestInvoker::stopRegisterStartCycle()
{
    // stop 后注册的命令滞留排队，再次 start() 后正常执行
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    startAndWaitIdle(&inv);
    inv.stop();
    QTRY_COMPARE_WITH_TIMEOUT(inv.state(), ProcInvoker::Stopped, 5000);

    QList<Result> results;
    inv.registerCommand(makeCmd(QStringLiteral("print again"),
                                [&](const Result &r) { results.append(r); }));
    QTest::qWait(150);
    QCOMPARE(results.size(), 0); // Stopped 状态不推进队列

    QVERIFY(inv.start());
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 5000);
    QCOMPARE(results[0].status, ProcInvoker::Status::Ok);
    QCOMPARE(results[0].text, QStringLiteral("again"));
}

void TestInvoker::exit0RestartFloodCapped()
{
    // D2 回归：能启动但启动即退出的程序不得无限重启洪泛——连续自动重启 5 次后
    // 停留 Faulted（初次死亡 + 5 次重启后各死一次 = 6 次 processDied）
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH), {QStringLiteral("--exit0")});
    inv.setRestartDelayMs(100);

    QSignalSpy diedSpy(&inv, &ProcInvoker::processDied);
    QSignalSpy restartedSpy(&inv, &ProcInvoker::restarted);

    QVERIFY(inv.start());

    QTRY_COMPARE_WITH_TIMEOUT(diedSpy.count(), 6, 10000);
    QCOMPARE(restartedSpy.count(), 5);
    QCOMPARE(inv.state(), ProcInvoker::Faulted);
    // ExitStatus 进入 reason：正常退出与崩溃可区分
    QVERIFY(diedSpy.first()[0].toString().contains(QStringLiteral("status=NormalExit")));

    QTest::qWait(300); // 超过两个重启周期，确认重启已停止
    QCOMPARE(diedSpy.count(), 6);
    QCOMPARE(restartedSpy.count(), 5);
}

void TestInvoker::enqueueWhileFaultedFailsImmediately()
{
    // D3 回归：FailedToStart 后无重启计划，入队命令立即收到 ProcessDied，不悬死
    ProcInvoker inv;
    inv.setProgram(QStringLiteral("/nonexistent/definitely-missing-procinvoker-binary"));
    QVERIFY(inv.start());
    QTRY_COMPARE_WITH_TIMEOUT(inv.state(), ProcInvoker::Faulted, 5000);

    QList<Result> results;
    inv.registerCommand(makeCmd(QStringLiteral("print x"),
                                [&](const Result &r) { results.append(r); }));
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 2000);
    QCOMPARE(results[0].status, ProcInvoker::Status::ProcessDied);
    QCOMPARE(inv.state(), ProcInvoker::Faulted);
}

void TestInvoker::enqueueWhileStoppedWarnsButQueues()
{
    // D3：Stopped 未启动时入队——告警提示，但命令滞留排队是有意设计
    // （register-before-start 合法），start() 后正常执行
    WarningCapture cap;
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));

    QList<Result> results;
    inv.registerCommand(makeCmd(QStringLiteral("print x"),
                                [&](const Result &r) { results.append(r); }));
    QTRY_VERIFY_WITH_TIMEOUT(
        WarningCapture::contains(QStringLiteral("process not running")), 2000);
    QCOMPARE(results.size(), 0); // 滞留排队，不失败

    QVERIFY(inv.start());
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 5000);
    QCOMPARE(results[0].status, ProcInvoker::Status::Ok);
    QCOMPARE(results[0].text, QStringLiteral("x"));
}

void TestInvoker::setMarkerMidFlightDeferred()
{
    // D1 回归：在途命令期间 setMarker 只记录新值，在途命令仍按旧标记结束；
    // 下一条命令起用新标记（与 promptMode 闩锁对称）
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    startAndWaitIdle(&inv);

    QList<Result> results1;
    inv.registerCommand(makeCmd(QStringLiteral("slow 300 x"),
                                [&](const Result &r) { results1.append(r); },
                                {}, ProcInvoker::ExpectResult, 2000));
    inv.setMarker(QStringLiteral("@@NEWMARK@@")); // queued 于命令之后：命令已按旧标记启动

    QTRY_COMPARE_WITH_TIMEOUT(results1.size(), 1, 5000);
    QCOMPARE(results1[0].status, ProcInvoker::Status::Ok); // 若即时替换则旧标记永不匹配 → Timeout
    QCOMPARE(results1[0].text, QStringLiteral("x"));

    QList<Result> results2;
    inv.registerCommand(makeCmd(QStringLiteral("print y"),
                                [&](const Result &r) { results2.append(r); }));
    QTRY_COMPARE_WITH_TIMEOUT(results2.size(), 1, 5000);
    QCOMPARE(results2[0].status, ProcInvoker::Status::Ok); // 新标记正常往返
    QCOMPARE(results2[0].text, QStringLiteral("y"));
}

void TestInvoker::setCodecMidFlightDeferred()
{
    // D1 回归：在途命令期间 setCodec 推迟到命令结束再重建；在途命令仍按旧编码解码。
    // 非 ASCII 内容在 UTF-8 与 Latin-1 下解码结果不同，可区分是否闩锁
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    startAndWaitIdle(&inv);

    const QString nonAscii = QString::fromUtf8("h\xc3\xa9llo"); // "héllo"
    QList<Result> results1;
    inv.registerCommand(makeCmd(QStringLiteral("print ") + nonAscii,
                                [&](const Result &r) { results1.append(r); },
                                {}, ProcInvoker::ExpectResult, 2000));
    inv.setCodec("ISO-8859-1"); // queued 于命令之后：命令已按 UTF-8 启动

    QTRY_COMPARE_WITH_TIMEOUT(results1.size(), 1, 5000);
    QCOMPARE(results1[0].status, ProcInvoker::Status::Ok);
    QCOMPARE(results1[0].text, nonAscii); // 若即时重建则按 Latin-1 解码成乱码

    QList<Result> results2;
    inv.registerCommand(makeCmd(QStringLiteral("print y"),
                                [&](const Result &r) { results2.append(r); }));
    QTRY_COMPARE_WITH_TIMEOUT(results2.size(), 1, 5000);
    QCOMPARE(results2[0].status, ProcInvoker::Status::Ok); // 新编码下 ASCII 内容正常往返
    QCOMPARE(results2[0].text, QStringLiteral("y"));
}

void TestInvoker::processDiedKeepsPartialOutput()
{
    // P1：进程死亡时，在途 CollectAll 命令的 ProcessDied 结果带已收集的部分输出；
    // 顺带断言 reason 带 exitCode 与 ExitStatus
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    inv.setRestartDelayMs(-1);
    startAndWaitIdle(&inv);

    QSignalSpy diedSpy(&inv, &ProcInvoker::processDied);
    QList<Result> results;
    // printcrash：打印一行后退出，探针标记永远等不到 → 进程死亡时已有部分输出
    inv.registerCommand(makeCmd(QStringLiteral("printcrash p1"),
                                [&](const Result &r) { results.append(r); }, {},
                                ProcInvoker::ExpectResult | ProcInvoker::CollectAll));

    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 5000);
    QCOMPARE(results[0].status, ProcInvoker::Status::ProcessDied);
    QCOMPARE(results[0].text, QStringLiteral("p1")); // 部分输出不丢弃
    QTRY_VERIFY_WITH_TIMEOUT(diedSpy.count() >= 1, 2000);
    // reason 带 exitCode 与 ExitStatus（exit(1) 属正常退出；CrashExit 仅用于信号死亡）
    QVERIFY(diedSpy.first()[0].toString().contains(
        QStringLiteral("exitCode=1, status=NormalExit")));
}

void TestInvoker::stderrLongLineCap()
{
    // stderr 无换行长行超过 64KB：强制冲刷为一条 stderrLine，防内存膨胀。
    // 首条冲刷在缓冲首次越限时触发（>64KB 且 ≤ 总量），残余无换行则留在缓冲
    ProcInvoker inv;
    inv.setProgram(QStringLiteral(FIXTURE_PATH));
    inv.setMarker(TEST_MARKER);
    inv.setProbeCommand(QStringLiteral("emitmark %1"));
    startAndWaitIdle(&inv);

    QSignalSpy stderrSpy(&inv, &ProcInvoker::stderrReceived);
    const QString big(100000, QLatin1Char('a'));
    inv.registerCommand(makeCmd(QStringLiteral("errnonl ") + big, {}, {},
                                ProcInvoker::FireAndForget));

    QTRY_VERIFY_WITH_TIMEOUT(stderrSpy.count() >= 1, 5000);
    const int flushed = stderrSpy[0][0].toString().size();
    QVERIFY(flushed > 64 * 1024);
    QVERIFY(flushed <= big.size());
}

QTEST_GUILESS_MAIN(TestInvoker)
#include "tst_invoker.moc"
