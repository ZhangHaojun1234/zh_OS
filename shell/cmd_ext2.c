/* cmd_ext2.c - ext2 只读的四个命令：ext2mount / ext2info / ext2ls / ext2cat。
 * 跟 ramfs 的 ls / cat、FAT16 的 fatls / fatcat 完全隔离，命令表在 commands.c 末尾追加。 */

#include "shell.h"    /* SHELL_CONTINUE / sh_println / err / ok / warn / plain */
#include "vga.h"      /* vga_putchar / VGA_LIGHT_GREY / VGA_BLACK */
#include "../fs/ext2.h"

/* 错误码 -> 可读原因 */
static const char *ext2_reason(int rc)
{
    switch (rc) {
    case EXT2_ERR_NODEV:      return "no disk on primary master";
    case EXT2_ERR_NOTEXT2:    return "not an ext2 filesystem (magic != 0xEF53)";
    case EXT2_ERR_SB:         return "invalid superblock fields";
    case EXT2_ERR_IO:         return "I/O error";
    case EXT2_ERR_NOENT:      return "no such file or directory";
    case EXT2_ERR_NOTDIR:     return "not a directory";
    case EXT2_ERR_ISDIR:      return "is a directory";
    case EXT2_ERR_NOTMOUNTED: return "not mounted, run ext2mount first";
    case EXT2_ERR_CORRUPT:    return "corrupt inode or directory entry";
    case EXT2_ERR_NOSPC:      return "no space left on the device";        /* [ADD] M21 */
    case EXT2_ERR_EXIST:      return "file exists";                        /* [ADD] M21 */
    default:                  return "error";
    }
}

static void emit_err_reason(const char *cmd, int rc)
{
    char buf[128];
    uint32_t n = 0u;
    uint32_t i = 0u;
    const char *msg;

    if (rc == EXT2_ERR_NOTMOUNTED) {
        err("not mounted, run ext2mount first");
        return;
    }
    while (cmd[i] != '\0' && n + 1u < sizeof(buf)) {
        buf[n++] = cmd[i++];
    }
    if (n + 2u < sizeof(buf)) {
        buf[n++] = ':';
        buf[n++] = ' ';
    }
    msg = ext2_reason(rc);
    i = 0u;
    while (msg[i] != '\0' && n + 1u < sizeof(buf)) {
        buf[n++] = msg[i++];
    }
    buf[n] = '\0';
    sh_println(SH_ERR, buf);
}

/* 十六进制打印（ext2 的魔数 0xEF53 这类值，十进制没法看） */
static void print_hex(const char *label, uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    char buf[64];
    uint32_t i = 0u;
    uint32_t n = 0u;

    while (label[i] != '\0' && n + 1u < sizeof(buf)) {
        buf[n++] = label[i++];
    }
    if (n + 3u < sizeof(buf)) {
        buf[n++] = '0';
        buf[n++] = 'x';
    }
    /* 8 位十六进制，去前导 0 但至少留一位 */
    {
        int sh;
        int started = 0;
        for (sh = 28; sh >= 0; sh -= 4) {
            uint32_t d = (v >> (uint32_t)sh) & 0xFu;
            if (d != 0u || started || sh == 0) {
                buf[n++] = digits[d];
                started = 1;
            }
            if (n + 2u >= sizeof(buf)) {
                break;
            }
        }
    }
    buf[n] = '\0';
    sh_println(SH_PLAIN, buf);
}

/* ext2mount                                                          */

int cmd_ext2mount(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    {
        const int rc = ext2_mount();
        if (rc == EXT2_OK) {
            ok("ext2 mounted from the primary IDE disk");
        } else {
            emit_err_reason("ext2mount", rc);
        }
    }
    return SHELL_CONTINUE;
}

/* ext2info：把超级块里读到的几何信息摊开，便于和 dumpe2fs 对照      */

int cmd_ext2info(int argc, char **argv)
{
    ext2_geom_t g;
    (void)argc;
    (void)argv;

    if (ext2_geometry(&g) != EXT2_OK) {
        err("not mounted, run ext2mount first");
        return SHELL_CONTINUE;
    }
    plain("=== ext2 superblock ===");
    print_hex("magic      : ", g.magic);
    sh_print(SH_PLAIN, "block size : ");
    sh_print_u32(SH_PLAIN, g.block_size);
    sh_print(SH_PLAIN, " bytes   blocks=");
    sh_print_u32(SH_PLAIN, g.blocks_count);
    sh_newline();
    sh_print(SH_PLAIN, "inodes     : ");
    sh_print_u32(SH_PLAIN, g.inodes_count);
    sh_print(SH_PLAIN, "   first_ino=");
    sh_print_u32(SH_PLAIN, g.first_ino);
    sh_print(SH_PLAIN, "   inode_size=");
    sh_print_u32(SH_PLAIN, g.inode_size);
    sh_newline();
    sh_print(SH_PLAIN, "per group  : blocks=");
    sh_print_u32(SH_PLAIN, g.blocks_per_group);
    sh_print(SH_PLAIN, "  inodes=");
    sh_print_u32(SH_PLAIN, g.inodes_per_group);
    sh_newline();
    sh_print(SH_PLAIN, "gdt block  : ");
    sh_print_u32(SH_PLAIN, g.gdt_block);
    sh_print(SH_PLAIN, "   inode table block=");
    sh_print_u32(SH_PLAIN, g.inode_table);
    sh_newline();
    return SHELL_CONTINUE;
}

/* ext2ls [path]                                                      */

int cmd_ext2ls(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "/";
    const int rc = ext2_list(path);

    if (rc != EXT2_OK) {
        emit_err_reason("ext2ls", rc);
    }
    return SHELL_CONTINUE;
}

/* ext2cat <path>                                                     */

int cmd_ext2cat(int argc, char **argv)
{
    static uint8_t buf[32768];   /* 够读 22KB 的 big.txt：越过 12 个直接块后走一级间接块 */
    uint64_t size = 0u;
    uint64_t outlen = 0u;
    uint64_t i;
    int rc;

    if (argc < 2) {
        err("usage: ext2cat <path>");
        return SHELL_CONTINUE;
    }
    rc = ext2_size(argv[1], &size);
    if (rc != EXT2_OK) {
        emit_err_reason("ext2cat", rc);
        return SHELL_CONTINUE;
    }
    rc = ext2_read(argv[1], buf, sizeof(buf), &outlen);
    if (rc != EXT2_OK) {
        emit_err_reason("ext2cat", rc);
        return SHELL_CONTINUE;
    }
    /* 先把"读了哪个文件、多少字节"打出来：大文件会把屏幕刷掉，
     * 这行说明 + 末尾的 TAIL-MARKER 一起，才是"间接块真的被走到了"的证据。 */
    sh_print(SH_PLAIN, "ext2cat: ");
    sh_print(SH_PLAIN, argv[1]);
    sh_print(SH_PLAIN, " (");
    sh_print_u32(SH_PLAIN, (uint32_t)size);
    sh_println(SH_PLAIN, " bytes)");
    for (i = 0u; i < outlen; ++i) {
        vga_putchar((char)buf[i], VGA_LIGHT_GREY, VGA_BLACK);
    }
    if (outlen > 0u && buf[outlen - 1u] != '\n') {
        vga_putchar('\n', VGA_LIGHT_GREY, VGA_BLACK);
    }
    if (size > (uint64_t)sizeof(buf)) {
        warn("truncated");
    }
    return SHELL_CONTINUE;
}

/* [ADD] M21：ext2create <path> <text...>                              */
/*   把后面的参数用空格拼成内容，真正写进磁盘：分配 inode -> 分配块 -> */
/*   写数据 -> 写 inode -> 父目录插目录项。                            */

#define CREATE_MAX_TEXT 4000u
static uint8_t g_ctext[CREATE_MAX_TEXT];

int cmd_ext2create(int argc, char **argv)
{
    uint32_t n = 0u;
    uint32_t i;
    uint32_t ino = 0u;
    int rc;

    if (argc < 3) {
        err("usage: ext2create <path> <text...>");
        return SHELL_CONTINUE;
    }
    for (i = 2u; i < (uint32_t)argc; ++i) {
        uint32_t k = 0u;
        if (i > 2u && n + 1u < CREATE_MAX_TEXT) {
            g_ctext[n++] = ' ';
        }
        while (argv[i][k] != '\0' && n + 1u < CREATE_MAX_TEXT) {
            g_ctext[n++] = (uint8_t)argv[i][k++];
        }
    }
    if (n + 1u < CREATE_MAX_TEXT) {
        g_ctext[n++] = '\n';               /* 文件末尾补一个换行，cat 才好看 */
    }
    g_ctext[n] = '\0';

    rc = ext2_create(argv[1], g_ctext, (uint64_t)n, &ino);
    if (rc != EXT2_OK) {
        emit_err_reason("ext2create", rc);
        return SHELL_CONTINUE;
    }
    sh_print(SH_OK, "created ");
    sh_print(SH_OK, argv[1]);
    sh_print(SH_OK, "  inode ");
    sh_print_u32(SH_OK, ino);
    sh_print(SH_OK, "  ");
    sh_print_u32(SH_OK, n);
    sh_println(SH_OK, " bytes written to disk");
    return SHELL_CONTINUE;
}

/* [ADD] M21：ext2free ， 超级块里的空闲块 / 空闲 inode 计数            */

int cmd_ext2free(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (ext2_mounted() == 0) {
        emit_err_reason("ext2free", EXT2_ERR_NOTMOUNTED);
        return SHELL_CONTINUE;
    }
    sh_print(SH_PLAIN, "free blocks ");
    sh_print_u32(SH_PLAIN, ext2_free_blocks());
    sh_print(SH_PLAIN, "   free inodes ");
    sh_print_u32(SH_PLAIN, ext2_free_inodes());
    sh_newline();
    return SHELL_CONTINUE;
}

/* [ADD] M22：ext2fill <path> <KB>                                     */
/*   造一个 >12KB 的文件，逼着写路径必须用一级间接块：                  */
/*   每行 "line NNN of the indirect-block write test"，最后一行是       */
/*   END-OF-INDIRECT-WRITE ， 它落在第 13 块之后，只有间接块走对了才  */
/*   能读回来。                                                       */

#define FILL_MAX 28672u                 /* 12 直接块 + 256 间接块 = 268KB，这里限 28KB */

static uint8_t  g_fillbuf[FILL_MAX];
static char     g_fillline[64];

int cmd_ext2fill(int argc, char **argv)
{
    uint32_t kb = 0u;
    uint32_t i;
    uint32_t n = 0u;
    uint32_t line_no = 1u;
    uint32_t ino = 0u, blocks = 0u, indirect = 0u;
    int rc;

    if (argc < 3) {
        err("usage: ext2fill <path> <KB>");
        return SHELL_CONTINUE;
    }
    for (i = 0u; argv[2][i] != '\0'; ++i) {
        if (argv[2][i] < '0' || argv[2][i] > '9') {
            err("usage: ext2fill <path> <KB>");
            return SHELL_CONTINUE;
        }
        kb = kb * 10u + (uint32_t)(argv[2][i] - '0');
    }
    if (kb == 0u) {
        kb = 12u;
    }
    if (kb > 28u) {
        kb = 28u;
    }

    /* 填到约 kb KB：每行 "line 000 of the indirect-block write test" */
    while (n < kb * 1024u - 40u && n + 64u < FILL_MAX) {
        uint32_t k = 0u;
        const char *p = "line ";
        while (*p != '\0') {
            g_fillline[k++] = *p++;
        }
        {
            /* line_no 至少 3 位，不够补 0 */
            char num[8];
            uint32_t nn = 0u;
            uint32_t v = line_no;
            if (v == 0u) {
                num[nn++] = '0';
            }
            while (v != 0u) {
                num[nn++] = (char)('0' + (v % 10u));
                v /= 10u;
            }
            while (nn < 3u) {
                num[nn++] = '0';
            }
            while (nn != 0u) {
                g_fillline[k++] = num[--nn];
            }
        }
        p = " of the indirect-block write test\n";
        while (*p != '\0') {
            g_fillline[k++] = *p++;
        }
        for (i = 0u; i < k; ++i) {
            g_fillbuf[n++] = (uint8_t)g_fillline[i];
        }
        ++line_no;
    }
    /* 收尾标记：保证它落在第 13 块之后 */
    {
        const char *p = "END-OF-INDIRECT-WRITE\n";
        while (*p != '\0' && n + 1u < FILL_MAX) {
            g_fillbuf[n++] = (uint8_t)*p++;
        }
    }

    rc = ext2_create_big(argv[1], g_fillbuf, (uint64_t)n, &ino, &blocks, &indirect);
    if (rc != EXT2_OK) {
        emit_err_reason("ext2fill", rc);
        return SHELL_CONTINUE;
    }
    sh_print(SH_OK, "wrote ");
    sh_print(SH_OK, argv[1]);
    sh_print(SH_OK, "  inode ");
    sh_print_u32(SH_OK, ino);
    sh_print(SH_OK, "  ");
    sh_print_u32(SH_OK, n);
    sh_print(SH_OK, " bytes, ");
    sh_print_u32(SH_OK, blocks);
    sh_print(SH_OK, " data blocks, indirect ptr block #");
    sh_print_u32(SH_OK, indirect);
    sh_newline();
    return SHELL_CONTINUE;
}

/* [ADD] M23：ext2rm ， 真正删一个文件（释放块 + 清 inode + 摘目录项）*/

int cmd_ext2rm(int argc, char **argv)
{
    uint32_t ino = 0u, freed = 0u;
    int rc;

    if (argc < 2) {
        err("usage: ext2rm <path>");
        return SHELL_CONTINUE;
    }
    rc = ext2_unlink(argv[1], &ino, &freed);
    if (rc != EXT2_OK) {
        if (rc == EXT2_ERR_ISDIR) {
            err("ext2rm: is a directory (rmdir is not supported yet)");
            return SHELL_CONTINUE;
        }
        emit_err_reason("ext2rm", rc);
        return SHELL_CONTINUE;
    }
    sh_print(SH_OK, "removed ");
    sh_print(SH_OK, argv[1]);
    sh_print(SH_OK, "  inode ");
    sh_print_u32(SH_OK, ino);
    sh_print(SH_OK, "  ");
    sh_print_u32(SH_OK, freed);
    sh_println(SH_OK, " blocks freed");
    return SHELL_CONTINUE;
}

/* [ADD] M28：ext2big ， 写一个超过 268KB 的文件（用上二/三级间接块） */

/* 生成内容：每 64 字节一行，行首带序号，末尾那段是校验标记。
 * 读回来的时候只要看到大小和尾标记，就说明指针树走对了。 */
static void big_gen(uint64_t off, uint8_t *buf, uint32_t n, void *ctx)
{
    uint64_t total = *(uint64_t *)ctx;
    uint32_t i;

    for (i = 0u; i < n; ++i) {
        uint64_t pos = off + i;
        uint32_t col = (uint32_t)(pos % 64u);
        uint64_t line = pos / 64u;

        if (col < 4u) {
            buf[i] = (uint8_t)('0' + (uint8_t)((line >> (8u * (3u - col))) & 0xFFu) % 10u);
        } else if (col == 4u) {
            buf[i] = ' ';
        } else if (col == 5u) {
            buf[i] = '|';
        } else if (col == 62u) {
            buf[i] = '|';
        } else if (col == 63u) {
            buf[i] = '\n';
        } else {
            buf[i] = (uint8_t)('a' + (uint8_t)((line + col) % 26u));
        }
    }
    /* 收尾标记：占最后 18 字节，保证落在文件末尾那一块 */
    if (total >= 18u && off + n >= total) {
        const char *m = "END-OF-HUGE-WRITE!";
        uint32_t mlen = 18u;
        uint32_t at = (uint32_t)(total - mlen) - (uint32_t)off;
        for (i = 0u; i < mlen && at + i < n; ++i) {
            buf[at + i] = (uint8_t)m[i];
        }
    }
}

int cmd_ext2big(int argc, char **argv)
{
    uint32_t kb = 1024u;
    uint64_t len;
    uint64_t total;
    uint32_t ino = 0u, blocks = 0u, ptrs = 0u, i1 = 0u, i2 = 0u, i3 = 0u;
    uint32_t i;
    int rc;

    if (argc < 2) {
        err("usage: ext2big <path> [KB]     KB default 1024, max 4096");
        return SHELL_CONTINUE;
    }
    if (argc >= 3) {
        uint32_t v = 0u;
        for (i = 0u; argv[2][i] != '\0'; ++i) {
            if (argv[2][i] < '0' || argv[2][i] > '9') {
                err("ext2big: KB must be 1..4096");
                return SHELL_CONTINUE;
            }
            v = v * 10u + (uint32_t)(argv[2][i] - '0');
        }
        if (v == 0u || v > 4096u) {
            err("ext2big: KB must be 1..4096");
            return SHELL_CONTINUE;
        }
        kb = v;
    }
    len = (uint64_t)kb * 1024u;
    total = len;

    rc = ext2_create_stream(argv[1], len, big_gen, &total,
                            &ino, &blocks, &ptrs, &i1, &i2, &i3);
    if (rc != EXT2_OK) {
        emit_err_reason("ext2big", rc);
        return SHELL_CONTINUE;
    }

    sh_print(SH_OK, "wrote ");
    sh_print(SH_OK, argv[1]);
    sh_print(SH_OK, "  inode ");
    sh_print_u32(SH_OK, ino);
    sh_print(SH_OK, "  ");
    sh_print_u32(SH_OK, (uint32_t)len);
    sh_print(SH_OK, " bytes, ");
    sh_print_u32(SH_OK, blocks);
    sh_print(SH_OK, " data blocks, ");
    sh_print_u32(SH_OK, ptrs);
    sh_print(SH_OK, " pointer block(s)");
    sh_newline();
    sh_print(SH_OK, "  indirect: i_block[12]=#");
    sh_print_u32(SH_OK, i1);
    sh_print(SH_OK, "  i_block[13]=#");
    sh_print_u32(SH_OK, i2);
    sh_print(SH_OK, "  i_block[14]=#");
    sh_print_u32(SH_OK, i3);
    sh_newline();
    return SHELL_CONTINUE;
}

/* [ADD] M28：ext2tail ， 读大文件的最后 N 字节，验证二级间接读路径   */

int cmd_ext2tail(int argc, char **argv)
{
    static uint8_t buf[512];
    uint64_t size = 0u, off = 0u, got = 0u, i;
    uint64_t want = 128u;
    int rc;

    if (argc < 2) {
        err("usage: ext2tail <path> [bytes]   (bytes default 128, max 512)");
        return SHELL_CONTINUE;
    }
    if (argc >= 3) {
        uint32_t v = 0u;
        for (i = 0u; argv[2][i] != '\0'; ++i) {
            if (argv[2][i] < '0' || argv[2][i] > '9') {
                err("ext2tail: bytes must be 1..512");
                return SHELL_CONTINUE;
            }
            v = v * 10u + (uint32_t)(argv[2][i] - '0');
        }
        if (v == 0u || v > 512u) {
            err("ext2tail: bytes must be 1..512");
            return SHELL_CONTINUE;
        }
        want = (uint64_t)v;
    }
    rc = ext2_size(argv[1], &size);
    if (rc != EXT2_OK) {
        emit_err_reason("ext2tail", rc);
        return SHELL_CONTINUE;
    }
    if (size > want) {
        off = size - want;
    }
    rc = ext2_read_at(argv[1], off, buf, want, &got);
    if (rc != EXT2_OK) {
        emit_err_reason("ext2tail", rc);
        return SHELL_CONTINUE;
    }
    sh_print(SH_PLAIN, "ext2tail: ");
    sh_print(SH_PLAIN, argv[1]);
    sh_print(SH_PLAIN, " (size ");
    sh_print_u32(SH_PLAIN, (uint32_t)size);
    sh_print(SH_PLAIN, ", from offset ");
    sh_print_u32(SH_PLAIN, (uint32_t)off);
    sh_println(SH_PLAIN, ")");
    for (i = 0u; i < got; ++i) {
        char ch = (char)buf[i];
        if (ch == '\n') {
            vga_putchar('\n', VGA_LIGHT_GREY, VGA_BLACK);
        } else if (ch < 32 || ch > 126) {
            vga_putchar('.', VGA_LIGHT_GREY, VGA_BLACK);
        } else {
            vga_putchar(ch, VGA_LIGHT_GREY, VGA_BLACK);
        }
    }
    vga_putchar('\n', VGA_LIGHT_GREY, VGA_BLACK);
    return SHELL_CONTINUE;
}
