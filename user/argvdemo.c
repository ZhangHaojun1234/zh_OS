/* argvdemo.c - M11 的演示程序，_start 第一次能拿到 argc / argv。内核 exec 成功后把 rdi=argc、
 * rsi=&argv[0] 交给 _start，参数字符串铺在新地址空间栈页底部，所以这里直接按 C ABI 写成
 * void _start(unsigned long argc, char argv)。argc == 0 说明我是内核起的：fork 一个子进程，
 * 让它带 {"argvdemo", "alpha", "beta"} 三个参数 exec 自己，然后等它；argc > 0 说明我是被 exec
 * 起来的：把 argc 和每个 argv 打出来，exit(0)。 */

typedef unsigned long u64;

#define ZH_SYS_WRITE  1ul
#define ZH_SYS_EXIT   3ul
#define ZH_SYS_FORK   7ul
#define ZH_SYS_GETPID 8ul
#define ZH_SYS_WAIT   9ul
#define ZH_SYS_EXEC   10ul

static u64 sys_call(u64 nr, u64 a1, u64 a2, u64 a3)
{
    u64 ret;

    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                     : "memory", "cc");
    return ret;
}

static char g_out[128];

static void print(u64 total)
{
    sys_call(ZH_SYS_WRITE, (u64)(unsigned long)g_out, total, 0u);
}

static void print_str(const char *s)
{
    u64 n = 0u;

    while (s[n] != '\0') {
        g_out[n] = s[n];
        ++n;
    }
    print(n);
}

static void print_u64(u64 value)
{
    char tmp[20];
    u64 digits = 0u;
    u64 n;

    if (value == 0u) {
        g_out[0] = '0';
        print(1u);
        return;
    }
    while (value != 0u && digits < 20u) {
        tmp[digits++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    for (n = 0u; n < digits; ++n) {
        g_out[n] = tmp[digits - 1u - n];
    }
    print(digits);
}

/* 入口：argc / argv 由内核通过 rdi / rsi 传进来（C ABI 就是这样） */
void _start(unsigned long argc, char **argv)
{
    static const char self[] = "/bin/argvdemo.elf";
    u64 me;
    u64 i;

    if (argc > 0u) {
        /* 我是被 exec 起来的 */
        me = sys_call(ZH_SYS_GETPID, 0u, 0u, 0u);
        print_str("[child ] I was exec()d with argc=");
        print_u64(argc);
        print_str("  (pid ");
        print_u64(me);
        print_str(")\n");
        for (i = 0u; i < argc; ++i) {
            print_str("          argv[");
            print_u64(i);
            print_str("] = \"");
            print_str(argv[i]);
            print_str("\"\n");
        }
        print_str("[child ] argv came from the kernel, through the new stack page\n");
        sys_call(ZH_SYS_EXIT, 0u, 0u, 0u);
    }

    /* 我是装载器起的：fork + exec（带参数）+ wait */
    me = sys_call(ZH_SYS_GETPID, 0u, 0u, 0u);
    print_str("argv demo: pid ");
    print_u64(me);
    print_str(" starts with argc=0 (loaded by the kernel, no argv)\n");
    print_str("           now forking a child that will exec me again WITH argv\n");

    me = sys_call(ZH_SYS_FORK, 0u, 0u, 0u);
    if (me == 0u) {
        static char a0[] = "argvdemo";
        static char a1[] = "alpha";
        static char a2[] = "beta";
        char *args[4];

        args[0] = a0;
        args[1] = a1;
        args[2] = a2;
        args[3] = (char *)0;
        sys_call(ZH_SYS_EXEC, (u64)(unsigned long)self,
                 (u64)(unsigned long)args, 3u);
        print_str("[child ] exec with argv failed\n");
        sys_call(ZH_SYS_EXIT, 9u, 0u, 0u);
    }

    {
        u64 code = sys_call(ZH_SYS_WAIT, 0u, 0u, 0u);

        print_str("[parent] wait() -> ");
        print_u64(code);
        print_str("   (the child printed its argv and exited 0)\n");
        if (code != 0u) {
            print_str("argv demo: FAILED\n");
            sys_call(ZH_SYS_EXIT, 9u, 0u, 0u);
        }
        print_str("argv demo: argc/argv crossed the exec boundary intact\n");
        sys_call(ZH_SYS_EXIT, 0u, 0u, 0u);
    }

    for (;;) {
        __asm__ volatile("hlt");
    }
}
