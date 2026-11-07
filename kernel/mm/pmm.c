#include "pmm.h"
#include "printf.h"
#include "string.h"

#define BITMAP_WORDS (MAX_PAGES / 32)
#define RESERVED_TOP 0x00100000u    /* 低端 1 MiB 保留给 BIOS/VGA/引导 */

static u32 bitmap[BITMAP_WORDS];
static u32 total_pages;
static u32 free_pages;

static inline void bitmap_set(u32 page)
{
    bitmap[page / 32] |= (1u << (page % 32));
}

static inline void bitmap_clear(u32 page)
{
    bitmap[page / 32] &= ~(1u << (page % 32));
}

static inline int bitmap_test(u32 page)
{
    return (bitmap[page / 32] >> (page % 32)) & 1u;
}

static void reserve_range(u64 base, u64 length)
{
    u64 start = ALIGN_DOWN(base, PAGE_SIZE);
    u64 end = ALIGN_UP(base + length, PAGE_SIZE);

    if (end > MAX_PHYS_MEM) {
        end = MAX_PHYS_MEM;
    }
    if (start >= end) {
        return;
    }

    for (u64 addr = start; addr < end; addr += PAGE_SIZE) {
        u32 page = (u32)(addr / PAGE_SIZE);
        if (!bitmap_test(page)) {
            bitmap_set(page);
            if (free_pages) {
                free_pages--;
            }
        }
    }
}

void pmm_init(u32 memmap_count, const struct e820_entry *entries)
{
    memset(bitmap, 0xFF, sizeof(bitmap));   /* 先标记全部已用 */
    total_pages = 0;
    free_pages = 0;

    for (u32 i = 0; i < memmap_count; i++) {
        const struct e820_entry *e = &entries[i];
        if (e->type != E820_USABLE) {
            continue;
        }
        u64 start = ALIGN_UP(e->base, PAGE_SIZE);
        u64 end = ALIGN_DOWN(e->base + e->length, PAGE_SIZE);

        if (end > MAX_PHYS_MEM) {
            end = MAX_PHYS_MEM;
        }
        if (start >= end) {
            continue;
        }

        total_pages += (u32)((end - start) / PAGE_SIZE);
        for (u64 addr = start; addr < end; addr += PAGE_SIZE) {
            bitmap_clear((u32)(addr / PAGE_SIZE));
        }
    }

    /* 保留低端内存、内核自身以及内核栈 */
    reserve_range(0, RESERVED_TOP);
    extern u8 kernel_start[];
    extern u8 kernel_end[];
    reserve_range((u64)(uintptr_t)kernel_start,
                  (u64)((uintptr_t)kernel_end - (uintptr_t)kernel_start));
    reserve_range(0x8F000, 0x1000);
    reserve_range(0x90000, 0x10000);

    free_pages = 0;
    for (u32 page = 0; page < MAX_PAGES; page++) {
        if (!bitmap_test(page)) {
            free_pages++;
        }
    }
    if (free_pages > total_pages) {
        total_pages = free_pages;
    }
}

u32 pmm_total_pages(void)
{
    return total_pages;
}

u32 pmm_free_pages(void)
{
    return free_pages;
}

u32 pmm_used_pages(void)
{
    return total_pages - free_pages;
}

u32 pmm_alloc_page(void)
{
    for (u32 word = (RESERVED_TOP / PAGE_SIZE) / 32; word < BITMAP_WORDS; word++) {
        if (bitmap[word] == 0xFFFFFFFFu) {
            continue;
        }
        for (u32 bit = 0; bit < 32; bit++) {
            u32 page = word * 32 + bit;
            if (!bitmap_test(page)) {
                bitmap_set(page);
                if (free_pages) {
                    free_pages--;
                }
                memset((void *)(uintptr_t)(page * PAGE_SIZE), 0, PAGE_SIZE);
                return page * PAGE_SIZE;
            }
        }
    }
    return 0;
}

void pmm_free_page(u32 addr)
{
    u32 page = addr / PAGE_SIZE;
    if (addr < RESERVED_TOP || page >= MAX_PAGES) {
        return;
    }
    if (bitmap_test(page)) {
        bitmap_clear(page);
        free_pages++;
    }
}

u64 pmm_total_bytes(void)
{
    return (u64)total_pages * PAGE_SIZE;
}

u64 pmm_free_bytes(void)
{
    return (u64)free_pages * PAGE_SIZE;
}

void pmm_dump_map(void)
{
    u32 rows = 0;
    u32 run_start = 0;
    int run_used = -1;

    kprintf("  范围(十六进制)          状态\n");
    for (u32 page = 0; page < MAX_PAGES; page++) {
        int used = bitmap_test(page);
        if (run_used == -1) {
            run_start = page;
            run_used = used;
            continue;
        }
        if (used != run_used || page + 1 == MAX_PAGES) {
            u32 end_page = (used != run_used) ? page : page + 1;
            if (end_page - run_start >= 256 || run_used == 0) {
                kprintf("  %08x - %08x  %s\n", run_start * PAGE_SIZE,
                        end_page * PAGE_SIZE, run_used ? "已用" : "可用");
                rows++;
            }
            run_start = page;
            run_used = used;
        }
    }
    UNUSED(rows);
}
