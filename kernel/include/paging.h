/* ---------------------------------------------------------------
 * 分页 (32 位两级页表)
 * --------------------------------------------------------------- */
#ifndef ZH_PAGING_H
#define ZH_PAGING_H

#include "types.h"

#define PAGE_PRESENT    0x01
#define PAGE_WRITE      0x02
#define PAGE_USER       0x04

void paging_init(void);
int paging_enabled(void);
u32 *paging_pd_phys(void);
u32 paging_mapped_bytes(void);

#endif
