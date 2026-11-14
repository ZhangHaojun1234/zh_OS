/* ============================================================================
 * zh_OS 0.1beta (x86_64)
 * File:    kernel/isr.c
 * Version: 0.1beta
 * Brief:   异常/IRQ 分发、异常信息打印与 EOI。
 * ============================================================================ */

#include "isr.h"

#include "idt.h"  /* IRQ_VECTOR_BASE / IRQ_VECTOR_END 定义在 idt.h */
#include "pic.h"
#include "vga.h"
#include "pit.h"  // [ADD]

/*
 * 异常与 IRQ 的默认处理。
 *
 * 0..31   CPU 异常：打印异常名与错误码后停住（cli + hlt）。
 * 32..47  硬件 IRQ：若是键盘（IRQ1 对应向量 33）先交给键盘驱动；
 *         其他未注册的 IRQ 只发 EOI 后返回，避免中断被永久屏蔽。
 */

#define EXCEPTION_COUNT 32
#define IRQ_COUNT       16

static const char *const exception_names[EXCEPTION_COUNT] = {
    "Divide Error (#DE)",
    "Debug (#DB)",
    "Non-Maskable Interrupt",
    "Breakpoint (#BP)",
    "Overflow (#OF)",
    "Bound Range Exceeded (#BR)",
    "Invalid Opcode (#UD)",
    "Device Not Available (#NM)",
    "Double Fault (#DF)",
    "Coprocessor Segment Overrun",
    "Invalid TSS (#TS)",
    "Segment Not Present (#NP)",
    "Stack-Segment Fault (#SS)",
    "General Protection Fault (#GP)",
    "Page Fault (#PF)",
    "Reserved (15)",
    "x87 Floating-Point (#MF)",
    "Alignment Check (#AC)",
    "Machine Check (#MC)",
    "SIMD Floating-Point (#XM)",
    "Virtualization (#VE)",
    "Control Protection (#CP)",
    "Reserved (22)",
    "Reserved (23)",
    "Reserved (24)",
    "Reserved (25)",
    "Reserved (26)",
    "Reserved (27)",
    "Hypervisor Injection (#HV)",
    "VMM Communication (#VC)",
    "Security Exception (#SX)",
    "Reserved (31)",
};

extern void keyboard_handle_irq(void);

static uint64_t total_interrupts;

static void print_hex64(uint64_t value, uint8_t color)
{
    static const char digits[] = "0123456789abcdef";
    char buf[19];

    buf[0] = '0';
    buf[1] = 'x';
    for (uint32_t i = 0; i < 16; ++i) {
        buf[2 + i] = digits[(value >> (60 - 4 * i)) & 0xFu];
    }
    buf[18] = '\0';

    /* 直接依赖 VGA 驱动，异常路径必须尽量简单可靠 */
    vga_print(buf, (enum vga_color)color, VGA_BLACK);
}

static void handle_exception(struct regs *r)
{
    const char *name = "Unknown Exception";

    if (r->vector < EXCEPTION_COUNT) {
        name = exception_names[r->vector];
    }

    vga_println("", VGA_LIGHT_GREY, VGA_BLACK);
    vga_print("KERNEL PANIC: ", VGA_LIGHT_RED, VGA_BLACK);
    vga_println(name, VGA_WHITE, VGA_BLACK);

    vga_print("  vector     = ", VGA_LIGHT_GREY, VGA_BLACK);
    print_hex64(r->vector, VGA_LIGHT_GREEN);
    vga_println("", VGA_LIGHT_GREY, VGA_BLACK);

    vga_print("  error code = ", VGA_LIGHT_GREY, VGA_BLACK);
    print_hex64(r->error_code, VGA_LIGHT_GREEN);
    vga_println("", VGA_LIGHT_GREY, VGA_BLACK);

    vga_print("  rip        = ", VGA_LIGHT_GREY, VGA_BLACK);
    print_hex64(r->rip, VGA_LIGHT_GREEN);
    vga_println("", VGA_LIGHT_GREY, VGA_BLACK);

    vga_print("  rsp        = ", VGA_LIGHT_GREY, VGA_BLACK);
    print_hex64(r->rsp, VGA_LIGHT_GREEN);
    vga_println("", VGA_LIGHT_GREY, VGA_BLACK);

    vga_print("  rflags     = ", VGA_LIGHT_GREY, VGA_BLACK);
    print_hex64(r->rflags, VGA_LIGHT_GREEN);
    vga_println("", VGA_LIGHT_GREY, VGA_BLACK);

    vga_println("System halted.", VGA_LIGHT_GREY, VGA_BLACK);
}

static void handle_irq(struct regs *r)
{
    const uint8_t vector = (uint8_t)r->vector;
    const uint8_t irq = (uint8_t)(vector - IRQ_VECTOR_BASE);

    if (vector == IRQ_VECTOR_BASE) { pit_irq_handler(); return; } // [ADD]
    if (vector == IRQ_VECTOR_BASE + 1) {
        /* IRQ1：PS/2 键盘。驱动内部只做入队，不做阻塞操作。 */
        keyboard_handle_irq();
    }

    /* 未注册的 IRQ 也必须回 EOI，否则该线（及同级/低优先级线）不再触发 */
    pic_send_eoi(irq);
}

void interrupt_dispatch(struct regs *r)
{
    ++total_interrupts;

    if (r->vector < IRQ_VECTOR_BASE) {
        handle_exception(r);
        for (;;) {
            __asm__ volatile("cli; hlt");
        }
    }

    if (r->vector <= IRQ_VECTOR_END) {
        handle_irq(r);
        return;
    }

    /* 48..255 未安装，安全起见直接停住 */
    vga_print("Unexpected interrupt vector ", VGA_LIGHT_RED, VGA_BLACK);
    print_hex64(r->vector, VGA_LIGHT_RED);
    vga_println("", VGA_LIGHT_GREY, VGA_BLACK);
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

uint64_t interrupt_count(void)
{
    return total_interrupts;
}
