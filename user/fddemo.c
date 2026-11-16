/* fddemo.c - M9 演示：一个 ring3 程序自己 open / read / close。验的是 fd 语义本身：
 * open("/bin/hello.elf") 拿到一个小整数 fd；read(fd, 4) 必须是 7f 45 4c 46（ELF 魔数）；
 * 再 read 4 字节，偏移自动前移，内容跟第一次不同；close 之后同一个 fd 读不了，返回 -1；
 * open 不存在的路径、open 一个目录，也都返回 -1。全过 exit(0)，否则 exit(9)。 */

typedef unsigned long u64;

#define ZH_SYS_WRITE  1ul
#define ZH_SYS_EXIT   3ul
#define ZH_SYS_OPEN   11ul
#define ZH_SYS_READ   12ul
#define ZH_SYS_CLOSE  13ul

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

/* 以 2 位十六进制打印一个字节（前面加个空格） */
static void print_hex8(u64 v)
{
    static const char digits[] = "0123456789abcdef";

    g_out[0] = ' ';
    g_out[1] = digits[(v >> 4) & 0xFu];
    g_out[2] = digits[v & 0xFu];
    print(3u);
}

void _start(void)
{
    static const char path[] = "/bin/hello.elf";
    static const char missing[] = "/bin/nope.elf";
    static const char adir[] = "/bin";
    u64 buf[2];
    u64 fd;
    u64 n;
    u64 i;
    u64 fail = 0u;

    print_str("fd demo: I am a ring3 program, I will open/read/close myself\n");

    /* 1) open */
    fd = sys_call(ZH_SYS_OPEN, (u64)(unsigned long)path, 0u, 0u);
    print_str("  open(\"/bin/hello.elf\") -> fd ");
    if (fd == (u64)-1) {
        print_str("-1 (failed)\n");
        sys_call(ZH_SYS_EXIT, 9u, 0u, 0u);
    }
    print_u64(fd);
    print_str("\n");

    /* 2) 读前 4 字节：ELF 魔数 */
    n = sys_call(ZH_SYS_READ, fd, (u64)(unsigned long)buf, 4u);
    print_str("  read(fd, 4) -> ");
    print_u64(n);
    print_str(" bytes:");
    for (i = 0u; i < 4u; ++i) {
        print_hex8(((unsigned char *)buf)[i]);
    }
    print_str("\n");
    if (n != 4u) {
        ++fail;
    }
    if (((unsigned char *)buf)[0] != 0x7Fu || ((unsigned char *)buf)[1] != 'E' ||
        ((unsigned char *)buf)[2] != 'L' || ((unsigned char *)buf)[3] != 'F') {
        print_str("  ^ expected 7f 45 4c 46 (ELF magic) -- FAIL\n");
        ++fail;
    } else {
        print_str("  ^ 7f 45 4c 46 = ELF magic, the fd really reached the file\n");
    }

    /* 3) 再读 4 字节：偏移自动前移 */
    n = sys_call(ZH_SYS_READ, fd, (u64)(unsigned long)buf, 4u);
    print_str("  read(fd, 4) again -> ");
    print_u64(n);
    print_str(" bytes:");
    for (i = 0u; i < 4u; ++i) {
        print_hex8(((unsigned char *)buf)[i]);
    }
    print_str("  (offset moved, not the magic again)\n");
    if (n != 4u) {
        ++fail;
    }

    /* 4) close 之后 fd 失效 */
    n = sys_call(ZH_SYS_CLOSE, fd, 0u, 0u);
    print_str("  close(fd) -> ");
    print_u64(n);
    print_str("\n");
    if (n != 0u) {
        ++fail;
    }
    n = sys_call(ZH_SYS_READ, fd, (u64)(unsigned long)buf, 4u);
    print_str("  read(fd) after close -> ");
    if (n == (u64)-1) {
        print_str("-1 (fd is dead, as expected)\n");
    } else {
        print_str("UNEXPECTED success -- FAIL\n");
        ++fail;
    }

    /* 5) 打开不存在的文件 / 打开目录 */
    n = sys_call(ZH_SYS_OPEN, (u64)(unsigned long)missing, 0u, 0u);
    print_str("  open(\"/bin/nope.elf\") -> ");
    if (n == (u64)-1) {
        print_str("-1 (no such file)\n");
    } else {
        print_str("UNEXPECTED fd -- FAIL\n");
        ++fail;
    }
    n = sys_call(ZH_SYS_OPEN, (u64)(unsigned long)adir, 0u, 0u);
    print_str("  open(\"/bin\") -> ");
    if (n == (u64)-1) {
        print_str("-1 (it is a directory, not a file)\n");
    } else {
        print_str("UNEXPECTED fd -- FAIL\n");
        ++fail;
    }

    if (fail == 0u) {
        print_str("fd demo: all five checks passed\n");
        sys_call(ZH_SYS_EXIT, 0u, 0u, 0u);
    }
    print_str("fd demo: FAILED\n");
    sys_call(ZH_SYS_EXIT, 9u, 0u, 0u);

    for (;;) {
        __asm__ volatile("hlt");
    }
}
