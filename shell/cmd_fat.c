/* ============================================================================
 * zh_OS 0.1beta (x86_64)
 * File:    shell/cmd_fat.c
 * Version: 0.1beta
 * Brief:   FAT16 只读的 3 个 Shell 命令：fatmount / fatls / fatcat。
 *
 * B 组第 3 个功能，纯增量「只添加，不修改」：本文件为新文件，命令表在
 * commands.c 末尾追加注册。FAT16 命令与 ramfs 的 ls / cat 完全隔离，
 * 不覆盖、不改变任何既有命令的输出与颜色。
 * ============================================================================ */

#include "shell.h"    /* SHELL_CONTINUE / sh_println / err / ok / warn 宏 */
#include "vga.h"      /* vga_putchar / VGA_LIGHT_GREY / VGA_BLACK */
#include "../fs/fat16.h"

/* 错误码 -> 可读英文原因（供命令层拼红字） */
static const char *fat16_reason(int rc)
{
    switch (rc) {
    case FAT16_ERR_NODEV:      return "no disk on primary slave";
    case FAT16_ERR_NOTFAT:     return "not a FAT16 filesystem";
    case FAT16_ERR_BPB:        return "invalid BPB";
    case FAT16_ERR_IO:         return "I/O error";
    case FAT16_ERR_NOENT:      return "no such file or directory";
    case FAT16_ERR_NOTDIR:     return "not a directory";
    case FAT16_ERR_ISDIR:      return "is a directory";
    case FAT16_ERR_NOTMOUNTED: return "not mounted, run fatmount first";
    default:                   return "error";
    }
}

/* 输出 "<cmd>: <reason>" 整行红字；未挂载时按任务要求输出固定提示。 */
static void emit_err_reason(const char *cmd, int rc)
{
    if (rc == FAT16_ERR_NOTMOUNTED) {
        err("not mounted, run fatmount first");
        return;
    }

    char buf[128];
    uint32_t n = 0;
    uint32_t i = 0;

    while (cmd[i] != '\0' && n + 1 < sizeof(buf)) {
        buf[n++] = cmd[i++];
    }
    if (n + 2 < sizeof(buf)) {
        buf[n++] = ':';
        buf[n++] = ' ';
    }
    const char *msg = fat16_reason(rc);
    i = 0;
    while (msg[i] != '\0' && n + 1 < sizeof(buf)) {
        buf[n++] = msg[i++];
    }
    buf[n] = '\0';
    sh_println(SH_ERR, buf);
}

/* ------------------------------------------------------------------ */
/* fatmount：挂载主通道从设备上的 FAT16                              */
/* ------------------------------------------------------------------ */

int cmd_fatmount(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const int rc = fat16_mount();
    if (rc == FAT16_OK) {
        ok("fat16 mounted");
    } else {
        /* 失败亮红并给出具体原因：无盘 / 不是 FAT16 / BPB 非法 */
        emit_err_reason("fatmount", rc);
    }
    return SHELL_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* fatls [path]：列目录，目录亮蓝、文件浅灰、每行 4 个                */
/* ------------------------------------------------------------------ */

int cmd_fatls(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "/";
    const int rc = fat16_list(path);
    if (rc != FAT16_OK) {
        emit_err_reason("fatls", rc);
    }
    return SHELL_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* fatcat <path>：打印文件内容，超过 4KB 截断并亮黄提示               */
/* ------------------------------------------------------------------ */

int cmd_fatcat(int argc, char **argv)
{
    static uint8_t buf[4096];

    if (argc < 2) {
        err("usage: fatcat <path>");
        return SHELL_CONTINUE;
    }

    uint64_t size = 0;
    int rc = fat16_size(argv[1], &size);
    if (rc != FAT16_OK) {
        emit_err_reason("fatcat", rc);
        return SHELL_CONTINUE;
    }

    uint64_t outlen = 0;
    rc = fat16_read(argv[1], buf, sizeof(buf), &outlen);
    if (rc != FAT16_OK) {
        emit_err_reason("fatcat", rc);
        return SHELL_CONTINUE;
    }

    for (uint64_t i = 0; i < outlen; ++i) {
        vga_putchar((char)buf[i], VGA_LIGHT_GREY, VGA_BLACK);
    }
    if (outlen > 0 && buf[outlen - 1] != '\n') {
        vga_putchar('\n', VGA_LIGHT_GREY, VGA_BLACK);
    }
    if (size > (uint64_t)sizeof(buf)) {
        warn("truncated");
    }
    return SHELL_CONTINUE;
}
