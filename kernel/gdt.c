/* ============================================================================
 * zh_OS 0.1beta (x86_64)
 * File:    kernel/gdt.c
 * Version: 0.1beta
 * Brief:   64 位 GDT 重建、lgdt 与远跳转刷新 CS。
 * ============================================================================ */

#include "gdt.h"

/*
 * 64 位 GDT：3 项
 *   0x00 空描述符
 *   0x08 内核代码段（L=1, DPL=0, 可执行可读）
 *   0x10 内核数据段（DPL=0, 可读写）
 * 长模式下基址与段限长被忽略，但 access/flag 位仍然决定必备属性。
 */

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct gdt_entry gdt[3];
static struct gdt_ptr gdt_ptr;

static void set_entry(int index, uint8_t access, uint8_t granularity)
{
    gdt[index].limit_low = 0xFFFF;
    gdt[index].base_low = 0;
    gdt[index].base_middle = 0;
    gdt[index].access = access;
    gdt[index].granularity = granularity;
    gdt[index].base_high = 0;
}

void gdt_init(void)
{
    set_entry(0, 0x00, 0x00);   /* 空描述符 */
    set_entry(1, 0x9A, 0xA0);   /* 0x08: P=1 DPL=0 S=1 Type=1010(代码,可读) ; L=1 */
    set_entry(2, 0x92, 0xA0);   /* 0x10: P=1 DPL=0 S=1 Type=0010(数据,可写) ; L=1 */

    gdt_ptr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdt_ptr.base = (uint64_t)&gdt[0];

    __asm__ volatile("lgdt %0" : : "m"(gdt_ptr));

    /* 远跳转刷新 CS，随后重载数据段寄存器 */
    __asm__ volatile(
        "pushq %[code]\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "mov %[data], %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        :
        : [code] "i"(GDT_KERNEL_CODE), [data] "i"(GDT_KERNEL_DATA)
        : "rax", "memory");
}
