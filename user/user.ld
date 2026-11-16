/* user.ld - 用户态程序的链接脚本，把自己链到进程私有窗口的起点 0x40000000。
 * 为什么是这个地址：M5 的 vmm.c 把每进程的私有窗口固定放在 1GB 处（VMM_USER_BASE），第一页放代码。
 * 用户程序必须"生在"这个地址上，.rodata 里的字符串指针这类绝对地址，搬进进程之后才依然有效。
 * 这也是 ET_EXEC 的特点（固定装载地址），PIE 和动态链接是后来的事。 */

ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    .text : {
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    .bss : {
        *(COMMON)
        *(.bss*)
    }

    /DISCARD/ : {
        *(.comment) *(.note*) *(.eh_frame*)
    }
}
