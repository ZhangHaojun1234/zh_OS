/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    drivers/pit.h
 * Version: 0.2beta
 * Brief:   8253/8254 PIT 通道 0 定时器接口（IRQ0 / 向量 32）。
 * ============================================================================ */

#ifndef ZH_OS_DRIVERS_PIT_H
#define ZH_OS_DRIVERS_PIT_H

#include <stdint.h>

/*
 * 纯增量模块：只新增本头文件与 drivers/pit.c、shell/cmd_time.*，
 * 已有模块只追加 include 与一行挂钩 / 命令注册，不改动任何已有行。
 */

/* 自开机以来的 tick 数，由 IRQ0 处理函数递增；64 位长模式下读取是原子的。 */
extern volatile uint64_t pit_ticks;

/* 配置 PIT 通道 0 为指定频率（hz==0 时用默认 100Hz），并打开 IRQ0。 */
void pit_init(uint32_t hz);

/* 返回 tick 总数。 */
uint64_t pit_get_ticks(void);

/* 返回开机秒数 = pit_ticks / 配置的频率。 */
uint64_t pit_get_uptime_seconds(void);

/*
 * IRQ0 中断处理：递增 pit_ticks，末尾调用 pic_send_eoi(0)。
 * 由 isr.c 追加的一行分发挂钩调用；挂钩在调用后直接 return，
 * 因此 isr.c 末尾统一的 pic_send_eoi(irq) 不会重复发给 IRQ0。
 */
void pit_irq_handler(void);

#endif /* ZH_OS_DRIVERS_PIT_H */
