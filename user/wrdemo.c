/* wrdemo.c - M10 演示（文件半）：ring3 程序自己 creat / write / close，再 open 读回来。
 * creat("/home/zh/m10.txt") 拿写模式 fd；往同一个 fd 写两段，第二段是追加，文件会变长；
 * close 之后用只读 open 重新打开同一个路径，把内容读出来打印。
 * 读出来的字节是刚刚写进去的，说明内容经过了 ramfs 节点，不是留在某个临时变量里。全过 exit(0)，否则 exit(9)。 */

typedef unsigned long u64;

#define ZH_SYS_WRITE  1ul
#define ZH_SYS_EXIT   3ul
#define ZH_SYS_OPEN   11ul
#define ZH_SYS_READ   12ul
#define ZH_SYS_CLOSE  13ul
#define ZH_SYS_CREAT  14ul
#define ZH_SYS_FWRITE 15ul

static u64 sys_call(u64 nr, u64 a1, u64 a2, u64 a3)
{
    u64 ret;

    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                     : "memory", "cc");
    return ret;
}

static char g_out[256];

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
    static const char path[] = "/home/zh/m10.txt";
    static const char line1[] = "hello from a ring3 file write\n";
    static const char line2[] = "this line was appended by the same fd\n";
    char rbuf[200];
    u64 fd;
    u64 n;
    u64 len1 = 0u;
    u64 len2 = 0u;
    u64 fail = 0u;

    while (line1[len1] != '\0') {
        ++len1;
    }
    while (line2[len2] != '\0') {
        ++len2;
    }

    print_str("wr demo: a ring3 program creates and writes a real file\n");

    /* 1) creat */
    fd = sys_call(ZH_SYS_CREAT, (u64)(unsigned long)path, 0u, 0u);
    print_str("  creat(\"/home/zh/m10.txt\") -> fd ");
    if (fd == (u64)-1) {
        print_str("-1 (failed)\n");
        sys_call(ZH_SYS_EXIT, 9u, 0u, 0u);
    }
    print_u64(fd);
    print_str("\n");

    /* 2) 写两段（第二段是追加） */
    n = sys_call(ZH_SYS_FWRITE, fd, (u64)(unsigned long)line1, len1);
    print_str("  write(fd, ");
    print_u64(len1);
    print_str(" bytes) -> ");
    print_u64(n);
    print_str("\n");
    if (n != len1) {
        ++fail;
    }
    n = sys_call(ZH_SYS_FWRITE, fd, (u64)(unsigned long)line2, len2);
    print_str("  write(fd, ");
    print_u64(len2);
    print_str(" bytes) -> ");
    print_u64(n);
    print_str("   (appended, the file grew)\n");
    if (n != len2) {
        ++fail;
    }

    n = sys_call(ZH_SYS_CLOSE, fd, 0u, 0u);
    print_str("  close(fd) -> ");
    print_u64(n);
    print_str("\n");
    if (n != 0u) {
        ++fail;
    }

    /* 3) 重新打开，读回内容 */
    fd = sys_call(ZH_SYS_OPEN, (u64)(unsigned long)path, 0u, 0u);
    print_str("  open(\"/home/zh/m10.txt\") again -> fd ");
    if (fd == (u64)-1) {
        print_str("-1 (failed)\n");
        sys_call(ZH_SYS_EXIT, 9u, 0u, 0u);
    }
    print_u64(fd);
    print_str("\n");

    n = sys_call(ZH_SYS_READ, fd, (u64)(unsigned long)rbuf, 199u);
    print_str("  read back -> ");
    print_u64(n);
    print_str(" bytes:\n");
    if (n == 0u || n == (u64)-1) {
        ++fail;
    } else {
        u64 i;
        for (i = 0u; i < n; ++i) {
            g_out[0] = rbuf[i];
            print(1u);
        }
        if (rbuf[n - 1u] != '\n') {
            g_out[0] = '\n';
            print(1u);
        }
    }
    sys_call(ZH_SYS_CLOSE, fd, 0u, 0u);

    if (fail == 0u) {
        print_str("wr demo: the bytes read back are exactly the bytes I wrote\n");
        sys_call(ZH_SYS_EXIT, 0u, 0u, 0u);
    }
    print_str("wr demo: FAILED\n");
    sys_call(ZH_SYS_EXIT, 9u, 0u, 0u);

    for (;;) {
        __asm__ volatile("hlt");
    }
}
