// batch_demo — 企业批量命令执行示例：步骤声明式定义，逐条执行并处理结果，
// 后续步骤可依赖前序步骤解析出的数据；任一步骤失败则中止批次。
//
// 核心模式：命令间有依赖时，在前一条命令的 onResult 回调中解析输出、
// 构造并注册下一条。回调运行在注册线程（此处即主线程），队列严格 FIFO，
// 因此该模式天然线程安全且顺序确定。
//
// 用法: batch_demo [--fail]   --fail 注入一个会产生非数字结果的步骤，演示中止路径

#include <QCoreApplication>
#include <QDebug>
#include <QMap>
#include <QTimer>

#include <functional>

#include <procinvoker/ProcInvoker.h>

#ifndef CALC_TCL_PATH
#define CALC_TCL_PATH "calc.tcl"
#endif

// 批量步骤：命令文本可由前序步骤解析出的上下文（ctx）构造；
// handle 返回 false 表示该步骤处理失败，中止整个批次。
struct Step {
    QString name;
    // 默认需要结果（ExpectResult + CollectAll）；通知类步骤用 FireAndForget
    ProcInvoker::CommandFlags flags = ProcInvoker::ExpectResult | ProcInvoker::CollectAll;
    std::function<QString(const QMap<QString, QString> &ctx)> makeCommand;
    std::function<bool(const ProcInvoker::Result &r, QMap<QString, QString> &ctx)> handle;
};

// 常用处理器：期望结果是一个数字，存入 ctx[key]
static bool storeNumber(const ProcInvoker::Result &r, QMap<QString, QString> &ctx,
                        const QString &key)
{
    if (r.status != ProcInvoker::Status::Ok) {
        qWarning() << "  命令失败, status =" << int(r.status);
        return false;
    }
    bool ok = false;
    const double v = r.text.trimmed().toDouble(&ok);
    if (!ok) {
        qWarning() << "  解析失败: 期望数字, 实际内容:" << r.text;
        return false;
    }
    ctx[key] = QString::number(v);
    return true;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const bool injectFailure = app.arguments().contains(QStringLiteral("--fail"));

    ProcInvoker inv;
    inv.setProgram(QStringLiteral("tclsh"), {QStringLiteral(CALC_TCL_PATH)});
    QObject::connect(&inv, &ProcInvoker::stderrReceived,
                     [](const QString &l) { qWarning().noquote() << "[stderr]" << l; });
    inv.start();

    // ---- 批量任务定义：一个真实的"先查基数 → 叠加偏移 → 换算 → 通知"流水线 ----
    QList<Step> steps;

    steps.append({QStringLiteral("查询基数"),
                  {}, // 默认 ExpectResult | CollectAll
                  [](const QMap<QString, QString> &) { return QStringLiteral("mul 6 7"); },
                  [](const ProcInvoker::Result &r, QMap<QString, QString> &ctx) {
                      return storeNumber(r, ctx, QStringLiteral("base"));
                  }});

    if (injectFailure) // 演示中止路径：该步骤结果不是数字，storeNumber 将失败
        steps.append({QStringLiteral("读取配置(将被污染)"),
                      {},
                      [](const QMap<QString, QString> &) { return QStringLiteral("expr {abc}"); },
                      [](const ProcInvoker::Result &r, QMap<QString, QString> &ctx) {
                          return storeNumber(r, ctx, QStringLiteral("conf"));
                      }});

    steps.append({QStringLiteral("叠加偏移"), // 依赖 step1 解析出的 base
                  {},
                  [](const QMap<QString, QString> &ctx) {
                      return QStringLiteral("add %1 8").arg(ctx[QStringLiteral("base")]);
                  },
                  [](const ProcInvoker::Result &r, QMap<QString, QString> &ctx) {
                      return storeNumber(r, ctx, QStringLiteral("shifted"));
                  }});

    steps.append({QStringLiteral("换算比例"), // 依赖 step2 的 shifted
                  {},
                  [](const QMap<QString, QString> &ctx) {
                      return QStringLiteral("div 100 %1").arg(ctx[QStringLiteral("shifted")]);
                  },
                  [](const ProcInvoker::Result &r, QMap<QString, QString> &ctx) {
                      return storeNumber(r, ctx, QStringLiteral("ratio"));
                  }});

    steps.append({QStringLiteral("通知监控"), // 即发即弃：不等待、不占队列窗口
                  {ProcInvoker::FireAndForget},
                  [](const QMap<QString, QString> &ctx) {
                      return QStringLiteral("puts \"[monitor] batch ratio=%1\"")
                          .arg(ctx[QStringLiteral("ratio")]);
                  },
                  {}});

    // ---- 批次执行器：逐步注册，依赖步骤在 onResult 中链式推进 ----
    QMap<QString, QString> ctx;
    int current = 0;
    std::function<void()> runNext;

    runNext = [&]() {
        if (current >= steps.size()) {
            qInfo().noquote() << "批次完成。上下文:" << ctx;
            app.quit();
            return;
        }
        const Step &step = steps.at(current);
        const int stepNo = current + 1;

        ProcInvoker::Command cmd;
        cmd.text = step.makeCommand(ctx);
        cmd.flags = step.flags;
        cmd.timeoutMs = 5000;
        qInfo().noquote() << QStringLiteral("[%1/%2] %3: %4")
                                 .arg(stepNo).arg(steps.size()).arg(step.name, cmd.text);

        cmd.onResult = [&, step, stepNo](const ProcInvoker::Result &r) {
            ++current;
            if (step.handle && !step.handle(r, ctx)) {
                qWarning().noquote() << QStringLiteral("批次中止于步骤 %1: %2")
                                            .arg(stepNo).arg(step.name);
                // 延迟退出：stderr 是独立管道，错误明细可能比结果稍晚投递
                QTimer::singleShot(300, &app, [&app] { app.exit(2); });
                return;
            }
            if (r.status == ProcInvoker::Status::Ok && !r.text.isEmpty())
                qInfo().noquote() << "  =>" << r.text;
            runNext(); // 链式推进：依赖已解析进 ctx，构造并注册下一步
        };
        inv.registerCommand(cmd);
    };

    runNext();
    QTimer::singleShot(30000, &app, [] { qFatal("批次整体超时"); });
    return app.exec();
}
