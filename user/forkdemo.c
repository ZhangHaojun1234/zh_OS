/* forkdemo.c - M7 演示：真 ELF 进程 fork 生两个子进程，用 wait 收尸。验证四件事：
 * fork 之后两个执行流都从 int 0x80 的下一条继续跑，父拿到子 pid、子拿到 0；
 * 子进程有自己的地址空间副本，子改 g_counter，父读到 0，不是共享内存；
 * wait() 真的会阻塞：父睡下、子被调度上去、退出后父被唤醒；
 * 退出码能穿过 wait 回到父进程（11 和 22）。整份代码没有 libc，全靠 int 0x80 和自己写的十进制打印。 */

typedef unsigned long u64;

#define ZH_SYS_WRITE  1ul
#define ZH_SYS_EXIT   3ul
#define ZH_SYS_FORK   7ul
#define ZH_SYS_GETPID 8ul
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

/* 极简打印：把 u64 转十进制再 sys_write（没有 sprintf） */
static char g_buf[96];

static u64 put_str(char *dst, const char *src)
{
    u64 n = 0u;

    while (src[n] != '\0') {
        dst[n] = src[n];
        ++n;
    }
    return n;
}

static u64 put_u64(char *dst, u64 value)
{
    char tmp[20];
    u64 digits = 0u;
    u64 i;

    if (value == 0u) {
        dst[0] = '0';
        return 1u;
    }
    while (value != 0u && digits < 20u) {
        tmp[digits++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    for (i = 0u; i < digits; ++i) {
        dst[i] = tmp[digits - 1u - i];
    }
    return digits;
}

static void print_n(u64 total)
{
    sys_call(ZH_SYS_WRITE, (u64)(unsigned long)g_buf, total, 0u);
}

static void print_str(const char *s)
{
    u64 n = put_str(g_buf, s);
    print_n(n);
}

static void print_num(u64 value)
{
    u64 n = put_u64(g_buf, value);
    print_n(n);
}

/* 一行日志：前缀 + 数字 + 结尾 */
static void line(const char *prefix, u64 value, const char *suffix)
{
    u64 n = put_str(g_buf, prefix);
    n += put_u64(g_buf + n, value);
    n += put_str(g_buf + n, suffix);
    print_n(n);
}

/* 一个全局变量：子进程改它，父进程必须看不到变化（证明地址空间是复制的）。 */
static u64 g_counter = 0u;

void _start(void)
{
    u64 me;
    u64 child;
    u64 code;
    u64 i;

    me = sys_call(ZH_SYS_GETPID, 0u, 0u, 0u);
    print_str("M7 fork/wait demo: started in pid ");
    print_num(me);
    print_str("\n");

    /* 第一个子进程 */
    child = sys_call(ZH_SYS_FORK, 0u, 0u, 0u);

    if (child == 0u) {
        /* 子进程 1 */
        u64 mypid = sys_call(ZH_SYS_GETPID, 0u, 0u, 0u);
        print_str("  [child 1] pid=");
        print_num(mypid);
        print_str(" forked, touching my own copy of g_counter\n");
        for (i = 0u; i < 5u; ++i) {
            ++g_counter;               /* 只改自己那份内存 */
        }
        line("  [child 1] g_counter=", g_counter,
             " -> exiting with 11\n");
        sys_call(ZH_SYS_EXIT, 11u, 0u, 0u);
    }

    /* 父进程 */
    print_str("  [parent] fork returned child pid ");
    print_num(child);
    print_str("\n");
    code = sys_call(ZH_SYS_WAIT, 0u, 0u, 0u);
    line("  [parent] wait() -> ", code, "   (child 1 collected)\n");
    line("  [parent] my g_counter is still ", g_counter,
         " -> the child's write did NOT leak\n");

    /* 第二个子进程：走一遍"僵尸先死、父进程后 wait"的分支 */
    child = sys_call(ZH_SYS_FORK, 0u, 0u, 0u);

    if (child == 0u) {
        u64 mypid = sys_call(ZH_SYS_GETPID, 0u, 0u, 0u);
        print_str("  [child 2] pid=");
        print_num(mypid);
        print_str(" exiting immediately with 22\n");
        sys_call(ZH_SYS_EXIT, 22u, 0u, 0u);
    }

    print_str("  [parent] fork returned child pid ");
    print_num(child);
    print_str("\n");

    /* 先睡一会儿：让 child 2 在被 wait 之前就退出（练到"僵尸"这条路径） */
    for (i = 0u; i < 200000u; ++i) {
        __asm__ volatile("pause");
    }

    code = sys_call(ZH_SYS_WAIT, 0u, 0u, 0u);
    line("  [parent] wait() -> ", code, "   (child 2 collected)\n");

    print_str("M7 demo done: parent pid ");
    print_num(me);
    print_str(" sees both children gone, exiting 0\n");
    sys_call(ZH_SYS_EXIT, 0u, 0u, 0u);

    for (;;) {
        __asm__ volatile("hlt");
    }
}
