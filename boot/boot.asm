; ============================================================================
; zh_OS 1.0 (x86_64)
; File:    boot/boot.asm
; Version: 1.0
; Brief:   Multiboot2 头、32->64 位两段式引导入口、临时页表与 16KB 栈。
; ============================================================================

; ============================================================================
; boot/boot.asm - zh_OS 引导入口（GRUB2 + Multiboot2 -> x86_64）
;
; 关于执行状态（重要）：
;   Multiboot2 规范要求 bootloader 以 32 位保护模式进入 OS 映像。
;   因此本文件是一段"两段式"入口：
;     1) _start        32 位保护模式：建临时页表 -> 开 PAE/LME/分页 -> 远跳
;     2) long_mode_entry  64 位长模式：设置段寄存器与栈，调用 kernel_main
;   进入长模式之后，GDT 与页表由内核（kernel/gdt.c、kernel/paging.c）重建。
; ============================================================================

MB2_MAGIC        equ 0xE85250D6
MB2_ARCH_I386    equ 0
ENTRY_STACK_SIZE equ 16 * 1024

; ---------------------------------------------------------------------------
; Multiboot2 头
; ---------------------------------------------------------------------------
section .multiboot
align 8
mb2_header_start:
    dd MB2_MAGIC
    dd MB2_ARCH_I386
    dd mb2_header_end - mb2_header_start
    dd -(MB2_MAGIC + MB2_ARCH_I386 + (mb2_header_end - mb2_header_start))
align 8

; [ADD] M25：帧缓冲请求标签（Multiboot2 type = 5）。
; 宽/高/色深写 0 表示"不指定"，让 GRUB 自己挑一个能用的图形模式；
; 内核拿到 type=8 标签后再用 Bochs VBE 接口运行时换分辨率（命令 res）。
; 注意：这个标签只在 FB_REQUEST 时编进来。GRUB 一看到 type=5 就一定给图形模式，
; 连 gfxpayload=text 也挡不住；所以要保留"纯文本模式"这一个退路，必须用两个内核：
;   zh_OS.elf    不带此标签 -> 文本模式（老的分等效验证脚本都靠读 0xB8000）
;   zh_OS_fb.elf 带此标签   -> 图形模式（res 命令在这边才有意义）
; [ADD] M34：宽/高/色深可以用 -DFB_W/-DFB_H/-DFB_D 指定。
; GRUB 只按"头里写的需求"挑模式，实测 gfxpayload 在 framebuffer 请求标签存在时
; 会被忽略（写了 1920x1200x32 仍给 1280x800），所以真机想换分辨率就得编出
; 一个"明确要求某个模式"的内核，用菜单项去选。
%ifdef FB_REQUEST
align 8
    dw 5                        ; 帧缓冲标签
    dw 0                        ; flags
    dd 20                       ; size
%ifdef FB_W
    dd FB_W                     ; width
%else
    dd 0                        ; width  = 0：不指定
%endif
%ifdef FB_H
    dd FB_H                     ; height
%else
    dd 0                        ; height = 0：不指定
%endif
%ifdef FB_D
    dd FB_D                     ; depth
%else
    dd 0                        ; depth  = 0：不指定
%endif
%endif
align 8
    dw 0                        ; 结束标签 type = 0
    dw 0                        ; flags
    dd 8                        ; size
mb2_header_end:

; ---------------------------------------------------------------------------
; 32 位入口：建立进入长模式所需的最小环境
; ---------------------------------------------------------------------------
section .text
bits 32
global _start
extern kernel_main

_start:
    cli
    ; 保存 GRUB 传入的 Multiboot2 信息（eax = magic，ebx = info 结构地址）
    mov [mb2_magic], eax
    mov [mb2_info], ebx

    ; 清零临时页表（.bss 由 GRUB 载入，这里显式清零更稳妥）
    mov edi, boot_pml4
    xor eax, eax
    mov ecx, (3 * 4096) / 4
    rep stosd

    mov esp, boot_stack_top
    xor ebp, ebp

    ; PML4[0] -> PDPT
    mov eax, boot_pdpt
    or eax, 0x003               ; Present | Writable
    mov dword [boot_pml4], eax
    mov dword [boot_pml4 + 4], 0

    ; PDPT[0] -> PD
    mov eax, boot_pd
    or eax, 0x003
    mov dword [boot_pdpt], eax
    mov dword [boot_pdpt + 4], 0

    ; 页目录：512 个 2MB 大页，恒等映射低 1GB（含 0xB8000 显存）
    mov edi, boot_pd
    mov eax, 0x83               ; Present | Writable | PageSize(2MB)
    mov ecx, 512
.fill_pd:
    mov dword [edi], eax
    mov dword [edi + 4], 0
    add eax, 0x200000
    add edi, 8
    loop .fill_pd

    ; CR4.PAE = 1
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; CR3 = PML4 物理地址
    mov eax, boot_pml4
    mov cr3, eax

    ; EFER.LME = 1
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    ; CR0: PG | WP | PE
    mov eax, cr0
    or eax, (1 << 31) | (1 << 16) | 1
    mov cr0, eax

    lgdt [boot_gdt_ptr32]
    jmp 0x08:long_mode_entry    ; 远跳转刷新 CS，进入 64 位

; ---------------------------------------------------------------------------
; 64 位长模式入口
; ---------------------------------------------------------------------------
bits 64
long_mode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    lea rsp, [rel boot_stack_top]
    xor rbp, rbp

    mov edi, dword [rel mb2_magic]
    mov esi, dword [rel mb2_info]
    call kernel_main

.hang:
    cli
    hlt
    jmp .hang

; ---------------------------------------------------------------------------
; 进入长模式用的临时 GDT（内核启动后由 kernel/gdt.c 重建）
; ---------------------------------------------------------------------------
section .rodata
align 8
boot_gdt:
    dq 0x0000000000000000       ; 0x00 空描述符
    dq 0x00AF9A000000FFFF       ; 0x08 64 位内核代码段
    dq 0x00AF92000000FFFF       ; 0x10 内核数据段
boot_gdt_end:

align 8
boot_gdt_ptr32:
    dw boot_gdt_end - boot_gdt - 1
    dd boot_gdt

; ---------------------------------------------------------------------------
; .bss：临时页表 + 16KB 内核栈 + Multiboot2 参数
; ---------------------------------------------------------------------------
section .bss
align 4096
boot_pml4:
    resb 4096
boot_pdpt:
    resb 4096
boot_pd:
    resb 4096

align 16
boot_stack_bottom:
    resb ENTRY_STACK_SIZE
boot_stack_top:

align 4
mb2_magic:
    resd 1
mb2_info:
    resd 1
