/* mtrr.h - MTRR 接口，就为了给帧缓冲开 write-combining */

#ifndef ZH_OS_MTRR_H
#define ZH_OS_MTRR_H

#include <stdint.h>

/* 把 [base, base+bytes) 尽可能覆盖成 WC 类型。
 * 返回被成功覆盖的字节数（0 = 一个空闲 MTRR 都没拿到）。
 * 若区间里有固件留下的、整段都落在帧缓冲范围内的 UC 项，会先把它清掉。 */
uint64_t mtrr_wc_range(uint64_t base, uint64_t bytes);

/* 打印 MTRRdefType 与所有有效的 variable MTRR，供 `memtype` 命令用。 */
void mtrr_dump(void);

#endif /* ZH_OS_MTRR_H */
