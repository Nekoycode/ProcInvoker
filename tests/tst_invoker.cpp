#include "procinvoker/ProcInvoker.h"
#include "fixture_path.h"

#include <QMutex>
#include <QSignalSpy>
#include <QtTest/QtTest>

static const QString TEST_MARKER = QStringLiteral("@@TESTMARK@@");

using Cmd = ProcInvoker::Command;
using Result = ProcInvoker::Result;

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
    QCOMPARE(inv.state(), ProcInvoker::Stopped);
    QVERIFY(inv.start());
    QTRY_COMPARE_WITH_TIMEOUT(inv.state(), ProcInvoker::Idle, 5000);
    inv.stop();
    QTRY_COMPARE_WITH_TIMEOUT(inv.state(), ProcInvoker::Stopped, 5000);
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
    inv.registerCommand(makeCmd(QStringLiteral("printmulti 3 tick"),
                                [&](const Result &r) { results.append(r); },
                                [&](const Result &r) { messages.append(r); }));

    QTRY_COMPARE_WITH_TIMEOUT(messages.size(), 3, 5000);
    for (const Result &m : messages) {
        QVERIFY(m.isIntermediate);
        QCOMPARE(m.text, QStringLiteral("tick"));
        QCOMPARE(m.status, ProcInvoker::Status::Ok);
    }
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
    inv.registerCommand(makeCmd(QStringLiteral("printmulti 3 tick"),
                                [&](const Result &r) { results.append(r); },
                                [&](const Result &r) { messages.append(r); }));

    QTRY_COMPARE_WITH_TIMEOUT(messages.size(), 3, 5000);
    for (const Result &m : messages)
        QCOMPARE(m.text, QStringLiteral("tick"));
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

QTEST_GUILESS_MAIN(TestInvoker)
#include "tst_invoker.moc"
