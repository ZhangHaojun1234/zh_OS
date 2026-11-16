/* tss.h - 7 项 GDT 与 TSS 的声明 */

#ifndef ZH_OS_TSS_H
#define ZH_OS_TSS_H

#include <stdint.h>

/*
 * M1 特权级切换的基座。kernel/gdt.c 里的 GDT 只有 3 项（空、内核代码、内核数据），全部
 * DPL=0，没有 DPL=3 的段也没有 TSS，CPU 根本没有进位到 ring 3 的材料。本文件不动 gdt.c，
 * 另外重建一张 7 项 GDT：
 *   0x00  空描述符
 *   0x08  内核代码段  DPL=0，跟 gdt.h 的 GDT_KERNEL_CODE 一致，IDT 门继续可用
 *   0x10  内核数据段  DPL=0，跟 gdt.h 的 GDT_KERNEL_DATA 一致
 *   0x18  用户代码段  DPL=3
 *   0x20  用户数据段  DPL=3
 *   0x28  64 位 TSS，16 字节描述符，占 0x28 和 0x30 两个槽
 * TSS 在长模式下不再管任务切换，只承担两件事：RSP0，从 ring3 进中断或异常时 CPU 自动切到
 * 这个内核栈；I/O 位图基址，设成 sizeof(TSS) 表示没有位图，ring3 访问任何端口都 #GP。
 * 选择子约定（带 RPL 3 的形式在进用户态时用）：用户代码 0x1B = 0x18 | 3，
 * 用户数据 0x23 = 0x20 | 3。
 */

#define GDT_USER_CODE 0x18
#define GDT_USER_DATA 0x20
#define GDT_TSS       0x28

#define GDT_USER_CODE_RPL3 0x1B
#define GDT_USER_DATA_RPL3 0x23

/* 建立 7 项 GDT、lgdt + 远跳转刷新 CS、重载数据段、ltr 装载 TR。 */
void tss_init(void);

/* 读/写 TSS.RSP0：每次切换到某个用户任务前，必须把它指向该任务的内核栈顶。 */
void tss_set_rsp0(uint64_t rsp0);
uint64_t tss_get_rsp0(void);

/* 本内核为 ring3 预留的内核栈顶（16 字节对齐）。 */
uint64_t tss_kernel_stack_top(void);

#endif /* ZH_OS_TSS_H */
