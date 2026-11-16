/* zhsh3.c - M13：跑在 ring 3 的 shell（/bin/zhsh3.elf），这一步最像 Linux。
 * M12 之前，shell 是内核里的一个循环加一张函数表，它自己就在 ring0，命令是内核函数，
 * 用户敲一条命令其实什么都没发生 ， 没有进程，也没有特权切换。
 * 这个程序跑在 ring3，能碰的只有几个系统调用：SYS_READKEY（一个字符一个字符取键盘，内核不再替它读整行）、
 * SYS_PUTC、SYS_FORK、SYS_EXEC、SYS_WAIT、SYS_CHDIR / SYS_GETCWD。
 * 它执行一条外部命令走的是 fork -> exec -> wait，跟 Linux 一模一样，少一个系统调用它就活不下去。
 * help / exit / pwd / cd / echo 这类 builtin 由它自己实现，这也是 Linux 的分法：
 * 少数命令在 shell 进程里跑，其余全是独立进程。 */

typedef unsigned long u64;
typedef unsigned int u32;

#define ZH_SYS_WRITE  1ul
#define ZH_SYS_EXIT   3ul
#define ZH_SYS_PUTC   5ul
#define ZH_SYS_SLEEP  6ul
#define ZH_SYS_FORK   7ul
#define ZH_SYS_GETPID 8ul
#define ZH_SYS_WAIT   9ul
#define ZH_SYS_EXEC   10ul
#define ZH_SYS_READKEY 17ul
#define ZH_SYS_CHDIR   18ul
#define ZH_SYS_GETCWD  19ul
#define ZH_SYS_WAITPID 21ul   /* [ADD] M17 */
#define ZH_SYS_SETFG   23ul   /* [ADD] M17 */
/* [ADD] M18：作业控制 */
#define ZH_SYS_SIGNAL  24ul   /* signal(sig, handler) */
#define ZH_SYS_KILL    22ul   /* [ADD] M18：用来发 SIGCONT */
#define ZH_SYS_GETSTATE 26ul  /* 查任务状态（0 FREE 1 READY 2 RUNNING 3 DEAD 4 WAIT 5 STOP） */
#define ZH_SIGINT      2u
#define ZH_SIGCONT     18u    /* 继续 */
#define ZH_SIGSTOP     19u
#define ZH_WNOHANG     1u     /* waitpid：不阻塞 */
#define ZH_WUNTRACED   2u     /* waitpid：被停住的子进程也算有状态变化 */
#define ZH_WSTOPPED    0x10000u   /* 返回值里的停止标记 */
#define ZH_ST_DEAD     3
#define ZH_ST_STOP     5

/* [ADD] M17：退出码 >= 128 时打印信号名，跟 Linux 的 sh 一个习惯 */
static const char *sig_name(int sig)
{
    switch (sig) {
    case 2:  return "SIGINT";
    case 9:  return "SIGKILL";
    case 15: return "SIGTERM";
    default: return "SIG?";
    }
}

static u64 sys_call(u64 nr, u64 a1, u64 a2, u64 a3)
{
    u64 ret;

    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                     : "memory", "cc");
    return ret;
}

/* 输出：一字符、一行 */

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

/* 字符串工具 */

static int seq(const char *a, const char *b)
{
    u32 i = 0u;

    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return 0;
        }
        ++i;
    }
    return (a[i] == '\0' && b[i] == '\0') ? 1 : 0;
}

/* 读一行：一个键一个键地取 */

#define LINE_MAX 96u

static char g_line[LINE_MAX + 1u];

/*
 * 取一行。屏幕上已经有提示符（调用者打的），这里只负责回显与编辑。
 * 缓冲区空的时候不忙等：sleep 1 tick（10ms）再问，把 CPU 让给别的任务 ，
 * 这正是 Linux shell 在 read(0) 上阻塞时内核替它干的事，只是这里由我们自己轮询。
 */
static u32 read_line(void)
{
    u32 len = 0u;
    u64 key;

    for (;;) {
        key = sys_call(ZH_SYS_READKEY, 0u, 0u, 0u);
        if (key == 0u) {
            sys_call(ZH_SYS_SLEEP, 1u, 0u, 0u);
            continue;
        }
        if (key == '\n' || key == '\r') {
            putc1('\n');
            g_line[len] = '\0';
            return len;
        }
        if (key == 0x08u || key == 0x7Fu) {          /* 退格 */
            if (len > 0u) {
                --len;
                putc1('\b');
            }
            continue;
        }
        if (key < 0x20u || key > 0x7Eu) {            /* 控制键 / 扩展键：忽略 */
            continue;
        }
        if (len < LINE_MAX) {
            g_line[len++] = (char)key;
            putc1((char)key);                        /* 回显由 shell 自己负责 */
        }
    }
}

/* 拆成 argv（就地切空格） */

#define ARG_MAX 8u

static char *g_argv[ARG_MAX + 1u];

static u32 split_args(void)
{
    u32 n = 0u;
    u32 i = 0u;

    while (g_line[i] != '\0' && n < ARG_MAX) {
        while (g_line[i] == ' ' || g_line[i] == '\t') {
            ++i;
        }
        if (g_line[i] == '\0') {
            break;
        }
        g_argv[n++] = &g_line[i];
        while (g_line[i] != '\0' && g_line[i] != ' ' && g_line[i] != '\t') {
            ++i;
        }
        if (g_line[i] != '\0') {
            g_line[i++] = '\0';
        }
    }
    g_argv[n] = (char *)0;
    return n;
}

/* 外部命令：fork -> exec -> wait */

#define PATH_MAX 64u

static char g_path[PATH_MAX + 1u];
static char g_cwd[PATH_MAX + 1u];

static void build_path(const char *name)
{
    u32 i = 0u;

    if (name[0] == '/') {
        while (name[i] != '\0' && i < PATH_MAX) {
            g_path[i] = name[i];
            ++i;
        }
        g_path[i] = '\0';
        return;
    }
    /* 不带路径：找 /bin/<name>.elf ， 本版的"PATH 搜索"就这一行 */
    g_path[0] = '/';
    g_path[1] = 'b';
    g_path[2] = 'i';
    g_path[3] = 'n';
    g_path[4] = '/';
    i = 5u;
    while (name[i - 5u] != '\0' && i < PATH_MAX - 5u) {
        g_path[i] = name[i - 5u];
        ++i;
    }
    g_path[i++] = '.';
    g_path[i++] = 'e';
    g_path[i++] = 'l';
    g_path[i++] = 'f';
    g_path[i] = '\0';
}

/*
 * 执行一条外部命令。返回子进程的退出码。
 * 这一段就是"ring3 shell 执行一条命令"的全部内核动作，一眼能看完：
 *   fork()                ， 复制一个我自己
 *   (子) exec(path, argv)  ， 把自己换成那个程序（argv 从 M11 起能带过去）
 *   (父) wait()            ， 等它退出、拿退出码
 */
/* [ADD] M18：作业表（shell 自己维护，内核只提供 pid 上的状态）            */

#define JOB_MAX      4u
#define JOB_FREE     0u
#define JOB_RUNNING  1u
#define JOB_STOPPED  2u

typedef struct {
    u32 used;                 /* 表项是否占用 */
    u32 pid;
    u32 state;                /* JOB_RUNNING / JOB_STOPPED */
    u32 last_code;            /* 最近一次等到的退出码 / 停止信号 */
    char name[24];
} job_t;

static job_t g_jobs[JOB_MAX];

static u32 job_slot_get(u32 number)     /* number 从 1 开始，0 = 失败 */
{
    if (number == 0u || number > JOB_MAX) {
        return 0u;
    }
    if (g_jobs[number - 1u].used == 0u) {
        return 0u;
    }
    return number;
}

static u32 job_slot_pid(u32 pid)
{
    u32 i;
    for (i = 0u; i < JOB_MAX; ++i) {
        if (g_jobs[i].used != 0u && g_jobs[i].pid == pid) {
            return i + 1u;
        }
    }
    return 0u;
}

static u32 job_add(u32 pid, const char *name)
{
    u32 i;
    u32 k = 0u;

    for (i = 0u; i < JOB_MAX; ++i) {
        if (g_jobs[i].used == 0u) {
            g_jobs[i].used = 1u;
            g_jobs[i].pid = pid;
            g_jobs[i].state = JOB_RUNNING;
            g_jobs[i].last_code = 0u;
            while (name[k] != '\0' && k + 1u < sizeof(g_jobs[i].name)) {
                g_jobs[i].name[k] = name[k];
                ++k;
            }
            g_jobs[i].name[k] = '\0';
            return i + 1u;
        }
    }
    return 0u;                    /* 表满：作业照样跑，只是不记账 */
}

static void job_forget(u32 pid)
{
    u32 n = job_slot_pid(pid);
    if (n != 0u) {
        g_jobs[n - 1u].used = 0u;
    }
}

/*
 * 把 pid 标成前台并把控制权交过去，直到它退出或被 ^Z 停住。
 * 返回内核给的 status：正常退出 = 退出码；被信号杀 = 128+sig；被停 = 0x10000|sig。
 */
static u64 wait_foreground(u32 pid)
{
    u64 code;

    sys_call(ZH_SYS_SETFG, (u64)pid, 0u, 0u);
    code = sys_call(ZH_SYS_WAITPID, (u64)pid, (u64)ZH_WUNTRACED, 0u);
    sys_call(ZH_SYS_SETFG, (u64)-1, 0u, 0u);   /* 交回提示符：前台清空 */
    return code;
}

static void job_print_line(u32 number, const char *tag, const char *name)
{
    putc1('[');
    print_u64((u64)number);
    putc1(']');
    putc1(' ');
    puts1(tag);
    putc1(' ');
    putc1(' ');
    puts1(name);
    putc1('\n');
}

/* `jobs` 内置命令：逐个报告，顺便收掉已经结束的 */
static void cmd_jobs(void)
{
    u32 i;
    u32 shown = 0u;

    for (i = 0u; i < JOB_MAX; ++i) {
        job_t *j = &g_jobs[i];
        u64 st;

        if (j->used == 0u) {
            continue;
        }
        /* 活着与否直接问内核：Linux 是读 /proc/<pid>/stat，这里是 SYS_GETSTATE */
        st = sys_call(ZH_SYS_GETSTATE, (u64)j->pid, 0u, 0u);
        if (st == (u64)-1 || (int)st == ZH_ST_DEAD) {
            u64 code = sys_call(ZH_SYS_WAITPID, (u64)j->pid, (u64)ZH_WNOHANG, 0u);
            job_print_line(i + 1u, "Done    ", j->name);
            puts1("       (reaped, status ");
            print_u64(code);
            puts1(")\n");
            j->used = 0u;
            ++shown;
            continue;
        }
        if ((int)st == ZH_ST_STOP) {
            job_print_line(i + 1u, "Stopped ", j->name);
            puts1("       (in memory, TASK_STOP, not scheduled)\n");
        } else {
            job_print_line(i + 1u, "Running ", j->name);
            puts1("       (still in the ready/run queue)\n");
        }
        ++shown;
    }
    if (shown == 0u) {
        puts1("zhsh3: no jobs\n");
    }
}

static void cmd_fg(u32 argc)
{
    u32 n = 0u;
    u32 slot;
    u32 pid;
    u64 code;
    u32 k = 0u;

    if (argc >= 2u) {
        while (g_argv[1][k] >= '0' && g_argv[1][k] <= '9') {
            n = n * 10u + (u32)(g_argv[1][k] - '0');
            ++k;
        }
    }
    slot = job_slot_get(n == 0u ? 1u : n);   /* 不带参数 = 最近那个 */
    if (slot == 0u) {
        puts1("zhsh3: fg: no such job\n");
        return;
    }
    pid = g_jobs[slot - 1u].pid;
    puts1(g_jobs[slot - 1u].name);
    puts1(" (pid ");
    print_u64((u64)pid);
    puts1(") -> SIGCONT, foreground\n");

    /* SIGCONT 把它从 TASK_STOP 放回就绪队列（Linux 里 fg 也是先 kill(pid, SIGCONT)） */
    sys_call(ZH_SYS_KILL, (u64)pid, (u64)ZH_SIGCONT, 0u);
    g_jobs[slot - 1u].state = JOB_RUNNING;

    code = wait_foreground(pid);
    if ((code & ZH_WSTOPPED) != 0u) {
        g_jobs[slot - 1u].state = JOB_STOPPED;
        g_jobs[slot - 1u].last_code = (u32)(code & 0xFFFFu);
        job_print_line(slot, "Stopped ", g_jobs[slot - 1u].name);
        return;
    }
    if (code >= 128u && code != (u64)-1) {
        puts1("[");
        puts1(g_jobs[slot - 1u].name);
        puts1(" killed by signal ");
        print_u64(code - 128u);
        puts1("]\n");
    } else if (code != 0u) {
        puts1("[");
        puts1(g_jobs[slot - 1u].name);
        puts1(" exited ");
        print_u64(code);
        puts1("]\n");
    }
    job_forget(pid);
}

static void cmd_bg(u32 argc)
{
    u32 n = 0u;
    u32 slot;
    u32 pid;
    u32 k = 0u;

    if (argc >= 2u) {
        while (g_argv[1][k] >= '0' && g_argv[1][k] <= '9') {
            n = n * 10u + (u32)(g_argv[1][k] - '0');
            ++k;
        }
    }
    slot = job_slot_get(n == 0u ? 1u : n);   /* 不带参数 = 最近那个 */
    if (slot == 0u) {
        puts1("zhsh3: bg: no such job\n");
        return;
    }
    pid = g_jobs[slot - 1u].pid;
    sys_call(ZH_SYS_KILL, (u64)pid, (u64)ZH_SIGCONT, 0u);
    g_jobs[slot - 1u].state = JOB_RUNNING;
    job_print_line(slot, "Running ", g_jobs[slot - 1u].name);
    puts1("       (SIGCONT sent, still in the background: shell does not wait)\n");
}

/*
 * 外部命令：fork -> exec -> (带作业记账的) wait。
 *
 * [ADD] M18 的改动只有三处：
 *   1) fork 之后先登记进作业表（这样 ^Z 停住它以后 jobs 能把它列出来）；
 *   2) waitpid 多传一个 WUNTRACED：被停住也算"有状态变化"，不会死等到底；
 *   3) 拿回 0x10000|sig 时说明它被 ^Z 停住了：打印 Stopped，进程留在内存里。
 */
static u64 run_command(u32 argc)
{
    u64 pid;
    u64 code;
    u32 slot;

    build_path(g_argv[0]);

    pid = sys_call(ZH_SYS_FORK, 0u, 0u, 0u);
    if (pid == 0u) {
        /* 子进程：argv[0] 保持用户敲的名字，exec 之后程序自己解析剩下的参数 */
        sys_call(ZH_SYS_EXEC, (u64)(unsigned long)g_path,
                 (u64)(unsigned long)g_argv, (u64)argc);
        /* exec 只会失败返回：最可能是文件不存在 */
        puts1("zhsh3: ");
        puts1(g_argv[0]);
        puts1(": cannot exec (no such program in /bin)\n");
        sys_call(ZH_SYS_EXIT, 127u, 0u, 0u);
    }
    if (pid == (u64)-1) {
        puts1("zhsh3: fork failed\n");
        return (u64)-1;
    }
    /*
     * [ADD] M17：把刚 fork 出来的子进程标成"前台"。
     * 内核在键盘中断里收到 Ctrl-C 时，只知道"有人按了 Ctrl-C"，
     * 不知道该送给谁 ， 这一句就是它的答案。
     * （Linux 用进程组 + 控制终端的 tcsetpgrp 做同一件事。）
     */
    slot = job_add((u32)pid, g_argv[0]);          /* [ADD] M18 */

    code = wait_foreground((u32)pid);             /* [ADD] M18：内含 SETFG / WAITPID / SETFG(-1) */

    /* [ADD] M18：^Z 停住 ， 退出码不是重点，重点是它还在内存里 */
    if ((code & ZH_WSTOPPED) != 0u && code != (u64)-1) {
        if (slot != 0u) {
            g_jobs[slot - 1u].state = JOB_STOPPED;
            g_jobs[slot - 1u].last_code = (u32)(code & 0xFFFFu);
            job_print_line(slot, "Stopped ", g_jobs[slot - 1u].name);
            puts1("       (SIGTSTP ");
            print_u64(code & 0xFFFFu);
            puts1(", stays in memory: try 'jobs', then 'fg ");
            print_u64((u64)slot);
            puts1("')\n");
        }
        return code;
    }

    if (code >= 128u && code != (u64)-1) {
        putc1('[');
        puts1(g_argv[0]);
        puts1(" killed by signal ");
        print_u64(code - 128u);
        putc1(' ');
        puts1(sig_name((int)(code - 128u)));
        puts1(", status ");
        print_u64(code);
        puts1("]\n");
        job_forget((u32)pid);
        return code;
    }
    if (code != 0u) {
        putc1('[');
        puts1(g_argv[0]);
        puts1(" exited ");
        print_u64(code);
        puts1("]\n");
    }
    job_forget((u32)pid);
    return code;
}

/* 提示符 */

static void prompt(void)
{
    u64 n = sys_call(ZH_SYS_GETCWD, (u64)(unsigned long)g_cwd,
                     (u64)(PATH_MAX + 1u), 0u);

    if (n == (u64)-1) {
        g_cwd[0] = '?';
        g_cwd[1] = '\0';
    }
    puts1("zh3:");
    puts1(g_cwd);
    puts1("$ ");
}

static void print_help(void)
{
    puts1("zhsh3 -- a shell running in ring 3 (argv[0]=");
    puts1(g_argv[0]);
    puts1(")\n");
    puts1("builtins (implemented by this process itself):\n");
    puts1("  help              this text\n");
    puts1("  exit              leave the ring3 shell, back to the kernel one\n");
    puts1("  echo <words>      print the words\n");
    puts1("  pwd               current directory (via SYS_GETCWD)\n");
    puts1("  cd <dir>          change directory (via SYS_CHDIR)\n");
    puts1("  jobs              list jobs (Running / Stopped, state via SYS_GETSTATE)\n");
    puts1("  fg <n>            SIGCONT + foreground + wait (like bash fg %n)\n");
    puts1("  bg <n>            SIGCONT and leave it running in the background\n");
    puts1("  ^C                SIGINT to the foreground job (default action: die)\n");
    puts1("  ^Z                SIGTSTP: the foreground job is stopped, then 'jobs'\n");
    puts1("everything else is an EXTERNAL program: fork -> exec -> wait:\n");
    puts1("  hello  argv  fd  wr  spawn  mm  reexec  forkdemo  execdemo ...\n");
    puts1("  (a bare name means /bin/<name>.elf; a leading / means that exact path)\n");
}

/* main */

void _start(unsigned long argc, char **argv)
{
    u64 me = sys_call(ZH_SYS_GETPID, 0u, 0u, 0u);

    (void)argc;
    (void)argv;

    puts1("\nzhsh3: I am a real ring-3 program (pid ");
    print_u64(me);
    puts1("), reading the keyboard one key at a time\n");
    puts1("zhsh3: every external command is fork() -> exec() -> wait(), like Linux\n");
    puts1("type 'help' for the builtins, 'exit' to go back\n\n");

    for (;;) {
        u32 n;

        prompt();
        read_line();
        n = split_args();
        if (n == 0u) {
            continue;
        }
        if (seq(g_argv[0], "exit")) {
            u64 code = 0u;               /* 无参 exit 一律退 0；exit 3 才退 3 */

            if (n >= 2u) {
                u32 k = 0u;
                while (g_argv[1][k] >= '0' && g_argv[1][k] <= '9') {
                    code = code * 10u + (u64)(g_argv[1][k] - '0');
                    ++k;
                }
            }
            puts1("zhsh3: exit (code ");
            print_u64(code);
            puts1(")\n");
            sys_call(ZH_SYS_EXIT, code, 0u, 0u);
        }
        if (seq(g_argv[0], "help")) {
            print_help();
            continue;
        }
        if (seq(g_argv[0], "echo")) {
            u32 i;
            for (i = 1u; i < n; ++i) {
                if (i > 1u) {
                    putc1(' ');
                }
                puts1(g_argv[i]);
            }
            putc1('\n');
            continue;
        }
        if (seq(g_argv[0], "pwd")) {
            u64 c = sys_call(ZH_SYS_GETCWD, (u64)(unsigned long)g_cwd,
                             (u64)(PATH_MAX + 1u), 0u);
            if (c == (u64)-1) {
                puts1("zhsh3: pwd failed\n");
            } else {
                puts1(g_cwd);
                putc1('\n');
            }
            continue;
        }
        if (seq(g_argv[0], "cd")) {
            if (n < 2u) {
                puts1("zhsh3: cd: missing directory\n");
                continue;
            }
            if (sys_call(ZH_SYS_CHDIR, (u64)(unsigned long)g_argv[1], 0u, 0u)
                == (u64)-1) {
                puts1("zhsh3: cd: ");
                puts1(g_argv[1]);
                puts1(": not a directory\n");
            }
            continue;
        }
        /* [ADD] M18：作业控制三个内置命令（都在 shell 自己的进程里实现） */
        if (seq(g_argv[0], "jobs")) {
            cmd_jobs();
            continue;
        }
        if (seq(g_argv[0], "fg")) {
            cmd_fg(n);
            continue;
        }
        if (seq(g_argv[0], "bg")) {
            cmd_bg(n);
            continue;
        }
        (void)run_command(n);
    }
}
