; ============================================================================
; zh_OS 0.2beta (x86_64)
; File:    kernel/task_switch.asm
; Version: 0.2beta
; Brief:   M4 进程调度：保存/恢复"内核现场"与按中断帧恢复任务的三段跳板。
; ============================================================================
;
; 为什么只需要三个函数
; --------------------
; Linux 的 switch_to 只切 callee-saved 寄存器（rbx/rbp/r12-r15）+ 栈指针，
; 因为它是在"普通函数调用"里切换的。本内核的切换点不在函数里，而在
; 时钟中断里 —— 那时任务的完整现场已经被 isr_common 压成 struct regs 存在
; 它自己的内核栈上了。所以"保存任务"根本不需要额外动作：
;     cur->frame = 中断帧地址      ; 一行 C 代码就够了
; "恢复任务"也只是把 rsp 拨到那帧上、弹出 15 个寄存器、跳过 vector/error、iretq。
; 这就是 zh_resume_frame 的全部内容。
;
; zh_sched_enter / zh_sched_leave 是另一半：Shell（ring0 普通 C 代码）不是任务，
; 它没有中断帧，所以只能用老办法 —— 存住 rsp 与 callee-saved，等所有任务都
; 死光了再原样还回去，ret 回到 task_run_all() 里 call 的下一条指令。
; 这与 kernel/ring3.asm 的 usermode_enter/usermode_resume 是同一套思路。
;
; 寄存器保存的"三条命"：
;   - rsp：唯一必须精确的，错一个字节 iretq 就跳到垃圾地址；
;   - rbx/rbp/r12-r15：SysV 规定它们跨函数调用不变，Shell 的局部变量可能就住在
;     这里面，弄丢一个，回 Shell 后第一个 leave 或第一次取数就崩；
;   - 其余（rax/rcx/rdx/rsi/rdi/r8-r11）：调用者保存，C 编译器本来就不指望它们，
;     所以不用管。
; ============================================================================

bits 64

section .bss
align 8
; 进入任务前保存的 Shell 现场：rsp 指向 zh_sched_enter 的返回地址。
sched_shell_rsp: resq 1
saved_rbx:       resq 1
saved_rbp:       resq 1
saved_r12:       resq 1
saved_r13:       resq 1
saved_r14:       resq 1
saved_r15:       resq 1

section .text

global zh_sched_enter:function
global zh_sched_leave:function
global zh_resume_frame:function

; ---------------------------------------------------------------------------
; void zh_sched_enter(uint64_t frame);   永不返回
;   rdi = 第一个任务的引导帧地址
; ---------------------------------------------------------------------------
zh_sched_enter:
    mov [rel sched_shell_rsp], rsp   ; 此刻 rsp 正指着 call 压进来的返回地址

    mov [rel saved_rbx], rbx
    mov [rel saved_rbp], rbp
    mov [rel saved_r12], r12
    mov [rel saved_r13], r13
    mov [rel saved_r14], r14
    mov [rel saved_r15], r15

    mov rsp, rdi                     ; 站到任务的内核栈上（帧顶）
    jmp resume_tail

; ---------------------------------------------------------------------------
; void zh_resume_frame(uint64_t frame);   永不返回
;   rdi = 要恢复的任务的中断帧地址
;   帧布局与 isr_common 完全一致：15 个通用寄存器 -> vector -> error
;   -> rip -> cs -> rflags -> rsp -> ss
; ---------------------------------------------------------------------------
zh_resume_frame:
    mov rsp, rdi

resume_tail:
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

    add rsp, 16                      ; 丢掉 vector + error_code 两个占位槽
    iretq                            ; rip/cs/rflags/rsp/ss 由 CPU 弹回 -> 回到任务现场

; ---------------------------------------------------------------------------
; void zh_sched_leave(void);   永不返回
;   最后一个任务也死掉之后，把 Shell 的现场原样还回去。
; ---------------------------------------------------------------------------
zh_sched_leave:
    mov rsp, [rel sched_shell_rsp]

    mov rbx, [rel saved_rbx]
    mov rbp, [rel saved_rbp]
    mov r12, [rel saved_r12]
    mov r13, [rel saved_r13]
    mov r14, [rel saved_r14]
    mov r15, [rel saved_r15]

    ret                              ; 回到 task_run_all() 里 call 的下一条指令
