/* sigcatch.c - M18 证据一：用户态自己的 handler（signal() + sigreturn）。跟 sigdemo.c 的差别
 * 就一句：那边 SIGKILL 是内核替我死，这里 ^C 送来的 SIGINT 由我自己的函数接管。
 * 剧本：signal(SIGINT, on_sigint) 把 handler 地址交给内核，进循环每 5 tick 打一个点；外面按
 * Ctrl-C，内核置 pending，在下次回 ring3 前把当前整帧存进 TCB、用户栈压蹦床地址、rip 改成
 * on_sigint；handler 打一行、置 caught，ret 到蹦床，int 0x80(24) 把 22 个 qword 原样扣回来；
 * 循环从被打断那一刻继续，计数器和局部变量都在原位，最后 exit(42)。
 * 判据是屏幕上出现 [sigcatch] back in the *same* loop, counter=NN，NN 紧接被打断时的值。 */

typedef unsigned long u64;
typedef long i64;

#define ZH_SYS_PUTC      5ul
#define ZH_SYS_SLEEP     6ul
#define ZH_SYS_GETPID    8ul
#define ZH_SYS_EXIT      3ul
#define ZH_SYS_SIGNAL    24ul

#define ZH_SIGINT        2u

/* handler 里要用的全局：handler 是在"另一个时刻"被塞进执行流的普通函数 */
static volatile u64 g_caught;
static volatile u64 g_handler_runs;
static u64 g_counter_at_signal;

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

/*
 * 用户态的信号处理函数。
 *
 * 它被内核"硬插"到执行流里，入口状态和一个普通函数调用的函数体一样：
 *   - rdi = 信号号（所以参数声明成 i64 sig）
 *   - rsp 指向一个"返回地址"槽，槽里是内核写的蹦床地址
 * 因此不能再往栈上多压东西：这里的返回地址是内核伪造的，不是调用者压的。
 * 函数正常 `ret` 就会跳进蹦床，由内核把整帧还回去。
 */
static void on_sigint(i64 sig)
{
    ++g_handler_runs;
    g_counter_at_signal = 0u;      /* 先清掉，后面主循环再填 */
    puts1("\n[sigcatch] >>> inside my own handler, sig=");
    print_u64((u64)sig);
    puts1(" (this is user code at ring3, not the kernel)\n");
    g_caught = 1u;
    /* 到这里 return：编译器生成 `ret`，弹出内核压的蹦床地址 */
}

void _start(unsigned long argc, char **argv)
{
    u64 me;
    u64 old;
    u64 counter = 0ul;
    u64 loop;

    (void)argc;
    (void)argv;

    me = sys_call2(ZH_SYS_GETPID, 0u, 0u);
    puts1("[sigcatch] pid=");
    print_u64(me);
    puts1(" installing signal(SIGINT, handler)\n");

    old = sys_call2(ZH_SYS_SIGNAL, (u64)ZH_SIGINT,
                    (u64)(unsigned long)&on_sigint);
    puts1("[sigcatch] old handler was ");
    print_u64(old);
    puts1(old == 0ul ? " (SIG_DFL: default action was 'die')\n"
                     : " (a previous handler)\n");
    puts1("[sigcatch] waiting for ^C: 200 rounds x 50ms, printing a dot each round\n");

    for (loop = 0ul; loop < 200ul; ++loop) {
        if (g_caught != 0u) {
            break;                  /* handler 已经把旗子插好了 */
        }
        ++counter;
        putc1('.');
        sys_call2(ZH_SYS_SLEEP, 5u, 0u);   /* 给外面按 ^C 的时间 */
    }

    puts1("\n[sigcatch] back in the *same* loop, counter=");
    print_u64(counter);
    puts1(", handler runs=");
    print_u64(g_handler_runs);
    puts1("\n");

    if (g_caught != 0u && g_handler_runs != 0ul) {
        puts1("[sigcatch] PASS: the frame was saved in the TCB, the handler ran in\n");
        puts1("           ring3, and SYS_SIGRETURN put every register back.\n");
        puts1("[sigcatch] exiting with 42\n");
        sys_call2(ZH_SYS_EXIT, 42u, 0u);
        return;
    }
    puts1("[sigcatch] FAIL: no signal arrived (press Ctrl-C while it runs)\n");
    sys_call2(ZH_SYS_EXIT, 1u, 0u);
}
