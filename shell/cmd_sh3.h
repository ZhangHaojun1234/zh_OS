/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    shell/cmd_sh3.h
 * Version: 0.2beta
 * Brief:   M13 `sh3` 命令：把 /bin/zhsh3.elf 当成一个进程跑起来（它就是 ring3 shell）。
 * ============================================================================ */

#ifndef ZH_OS_CMD_SH3_H
#define ZH_OS_CMD_SH3_H

int cmd_sh3(int argc, char **argv);

#endif /* ZH_OS_CMD_SH3_H */
