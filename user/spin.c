/* spin.c - 一个纯自旋程序，一次系统调用都不做，专门用来证明信号能打断根本不进内核的进程：
 * 它只会在时钟中断（IRQ0）里被迫进内核，内核在 iretq 回 ring3 之前查 pending 信号，于是它被 SIGINT 杀掉。
 * Linux 也是这样，信号不是等程序主动查才生效，是靠中断返回路径送进去的。
 * 用法：在 zhsh3 里跑 spin，然后按 Ctrl-C。 */

typedef unsigned long u64;

#define ZH_SYS_PUTC   5ul
#define ZH_SYS_EXIT   3ul

static u64 sys_call(u64 nr, u64 a1, u64 a2)
{
    u64 ret;

    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2)
                     : "memory", "cc");
    return ret;
}

static void putc1(char c)
{
    sys_call(ZH_SYS_PUTC, (u64)(unsigned long)c, 0u);
}

static void puts1(const char *s)
{
    while (*s != '\0') {
        putc1(*s++);
    }
}

void _start(unsigned long argc, char **argv)
{
    u64 i;
    volatile u64 spin = 0ul;

    (void)argc;
    (void)argv;

    puts1("[spin] I never call a syscall: only timer IRQs can interrupt me.\n");
    puts1("[spin] press Ctrl-C -- the kernel should kill me on IRQ0's return path.\n");
    puts1("[spin] (screen will now freeze: this is a pure spin loop)\n");

    for (;;) {
        /* 纯算术自旋：没有 syscall，没有 hlt，只有 IRQ0 会把我打进内核 */
        for (i = 0ul; i < 16000000ul; ++i) {
            spin += (i ^ (spin >> 3)) + 1ul;
        }
    }
    sys_call(ZH_SYS_EXIT, 0u, 0u);
}
