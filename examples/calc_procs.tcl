# calc_procs.tcl — 演示命令集（被 calc.tcl 与 embedded_host 共用的唯一事实源）
proc add {a b} { expr {$a + $b} }
proc mul {a b} { expr {$a * $b} }
proc div {a b} { expr {$a / $b} }   ;# 整数除法，除零抛错（double 除零按 IEEE 得 Inf，不抛错）

# 边算边打印中间步骤，用于演示流式 onMessage
proc fib {n} {
    set a 0
    set b 1
    for {set i 0} {$i < $n} {incr i} {
        puts "fib($i) = $a"
        set next [expr {$a + $b}]
        set a $b
        set b $next
    }
    return $a
}
