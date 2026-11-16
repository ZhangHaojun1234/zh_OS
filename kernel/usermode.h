/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    kernel/usermode.h
 * Version: 0.2beta
 * Brief:   M1 用户态：ring3 落点（用户窗口、iretq 帧）接口声明。
 * ============================================================================ */

#ifndef ZH_OS_USERMODE_H
#define ZH_OS_USERMODE_H

#include <stdint.h>

/*
 * M1：第一次把 CPU 送进 ring 3。
 *
 * 用户"进程"现在还不是真正的进程（没有地址空间隔离、没有调度），
 * 它只有三样东西：
 *   1) 一个独占的 2MB 大页窗口，代码放在窗口起点，栈放在窗口顶部；
 *   2) 页表每一级的 U/S 位都置 1，否则 ring3 取指立刻 #PF；
 *   3) 一个手工压出来的 iretq 帧（SS/RSP/RFLAGS/CS/RIP）。
 *
 * 用户代码刻意做了两件事，用来证明"特权级是真的生效了"：
 *   a) 直接往 VGA 文本缓冲写一串绿底白字（ring3 能读到那块内存，说明 U/S 位通了）；
 *   b) 紧接着执行 cli —— 这条指令在 CPL=3 下必然触发 #GP。
 * 内核的异常处理会打印 rip/cs，cs = 0x001b（RPL=3）即铁证。
 *
 * 注意：M1 的这次 #GP 是终点，内核按既有策略 panic + halt，
 * 需要重启才能回到 Shell。这是刻意的"一次性验收"设计。
 */
void usermode_self_test(void);

/* 用户代码落点（2MB 对齐窗口起点），供日志与对比 panic dump 使用。 */
uint64_t usermode_code_base(void);

#endif /* ZH_OS_USERMODE_H */
