/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    user/sigdemo.c
 * Version: 0.2beta
 * Brief:   M17：kill + waitpid 的用户态证据（父子都在 ring3 里演）。
 *
 * 剧本：
 *   1) fork()：子进程进死循环（一次系统调用都不做）；
 *   2) 父进程 sleep 30 tick（300ms），让子在 ring3 里真的转起来；
 *   3) 父进程 kill(子pid, SIGKILL) —— 只是往内核的 pending 位图里置一位；
 *   4) 子进程下一次从时钟中断返回 ring3 之前被内核杀掉，退出码 = 128+9 = 137；
 *   5) 父进程 waitpid(子pid) 精确等这一个孩子，拿回 137。
 *
 * 判据：拿回 137（而不是 -1、也不是 9）。137 这个数字是 Linux 里
 *       WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL 的等价物。
 * ============================================================================ */

typedef unsigned long u64;
typedef long i64;

#define ZH_SYS_PUTC    5ul
#define ZH_SYS_SLEEP   6ul
#define ZH_SYS_FORK    7ul
#define ZH_SYS_GETPID  8ul
#define ZH_SYS_EXIT    3ul
#define ZH_SYS_WAITPID 21ul
#define ZH_SYS_KILL    22ul

#define ZH_SIGKILL     9

static u64 sys_call2(u64 nr, u64 a1, u64 a2)
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
    sys_call2(ZH_SYS_PUTC, (u64)(unsigned long)c, 0u);
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
    u64 pid;
    u64 me;
    u64 code;
    u64 killed;
    volatile u64 spin = 0ul;
    u64 i;

    (void)argc;
    (void)argv;

    me = sys_call2(ZH_SYS_GETPID, 0u, 0u);
    puts1("[sigdemo] parent pid=");
    print_u64(me);
    puts1("\n");

    pid = sys_call2(ZH_SYS_FORK, 0u, 0u);
    if (pid == 0u) {
        puts1("[sigdemo] child: spinning in ring3, no syscalls at all...\n");
        for (;;) {
            for (i = 0ul; i < 16000000ul; ++i) {
                spin += (i ^ (spin >> 3)) + 1ul;
            }
        }
        sys_call2(ZH_SYS_EXIT, 0u, 0u);
        return;
    }
    if (pid == (u64)-1) {
        puts1("[sigdemo] FAIL: fork failed\n");
        sys_call2(ZH_SYS_EXIT, 1u, 0u);
        return;
    }

    puts1("[sigdemo] parent: forked child pid=");
    print_u64(pid);
    puts1(", sleeping 30 ticks (300 ms)\n");
    sys_call2(ZH_SYS_SLEEP, 30u, 0u);

    puts1("[sigdemo] parent: kill(child, SIGKILL) -> ");
    killed = sys_call2(ZH_SYS_KILL, pid, (u64)ZH_SIGKILL);
    puts1(killed == 0u ? "queued\n" : "FAILED\n");

    code = sys_call2(ZH_SYS_WAITPID, pid, 0u);
    puts1("[sigdemo] parent: waitpid(");
    print_u64(pid);
    puts1(") = ");
    print_u64(code);
    puts1("   (128 + 9 = 137 means \"killed by SIGKILL\")\n");

    if (code == 137u) {
        puts1("[sigdemo] PASS: kill() queued a signal, the kernel delivered it on\n");
        puts1("           the child's next return to ring3, and waitpid() collected 137.\n");
        sys_call2(ZH_SYS_EXIT, 0u, 0u);
    } else {
        puts1("[sigdemo] FAIL: unexpected status\n");
        sys_call2(ZH_SYS_EXIT, 1u, 0u);
    }
}
