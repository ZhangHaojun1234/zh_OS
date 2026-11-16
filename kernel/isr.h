/* ============================================================================
 * zh_OS 1.0 (x86_64)
 * File:    kernel/isr.h
 * Version: 1.0
 * Brief:   struct regs 与中断分发接口。
 * ============================================================================ */

#ifndef ZH_OS_ISR_H
#define ZH_OS_ISR_H

#include <stdint.h>

/*
 * 中断发生时栈上的寄存器快照。
 *
 * 内存布局（地址从低到高）由 kernel/interrupt.asm 的压栈顺序决定：
 *   interrupt.asm 最后压入 r15，所以 r15 位于最低地址，即结构体第一个字段。
 *   vector / error_code 由 stub 压入（无错误码的异常压入 0 占位）。
 *   rip / cs / rflags / rsp / ss 是 CPU 在 64 位模式下无条件压入的中断帧。
 */
struct regs {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;

    uint64_t vector;
    uint64_t error_code;

    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

/* 由 interrupt.asm 调用：统一分发异常与 IRQ */
void interrupt_dispatch(struct regs *r);

/* 供 console / kernel 判断：当前是否处理过中断 */
uint64_t interrupt_count(void);

#endif /* ZH_OS_ISR_H */
