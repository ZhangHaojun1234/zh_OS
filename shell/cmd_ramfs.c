/* ============================================================================
 * zh_OS 1.0 (x86_64)
 * File:    shell/cmd_ramfs.c
 * Version: 1.0
 * Brief:   基于 ramfs 的 7 个 Shell 命令：ls / cd / pwd / cat / mkdir / touch / rm。
 *
 * B 组第 1 个功能，纯增量「只添加，不修改」：本文件为新文件，命令表在
 * commands.c 末尾追加注册，行为、颜色均按任务要求，不改动任何既有命令。
 * ============================================================================ */

#include "shell.h"    /* SHELL_CONTINUE / sh_println / err / warn / 输出宏 */
#include "vga.h"      /* vga_print / vga_putchar / enum vga_color */
#include "../fs/ramfs.h"  /* ramfs 接口与错误码 */
#include "../fs/vfs.h"    /* [ADD] M20：挂载表与路径解析 */

/* cat 读缓冲（静态，避免占用内核栈；4096 为截断阈值） */
static uint8_t g_catbuf[4096];

/* 拼接 "前缀: 原因" 一次整行输出（freestanding 无 sprintf） */
static void emit_msg(enum sh_level level, const char *prefix, const char *msg)
{
    char buf[128];
    uint32_t n = 0;
    uint32_t i = 0;

    while (prefix[i] != '\0' && n + 1 < sizeof(buf)) {
        buf[n++] = prefix[i++];
    }
    if (n + 2 < sizeof(buf)) {   /* 前缀与原因之间加 ": " */
        buf[n++] = ':';
        buf[n++] = ' ';
    }
    i = 0;
    while (msg[i] != '\0' && n + 1 < sizeof(buf)) {
        buf[n++] = msg[i++];
    }
    buf[n] = '\0';
    sh_println(level, buf);
}

static void emit_err(const char *prefix, const char *msg)
{
    emit_msg(SH_ERR, prefix, msg);
}

static void emit_ok(const char *prefix, const char *msg)
{
    emit_msg(SH_OK, prefix, msg);
}

static bool ends_with(const char *s, const char *suffix)
{
    uint32_t sl = 0;
    while (s[sl] != '\0') {
        ++sl;
    }
    uint32_t su = 0;
    while (suffix[su] != '\0') {
        ++su;
    }
    if (su > sl) {
        return false;
    }
    uint32_t i = 0;
    while (i < su) {
        if (s[sl - su + i] != suffix[i]) {
            return false;
        }
        ++i;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* ls [path]：列目录，4 个一行；目录亮蓝、文件浅灰、.sh(非空)亮绿      */
/* ------------------------------------------------------------------ */

int cmd_ls(int argc, char **argv)
{
    const char *target = (argc > 1) ? argv[1] : ".";

    /* [ADD] M20：路径落在某个挂载点里（比如 /mnt）就交给那个文件系统。
     * 没挂载点匹配时 vfs_under_mount() 返回 0，下面走的还是 M1 以来的老代码，
     * 输出一个字节都不变。 */
    if (vfs_under_mount(target)) {
        const int vrc = vfs_list(target);
        if (vrc != VFS_OK) {
            emit_err("ls", ramfs_strerror(RAMFS_ERR_NOENT));
        }
        return SHELL_CONTINUE;
    }

    ramfs_node_t *node = ramfs_lookup(target);
    if (node == 0) {
        emit_err("ls", ramfs_strerror(RAMFS_ERR_NOENT));
        return SHELL_CONTINUE;
    }
    if (node->type != RAMFS_DIR) {
        emit_err("ls", ramfs_strerror(RAMFS_ERR_NOTDIR));
        return SHELL_CONTINUE;
    }

    uint32_t count = 0;
    ramfs_node_t *child = node->children;
    while (child != 0) {
        bool isdir = (child->type == RAMFS_DIR);
        bool exe = (!isdir && child->size > 0 && ends_with(child->name, ".sh"));
        enum vga_color col = isdir ? VGA_LIGHT_BLUE
                                   : (exe ? VGA_LIGHT_GREEN : VGA_LIGHT_GREY);

        char entry[96];
        uint32_t e = 0;
        uint32_t k = 0;
        while (child->name[k] != '\0' && e + 1 < sizeof(entry)) {
            entry[e++] = child->name[k++];
        }
        if (isdir) {
            if (e + 1 < sizeof(entry)) {
                entry[e++] = '/';
            }
        } else {
            if (e + 1 < sizeof(entry)) {
                entry[e++] = ' ';
            }
            /* 追加文件大小（十进制） */
            char num[24];
            uint32_t nn = 0;
            uint64_t v = child->size;
            if (v == 0) {
                num[nn++] = '0';
            } else {
                char t[20];
                uint32_t t2 = 0;
                while (v != 0) {
                    t[t2++] = (char)('0' + (v % 10));
                    v /= 10;
                }
                while (t2 != 0) {
                    num[nn++] = t[--t2];
                }
            }
            num[nn] = '\0';
            k = 0;
            while (num[k] != '\0' && e + 1 < sizeof(entry)) {
                entry[e++] = num[k++];
            }
        }
        entry[e] = '\0';

        vga_print(entry, col, VGA_BLACK);
        uint32_t pad = e;
        while (pad < 20) {       /* 每行 4 列，列宽约 20 */
            vga_print(" ", col, VGA_BLACK);
            ++pad;
        }
        ++count;
        if (count % 4 == 0) {
            vga_putchar('\n', col, VGA_BLACK);
        }
        child = child->next;
    }
    if (count == 0) {
        vga_putchar('\n', VGA_LIGHT_GREY, VGA_BLACK);   /* 空目录也换行 */
    } else if (count % 4 != 0) {
        vga_putchar('\n', VGA_LIGHT_GREY, VGA_BLACK);
    }
    return SHELL_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* cd <path>：切换目录，成功静默，失败亮红                            */
/* ------------------------------------------------------------------ */

int cmd_cd(int argc, char **argv)
{
    if (argc < 2) {
        err("usage: cd <path>");
        return SHELL_CONTINUE;
    }

    /* [ADD] M20：进入挂载点。ramfs 的目录树里根本没有 /mnt 这个节点，
     * 所以挂载点里的 cwd 由 VFS 自己持有，而不是 ramfs。 */
    if (argv[1][0] == '/' && vfs_under_mount(argv[1])) {
        vfs_set_cwd(argv[1]);
        return SHELL_CONTINUE;
    }
    if (vfs_in_mount_cwd()) {
        /* 在挂载点内部用相对路径：把 cwd 和参数拼起来（".." 就砍掉末尾一段），
         * 这就是 path lookup 里 "父目录" 那一步的简化版。 */
        const char *cwd = vfs_get_cwd();
        char joined[VFS_PATH_MAX];
        if (argv[1][0] == '.' && argv[1][1] == '.') {
            uint32_t n = 0u;
            uint32_t last = 0u;
            while (cwd[n] != '\0') {
                if (cwd[n] == '/') {
                    last = n;
                }
                ++n;
            }
            if (last == 0u) {
                joined[0] = '/';
                joined[1] = '\0';
            } else {
                uint32_t k = 0u;
                while (k < last) {
                    joined[k] = cwd[k];
                    ++k;
                }
                joined[last] = '\0';
            }
        } else {
            vfs_join_cwd(joined, sizeof(joined), argv[1]);
        }
        vfs_set_cwd(joined);
        return SHELL_CONTINUE;
    }

    int rc = ramfs_chdir(argv[1]);
    if (rc == RAMFS_OK) {
        /* [ADD] M20：从挂载点里 cd 回 ramfs，VFS 的 cwd 要退回根，
         * 否则下一次 "ls ." 还会被当成在挂载点里。 */
        vfs_set_cwd("/");
    }
    if (rc != RAMFS_OK) {
        if (rc == RAMFS_ERR_NOTDIR) {
            emit_err("cd", ramfs_strerror(RAMFS_ERR_NOTDIR));
        } else {
            emit_err("cd", ramfs_strerror(RAMFS_ERR_NOENT));
        }
    }
    return SHELL_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* pwd：打印当前目录（显示用路径），亮青                              */
/* ------------------------------------------------------------------ */

int cmd_pwd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const char *cwd = ramfs_get_cwd();

    /* [ADD] M20：cwd 落在挂载点里时，真实路径由 VFS 持有（ramfs 里没有 /mnt）。 */
    if (vfs_in_mount_cwd()) {
        cwd = vfs_get_cwd();
    }
    vga_print(cwd, VGA_LIGHT_CYAN, VGA_BLACK);
    vga_putchar('\n', VGA_LIGHT_CYAN, VGA_BLACK);
    return SHELL_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* cat <file>：读取并打印文件内容（浅灰），超过 4KB 截断并亮黄提示    */
/* ------------------------------------------------------------------ */

int cmd_cat(int argc, char **argv)
{
    if (argc < 2) {
        err("usage: cat <file>");
        return SHELL_CONTINUE;
    }

    /* [ADD] M20：挂载点里的文件交给对应的文件系统读（ext2 / fat16）。 */
    if (vfs_under_mount(argv[1])) {
        uint64_t out = 0u;
        const int vrc = vfs_read(argv[1], g_catbuf, 4096, &out);
        if (vrc != VFS_OK) {
            emit_err("cat", "no such file or directory");
            return SHELL_CONTINUE;
        }
        for (uint64_t i = 0; i < out; ++i) {
            vga_putchar((char)g_catbuf[i], VGA_LIGHT_GREY, VGA_BLACK);
        }
        if (out > 0u && g_catbuf[out - 1u] != '\n') {
            vga_putchar('\n', VGA_LIGHT_GREY, VGA_BLACK);
        }
        return SHELL_CONTINUE;
    }

    ramfs_node_t *node = ramfs_lookup(argv[1]);
    if (node == 0) {
        emit_err("cat", ramfs_strerror(RAMFS_ERR_NOENT));
        return SHELL_CONTINUE;
    }
    if (node->type != RAMFS_FILE) {
        emit_err("cat", ramfs_strerror(RAMFS_ERR_ISDIR));
        return SHELL_CONTINUE;
    }
    uint64_t outlen = 0;
    ramfs_read_file(argv[1], g_catbuf, 4096, &outlen);
    bool truncated = (node->size > 4096);

    for (uint64_t i = 0; i < outlen; ++i) {
        vga_putchar((char)g_catbuf[i], VGA_LIGHT_GREY, VGA_BLACK);
    }
    if (outlen > 0 && g_catbuf[outlen - 1] != '\n') {
        vga_putchar('\n', VGA_LIGHT_GREY, VGA_BLACK);
    }
    if (truncated) {
        warn("truncated");
    }
    return SHELL_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* mkdir / touch / rm：成功亮绿 "created/removed: <path>"，失败亮红    */
/* ------------------------------------------------------------------ */

int cmd_mkdir(int argc, char **argv)
{
    if (argc < 2) {
        err("usage: mkdir <path>");
        return SHELL_CONTINUE;
    }
    int rc = ramfs_mkdir(argv[1]);
    if (rc != RAMFS_OK) {
        emit_err("mkdir", ramfs_strerror(rc));
    } else {
        emit_ok("created", argv[1]);
    }
    return SHELL_CONTINUE;
}

/* [ADD] M24：VFS 错误码 -> 人话（写路径用） */
static const char *vfs_err_text(int rc)
{
    switch (rc) {
    case VFS_ERR_NOENT: return "no such file or directory";
    case VFS_ERR_EXIST: return "file exists";
    case VFS_ERR_NOTDIR: return "not a directory";
    case VFS_ERR_ISDIR: return "is a directory";
    case VFS_ERR_NOSPC: return "no space left on the device";
    case VFS_ERR_RO:    return "read-only file system";
    default:            return "I/O error";
    }
}

/* [ADD] M24：拼一行 "created <path>  (inode N)" 之类的成功提示 */
static void emit_created_inode(const char *verb, const char *path, uint32_t ino)
{
    char buf[128];
    uint32_t n = 0;
    uint32_t i = 0;
    char digits[12];
    uint32_t dn = 0;

    while (verb[i] != '\0' && n + 1 < sizeof(buf)) {
        buf[n++] = verb[i++];
    }
    if (n + 1 < sizeof(buf)) {
        buf[n++] = ' ';
    }
    i = 0;
    while (path[i] != '\0' && n + 1 < sizeof(buf)) {
        buf[n++] = path[i++];
    }
    if (n + 11 < sizeof(buf)) {
        buf[n++] = ' '; buf[n++] = ' '; buf[n++] = '(';
        buf[n++] = 'i'; buf[n++] = 'n'; buf[n++] = 'o'; buf[n++] = 'd';
        buf[n++] = 'e'; buf[n++] = ' ';
    }
    if (ino == 0u) {
        digits[dn++] = '0';
    }
    while (ino != 0u) {
        digits[dn++] = (char)('0' + (ino % 10u));
        ino /= 10u;
    }
    while (dn != 0u && n + 1 < sizeof(buf)) {
        buf[n++] = digits[--dn];
    }
    if (n + 2 < sizeof(buf)) {
        buf[n++] = ')';
    }
    buf[n] = '\0';
    sh_println(SH_OK, buf);
}

int cmd_touch(int argc, char **argv)
{
    /* [ADD] M24：落在挂载点里就交给 VFS（ext2），否则原样走 ramfs */
    if (argc >= 2 && vfs_write_target_is_mount(argv[1], (vfs_fs_t *)0)) {
        uint32_t ino = 0u;
        const int vrc = vfs_create(argv[1], (const uint8_t *)0, 0u, &ino);
        if (vrc != VFS_OK) {
            emit_err("touch", vfs_err_text(vrc));
            return SHELL_CONTINUE;
        }
        emit_created_inode("created", argv[1], ino);
        return SHELL_CONTINUE;
    }

    if (argc < 2) {
        err("usage: touch <path>");
        return SHELL_CONTINUE;
    }
    int rc = ramfs_touch(argv[1]);
    if (rc != RAMFS_OK) {
        emit_err("touch", ramfs_strerror(rc));
    } else {
        emit_ok("created", argv[1]);
    }
    return SHELL_CONTINUE;
}

int cmd_rm(int argc, char **argv)
{
    /* [ADD] M24：落在挂载点里就交给 VFS（ext2 的 unlink），否则走 ramfs */
    if (argc >= 2 && vfs_write_target_is_mount(argv[1], (vfs_fs_t *)0)) {
        uint32_t freed = 0u;
        const int vrc = vfs_unlink(argv[1], &freed);
        if (vrc != VFS_OK) {
            emit_err("rm", vfs_err_text(vrc));
            return SHELL_CONTINUE;
        }
        {
            char buf[128];
            uint32_t n = 0;
            uint32_t i = 0;
            char digits[12];
            uint32_t dn = 0;
            while (n + 1 < sizeof(buf) && "removed "[i] != '\0') {
                buf[n++] = "removed "[i++];
            }
            i = 0;
            while (argv[1][i] != '\0' && n + 1 < sizeof(buf)) {
                buf[n++] = argv[1][i++];
            }
            if (freed == 0u) {
                digits[dn++] = '0';
            }
            while (freed != 0u) {
                digits[dn++] = (char)('0' + (freed % 10u));
                freed /= 10u;
            }
            if (n + 20 < sizeof(buf)) {
                buf[n++] = ' '; buf[n++] = ' '; buf[n++] = '-'; buf[n++] = ' '; 
            }
            while (dn != 0u && n + 1 < sizeof(buf)) {
                buf[n++] = digits[--dn];
            }
            if (n + 13 < sizeof(buf)) {
                buf[n++] = ' '; buf[n++] = 'b'; buf[n++] = 'l'; buf[n++] = 'o';
                buf[n++] = 'c'; buf[n++] = 'k'; buf[n++] = 's';
                buf[n++] = ' '; buf[n++] = 'f'; buf[n++] = 'r'; buf[n++] = 'e';
                buf[n++] = 'e'; buf[n++] = 'd';
            }
            buf[n] = '\0';
            sh_println(SH_OK, buf);
        }
        return SHELL_CONTINUE;
    }

    if (argc < 2) {
        err("usage: rm <path>");
        return SHELL_CONTINUE;
    }
    int rc = ramfs_rm(argv[1]);
    if (rc != RAMFS_OK) {
        emit_err("rm", ramfs_strerror(rc));
    } else {
        emit_ok("removed", argv[1]);
    }
    return SHELL_CONTINUE;
}
