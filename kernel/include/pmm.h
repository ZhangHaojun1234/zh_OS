/* ---------------------------------------------------------------
 * 物理内存管理 (位图分配器)
 * --------------------------------------------------------------- */
#ifndef ZH_PMM_H
#define ZH_PMM_H

#include "types.h"

#define PAGE_SIZE       4096u
#define PAGE_SHIFT      12
#define MAX_PHYS_MEM    (256u * 1024u * 1024u)
#define MAX_PAGES       (MAX_PHYS_MEM / PAGE_SIZE)

struct e820_entry {
    u64 base;
    u64 length;
    u32 type;
    u32 acpi_ext;
} PACKED;

#define E820_USABLE 1

void pmm_init(u32 memmap_count, const struct e820_entry *entries);
u32 pmm_total_pages(void);
u32 pmm_free_pages(void);
u32 pmm_used_pages(void);
u32 pmm_alloc_page(void);
void pmm_free_page(u32 addr);
u64 pmm_total_bytes(void);
u64 pmm_free_bytes(void);
void pmm_dump_map(void);

#endif
