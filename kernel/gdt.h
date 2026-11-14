/* ============================================================================
 * zh_OS 0.1beta (x86_64)
 * File:    kernel/gdt.h
 * Version: 0.1beta
 * Brief:   GDT 接口声明。
 * ============================================================================ */

#ifndef ZH_OS_GDT_H
#define ZH_OS_GDT_H

#include <stdint.h>

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10

/* 重建 64 位 GDT（空 / 内核代码 / 内核数据），lgdt 后远跳转刷新 CS。 */
void gdt_init(void);

#endif /* ZH_OS_GDT_H */
