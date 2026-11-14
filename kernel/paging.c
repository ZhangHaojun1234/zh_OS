/* ============================================================================
 * zh_OS 0.1beta (x86_64)
 * File:    kernel/paging.c
 * Version: 0.1beta
 * Brief:   重建 4 级页表、2MB 大页恒等映射低 1GB 并加载 CR3。
 * ============================================================================ */

#include "paging.h"

/*
 * 4 级页表：PML4 -> PDPT -> PD -> PT
 * 本步只做恒等映射（虚拟地址 == 物理地址）：
 *   PML4[0] -> PDPT[0] -> PD，PD 内 512 个 2MB 大页 => 覆盖 0x0 ~ 0x40000000 (1GB)
 * 0xB8000（VGA 文本显存）、内核映像（0x100000 起）、内核栈都在低 1GB 内。
 */

static uint64_t pml4[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t pdpt[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t pd[512] __attribute__((aligned(PAGE_SIZE)));

static void zero_table(uint64_t *table)
{
    for (unsigned int i = 0; i < 512; ++i) {
        table[i] = 0;
    }
}

static inline uint64_t read_cr3(void)
{
    uint64_t value;
    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

static inline void write_cr3(uint64_t value)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(value) : "memory");
}

static inline void enable_pae(void)
{
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 5); /* CR4.PAE */
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
}

void paging_init(void)
{
    zero_table(pml4);
    zero_table(pdpt);
    zero_table(pd);

    /* PML4[0] -> PDPT（物理地址即符号地址，恒等映射） */
    pml4[0] = (uint64_t)pdpt | PAGE_PRESENT | PAGE_WRITABLE;

    /* PDPT[0] -> PD（不设置 HUGE：PDPT 仍指向页目录） */
    pdpt[0] = (uint64_t)pd | PAGE_PRESENT | PAGE_WRITABLE;

    /* PD 内 512 个 2MB 大页：0x0 ~ 0x40000000 */
    for (unsigned int i = 0; i < 512; ++i) {
        pd[i] = (uint64_t)i * PAGE_2MB | PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE;
    }

    enable_pae();
    write_cr3((uint64_t)pml4);
}

uint64_t paging_get_cr3(void)
{
    return read_cr3();
}
