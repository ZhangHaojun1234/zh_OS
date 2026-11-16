/* ============================================================================
 * zh_OS 1.0 (x86_64)
 * File:    kernel/history.h
 * Version: 1.0
 * Brief:   Shell 命令历史环：16 条、每条最多 128 字符。
 * ============================================================================ */

#ifndef ZH_OS_KERNEL_HISTORY_H
#define ZH_OS_KERNEL_HISTORY_H

#include <stdint.h>

#define HIST_MAX      16
#define HIST_LINE_MAX 128

/* 初始化（清空历史、把游标放到"当前输入行"位置）。幂等。 */
void history_init(void);

/* 记录一条命令：空行不入历史，与上一条完全相同也不重复入。满 16 条后覆盖最旧。 */
void history_add(const char *line);

/* 向上浏览：返回上一条；已在最旧时仍返回最旧；没有历史返回 NULL。 */
const char *history_prev(void);

/* 向下浏览：返回下一条；回到最新（当前输入行）时返回 NULL，用于恢复空行。 */
const char *history_next(void);

/* 开始一次新输入前调用：把浏览游标移回最新之后。 */
void history_reset_cursor(void);

#endif /* ZH_OS_KERNEL_HISTORY_H */
