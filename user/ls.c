/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    user/ls.c
 * Version: 0.2beta
 * Brief:   M14：真正跑在 ring 3 的 ls（/bin/ls.elf）。
 *
 * 以前 ls 是内核里的 cmd_ls()：它直接遍历 ramfs 的 children 链表。
 * 这个程序做同一件事，但它看不见任何 ramfs 结构体 —— 它只有 SYS_READDIR：
 *
 *     readdir(path, index, name_buf) -> 0 = 文件，1 = 目录，-1 = 没有了
 *
 * 用法（和 Linux 一样）：
 *     ls            列当前目录（靠 SYS_GETCWD 拿到路径）
 *     ls /bin       列指定目录
 *
 * 目录后面补一个 '/'，这是本版 ls 唯一"花"的一点点格式。
 * ============================================================================ */

typedef unsigned long u64;
typedef unsigned int u32;

#define ZH_SYS_WRITE   1ul
#define ZH_SYS_PUTC    5ul
#define ZH_SYS_EXIT    3ul
#define ZH_SYS_GETCWD  19ul
#define ZH_SYS_READDIR 20ul

static u64 sys_call(u64 nr, u64 a1, u64 a2, u64 a3)
{
    u64 ret;

    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                     : "memory", "cc");
    return ret;
}

static char g_cwd[64];

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

void _start(unsigned long argc, char **argv)
{
    const char *path;
    char name[64];
    u32 index = 0u;
    u32 count = 0u;
    u64 r;

    if (argc > 1u && argv[1] != (char *)0) {
        path = argv[1];
    } else {
        if (sys_call(ZH_SYS_GETCWD, (u64)(unsigned long)g_cwd, 63u, 0u)
            == (u64)-1) {
            puts1("ls: cannot get the current directory\n");
            sys_call(ZH_SYS_EXIT, 1u, 0u, 0u);
        }
        path = g_cwd;
    }

    for (;;) {
        r = sys_call(ZH_SYS_READDIR, (u64)(unsigned long)path, (u64)index,
                     (u64)(unsigned long)name);
        if (r == (u64)-1) {
            break;                       /* 枚举结束（或不是目录） */
        }
        puts1(name);
        if (r == 1u) {
            putc1('/');                  /* 目录：补一个 '/' */
        }
        putc1('\n');
        ++count;
        ++index;
    }

    if (count == 0u) {
        puts1("ls: ");
        puts1(path);
        puts1(": empty or not a directory\n");
        sys_call(ZH_SYS_EXIT, 1u, 0u, 0u);
    }
    puts1("(");
    print_u64(count);
    puts1(" entries, read one by one through SYS_READDIR)\n");
    sys_call(ZH_SYS_EXIT, 0u, 0u, 0u);
}
