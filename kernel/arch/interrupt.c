#include "interrupt.h"
#include "descriptors.h"
#include "io.h"
#include "printf.h"

static irq_handler_t irq_handlers[16];

static void pic_remap(void)
{
    u8 mask1 = inb(0x21);
    u8 mask2 = inb(0xA1);

    outb(0x20, 0x11);
    io_wait();
    outb(0xA0, 0x11);
    io_wait();
    outb(0x21, 0x20);       /* 主 PIC 偏移 0x20 */
    io_wait();
    outb(0xA1, 0x28);       /* 从 PIC 偏移 0x28 */
    io_wait();
    outb(0x21, 0x04);
    io_wait();
    outb(0xA1, 0x02);
    io_wait();
    outb(0x21, 0x01);
    io_wait();
    outb(0xA1, 0x01);
    io_wait();

    outb(0x21, mask1);
    outb(0xA1, mask2);

    outb(0x21, 0xFF);       /* 先全部屏蔽 */
    outb(0xA1, 0xFF);
}

static const char *exception_name(u32 num)
{
    static const char *names[32] = {
        "Divide-by-zero", "Debug", "NMI", "Breakpoint",
        "Overflow", "Bound range", "Invalid opcode", "No FPU",
        "Double fault", "FPU overrun", "Invalid TSS", "Segment not present",
        "Stack fault", "General protection fault", "Page fault", "Reserved",
        "x87 FPU", "Alignment check", "Machine check", "SIMD",
        "Virtualization", "Control protection", "Reserved", "Reserved",
        "Reserved", "Reserved", "Reserved", "Reserved",
        "Hypervisor", "VMM communication", "Security", "Reserved",
    };
    return names[num < 32 ? num : 0];
}

void isr_handler(struct registers *regs)
{
    if (regs->int_no < 32) {
        kprintf("\n[exception] %s (int %u, err 0x%x) at eip=0x%x\n",
                exception_name(regs->int_no), regs->int_no,
                regs->err_code, regs->eip);
        kpanic("unhandled CPU exception %u", regs->int_no);
    }
}

void irq_handler(struct registers *regs)
{
    u32 irq = regs->int_no - 32;

    if (irq < 16 && irq_handlers[irq]) {
        irq_handlers[irq](regs);
    }

    if (irq >= 8) {
        outb(0xA0, 0x20);
    }
    outb(0x20, 0x20);
}

void irq_register(u8 irq, irq_handler_t handler)
{
    if (irq < 16) {
        irq_handlers[irq] = handler;
    }
}

void irq_unmask(u8 irq)
{
    u16 port = (irq < 8) ? 0x21 : 0xA1;
    u8 bit = (u8)(irq & 7);
    u8 mask = inb(port);
    mask &= (u8)~(1u << bit);
    outb(port, mask);
}

void irq_mask(u8 irq)
{
    u16 port = (irq < 8) ? 0x21 : 0xA1;
    u8 bit = (u8)(irq & 7);
    u8 mask = inb(port);
    mask |= (u8)(1u << bit);
    outb(port, mask);
}

u32 irq_disable_all(void)
{
    u32 flags = irq_save();
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
    return flags;
}

void irq_restore_all(u32 flags)
{
    outb(0x21, 0x00);
    outb(0xA1, 0x00);
    if (flags & 0x200) {
        cpu_sti();
    }
}

void interrupt_init(void)
{
    pic_remap();
    idt_init();
}
