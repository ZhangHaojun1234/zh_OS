/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    linker.ld
 * Version: 0.2beta
 * Brief:   内核链接脚本：0x100000 加载地址、段布局与 .bss 4K 对齐。
 * ============================================================================ */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    /* Multiboot2 头必须位于映像前 32KB 内，且 8 字节对齐 */
    .multiboot ALIGN(8) :
    {
        KEEP(*(.multiboot))
    }

    .text ALIGN(16) :
    {
        *(.text .text.*)
    }

    .rodata ALIGN(16) :
    {
        *(.rodata .rodata.*)
    }

    .data ALIGN(16) :
    {
        *(.data .data.*)
    }

    .bss ALIGN(4096) :
    {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        . = ALIGN(4096);
        __bss_end = .;
    }

    _kernel_start = 0x100000;
    _kernel_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame .eh_frame_hdr)
    }
}
