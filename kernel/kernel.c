/* ============================================================================
 * zh_OS 0.1beta (x86_64)
 * File:    kernel/kernel.c
 * Version: 0.1beta
 * Brief:   内核主入口：初始化各子系统，最后进入登录 + Shell 循环。
 * ============================================================================ */

#include <stdint.h>

#include "gdt.h"
#include "idt.h"
#include "isr.h"
#include "keyboard.h"
#include "login.h"
#include "paging.h"
#include "pic.h"
#include "shell.h"
#include "users.h"
#include "version.h"
#include "vga.h"
#include "../mm/heap.h"  // [ADD]
#include "pit.h"  // [ADD]
#include "history.h"  // [ADD]
#include "../fs/ramfs.h"  // [ADD]
#include "ata.h"  // [ADD]  B 组第 2 个功能：ATA PIO 驱动
#include "users_persist.h"  // [ADD]  B 组第 2 个功能：用户表持久化
// [ADD] 本文件已有 static klog(enum log_level,...)，不能包含 klog.h，改用别名声明
extern void klog_init(void); // [ADD]
extern void klog_ring(const char *fmt, ...); // [ADD]

/*
 * zh_OS 内核入口。
 *
 * 执行状态下述：
 *   GRUB（multiboot2）把控制权交到 boot/boot.asm 的 _start 时，CPU 处于 32 位保护模式。
 *   boot.asm 自行建立临时页表、开启 PAE/LME/分页并远跳进入 x86_64 长模式。
 *   因此进入 kernel_main 时：长模式已激活（由引导代码完成），
 *   而 GDT 与页表在本函数里由内核重新建立（gdt_init / paging_init）。
 *
 * 显示风格模仿 Linux 内核控制台：纯文本、无艺术字，只用 4 种颜色区分日志级别
 * （浅灰 info / 亮绿 ok / 黄 warn / 亮红 err）。时间戳等 PIT 定时器就绪后再加。
 *
 * 第 5 步：中断与键盘就绪后载入内存账户表（user_init），开中断，
 * 然后进入 "登录 -> Shell -> logout -> 重新登录" 的无限循环。
 * login_screen() 负责清屏与认证，shell_run() 负责彩色提示符与命令执行；
 * 内核本身不再解析任何用户输入的命令。
 */

#ifndef ZH_VGA_SELFTEST
#define ZH_VGA_SELFTEST 0
#endif

/* 版本号统一由 kernel/version.h 提供（ZH_OS_VERSION_TEXT）。 */

/* Linux printk 风格的日志级别：只用少数几种颜色 */
enum log_level {
    LOG_INFO = 0, /* 浅灰：普通信息，绝大多数输出 */
    LOG_OK,       /* 亮绿：成功 */
    LOG_WARN,     /* 黄：警告 */
    LOG_ERR       /* 亮红：错误 */
};

static enum vga_color level_color(enum log_level level)
{
    switch (level) {
    case LOG_OK:
        return VGA_LIGHT_GREEN;
    case LOG_WARN:
        return VGA_YELLOW;
    case LOG_ERR:
        return VGA_LIGHT_RED;
    case LOG_INFO:
    default:
        return VGA_LIGHT_GREY;
    }
}

/* 输出固定宽度十六进制数（0x + 16 位小写）。 */
static void klog_hex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char buf[19];

    buf[0] = '0';
    buf[1] = 'x';
    for (uint32_t i = 0; i < 16; ++i) {
        buf[2 + i] = digits[(value >> (60 - 4 * i)) & 0xFu];
    }
    buf[18] = '\0';

    vga_print(buf, VGA_LIGHT_GREY, VGA_BLACK);
}

static void klog(enum log_level level, const char *message)
{
    vga_println(message, level_color(level), VGA_BLACK);
}

/* 第 5 步：字符串工具与十进制输出分别由 users/、shell/ 本地实现。 */

/* 首行 banner，格式参考 Linux 的 "Linux version ..." 行 */
static void print_banner(void)
{
    /* 首行以统一版本号开头：zh_OS 0.1beta (x86_64) */
    vga_print(ZH_OS_VERSION_TEXT " (", VGA_WHITE, VGA_BLACK);
#if defined(__clang__)
    vga_print("clang ", VGA_WHITE, VGA_BLACK);
    vga_print(__clang_version__, VGA_WHITE, VGA_BLACK);
#elif defined(__GNUC__)
    vga_print("gcc ", VGA_WHITE, VGA_BLACK);
    vga_print(__VERSION__, VGA_WHITE, VGA_BLACK);
#else
    vga_print("unknown compiler", VGA_WHITE, VGA_BLACK);
#endif
    /* 单处理器内核：banner 不再声明 SMP，只写构建编号 #1。 */
    vga_print(") #1 ", VGA_WHITE, VGA_BLACK);
    vga_print(__DATE__, VGA_WHITE, VGA_BLACK);
    vga_print(" ", VGA_WHITE, VGA_BLACK);
    vga_println(__TIME__, VGA_WHITE, VGA_BLACK);
}

/*
 * 临时自检函数：16 种前景色各打印一行。
 *
 * 启动时默认不编译（ZH_VGA_SELFTEST=0），保持 Linux 风格的朴素输出。
 * 需要验证调色板时：ZH_VGA_SELFTEST=1 ./build.sh
 * 第 5 步已提供 Shell 的 color 命令（运行时 16 色板），这里保留编译期自检开关。
 */
#if ZH_VGA_SELFTEST
static void color_selftest(void)
{
    static const char *const names[16] = {
        "BLACK",      "BLUE",       "GREEN",        "CYAN",
        "RED",        "MAGENTA",    "BROWN",        "LIGHT_GREY",
        "DARK_GREY",  "LIGHT_BLUE", "LIGHT_GREEN",  "LIGHT_CYAN",
        "LIGHT_RED",  "LIGHT_MAGENTA", "YELLOW",    "WHITE",
    };

    klog(LOG_INFO, "vga: color selftest start");

    for (uint32_t i = 0; i < 16; ++i) {
        char line[80];
        uint32_t n = 0;

        line[n++] = '[';
        if (i >= 10) {
            line[n++] = (char)('0' + i / 10);
        }
        line[n++] = (char)('0' + i % 10);
        line[n++] = ']';
        line[n++] = ' ';

        const char *name = names[i];
        while (*name != '\0' && n < sizeof(line) - 1) {
            line[n++] = *name++;
        }
        while (n < 20) {
            line[n++] = ' ';
        }

        const char *sample = ": The quick brown fox jumps over the lazy dog";
        while (*sample != '\0' && n < sizeof(line) - 1) {
            line[n++] = *sample++;
        }
        line[n] = '\0';

        /* 黑色前景在黑底上不可见，该行改用浅灰背景 */
        const enum vga_color bg = (i == 0) ? VGA_LIGHT_GREY : VGA_BLACK;
        vga_println(line, (enum vga_color)i, bg);
    }

    klog(LOG_INFO, "vga: color selftest done");
}
#endif

/* ------------------------------------------------------------------ */
/* 第 5 步：命令实现全部移到 shell/commands.c                          */
/* ------------------------------------------------------------------ */

void kernel_main(uint32_t mb2_magic, uint32_t mb2_info)
{
    (void)mb2_magic;
    (void)mb2_info;

    gdt_init();
    paging_init();

    vga_init();

    heap_init(); // [ADD]
    klog_init(); // [ADD]
    history_init(); // [ADD]
    ramfs_init(); // [ADD]
    shell_set_cwd_fn(ramfs_get_cwd); // [ADD]
    print_banner();

    klog(LOG_INFO, "gdt: 3 entries, kernel code 0x08, kernel data 0x10");
    klog_ring("gdt: 3 entries, kernel code 0x08, kernel data 0x10"); // [ADD]

    klog(LOG_INFO, "paging: 4-level page tables, low 1GB identity-mapped (2MB pages)");
    klog_ring("paging: 4-level page tables, low 1GB identity-mapped (2MB pages)"); // [ADD]
    vga_print("paging: PML4 loaded into CR3 at ", VGA_LIGHT_GREY, VGA_BLACK);
    klog_hex(paging_get_cr3());
    vga_putchar('\n', VGA_LIGHT_GREY, VGA_BLACK);

    klog(LOG_INFO, "vga: text mode 80x25, framebuffer 0xb8000");
    klog_ring("vga: text mode 80x25, framebuffer 0xb8000"); // [ADD]
    klog(LOG_INFO, "console [tty0] enabled");
    klog_ring("console [tty0] enabled"); // [ADD]

    klog(LOG_OK, "Long mode active.");
    klog_ring("Long mode active."); // [ADD]

#if ZH_VGA_SELFTEST
    color_selftest();
#endif

    /* ---- 中断子系统：顺序必须是 IDT -> PIC -> 键盘驱动 -> 最后开中断 ---- */
    idt_init();
    klog(LOG_INFO, "idt: 256 entries, vectors 0-47 installed");
    klog_ring("idt: 256 entries, vectors 0-47 installed"); // [ADD]

    pic_init();
    klog(LOG_INFO, "pic: 8259A remapped, IRQ0-15 -> vectors 32-47");
    klog_ring("pic: 8259A remapped, IRQ0-15 -> vectors 32-47"); // [ADD]

    keyboard_init();
    klog(LOG_INFO, "keyboard: PS/2 IRQ1 enabled, 256-byte ring buffer");
    klog_ring("keyboard: PS/2 IRQ1 enabled, 256-byte ring buffer"); // [ADD]

    pit_init(100); // [ADD]

    /* 内存态账户表：在任何登录尝试之前载入。 */
    user_init();
    klog(LOG_INFO, "users: 2 accounts in RAM (root, zh), FNV-1a 64-bit hashes");
    klog_ring("users: 2 accounts in RAM (root, zh), FNV-1a 64-bit hashes"); // [ADD]

    klog(LOG_OK, "Interrupts enabled (sti).");
    klog_ring("Interrupts enabled (sti)."); // [ADD]
    __asm__ volatile("sti");

    /*
     * 第 5 步：登录 -> Shell -> logout -> 重新登录，无限循环。
     * login_screen() 负责清屏、绘制登录横幅并设置 zh_current_user；
     * shell_run() 负责彩色提示符与命令执行；用户执行 logout 时 shell_run()
     * 返回，这里重新进入登录流程。shutdown 命令直接停机，不会回到这里。
     */
    /* B 组第 2 个功能：启动期做一次磁盘检测 + 用户表载入（仅一次）。 */
    ata_init(); // [ADD] 检测主通道是否有硬盘；缓存"磁盘是否存在"标记
    users_load(); // [ADD] 从 LBA 200 尝试载入持久化账户表，失败静默保留默认
    if (ata_disk_present()) { // [ADD] 蓝字日志：有磁盘
        vga_print("[ATA] user table loaded from disk\n", VGA_BLUE, VGA_BLACK); // [ADD]
    } else { // [ADD] 蓝字日志：无磁盘（如从 ISO 启动）
        vga_print("[ATA] no disk, using defaults\n", VGA_BLUE, VGA_BLACK); // [ADD]
    } // [ADD]

    for (;;) {
        login_screen();
        ramfs_chdir("~"); // [ADD] 普通用户进自己家目录，root 进 /root；失败静默保持在 /
        shell_run();
    }
}
