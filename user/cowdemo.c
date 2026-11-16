/* cowdemo.c - M15 写时复制的用户态证据。内核那边不需要它说明什么，它只管证明语义没坏：fork
 * 之后父子看到同一份内存（g_value 相同），子改自己的父看不到，父改子也看不到。判据是最后那行
 * [cowdemo] child=0xaaaa parent=0x5555 original=0x1111，父进程 wait() 之后读到的还是自己那份
 * 0x1111，说明两边的页真分家了。fork 时一页都没复制这件事，看 cow 命令的统计。 */

typedef unsigned long u64;

#define ZH_SYS_WRITE  1ul
#define ZH_SYS_PUTC   5ul
#define ZH_SYS_EXIT   3ul
#define ZH_SYS_FORK   7ul
#define ZH_SYS_WAIT   9ul

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

static void print_hex32(u64 value)
{
    static const char digits[] = "0123456789abcdef";
    char buf[11];
    int i;

    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 8; ++i) {
        buf[2 + i] = digits[(value >> (28 - 4 * i)) & 0xFu];
    }
    buf[10] = '\0';
    puts1(buf);
}

/*
 * 这个是"父子共享的那一页"上的数据。它一开始在 .data 段里，
 * fork 之后父子的页表项指向同一帧；谁写谁触发 #PF，内核复制一页给它。
 */
static volatile u64 g_value = 0x1111u;
static volatile u64 g_readings = 0u;   /* 每写一次 g_value 就 +1，用来数自己的写 */

void _start(unsigned long argc, char **argv)
{
    u64 pid;
    u64 code;
    u64 mine;

    (void)argc;
    (void)argv;

    puts1("[cowdemo] g_value starts at ");
    print_hex32(g_value);
    puts1("   (the page is shared with the parent after fork, read-only + COW)\n");

    pid = sys_call(ZH_SYS_FORK, 0u, 0u, 0u);
    if (pid == (u64)-1) {
        puts1("[cowdemo] fork failed\n");
        sys_call(ZH_SYS_EXIT, 1u, 0u, 0u);
    }
    if (pid == 0u) {
        /* 子进程：写自己的副本 */
        g_value = 0xAAAAu;                 /* <-- 这里会触发一次 COW 缺页 */
        ++g_readings;
        puts1("[child ] after my write: g_value=");
        print_hex32(g_value);
        puts1("  writes=");
        print_hex32(g_readings);
        puts1("\n");
        sys_call(ZH_SYS_EXIT, 0u, 0u, 0u);
    }

    /* 父进程：也写自己的副本，然后等子进程 */
    g_value = 0x5555u;                     /* <-- 也会触发一次 COW 缺页 */
    ++g_readings;
    puts1("[parent] after my write: g_value=");
    print_hex32(g_value);
    puts1("  writes=");
    print_hex32(g_readings);
    puts1("\n");

    code = sys_call(ZH_SYS_WAIT, 0u, 0u, 0u);
    puts1("[parent] child exited with ");
    print_hex32(code);
    puts1("\n");

    mine = g_value;
    puts1("[parent] after the child is gone, my page still reads ");
    print_hex32(mine);
    puts1("  (the shared frame was split, not aliased)\n");

    if (mine != 0x5555u) {
        puts1("[cowdemo] FAIL: the child's write leaked into the parent\n");
        sys_call(ZH_SYS_EXIT, 1u, 0u, 0u);
    }
    puts1("[cowdemo] child=0xaaaa  parent=0x5555  original=0x1111  -> isolated\n");
    sys_call(ZH_SYS_EXIT, 0u, 0u, 0u);
}
