/* ---------------------------------------------------------------------------
 * zh_OS 内核链接脚本
 * 内核被引导程序加载到物理地址 0x10000，因此由这里定义起始位置。
 * --------------------------------------------------------------------------- */
ENTRY(_start)

SECTIONS
{
    . = 0x10000;

    .text : {
        kernel_start = .;
        *(.text.entry)
        *(.text)
        *(.text.*)
    }

    .rodata : ALIGN(4K) {
        *(.rodata)
        *(.rodata.*)
    }

    .data : ALIGN(4K) {
        *(.data)
        *(.data.*)
    }

    .bss : ALIGN(4K) {
        bss_start = .;
        *(COMMON)
        *(.bss)
        *(.bss.*)
        . = ALIGN(4);
    }
    bss_end = .;

    kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
