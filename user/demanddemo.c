/* demanddemo.c - M16 按需分页的用户态证据。
 * 声明了一块 64KB 的 .bss（16 个 4KB 页），只碰第 5、10、15 页，故意跳着摸，也不碰 0 号页
 * （它和 .data 共用一页）。装载器一帧都不给，只在 desire[] 里登记 16 笔"将来是零"。
 * 于是装载时只付代码和数据的 1~2 页，跑起来只有被碰的三页真的出现，剩下 13 页从头到尾不占内存。
 * 注意读一页也算碰（读一样会 #PF），所以这里只读三页，遍历全部 16 页就等于把它们全买回来了。 */

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

/* 64KB 的 .bss：ELF 里 p_memsz 比 p_filesz 大 64KB，整块登记为按需页（16 页）。 */
#define AREA_PAGES 16u
static volatile unsigned char g_area[AREA_PAGES * 4096u];

/* 只碰这 3 页（跳着来，证明内核是一页一页地给，不是整段预装） */
#define PAGE_A 5u
#define PAGE_B 10u
#define PAGE_C 15u

void _start(unsigned long argc, char **argv)
{
    volatile unsigned char *pa = &g_area[PAGE_A * 4096u];
    volatile unsigned char *pb = &g_area[PAGE_B * 4096u];
    volatile unsigned char *pc = &g_area[PAGE_C * 4096u];

    (void)argc;
    (void)argv;

    puts1("[demand] 64 KB .bss = ");
    print_u64((u64)AREA_PAGES);
    puts1(" pages registered, 0 frames paid at load time.\n");

    /* 第 1 次读 0 号页：此刻它还是"洞"，读就会 #PF -> 内核给一帧零页 */
    puts1("[demand] read  page ");
    print_u64((u64)PAGE_A);
    puts1(" -> ");
    print_u64((u64)*pa);
    puts1("   (first touch: #PF -> a fresh zero page appeared)\n");

    puts1("[demand] read  page ");
    print_u64((u64)PAGE_B);
    puts1(" -> ");
    print_u64((u64)*pb);
    puts1("   (4 pages skipped: they are still holes, not RAM)\n");

    puts1("[demand] read  page ");
    print_u64((u64)PAGE_C);
    puts1(" -> ");
    print_u64((u64)*pc);
    puts1("   (another 4 pages skipped, and 1 above it is still a hole)\n");

    /* 写这三页（已是可写页），再读回来确认自己的写生效 */
    *pa = 0xABu;
    *pb = 0x11u;
    *pc = 0xCDu;
    puts1("[demand] wrote page ");
    print_u64((u64)PAGE_A);
    puts1("=");
    print_u64((u64)*pa);
    puts1("  page ");
    print_u64((u64)PAGE_B);
    puts1("=");
    print_u64((u64)*pb);
    puts1("  page ");
    print_u64((u64)PAGE_C);
    puts1("=");
    print_u64((u64)*pc);
    puts1("\n");

    puts1("[demand] 3 of ");
    print_u64((u64)AREA_PAGES);
    puts1(" pages materialised; the other 13 never took a frame.\n");
    sys_call(ZH_SYS_EXIT, 0u, 0u);
}
