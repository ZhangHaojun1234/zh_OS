; ring3.asm - 进 ring3 的入口，和从 ring3 回内核的出口。
; M1（usermode.c）用内联汇编 iretq 进用户态，那条路是单向的，用户态出错只能 panic 停机。
; M2 要让它双向：usermode_enter 进之前把内核现场存好，usermode_resume 把现场还原、直接 ret 回调用点。
; 关键在栈：从 ring3 陷进来时 CPU 用 TSS.RSP0 换到一块单独的 16KB 内核栈（tss.c 的
; g_tss_kernel_stack），Shell 自己的栈一字节都没被碰。所以 usermode_resume 只要把 rsp 拨回
; kernel_resume_rsp 再 ret，就精确回到 usermode_enter 的调用者，Shell、局部变量、返回地址全是原样。
; 但 callee-saved 寄存器（rbx / rbp / r12-r15）在中断栈上那段 C 代码里会被改，而且 sys_exit
; 不走 isr_common 的 pop 序列，所以这里单独存一份、单独还一份。漏掉 rbp 的话，回 Shell 后第一个 leave 就崩。

bits 64

section .bss
align 8
; 进用户态前保存的内核栈指针：指向 usermode_enter 的返回地址。
kernel_resume_rsp: resq 1
; 调用者（Shell 一侧）的 callee-saved 寄存器。
saved_rbx: resq 1
saved_rbp: resq 1
saved_r12: resq 1
saved_r13: resq 1
saved_r14: resq 1
saved_r15: resq 1

section .text

global usermode_enter:function
global usermode_resume:function

; void usermode_enter(uint64_t rip, uint64_t rsp, uint64_t rflags,
;                     uint64_t cs,  uint64_t ss);
;   SysV 参数：rdi=rip  rsi=rsp  rdx=rflags  rcx=cs  r8=ss
usermode_enter:
    mov [rel kernel_resume_rsp], rsp    ; 此刻 rsp 正指着 call 压进来的返回地址

    mov [rel saved_rbx], rbx
    mov [rel saved_rbp], rbp
    mov [rel saved_r12], r12
    mov [rel saved_r13], r13
    mov [rel saved_r14], r14
    mov [rel saved_r15], r15

    cli                                 ; 压 iret 帧期间别让时钟插进来
    push r8                             ; SS  = 0x23（用户数据段 RPL3）
    push rsi                            ; RSP = 用户栈顶
    push rdx                            ; RFLAGS（IF=1，ring3 也能被时钟打断）
    push rcx                            ; CS  = 0x1b（用户代码段 RPL3）
    push rdi                            ; RIP = 用户代码入口
    iretq                               ; 返回 = 降权进 ring3

; void usermode_resume(void);   永不返回
;   由 sys_exit 调用：丢掉 ring0 那套处理栈，回到 usermode_enter 的调用点。
usermode_resume:
    mov rsp, [rel kernel_resume_rsp]

    mov rbx, [rel saved_rbx]
    mov rbp, [rel saved_rbp]
    mov r12, [rel saved_r12]
    mov r13, [rel saved_r13]
    mov r14, [rel saved_r14]
    mov r15, [rel saved_r15]
    ret
