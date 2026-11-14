/* ============================================================================
 * zh_OS 0.1beta (x86_64)
 * File:    kernel/pic.c
 * Version: 0.1beta
 * Brief:   8259A 重映射、EOI 与 IRQ 屏蔽位开关。
 * ============================================================================ */

#include "pic.h"

/*
 * 8259A 可编程中断控制器初始化。
 *
 * 开机时 PIC 默认把 IRQ0..7 映射到向量 0x08..0x0F、IRQ8..15 映射到 0x70..0x77，
 * 与 CPU 异常（#DF=8、#GP=13 等）冲突，必须先重映射到 32..47。
 *
 * ICW 序列：
 *   ICW1 0x11 : 边沿触发、级联、需要 ICW4
 *   ICW2 0x20 : 主片向量基址 32；从片基址 40
 *   ICW3 0x04 : 主片 IRQ2 上挂从片；从片接到主片 IRQ2（值 2）
 *   ICW4 0x01 : 8086/88 模式
 */

#define ICW1_INIT 0x10
#define ICW1_ICW4 0x01
#define ICW4_8086 0x01

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

/* 向一个慢速设备端口写 0 制造短暂 I/O 延时，等待 PIC 就绪 */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

void pic_init(void)
{
    /* 保存原有屏蔽字 */
    const uint8_t mask1 = inb(PIC1_DATA);
    const uint8_t mask2 = inb(PIC2_DATA);

    outb(PIC1_COMMAND, (uint8_t)(ICW1_INIT | ICW1_ICW4));
    io_wait();
    outb(PIC2_COMMAND, (uint8_t)(ICW1_INIT | ICW1_ICW4));
    io_wait();

    outb(PIC1_DATA, 32); /* 主片 IRQ0..7  -> 向量 32..39 */
    io_wait();
    outb(PIC2_DATA, 40); /* 从片 IRQ8..15 -> 向量 40..47 */
    io_wait();

    outb(PIC1_DATA, 0x04); /* 主片：IRQ2 上挂从片 */
    io_wait();
    outb(PIC2_DATA, 0x02); /* 从片：级联到主片 IRQ2 */
    io_wait();

    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /* 恢复屏蔽字；随后由各驱动用 pic_clear_mask 打开需要的线 */
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_set_mask(uint8_t irq)
{
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq = (uint8_t)(irq - 8);
    }
    value = (uint8_t)(inb(port) | (uint8_t)(1u << irq));
    outb(port, value);
}

void pic_clear_mask(uint8_t irq)
{
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq = (uint8_t)(irq - 8);
    }
    value = (uint8_t)(inb(port) & (uint8_t)~(1u << irq));
    outb(port, value);
}
