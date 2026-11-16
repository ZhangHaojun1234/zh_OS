/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    shell/cmd_time.h
 * Version: 0.2beta
 * Brief:   uptime / date 两个新命令的入口声明。
 * ============================================================================ */

#ifndef ZH_OS_SHELL_CMD_TIME_H
#define ZH_OS_SHELL_CMD_TIME_H

/* 为避免改动任何已有头文件，新命令的声明放在这个新头文件里。 */
int cmd_uptime(int argc, char **argv);
int cmd_date(int argc, char **argv);

#endif /* ZH_OS_SHELL_CMD_TIME_H */
