/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    shell/cmd_fd.h
 * Version: 0.2beta
 * Brief:   M9 fd 命令：把 /bin/fddemo.elf 装成进程，让它自己 open/read/close。
 * ============================================================================ */

#ifndef ZH_OS_CMD_FD_H
#define ZH_OS_CMD_FD_H

int cmd_fd(int argc, char **argv);

#endif /* ZH_OS_CMD_FD_H */
