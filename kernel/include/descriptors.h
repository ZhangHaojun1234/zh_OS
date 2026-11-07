/* ---------------------------------------------------------------
 * 全局描述符表 (GDT) / 中断描述符表 (IDT)
 * --------------------------------------------------------------- */
#ifndef ZH_DESCRIPTORS_H
#define ZH_DESCRIPTORS_H

#include "types.h"

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x18
#define GDT_USER_DATA   0x20
#define GDT_TSS         0x28

#define IDT_ENTRIES 256

struct PACKED gdt_entry {
    u16 limit_low;
    u16 base_low;
    u8 base_middle;
    u8 access;
    u8 granularity;
    u8 base_high;
};

struct PACKED gdt_ptr {
    u16 limit;
    u32 base;
};

struct PACKED idt_entry {
    u16 base_low;
    u16 selector;
    u8 zero;
    u8 flags;
    u16 base_high;
};

struct PACKED idt_ptr {
    u16 limit;
    u32 base;
};

void gdt_init(void);
void idt_init(void);
void idt_set_gate(u8 vector, u32 handler, u16 selector, u8 flags);

extern void gdt_flush(const struct gdt_ptr *ptr);
extern void idt_load(const struct idt_ptr *ptr);

#endif
