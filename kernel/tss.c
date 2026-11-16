/* ============================================================================
 * zh_OS 1.0 (x86_64)
 * File:    kernel/tss.c
 * Version: 1.0
 * Brief:   M1 用户态：重建 7 项 GDT（+ 用户段 + TSS）、ltr 装载任务寄存器。
 * ============================================================================ */

#include "tss.h"

#include "gdt.h"

/* ------------------------------------------------------------------ */
/* 64 位 TSS 结构（104 字节，intel SDM Vol.3 8.7 图 8-11）             */
/* ------------------------------------------------------------------ */

struct tss_entry {
    uint32_t reserved0;         /* +0  ：保留 */
    uint64_t rsp0;              /* +4  ：ring0 栈顶（从 ring3 进中断时 CPU 用） */
    uint64_t rsp1;              /* +12 ：ring1 栈顶（未用） */
    uint64_t rsp2;              /* +20 ：ring2 栈顶（未用） */
    uint64_t reserved1;         /* +28 ：保留 */
    uint64_t ist[7];            /* +36 ：IST1..IST7（未用，全 0 表示不用独立中断栈） */
    uint64_t reserved2;         /* +92 ：保留 */
    uint16_t reserved3;         /* +100：保留 */
    uint16_t iomap_base;        /* +102：I/O 位图基址（104 = 无位图 = 拒绝一切端口 IO） */
} __attribute__((packed));

typedef char tss_size_check[(sizeof(struct tss_entry) == 104u) ? 1 : -1];

/* 从 ring3 进中断/异常时 CPU 使用的内核栈（TSS.RSP0 指向它的顶部）。 */
#define TSS_KERNEL_STACK_BYTES (16u * 1024u)

static struct tss_entry g_tss;
static uint8_t g_tss_kernel_stack[TSS_KERNEL_STACK_BYTES]
    __attribute__((aligned(16)));

/* 8 个 8 字节槽 = 7 项描述符，其中 TSS 占 2 个槽。 */
static uint64_t g_gdt[8];

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct gdt_ptr g_gdt_ptr;

/* ------------------------------------------------------------------ */
/* 描述符构造                                                         */
/* ------------------------------------------------------------------ */

/*
 * 长模式下的代码/数据段基址与限长被忽略，只有 access 与 flags 起作用。
 * gran=0xA0 即 G=1（粒度 4K）+ L=1（64 位代码段）。
 */
static uint64_t flat_desc(uint8_t access, uint8_t granularity)
{
    return (uint64_t)0xFFFFu
         | ((uint64_t)access << 40)
         | ((uint64_t)granularity << 48);
}

void tss_init(void)
{
    uint32_t i;

    /* ---- TSS 内容 ---- */
    g_tss.reserved0 = 0u;
    g_tss.rsp0 = tss_kernel_stack_top();
    g_tss.rsp1 = 0u;
    g_tss.rsp2 = 0u;
    g_tss.reserved1 = 0u;
    for (i = 0u; i < 7u; ++i) {
        g_tss.ist[i] = 0u;
    }
    g_tss.reserved2 = 0u;
    g_tss.reserved3 = 0u;
    /* I/O 位图基址 = 结构体大小 = 没有位图 → ring3 的 in/out 一律 #GP。 */
    g_tss.iomap_base = (uint16_t)sizeof(struct tss_entry);

    /* ---- 7 项 GDT ---- */
    g_gdt[0] = 0u;                        /* 0x00 空 */
    g_gdt[1] = flat_desc(0x9A, 0xA0);     /* 0x08 内核代码 P=1 DPL=0 S=1 type=1010 */
    g_gdt[2] = flat_desc(0x92, 0xA0);     /* 0x10 内核数据 P=1 DPL=0 S=1 type=0010 */
    g_gdt[3] = flat_desc(0xFA, 0xA0);     /* 0x18 用户代码 P=1 DPL=3 S=1 type=1010 */
    g_gdt[4] = flat_desc(0xF2, 0xA0);     /* 0x20 用户数据 P=1 DPL=3 S=1 type=0010 */

    /* 0x28 TSS：16 字节系统描述符，类型 0x9（可用的 64 位 TSS），DPL=0 */
    {
        const uint64_t base = (uint64_t)(uintptr_t)&g_tss;
        const uint64_t limit = (uint64_t)sizeof(struct tss_entry) - 1u;

        g_gdt[5] = (limit & 0xFFFFu)
                 | ((base & 0xFFFFFFu) << 16)
                 | (((base >> 24) & 0xFFu) << 32)
                 | ((uint64_t)0x89u << 40)   /* P=1 DPL=0 S=0 type=1001 */
                 | (((base >> 32) & 0xFFu) << 56);
        g_gdt[6] = base >> 32;               /* 高 32 位基址落在第二个槽 */
    }
    g_gdt[7] = 0u;

    g_gdt_ptr.limit = (uint16_t)(sizeof(g_gdt) - 1u);
    g_gdt_ptr.base = (uint64_t)(uintptr_t)&g_gdt[0];

    /* 装载新 GDT，并用远返回刷新 CS（0x08 不变，IDT 门里的选择子继续有效）。 */
    __asm__ volatile(
        "lgdt %0\n\t"
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
        : "m"(g_gdt_ptr), [code] "i"(GDT_KERNEL_CODE), [data] "i"(GDT_KERNEL_DATA)
        : "rax", "memory");

    /* 装载任务寄存器。不 ltr 的话，ring3 进中断时 CPU 找不到 RSP0 → 三重故障。 */
    __asm__ volatile("ltr %w0" : : "r"((uint16_t)GDT_TSS));
}

void tss_set_rsp0(uint64_t rsp0)
{
    g_tss.rsp0 = rsp0;
}

uint64_t tss_get_rsp0(void)
{
    return g_tss.rsp0;
}

uint64_t tss_kernel_stack_top(void)
{
    return (uint64_t)(uintptr_t)g_tss_kernel_stack
         + (uint64_t)TSS_KERNEL_STACK_BYTES;
}
