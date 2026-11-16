/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    kernel/isr.c
 * Version: 0.2beta
 * Brief:   异常/IRQ 分发、异常信息打印与 EOI。
 * ============================================================================ */

#include "isr.h"

#include "idt.h"  /* IRQ_VECTOR_BASE / IRQ_VECTOR_END 定义在 idt.h */
#include "pic.h"
#include "vga.h"
#include "pit.h"  // [ADD]
#include "syscall.h"  // [ADD]  M2 系统调用：int 0x80 分发给 syscall_dispatch
#include "task.h"  // [ADD]  M4 调度：IRQ0 抢占点 + 用户态异常杀任务
#include "../drivers/keyboard.h"  // [ADD] M17：Ctrl-C 检测
#include "../shell/shell.h"      // [ADD] M17：^C 提示
#include "vmm.h"  // [ADD]  M15 COW：缺页时先尝试自己修好（vmm_cow_fault）

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

static int handle_exception(struct regs *r)
{
    // [ADD] M15：先给 #PF 一次"自己修好自己"的机会 —— COW fork 之后，
    //  父子共享的页是只读的，谁写谁就会在这里收到一次 #PF；
    //  vmm_cow_fault() 复制一份、改成可写，然后我们直接返回 ring3 重新执行那条写指令。
    //  修好之后**不能**往下走（下面是要杀任务/panic 的路径）。
    if (r->vector == 14u && task_sched_active()) {
        uint64_t cr2_value;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2_value));
        if (vmm_page_fault(cr2_value, r->error_code) != 0) {
            return 1;    /* 已修复：iretq 回去重试那条写指令（ring0/ring3 都行） */
        }
    }

    // [ADD] M4：如果异常来自 ring3 且调度器正在接管 CPU，那就是"这个任务自己搞死自己"，
    //        不该 panic 整机 —— 杀掉该任务、换下一个接着跑。CS 的 RPL 位是唯一判据。
    if ((r->cs & 3u) == 3u && task_sched_active()) {
        task_fault(r);   /* 不返回：把 CPU 交给下一个任务 */
    }

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

    if (r->vector == 14u) { // [ADD]  #PF 的出错线性地址不在中断帧里，只在 CR2
        uint64_t cr2_value; // [ADD]
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2_value)); // [ADD]
        vga_print("  cr2        = ", VGA_LIGHT_GREY, VGA_BLACK); // [ADD]
        print_hex64(cr2_value, VGA_LIGHT_GREEN); // [ADD]
        vga_println("", VGA_LIGHT_GREY, VGA_BLACK); // [ADD]
    } // [ADD]

    vga_print("  rip        = ", VGA_LIGHT_GREY, VGA_BLACK);
    print_hex64(r->rip, VGA_LIGHT_GREEN);
    vga_println("", VGA_LIGHT_GREY, VGA_BLACK);

    vga_print("  rsp        = ", VGA_LIGHT_GREY, VGA_BLACK);
    print_hex64(r->rsp, VGA_LIGHT_GREEN);
    vga_println("", VGA_LIGHT_GREY, VGA_BLACK);

    vga_print("  rflags     = ", VGA_LIGHT_GREY, VGA_BLACK);
    print_hex64(r->rflags, VGA_LIGHT_GREEN);
    vga_println("", VGA_LIGHT_GREY, VGA_BLACK);

    /* [ADD] M1 用户态：中断帧里的 CS/SS 决定"异常发生在哪个特权级"。
     * CS 低两位是 RPL，0x1b = 用户代码段 + RPL 3，0x08 = 内核代码段。 */
    vga_print("  cs         = ", VGA_LIGHT_GREY, VGA_BLACK); // [ADD]
    print_hex64(r->cs, VGA_LIGHT_GREEN); // [ADD]
    vga_println("", VGA_LIGHT_GREY, VGA_BLACK); // [ADD]
    vga_print("  ss         = ", VGA_LIGHT_GREY, VGA_BLACK); // [ADD]
    print_hex64(r->ss, VGA_LIGHT_GREEN); // [ADD]
    vga_println("", VGA_LIGHT_GREY, VGA_BLACK); // [ADD]
    if ((r->cs & 3u) == 3u) { // [ADD]
        vga_println("  >> fault came from RING 3 (CPL=3): privilege level works.", // [ADD]
                    VGA_LIGHT_GREEN, VGA_BLACK); // [ADD]
    } else { // [ADD]
        vga_println("  >> fault came from RING 0 (kernel itself).", // [ADD]
                    VGA_YELLOW, VGA_BLACK); // [ADD]
    } // [ADD]

    /* [ADD] 中断总数：与进入 ring3 之前打印的计数相减，
     *       差值就是"在 CPL=3 期间被正确处理并返回的时钟中断次数"。 */
    vga_print("  interrupts = ", VGA_LIGHT_GREY, VGA_BLACK); // [ADD]
    print_hex64(total_interrupts, VGA_LIGHT_GREEN); // [ADD]
    vga_println("", VGA_LIGHT_GREY, VGA_BLACK); // [ADD]

    vga_println("System halted.", VGA_LIGHT_GREY, VGA_BLACK);
    return 0;      /* [ADD] M15：0 = 没能修好，调用方停在 hlt 里 */
}

static void handle_irq(struct regs *r)
{
    const uint8_t vector = (uint8_t)r->vector;
    const uint8_t irq = (uint8_t)(vector - IRQ_VECTOR_BASE);

    // [ADD] M4：IRQ0 就是抢占点。pit_irq_handler() 里已经发了 EOI，
    //        所以这里换栈换任务不会把 PIC 卡住。sched_tick 可能直接切换、不返回。
    if (vector == IRQ_VECTOR_BASE) { pit_irq_handler(); sched_tick(r); return; } // [ADD]
    if (vector == IRQ_VECTOR_BASE + 1) {
        /* IRQ1：PS/2 键盘。驱动内部只做入队，不做阻塞操作。 */
        keyboard_handle_irq();

        /* 坑（这轮踩了）：EOI 必须**先**发。第一次写这一支时把 return 放在
         * pic_send_eoi() 之前，结果键盘只响应第一个键、之后整条线被屏蔽 ——
         * 现象是登录界面敲完第一个字符就死了。 */
        pic_send_eoi(irq);

        /* [ADD] M17：Ctrl-C 不产生字符，而是给前台进程投一个 SIGINT。
         * 这里只投递（置 pending 位），真正送达在 interrupt_dispatch 出口的
         * task_check_signals() —— 也就是 Linux 的"中断里 send_sig，
         * 出门回用户态时才 kill 自己"。 */
        if (keyboard_take_ctrl_c()) {
            if (task_signal_foreground(ZH_SIGINT) == 0) {
                sh_print(SH_WARN, "[kernel] ^C -> SIGINT queued for the foreground process");
                sh_newline();
            }
        }
        /* [ADD] M18：Ctrl-Z 同理，只是换成 SIGTSTP（作业控制的"停住"） */
        if (keyboard_take_ctrl_z()) {
            if (task_signal_foreground(ZH_SIGTSTP) == 0) {
                sh_print(SH_WARN, "[kernel] ^Z -> SIGTSTP queued for the foreground process");
                sh_newline();
            }
        }
        return;   /* EOI 已发，这一支自己收尾 */
    }

    /* 未注册的 IRQ 也必须回 EOI，否则该线（及同级/低优先级线）不再触发 */
    pic_send_eoi(irq);
}

void interrupt_dispatch(struct regs *r)
{
    ++total_interrupts;

    if (r->vector < IRQ_VECTOR_BASE) {
        if (handle_exception(r) != 0) {
            /* [ADD] M15：COW / 按需缺页已修好 -> iretq 回 ring3 重试那条指令。
             * [ADD] M17：修好之后同样要查一次信号（否则"缺页打断 + ^C"会漏送）。 */
            task_check_signals(r);
            return;
        }
        for (;;) {
            __asm__ volatile("cli; hlt");
        }
    }

    if (r->vector <= IRQ_VECTOR_END) {
        handle_irq(r);
        task_check_signals(r);   /* [ADD] M17：回 ring3 之前的最后一站 */
        return;
    }

    /* [ADD] M2 系统调用：0x80 从 ring3 进来，走自己的分发器（不 panic）。 */
    if (r->vector == ZH_SYSCALL_VECTOR) {
        syscall_dispatch(r);
        task_check_signals(r);   /* [ADD] M17 */
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
