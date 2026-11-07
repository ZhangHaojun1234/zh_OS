/* ---------------------------------------------------------------
 * 命令行输入 (带行编辑与口令隐藏)
 * --------------------------------------------------------------- */
#ifndef ZH_LINE_INPUT_H
#define ZH_LINE_INPUT_H

#include "types.h"

#define LINE_MAX_LENGTH 128

/* 读一行, 支持退格。flags: LINE_ECHO_MASK 时不回显字符 */
enum {
    LINE_ECHO_NORMAL = 0,
    LINE_ECHO_MASK   = 1,
};

/* 返回实际长度; 输入被取消 (Ctrl+C) 时返回 -1 */
int line_read(char *buffer, u32 size, int flags);

#endif
