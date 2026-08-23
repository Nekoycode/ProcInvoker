#include "MarkerFramer.h"

static bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

MarkerFramer::FeedResult MarkerFramer::feed(const QByteArray &data)
{
    FeedResult out;

    QByteArray input = data;
    if (m_skipLineTerminator) {
        // 上一次消费的标记行尾现在才到达：吞掉一个 \r?\n，避免切出空帧
        if (input.startsWith('\r'))
            input.remove(0, 1);
        if (input.startsWith('\n')) {
            input.remove(0, 1);
            m_skipLineTerminator = false;
        } else if (!input.isEmpty()) {
            m_skipLineTerminator = false; // 标记后本无换行，取消吞并
        }
    }
    m_buffer += input;

    int pos = 0; // m_buffer 内的解析游标
    while (true) {
        const int baseIdx = m_marker.isEmpty() ? -1 : m_buffer.indexOf(m_marker, pos);
        const int end = (baseIdx >= 0) ? baseIdx : m_buffer.size();

        // 抽取 [pos, end) 内的完整行
        int lineStart = pos;
        while (true) {
            const int nl = m_buffer.indexOf('\n', lineStart);
            if (nl < 0 || nl >= end)
                break;
            QByteArray line = m_buffer.mid(lineStart, nl - lineStart);
            if (line.endsWith('\r'))
                line.chop(1);
            out.frames.append(line);
            lineStart = nl + 1;
        }

        if (baseIdx < 0) {
            // 保留未完成部分：可能是半行，也可能含有跨缓冲区的标记前缀
            m_buffer = m_buffer.mid(lineStart);
            return out;
        }

        // 提取候选标记 token：base + 数字 id 后缀
        int tokenEnd = baseIdx + m_marker.size();
        while (tokenEnd < m_buffer.size() && isDigit(m_buffer.at(tokenEnd)))
            ++tokenEnd;
        const QByteArray token = m_buffer.mid(baseIdx, tokenEnd - baseIdx);

        if (tokenEnd == m_buffer.size() && token != m_expected
            && !m_expected.isEmpty() && m_expected.startsWith(token)) {
            // token 到达缓冲末尾但可能尚未完整（后续还可能跟数字），
            // 且是期望标记的真前缀：保留等待后续分片
            m_buffer = m_buffer.mid(lineStart);
            return out;
        }

        if (!m_expected.isEmpty() && token == m_expected) {
            // 匹配标记前未换行的数据冲刷为帧，归入当前命令
            QByteArray tail = m_buffer.mid(lineStart, baseIdx - lineStart);
            if (tail.endsWith('\r'))
                tail.chop(1);
            if (!tail.isEmpty())
                out.frames.append(tail);
            out.markerFound = true;
        }
        // 不匹配的 token（陈旧/无关标记）连同其前的无主残行一起丢弃

        pos = tokenEnd;
        // 跳过标记自身所在行的行尾，避免产生空帧
        bool ateTerminator = false;
        if (pos < m_buffer.size() && m_buffer.at(pos) == '\r')
            ++pos;
        if (pos < m_buffer.size() && m_buffer.at(pos) == '\n') {
            ++pos;
            ateTerminator = true;
        }
        if (!ateTerminator && pos >= m_buffer.size())
            m_skipLineTerminator = true; // 行尾跨分片，下次 feed() 开头吞掉
    }
}
