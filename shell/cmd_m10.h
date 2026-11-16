/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    shell/cmd_m10.h
 * Version: 0.2beta
 * Brief:   M10 的两条演示命令：`wr`（可写文件）与 `spawn`（fork->exec->wait）。
 * ============================================================================ */

#ifndef ZH_OS_CMD_M10_H
#define ZH_OS_CMD_M10_H

int cmd_wr(int argc, char **argv);
int cmd_spawn(int argc, char **argv);

#endif /* ZH_OS_CMD_M10_H */
