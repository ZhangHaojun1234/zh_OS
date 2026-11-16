/* vga.c - VGA 80x25 彩色文本：滚屏和硬件光标 */

#include "vga.h"
#include "fb.h"   /* [ADD] M25：图形模式下转发到 fb.c */

/*
 * VGA 彩色文本模式驱动（80x25，16 色）
 *
 * 显存位于物理地址 0xB8000，每个字符占 2 字节：
 *   低字节 = 字符码（CP437）
 *   高字节 = 属性：高 4 位背景色，低 4 位前景色
 * 0xB8000 已被 boot/boot.asm 与 kernel/paging.c 恒等映射到低 1GB 内，可直接访问。
 */

#define VGA_MEMORY_ADDR  0xB8000u
#define VGA_CRTC_INDEX   0x3D4u
#define VGA_CRTC_DATA    0x3D5u

#define CRTC_CURSOR_HIGH  0x0Eu
#define CRTC_CURSOR_LOW   0x0Fu
#define CRTC_CURSOR_START 0x0Au
#define CRTC_CURSOR_END   0x0Bu

#define TAB_WIDTH 4u

static volatile uint16_t *const vga_buffer = (volatile uint16_t *)VGA_MEMORY_ADDR;

static uint32_t vga_row;
static uint32_t vga_col;
static enum vga_color vga_bg = VGA_BLACK;

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint16_t make_cell(char c, enum vga_color fg, enum vga_color bg)
{
    const uint8_t attr = (uint8_t)((uint8_t)fg | (uint8_t)((uint8_t)bg << 4));
    return (uint16_t)((uint8_t)c | (uint16_t)((uint16_t)attr << 8));
}

static void update_cursor(void)
{
    const uint16_t pos = (uint16_t)(vga_row * VGA_WIDTH + vga_col);
    outb(VGA_CRTC_INDEX, CRTC_CURSOR_LOW);
    outb(VGA_CRTC_DATA, (uint8_t)(pos & 0xFFu));
    outb(VGA_CRTC_INDEX, CRTC_CURSOR_HIGH);
    outb(VGA_CRTC_DATA, (uint8_t)((pos >> 8) & 0xFFu));
}

static void set_cursor_shape(void)
{
    outb(VGA_CRTC_INDEX, CRTC_CURSOR_START);
    outb(VGA_CRTC_DATA, 14);
    outb(VGA_CRTC_INDEX, CRTC_CURSOR_END);
    outb(VGA_CRTC_DATA, 15);
}

void vga_clear(enum vga_color fg, enum vga_color bg)
{
    /* [ADD] M25：有帧缓冲时整块显存改由 fb.c 画（文本模式那段原样保留） */
    if (fb_active()) {
        fb_clear(fg, bg);
        return;
    }
    vga_bg = bg;
    vga_row = 0;
    vga_col = 0;

    const uint16_t blank = make_cell(' ', fg, bg);
    for (uint32_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; ++i) {
        vga_buffer[i] = blank;
    }
    update_cursor();
}

void vga_init(void)
{
    vga_clear(VGA_LIGHT_GREY, VGA_BLACK);
    set_cursor_shape();
    vga_set_cursor(0, 0);
}

void vga_scroll(void)
{
    for (uint32_t row = 1; row < VGA_HEIGHT; ++row) {
        for (uint32_t col = 0; col < VGA_WIDTH; ++col) {
            vga_buffer[(row - 1) * VGA_WIDTH + col] = vga_buffer[row * VGA_WIDTH + col];
        }
    }

    const uint16_t blank = make_cell(' ', VGA_LIGHT_GREY, vga_bg);
    for (uint32_t col = 0; col < VGA_WIDTH; ++col) {
        vga_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + col] = blank;
    }

    if (vga_row > 0) {
        --vga_row;
    }
}

void vga_set_cursor(uint32_t row, uint32_t col)
{
    if (row >= VGA_HEIGHT) {
        row = VGA_HEIGHT - 1;
    }
    if (col >= VGA_WIDTH) {
        col = VGA_WIDTH - 1;
    }
    vga_row = row;
    vga_col = col;
    update_cursor();
}

void vga_putchar(char c, enum vga_color fg, enum vga_color bg)
{
    /* [ADD] M25：同上，字符交给 fb 的 8x16 点阵渲染 */
    if (fb_active()) {
        fb_putchar(c, fg, bg);
        return;
    }
    vga_bg = bg;

    switch (c) {
    case '\n':
        vga_col = 0;
        ++vga_row;
        break;
    case '\r':
        vga_col = 0;
        break;
    case '\t':
        vga_col = (vga_col + TAB_WIDTH) & ~(TAB_WIDTH - 1);
        if (vga_col >= VGA_WIDTH) {
            vga_col = 0;
            ++vga_row;
        }
        break;
    case '\b':
        if (vga_col > 0) {
            --vga_col;
        } else if (vga_row > 0) {
            --vga_row;
            vga_col = VGA_WIDTH - 1;
        }
        vga_buffer[vga_row * VGA_WIDTH + vga_col] = make_cell(' ', fg, bg);
        break;
    default:
        vga_buffer[vga_row * VGA_WIDTH + vga_col] = make_cell(c, fg, bg);
        if (++vga_col >= VGA_WIDTH) {
            vga_col = 0;
            ++vga_row;
        }
        break;
    }

    while (vga_row >= VGA_HEIGHT) {
        vga_scroll();
    }
    update_cursor();
}

void vga_print(const char *s, enum vga_color fg, enum vga_color bg)
{
    while (*s != '\0') {
        vga_putchar(*s++, fg, bg);
    }
}

void vga_println(const char *s, enum vga_color fg, enum vga_color bg)
{
    vga_print(s, fg, bg);
    vga_putchar('\n', fg, bg);
}
