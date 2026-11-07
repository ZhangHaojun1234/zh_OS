/* ---------------------------------------------------------------
 * 文本控制台: VGA 文本模式 (80x25) + COM1 串口镜像输出
 * --------------------------------------------------------------- */
#ifndef ZH_CONSOLE_H
#define ZH_CONSOLE_H

#include "types.h"

#define CONSOLE_COLS 80
#define CONSOLE_ROWS 25

enum {
    COLOR_BLACK = 0,
    COLOR_BLUE,
    COLOR_GREEN,
    COLOR_CYAN,
    COLOR_RED,
    COLOR_MAGENTA,
    COLOR_BROWN,
    COLOR_LIGHT_GREY,
    COLOR_DARK_GREY,
    COLOR_LIGHT_BLUE,
    COLOR_LIGHT_GREEN,
    COLOR_LIGHT_CYAN,
    COLOR_LIGHT_RED,
    COLOR_LIGHT_MAGENTA,
    COLOR_YELLOW,
    COLOR_WHITE,
};

void console_init(void);
void console_clear(void);
void console_putc(char c);
void console_write(const char *s);
void console_set_color(u8 fg, u8 bg);
void console_reset_color(void);
void console_newline(void);
void console_backspace(void);

#endif
