#ifndef PROMPTFRAMER_H
#define PROMPTFRAMER_H

#include <QByteArray>
#include <QList>
#include <QRegularExpression>

class QTextCodec;

// 提示符分帧器：提示符模式的兜底，用于无法注入标记的封闭 REPL。
// 判定规则：输出流尾部（最后一个换行之后的不完整尾段）匹配提示符正则即命令结束，
// 提示符文本被剥除、不作为帧投递。完整行永远是内容（提示符不以换行结尾，
// 因此完整行内类似提示符的文本不会误判）。
// 尾部分片安全：尾段不匹配时保留等待更多数据，不会因部分前缀误判/漏判。
// 尾段超过 1MB 仍无匹配时冲刷为帧并清空（防内存 DoS 与反复全量匹配的 O(n²)）。
class PromptFramer
{
public:
    struct FeedResult {
        QList<QByteArray> frames; // 完整行（不含换行符）；提示符前的残余尾部也会冲刷为帧
        bool promptFound = false;
    };

    PromptFramer() = default;
    explicit PromptFramer(const QString &pattern) { setPattern(pattern); }

    // pattern 为空则停用（framer 只做行分帧，永不判定提示符）
    void setPattern(const QString &pattern);
    bool isActive() const { return m_re.isValid() && !m_re.pattern().isEmpty(); }

    // 与 ProcInvoker::setCodec 同步；nullptr = UTF-8
    void setCodec(QTextCodec *codec) { m_codec = codec; }

    FeedResult feed(const QByteArray &data);
    void reset() { m_buffer.clear(); }

private:
    QRegularExpression m_re; // 已包为 (?:pattern)\z，锚定流尾
    QByteArray m_buffer;     // 未换行的尾段
    QTextCodec *m_codec = nullptr;
};

#endif // PROMPTFRAMER_H
