#ifndef MARKERFRAMER_H
#define MARKERFRAMER_H

#include <QByteArray>
#include <QList>

// 分帧器：把 stdout 字节流按行切帧，并识别结束标记。
// 不依赖 QProcess，可被纯单元测试直接驱动。
//
// 标记唯一化：base 标记（setMarker）固定；每条在途命令的期望标记为 base+commandId
// （setExpectedMarker）。字节流中出现的任何 base 标记都会被提取为 token
// （base + 数字后缀）：与期望标记完全一致则判定命令结束，否则一律丢弃
// （超时命令的陈旧标记因此不会错位结束下一条命令）。
// 标记与不完整行都可能跨两次 feed() 分片到达，内部缓冲会正确处理。
class MarkerFramer
{
public:
    struct FeedResult {
        QList<QByteArray> frames; // 完整行（不含换行符）；匹配标记前未换行的尾部也会冲刷为帧
        bool markerFound = false; // 期望标记命中
    };

    MarkerFramer() = default;
    explicit MarkerFramer(const QByteArray &marker) : m_marker(marker) {}

    void setMarker(const QByteArray &marker) { m_marker = marker; }
    QByteArray marker() const { return m_marker; }

    // 空表示无在途命令：所有标记 token 一律丢弃
    void setExpectedMarker(const QByteArray &expected) { m_expected = expected; }
    QByteArray expectedMarker() const { return m_expected; }

    FeedResult feed(const QByteArray &data);
    void reset()
    {
        m_buffer.clear();
        m_skipLineTerminator = false;
    }

private:
    QByteArray m_marker;   // base 标记
    QByteArray m_expected; // 当前期望标记（base + commandId），可为空
    QByteArray m_buffer;
    // 标记恰好在缓冲末尾被消费、其行尾尚未到达时置位，下一次 feed() 开头吞掉一个 \r?\n
    bool m_skipLineTerminator = false;
};

#endif // MARKERFRAMER_H
