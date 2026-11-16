/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    shell/cmd_syscall.h
 * Version: 0.2beta
 * Brief:   M2 系统调用：syscall 命令声明。
 * ============================================================================ */

#ifndef ZH_OS_CMD_SYSCALL_H
#define ZH_OS_CMD_SYSCALL_H

/* syscall：进 ring3 跑一段用 int 0x80 调内核打印的程序，最后 sys_exit 回 Shell。 */
int cmd_syscall(int argc, char **argv);

#endif /* ZH_OS_CMD_SYSCALL_H */
