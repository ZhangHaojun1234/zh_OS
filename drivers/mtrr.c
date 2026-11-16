/* ============================================================================
 * zh_OS 1.0 (x86_64)
 * File:    drivers/mtrr.c
 * Version: 1.0
 * Brief:   MTRR 支持 —— 把帧缓冲那段物理内存设成 write-combining（WC）。
 *
 * 为什么需要它（M34 真机教训）：
 *   光把页表 PAT 位设成 WC 是不够的。x86 的内存类型是"MTRR 与 PAT 取最严格"：
 *   固件通常给 MMIO 空洞留一条 UC 的 variable MTRR，只要这条 MTRR 压在上面，
 *   页表里写 PAT=WC 也照样按 UC 跑，表现就是"每写一个像素等一次总线"。
 *   所以必须自己拿一条 MTRR 显式把这段标成 WC（必要时先清掉整段落在帧缓冲
 *   范围内的 UC 项）。
 * ============================================================================ */

#include <stdint.h>
#include "mtrr.h"
#include "vga.h"

#define MSR_MTRRCAP        0x000000FEu
#define MSR_MTRR_DEF_TYPE  0x000002FFu
#define MSR_PHYBASE0       0x00000200u
#define MSR_PHYMASK0       0x00000201u

#define MTRR_DEF_E         (1ull << 11)   /* MTRRdefType 的 E 位 */
#define MTRR_VALID         (1ull << 11)   /* physmask 的 V 位     */
#define MTRR_TYPE_MASK     0xFFull
#define PHYS_MASK_BITS     0x000FFFFFFFFFF000ull
#define MTRR_TYPE_UC       0ull
#define MTRR_TYPE_WC       1ull
/* mask 里只有 bit 51:12 参与；算大小必须先把 bit 63:52 的 1 抹掉，
 * 否则取反出来的全是 1，会得到一个天文数字 */
#define PHYS_LOW_BITS      0x000FFFFFFFFFFFFFull

static uint64_t mtrr_entry_size(uint64_t mask)
{
    /* mask 的 bit 51:12 里，0 表示"大小"位，1 表示"必须匹配"位。
     * 从 bit12 往上数连续多少个 0 位（也就是取反后连续的 1 位），
     * 数到 k 个，大小就是 2^(12+k)。注意 bit11 是 V 位，不能去反了算进去。 */
    uint64_t t = (~mask) & PHYS_MASK_BITS;
    uint32_t n = 12u;

    while (n < 52u && (t & (1ull << n)) != 0ull) {
        ++n;
    }
    return 1ull << n;
}

/* 小工具：MSR 读写 */
static uint64_t rdmsr64(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static void wrmsr64(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr" : : "a"((uint32_t)(value & 0xFFFFFFFFu)),
                                "d"((uint32_t)(value >> 32)), "c"(msr));
}

static void wbinvd_all(void)
{
    __asm__ volatile("wbinvd" : : : "memory");
}

/* ---- MTRR 布局小工具 ---- */

static uint32_t free_slots(uint32_t vcnt)
{
    uint32_t i, n = 0u;

    for (i = 0u; i < vcnt; ++i) {
        if ((rdmsr64(MSR_PHYMASK0 + 2u * i) & MTRR_VALID) == 0ull) {
            ++n;
        }
    }
    return n;
}

/* 从 cur 开始、不超过 end 的最大对齐块（2 的幂，>=4KB） */
static uint64_t pick_block(uint64_t cur, uint64_t end)
{
    uint64_t probe;

    if (end <= cur) {
        return 0ull;
    }
    for (probe = 0x40000000ull; probe >= 0x1000ull; probe >>= 1) {
        if ((cur & (probe - 1ull)) == 0ull && probe <= (end - cur)) {
            return probe;
        }
    }
    return 0ull;
}

static uint32_t blocks_needed(uint64_t s, uint64_t e)
{
    uint32_t n = 0u;
    uint64_t cur = s;

    while (cur < e) {
        uint64_t sz = pick_block(cur, e);
        if (sz == 0ull) {
            break;
        }
        ++n;
        cur += sz;
    }
    return n;
}

/* 用空闲槽把 [s,e) 覆盖成 type；返回实际覆盖的字节数 */
static uint64_t cover_range(uint32_t vcnt, uint64_t s, uint64_t e, uint64_t type)
{
    uint64_t cur = s, covered = 0ull;

    while (cur < e) {
        uint64_t sz = pick_block(cur, e);
        uint32_t slot = 0xFFFFFFFFu;
        uint32_t i;

        if (sz == 0ull) {
            break;
        }
        for (i = 0u; i < vcnt; ++i) {
            if ((rdmsr64(MSR_PHYMASK0 + 2u * i) & MTRR_VALID) == 0ull) {
                slot = i;
                break;
            }
        }
        if (slot == 0xFFFFFFFFu) {
            break;
        }
        wrmsr64(MSR_PHYBASE0 + 2u * slot, cur | type);
        wrmsr64(MSR_PHYMASK0 + 2u * slot,
                (~(sz - 1ull) & PHYS_MASK_BITS) | MTRR_VALID);
        cur += sz;
        covered += sz;
    }
    return covered;
}

/* ---- 打印小工具（只用到 vga_print / vga_putchar，跟其它驱动一致） ---- */
static void pr(const char *s)
{
    vga_print(s, VGA_LIGHT_CYAN, VGA_BLACK);
}

static void pr_udec(uint64_t v)
{
    char buf[24];
    int n = 0;

    if (v == 0ull) {
        vga_putchar('0', VGA_LIGHT_CYAN, VGA_BLACK);
        return;
    }
    while (v > 0ull && n < 20) {
        buf[n++] = (char)('0' + (int)(v % 10ull));
        v /= 10ull;
    }
    while (n > 0) {
        vga_putchar(buf[--n], VGA_LIGHT_CYAN, VGA_BLACK);
    }
}

static void pr_hex(uint64_t v)
{
    int shift;
    int started = 0;

    pr("0x");
    for (shift = 60; shift >= 0; shift -= 4) {
        uint32_t d = (uint32_t)((v >> shift) & 0xFu);
        if (d != 0u || shift == 0) {
            started = 1;
        }
        if (started) {
            vga_putchar((char)(d < 10u ? ('0' + (int)d) : ('a' + (int)(d - 10u))),
                        VGA_LIGHT_CYAN, VGA_BLACK);
        }
    }
}

static const char *type_name(uint64_t t)
{
    switch (t) {
    case 0ull: return "UC";
    case 1ull: return "WC";
    case 4ull: return "WT";
    case 5ull: return "WP";
    case 6ull: return "WB";
    default:   return "??";
    }
}

/* 打印 MTRRdefType 与全部 variable MTRR */
void mtrr_dump(void)
{
    uint64_t cap = rdmsr64(MSR_MTRRCAP);
    uint64_t def = rdmsr64(MSR_MTRR_DEF_TYPE);
    uint32_t vcnt = (uint32_t)(cap & 0xFFull);
    uint32_t i;

    pr("MTRRCAP vcnt=");
    pr_udec((uint64_t)vcnt);
    pr("  defType=");
    pr_hex(def);
    pr("  E=");
    pr_udec(((def & MTRR_DEF_E) != 0ull) ? 1ull : 0ull);
    pr("  default=");
    pr(type_name(def & MTRR_TYPE_MASK));
    vga_putchar('\n', VGA_LIGHT_CYAN, VGA_BLACK);

    for (i = 0u; i < vcnt; ++i) {
        uint64_t b = rdmsr64(MSR_PHYBASE0 + 2u * i);
        uint64_t m = rdmsr64(MSR_PHYMASK0 + 2u * i);
        uint64_t s = b & PHYS_MASK_BITS;

        pr("  MTRR");
        pr_udec((uint64_t)i);
        if ((m & MTRR_VALID) == 0ull) {
            pr("  free\n");
            continue;
        }
        pr("  base=");
        pr_hex(s);
        pr(" size=");
        pr_udec(mtrr_entry_size(m));
        pr(" type=");
        pr(type_name(b & MTRR_TYPE_MASK));
        vga_putchar('\n', VGA_LIGHT_CYAN, VGA_BLACK);
    }
}

/* 想用一条 MTRR 盖住 [base, base+bytes)：找一个 base 对齐、且大小不小于 bytes 的
 * 2 的幂，多盖出来的部分不超过 max_over 就用它（盖进显卡 aperture 里没关系）。 */
static uint64_t wc_super_size(uint64_t base, uint64_t bytes, uint64_t max_over)
{
    uint64_t probe;

    for (probe = 0x1000ull; probe <= 0x80000000ull; probe <<= 1) {
        if (probe >= bytes && (base & (probe - 1ull)) == 0ull &&
            (probe - bytes) <= max_over) {
            return probe;
        }
    }
    return 0ull;
}

/* 把 [base, base+bytes) 覆盖成 WC；返回被覆盖的字节数 */
uint64_t mtrr_wc_range(uint64_t base, uint64_t bytes)
{
    uint64_t cap, def, end, covered = 0ull;
    uint64_t super_sz, plan_end;
    uint32_t vcnt, i, wc_blocks;

    if (bytes == 0ull) {
        return 0ull;
    }
    cap = rdmsr64(MSR_MTRRCAP);
    vcnt = (uint32_t)(cap & 0xFFull);
    if (vcnt == 0u) {
        return 0ull;
    }
    def = rdmsr64(MSR_MTRR_DEF_TYPE);
    end = base + bytes;

    /* 先挑覆盖方案：优先"一条对齐的大 MTRR"，多盖 <=16MB 就接受 ——
     * 帧缓冲基本都落在显卡 aperture 里，多标的这一点也是显存，不会伤到设备寄存器。
     * 一条盖不住就退回逐块拼。 */
    super_sz = wc_super_size(base, bytes, 16ull * 1024ull * 1024ull);
    if (super_sz != 0ull) {
        plan_end = base + super_sz;
        wc_blocks = 1u;
    } else {
        plan_end = end;
        wc_blocks = blocks_needed(base, end);
    }
    if (wc_blocks == 0u) {
        return 0ull;
    }

    /* 第二步：处理跟帧缓冲重叠的固件 UC 项。
     * 为什么：x86 的内存类型是"MTRR 里最严格的那条胜出"，固件给 MMIO 留的 UC
     * 项只要压在这段上，页表的 PAT=WC 就完全失效。
     * 做法：把那条腾出来，先把它落在帧缓冲外面的部分用 UC 补回去（顺序不能反：
     * 得先保证别的 MMIO 仍然不可缓存），最后再谈 WC 加速。
     * 槽不够就整条不动 —— 宁可慢，也不拿错误的类型罩住别的 MMIO。 */
    for (i = 0u; i < vcnt; ++i) {
        uint64_t b = rdmsr64(MSR_PHYBASE0 + 2u * i);
        uint64_t m = rdmsr64(MSR_PHYMASK0 + 2u * i);
        uint64_t s, sz, e2, need;

        if ((m & MTRR_VALID) == 0ull) {
            continue;
        }
        if ((b & MTRR_TYPE_MASK) != MTRR_TYPE_UC) {
            continue;
        }
        s = b & PHYS_MASK_BITS;
        sz = mtrr_entry_size(m);
        e2 = s + sz;
        if (e2 <= base || s >= end) {
            continue;                       /* 跟帧缓冲不重叠 */
        }
        need = 0u;
        if (s < base) {
            need += blocks_needed(s, base);
        }
        if (e2 > plan_end) {
            need += blocks_needed(plan_end, e2);
        }
        if (free_slots(vcnt) + 1u < need + wc_blocks) {
            continue;                       /* 腾出来也补不回去，跳过 */
        }
        wrmsr64(MSR_PHYBASE0 + 2u * i, 0ull);
        wrmsr64(MSR_PHYMASK0 + 2u * i, 0ull);
        if (s < base) {
            (void)cover_range(vcnt, s, base, MTRR_TYPE_UC);
        }
        if (e2 > plan_end) {
            (void)cover_range(vcnt, plan_end, e2, MTRR_TYPE_UC);
        }
    }

    if (super_sz != 0ull) {
        for (i = 0u; i < vcnt; ++i) {
            if ((rdmsr64(MSR_PHYMASK0 + 2u * i) & MTRR_VALID) == 0ull) {
                wrmsr64(MSR_PHYBASE0 + 2u * i, base | MTRR_TYPE_WC);
                wrmsr64(MSR_PHYMASK0 + 2u * i,
                        (~(super_sz - 1ull) & PHYS_MASK_BITS) | MTRR_VALID);
                covered = bytes;
                break;
            }
        }
    } else {
        covered = cover_range(vcnt, base, end, MTRR_TYPE_WC);
    }

    if (covered != 0ull) {
        if ((def & MTRR_DEF_E) == 0ull) {
            def |= MTRR_DEF_E;
            wrmsr64(MSR_MTRR_DEF_TYPE, def);
        }
        wbinvd_all();
    } else if ((def & MTRR_DEF_E) == 0ull) {
        /* 没有 MTRR 生效时，PAT 的 WC 也不会起作用 */
        def |= MTRR_DEF_E;
        wrmsr64(MSR_MTRR_DEF_TYPE, def);
        wbinvd_all();
    }
    return covered;
}
