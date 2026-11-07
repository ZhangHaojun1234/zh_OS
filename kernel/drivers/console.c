#include "console.h"
#include "io.h"
#include "string.h"

#define VGA_MEMORY ((volatile u16 *)0xB8000)
#define TAB_WIDTH 4

static u32 cursor_row;
static u32 cursor_col;
static u8 attribute = (COLOR_BLACK << 4) | COLOR_LIGHT_GREY;

/* ------------------------------------------------------------------ */
/* COM1: 串口输出 (QEMU -serial 可见)                                  */
/* ------------------------------------------------------------------ */
static void serial_init(void)
{
    outb(0x3FB, 0x80);      /* 打开 DLAB */
    outb(0x3F8, 0x01);      /* 除数低字节: 115200 */
    outb(0x3F9, 0x00);      /* 除数高字节 */
    outb(0x3FB, 0x03);      /* 8N1 */
    outb(0x3FA, 0xC7);      /* 开启并清空 FIFO */
    outb(0x3FC, 0x0B);      /* DTR | RTS | OUT2 */
}

static void serial_putc(char c)
{
    while ((inb(0x3FD) & 0x20) == 0) {
    }
    if (c == '\n') {
        outb(0x3F8, '\r');
    }
    outb(0x3F8, (u8)c);
}

/* ------------------------------------------------------------------ */
/* 光标                                                                */
/* ------------------------------------------------------------------ */
static void update_hardware_cursor(void)
{
    u16 pos = (u16)(cursor_row * CONSOLE_COLS + cursor_col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (u8)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (u8)((pos >> 8) & 0xFF));
}

static void scroll_if_needed(void)
{
    if (cursor_row < CONSOLE_ROWS) {
        return;
    }

    for (u32 row = 1; row < CONSOLE_ROWS; row++) {
        for (u32 col = 0; col < CONSOLE_COLS; col++) {
            VGA_MEMORY[(row - 1) * CONSOLE_COLS + col] =
                VGA_MEMORY[row * CONSOLE_COLS + col];
        }
    }
    for (u32 col = 0; col < CONSOLE_COLS; col++) {
        VGA_MEMORY[(CONSOLE_ROWS - 1) * CONSOLE_COLS + col] =
            (u16)(' ' | (attribute << 8));
    }
    cursor_row = CONSOLE_ROWS - 1;
}

/* ------------------------------------------------------------------ */
void console_init(void)
{
    serial_init();
    console_clear();
}

void console_clear(void)
{
    for (u32 i = 0; i < CONSOLE_COLS * CONSOLE_ROWS; i++) {
        VGA_MEMORY[i] = (u16)(' ' | (attribute << 8));
    }
    cursor_row = 0;
    cursor_col = 0;
    update_hardware_cursor();
}

void console_set_color(u8 fg, u8 bg)
{
    attribute = (u8)((bg << 4) | (fg & 0x0F));
}

void console_reset_color(void)
{
    attribute = (u8)((COLOR_BLACK << 4) | COLOR_LIGHT_GREY);
}

void console_newline(void)
{
    cursor_col = 0;
    cursor_row++;
    scroll_if_needed();
    update_hardware_cursor();
}

void console_backspace(void)
{
    if (cursor_col > 0) {
        cursor_col--;
    } else if (cursor_row > 0) {
        cursor_row--;
        cursor_col = CONSOLE_COLS - 1;
    } else {
        return;
    }
    VGA_MEMORY[cursor_row * CONSOLE_COLS + cursor_col] =
        (u16)(' ' | (attribute << 8));
    update_hardware_cursor();
}

void console_putc(char c)
{
    serial_putc(c);

    switch (c) {
    case '\n':
        console_newline();
        return;
    case '\r':
        cursor_col = 0;
        update_hardware_cursor();
        return;
    case '\b':
        console_backspace();
        return;
    case '\t':
        for (int i = 0; i < TAB_WIDTH; i++) {
            console_putc(' ');
        }
        return;
    default:
        break;
    }

    VGA_MEMORY[cursor_row * CONSOLE_COLS + cursor_col] =
        (u16)((u8)c | (attribute << 8));
    cursor_col++;
    if (cursor_col >= CONSOLE_COLS) {
        console_newline();
    } else {
        update_hardware_cursor();
    }
}

void console_write(const char *s)
{
    while (*s) {
        console_putc(*s++);
    }
}
