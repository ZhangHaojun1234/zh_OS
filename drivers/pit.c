/* ============================================================================
 * zh_OS 0.1beta (x86_64)
 * File:    drivers/pit.c
 * Version: 0.1beta
 * Brief:   PIT 通道 0 定时器驱动：100Hz 方波、IRQ0 计数与 uptime 换算。
 * ============================================================================ */

#include "pit.h"

#include "pic.h"
#include "klog.h"  // [ADD]

/*
 * 硬件背景
 * --------
 * 8253/8254 的输入时钟约为 1193182 Hz。通道 0 的数据端口是 0x40，
 * 控制寄存器是 0x43。控制字 0x36 表示：
 *     通道 0、先写低字节再写高字节、模式 3（方波）、二进制计数。
 * 分频值 divisor = 1193182 / hz，写入后通道 0 即以该频率在 IRQ0 上触发。
 * PIC 已在第 3 步重映射 IRQ0..15 -> 向量 32..47，本文件不修改任何 PIC 代码，
 * 只调用已有的 pic_clear_mask(0) 打开 IRQ0（与 keyboard.c 打开 IRQ1 的方式一致）。
 *
 * EOI：按任务要求，pit_irq_handler() 末尾自行调用 pic_send_eoi(0)。
 * isr.c 追加的分发挂钩在调用本函数后直接 return，避免同一 IRQ 收到两次 EOI。
 *
 * 精度：100Hz 时 divisor=11931，实际频率约 100.0007 Hz，
 * pit_get_uptime_seconds() 用配置频率整除，教学用途误差可忽略。
 */

#define PIT_INPUT_HZ      1193182u
#define PIT_DEFAULT_HZ    100u
#define PIT_CHANNEL0_PORT 0x40u
#define PIT_COMMAND_PORT  0x43u
#define PIT_CTRL_CH0_SQW  0x36u

volatile uint64_t pit_ticks;

static uint32_t pit_frequency;

static inline void pit_outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

void pit_init(uint32_t hz)
{
    uint32_t frequency = (hz == 0u) ? PIT_DEFAULT_HZ : hz;
    uint32_t divisor;

    if (frequency > PIT_INPUT_HZ) {
        frequency = PIT_DEFAULT_HZ;
    }

    divisor = PIT_INPUT_HZ / frequency;
    if (divisor == 0u) {
        divisor = 1u;
    }
    if (divisor > 0xFFFFu) {
        divisor = 0xFFFFu;
    }

    pit_ticks = 0u;
    pit_frequency = frequency;

    pit_outb(PIT_COMMAND_PORT, PIT_CTRL_CH0_SQW);
    pit_outb(PIT_CHANNEL0_PORT, (uint8_t)(divisor & 0xFFu));
    pit_outb(PIT_CHANNEL0_PORT, (uint8_t)((divisor >> 8) & 0xFFu));

    /* 只调用已有 API，不修改 PIC 代码；打开 IRQ0 后 PIT 中断才能送达。 */
    pic_clear_mask(0);
    klog("pit initialized at %u Hz", hz); // [ADD]
}

uint64_t pit_get_ticks(void)
{
    return pit_ticks;
}

uint64_t pit_get_uptime_seconds(void)
{
    const uint32_t hz = (pit_frequency != 0u) ? pit_frequency : PIT_DEFAULT_HZ;
    return pit_ticks / hz;
}

void pit_irq_handler(void)
{
    ++pit_ticks;
    pic_send_eoi(0);   /* 任务要求：IRQ0 处理末尾必须发 EOI */
}
