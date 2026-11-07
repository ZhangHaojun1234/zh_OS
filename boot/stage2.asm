; ---------------------------------------------------------------------------
; zh_OS 第二阶段引导 (加载到 0x7E00, 最长 4 KiB)
;
; 工作:
;   1. 初始化串口 COM1
;   2. BIOS E820 内存探测 -> 0x4F00 (数量) / 0x5000 (条目)
;   3. 打开 A20
;   4. INT 13h/EDD 把内核读到 0x10000 (最多 64 KiB)
;   5. 建立 GDT 并进入 32 位保护模式
; ---------------------------------------------------------------------------

        bits 16
        org 0x7E00

%define KERNEL_LBA      2048
%define CHUNK_SECTORS   16              ; 每次 8 KiB
%define CHUNKS          8               ; 共 128 扇区 = 64 KiB
%define KERNEL_SEG      0x1000
%define MEMMAP_COUNT    0x4F00
%define MEMMAP_ENTRIES  0x5000
%define MAX_E820        32

stage2:
        cli
        xor ax, ax
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov sp, 0x7C00
        sti

        mov [drive], dl
        call serial_init

        mov si, msg_boot
        call puts

        call e820

        mov si, msg_load
        call puts
        call load_kernel

        mov si, msg_pm
        call puts

        call enable_a20

        cli
        lgdt [gdt_desc]
        mov eax, cr0
        or eax, 1
        mov cr0, eax
        jmp 0x08:pm_entry

; ---------------------------------------------------------------------------
; puts: SI = 以 0 结尾的字符串
; ---------------------------------------------------------------------------
puts:
        lodsb
        test al, al
        jz .done
        mov ah, 0x0E
        mov bx, 0x0007
        push ax
        call serial_putc
        pop ax
        int 0x10
        jmp puts
.done:
        ret

; ---------------------------------------------------------------------------
; COM1
; ---------------------------------------------------------------------------
serial_init:
        mov dx, 0x3FB
        mov al, 0x80
        out dx, al
        mov dx, 0x3F8
        mov al, 1
        out dx, al
        mov dx, 0x3F9
        xor al, al
        out dx, al
        mov dx, 0x3FB
        mov al, 0x03
        out dx, al
        mov dx, 0x3FA
        mov al, 0xC7
        out dx, al
        mov dx, 0x3FC
        mov al, 0x0B
        out dx, al
        ret

serial_putc:                    ; AL
        push ax
        push dx
        mov ah, al
.wait:
        mov dx, 0x3FD
        in al, dx
        test al, 0x20
        jz .wait
        mov dx, 0x3F8
        mov al, ah
        out dx, al
        pop dx
        pop ax
        ret

; ---------------------------------------------------------------------------
; E820
; ---------------------------------------------------------------------------
e820:
        mov di, MEMMAP_ENTRIES
        xor ebx, ebx
        xor bp, bp
.loop:
        xor ax, ax
        mov es, ax
        mov eax, 0xE820
        mov edx, 0x534D4150
        mov ecx, 24
        int 0x15
        jc .done
        cmp eax, 0x534D4150
        jne .done
        test ecx, ecx
        jz .done
        inc bp
        cmp bp, MAX_E820
        jae .done
        add di, 24
        test ebx, ebx
        jnz .loop
.done:
        mov [MEMMAP_COUNT], bp
        ret

; ---------------------------------------------------------------------------
; A20
; ---------------------------------------------------------------------------
enable_a20:
        in al, 0x92
        test al, 2
        jnz .done
        or al, 2
        and al, 0xFE
        out 0x92, al
.done:
        ret

; ---------------------------------------------------------------------------
; 加载内核
; ---------------------------------------------------------------------------
load_kernel:
        mov ah, 0x41
        mov bx, 0x55AA
        mov dl, [drive]
        int 0x13
        jc .fail
        cmp bx, 0xAA55
        jne .fail
        test cl, 1
        jz .fail

        mov word [dap_count], CHUNK_SECTORS
        mov word [dap_off], 0
        mov word [dap_seg], KERNEL_SEG
        mov dword [dap_lba], KERNEL_LBA
        xor ecx, ecx
        mov cx, CHUNKS
.read:
        push cx
        xor ax, ax
        mov ds, ax
        mov si, dap
        mov dl, [drive]
        mov ah, 0x42
        int 0x13
        jc .read_err
        pop cx
        add dword [dap_lba], CHUNK_SECTORS
        add word [dap_seg], CHUNK_SECTORS * 32
        loop .read
        ret
.read_err:
        pop cx
.fail:
        mov si, msg_err
        call puts
.hang:
        hlt
        jmp .hang

; ---------------------------------------------------------------------------
; 数据
; ---------------------------------------------------------------------------
msg_boot: db "zh_OS bootloader v0.1", 13, 10, 0
msg_load: db "Loading kernel ...", 13, 10, 0
msg_pm:   db "Entering protected mode ...", 13, 10, 0
msg_err:  db 13, 10, "Disk read error", 13, 10, 0

drive:    db 0

        align 4
dap:
        db 0x10, 0
dap_count: dw CHUNK_SECTORS
dap_off:   dw 0
dap_seg:   dw KERNEL_SEG
dap_lba:   dq KERNEL_LBA

        align 8
gdt_start:
        dq 0x0000000000000000
gdt_code:
        dw 0xFFFF, 0x0000
        db 0x00, 0x9A, 0xCF, 0x00
gdt_data:
        dw 0xFFFF, 0x0000
        db 0x00, 0x92, 0xCF, 0x00
gdt_end:

gdt_desc:
        dw gdt_end - gdt_start - 1
        dd gdt_start

; ---------------------------------------------------------------------------
        bits 32
pm_entry:
        mov ax, 0x10
        mov ds, ax
        mov es, ax
        mov fs, ax
        mov gs, ax
        mov ss, ax
        mov esp, 0x90000
        jmp 0x10000

        times 4096 - ($ - $$) db 0
