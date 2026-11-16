/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    user/jobdemo.c
 * Version: 0.2beta
 * Brief:   M18 证据二：一个"长跑靶子" —— 作业控制（^Z / fg / bg / jobs）要用它。
 *
 * 它只做一件事：每 100ms 打印一行 tick，永远不自己退出。
 *   - 按 ^Z（SIGTSTP/SIGSTOP）-> 内核把它标成 TASK_STOP，pick_next 不再挑它；
 *     屏幕上的 tick 立刻停住，但进程还在内存里（jobs 里显示 Stopped）；
 *   - `fg` / `bg`（SIGCONT）-> 状态改回 TASK_READY，它从**被停住的那一行**
 *     接着往下数（寄存器现场一直在 TCB 里，没动过）；
 *   - 按 ^C（SIGINT）-> 默认动作，退出码 130。
 *
 * 判据：停住前后 tick 号必须连得上（12 -> 停 -> 继续 -> 13），中间不丢不重。
 * ============================================================================ */

typedef unsigned long u64;

#define ZH_SYS_PUTC      5ul
#define ZH_SYS_SLEEP     6ul
#define ZH_SYS_EXIT      3ul
#define ZH_SYS_GETPID    8ul
#define ZH_SYS_SIGNAL    24ul

#define ZH_SIGINT        2u

static u64 sys_call2(u64 nr, u64 a1, u64 a2)
{
    u64 ret;

    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2)
                     : "memory", "cc");
    return ret;
}

static void putc1(char c) { sys_call2(ZH_SYS_PUTC, (u64)(unsigned long)c, 0u); }

static void puts1(const char *s)
{
    while (*s != '\0') {
        putc1(*s++);
    }
}

static void print_u64(u64 value)
{
    char tmp[20];
    u64 digits = 0u;
    u64 i;

    if (value == 0u) {
        putc1('0');
        return;
    }
    while (value != 0u && digits < 20u) {
        tmp[digits++] = (char)('0' + (int)(value % 10u));
        value /= 10u;
    }
    for (i = 0u; i < digits; ++i) {
        putc1(tmp[digits - 1u - i]);
    }
}

void _start(unsigned long argc, char **argv)
{
    u64 me;
    u64 tick = 0ul;

    (void)argc;
    (void)argv;

    me = sys_call2(ZH_SYS_GETPID, 0u, 0u);
    puts1("[jobdemo] pid=");
    print_u64(me);
    puts1(" long-running target: one tick every 100ms\n");
    puts1("[jobdemo] ^Z stops me, fg/bg (SIGCONT) resumes me, ^C kills me (130)\n");

    for (;;) {
        ++tick;
        /* 每 20 个 tick（约 2 秒）打一行：屏幕只有 24 行，别把作业控制的证据刷掉 */
        if ((tick % 20ul) == 0ul) {
            puts1("[jobdemo] tick ");
            print_u64(tick);
            puts1("  (still the same process, same frame)\n");
        }
        sys_call2(ZH_SYS_SLEEP, 10u, 0u);   /* 100ms */
    }
    sys_call2(ZH_SYS_EXIT, 0u, 0u);
}
