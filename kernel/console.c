/* ============================================================================
 * zh_OS 0.1beta (x86_64)
 * File:    kernel/console.c
 * Version: 0.1beta
 * Brief:   行编辑器：共享静态缓冲的 read_line / read_password。
 * ============================================================================ */

#include "console.h"

#include "keyboard.h"
#include "vga.h"

/*
 * 行编辑器。
 *
 * 实现要点：
 *   - 输入来自 drivers/keyboard.c 的环形缓冲区，read_line 自身是"消费侧"，
 *     可以安全阻塞（keyboard_getchar 内部用 sti + hlt 等待中断）。
 *   - 只接受可打印 ASCII（0x20..0x7E）与退格 / 回车，其余控制字符忽略，
 *     避免把 \t、\x1b 之类直接塞进缓冲区造成屏幕上出现异常字符。
 *   - 退格时同时擦除屏幕上的字符并把光标左移一格。
 */

#define LINE_BUFFER_SIZE 128

static char line_buffer[LINE_BUFFER_SIZE];

static void echo_backspace(void)
{
    vga_putchar('\b', VGA_LIGHT_GREY, VGA_BLACK);
}

char *read_line(const char *prompt)
{
    uint32_t length = 0;

    if (prompt != 0) {
        vga_print(prompt, VGA_LIGHT_GREEN, VGA_BLACK);
    }

    for (;;) {
        const char ch = keyboard_getchar();

        if (ch == '\n') {
            vga_putchar('\n', VGA_LIGHT_GREY, VGA_BLACK);
            break;
        }

        if (ch == '\b') {
            if (length > 0) {
                --length;
                echo_backspace();
            }
            continue;
        }

        if (ch < 0x20 || ch > 0x7E) {
            continue;
        }

        if (length + 1 >= LINE_BUFFER_SIZE) {
            /* 缓冲区已满：忽略后续字符（保留一份 \0 的空间） */
            continue;
        }

        line_buffer[length++] = ch;
        vga_putchar(ch, VGA_LIGHT_GREY, VGA_BLACK);
    }

    line_buffer[length] = '\0';
    return line_buffer;
}

char *read_password(const char *prompt)
{
    uint32_t length = 0;

    if (prompt != 0) {
        vga_print(prompt, VGA_LIGHT_GREEN, VGA_BLACK);
    }

    for (;;) {
        const char ch = keyboard_getchar();

        if (ch == '\n') {
            vga_putchar('\n', VGA_LIGHT_GREY, VGA_BLACK);
            break;
        }

        if (ch == '\b') {
            if (length > 0) {
                --length;
                echo_backspace();
            }
            continue;
        }

        if (ch < 0x20 || ch > 0x7E) {
            continue;
        }

        if (length + 1 >= LINE_BUFFER_SIZE) {
            continue;
        }

        line_buffer[length++] = ch;
        vga_putchar('*', VGA_LIGHT_GREY, VGA_BLACK);
    }

    line_buffer[length] = '\0';
    return line_buffer;
}
