/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    user/reexec.c
 * Version: 0.2beta
 * Brief:   M12 演示：一个程序连续 exec 自己 7 次，用 argv 传"剩余深度"。
 *
 * 为什么要这个程序？因为 MM 回收（M12）没法直接"看见"—— 它只表现为
 * "页帧数不涨"。所以要有一个**反复 exec** 的东西去压力测试它：
 *
 *   depth = argv[1]（缺省 6）
 *   depth > 0 : 打印一行，然后 exec("/bin/reexec.elf", {"reexec", depth-1})
 *   depth == 0: 打印收尾，exit(0)
 *
 * 每次 exec 都会建一整套新 mm（4 张页表 + 代码页 + 栈页）。如果不回收，
 * 8 层下来要吃掉 40 多帧；M12 之后，跑完的帧数应该和跑一次几乎一样。
 * 这也顺便再次验证了 M11 的 argv（深度就是靠 argv 传下去的，没有 argv 就没法写这个测试）。
 * ============================================================================ */

typedef unsigned long u64;

#define ZH_SYS_WRITE  1ul
#define ZH_SYS_EXIT   3ul
#define ZH_SYS_GETPID 8ul
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

static char g_out[160];

static void print(u64 total)
{
    sys_call(ZH_SYS_WRITE, (u64)(unsigned long)g_out, total, 0u);
}

static void print_str(const char *s)
{
    u64 n = 0u;

    while (s[n] != '\0' && n < 150u) {
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

void _start(unsigned long argc, char **argv)
{
    static const char self[] = "/bin/reexec.elf";
    static char next[8];          /* 下一层的深度字符串（可写，在 .data 里） */
    char *args[3];
    u64 depth = 6u;
    u64 me;

    /* argv[1] 是十进制深度；内核装载器起的时候 argc=0，就用缺省 6 */
    if (argc > 1u && argv[1] != (char *)0) {
        depth = 0u;
        while (argv[1][0] >= '0' && argv[1][0] <= '9') {
            depth = depth * 10u + (u64)(argv[1][0] - '0');
            ++argv[1];
        }
    }

    me = sys_call(ZH_SYS_GETPID, 0u, 0u, 0u);

    if (depth == 0u) {
        print_str("[reexec] pid ");
        print_u64(me);
        print_str(": depth 0 reached, no more exec. exiting 0\n");
        sys_call(ZH_SYS_EXIT, 0u, 0u, 0u);
    }

    print_str("[reexec] pid ");
    print_u64(me);
    print_str(": depth ");
    print_u64(depth);
    print_str("  -> exec myself with ");
    print_u64(depth - 1u);
    print_str("\n");

    /* 把 depth-1 写成字符串 */
    {
        u64 v = depth - 1u;
        char tmp[8];
        u64 d = 0u;
        u64 i;

        if (v == 0u) {
            next[0] = '0';
            next[1] = '\0';
        } else {
            while (v != 0u && d < 7u) {
                tmp[d++] = (char)('0' + (v % 10u));
                v /= 10u;
            }
            for (i = 0u; i < d; ++i) {
                next[i] = tmp[d - 1u - i];
            }
            next[d] = '\0';
        }
    }

    args[0] = (char *)(unsigned long)self;
    args[1] = next;
    args[2] = (char *)0;
    sys_call(ZH_SYS_EXEC, (u64)(unsigned long)self,
             (u64)(unsigned long)args, 2u);

    print_str("[reexec] exec() failed\n");
    sys_call(ZH_SYS_EXIT, 9u, 0u, 0u);

    for (;;) {
        __asm__ volatile("hlt");
    }
}
