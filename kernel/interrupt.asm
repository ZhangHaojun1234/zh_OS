
; interrupt.asm - 64 位 ISR / IRQ stub，48 个
; 栈形状先说清楚。进 stub 时 CPU 已经按 IA-32e 规则压好中断帧，五个 8 字节无条件压，
; 不管有没有特权级切换：rip、cs、rflags、rsp、ss（由低到高）。
; 带错误码的异常，错误码在 rip 上面，也就是先压进来。
; 统一化：有错误码的（8/10/11/12/13/14/17/21/29/30）CPU 已经压了，stub 只补压中断号；
; 其余异常和全部 IRQ 先补一个 0 当占位错误码，再压中断号。这样进 common 时栈形状都一样
; （形状见 isr.h 的 struct regs）。
; 16 字节对齐得自己来：中断可以打断在任何位置，被中断代码的 rsp 对齐状态不可知。
; 40（CPU 帧）+ 16（错误码加中断号）+ 120（15 个通用寄存器）= 176，是 16 的倍数，靠不住。
; 所以 common 里先把 rsp 记到 rbx，and 到 16 字节边界再 call interrupt_dispatch，完了从 rbx 恢复。
; 向下对齐只挪栈指针，不动已保存的寄存器（保存区在更高地址），返回地址落在保存区下方的空闲栈上。

bits 64

section .text

extern interrupt_dispatch

; 两种 stub 宏
%macro ISR_NOERR 1
global isr%1
isr%1:
    push qword 0            ; 占位错误码，保证栈布局统一
    push qword %1           ; 中断号
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push qword %1           ; 错误码已由 CPU 压入，只补中断号
    jmp isr_common
%endmacro

; 0..31 异常
;   带错误码：8 双故障、10 无效 TSS、11 段不存在、12 栈段错误、13 通用保护、
;             14 缺页、17 对齐检查、21 控制保护、29 VMM、30 安全异常
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

; 32..47 硬件 IRQ（PIC 重映射后），均不带错误码
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

; [ADD] 128 = 0x80：M2 的系统调用门
;   软件中断不压错误码，所以用 ISR_NOERR（先补 0 占位，再补向量号），
;   栈形状与 isr_common 完全一致，可以直接复用下面的公共出入口。
;   这个 stub 不进 isr_stub_table（表只有 0..47），由 syscall_init() 单独
;   用 idt_set_gate(0x80, isr128, 0, 0xEE) 挂上门 ， 0xEE 的 DPL=3 才是
;   "ring3 允许触发"的关键。
ISR_NOERR 128

; 公共出入口
isr_common:
    cld                         ; SysV ABI 要求方向标志为 0

    ; 压入 15 个通用寄存器。压栈顺序决定 struct regs 的字段顺序（逆序）。
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp                ; 第一个参数：struct regs *
    mov rbx, rsp                ; 保存栈基址（rbx 已在上面存过，可安全复用）
    and rsp, 0xFFFFFFFFFFFFFFF0 ; 16 字节对齐
    call interrupt_dispatch
    mov rsp, rbx                ; 恢复

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16                 ; 丢弃 中断号 + 错误码
    iretq

; stub 地址表：idt.c 通过 isr_stub_table[vector] 取门地址
section .rodata
align 8
global isr_stub_table
isr_stub_table:
    dq isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7
    dq isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15
    dq isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    dq isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    dq isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39
    dq isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47
