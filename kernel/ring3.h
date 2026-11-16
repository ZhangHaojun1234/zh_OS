/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    kernel/ring3.h
 * Version: 0.2beta
 * Brief:   M2 系统调用：进/出 ring3 的汇编入口声明（kernel/ring3.asm）。
 * ============================================================================ */

#ifndef ZH_OS_RING3_H
#define ZH_OS_RING3_H

#include <stdint.h>

/*
 * 进用户态。
 *   rip/rsp/rflags/cs/ss 这 5 个值就是 iretq 弹出来的 iret 帧；
 *   cs/ss 必须带 RPL=3（0x1b / 0x23），否则 iretq 只是"同特权级远返回"。
 *   返回：只有走 sys_exit -> usermode_resume 这条路时才会回到调用点，
 *         返回值是 void —— 回到调用点意味着用户态已经主动结束。
 */
void usermode_enter(uint64_t rip, uint64_t rsp, uint64_t rflags, uint64_t cs, uint64_t ss);

/*
 * 从 ring3 语境直接回内核调用点：不经过 iretq，也不回到中断栈上的 C 调用链。
 * 只在系统调用（sys_exit）里用。
 */
void usermode_resume(void) __attribute__((noreturn));

#endif /* ZH_OS_RING3_H */
