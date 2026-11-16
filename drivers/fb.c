/* ============================================================================
 * zh_OS 1.0 (x86_64)
 * File:    drivers/fb.c
 * Version: 1.0
 * Brief:   线性帧缓冲文字控制台，[ADD] M25。
 *
 * 三件事：
 *   1) 从 Multiboot2 标签里拿到 framebuffer（地址/宽/高/行跨距/色深）
 *   2) 把那块物理内存映射进内核地址空间（它常在 0xFD000000，超过恒等映射的 1GB）
 *   3) 用 8x16 点阵字体画字：清屏、写字符、滚屏
 * ============================================================================ */

#include <stdint.h>
#include "fb.h"
#include "font8x16.h"
#include "mtrr.h"
#include "../kernel/paging.h"

/* [ADD] M34：帧缓冲实际拿到的 WC 覆盖字节数（0 = MTRR 没插上，仍是 UC） */
static uint64_t g_wc_bytes = 0ull;

/* Multiboot2 标签类型 */
#define MB2_TAG_END        0u
#define MB2_TAG_FRAMEBUFFER 8u

/* framebuffer 标签里的 framebuffer_type */
#define MB2_FB_TYPE_RGB    1u

static int      g_active = 0;
static uint32_t g_phys = 0u;      /* 硬件给的线性地址 */
static uint8_t *g_base = 0;       /* 映射后可写的指针 */
static uint32_t g_w = 0u, g_h = 0u, g_pitch = 0u, g_bpp = 0u;
static uint32_t g_cols = 0u, g_rows = 0u;
static uint32_t g_cx = 0u, g_cy = 0u;
static uint32_t g_fg = 0xC8C8C8u, g_bg = 0x000000u;

/* [ADD] M34：字体缩放倍数。真机 2880x1800 面板下 res 用不了（Bochs dispi 不存在），
 * 唯一能让字变大的办法是把 8x16 字模按 NxN 像素块画出来。格子变成 8N x 16N，
 * 行列数按新格子算，逻辑上等价于"分辨率降 N 倍"。 */
static uint32_t g_scale = 1u;

static uint32_t cell_w(void) { return FONT_W * g_scale; }
static uint32_t cell_h(void) { return FONT_H * g_scale; }

/* [ADD] M26：整屏字符缓冲。
 *
 * 为什么必须有：换分辨率以前的实现是"直接往帧缓冲画像素"，屏幕上的字就是像素本身，
 * 一旦换模式（旧的显存内容作废），上一屏的文字就再也找不回来了 —— 所以 res 只能清屏。
 * Linux 的 fbcon 能重绘，是因为它上面还有一层 vc（virtual console）保存着字符。
 *
 * 这里存的就是那一层：每个格子记 字符 + 前景/背景调色板号（各 1 字节，不存 RGB，
 * 省空间也省得换色时失真）。480x160 是上限（3840x2160 的 8x16 网格正好这么密），
 * 实际只用能看见的 g_rows/g_cols。
 */
#define FB_BUF_COLS 480u   /* [ADD] M29：真机 2880x1800 = 360x112，留到 3840x2160 */
#define FB_BUF_ROWS 160u

struct fb_cell {
    uint8_t ch;
    uint8_t fg;
    uint8_t bg;
    uint8_t pad;
};

static struct fb_cell g_buf[FB_BUF_ROWS][FB_BUF_COLS];
static uint8_t g_fg_idx = 7u, g_bg_idx = 0u;
/* 已经用到的行数（= 光标所在行 + 1）。换模式时只搬这几行，免得把成片空行也搬过去、
 * 结果历史被顶到屏幕最上面、光标在屏幕最底下，中间留一大条空白。 */
static uint32_t g_used_rows = 0u;

/* VGA 16 色调色板 -> RGB888（和文本模式的观感尽量接近） */
static const uint32_t g_palette[16] = {
    0x000000u, 0x0000AAu, 0x00AA00u, 0x00AAAAu,
    0xAA0000u, 0xAA00AAu, 0xAA5500u, 0xAAAAAAu,
    0x555555u, 0x5555FFu, 0x55FF55u, 0x55FFFFu,
    0xFF5555u, 0xFF55FFu, 0xFFDD55u, 0xFFFFFFu
};

static uint32_t color_of(enum vga_color c)
{
    uint32_t i = (uint32_t)c;
    if (i > 15u) {
        i = 15u;
    }
    return g_palette[i];
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd_u64(const uint8_t *p)
{
    return (uint64_t)rd_u32(p) | ((uint64_t)rd_u32(p + 4) << 32);
}

/* 把 (x,y) 这一个像素写成 fg 色 */
static void put_pixel(uint32_t x, uint32_t y, uint32_t rgb)
{
    uint8_t *p = g_base + (uint64_t)y * g_pitch + (uint64_t)x * (g_bpp / 8u);

    if (g_bpp == 32u) {
        /* [ADD] M34：一次 32 位写，别拆成 4 次字节写 */
        *(volatile uint32_t *)p = rgb;
    } else if (g_bpp == 24u) {
        p[0] = (uint8_t)(rgb & 0xFFu);
        p[1] = (uint8_t)((rgb >> 8) & 0xFFu);
        p[2] = (uint8_t)((rgb >> 16) & 0xFFu);
    } else if (g_bpp == 16u) {
        /* RGB565 */
        uint16_t v = (uint16_t)(((rgb >> 19) & 0x1Fu) << 11 |
                                ((rgb >> 10) & 0x3Fu) << 5 |
                                ((rgb >> 3) & 0x1Fu));
        p[0] = (uint8_t)(v & 0xFFu);
        p[1] = (uint8_t)(v >> 8);
    }
}

/* 一个矩形刷成单一颜色（缩放后一个字/一个像素块都是矩形） */
static void fill_rect(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t rgb)
{
    uint32_t x, y;

    if (g_bpp == 32u) {
        /* [ADD] M34：32bpp 走 32 位写 + 同色行内复用，比逐像素算指针快得多 */
        for (y = 0u; y < h; ++y) {
            volatile uint32_t *row =
                (volatile uint32_t *)(g_base + (uint64_t)(y0 + y) * g_pitch) + x0;
            for (x = 0u; x < w; ++x) {
                row[x] = rgb;
            }
        }
        return;
    }
    for (y = 0u; y < h; ++y) {
        for (x = 0u; x < w; ++x) {
            put_pixel(x0 + x, y0 + y, rgb);
        }
    }
}

/* 一个格子刷成单一颜色（滚屏/清屏用） */
static void fill_cell(uint32_t cx, uint32_t cy, uint32_t rgb)
{
    fill_rect(cx * cell_w(), cy * cell_h(), cell_w(), cell_h(), rgb);
}

static void fill_all(uint32_t rgb)
{
    uint32_t x, y;
    for (y = 0u; y < g_h; ++y) {
        for (x = 0u; x < g_w; ++x) {
            put_pixel(x, y, rgb);
        }
    }
}

/* ---- [ADD] M26：字符缓冲的四个基本动作 ---- */

/* 缓冲里清掉所有可见格子（不动显存，显存由调用方 fill_all） */
static void buf_clear(void)
{
    uint32_t x, y;
    for (y = 0u; y < g_rows && y < FB_BUF_ROWS; ++y) {
        for (x = 0u; x < g_cols && x < FB_BUF_COLS; ++x) {
            g_buf[y][x].ch = ' ';
            g_buf[y][x].fg = g_fg_idx;
            g_buf[y][x].bg = g_bg_idx;
        }
    }
}

/* 往缓冲的一个格子里记一个字符 */
static void buf_put(uint32_t cx, uint32_t cy, char ch)
{
    if (cx >= FB_BUF_COLS || cy >= FB_BUF_ROWS) {
        return;
    }
    g_buf[cy][cx].ch = (uint8_t)ch;
    g_buf[cy][cx].fg = g_fg_idx;
    g_buf[cy][cx].bg = g_bg_idx;
}

/* 按缓冲内容重画一个格子（这是唯一真正落像素的地方） */
static void draw_cell(uint32_t cx, uint32_t cy)
{
    const struct fb_cell *cell;
    uint32_t fg, bg, x, y;
    uint8_t bits;

    if (cx >= FB_BUF_COLS || cy >= FB_BUF_ROWS) {
        return;
    }
    cell = &g_buf[cy][cx];
    fg = g_palette[cell->fg > 15u ? 15u : cell->fg];
    bg = g_palette[cell->bg > 15u ? 15u : cell->bg];

    if (cell->ch < FONT_FIRST || cell->ch > FONT_LAST) {
        fill_cell(cx, cy, bg);
        return;
    }
    /* [ADD] M34：32bpp 快路径 —— 每行只算一次行指针，行内连续写；
     * 缩放时把每个字模像素展开成 g_scale 个连续字。 */
    if (g_bpp == 32u) {
        uint32_t x0 = cx * cell_w();
        for (y = 0u; y < FONT_H; ++y) {
            bits = g_font8x16[cell->ch - FONT_FIRST][y];
            if (g_scale == 1u) {
                volatile uint32_t *row =
                    (volatile uint32_t *)(g_base + (uint64_t)(cy * cell_h() + y) * g_pitch) + x0;
                for (x = 0u; x < FONT_W; ++x) {
                    row[x] = (bits & (0x80u >> x)) ? fg : bg;
                }
            } else {
                uint32_t dy, dx;
                for (dy = 0u; dy < g_scale; ++dy) {
                    volatile uint32_t *row =
                        (volatile uint32_t *)(g_base +
                            (uint64_t)(cy * cell_h() + y * g_scale + dy) * g_pitch) + x0;
                    for (x = 0u; x < FONT_W; ++x) {
                        uint32_t c = (bits & (0x80u >> x)) ? fg : bg;
                        for (dx = 0u; dx < g_scale; ++dx) {
                            row[x * g_scale + dx] = c;
                        }
                    }
                }
            }
        }
        return;
    }
    for (y = 0u; y < FONT_H; ++y) {
        bits = g_font8x16[cell->ch - FONT_FIRST][y];
        for (x = 0u; x < FONT_W; ++x) {
            /* [ADD] M34：一位字模放大成 g_scale x g_scale 的像素块 */
            fill_rect(cx * cell_w() + x * g_scale,
                      cy * cell_h() + y * g_scale,
                      g_scale, g_scale,
                      (bits & (0x80u >> x)) ? fg : bg);
        }
    }
}

/* 整屏按缓冲重画（换模式后靠这个把内容接回来） */
void fb_redraw(void)
{
    uint32_t x, y;

    if (!g_active) {
        return;
    }
    for (y = 0u; y < g_rows; ++y) {
        for (x = 0u; x < g_cols; ++x) {
            draw_cell(x, y);
        }
    }
}

/* 解析 Multiboot2 标签链 */
static int scan_tags(uint32_t mb2_info)
{
    const uint8_t *base = (const uint8_t *)(uintptr_t)mb2_info;
    uint32_t total = rd_u32(base);
    uint32_t off = 8u;

    if (total < 16u || total > (16u * 1024u * 1024u)) {
        return 0;
    }
    while (off + 8u <= total) {
        const uint8_t *tag = base + off;
        uint32_t type = rd_u32(tag);
        uint32_t size = rd_u32(tag + 4u);

        if (type == MB2_TAG_END || size < 8u) {
            break;
        }
        if (type == MB2_TAG_FRAMEBUFFER && size >= 32u) {
            uint64_t addr = rd_u64(tag + 8u);
            uint32_t pitch = rd_u32(tag + 16u);
            uint32_t width = rd_u32(tag + 20u);
            uint32_t height = rd_u32(tag + 24u);
            uint8_t  bpp = tag[28];
            uint8_t  fbtype = tag[29];

            if (fbtype != MB2_FB_TYPE_RGB || width == 0u || height == 0u) {
                return 0;
            }
            if (bpp != 32u && bpp != 24u && bpp != 16u) {
                return 0;
            }
            g_phys = (uint32_t)addr;
            g_pitch = pitch;
            g_w = width;
            g_h = height;
            g_bpp = bpp;
            return 1;
        }
        off += (size + 7u) & ~7u;         /* 标签按 8 字节对齐 */
    }
    return 0;
}

int fb_init(uint32_t mb2_info)
{
    if (mb2_info == 0u || !scan_tags(mb2_info)) {
        g_active = 0;
        return 0;
    }
    /* 帧缓冲常在 0xFD000000（PCI 空洞里，超过恒等映射的 1GB），必须显式映射 */
    if (!paging_map_2mb_range_type((uint64_t)g_phys, (uint64_t)g_pitch * g_h, 1)) {
        g_active = 0;
        return 0;
    }
    g_base = (uint8_t *)(uintptr_t)g_phys;
    /* [ADD] M34：页表的 PAT 只在一半 —— MTRR 里若有 UC 项压着，WC 就失效。
     * 这里再拿一条 MTRR 把整段显式标成 WC，真机宽度 2880 时差别是几十倍。 */
    g_wc_bytes = mtrr_wc_range((uint64_t)g_phys, (uint64_t)g_pitch * g_h);
    g_cols = g_w / cell_w();
    g_rows = g_h / cell_h();
    if (g_cols == 0u || g_rows == 0u) {
        g_active = 0;
        return 0;
    }
    g_cx = 0u;
    g_cy = 0u;
    g_active = 1;
    g_fg_idx = 7u;
    g_bg_idx = 0u;
    buf_clear();
    fill_all(g_bg);
    return 1;
}

int fb_active(void)          { return g_active; }
uint32_t fb_width(void)      { return g_w; }
uint32_t fb_height(void)     { return g_h; }
uint32_t fb_bpp(void)        { return g_bpp; }
uint32_t fb_pitch(void)      { return g_pitch; }
uint32_t fb_cols(void)       { return g_cols; }
uint32_t fb_rows(void)       { return g_rows; }

void fb_clear(enum vga_color fg, enum vga_color bg)
{
    g_fg = color_of(fg);
    g_bg = color_of(bg);
    g_fg_idx = (uint8_t)fg;
    g_bg_idx = (uint8_t)bg;
    g_cx = 0u;
    g_cy = 0u;
    g_used_rows = 0u;
    buf_clear();
    fill_all(g_bg);
}

void fb_scroll(void)
{
    uint8_t *dst = g_base;
    uint8_t *src = g_base + (uint64_t)cell_h() * g_pitch;
    uint64_t bytes = (uint64_t)g_pitch * (g_h - cell_h());
    uint32_t x, y;
    uint64_t i;

    for (i = 0u; i + 8u <= bytes; i += 8u) {
        /* [ADD] M34：8 字节一次拷贝 —— 大分辨率下一屏 20MB，按字节搬太慢 */
        *(uint64_t *)(void *)(dst + i) = *(uint64_t *)(void *)(src + i);
    }
    for (; i < bytes; ++i) {
        dst[i] = src[i];
    }
    /* [ADD] M26：缓冲也要跟着上滚一行，否则重绘时会错一行 */
    for (y = 0u; y + 1u < g_rows; ++y) {
        for (x = 0u; x < g_cols; ++x) {
            g_buf[y][x] = g_buf[y + 1u][x];
        }
    }
    for (x = 0u; x < g_cols; ++x) {
        g_buf[g_rows - 1u][x].ch = ' ';
        g_buf[g_rows - 1u][x].fg = g_fg_idx;
        g_buf[g_rows - 1u][x].bg = g_bg_idx;
        fill_cell(x, g_rows - 1u, g_bg);
    }
    g_cy = g_rows - 1u;
    g_used_rows = g_rows;
}

void fb_putchar(char c, enum vga_color fg, enum vga_color bg)
{
    g_fg = color_of(fg);
    g_bg = color_of(bg);
    g_fg_idx = (uint8_t)fg;
    g_bg_idx = (uint8_t)bg;

    if (c == '\n') {
        g_cx = 0u;
        ++g_cy;
    } else if (c == '\r') {
        g_cx = 0u;
    } else if (c == '\t') {
        g_cx = (g_cx + 8u) & ~7u;
        if (g_cx >= g_cols) {
            g_cx = 0u;
            ++g_cy;
        }
    } else if (c == '\b') {
        if (g_cx > 0u) {
            --g_cx;
        }
        buf_put(g_cx, g_cy, ' ');
        draw_cell(g_cx, g_cy);
    } else if ((uint32_t)c >= FONT_FIRST && (uint32_t)c <= FONT_LAST) {
        buf_put(g_cx, g_cy, c);
        draw_cell(g_cx, g_cy);
        ++g_cx;
        if (g_cx >= g_cols) {
            g_cx = 0u;
            ++g_cy;
        }
    }
    if (g_cy >= g_rows) {
        fb_scroll();
    }
    if (g_cy < g_rows && g_cy + 1u > g_used_rows) {
        g_used_rows = g_cy + 1u;
    }
}

/* ------------------------------------------------------------------ */
/* Shell 命令 videomode：把当前几何报出来（也是"调分辨率生效了没有"的判据） */
/* ------------------------------------------------------------------ */

static void put_u32(uint32_t v)
{
    char digits[12];
    uint32_t n = 0u;

    if (v == 0u) {
        digits[n++] = '0';
    }
    while (v != 0u) {
        digits[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n != 0u) {
        vga_putchar(digits[--n], VGA_LIGHT_GREEN, VGA_BLACK);
    }
}

int cmd_videomode(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!fb_active()) {
        vga_print("text mode 80x25 (no framebuffer from GRUB)\n",
                  VGA_LIGHT_RED, VGA_BLACK);
        return 0;
    }
    vga_print("framebuffer ", VGA_LIGHT_CYAN, VGA_BLACK); put_u32(fb_width());
    vga_putchar('x', VGA_LIGHT_CYAN, VGA_BLACK);          put_u32(fb_height());
    vga_print("x", VGA_LIGHT_CYAN, VGA_BLACK);            put_u32(fb_bpp());
    vga_print("  pitch ", VGA_LIGHT_CYAN, VGA_BLACK);     put_u32(fb_pitch());
    vga_print("\ntext grid ", VGA_LIGHT_CYAN, VGA_BLACK); put_u32(fb_cols());
    vga_putchar('x', VGA_LIGHT_CYAN, VGA_BLACK);          put_u32(fb_rows());
    vga_print(" cells (8x16 font)\n", VGA_LIGHT_CYAN, VGA_BLACK);
    return 0;
}

/* ================================================================== */
/* [ADD] M25 第二部分：运行时换分辨率（Bochs VBE dispi 接口）        */
/*                                                                  */
/* 为什么用这个接口：GRUB 只在启动那一刻定模式（Multiboot2 帧缓冲标签   */
/* 要嘛指定死一个模式，要嘛全交给 auto），跑起来之后想换，得自己跟显卡   */
/* 说话。x86_64 长模式下再也调不到 int 10h，QEMU 的 VGA 正好提供 Bochs  */
/* dispi 寄存器组（端口 0x1CE 索引 / 0x1CF 数据），能直接改模式。       */
/* 真机没有这套接口 -> 读 ID 不是 0xB0Cx 就直接失败，绝不乱写端口。     */
/* ================================================================== */

#define VBE_DISPI_INDEX_ID        0x0u
#define VBE_DISPI_INDEX_XRES      0x1u
#define VBE_DISPI_INDEX_YRES      0x2u
#define VBE_DISPI_INDEX_BPP       0x3u
#define VBE_DISPI_INDEX_ENABLE    0x4u
#define VBE_DISPI_INDEX_BANK      0x5u
#define VBE_DISPI_ENABLED         0x01u
#define VBE_DISPI_LFB_ENABLED     0x40u
#define VBE_DISPI_DISABLED        0x00u
#define VBE_DISPI_ID0             0xB0C0u

static inline void dispi_write(uint16_t index, uint16_t value)
{
    __asm__ volatile("outw %0, %1" : : "a"(index), "d"((uint16_t)0x1CE));
    __asm__ volatile("outw %0, %1" : : "a"(value), "d"((uint16_t)0x1CF));
}

static inline uint16_t dispi_read(uint16_t index)
{
    uint16_t v;
    __asm__ volatile("outw %0, %1" : : "a"(index), "d"((uint16_t)0x1CE));
    __asm__ volatile("inw %1, %0" : "=a"(v) : "d"((uint16_t)0x1CF));
    return v;
}

int fb_set_mode(uint32_t w, uint32_t h, uint32_t bpp)
{
    uint16_t id;

    if (!g_active) {
        return 0;
    }
    id = dispi_read(VBE_DISPI_INDEX_ID);
    /* 0xB0C0..0xB0C5 是 Bochs/QEMU VBE 的版本号族 */
    if (id < VBE_DISPI_ID0 || id > (VBE_DISPI_ID0 + 5u)) {
        return 0;
    }
    if (bpp != 32u && bpp != 24u && bpp != 16u) {
        return 0;
    }
    if (w == 0u || h == 0u || w > 3840u || h > 2160u) {   /* [ADD] M29：真机 2880x1800 */
        return 0;
    }

    /* [ADD] M27：先把新尺寸要用的帧缓冲映射出来，再切模式。
     *
     * 启动时 fb_init 只按 GRUB 给的那一档建过页表（1280x800x32 -> 5120*800 ≈ 4MB），
     * 而 1920x1080x32 需要 1920*4*1080 ≈ 8.3MB —— 不补映射就写像素，写到 4MB 之后
     * 立刻 #PF（error code 0x2：write + page not present），表现就是 KERNEL PANIC。
     *
     * paging_map_2mb_range 对已经映射过的 2MB 页只是覆盖重写同一项，不会重复吃 pd_pool，
     * 所以放大/缩小分辨率都可以反复调用。 */
    if (!paging_map_2mb_range_type((uint64_t)g_phys,
                                   (uint64_t)w * (uint64_t)(bpp / 8u) * (uint64_t)h, 1)) {
        return 0;
    }

    dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    dispi_write(VBE_DISPI_INDEX_XRES, (uint16_t)w);
    dispi_write(VBE_DISPI_INDEX_YRES, (uint16_t)h);
    dispi_write(VBE_DISPI_INDEX_BPP, (uint16_t)bpp);
    dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    /* 线性帧缓冲的基址和行跨距 —— 基址不变，跨距按新宽度算 */
    uint32_t old_rows = g_rows, old_cols = g_cols, old_used = g_used_rows;
    uint32_t keep, i2, x2;

    g_w = w;
    g_h = h;
    g_bpp = bpp;
    g_pitch = w * (bpp / 8u);
    g_cols = g_w / cell_w();
    g_rows = g_h / cell_h();
    g_cx = 0u;
    g_cy = 0u;
    /* 模式刚切完，显示管线还在按旧跨距扫，立刻重画会留下上一屏的残影；
     * 先空转一小会儿（约几十毫秒）等它稳定。 */
    for (volatile uint64_t spin = 0u; spin < 8000000ull; ++spin) {
        __asm__ volatile("pause");
    }
    /* 再关一次再开一次：逼显示端整屏重新扫描，避免顶部残留上一屏的残影 */
    dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    /* [ADD] M26 / [ADD] M27：把旧屏幕“真正用过的最后 keep 行”搬到新屏幕，再整屏重绘。
     *
     * 对齐方式按“新屏装不装得下”决定，而不是一律贴底：
     *  - keep == g_rows（历史够多，正好填满新屏）：搬最后 g_rows 行，屏满，无空白；
     *  - keep <  g_rows（1280x800 换到 1920x1080 就多出 17 行）：历史贴顶部、
     *    光标紧跟历史最后一行 —— 否则历史被压到屏幕最底下、中间空一大片，
     *    看起来就像屏坏了（1920x1080 实测就是这样）。
     * src 起点恒 >= dst 起点，所以正序搬不会踩到还没读的行。 */
    keep = old_used < g_rows ? old_used : g_rows;
    if (keep > FB_BUF_ROWS) {
        keep = FB_BUF_ROWS;
    }
    if (keep > old_used) {
        keep = old_used;
    }
    for (i2 = 0u; i2 < keep; ++i2) {
        uint32_t src_row = old_used - keep + i2;
        uint32_t dst_row = i2;
        for (x2 = 0u; x2 < g_cols; ++x2) {
            if (x2 < old_cols && x2 < FB_BUF_COLS && src_row < FB_BUF_ROWS) {
                g_buf[dst_row][x2] = g_buf[src_row][x2];
            } else {
                g_buf[dst_row][x2].ch = ' ';
                g_buf[dst_row][x2].fg = g_fg_idx;
                g_buf[dst_row][x2].bg = g_bg_idx;
            }
        }
    }
    /* 历史下面的空行、新屏右侧多出来的列，一起刷成空格 */
    for (i2 = 0u; i2 < g_rows && i2 < FB_BUF_ROWS; ++i2) {
        uint32_t from = (i2 < keep) ? g_cols : 0u;
        for (x2 = from; x2 < g_cols && x2 < FB_BUF_COLS; ++x2) {
            g_buf[i2][x2].ch = ' ';
            g_buf[i2][x2].fg = g_fg_idx;
            g_buf[i2][x2].bg = g_bg_idx;
        }
    }
    fb_redraw();

    /* 光标落在历史最后一行之后；历史填满整屏时就是最后一行 */
    g_used_rows = (keep < g_rows) ? keep : g_rows;
    g_cy = (keep < g_rows) ? keep : (g_rows - 1u);
    g_cx = 0u;
    return 1;
}

/* res <宽>x<高>  例：res 1024x768   也可 res 800x600 16 */
int cmd_res(int argc, char **argv)
{
    uint32_t w = 0u, h = 0u, bpp = 32u;
    const char *s;

    if (argc < 2) {
        vga_print("usage: res <width>x<height>   e.g. res 1024x768  or  res 1024*768\n",
                  VGA_LIGHT_RED, VGA_BLACK);
        return 0;
    }
    s = argv[1];
    while (*s >= '0' && *s <= '9') {
        w = w * 10u + (uint32_t)(*s - '0');
        ++s;
    }
    /* [ADD] M26：分隔符放宽 —— 字母 x / X / 星号 * 都认。
     * 装文档里写成 1280x800 有人会照着敲"×"（乘号），键盘上根本打不出来；
     * 星号是随手能敲的，所以直接收。 */
    if (*s != 'x' && *s != 'X' && *s != '*') {
        vga_print("res: expected <width>x<height>  (x or *)\n", VGA_LIGHT_RED, VGA_BLACK);
        return 0;
    }
    ++s;
    while (*s >= '0' && *s <= '9') {
        h = h * 10u + (uint32_t)(*s - '0');
        ++s;
    }
    if (argc >= 3) {
        bpp = 0u;
        for (const char *b = argv[2]; *b >= '0' && *b <= '9'; ++b) {
            bpp = bpp * 10u + (uint32_t)(*b - '0');
        }
        if (bpp == 0u) {
            bpp = 32u;
        }
    }
    if (!fb_active()) {
        vga_print("res: no framebuffer, this kernel is in text mode\n",
                  VGA_LIGHT_RED, VGA_BLACK);
        return 0;
    }
    if (!fb_set_mode(w, h, bpp)) {
        vga_print("res: the video device refused that mode\n",
                  VGA_LIGHT_RED, VGA_BLACK);
        return 0;
    }
    /* [ADD] M26：换模式后按字符缓冲整屏重绘，旧内容接在保留的历史行后面 */
    cmd_videomode(0, 0);
    return 0;
}

/* ================================================================== */
/* [ADD] M34：字体缩放 —— 真机唯一可用的"放大字"手段                  */
/*                                                                    */
/* 真机 Intel 核显没有 Bochs dispi 那套寄存器，res 必然被拒；而        */
/* ExitBootServices 之后内核也调不到 GOP 的 SetMode。所以分辨率只能靠  */
/* GRUB 启动时定。定了之后如果还是嫌字小（2880x1800 面板 360x112 格）， */
/* 就只能在软件层把字模放大：8x16 的每一位画成 NxN 像素块，格子变      */
/* 8Nx16N，行列数相应减少 —— 等效于把"逻辑分辨率"降低 N 倍。           */
/* ================================================================== */

uint32_t fb_scale(void)
{
    return g_scale;
}

int fb_set_scale(uint32_t s)
{
    uint32_t nc, nr;

    if (!g_active || s < 1u || s > 4u) {
        return 0;
    }
    if (s == g_scale) {
        return 1;
    }
    nc = g_w / (FONT_W * s);
    nr = g_h / (FONT_H * s);
    if (nc == 0u || nr == 0u || nc > FB_BUF_COLS || nr > FB_BUF_ROWS) {
        return 0;                        /* 这个倍数在这块屏上放不下 */
    }
    g_scale = s;
    g_cols = nc;
    g_rows = nr;
    g_cx = 0u;
    g_cy = 0u;
    g_used_rows = 0u;
    buf_clear();
    fill_all(g_bg);
    return 1;
}

int cmd_fontscale(int argc, char **argv)
{
    uint32_t s = 0u;
    const char *p;

    if (argc < 2) {
        if (!fb_active()) {
            vga_print("fontscale: no framebuffer (text mode has a fixed 8x16 font)\n",
                      VGA_LIGHT_RED, VGA_BLACK);
        } else {
            vga_print("usage: fontscale <1..4>   current ", VGA_LIGHT_RED, VGA_BLACK);
            put_u32(fb_scale());
            vga_putchar('\n', VGA_LIGHT_RED, VGA_BLACK);
        }
        return 0;
    }
    for (p = argv[1]; *p >= '0' && *p <= '9'; ++p) {
        s = s * 10u + (uint32_t)(*p - '0');
    }
    if (!fb_active()) {
        vga_print("fontscale: no framebuffer, this kernel is in text mode\n",
                  VGA_LIGHT_RED, VGA_BLACK);
        return 0;
    }
    if (!fb_set_scale(s)) {
        vga_print("fontscale: cannot use ", VGA_LIGHT_RED, VGA_BLACK);
        put_u32(s);
        vga_print(" on this screen (1..4, must fit)\n", VGA_LIGHT_RED, VGA_BLACK);
        return 0;
    }
    vga_print("fontscale ", VGA_LIGHT_GREEN, VGA_BLACK);
    put_u32(fb_scale());
    vga_print("  grid ", VGA_LIGHT_GREEN, VGA_BLACK);
    put_u32(fb_cols());
    vga_putchar('x', VGA_LIGHT_GREEN, VGA_BLACK);
    put_u32(fb_rows());
    vga_print(" cells\n", VGA_LIGHT_GREEN, VGA_BLACK);
    return 0;
}

uint64_t fb_wc_bytes(void)
{
    return g_wc_bytes;
}

/* [ADD] M34：`bench` —— 往显存里成片写 32 位字，用 RDTSC 数周期。
 * 判断 WC 是否生效看 cycles/byte：WC 大约 1~3，UC 会到几十上百。 */
int cmd_bench(int argc, char **argv)
{
    volatile uint32_t *p;
    uint64_t bytes, words, i, t0, t1, cycles, per_byte_x100;
    uint32_t lo, hi;

    if (!fb_active()) {
        vga_print("bench: no framebuffer\n", VGA_LIGHT_RED, VGA_BLACK);
        return 0;
    }
    bytes = (uint64_t)g_pitch * (uint64_t)g_h;
    words = bytes / 4ull;
    p = (volatile uint32_t *)(void *)g_base;

    __asm__ volatile("mfence" : : : "memory");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    t0 = ((uint64_t)hi << 32) | (uint64_t)lo;
    for (i = 0ull; i < words; ++i) {
        p[i] = 0x00336699u;
    }
    __asm__ volatile("mfence" : : : "memory");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    t1 = ((uint64_t)hi << 32) | (uint64_t)lo;
    cycles = t1 - t0;
    per_byte_x100 = (bytes != 0ull) ? (cycles * 100ull / bytes) : 0ull;

    vga_print("bench: wrote ", VGA_LIGHT_GREEN, VGA_BLACK);
    put_u32((uint32_t)(bytes >> 10));
    vga_print(" KB to the framebuffer in ", VGA_LIGHT_GREEN, VGA_BLACK);
    put_u32((uint32_t)(cycles >> 10));
    vga_print("K cycles -> ", VGA_LIGHT_GREEN, VGA_BLACK);
    put_u32((uint32_t)(per_byte_x100 / 100ull));
    vga_putchar('.', VGA_LIGHT_GREEN, VGA_BLACK);
    put_u32((uint32_t)(per_byte_x100 % 100ull));
    vga_print(" cycles/byte   WC bytes ", VGA_LIGHT_GREEN, VGA_BLACK);
    put_u32((uint32_t)(g_wc_bytes >> 10));
    vga_print(" KB\n", VGA_LIGHT_GREEN, VGA_BLACK);
    vga_print("  (<10 = write-combining is on;  >50 = still uncached)\n",
              VGA_LIGHT_GREY, VGA_BLACK);

    fb_redraw();
    return 0;
}

int cmd_memtype(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    mtrr_dump();
    vga_print("fb: phys ", VGA_LIGHT_CYAN, VGA_BLACK);
    put_u32(g_phys);
    vga_print("  WC-covered ", VGA_LIGHT_CYAN, VGA_BLACK);
    put_u32((uint32_t)(g_wc_bytes >> 10));
    vga_print(" KB of ", VGA_LIGHT_CYAN, VGA_BLACK);
    put_u32((uint32_t)(((uint64_t)g_pitch * g_h) >> 10));
    vga_print(" KB\n", VGA_LIGHT_CYAN, VGA_BLACK);
    if (g_wc_bytes == 0ull) {
        vga_print("  ! no WC MTRR was installed — framebuffer writes stay uncached\n",
                  VGA_YELLOW, VGA_BLACK);
    }
    return 0;
}
