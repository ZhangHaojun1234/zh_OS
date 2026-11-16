/* phys.h - M3 的接口：内存图解析加页帧位图分配器 */

#ifndef ZH_OS_PHYS_H
#define ZH_OS_PHYS_H

#include <stdint.h>

/*
 * 为什么要有这一层（M3 的目标）。之前的内核只有两样东西：kernel/paging.c 那 512 个 2MB 大页
 * 恒等映射（低 1GB 全给内核，没有"谁的"概念），和 mm/heap.c 那 2MB 静态池（.bss 里挖一块，
 * 跟物理内存真实分布无关）。也就是说内核根本不知道自己有多少内存、哪些能碰。
 * M3 补上 Linux memblock / buddy 那一步的前半段：从 GRUB 传进来的 Multiboot2 信息里读出 BIOS
 * 的 E820 内存图；用 1 bit = 1 帧（4KB）的位图记录哪些物理帧被占用；提供分配、释放、带对齐的
 * 连续分配，后面 M5 的独立地址空间、M6 的进程内核栈都从这里拿真实物理页。
 * 约定：本内核目前恒等映射，所以这些函数返回的"物理地址"可以直接当指针用。等 M5 换成按页
 * 映射之后，返回值就只能当物理地址看，必须过页表才能访问，到时候这一点要改。
 */

#define PHYS_PAGE_SIZE   4096ull
#define PHYS_MAX_REGIONS 24u

/* Multiboot2 / E820 的区域类型（只保留会用到的五种） */
enum phys_mem_type {
    PHYS_MEM_AVAILABLE        = 1,   /* 可用 RAM */
    PHYS_MEM_RESERVED         = 2,   /* 固件/保留，不可碰 */
    PHYS_MEM_ACPI_RECLAIMABLE = 3,   /* ACPI 可回收 */
    PHYS_MEM_NVS              = 4,   /* ACPI NVS，不可当普通内存 */
    PHYS_MEM_BADRAM           = 5,   /* 坏内存 */
};

typedef struct {
    uint64_t base;      /* 起始物理地址 */
    uint64_t length;    /* 长度（字节） */
    uint32_t type;     /* enum phys_mem_type */
} phys_region_t;

/*
 * 解析 Multiboot2 信息里的内存图（tag type = 6）并建立位图分配器。返回 0 成功、-1 magic
 * 不对、-2 没有内存图 tag、-3 内存图解析不了。必须在 vga_init() 之后调，因为会借用 shell
 * 的输出宏打一行 Memory: ...。
 */
int phys_init(uint32_t mb2_magic, uint32_t mb2_info);

/*
 * 分配 count 个连续 4KB 帧，起始物理地址按 align 字节对齐（align 传 0 或小于 4KB 时按 4KB
 * 算）。成功返回物理地址（已清零），失败返回 0。比如 2MB 大页窗口就是
 * phys_alloc_frames(512, 0x200000)。
 */
void *phys_alloc_frames(uint64_t count, uint64_t align);

/* 分配单个 4KB 帧（等价于 phys_alloc_frames(1, 0)）。 */
void *phys_alloc_frame(void);

/* 释放帧；地址必须是之前分配过的起点，否则打印错误并返回。 */
void phys_free_frames(void *addr, uint64_t count);
void phys_free_frame(void *addr);

/* 统计 */
/* [ADD] M15：页帧引用计数（COW fork 需要）。 */
void phys_ref_init(void);
void phys_share_frame(uint64_t pa);      /* 多一个映射者 */
int  phys_release_frame(uint64_t pa);    /* 少一个映射者，返回 1 = 真的归还了 */
uint32_t phys_ref_count(uint64_t pa);

uint64_t phys_total_frames(void);
uint64_t phys_used_frame_count(void);
uint64_t phys_free_frame_count(void);
uint64_t phys_total_bytes(void);

/* 内存图条目（供 memmap 命令显示） */
uint32_t phys_region_count(void);
const phys_region_t *phys_region_get(uint32_t index);

/* 内核映像的物理范围（链接脚本 _kernel_start / _kernel_end） */
uint64_t phys_kernel_start(void);
uint64_t phys_kernel_end(void);

/* 自测：分配/释放/连续性/对齐/越界释放，全部通过返回 0，否则返回失败序号。 */
int phys_self_test(void);

#endif /* ZH_OS_PHYS_H */
