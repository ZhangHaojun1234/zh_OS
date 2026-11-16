/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    shell/cmd_vfs.c
 * Version: 0.2beta
 * Brief:   VFS 的 3 个 Shell 命令：mount / umount / mounts（M20）。
 *
 * 纯增量：本文件是新文件，命令表在 commands.c 末尾追加。
 * mount 只登记挂载点，不负责探测设备 —— 底层 FS 已经由 ext2mount / fatmount 挂好了，
 * 这正好对应 Linux 里 "先有 super_block，再有 vfsmount" 的顺序。
 * ============================================================================ */

#include "shell.h"
#include "vga.h"
#include "../fs/vfs.h"
#include "../fs/ext2.h"
#include "../fs/fat16.h"

static const char *fs_name(vfs_fs_t fs)
{
    switch (fs) {
    case VFS_FS_EXT2:  return "ext2";
    case VFS_FS_FAT16: return "fat16";
    case VFS_FS_RAMFS: return "ramfs";
    default:           return "?";
    }
}

/* 打印 "<cmd>: <reason>" 红字 */
static void emit_reason(const char *cmd, const char *reason)
{
    char buf[96];
    uint32_t n = 0u;
    uint32_t i = 0u;

    while (cmd[i] != '\0' && n + 1u < sizeof(buf)) {
        buf[n++] = cmd[i++];
    }
    if (n + 2u < sizeof(buf)) {
        buf[n++] = ':';
        buf[n++] = ' ';
    }
    i = 0u;
    while (reason[i] != '\0' && n + 1u < sizeof(buf)) {
        buf[n++] = reason[i++];
    }
    buf[n] = '\0';
    sh_println(SH_ERR, buf);
}

/* ------------------------------------------------------------------ */
/* mount <ext2|fat> <dir>                                             */
/* ------------------------------------------------------------------ */

int cmd_mount(int argc, char **argv)
{
    vfs_fs_t fs;
    int rc;

    if (argc < 3) {
        err("usage: mount <ext2|fat> </dir>");
        return SHELL_CONTINUE;
    }

    /* 先把底层 FS 挂上（和 Linux 一样：superblock -> vfsmount） */
    if (argv[1][0] == 'e' && argv[1][1] == 'x') {
        fs = VFS_FS_EXT2;
        if (ext2_mounted() == 0) {
            rc = ext2_mount();
            if (rc != EXT2_OK) {
                emit_reason("mount", "ext2mount failed: run ext2mount first to see why");
                return SHELL_CONTINUE;
            }
        }
    } else if (argv[1][0] == 'f' && argv[1][1] == 'a') {
        fs = VFS_FS_FAT16;
        if (fat16_mount() != FAT16_OK) {
            emit_reason("mount", "fatmount failed: no FAT16 on the primary slave");
            return SHELL_CONTINUE;
        }
    } else {
        err("usage: mount <ext2|fat> </dir>");
        return SHELL_CONTINUE;
    }

    rc = vfs_mount(argv[2], fs);
    if (rc == VFS_OK) {
        sh_print(SH_OK, "mounted ");
        sh_print(SH_OK, fs_name(fs));
        sh_print(SH_OK, " on ");
        sh_println(SH_OK, argv[2]);
    } else if (rc == VFS_ERR_EXIST) {
        emit_reason("mount", "already mounted there");
    } else if (rc == VFS_ERR_INVAL) {
        emit_reason("mount", "mount point must be an absolute path like /mnt");
    } else {
        emit_reason("mount", "no free mount slot");
    }
    return SHELL_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* umount <dir>                                                       */
/* ------------------------------------------------------------------ */

int cmd_umount(int argc, char **argv)
{
    int rc;

    if (argc < 2) {
        err("usage: umount </dir>");
        return SHELL_CONTINUE;
    }
    rc = vfs_umount(argv[1]);
    if (rc == VFS_OK) {
        sh_print(SH_OK, "unmounted ");
        sh_println(SH_OK, argv[1]);
    } else if (rc == VFS_ERR_NOENT) {
        emit_reason("umount", "not a mount point");
    } else {
        emit_reason("umount", "invalid argument");
    }
    return SHELL_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* mounts：列出挂载表                                                 */
/* ------------------------------------------------------------------ */

int cmd_mounts(int argc, char **argv)
{
    uint32_t i = 0u;
    uint32_t n;
    (void)argc;
    (void)argv;

    plain("=== mount table ===");
    n = vfs_mount_count();
    if (n == 0u) {
        warn("  (empty: every path goes to ramfs)");
        return SHELL_CONTINUE;
    }
    for (i = 0u; i < VFS_MAX_MOUNTS; ++i) {
        const vfs_mount_t *m = vfs_mount_at(i);
        if (m == (const vfs_mount_t *)0) {
            continue;
        }
        sh_print(SH_PLAIN, "  ");
        sh_print(SH_PLAIN, m->prefix);
        sh_print(SH_PLAIN, "   -> ");
        sh_print(SH_PLAIN, fs_name(m->fs));
        if (m->fs == VFS_FS_EXT2) {
            sh_print(SH_PLAIN, "   (superblock read from the primary IDE disk)");
        } else if (m->fs == VFS_FS_FAT16) {
            sh_print(SH_PLAIN, "   (primary slave)");
        }
        sh_newline();
    }
    return SHELL_CONTINUE;
}
