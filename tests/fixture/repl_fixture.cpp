// ProcInvoker 测试用 fixture 子进程：行协议 REPL，不依赖 Qt。
// 指令：
//   print <text>          原样打印一行
//   printmulti <n> <text> 打印 n 行
//   emitmark <text>       原样打印 text（probe 打印期望标记 marker+id）
//   emitmark              打印默认结束标记 @@TESTMARK@@
//   slow <ms> <text>      延迟 ms 后打印
//   err <text>            打印到 stderr
//   nop                   不产生任何输出
//   crash                 立即 exit(1)
//   其他输入              原样回显一行
// 提示符模式：以 `--prompt <str>` 启动时，启动即打印一次提示符（不换行），
// 之后每处理完一行命令再打印一次提示符。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

static std::string g_prompt; // 空 = 标记模式（默认）

static void outLine(const std::string &s)
{
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

static void printPrompt()
{
    if (g_prompt.empty())
        return;
    std::fwrite(g_prompt.data(), 1, g_prompt.size(), stdout); // 不换行
    std::fflush(stdout);
}

int main(int argc, char **argv)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--prompt") == 0)
            g_prompt = argv[i + 1];
    }
    printPrompt(); // 启动提示符

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (line.compare(0, 6, "print ") == 0) {
            outLine(line.substr(6));
        } else if (line.compare(0, 11, "printmulti ") == 0) {
            const std::string rest = line.substr(11);
            const auto sp = rest.find(' ');
            const int n = std::atoi(rest.substr(0, sp).c_str());
            const std::string text = sp == std::string::npos ? std::string() : rest.substr(sp + 1);
            for (int i = 0; i < n; ++i)
                outLine(text);
        } else if (line.compare(0, 9, "emitmark ") == 0) {
            outLine(line.substr(9)); // 原样打印期望标记（probeCommand 经 %1 注入 marker+id）
        } else if (line == "emitmark") {
            outLine("@@TESTMARK@@");
        } else if (line.compare(0, 5, "slow ") == 0) {
            const std::string rest = line.substr(5);
            const auto sp = rest.find(' ');
            const int ms = std::atoi(rest.substr(0, sp).c_str());
            const std::string text = sp == std::string::npos ? std::string() : rest.substr(sp + 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            outLine(text);
        } else if (line.compare(0, 4, "err ") == 0) {
            const std::string text = line.substr(4);
            std::fwrite(text.data(), 1, text.size(), stderr);
            std::fputc('\n', stderr);
            std::fflush(stderr);
        } else if (line == "nop") {
            // 无输出
        } else if (line == "crash") {
            std::fflush(stdout);
            std::exit(1);
        } else {
            outLine(line);
        }
        printPrompt(); // 每处理完一行命令打印一次提示符
    }
    return 0;
}
