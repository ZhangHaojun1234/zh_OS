; ---------------------------------------------------------------------------
; zh_OS 内核入口与中断桩 (NASM, 32 位)
; ---------------------------------------------------------------------------

        bits 32

        section .text.entry
        global _start
        global gdt_flush
        global idt_load
        global isr_stub_table
        global irq_stub_table

        extern kmain
        extern isr_handler
        extern irq_handler
        extern kernel_end
        extern bss_start

_start:
        cli
        mov ax, 0x10
        mov ds, ax
        mov es, ax
        mov fs, ax
        mov gs, ax
        mov ss, ax
        mov esp, 0x90000
        mov ebp, esp

        ; 清零 .bss (引导程序只加载已初始化的数据)
        mov edi, bss_start
        mov ecx, kernel_end
        sub ecx, edi
        xor eax, eax
        rep stosb

        call kmain

.hang:
        cli
        hlt
        jmp .hang

gdt_flush:
        mov eax, [esp + 4]
        lgdt [eax]

        mov ax, 0x10
        mov ds, ax
        mov es, ax
        mov fs, ax
        mov gs, ax
        mov ss, ax

        jmp 0x08:.flush
.flush:
        ret

idt_load:
        mov eax, [esp + 4]
        lidt [eax]
        ret

%macro ISR_NOERR 1
isr%1:
        cli
        push dword 0
        push dword %1
        jmp isr_common
%endmacro

%macro ISR_ERR 1
isr%1:
        cli
        push dword %1
        jmp isr_common
%endmacro

%macro IRQ 2
irq%1:
        cli
        push dword 0
        push dword %2
        jmp irq_common
%endmacro

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
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

IRQ 0,  32
IRQ 1,  33
IRQ 2,  34
IRQ 3,  35
IRQ 4,  36
IRQ 5,  37
IRQ 6,  38
IRQ 7,  39
IRQ 8,  40
IRQ 9,  41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

isr_common:
        pusha
        push ds
        push es
        push fs
        push gs

        mov ax, 0x10
        mov ds, ax
        mov es, ax
        mov fs, ax
        mov gs, ax

        push esp
        call isr_handler
        add esp, 4

        pop gs
        pop fs
        pop es
        pop ds
        popa
        add esp, 8
        iret

irq_common:
        pusha
        push ds
        push es
        push fs
        push gs

        mov ax, 0x10
        mov ds, ax
        mov es, ax
        mov fs, ax
        mov gs, ax

        push esp
        call irq_handler
        add esp, 4

        pop gs
        pop fs
        pop es
        pop ds
        popa
        add esp, 8
        iret

        section .rodata
        align 4
isr_stub_table:
%assign i 0
%rep 32
        dd isr%+i
%assign i i+1
%endrep

        align 4
irq_stub_table:
%assign i 0
%rep 16
        dd irq%+i
%assign i i+1
%endrep
