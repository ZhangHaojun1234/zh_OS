/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    shell/cmd_time.c
 * Version: 0.2beta
 * Brief:   uptime（PIT tick 换算）与 date（CMOS RTC 读取）两个新命令。
 * ============================================================================ */

#include "cmd_time.h"

#include <stdint.h>

#include "pit.h"
#include "shell.h"

/*
 * 时间来源
 * --------
 * uptime 用 drivers/pit.c 的 pit_get_uptime_seconds()（PIT 100Hz tick 换算）。
 * date 直接读 CMOS RTC：索引端口 0x70、数据端口 0x71，
 * 寄存器 0x00 秒 / 0x02 分 / 0x04 时 / 0x07 日 / 0x08 月 / 0x09 年，
 * 状态寄存器 A(0x0A) 的 bit7 = UIP（更新进行中），状态寄存器 B(0x0B) 的
 * bit2 = 二进制模式（0 为 BCD）、bit1 = 24 小时模式（0 为 12 小时 + PM 位）。
 *
 * 已知限制：不处理时区、夏令时、闰秒，也不做世纪跨年的自动推断
 * （年份按 2000 + CMOS 两位年处理）。RTC 给出的是虚拟机固件时间，
 * 通常为宿主机 UTC；本命令原样显示，不做任何偏移换算。
 */

#define CMOS_INDEX_PORT 0x70u
#define CMOS_DATA_PORT  0x71u
#define CMOS_REG_STATUS_A 0x0Au
#define CMOS_REG_STATUS_B 0x0Bu
#define CMOS_UIP_BIT      0x80u
#define CMOS_BINARY_BIT   0x04u
#define CMOS_24HOUR_BIT   0x02u

typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint8_t year;
} rtc_raw_t;

static inline uint8_t rtc_inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void rtc_outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

/* 写索引时置 bit7 以关闭 NMI，避免读取过程中被 NMI 打断。 */
static uint8_t cmos_read(uint8_t reg)
{
    rtc_outb(CMOS_INDEX_PORT, (uint8_t)(reg | 0x80u));
    return rtc_inb(CMOS_DATA_PORT);
}

static uint8_t bcd_to_bin(uint8_t value, int binary_mode)
{
    if (binary_mode) {
        return value;
    }
    return (uint8_t)(((value >> 4) & 0x0Fu) * 10u + (value & 0x0Fu));
}

static void rtc_snapshot(rtc_raw_t *out)
{
    out->second = cmos_read(0x00u);
    out->minute = cmos_read(0x02u);
    out->hour   = cmos_read(0x04u);
    out->day    = cmos_read(0x07u);
    out->month  = cmos_read(0x08u);
    out->year   = cmos_read(0x09u);
}

static int rtc_same(const rtc_raw_t *a, const rtc_raw_t *b)
{
    return a->second == b->second && a->minute == b->minute &&
           a->hour == b->hour && a->day == b->day &&
           a->month == b->month && a->year == b->year;
}

static void put2(char *buf, uint32_t value)
{
    buf[0] = (char)('0' + ((value / 10u) % 10u));
    buf[1] = (char)('0' + (value % 10u));
}

static void put4(char *buf, uint32_t value)
{
    buf[0] = (char)('0' + ((value / 1000u) % 10u));
    buf[1] = (char)('0' + ((value / 100u) % 10u));
    buf[2] = (char)('0' + ((value / 10u) % 10u));
    buf[3] = (char)('0' + (value % 10u));
}

/* ------------------------------------------------------------------ */
/* uptime                                                             */
/* ------------------------------------------------------------------ */

int cmd_uptime(int argc, char **argv)
{
    const uint64_t total = pit_get_uptime_seconds();
    const uint64_t hours = total / 3600u;
    const uint64_t minutes = (total / 60u) % 60u;
    const uint64_t seconds = total % 60u;

    (void)argc;
    (void)argv;

    /* 标签亮青、数字亮绿 */
    sh_print(SH_WARN, "up ");
    sh_print_u32(SH_OK, (uint32_t)hours);
    sh_print(SH_WARN, " hours, ");
    sh_print_u32(SH_OK, (uint32_t)minutes);
    sh_print(SH_WARN, " minutes, ");
    sh_print_u32(SH_OK, (uint32_t)seconds);
    sh_println(SH_WARN, " seconds");
    return SHELL_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* date                                                               */
/* ------------------------------------------------------------------ */

int cmd_date(int argc, char **argv)
{
    rtc_raw_t first;
    rtc_raw_t second;
    uint8_t status_b;
    int binary_mode;
    int hour_24;
    uint8_t sec;
    uint8_t min;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint8_t year;
    uint32_t full_year;
    char date_buf[11];
    char time_buf[9];

    (void)argc;
    (void)argv;

    /* 等 UIP 清零；加次数上限，避免 CMOS 异常时把内核挂死。 */
    for (uint32_t i = 0; i < 100000u; ++i) {
        if ((cmos_read(CMOS_REG_STATUS_A) & CMOS_UIP_BIT) == 0u) {
            break;
        }
    }

    status_b = cmos_read(CMOS_REG_STATUS_B);
    binary_mode = (status_b & CMOS_BINARY_BIT) != 0u;
    hour_24 = (status_b & CMOS_24HOUR_BIT) != 0u;

    /* 连续读两次，完全一致才采用，避免跨秒读数撕裂。 */
    for (int attempt = 0; attempt < 3; ++attempt) {
        rtc_snapshot(&first);
        rtc_snapshot(&second);
        if (rtc_same(&first, &second)) {
            break;
        }
    }

    sec   = bcd_to_bin(first.second, binary_mode);
    min   = bcd_to_bin(first.minute, binary_mode);
    day   = bcd_to_bin(first.day, binary_mode);
    month = bcd_to_bin(first.month, binary_mode);
    year  = bcd_to_bin(first.year, binary_mode);

    if (hour_24) {
        hour = bcd_to_bin(first.hour, binary_mode);
    } else {
        /* 12 小时模式：bit7 为 PM 标志，需换算成 24 小时制。 */
        const int pm = (first.hour & 0x80u) != 0u;
        hour = bcd_to_bin((uint8_t)(first.hour & 0x7Fu), binary_mode);
        if (pm && hour < 12u) {
            hour = (uint8_t)(hour + 12u);
        }
        if (!pm && hour == 12u) {
            hour = 0u;
        }
    }

    full_year = 2000u + year;

    put4(&date_buf[0], full_year);
    date_buf[4] = '-';
    put2(&date_buf[5], month);
    date_buf[7] = '-';
    put2(&date_buf[8], day);
    date_buf[10] = '\0';

    put2(&time_buf[0], hour);
    time_buf[2] = ':';
    put2(&time_buf[3], min);
    time_buf[5] = ':';
    put2(&time_buf[6], sec);
    time_buf[8] = '\0';

    /* 日期亮黄、时间亮青，中间空格浅灰；不附加时区信息。 */
    sh_print(SH_WARN, date_buf);
    sh_print(SH_PLAIN, " ");
    sh_print(SH_INFO, time_buf);
    sh_newline();
    return SHELL_CONTINUE;
}
