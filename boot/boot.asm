; ---------------------------------------------------------------------------
; zh_OS 第一阶段引导 (MBR, 512 字节)
;
; 职责很轻: 只负责把第二阶段引导程序从 LBA 1 读到 0x7E00 然后跳过去。
; 剩下的工作 (E820、A20、读内核、进保护模式) 都由 stage2 完成。
;
; 镜像布局 (make_image.bat 生成):
;   LBA 0        本 MBR
;   LBA 1 .. 8   stage2 (4 KiB, 加载到 0x7E00)
;   LBA 2048     内核平铺二进制 (加载到 0x10000)
; ---------------------------------------------------------------------------

        bits 16
        org 0x7C00

%define STAGE2_LBA      1
%define STAGE2_SECTORS  8
%define STAGE2_SEG      0x07E0          ; 0x07E0:0000 = 0x7E00

start:
        cli
        xor ax, ax
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov sp, 0x7C00
        sti

        mov [drive], dl

        mov ah, 0x41                    ; 检查 INT 13h 扩展读
        mov bx, 0x55AA
        int 0x13
        jc .no_lba
        cmp bx, 0xAA55
        jne .no_lba
        test cl, 1
        jz .no_lba

        mov word [dap_count], STAGE2_SECTORS
        mov word [dap_off], 0
        mov word [dap_seg], STAGE2_SEG
        mov dword [dap_lba], STAGE2_LBA
        mov si, dap
        mov dl, [drive]
        mov ah, 0x42
        int 0x13
        jc .fail

        mov dl, [drive]
        jmp 0x0000:0x7E00

.no_lba:
.fail:
        mov si, msg_err
.puts:
        lodsb
        test al, al
        jz .hang
        mov ah, 0x0E
        mov bx, 0x0007
        int 0x10
        jmp .puts
.hang:
        hlt
        jmp .hang

msg_err: db "zh_OS: disk read error", 13, 10, 0
drive:   db 0

        align 4
dap:
        db 0x10, 0
dap_count: dw STAGE2_SECTORS
dap_off:   dw 0
dap_seg:   dw STAGE2_SEG
dap_lba:   dq STAGE2_LBA

; ---------------------------------------------------------------------------
; 分区表 (偏移 0x1BE) + 引导签名
; ---------------------------------------------------------------------------
        times 0x1BE - ($ - $$) db 0

partition_table:
        db 0x80                      ; 活动分区
        db 0x00, 0x00, 0x00          ; 起始 CHS
        db 0x83                      ; 类型: Linux
        db 0x00, 0x00, 0x00          ; 结束 CHS
        dd 2048                      ; 起始 LBA
        dd 6144                      ; 扇区数

        times 48 db 0

        dw 0xAA55
