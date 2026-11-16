/* fb.h - 线性帧缓冲（LFB）文字控制台的接口。
 * 0xB8000 的文本模式分辨率是写死的，QEMU 下永远 80x25 / 720x400，想改分辨率没意义。
 * 正经做法是让 GRUB 切图形模式（VBE/GOP），内核拿 Multiboot2 的 framebuffer 标签，
 * 自己用点阵字体往上画 ， Linux 的 fbcon 就是这么干的。
 * 几何全从硬件给的值算：列 = 宽/8，行 = 高/16。800x600 -> 100x37。 */

#ifndef ZH_OS_FB_H
#define ZH_OS_FB_H

#include <stdint.h>
#include "vga.h"

#define FB_NONE      0
#define FB_RGB       1        /* framebuffer_type == 1：直色 */

/* 从 Multiboot2 信息结构里找 framebuffer 标签并初始化。
 * 返回 1 = 图形模式可用（此后字符走 fb_*），0 = 没有，保持文本模式。 */
int fb_init(uint32_t mb2_info);

int fb_active(void);

uint32_t fb_width(void);
uint32_t fb_height(void);
uint32_t fb_bpp(void);
uint32_t fb_pitch(void);
uint32_t fb_cols(void);
uint32_t fb_rows(void);

/* 字符控制台的三个原语（vga.c 在 fb 可用时转发到这里） */
void fb_clear(enum vga_color fg, enum vga_color bg);
void fb_putchar(char c, enum vga_color fg, enum vga_color bg);
void fb_scroll(void);
void fb_redraw(void);   /* [ADD] M26：按字符缓冲整屏重画（换模式后把内容接回来） */

/* 运行时改分辨率：走 Bochs VBE dispi 接口（QEMU / Bochs 的网卡在，
 * 真机上没有这个接口就返回 0）。成功后按新几何重算行列并清屏。 */
int fb_set_mode(uint32_t w, uint32_t h, uint32_t bpp);

/* [ADD] M34：字体缩放（1..4）。不动硬件模式，把 8x16 字模按 NxN 像素画大，
 * 真机那种 2880x1800 原生面板上 res 用不了，靠这个把字放大。 */
int fb_set_scale(uint32_t s);
uint32_t fb_scale(void);

/* [ADD] M34：帧缓冲被 MTRR 覆盖成 WC 的字节数（0 = 还是 UC，会非常慢） */
uint64_t fb_wc_bytes(void);

/* [ADD] M34：`bench` ， 量一段显存写入的 cycles/byte，判断 WC 有没有生效 */
int cmd_bench(int argc, char **argv);

/* [ADD] M34：`memtype` ， 打印 MTRRdefType 与全部 variable MTRR */
int cmd_memtype(int argc, char **argv);

/* Shell 命令 videomode（M25） */
int cmd_videomode(int argc, char **argv);
int cmd_res(int argc, char **argv);
int cmd_fontscale(int argc, char **argv);

#endif /* ZH_OS_FB_H */
