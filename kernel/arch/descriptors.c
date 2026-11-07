#include "descriptors.h"
#include "console.h"
#include "io.h"
#include "string.h"

static struct gdt_entry gdt[6];
static struct gdt_ptr gdt_pointer;
static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idt_pointer;

static void gdt_set_entry(int index, u32 base, u32 limit, u8 access, u8 granularity)
{
    gdt[index].base_low = (u16)(base & 0xFFFF);
    gdt[index].base_middle = (u8)((base >> 16) & 0xFF);
    gdt[index].base_high = (u8)((base >> 24) & 0xFF);
    gdt[index].limit_low = (u16)(limit & 0xFFFF);
    gdt[index].granularity = (u8)(((limit >> 16) & 0x0F) | (granularity & 0xF0));
    gdt[index].access = access;
}

void gdt_init(void)
{
    gdt_pointer.limit = (u16)(sizeof(gdt) - 1);
    gdt_pointer.base = (u32)&gdt;

    gdt_set_entry(0, 0, 0, 0, 0);
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xCF);   /* 内核代码 */
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xCF);   /* 内核数据 */
    gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xCF);   /* 用户代码 */
    gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xCF);   /* 用户数据 */
    gdt_set_entry(5, 0, 0, 0, 0);               /* TSS 占位 */

    gdt_flush(&gdt_pointer);
}

void idt_set_gate(u8 vector, u32 handler, u16 selector, u8 flags)
{
    idt[vector].base_low = (u16)(handler & 0xFFFF);
    idt[vector].base_high = (u16)((handler >> 16) & 0xFFFF);
    idt[vector].selector = selector;
    idt[vector].zero = 0;
    idt[vector].flags = flags;
}

extern u32 isr_stub_table[];
extern u32 irq_stub_table[];

void idt_init(void)
{
    idt_pointer.limit = (u16)(sizeof(idt) - 1);
    idt_pointer.base = (u32)&idt;

    memset(idt, 0, sizeof(idt));

    for (int i = 0; i < 32; i++) {
        idt_set_gate((u8)i, isr_stub_table[i], GDT_KERNEL_CODE, 0x8E);
    }
    for (int i = 0; i < 16; i++) {
        idt_set_gate((u8)(32 + i), irq_stub_table[i], GDT_KERNEL_CODE, 0x8E);
    }

    idt_load(&idt_pointer);
}
