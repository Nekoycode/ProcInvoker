/* embedded_host.c — 嵌入式 Tcl 宿主示例（形态 B：自建循环 eval stdin 行 + 手动泵 Tcl 事件）
 *
 * 演示把 Tcl 解释器集成进 C 程序时如何满足 ProcInvoker 的接入契约：
 *   1. stdout 双层行缓冲（C stdio 与 Tcl channel 各一层，漏一层调用器就会超时）
 *   2. 结果走 stdout、错误走 stderr
 *   3. 每条命令后泵 Tcl 事件（after/fileevent/vwait 类命令才不会饿死）
 *
 * 用法: embedded_host [init.tcl]   —— init.tcl 在启动时 eval，用于注册自定义命令
 */
#include <stdio.h>
#include <string.h>
#include <tcl.h>

int main(int argc, char **argv)
{
    Tcl_Interp *interp = Tcl_CreateInterp();
    if (Tcl_Init(interp) != TCL_OK) {
        fprintf(stderr, "Tcl_Init failed: %s\n", Tcl_GetStringResult(interp));
        return 1;
    }

    /* 要求 1：C stdio 侧行缓冲 */
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* 要求 1：puts 走的是 Tcl channel，有自己的缓冲，同样要行缓冲 */
    Tcl_Channel out = Tcl_GetChannel(interp, "stdout", NULL);
    Tcl_SetChannelOption(NULL, out, "-buffering", "line");

    /* 可选：启动脚本，注册自定义命令（如 calc_procs.tcl） */
    if (argc > 1 && Tcl_EvalFile(interp, argv[1]) != TCL_OK) {
        fprintf(stderr, "init script failed: %s\n", Tcl_GetStringResult(interp));
        return 1;
    }

    char line[4096];
    while (fgets(line, sizeof line, stdin)) {
        int code = Tcl_Eval(interp, line);
        const char *res = Tcl_GetStringResult(interp);
        if (code == TCL_ERROR)
            fprintf(stderr, "ERROR: %s\n", res);   /* 要求 2：错误走 stderr */
        else if (res[0] != '\0')
            printf("%s\n", res);                    /* 要求 2：结果走 stdout */
        fflush(stdout);

        /* 要求 3：泵 Tcl 事件，after/fileevent 等异步命令才能工作 */
        while (Tcl_DoOneEvent(TCL_DONT_WAIT)) {}
    }

    Tcl_DeleteInterp(interp);
    return 0;
}
