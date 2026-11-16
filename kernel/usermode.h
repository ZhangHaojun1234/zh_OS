/* usermode.h - ring3 落点的接口：用户窗口与 iretq 帧 */

#ifndef ZH_OS_USERMODE_H
#define ZH_OS_USERMODE_H

#include <stdint.h>

/*
 * M1：第一次把 CPU 送进 ring 3。这时的用户"进程"还不算真进程，没有地址空间隔离也没有调度，
 * 只有三样东西：一个独占的 2MB 大页窗口，代码放窗口起点、栈放窗口顶部；页表每一级的 U/S 位
 * 都置 1，否则 ring3 取指立刻 #PF；一个手工压出来的 iretq 帧（SS/RSP/RFLAGS/CS/RIP）。
 * 用户代码刻意干了两件事来证明特权级真的生效：往 VGA 文本缓冲写一串绿底白字，ring3 能读到
 * 那块内存说明 U/S 通了；紧接着执行 cli，这条在 CPL=3 下必然 #GP。内核的异常处理会把 rip/cs
 * 打出来，cs = 0x001b（RPL=3）就是铁证。
 * 这次 #GP 是终点，内核按既有策略 panic + halt，得重启才能回 Shell。一次性验收，故意的。
 */
void usermode_self_test(void);

/* 用户代码落点（2MB 对齐窗口起点），供日志与对比 panic dump 使用。 */
uint64_t usermode_code_base(void);

#endif /* ZH_OS_USERMODE_H */
