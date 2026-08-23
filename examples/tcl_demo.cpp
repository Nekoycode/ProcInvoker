// tcl_demo — ProcInvoker 驱动真实 Tcl 程序（calc.tcl）的端到端示例。
// 覆盖：ExpectResult 往返、CollectAll、流式 onMessage、stderr 转发、FireAndForget、顺序注册。

#include <QCoreApplication>
#include <QDebug>
#include <QTimer>

#include <procinvoker/ProcInvoker.h>

#ifndef CALC_TCL_PATH
#define CALC_TCL_PATH "calc.tcl"
#endif
#ifndef EMBEDDED_HOST_PATH
#define EMBEDDED_HOST_PATH "embedded_host"
#endif
#ifndef CALC_PROCS_PATH
#define CALC_PROCS_PATH "calc_procs.tcl"
#endif

static const char *statusName(ProcInvoker::Status s)
{
    switch (s) {
    case ProcInvoker::Status::Ok:          return "Ok";
    case ProcInvoker::Status::Timeout:     return "Timeout";
    case ProcInvoker::Status::ProcessDied: return "ProcessDied";
    case ProcInvoker::Status::WriteError:  return "WriteError";
    case ProcInvoker::Status::Cancelled:   return "Cancelled";
    }
    return "?";
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    ProcInvoker inv;
    // 默认驱动 tclsh 跑 calc.tcl；--embedded 改为驱动嵌入式 Tcl 宿主（形态 B），
    // 两种形态对调用器完全透明——协议配置一行不变
    if (app.arguments().contains(QStringLiteral("--embedded")))
        inv.setProgram(QStringLiteral(EMBEDDED_HOST_PATH), {QStringLiteral(CALC_PROCS_PATH)});
    else
        inv.setProgram(QStringLiteral("tclsh"), {QStringLiteral(CALC_TCL_PATH)});

    QObject::connect(&inv, &ProcInvoker::stderrReceived, [](const QString &line) {
        qInfo().noquote() << "[stderr ]" << line;
    });
    QObject::connect(&inv, &ProcInvoker::processDied, [](const QString &reason) {
        qInfo().noquote() << "[died   ]" << reason;
    });
    QObject::connect(&inv, &ProcInvoker::restarted, [] {
        qInfo().noquote() << "[restart] 进程已自动重启";
    });

    if (!inv.start()) {
        qWarning() << "start 失败：未设置程序";
        return 1;
    }

    // 5 条命令，全部一次性顺序注册；队列保证按序执行、结果按注册顺序归属
    int pending = 5;
    auto done = [&app, &pending](const char *tag, const ProcInvoker::Result &r) {
        qInfo().noquote() << "[" << tag << "] status=" << statusName(r.status)
                          << " text=" << r.text;
        // 延迟退出：stderr 与 stdout 是两条独立管道，错误行可能比结果稍晚投递
        if (--pending == 0)
            QTimer::singleShot(300, &app, &QCoreApplication::quit);
    };

    // 1) 简单求值：CollectAll 聚合输出
    ProcInvoker::Command c1;
    c1.text  = QStringLiteral("add 3 4");
    c1.flags = ProcInvoker::ExpectResult | ProcInvoker::CollectAll;
    c1.onResult = [&done](const ProcInvoker::Result &r) { done("add    ", r); };
    inv.registerCommand(c1);

    // 2) 乘法
    ProcInvoker::Command c2;
    c2.text  = QStringLiteral("mul 6 7");
    c2.flags = ProcInvoker::ExpectResult | ProcInvoker::CollectAll;
    c2.onResult = [&done](const ProcInvoker::Result &r) { done("mul    ", r); };
    inv.registerCommand(c2);

    // 3) fib 8：不加 CollectAll，走流式 onMessage
    ProcInvoker::Command c3;
    c3.text = QStringLiteral("fib 8");
    c3.onMessage = [](const ProcInvoker::Result &r) {
        qInfo().noquote() << "[fib 流式]" << r.text;
    };
    c3.onResult = [&done](const ProcInvoker::Result &r) { done("fib    ", r); };
    inv.registerCommand(c3);

    // 4) 除零：Tcl 报错走 stderr 通道，命令本身仍正常结束
    ProcInvoker::Command c4;
    c4.text  = QStringLiteral("div 1 0");
    c4.flags = ProcInvoker::ExpectResult | ProcInvoker::CollectAll;
    c4.onResult = [&done](const ProcInvoker::Result &r) { done("div    ", r); };
    inv.registerCommand(c4);

    // 5) FireAndForget：写完即完成，不等返回（其输出无归属，仅演示队列推进）
    ProcInvoker::Command c5;
    c5.text  = QStringLiteral("puts \"fire-and-forget 已写入\"");
    c5.flags = ProcInvoker::FireAndForget;
    c5.onResult = [&done](const ProcInvoker::Result &r) { done("ff     ", r); };
    inv.registerCommand(c5);

    // 兜底：10 秒内未完成则退出（正常路径远快于此）
    QTimer::singleShot(10000, &app, &QCoreApplication::quit);

    return app.exec();
}
