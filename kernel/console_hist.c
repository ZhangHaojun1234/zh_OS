/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    kernel/console_hist.c
 * Version: 0.2beta
 * Brief:   支持上下键历史的行编辑器（原 console.c 一行未改）。
 * ============================================================================ */

#include "console_hist.h"

#include <stdint.h>

#include "history.h"
#include "keyboard.h"
#include "vga.h"

/*
 * 与 kernel/console.c 的 read_line() 保持一致的处理：
 *   - 静态行缓冲 128 字节、可打印 ASCII(0x20..0x7E) 过滤、退格擦除、回车换行返回。
 *   - 提示符 / 回显 / 擦除所用的颜色与 console.c 的 read_line 完全相同。
 *     当前项目为 Linux 红/黄/绿配色，read_line 的 prompt 是亮绿；
 *     这里跟随同一颜色，不额外引入青色，保证与原行编辑器观感一致。
 *
 * 新增行为：
 *   - 上箭头：history_prev()，先擦掉当前行，再把历史命令填进缓冲并回显。
 *   - 下箭头：history_next()，返回 NULL 时恢复空行。
 *   - 左右箭头：本次实现为忽略（光标始终保持在行尾，不做行内编辑），
 *     注释里保留这个约定；以后实现行内光标时再扩展。
 *   - 等待时同时轮询 keyboard_haschar() 与 keyboard_has_special()，
 *     两者都空才 sti; hlt，因此不会卡死在原 keyboard_getchar() 的阻塞里。
 *   - 回车返回前 history_add(缓冲内容) + history_reset_cursor()，
 *     空行与连续重复命令由 history_add 内部过滤。
 *
 * 注意：本文件使用自己独立的静态行缓冲，不复用 console.c 的 line_buffer，
 * 避免两个行编辑器互相覆盖；返回值语义与 read_line 一致（指向静态缓冲）。
 */

#define LINE_BUFFER_SIZE HIST_LINE_MAX

static char hist_line_buffer[LINE_BUFFER_SIZE];

static void echo_backspace(void)
{
    vga_putchar('\b', VGA_LIGHT_GREY, VGA_BLACK);
}

/* 把当前已回显的 length 个字符逐个擦掉，光标退回提示符末尾。 */
static void erase_current_line(uint32_t length)
{
    for (uint32_t i = 0; i < length; ++i) {
        echo_backspace();
    }
}

/* 把 src 拷进本地行缓冲并逐字符回显，返回新长度；src 为 NULL 时得到空行。 */
static uint32_t load_and_echo(char *buf, const char *src)
{
    uint32_t n = 0;

    if (src != 0) {
        while (src[n] != '\0' && n + 1u < LINE_BUFFER_SIZE) {
            buf[n] = src[n];
            ++n;
        }
    }
    buf[n] = '\0';
    for (uint32_t i = 0; i < n; ++i) {
        vga_putchar(buf[i], VGA_LIGHT_GREY, VGA_BLACK);
    }
    return n;
}

char *read_line_with_history(const char *prompt)
{
    uint32_t length = 0;

    if (prompt != 0) {
        vga_print(prompt, VGA_LIGHT_GREEN, VGA_BLACK);
    }

    /* 每次开始新输入前把历史浏览游标移回最新之后 */
    history_reset_cursor();

    for (;;) {
        /* 1) 优先消费方向键等特殊事件（只处理上下，左右忽略） */
        if (keyboard_has_special()) {
            const uint8_t key = keyboard_get_special();

            if (key == KEY_UP || key == KEY_DOWN) {
                const char *entry = (key == KEY_UP)
                                        ? history_prev() : history_next();
                erase_current_line(length);
                length = load_and_echo(hist_line_buffer, entry);
            }
            /* KEY_LEFT / KEY_RIGHT / KEY_NONE：本次忽略，光标保持在行尾 */
            continue;
        }

        /* 2) 普通字符，处理逻辑与 console.c 的 read_line 完全一致 */
        if (keyboard_haschar()) {
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

            if (length + 1u >= LINE_BUFFER_SIZE) {
                /* 缓冲区已满：忽略后续字符（保留一份 '\0' 的空间） */
                continue;
            }

            hist_line_buffer[length++] = ch;
            vga_putchar(ch, VGA_LIGHT_GREY, VGA_BLACK);
            continue;
        }

        /* 3) 两类事件都没有：开中断并 hlt，等下一次 IRQ 唤醒，不忙等 */
        __asm__ volatile("sti; hlt");
    }

    hist_line_buffer[length] = '\0';
    history_add(hist_line_buffer);
    history_reset_cursor();
    return hist_line_buffer;
}
