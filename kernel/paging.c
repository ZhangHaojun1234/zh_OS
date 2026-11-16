/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    kernel/paging.c
 * Version: 0.2beta
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
    /*
     * [ADD] M11：打开 SSE 状态保存。
     *   CR4.OSFXSR(9)     = 1 —— 内核支持 FXSAVE/FXRSTOR 及其它 SSE 指令；
     *   CR4.OSXMMEXCPT(10)= 1 —— 未屏蔽的 SIMD 异常走 #XF 而不是 #UD。
     * 不置这两位的后果：gcc 自动向量化出来的 `movq (%rip),%xmm0` /
     * `movaps` 在 ring3 里直接变成 #UD（vector 6）—— 这一步就是这么踩进去的。
     * Linux 在 cr4_init_shadow() 里做的是同一件事。
     */
    cr4 |= (1ULL << 9) | (1ULL << 10);
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
}

/*
 * [ADD] M11：确认 CR0 的 EM(2) / TS(3) 是 0。
 *   EM=1 会把所有 x87 指令变成 #UD；TS=1 会让首次 SIMD 访问抛 #NM。
 * 有些引导环境会留下这两个位，所以这里清一次，保证用户态能用浮点/SIMD。
 */
static inline void enable_fpu(void)
{
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~((1ULL << 2) | (1ULL << 3));   /* 清 EM 与 TS */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
    __asm__ volatile("fninit");            /* x87 复位到已知状态 */
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
    enable_fpu();   /* [ADD] M11：清 CR0.EM/TS，用户态才能用 x87/SIMD */
    write_cr3((uint64_t)pml4);
}

uint64_t paging_get_cr3(void)
{
    return read_cr3();
}

/* ==================================================================== */
/* [ADD] M25：把一段物理区间映射成内核可写的恒等映射（2MB 大页）        */
/*                                                                    */
/* 为什么要单独写这个：帧缓冲通常落在 PCI 空洞里（QEMU std VGA 的 LFB    */
/* 在 0xFD000000，接近 3.95GB），而 paging_init 只恒等映射了低 1GB。     */
/* 不改这一段，往 fb 写一个像素就会 #PF。                              */
/*                                                                    */
/* 做法和 Linux 的 ioremap() 一个思路：按 2MB 粒度建 PTE（HUGE），      */
/* 页表本身从内核 .bss 里的静态池拿（内核镜像在 1MB 处，VA==PA，所以     */
/* 直接把指针当物理地址填进页表是安全的）。                              */
/* ==================================================================== */

#define MAP_PD_POOL 8u    /* 最多 8 张页目录 = 8GB 覆盖，够用 */
#define PHYS_ADDR_MASK 0x000FFFFFFFFFF000ull

static uint64_t pd_pool[MAP_PD_POOL][512] __attribute__((aligned(PAGE_SIZE)));
static uint32_t pd_pool_used = 0u;

static void invlpg_one(uint64_t va)
{
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

int paging_map_2mb_range(uint64_t phys, uint64_t bytes)
{
    uint64_t start = phys & ~(PAGE_2MB - 1ull);
    uint64_t end = phys + bytes - 1ull;
    uint64_t addr;
    uint64_t *pdpt_ptr;

    if (bytes == 0ull) {
        return 1;
    }
    /* 只处理 PML4[0] 覆盖的 512GB（低半区），帧缓冲不会跑到那边去 */
    if ((start >> 39) != 0ull || (end >> 39) != 0ull) {
        return 0;
    }
    if ((pml4[0] & PAGE_PRESENT) == 0ull) {
        return 0;
    }
    pdpt_ptr = (uint64_t *)(uintptr_t)(pml4[0] & PHYS_ADDR_MASK);

    for (addr = start; addr <= end; addr += PAGE_2MB) {
        uint32_t i3 = (uint32_t)((addr >> 30) & 511ull);
        uint32_t i2 = (uint32_t)((addr >> 21) & 511ull);
        uint64_t *pd_ptr;

        if ((pdpt_ptr[i3] & PAGE_PRESENT) == 0ull) {
            if (pd_pool_used >= MAP_PD_POOL) {
                return 0;
            }
            pd_ptr = pd_pool[pd_pool_used++];
            zero_table(pd_ptr);
            pdpt_ptr[i3] = (uint64_t)(uintptr_t)pd_ptr | PAGE_PRESENT | PAGE_WRITABLE;
        }
        pd_ptr = (uint64_t *)(uintptr_t)(pdpt_ptr[i3] & PHYS_ADDR_MASK);
        if ((pd_ptr[i2] & PAGE_PRESENT) == 0ull) {
            pd_ptr[i2] = (addr & ~(PAGE_2MB - 1ull)) | PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE;
        }
        invlpg_one(addr);
        if (addr >= (0xFFFFFFFFull - PAGE_2MB)) {
            break;
        }
    }
    return 1;
}
