#include "PromptFramer.h"

#include <QtTest/QtTest>

static const QString PROMPT = QStringLiteral("% "); // 类 tclsh 提示符，不以换行结尾

class TestPromptFramer : public QObject
{
    Q_OBJECT
private slots:
    void promptStripped();
    void promptOnly();
    void promptSplitAcrossFeeds();
    void inlineContentBeforePrompt();
    void promptLikeTextInCompleteLine();
    void noMatchHolds();
    void dataAfterPrompt();
    void invalidPatternStaysInactive();
    void emptyPatternInactive();
};

void TestPromptFramer::promptStripped()
{
    PromptFramer f(PROMPT);
    const auto r = f.feed("hello\n% ");
    QVERIFY(r.promptFound);
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.frames[0], QByteArray("hello")); // 提示符文本被剥除
}

void TestPromptFramer::promptOnly()
{
    PromptFramer f(PROMPT);
    const auto r = f.feed("% ");
    QVERIFY(r.promptFound);
    QVERIFY(r.frames.isEmpty());
}

void TestPromptFramer::promptSplitAcrossFeeds()
{
    PromptFramer f(PROMPT);
    auto r = f.feed("hello\n%"); // 提示符前缀：保留等待，不误判
    QVERIFY(!r.promptFound);
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.frames[0], QByteArray("hello"));
    r = f.feed(" ");
    QVERIFY(r.promptFound);
    QVERIFY(r.frames.isEmpty());
}

void TestPromptFramer::inlineContentBeforePrompt()
{
    // 提示符前同行残余冲刷为帧
    PromptFramer f(PROMPT);
    const auto r = f.feed("abc% ");
    QVERIFY(r.promptFound);
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.frames[0], QByteArray("abc"));
}

void TestPromptFramer::promptLikeTextInCompleteLine()
{
    // 完整行内类似提示符的文本不误判（提示符只认流尾）
    PromptFramer f(PROMPT);
    const auto r = f.feed("progress 100% done\n% ");
    QVERIFY(r.promptFound);
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.frames[0], QByteArray("progress 100% done"));
}

void TestPromptFramer::noMatchHolds()
{
    // 不匹配的尾段保留，与后续分片拼成行
    PromptFramer f(PROMPT);
    auto r = f.feed("abc");
    QVERIFY(!r.promptFound);
    QVERIFY(r.frames.isEmpty());
    r = f.feed("def\n% ");
    QVERIFY(r.promptFound);
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.frames[0], QByteArray("abcdef"));
}

void TestPromptFramer::dataAfterPrompt()
{
    PromptFramer f(PROMPT);
    auto r = f.feed("% ");
    QVERIFY(r.promptFound);
    r = f.feed("x\ny\n% ");
    QVERIFY(r.promptFound);
    QCOMPARE(r.frames.size(), 2);
    QCOMPARE(r.frames[0], QByteArray("x"));
    QCOMPARE(r.frames[1], QByteArray("y"));
}

void TestPromptFramer::invalidPatternStaysInactive()
{
    // 无效正则：告警并停用，永不判定提示符（不会进入"半开"状态）
    QTest::ignoreMessage(QtWarningMsg, "PromptFramer: invalid prompt pattern, framer disabled");
    PromptFramer f(QStringLiteral("(unclosed"));
    QVERIFY(!f.isActive());
    const auto r = f.feed("% \n");
    QVERIFY(!r.promptFound);
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.frames[0], QByteArray("% "));
}

void TestPromptFramer::emptyPatternInactive()
{
    // 空 pattern = 停用：只做行分帧
    PromptFramer f;
    f.setPattern(QString());
    QVERIFY(!f.isActive());
    auto r = f.feed("% ");
    QVERIFY(!r.promptFound);
    QVERIFY(r.frames.isEmpty()); // 无换行尾段保留等待
    r = f.feed("\n");
    QVERIFY(!r.promptFound);
    QCOMPARE(r.frames.size(), 1);
}

QTEST_APPLESS_MAIN(TestPromptFramer)
#include "tst_promptframer.moc"
