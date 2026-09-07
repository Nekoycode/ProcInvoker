#include "PromptFramer.h"

#include <QTextCodec>

void PromptFramer::setPattern(const QString &pattern)
{
    // 注意：pattern 不得能匹配实际提示符的真前缀（如 "% ?" 能匹配 "%"），
    // 否则提示符跨分片到达时会在前缀处提前误判
    if (pattern.isEmpty()) {
        m_re = QRegularExpression();
        return;
    }
    // 锚定流尾，避免输出中间的类似文本误判
    m_re = QRegularExpression(QStringLiteral("(?:") + pattern + QStringLiteral(")\\z"));
    if (!m_re.isValid())
        qWarning("PromptFramer: invalid prompt pattern, framer disabled");
}

PromptFramer::FeedResult PromptFramer::feed(const QByteArray &data)
{
    FeedResult out;
    m_buffer += data;

    // 完整行永远是内容，立即切出
    int lineStart = 0;
    while (true) {
        const int nl = m_buffer.indexOf('\n', lineStart);
        if (nl < 0)
            break;
        QByteArray line = m_buffer.mid(lineStart, nl - lineStart);
        if (line.endsWith('\r'))
            line.chop(1);
        out.frames.append(line);
        lineStart = nl + 1;
    }
    m_buffer = m_buffer.mid(lineStart);

    // 尾段（可能跨分片，不完整时保留等待）检查是否以提示符结尾
    if (isActive() && !m_buffer.isEmpty()) {
        const auto m = m_re.match(m_codec ? m_codec->toUnicode(m_buffer)
                                          : QString::fromUtf8(m_buffer));
        if (m.hasMatch()) {
            // 以匹配文本的编码后字节长度剥除，避免字符/字节偏移不一致
            const int promptBytes = m_codec ? m_codec->fromUnicode(m.captured()).size()
                                            : m.captured().toUtf8().size();
            QByteArray tail = m_buffer.left(m_buffer.size() - promptBytes);
            if (tail.endsWith('\r'))
                tail.chop(1);
            if (!tail.isEmpty())
                out.frames.append(tail);
            out.promptFound = true;
            m_buffer.clear();
        }
    }
    // 尾段上限：无提示符匹配且超过 1MB 时冲刷为帧并清空，
    // 防对端永不换行/永不匹配导致内存膨胀与反复全量匹配的 O(n²)
    static constexpr int kMaxTailBuffer = 1024 * 1024;
    if (m_buffer.size() > kMaxTailBuffer) {
        out.frames.append(m_buffer);
        m_buffer.clear();
    }
    return out;
}
