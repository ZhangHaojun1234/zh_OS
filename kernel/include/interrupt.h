/* ---------------------------------------------------------------
 * 中断/异常处理
 * --------------------------------------------------------------- */
#ifndef ZH_INTERRUPT_H
#define ZH_INTERRUPT_H

#include "types.h"

struct registers {
    u32 gs, fs, es, ds;
    u32 edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    u32 int_no, err_code;
    u32 eip, cs, eflags, useresp, ss;
};

typedef void (*irq_handler_t)(struct registers *regs);

void interrupt_init(void);
void irq_register(u8 irq, irq_handler_t handler);
void irq_unmask(u8 irq);
void irq_mask(u8 irq);
u32 irq_disable_all(void);
void irq_restore_all(u32 flags);

#endif
