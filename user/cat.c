/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    user/cat.c
 * Version: 0.2beta
 * Brief:   M14：真正跑在 ring 3 的 cat（/bin/cat.elf）。
 *
 * 以前 cat 是内核里的 cmd_cat()：ramfs_read_file() 一次把整份文件读出来。
 * 这个程序走的是 Linux 的老三样 —— open / read / close（M9 的 11/12/13）：
 *
 *     fd = open(path);   while ((n = read(fd, buf, 256)) > 0) write(1, buf, n);   close(fd);
 *
 * 意义在于：它证明了 M9 的 fd 表不是摆设 —— 一个用户态程序可以按偏移一段一段
 * 读一个大文件，而不是让内核把整份内容塞给它。
 * ============================================================================ */

typedef unsigned long u64;
typedef unsigned int u32;

#define ZH_SYS_WRITE  1ul
#define ZH_SYS_PUTC   5ul
#define ZH_SYS_EXIT   3ul
#define ZH_SYS_OPEN   11ul
#define ZH_SYS_READ   12ul
#define ZH_SYS_CLOSE  13ul

static u64 sys_call(u64 nr, u64 a1, u64 a2, u64 a3)
{
    u64 ret;

    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                     : "memory", "cc");
    return ret;
}

static void putc1(char c)
{
    sys_call(ZH_SYS_PUTC, (u64)(unsigned long)c, 0u, 0u);
}

static void puts1(const char *s)
{
    while (*s != '\0') {
        putc1(*s++);
    }
}

static void print_u64(u64 value)
{
    char tmp[20];
    u32 digits = 0u;
    u32 i;

    if (value == 0u) {
        putc1('0');
        return;
    }
    while (value != 0u && digits < 20u) {
        tmp[digits++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    for (i = 0u; i < digits; ++i) {
        putc1(tmp[digits - 1u - i]);
    }
}

#define CHUNK 256u

static char g_buf[CHUNK + 1u];

void _start(unsigned long argc, char **argv)
{
    u64 fd;
    u64 total = 0u;

    if (argc < 2u || argv[1] == (char *)0) {
        puts1("usage: cat <file>   (ring-3 cat: open / read / close)\n");
        sys_call(ZH_SYS_EXIT, 1u, 0u, 0u);
    }

    fd = sys_call(ZH_SYS_OPEN, (u64)(unsigned long)argv[1], 0u, 0u);
    if (fd == (u64)-1) {
        puts1("cat: ");
        puts1(argv[1]);
        puts1(": cannot open\n");
        sys_call(ZH_SYS_EXIT, 1u, 0u, 0u);
    }
    puts1("cat: open(\"");
    puts1(argv[1]);
    puts1("\") -> fd ");
    print_u64(fd);
    puts1("  (ring 3, one 256-byte chunk at a time)\n");
    puts1("-----------------------------------------------------------\n");

    for (;;) {
        u64 n = sys_call(ZH_SYS_READ, fd, (u64)(unsigned long)g_buf, CHUNK);

        if (n == (u64)-1) {
            puts1("cat: read failed\n");
            sys_call(ZH_SYS_CLOSE, fd, 0u, 0u);
            sys_call(ZH_SYS_EXIT, 1u, 0u, 0u);
        }
        if (n == 0u) {
            break;                      /* EOF */
        }
        total += n;
        /* 直接用 sys_write 输出这一块（逐字节 putc 也能做，但这样更像 Linux 的 write(1)） */
        sys_call(ZH_SYS_WRITE, (u64)(unsigned long)g_buf, n, 0u);
    }
    puts1("\n-----------------------------------------------------------\n");
    puts1("cat: EOF after ");
    print_u64(total);
    puts1(" bytes, close(fd) -> ");
    sys_call(ZH_SYS_CLOSE, fd, 0u, 0u);
    puts1("0\n");
    sys_call(ZH_SYS_EXIT, 0u, 0u, 0u);
}
