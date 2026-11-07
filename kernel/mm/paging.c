#include "paging.h"
#include "pmm.h"
#include "printf.h"

#define PAGE_TABLES     16                          /* 16 * 4 MiB = 64 MiB */
#define IDENTITY_LIMIT  (PAGE_TABLES * 4u * 1024u * 1024u)

static u32 page_directory[1024] __attribute__((aligned(4096)));
static u32 page_tables[PAGE_TABLES][1024] __attribute__((aligned(4096)));

static u32 mapped_bytes;

void paging_init(void)
{
    for (u32 table = 0; table < PAGE_TABLES; table++) {
        for (u32 entry = 0; entry < 1024; entry++) {
            u32 addr = table * 0x400000u + entry * PAGE_SIZE;
            page_tables[table][entry] = addr | PAGE_PRESENT | PAGE_WRITE;
        }
        page_directory[table] = ((u32)(uintptr_t)page_tables[table])
                                | PAGE_PRESENT | PAGE_WRITE;
    }

    for (u32 i = PAGE_TABLES; i < 1024; i++) {
        page_directory[i] = 0;
    }

    __asm__ volatile("mov %0, %%cr3" : : "r"(page_directory) : "memory");

    u32 cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");

    mapped_bytes = IDENTITY_LIMIT;
}

int paging_enabled(void)
{
    u32 cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    return (cr0 & 0x80000000u) != 0;
}

u32 *paging_pd_phys(void)
{
    return page_directory;
}

u32 paging_mapped_bytes(void)
{
    return mapped_bytes;
}
