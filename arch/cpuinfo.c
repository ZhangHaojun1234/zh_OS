/* cpuinfo.c - CPUID 封装和 cpuinfo 命令。特性位定义参考 Intel SDM 卷 2A 表 3-8
 * （叶 1 的 EDX/ECX），Family/Model/Stepping 的拆法看表 3-6（叶 1 EAX）。 */

#include "cpuinfo.h"

#include <stdint.h>

#include "vga.h"    /* vga_print / vga_println / enum vga_color */
#include "shell.h"  /* sh_print_u32 / SH_* / SHELL_CONTINUE */

/*
 * 内联汇编读 CPUID 叶。leaf/subleaf 是入参，EAX/EBX/ECX/EDX 出参。64 位下 RBX 受调用约定
 * 保护，GCC 的 =b 输出约束会自己备份恢复，不用像 32 位内核那样手工存 RBX。本内核按
 * -fno-pic -mcmodel=kernel 编译，EBX 不当 GOT 指针，可以直接拿来做输出操作数。
 */
static inline void cpuid(uint32_t leaf, uint32_t subleaf,
                         uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ __volatile__ (
        "cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(leaf), "c"(subleaf)
    );
}

/* 不链接 libc：自己把无符号 32 位整数转十进制并以指定颜色打印。 */
static void vga_print_u32(uint32_t value, enum vga_color color)
{
    char buf[12];
    int i = 0;

    if (value == 0u) {
        buf[i++] = '0';
    } else {
        char tmp[11];
        int t = 0;
        while (value != 0u) {
            tmp[t++] = (char)('0' + (value % 10u));
            value /= 10u;
        }
        while (t != 0) {
            buf[i++] = tmp[--t];
        }
    }
    buf[i] = '\0';
    vga_print(buf, color, VGA_BLACK);
}

/*
 * 打印一个特性名，按固定 8 字符宽度对齐：
 * 支持的特性亮绿，不支持的深灰。空格在黑底渲染，前景色无视觉影响。
 */
static void print_feature(const char *name, int supported)
{
    enum vga_color color = supported ? VGA_LIGHT_GREEN : VGA_DARK_GREY;
    int len = 0;

    vga_print(name, color, VGA_BLACK);
    while (name[len] != '\0') {
        ++len;
    }
    for (int i = len; i < 8; ++i) {
        vga_print(" ", VGA_BLACK, VGA_BLACK);
    }
}

/* 要检测的特性位（叶 1 EDX 或 ECX）。位定义见 Intel SDM 卷 2A 表 3-8。 */
typedef struct {
    const char *name;
    int        is_ecx;   /* 1=ECX，0=EDX */
    int        bit;      /* 位号 */
} cpu_feature_t;

static const cpu_feature_t g_features[] = {
    { "FPU",    0,  0 }, /* EDX bit 0  */
    { "MMX",    0, 23 }, /* EDX bit 23 */
    { "SSE",    0, 25 }, /* EDX bit 25 */
    { "SSE2",   0, 26 }, /* EDX bit 26 */
    { "SSE3",   1,  0 }, /* ECX bit 0  */
    { "SSSE3",  1,  9 }, /* ECX bit 9  */
    { "SSE4.1", 1, 19 }, /* ECX bit 19 */
    { "SSE4.2", 1, 20 }, /* ECX bit 20 */
    { "AVX",    1, 28 }, /* ECX bit 28 */
    { "AES",    1, 25 }, /* ECX bit 25 */
    { "64BIT",  0, 29 }, /* EDX bit 29 */
};

void cpuinfo_print(void)
{
    uint32_t a, b, c, d;

    /* 标题：亮青 */
    vga_println("=== CPU Information ===", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_println("", VGA_LIGHT_GREY, VGA_BLACK);

    /* Vendor ID：叶 0，EBX/EDX/ECX 各 4 字节按序拼成 12 字节字符串 */
    cpuid(0u, 0u, &a, &b, &c, &d);
    {
        char vendor[13];

        vendor[0]  = (char)(b & 0xFFu);
        vendor[1]  = (char)((b >> 8)  & 0xFFu);
        vendor[2]  = (char)((b >> 16) & 0xFFu);
        vendor[3]  = (char)((b >> 24) & 0xFFu);
        vendor[4]  = (char)(d & 0xFFu);
        vendor[5]  = (char)((d >> 8)  & 0xFFu);
        vendor[6]  = (char)((d >> 16) & 0xFFu);
        vendor[7]  = (char)((d >> 24) & 0xFFu);
        vendor[8]  = (char)(c & 0xFFu);
        vendor[9]  = (char)((c >> 8)  & 0xFFu);
        vendor[10] = (char)((c >> 16) & 0xFFu);
        vendor[11] = (char)((c >> 24) & 0xFFu);
        vendor[12] = '\0';

        vga_print("Vendor ID:   ", VGA_LIGHT_GREY, VGA_BLACK);
        vga_println(vendor, VGA_YELLOW, VGA_BLACK);
    }

    /* Brand String：扩展叶 0x80000002/03/04，各 16 字节拼 48 字节 */
    cpuid(0x80000000u, 0u, &a, &b, &c, &d);
    {
        int supported = (a >= 0x80000004u);

        vga_print("Brand String: ", VGA_LIGHT_GREY, VGA_BLACK);
        if (supported) {
            char brand[49];
            uint32_t regs[4];
            uint32_t leaves[3] = { 0x80000002u, 0x80000003u, 0x80000004u };
            int idx = 0;

            for (int li = 0; li < 3; ++li) {
                cpuid(leaves[li], 0u, &regs[0], &regs[1], &regs[2], &regs[3]);
                for (int r = 0; r < 4; ++r) {
                    uint32_t w = regs[r];
                    brand[idx++] = (char)(w & 0xFFu);
                    brand[idx++] = (char)((w >> 8)  & 0xFFu);
                    brand[idx++] = (char)((w >> 16) & 0xFFu);
                    brand[idx++] = (char)((w >> 24) & 0xFFu);
                }
            }
            brand[48] = '\0';
            /* 去掉尾部空格 */
            {
                int last = 47;
                while (last >= 0 && brand[last] == ' ') {
                    brand[last--] = '\0';
                }
            }
            vga_println(brand, VGA_LIGHT_GREY, VGA_BLACK);
        } else {
            vga_println("Brand string not supported", VGA_YELLOW, VGA_BLACK);
        }
    }

    /* Family / Model / Stepping：叶 1 EAX 按 Intel SDM 卷 2A 表 3-6 拆解 */
    cpuid(1u, 0u, &a, &b, &c, &d);
    {
        uint32_t stepping    = a & 0xFu;
        uint32_t base_model  = (a >> 4)  & 0xFu;
        uint32_t base_family = (a >> 8)  & 0xFu;
        uint32_t ext_model   = (a >> 16) & 0xFu;
        uint32_t ext_family  = (a >> 20) & 0xFFu;
        uint32_t family      = base_family;
        uint32_t model       = base_model;

        if (base_family == 0xFu) {
            family += ext_family;
        }
        if (base_family == 0x6u || base_family == 0xFu) {
            model += (ext_model << 4);
        }

        vga_print("Family ", VGA_LIGHT_GREY, VGA_BLACK);
        vga_print_u32(family, VGA_LIGHT_GREEN);
        vga_print("  Model ", VGA_LIGHT_GREY, VGA_BLACK);
        vga_print_u32(model, VGA_LIGHT_GREEN);
        vga_print("  Stepping ", VGA_LIGHT_GREY, VGA_BLACK);
        vga_print_u32(stepping, VGA_LIGHT_GREEN);
        vga_println("", VGA_LIGHT_GREY, VGA_BLACK);
    }

    /* Features：叶 1 EDX/ECX，支持的亮绿、不支持的深灰，每行 4 个对齐 */
    vga_print("Features:    ", VGA_LIGHT_GREY, VGA_BLACK);
    {
        int count = 0;

        for (int i = 0; i < 11; ++i) {
            uint32_t reg = g_features[i].is_ecx ? c : d;
            int supported = (int)((reg >> g_features[i].bit) & 1u);

            print_feature(g_features[i].name, supported);
            ++count;
            if (count % 4 == 0) {
                vga_println("", VGA_LIGHT_GREY, VGA_BLACK);
                vga_print("             ", VGA_LIGHT_GREY, VGA_BLACK);
            }
        }
        if (count % 4 != 0) {
            vga_println("", VGA_LIGHT_GREY, VGA_BLACK);
        }
    }

    /* Logical processors：叶 1 EBX[23:16]，为 0 时显示 1 */
    {
        uint32_t lp = (b >> 16) & 0xFFu;

        if (lp == 0u) {
            lp = 1u;
        }
        vga_print("Logical processors: ", VGA_LIGHT_GREY, VGA_BLACK);
        vga_print_u32(lp, VGA_LIGHT_CYAN);
        vga_println("", VGA_LIGHT_GREY, VGA_BLACK);
    }
}

int cmd_cpuinfo(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    cpuinfo_print();
    return SHELL_CONTINUE;
}
