/* cmd_syscall.c - M2 的 syscall 命令：进用户态走 int 0x80 再回内核 */

#include "cmd_syscall.h"

#include "shell.h"
#include "../kernel/syscall.h"

/*
 * syscall ， M2 的验收命令。
 *
 * 和 `ring3` 的区别：`ring3` 是"只能进、出不来"（M1，靠 #PF 停机证明特权级）；
 * 本命令是双向的：ring3 通过 int 0x80 让内核替它打印、拿回 tick 与 cs，
 * 最后 sys_exit 把控制权干干净净地交还 Shell，命令返回后提示符照常出现。
 */
int cmd_syscall(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    syscall_self_test();

    return SHELL_CONTINUE;
}
