#!/usr/bin/env tclsh
# calc.tcl — ProcInvoker 演示用被调程序：stdin 逐行读 Tcl 命令并 eval，结果打印到 stdout。
# 调用器的探针（puts "<marker><id>"）也是合法 Tcl，经 eval 后原样打印标记，协议天然兼容。

# 关键：stdout 经管道连接时是块缓冲，必须切行缓冲，否则调用器迟迟收不到输出
fconfigure stdout -buffering line
fconfigure stderr -buffering line

source [file join [file dirname [info script]] calc_procs.tcl]

while {[gets stdin line] >= 0} {
    if {[string trim $line] eq ""} continue
    if {[catch {eval $line} result]} {
        puts stderr "ERROR: $result"          ;# 命令出错 → stderr 通道
    } elseif {$result ne ""} {
        puts $result                          ;# 有返回值 → 打印结果
    }
}
