/* ============================================================================
 * zh_OS 1.0 (x86_64)
 * File:    shell/cmd_user.c
 * Version: 1.0
 * Brief:   M1 用户态：ring3 命令（进入用户态做特权级自检）。
 * ============================================================================ */

#include "cmd_user.h"

#include "shell.h"
#include "../kernel/usermode.h"

/*
 * ring3 —— 一次性验收入口。
 *
 * 它把 CPU 降到 ring 3 执行内核现场拼出来的一段机器码；那段代码先往 VGA
 * 显存第 24 行写 "RING3 RUNNING"，再执行 cli 触发 #GP。
 * 内核异常处理随后打印 rip / cs：cs = 0x001b 就是"确实来自 ring 3"的证据。
 */
int cmd_ring3(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    usermode_self_test();

    /* 正常路径不会返回到这里（iretq 之后必然在内核异常处理里停机）。 */
    err("ring3: unexpected return from user mode");
    return SHELL_CONTINUE;
}
