/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    kernel/klog.c
 * Version: 0.2beta
 * Brief:   128 条内核日志环、极简格式化输出、dmesg 命令实现。
 * ============================================================================ */

#include "klog.h"

#include "pit.h"
#include "vga.h"
#include "../shell/shell.h"

/*
 * 设计说明
 * --------
 * 环形缓冲：klog_ring_buf[KLOG_SIZE]，head 指向下一条写入位置，count 为当前条数。
 * 写满后 head 回绕并覆盖最旧条目；dump 从 (head - count + KLOG_SIZE) 开始，
 * 按最旧 -> 最新的顺序打印。
 *
 * 初始化顺序：kernel_main 里 heap_init() 先于 klog_init() 调用，
 * 而 heap_init() 末尾会写第一条日志。因此 klog_init() 故意不重置 head/count
 * （.bss 已经把它们清零），只写一条就绪日志，避免把先写入的日志抹掉。
 * 同理，klog() 在 klog_init() 之前调用也是安全的。
 *
 * 时间戳：pit_get_ticks()。PIT 初始化之前的日志 tick 为 0，符合实际。
 *
 * 格式化：使用 GCC 内建的 __builtin_va_*，不链接 libc、不依赖 stdarg.h。
 * 支持 %s %d %u %x %c %%，未知转换按字面 '%' + 字符输出；
 * 单条超过 160 字节自动截断，保证 '\0' 结尾。
 *
 * 颜色：按本次任务要求，dump 的 tick 与 dmesg 标题用亮青，内容用浅灰
 * （这是全项目红/黄/绿三色配色之外的显式例外）。
 */

typedef struct {
    uint64_t tick;
    char text[KLOG_LINE_MAX];
} klog_entry_t;

static klog_entry_t klog_ring_buf[KLOG_SIZE];
static uint32_t klog_head;
static uint32_t klog_count;

/* ------------------------------------------------------------------ */
/* 字符串与数字工具（freestanding 无 libc）                           */
/* ------------------------------------------------------------------ */

static void put_char(char *buf, uint32_t *n, char c)
{
    if (*n + 1u < (uint32_t)KLOG_LINE_MAX) {
        buf[(*n)++] = c;
    }
}

static void put_str(char *buf, uint32_t *n, const char *s)
{
    if (s == 0) {
        s = "(null)";
    }
    while (*s != '\0') {
        put_char(buf, n, *s++);
    }
}

static void put_u64(char *buf, uint32_t *n, uint64_t value, uint32_t base)
{
    static const char digits[] = "0123456789abcdef";
    char tmp[20];
    uint32_t t = 0;

    if (value == 0u) {
        tmp[t++] = '0';
    }
    while (value != 0u) {
        tmp[t++] = digits[value % base];
        value /= base;
    }
    while (t != 0u) {
        put_char(buf, n, tmp[--t]);
    }
}

/* 把 tick 转成十进制字符串，返回长度；供 dump 做 8 列右对齐。 */
static uint32_t tick_to_str(uint64_t value, char *buf)
{
    char tmp[20];
    uint32_t t = 0;
    uint32_t n = 0;

    if (value == 0u) {
        tmp[t++] = '0';
    }
    while (value != 0u) {
        tmp[t++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (t != 0u) {
        buf[n++] = tmp[--t];
    }
    buf[n] = '\0';
    return n;
}

/* ------------------------------------------------------------------ */
/* 环形缓冲                                                           */
/* ------------------------------------------------------------------ */

static void klog_store(const char *line)
{
    klog_entry_t *entry = &klog_ring_buf[klog_head];
    uint32_t i = 0;

    entry->tick = pit_get_ticks();
    while (line[i] != '\0' && i + 1u < (uint32_t)KLOG_LINE_MAX) {
        entry->text[i] = line[i];
        ++i;
    }
    entry->text[i] = '\0';

    klog_head = (klog_head + 1u) % (uint32_t)KLOG_SIZE;
    if (klog_count < (uint32_t)KLOG_SIZE) {
        ++klog_count;
    }
}

static void klog_vwrite(const char *fmt, __builtin_va_list ap)
{
    char line[KLOG_LINE_MAX];
    uint32_t n = 0;

    if (fmt == 0) {
        fmt = "(null)";
    }

    while (*fmt != '\0' && n + 1u < (uint32_t)KLOG_LINE_MAX) {
        if (*fmt != '%') {
            put_char(line, &n, *fmt++);
            continue;
        }

        ++fmt;
        if (*fmt == '\0') {
            put_char(line, &n, '%');
            break;
        }

        switch (*fmt) {
        case '%':
            put_char(line, &n, '%');
            break;
        case 's':
            put_str(line, &n, __builtin_va_arg(ap, const char *));
            break;
        case 'c':
            put_char(line, &n, (char)__builtin_va_arg(ap, int));
            break;
        case 'd': {
            const int value = __builtin_va_arg(ap, int);
            if (value < 0) {
                put_char(line, &n, '-');
                put_u64(line, &n, (uint64_t)(-(int64_t)value), 10u);
            } else {
                put_u64(line, &n, (uint64_t)value, 10u);
            }
            break;
        }
        case 'u':
            put_u64(line, &n, (uint64_t)__builtin_va_arg(ap, unsigned int), 10u);
            break;
        case 'x':
            put_u64(line, &n, (uint64_t)__builtin_va_arg(ap, unsigned int), 16u);
            break;
        default:
            put_char(line, &n, '%');
            put_char(line, &n, *fmt);
            break;
        }
        ++fmt;
    }

    line[n] = '\0';
    klog_store(line);
}

void klog(const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    klog_vwrite(fmt, ap);
    __builtin_va_end(ap);
}

void klog_ring(const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    klog_vwrite(fmt, ap);
    __builtin_va_end(ap);
}

void klog_init(void)
{
    /* 不重置 head/count：heap_init() 已先写入一条日志，不能抹掉。 */
    klog("kernel log ring ready, %u slots", (unsigned int)KLOG_SIZE);
}

/* ------------------------------------------------------------------ */
/* 输出                                                               */
/* ------------------------------------------------------------------ */

void klog_dump(void)
{
    const uint32_t start =
        (klog_head + (uint32_t)KLOG_SIZE - klog_count) % (uint32_t)KLOG_SIZE;
    char num[21];

    for (uint32_t i = 0; i < klog_count; ++i) {
        const klog_entry_t *entry =
            &klog_ring_buf[(start + i) % (uint32_t)KLOG_SIZE];
        const uint32_t len = tick_to_str(entry->tick, num);

        vga_print("[", VGA_LIGHT_CYAN, VGA_BLACK);
        for (uint32_t pad = len; pad < 8u; ++pad) {
            vga_print(" ", VGA_LIGHT_CYAN, VGA_BLACK);
        }
        vga_print(num, VGA_LIGHT_CYAN, VGA_BLACK);
        vga_print("] ", VGA_LIGHT_CYAN, VGA_BLACK);
        vga_println(entry->text, VGA_LIGHT_GREY, VGA_BLACK);
    }
}

/* ------------------------------------------------------------------ */
/* dmesg 命令                                                         */
/* ------------------------------------------------------------------ */

int cmd_dmesg(int argc, char **argv)
{
    const int stress = (argc >= 2);

    (void)argv;

    if (stress) {
        /* 写 139 条编号日志 + 1 条格式测试 = 140 条，超过 128 槽，
           最旧 12 条应被覆盖，用于验证环回绕、顺序与全部格式符。 */
        for (uint32_t i = 0; i < 139u; ++i) {
            klog("ring stress %u", i);
        }
        klog("fmt test: s=%s d=%d u=%u x=%x c=%c p=%%",
             "ok", -42, 42u, 0x2Au, 'Z');
    }

    vga_println("=== kernel log ===", VGA_LIGHT_CYAN, VGA_BLACK);
    klog_dump();

    if (stress) {
        vga_println("dmesg: stress done (140 writes, 128 slots)",
                    VGA_LIGHT_CYAN, VGA_BLACK);
    }
    return SHELL_CONTINUE;
}
