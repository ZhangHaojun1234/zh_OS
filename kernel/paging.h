/* ============================================================================
 * zh_OS 0.1beta (x86_64)
 * File:    kernel/paging.h
 * Version: 0.1beta
 * Brief:   页表接口声明。
 * ============================================================================ */

#ifndef ZH_OS_PAGING_H
#define ZH_OS_PAGING_H

#include <stdint.h>

#define PAGE_SIZE      4096ULL
#define PAGE_2MB       0x200000ULL
#define PAGE_1GB       0x40000000ULL

#define PAGE_PRESENT    (1ULL << 0)
#define PAGE_WRITABLE   (1ULL << 1)
#define PAGE_USER       (1ULL << 2)
#define PAGE_HUGE       (1ULL << 7)

/* 建立 4 级页表并恒等映射低 1GB，然后加载 CR3。 */
void paging_init(void);

/* 返回当前 CR3（PML4 物理地址）。 */
uint64_t paging_get_cr3(void);

#endif /* ZH_OS_PAGING_H */
