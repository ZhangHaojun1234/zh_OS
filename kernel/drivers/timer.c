#include "timer.h"
#include "interrupt.h"
#include "io.h"

static volatile u64 ticks;

static void timer_irq(struct registers *regs)
{
    UNUSED(regs);
    ticks++;
}

void timer_init(void)
{
    u32 divisor = 1193182u / TIMER_HZ;

    outb(0x43, 0x36);
    outb(0x40, (u8)(divisor & 0xFF));
    outb(0x40, (u8)((divisor >> 8) & 0xFF));

    irq_register(0, timer_irq);
    irq_unmask(0);
}

u64 timer_ticks(void)
{
    return ticks;
}

u32 timer_uptime_ms(void)
{
    return (u32)((ticks * 1000u) / TIMER_HZ);
}

void timer_sleep_ms(u32 ms)
{
    u64 target = ticks + ((u64)ms * TIMER_HZ) / 1000u;
    if (target <= ticks) {
        target = ticks + 1;
    }
    while (ticks < target) {
        cpu_halt();
    }
}
