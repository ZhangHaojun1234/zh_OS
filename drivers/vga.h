/* vga.h - VGA 16 色枚举和驱动接口 */

#ifndef ZH_OS_VGA_H
#define ZH_OS_VGA_H

#include <stdint.h>

#define VGA_WIDTH  80u
#define VGA_HEIGHT 25u

/* VGA 16 色（前景低 4 位，背景高 4 位） */
enum vga_color {
    VGA_BLACK = 0,
    VGA_BLUE = 1,
    VGA_GREEN = 2,
    VGA_CYAN = 3,
    VGA_RED = 4,
    VGA_MAGENTA = 5,
    VGA_BROWN = 6,
    VGA_LIGHT_GREY = 7,
    VGA_DARK_GREY = 8,
    VGA_LIGHT_BLUE = 9,
    VGA_LIGHT_GREEN = 10,
    VGA_LIGHT_CYAN = 11,
    VGA_LIGHT_RED = 12,
    VGA_LIGHT_MAGENTA = 13,
    VGA_YELLOW = 14,
    VGA_WHITE = 15
};

/* 初始化：清屏为浅灰字/黑底，复位光标并设置光标形状。 */
void vga_init(void);

/* 用指定前景/背景色清空整屏。 */
void vga_clear(enum vga_color fg, enum vga_color bg);

/* 输出单字符，支持 \n \r \t \b。 */
void vga_putchar(char c, enum vga_color fg, enum vga_color bg);

/* 输出字符串 / 字符串加换行。 */
void vga_print(const char *s, enum vga_color fg, enum vga_color bg);
void vga_println(const char *s, enum vga_color fg, enum vga_color bg);

/* 满屏上滚一行（底部用当前背景色填充）。 */
void vga_scroll(void);

/* 定位光标（行列从 0 开始，自动钳制到屏幕范围内）并同步硬件光标。 */
void vga_set_cursor(uint32_t row, uint32_t col);

#endif /* ZH_OS_VGA_H */
