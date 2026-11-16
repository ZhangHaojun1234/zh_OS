/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    shell/cmd_disk.c
 * Version: 0.2beta
 * Brief:   diskinfo 命令：显示 ATA 磁盘状态与持久化扇区信息。
 *
 * B 组第 2 个功能（ATA PIO + 用户表持久化）的纯增量新增文件。
 * 直接读取 drivers/ata.c 缓存的"磁盘是否存在"标记，不改任何既有命令。
 * 标题按任务要求用亮青（VGA_LIGHT_CYAN），与 dmesg 的显式任务要求一致。
 * ============================================================================ */

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
