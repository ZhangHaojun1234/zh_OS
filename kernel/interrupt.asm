; ============================================================================
; zh_OS 0.2beta (x86_64)
; File:    kernel/interrupt.asm
; Version: 0.2beta
; Brief:   48 个 ISR/IRQ stub、寄存器保存恢复与栈对齐。
; ============================================================================

; ============================================================================
; kernel/interrupt.asm - 64 位 ISR / IRQ stub
;
; 栈布局说明（关键）：
;   进入 stub 时 CPU 已按 64 位模式规则压好中断帧。在 IA-32e 模式下，
;   CPU 会"无条件"压入 5 个 8 字节（不论是否发生特权级切换）：
;       [rsp+0]  rip
;       [rsp+8]  cs
;       [rsp+16] rflags
;       [rsp+24] rsp   （被中断者）
;       [rsp+32] ss    （被中断者）
;   若该异常会带错误码，则错误码位于最上方（rip 之上），即先被压入。
;
; 统一化处理：
;   有错误码的异常（8/10/11/12/13/14/17/21/29/30）：CPU 已压错误码，
;       stub 只补压中断号。
;   无错误码的异常与全部 IRQ：stub 先补压 0 作为占位错误码，再压中断号。
;   这样所有 stub 进入 common 时栈形状完全一致（见 kernel/isr.h 的 struct regs）。
;
; 栈 16 字节对齐（SysV AMD64 ABI 要求 call 前 rsp % 16 == 0）：
;   中断可以在任意位置打断，被中断代码的 rsp 对齐状态不可预知，
;   压栈总数 40 (CPU 帧) + 16 (错误码+中断号) + 120 (15 个通用寄存器) = 176
;   是 16 的倍数，所以"不能"依赖它。因此 common 里显式对齐：
;       mov  rbx, rsp        ; 记住保存区基址
;       and  rsp, ~0xF       ; 向下对齐到 16 字节
;       call interrupt_dispatch
;       mov  rsp, rbx        ; 恢复
;   向下对齐只移动栈指针、不覆盖已保存的寄存器（保存区在更高地址），
;   call 压入的返回地址落在保存区下方，是安全的空闲栈空间。
; ============================================================================

bits 64

section .text

extern interrupt_dispatch

; ---------------------------------------------------------------------------
; 两种 stub 宏
; ---------------------------------------------------------------------------
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

; ---------------------------------------------------------------------------
; 0..31 异常
;   带错误码：8 双故障、10 无效 TSS、11 段不存在、12 栈段错误、13 通用保护、
;             14 缺页、17 对齐检查、21 控制保护、29 VMM、30 安全异常
; ---------------------------------------------------------------------------
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

; ---------------------------------------------------------------------------
; 32..47 硬件 IRQ（PIC 重映射后），均不带错误码
; ---------------------------------------------------------------------------
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

; ---------------------------------------------------------------------------
; [ADD] 128 = 0x80：M2 的系统调用门
;   软件中断不压错误码，所以用 ISR_NOERR（先补 0 占位，再补向量号），
;   栈形状与 isr_common 完全一致，可以直接复用下面的公共出入口。
;   这个 stub 不进 isr_stub_table（表只有 0..47），由 syscall_init() 单独
;   用 idt_set_gate(0x80, isr128, 0, 0xEE) 挂上门 —— 0xEE 的 DPL=3 才是
;   "ring3 允许触发"的关键。
; ---------------------------------------------------------------------------
ISR_NOERR 128

; ---------------------------------------------------------------------------
; 公共出入口
; ---------------------------------------------------------------------------
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

; ---------------------------------------------------------------------------
; stub 地址表：idt.c 通过 isr_stub_table[vector] 取门地址
; ---------------------------------------------------------------------------
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
