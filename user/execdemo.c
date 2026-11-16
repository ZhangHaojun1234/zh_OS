/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    user/execdemo.c
 * Version: 0.2beta
 * Brief:   M8 的演示程序：一个进程把自己"换掉"——exec("/bin/hello.elf")。
 *
 * 这个程序只做两件事：
 *   1) 打印自己的 pid；
 *   2) 调 exec("/bin/hello.elf")，从此这个进程里跑的就是 hello 了。
 *
 * exec 成功的话这一行永远执行不到（新程序从 e_entry 开始跑、旧代码被替换），
 * 所以屏幕上"hello from a real ELF64 binary"出现、并且 pid 没变，
 * 就同时证明了：按路径从 ramfs 打开 ELF、装载进去、原地换映像、pid 不变。
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

static char g_buf[128];

static void print(u64 total)
{
    sys_call(ZH_SYS_WRITE, (u64)(unsigned long)g_buf, total, 0u);
}

static void print_str(const char *s)
{
    u64 n = 0u;

    while (s[n] != '\0') {
        g_buf[n] = s[n];
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
        g_buf[0] = '0';
        print(1u);
        return;
    }
    while (value != 0u && digits < 20u) {
        tmp[digits++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    for (n = 0u; n < digits; ++n) {
        g_buf[n] = tmp[digits - 1u - n];
    }
    print(digits);
}

void _start(void)
{
    static const char path[] = "/bin/hello.elf";
    u64 me;
    u64 rc;

    me = sys_call(ZH_SYS_GETPID, 0u, 0u, 0u);
    print_str("exec demo: pid ");
    print_u64(me);
    print_str(" is about to exec /bin/hello.elf\n");
    print_str("           (this process image is about to be replaced)\n");

    rc = sys_call(ZH_SYS_EXEC, (u64)(unsigned long)path, 0u, 0u);

    /* 只有失败才会走到这里：成功的话 rip 已经被改到新程序的 e_entry。 */
    print_str("exec demo: exec failed, rc=");
    print_u64(rc);
    print_str("\n");
    sys_call(ZH_SYS_EXIT, 9u, 0u, 0u);

    for (;;) {
        __asm__ volatile("hlt");
    }
}
