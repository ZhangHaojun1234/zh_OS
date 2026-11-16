/* cmd_disk.c - diskinfo 命令：显示 ATA 磁盘状态和持久化扇区信息。
 * 直接读 drivers/ata.c 缓存的"有没有盘"标记，不碰任何既有命令。
 * 标题用亮青，跟 dmesg 那边的要求保持一致。 */

#include "shell.h"
#include "vga.h"
#include "../drivers/ata.h"

int cmd_diskinfo(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* 标题：亮青（任务显式要求）。 */
    vga_print("=== Disk ===\n", VGA_LIGHT_CYAN, VGA_BLACK);

    if (ata_disk_present()) {
        ok("ATA primary master: present");
        plain("persist sectors: LBA 200-207");
    } else {
        warn("no ATA disk detected");
        plain("run QEMU with -hda disk.img to persist users");
    }

    return SHELL_CONTINUE;
}
