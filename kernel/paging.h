/* ============================================================================
 * zh_OS 1.0 (x86_64)
 * File:    kernel/paging.h
 * Version: 1.0
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
/* [ADD] M15：PTE 第 9 位（x86-64 留给 OS 用的三位之一）。
 * 置位表示"这一页是 COW 页"：父子共享、只读，谁写谁触发 #PF 再复制一份。 */
#define PAGE_COW        (1ULL << 9)

/* 建立 4 级页表并恒等映射低 1GB，然后加载 CR3。 */
void paging_init(void);

/* 返回当前 CR3（PML4 物理地址）。 */
uint64_t paging_get_cr3(void);

/* [ADD] M25：把物理区间映射成恒等可写（2MB 大页），帧缓冲用 */
int paging_map_2mb_range(uint64_t phys, uint64_t bytes);

/* [ADD] M34：帧缓冲专用 —— 同时把这段物理内存设成 write-combining。
 * 真机 2880x1800x32 一屏 20MB，走默认的 UC 会让滚屏/重绘慢到没法用。 */
int paging_map_2mb_range_type(uint64_t phys, uint64_t bytes, int wc);

#endif /* ZH_OS_PAGING_H */
