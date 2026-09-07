#include "MarkerFramer.h"

#include <QtTest/QtTest>

static const QByteArray MARK = "\x1d" "DONE" "\x1d"; // base 标记
static const QByteArray FULL = MARK + "7";            // 期望标记 = base + commandId

// 构造 base=MARK、期望=FULL 的 framer
static MarkerFramer makeFramer()
{
    MarkerFramer f(MARK);
    f.setExpectedMarker(FULL);
    return f;
}

class TestFramer : public QObject
{
    Q_OBJECT
private slots:
    void completeLines();
    void lineSplitAcrossFeeds();
    void markerAfterLine();
    void markerInlineFlushesPartial();
    void markerSplitAcrossFeeds();
    void markerSplitWithData();
    void multipleMarkers();
    void crlfStripped();
    void markerLineTerminatorNotEmitted();
    void terminatorSplitAcrossFeeds();
    void wrongIdMarkerDropped();
    void bareMarkerDropped();
    void markerDroppedWhenNoExpected();
    void resetClearsExpected();
};

void TestFramer::completeLines()
{
    MarkerFramer f = makeFramer();
    const auto r = f.feed("hello\nworld\n");
    QVERIFY(!r.markerFound);
    QCOMPARE(r.frames.size(), 2);
    QCOMPARE(r.frames[0], QByteArray("hello"));
    QCOMPARE(r.frames[1], QByteArray("world"));
}

void TestFramer::lineSplitAcrossFeeds()
{
    MarkerFramer f = makeFramer();
    auto r = f.feed("hel");
    QVERIFY(r.frames.isEmpty());
    QVERIFY(!r.markerFound);
    r = f.feed("lo\n");
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.frames[0], QByteArray("hello"));
}

void TestFramer::markerAfterLine()
{
    MarkerFramer f = makeFramer();
    const auto r = f.feed(QByteArray("out\n") + FULL + "\n");
    QVERIFY(r.markerFound);
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.frames[0], QByteArray("out"));
}

void TestFramer::markerInlineFlushesPartial()
{
    // 标记紧跟未换行的数据：尾部被冲刷为帧，归入当前命令
    MarkerFramer f = makeFramer();
    const auto r = f.feed(QByteArray("data") + FULL);
    QVERIFY(r.markerFound);
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.frames[0], QByteArray("data"));
}

void TestFramer::markerSplitAcrossFeeds()
{
    MarkerFramer f = makeFramer();
    auto r = f.feed(FULL.left(3)); // 标记前缀单独到达 → 保留等待
    QVERIFY(!r.markerFound);
    QVERIFY(r.frames.isEmpty());
    r = f.feed(FULL.mid(3, MARK.size() - 3)); // base 已完整但缺 id，仍是期望标记的前缀 → 继续等待
    QVERIFY(!r.markerFound);
    QVERIFY(r.frames.isEmpty());
    r = f.feed(QByteArray("7")); // id 到达，完整标记恰好在缓冲末尾 → 立即判定匹配
    QVERIFY(r.markerFound);
    QVERIFY(r.frames.isEmpty());
}

void TestFramer::markerSplitWithData()
{
    MarkerFramer f = makeFramer();
    auto r = f.feed(QByteArray("abc\nxy") + FULL.left(2));
    QVERIFY(!r.markerFound);
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.frames[0], QByteArray("abc"));
    // 标记后半到达，其后字节继续正常分帧
    r = f.feed(FULL.mid(2) + "zz\n");
    QVERIFY(r.markerFound);
    QCOMPARE(r.frames.size(), 2);
    QCOMPARE(r.frames[0], QByteArray("xy"));
    QCOMPARE(r.frames[1], QByteArray("zz"));
}

void TestFramer::multipleMarkers()
{
    MarkerFramer f = makeFramer();
    const auto r = f.feed(QByteArray("a\n") + FULL + "\n" + QByteArray("b\n") + FULL + "\n");
    QVERIFY(r.markerFound);
    QCOMPARE(r.frames.size(), 2);
    QCOMPARE(r.frames[0], QByteArray("a"));
    QCOMPARE(r.frames[1], QByteArray("b"));
}

void TestFramer::crlfStripped()
{
    MarkerFramer f = makeFramer();
    auto r = f.feed("a\r\n");
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.frames[0], QByteArray("a"));
    // 标记前冲刷的尾部同样剥 \r
    r = f.feed(QByteArray("b\r") + FULL + "\n");
    QVERIFY(r.markerFound);
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.frames[0], QByteArray("b"));
}

void TestFramer::markerLineTerminatorNotEmitted()
{
    // 标记所在行的换行符不应产生空帧
    MarkerFramer f = makeFramer();
    const auto r = f.feed(QByteArray("x\n") + FULL + "\n");
    QVERIFY(r.markerFound);
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.frames[0], QByteArray("x"));
}

void TestFramer::terminatorSplitAcrossFeeds()
{
    // 期望标记恰好在缓冲末尾被完整接收，其行尾在下一个 feed() 才到达：
    // 不得切出空帧，后续数据正常分帧
    MarkerFramer f = makeFramer();
    auto r = f.feed(QByteArray("out\n") + FULL);
    QVERIFY(r.markerFound);
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.frames[0], QByteArray("out"));

    r = f.feed("\n"); // 迟到的行尾被吞掉
    QVERIFY(!r.markerFound);
    QVERIFY(r.frames.isEmpty());

    r = f.feed("next\n");
    QVERIFY(!r.markerFound);
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.frames[0], QByteArray("next"));
}

void TestFramer::wrongIdMarkerDropped()
{
    // 陈旧标记（id 不匹配）一律丢弃，不判定结束，其所在行不产生帧
    MarkerFramer f = makeFramer();
    auto r = f.feed(QByteArray("late\n") + MARK + "3\n");
    QVERIFY(!r.markerFound);
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.frames[0], QByteArray("late"));
    // 随后的真实期望标记正常命中
    r = f.feed(QByteArray("ok\n") + FULL + "\n");
    QVERIFY(r.markerFound);
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.frames[0], QByteArray("ok"));
}

void TestFramer::bareMarkerDropped()
{
    // 裸 base 标记（无 id 后缀）同样不匹配、被丢弃
    MarkerFramer f = makeFramer();
    const auto r = f.feed(MARK + "\n");
    QVERIFY(!r.markerFound);
    QVERIFY(r.frames.isEmpty());
}

void TestFramer::markerDroppedWhenNoExpected()
{
    // 无在途命令（期望标记为空）：一切标记都丢弃，仅做行分帧
    MarkerFramer f(MARK);
    const auto r = f.feed(QByteArray("noise\n") + FULL + "\n");
    QVERIFY(!r.markerFound);
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.frames[0], QByteArray("noise"));
}

void TestFramer::resetClearsExpected()
{
    // reset() 顺带清空期望标记：复位后原期望标记按陈旧标记丢弃，不判定结束
    MarkerFramer f = makeFramer();
    f.reset();
    const auto r = f.feed(QByteArray("noise\n") + FULL + "\n");
    QVERIFY(!r.markerFound);
    QCOMPARE(r.frames.size(), 1);
    QCOMPARE(r.frames[0], QByteArray("noise"));
}

QTEST_APPLESS_MAIN(TestFramer)
#include "tst_framer.moc"
