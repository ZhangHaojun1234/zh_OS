/* cmd_memmap.c - M3 的 memmap 命令：内存图 + 页帧分配器状态 + 自测 */

#include "cmd_memmap.h"

#include "shell.h"
#include "../mm/phys.h"

/*
 * memmap 与 meminfo 的分工
 *   meminfo : mm/heap.c 的"内核堆"视角 ， 2MB 静态池里 kmalloc 用了多少；
 *   memmap  : mm/phys.c 的"物理内存"视角 ， 机器一共有多少 RAM、哪些是
 *             BIOS 说的可用区、4KB 页帧被占了多少。
 * 前者是堆，后者是堆的上游：以后 kmalloc 想扩容，就得从这里要物理页。
 */

/* 本地输出工具（freestanding 无 libc）                               */

static void out(const char *s)
{
    sh_print(SH_PLAIN, s);
}

static void out_hex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char buf[20];
    char s[2] = { 0, 0 };

    buf[0] = '0';
    buf[1] = 'x';
    for (uint32_t i = 0u; i < 16u; ++i) {
        buf[2u + i] = digits[(value >> (60u - 4u * i)) & 0xFu];
    }
    buf[18] = '\0';

    for (uint32_t i = 0u; i < 18u; ++i) {
        s[0] = buf[i];
        sh_print(SH_OK, s);
    }
}

static void out_dec(uint64_t value)
{
    char buf[24];
    uint32_t n = 0u;
    char s[2] = { 0, 0 };

    if (value == 0ull) {
        buf[n++] = '0';
    } else {
        char tmp[24];
        uint32_t t = 0u;
        while (value != 0ull) {
            tmp[t++] = (char)('0' + (value % 10ull));
            value /= 10ull;
        }
        while (t != 0u) {
            buf[n++] = tmp[--t];
        }
    }
    buf[n] = '\0';

    for (uint32_t i = 0u; i < n; ++i) {
        s[0] = buf[i];
        sh_print(SH_OK, s);
    }
}

static void out_num(const char *label, uint64_t value)
{
    out(label);
    out_dec(value);
    sh_newline();
}

/* 十六进制 + 一个空格，用来凑固定列宽 */
static void out_hex_padded(uint64_t value)
{
    out_hex(value);
    out(" ");
}

static const char *type_name(uint32_t type)
{
    switch (type) {
    case PHYS_MEM_AVAILABLE:        return "available";
    case PHYS_MEM_RESERVED:         return "reserved ";
    case PHYS_MEM_ACPI_RECLAIMABLE: return "acpi-recl";
    case PHYS_MEM_NVS:              return "acpi-nvs ";
    case PHYS_MEM_BADRAM:           return "bad-ram  ";
    default:                        return "unknown  ";
    }
}

/* 命令实现                                                           */

static int arg_is_test(int argc, char **argv)
{
    const char *want = "test";
    const char *got;
    uint32_t i = 0u;

    if (argc < 2 || argv[1] == (char *)0) {
        return 0;
    }
    got = argv[1];
    while (want[i] != '\0' && got[i] != '\0') {
        char a = got[i];
        const char b = want[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (a != b) {
            return 0;
        }
        ++i;
    }
    return (want[i] == '\0' && got[i] == '\0');
}

static void show_map(void)
{
    const uint32_t count = phys_region_count();

    info("Physical memory map (Multiboot2 tag 6 / E820):");
    out("  base              length            type");
    sh_newline();
    for (uint32_t i = 0u; i < count; ++i) {
        const phys_region_t *r = phys_region_get(i);
        const uint64_t kb = (r->length + 1023ull) / 1024ull;

        out("  ");
        out_hex_padded(r->base);
        out_hex_padded(r->length);
        out(type_name(r->type));
        out("  (");
        if (r->length >= (1024ull * 1024ull)) {
            out_dec(kb / 1024ull);
            out(" MB)");
        } else {
            out_dec(kb);
            out(" KB)");
        }
        sh_newline();
    }
    if (count >= PHYS_MAX_REGIONS) {
        warn("  (region table full: later entries were dropped)");
    }
}

static void show_frames(void)
{
    const uint64_t total = phys_total_frames();
    const uint64_t used = phys_used_frame_count();
    const uint64_t free_frames = phys_free_frame_count();

    info("Frame allocator (bitmap, 1 bit = 1 frame = 4 KB):");
    out("  total RAM  : ");
    out_dec(phys_total_bytes() >> 20);
    out(" MB = ");
    out_dec(total);
    out(" frames");
    sh_newline();

    out_num("  used       : ", used);
    out_num("  free       : ", free_frames);
    out("  free bytes : ");
    out_dec((free_frames * PHYS_PAGE_SIZE) >> 20);
    out(" MB");
    sh_newline();

    out("  kernel img : ");
    out_hex_padded(phys_kernel_start());
    out("- ");
    out_hex(phys_kernel_end());
    sh_newline();
    out("  kmalloc pool (meminfo) lives inside the kernel image, so the");
    sh_newline();
    out("  frame allocator already counts it as used.");
    sh_newline();
}

static void memmap_self_test(void)
{
    info("phys self-test (memmap test):");

    const uint64_t before = phys_used_frame_count();
    const int rc = phys_self_test();
    const uint64_t after = phys_used_frame_count();

    out_num("  start used : ", before);
    out_num("  end used   : ", after);

    if (rc == 0) {
        ok("  PASS: alloc/free/contig/align/double-free all as expected");
        plain("  (the red lines above are the negative cases, rejected by");
        plain("   design: misaligned free and double free must not touch");
        plain("   the bitmap)");
    } else {
        err("  FAIL: phys_self_test rejected an invariant");
        out("  failed check #");
        out_dec((uint64_t)rc);
        sh_newline();
        out("  1=no frame 2=same frame twice 3=misaligned 4=not zeroed");
        sh_newline();
        out("  5=contig failed 6=count leak 7=no 2MB block 8=not 2MB aligned");
        sh_newline();
        out("  9=count leak after big free");
        sh_newline();
    }
}

int cmd_memmap(int argc, char **argv)
{
    if (arg_is_test(argc, argv)) {
        memmap_self_test();
        return SHELL_CONTINUE;
    }
    show_map();
    sh_newline();
    show_frames();
    return SHELL_CONTINUE;
}
