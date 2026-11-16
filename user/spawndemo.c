/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    user/spawndemo.c
 * Version: 0.2beta
 * Brief:   M10 演示（进程半）：fork -> 子进程 exec -> 父进程 wait，一条闭环。
 *
 * 这就是 Linux 里 shell 执行一条外部命令的全部内核动作：
 *   pid = fork();
 *   if (pid == 0) { exec("/bin/xxx"); }   // 子进程变成新程序
 *   else          { wait(&code); }        // 父进程等它结束
 * 屏幕上要能看到三件事：
 *   - fork 返回两个不同的值（父 >0，子 ==0）；
 *   - 子进程 exec 之后变成 hello，并且**这个 pid 里跑的是新映像**；
 *   - 父进程被 wait 唤醒并拿到 42（hello 的退出码），说明收尸成功。
 * 全过 exit(0)，否则 exit(9)。
 * ============================================================================ */

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

void _start(void)
{
    static const char prog[] = "/bin/hello.elf";
    u64 parent;
    u64 pid;
    u64 code;

    parent = sys_call(ZH_SYS_GETPID, 0u, 0u, 0u);
    print_str("spawn demo: pid ");
    print_u64(parent);
    print_str(" will fork, the child will exec, the parent will wait\n");

    pid = sys_call(ZH_SYS_FORK, 0u, 0u, 0u);

    if (pid == 0u) {
        /* ---- 子进程：exec 成 hello ---- */
        u64 me = sys_call(ZH_SYS_GETPID, 0u, 0u, 0u);

        print_str("[child ] fork returned 0, I am pid ");
        print_u64(me);
        print_str(", now replacing myself with hello.elf\n");
        sys_call(ZH_SYS_EXEC, (u64)(unsigned long)prog, 0u, 0u);
        print_str("[child ] exec failed\n");
        sys_call(ZH_SYS_EXIT, 9u, 0u, 0u);
    }

    /* ---- 父进程：等子进程收尸 ---- */
    print_str("[parent] fork returned child pid ");
    print_u64(pid);
    print_str("\n");

    code = sys_call(ZH_SYS_WAIT, 0u, 0u, 0u);
    print_str("[parent] wait() -> ");
    print_u64(code);
    print_str("   (child ");
    print_u64(pid);
    print_str(" collected)\n");

    if (code != 42u) {
        print_str("spawn demo: FAILED, expected exit code 42 from hello\n");
        sys_call(ZH_SYS_EXIT, 9u, 0u, 0u);
    }
    print_str("spawn demo: fork -> exec -> wait closed the loop in one process tree\n");
    sys_call(ZH_SYS_EXIT, 0u, 0u, 0u);

    for (;;) {
        __asm__ volatile("hlt");
    }
}
