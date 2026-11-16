; task_switch.asm - 调度用的三段跳板：保存内核现场、按中断帧恢复任务。
; 为什么只要三个函数：Linux 的 switch_to 只切 callee-saved 加栈指针，因为它是在普通函数调用里切的。
; 这里的切换点在时钟中断里，任务现场早被 isr_common 压成 struct regs 放在自己的内核栈上，
; 所以"保存任务"一行 C 代码就够：cur->frame = 中断帧地址。
; "恢复任务"也只是把 rsp 拨到那帧上、弹出 15 个寄存器、跳过 vector/error、iretq ，
; zh_resume_frame 的全部内容就这些。
; zh_sched_enter / zh_sched_leave 管另一半：Shell 是 ring0 的普通 C 代码，不是任务，没有中断帧，
; 只能用老办法存住 rsp 和 callee-saved，等任务全死光了再原样还回去，ret 回 task_run_all() 里 call 的下一条。
; 这跟 ring3.asm 的 usermode_enter / usermode_resume 是一套思路。
; 寄存器里有三种命：rsp 必须精确，错一个字节 iretq 就跳到垃圾地址；rbx/rbp/r12-r15 是 SysV 规定
; 跨函数调用不变的，Shell 的局部变量可能就住在里面，弄丢一个回 Shell 第一个 leave 就崩；
; 其余（rax/rcx/rdx/rsi/rdi/r8-r11）调用者保存，C 编译器本来就不指望，不用管。

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

; void zh_sched_enter(uint64_t frame);   永不返回
;   rdi = 第一个任务的引导帧地址
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

; void zh_resume_frame(uint64_t frame);   永不返回
;   rdi = 要恢复的任务的中断帧地址
;   帧布局与 isr_common 完全一致：15 个通用寄存器 -> vector -> error
;   -> rip -> cs -> rflags -> rsp -> ss
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

; void zh_sched_leave(void);   永不返回
;   最后一个任务也死掉之后，把 Shell 的现场原样还回去。
zh_sched_leave:
    mov rsp, [rel sched_shell_rsp]

    mov rbx, [rel saved_rbx]
    mov rbp, [rel saved_rbp]
    mov r12, [rel saved_r12]
    mov r13, [rel saved_r13]
    mov r14, [rel saved_r14]
    mov r15, [rel saved_r15]

    ret                              ; 回到 task_run_all() 里 call 的下一条指令
