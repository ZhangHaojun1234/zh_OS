/* ============================================================================
 * zh_OS 1.0 (x86_64)
 * File:    kernel/history.c
 * Version: 1.0
 * Brief:   16 条命令历史环与上下浏览游标实现。
 * ============================================================================ */

#include "history.h"

/*
 * 存储：hist[HIST_MAX][HIST_LINE_MAX] 当作环形数组，head 指向下一条写入位置，
 * count 为当前条数（<=16）。逻辑序号 0 表示最旧、count-1 表示最新：
 *     物理下标 = (head + HIST_MAX - count + 逻辑序号) % HIST_MAX
 *
 * 游标 cursor 的取值范围 0..count：
 *   cursor == count  -> 正在编辑"新的当前输入行"（不属于历史）
 *   cursor 在 0..count-1 -> 正在浏览某条历史
 * history_reset_cursor() 每次开始新输入时把 cursor 置回 count。
 */

static char hist[HIST_MAX][HIST_LINE_MAX];
static uint32_t hist_count;
static uint32_t hist_head;
static int32_t hist_cursor;

static uint32_t index_of(int32_t logical)
{
    return (hist_head + (uint32_t)HIST_MAX - hist_count + (uint32_t)logical)
           % (uint32_t)HIST_MAX;
}

static int str_equal(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

void history_init(void)
{
    hist_count = 0u;
    hist_head = 0u;
    hist_cursor = 0;
}

void history_add(const char *line)
{
    uint32_t n = 0;

    /* 空行不进入历史 */
    if (line == 0 || line[0] == '\0') {
        return;
    }

    /* 连续重复的上一条命令不重复记录 */
    if (hist_count > 0u) {
        const char *last = hist[index_of((int32_t)hist_count - 1)];
        if (str_equal(last, line)) {
            return;
        }
    }

    while (line[n] != '\0' && n + 1u < (uint32_t)HIST_LINE_MAX) {
        hist[hist_head][n] = line[n];
        ++n;
    }
    hist[hist_head][n] = '\0';

    hist_head = (hist_head + 1u) % (uint32_t)HIST_MAX;
    if (hist_count < (uint32_t)HIST_MAX) {
        ++hist_count;
    }
}

const char *history_prev(void)
{
    if (hist_count == 0u) {
        return 0;
    }
    if (hist_cursor > 0) {
        --hist_cursor;   /* 已在最旧(0)时保持 0，即"返回最旧" */
    }
    if (hist_cursor >= (int32_t)hist_count) {
        hist_cursor = (int32_t)hist_count - 1;
    }
    return hist[index_of(hist_cursor)];
}

const char *history_next(void)
{
    if (hist_count == 0u) {
        return 0;
    }
    if (hist_cursor >= (int32_t)hist_count) {
        return 0;       /* 已经回到最新 / 当前输入行 */
    }
    ++hist_cursor;
    if (hist_cursor >= (int32_t)hist_count) {
        return 0;       /* 越过最新，交给调用方恢复空行 */
    }
    return hist[index_of(hist_cursor)];
}

void history_reset_cursor(void)
{
    hist_cursor = (int32_t)hist_count;
}
