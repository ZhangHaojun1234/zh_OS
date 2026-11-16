/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    mm/phys.c
 * Version: 0.2beta
 * Brief:   M3 物理内存管理：解析 Multiboot2 内存图，建 4KB 页帧位图分配器。
 * ============================================================================ */

#include "phys.h"

#include "../shell/shell.h"
#include "klog.h"

/*
 * 设计说明
 * --------
 * 位图口径：1 bit = 1 帧 = 4KB。bit = 1 表示"被占用"，bit = 0 表示"空闲"。
 *   容量上限固定为 4GB（位图 128KB，放在 .bss 里，跟内核映像一起被 GRUB 清零）。
 *   QEMU -m 128 只用到 32768 个 bit，剩下的位图空间不会浪费物理内存。
 *
 * 初始化顺序（照 BIOS/E820 的标准做法，先保守再放开）：
 *   1) 整张位图标记为"全部占用"  —— 任何没被证明可用的地址都默认不许碰；
 *   2) 遍历内存图，把 type=1(available) 的区域标记为"空闲"；
 *   3) 再把必须自己留着的地方标记回"占用"：
 *        低 1MB（IVT / BIOS / EBDA / VGA 显存 0xB8000 都在里面）
 *        内核映像 [_kernel_start, _kernel_end)
 *        位图自身（本就在 .bss，已被内核映像覆盖，这里再显式标一次更稳）
 *        Multiboot2 信息结构（GRUB 放在低内存，读完就没用了，但先别给出去）
 *
 * 这样"用"和"不用"只由第 1、2 步决定，第 3 步只是收回保留区，
 * 计数最后统一重扫一遍位图得到，不需要在分配过程中维护计数器。
 */

#define PHYS_BITMAP_BYTES_LIMIT  (4ull * 1024ull * 1024ull * 1024ull)          /* 4GB */
#define PHYS_FRAME_LIMIT         (PHYS_BITMAP_BYTES_LIMIT / PHYS_PAGE_SIZE)    /* 1048576 帧 */
#define PHYS_BITMAP_WORDS        (PHYS_FRAME_LIMIT / 64ull)                    /* 16384 个 u64 */

#define MB2_BOOT_MAGIC   0x36D76289u
#define MB2_TAG_END      0u
#define MB2_TAG_MEMINFO  4u
#define MB2_TAG_MMAP     6u

/* 低 1MB 一律保留：里面是 IVT、BIOS 数据区、EBDA、VGA 文本缓冲 0xB8000。 */
#define LOW_RESERVED_BYTES (1024ull * 1024ull)

/* ------------------------------------------------------------------ */
/* Multiboot2 结构（只取用得到的字段，按规范手工展开）                */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t total_size;    /* 整个 info 结构的总长度（含对齐填充） */
    uint32_t reserved;
} mb2_info_t;

typedef struct {
    uint32_t type;
    uint32_t size;
} mb2_tag_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
} mb2_tag_mmap_t;

typedef struct {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;
} mb2_mmap_entry_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t mem_lower;
    uint32_t mem_upper;
} mb2_tag_meminfo_t;

/* ------------------------------------------------------------------ */
/* 状态                                                               */
/* ------------------------------------------------------------------ */

static uint64_t g_bitmap[PHYS_BITMAP_WORDS];

static phys_region_t g_regions[PHYS_MAX_REGIONS];
static uint32_t g_region_count;

static uint64_t g_total_frames;      /* 系统里"值得管理"的帧总数 */
static uint64_t g_used_frames;       /* 占用帧数（初始化末尾统一重扫） */
static uint64_t g_kernel_start;
static uint64_t g_kernel_end;
static int g_ready;

/* 链接脚本里定义的符号：内核映像的起止物理地址 */
extern uint8_t _kernel_start[];
extern uint8_t _kernel_end[];

/* ------------------------------------------------------------------ */
/* 位图操作                                                           */
/* ------------------------------------------------------------------ */

static void bitmap_set_used(uint64_t frame)
{
    g_bitmap[frame >> 6] |= (1ull << (frame & 63ull));
}

static void bitmap_set_free(uint64_t frame)
{
    g_bitmap[frame >> 6] &= ~(1ull << (frame & 63ull));
}

static int bitmap_is_free(uint64_t frame)
{
    return (g_bitmap[frame >> 6] & (1ull << (frame & 63ull))) == 0ull;
}

static void zero_frames(uint64_t base, uint64_t count)
{
    uint8_t *p = (uint8_t *)(uintptr_t)base;
    const uint64_t bytes = count * PHYS_PAGE_SIZE;

    for (uint64_t i = 0ull; i < bytes; ++i) {
        p[i] = 0u;
    }
}

/* ------------------------------------------------------------------ */
/* 打印辅助（freestanding 没有 printf，自己拼）                       */
/* ------------------------------------------------------------------ */

static void print_dec(uint64_t value)
{
    char buf[24];
    uint32_t n = 0u;
    uint32_t i;

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

    for (i = 0u; i < n; ++i) {
        char c = buf[i];
        char s[2];
        s[0] = c;
        s[1] = '\0';
        sh_print(SH_PLAIN, s);
    }
}

static void print_hex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char buf[19];
    char s[2] = { 0, 0 };

    buf[0] = '0';
    buf[1] = 'x';
    for (uint32_t i = 0u; i < 16u; ++i) {
        buf[2u + i] = digits[(value >> (60u - 4u * i)) & 0xFu];
    }
    buf[18] = '\0';

    for (uint32_t i = 0u; i < 18u; ++i) {
        s[0] = buf[i];
        sh_print(SH_PLAIN, s);
    }
}

/* ------------------------------------------------------------------ */
/* 位图标记范围                                                       */
/* ------------------------------------------------------------------ */

static void free_phys_range(uint64_t base, uint64_t length)
{
    if (length == 0ull) {
        return;
    }
    /* 起点向上取整到帧边界，终点向下取整：只放出完整落在区域内的帧。 */
    uint64_t start = (base + PHYS_PAGE_SIZE - 1ull) / PHYS_PAGE_SIZE;
    uint64_t end = (base + length) / PHYS_PAGE_SIZE;

    if (end > g_total_frames) {
        end = g_total_frames;
    }
    for (uint64_t f = start; f < end; ++f) {
        bitmap_set_free(f);
    }
}

static void reserve_phys_range(uint64_t base, uint64_t length)
{
    if (length == 0ull) {
        return;
    }
    uint64_t start = base / PHYS_PAGE_SIZE;
    uint64_t end = (base + length + PHYS_PAGE_SIZE - 1ull) / PHYS_PAGE_SIZE;

    if (end > g_total_frames) {
        end = g_total_frames;
    }
    for (uint64_t f = start; f < end; ++f) {
        bitmap_set_used(f);
    }
}

static void recount_used(void)
{
    uint64_t used = 0ull;

    for (uint64_t f = 0ull; f < g_total_frames; ++f) {
        if (!bitmap_is_free(f)) {
            ++used;
        }
    }
    g_used_frames = used;
}

/* ------------------------------------------------------------------ */
/* 内存图采集                                                         */
/* ------------------------------------------------------------------ */

static void push_region(uint64_t base, uint64_t length, uint32_t type)
{
    if (g_region_count >= PHYS_MAX_REGIONS) {
        return;   /* 条目上限：真实机器 24 条通常够用，多出来的忽略 */
    }
    g_regions[g_region_count].base = base;
    g_regions[g_region_count].length = length;
    g_regions[g_region_count].type = type;
    ++g_region_count;
}

/* 解析 tag 6（内存图）；返回 1 表示至少拿到一条，0 表示没有 */
static int parse_mmap_tag(const mb2_tag_mmap_t *tag)
{
    const uint32_t entry_size = tag->entry_size;
    uint64_t offset = sizeof(mb2_tag_mmap_t);

    if (entry_size < sizeof(mb2_mmap_entry_t)) {
        return 0;   /* 条目比规范要求的小，没法安全步进 */
    }
    while ((offset + entry_size) <= (uint64_t)tag->size) {
        const mb2_mmap_entry_t *e =
            (const mb2_mmap_entry_t *)(uintptr_t)((uintptr_t)tag + offset);

        push_region(e->addr, e->len, e->type);
        offset += entry_size;
    }
    return 1;
}

/* tag 4 只是"低端/高端内存各多少 KB"的粗略信息，内存图缺失时兜底用 */
static int parse_meminfo_tag(const mb2_tag_meminfo_t *tag)
{
    push_region(0ull, (uint64_t)tag->mem_lower * 1024ull, PHYS_MEM_AVAILABLE);
    push_region(1024ull * 1024ull, (uint64_t)tag->mem_upper * 1024ull,
                PHYS_MEM_AVAILABLE);
    return 1;
}

static int collect_regions(uint32_t mb2_info)
{
    const mb2_info_t *info = (const mb2_info_t *)(uintptr_t)mb2_info;
    uint64_t offset;
    int have_mmap = 0;
    uint32_t meminfo_addr = 0u;

    if (info == (const mb2_info_t *)0) {
        return -2;
    }
    /* 总长先做一次合理性检查：串了就说明 magic 或指针有问题 */
    if (info->total_size < 16u || info->total_size > (16u * 1024u * 1024u)) {
        return -3;
    }

    offset = 8ull;   /* 头部 8 字节之后是第一个 tag */
    while ((offset + sizeof(mb2_tag_t)) <= (uint64_t)info->total_size) {
        const mb2_tag_t *tag = (const mb2_tag_t *)(uintptr_t)(mb2_info + offset);

        if (tag->type == MB2_TAG_END) {
            break;
        }
        if (tag->size < sizeof(mb2_tag_t)) {
            break;   /* 长度非法，直接停，不越界读 */
        }
        if (tag->type == MB2_TAG_MMAP) {
            /* 有内存图就以内存图为准，绝不把 meminfo 的粗略值也塞进来，
             * 否则同一段内存会在表里出现两次。 */
            have_mmap |= parse_mmap_tag((const mb2_tag_mmap_t *)tag);
        } else if (tag->type == MB2_TAG_MEMINFO && meminfo_addr == 0u) {
            meminfo_addr = (uint32_t)(uintptr_t)tag;
        }
        /* tag 之间 8 字节对齐 */
        offset += ((uint64_t)tag->size + 7ull) & ~7ull;
    }

    if (have_mmap) {
        return 1;   /* 优先用内存图 */
    }
    if (meminfo_addr != 0u) {
        (void)parse_meminfo_tag(
            (const mb2_tag_meminfo_t *)(uintptr_t)meminfo_addr);
        return 2;   /* 兜底：只有基本内存信息 */
    }
    return -3;
}

/* ------------------------------------------------------------------ */
/* 初始化                                                             */
/* ------------------------------------------------------------------ */

int phys_init(uint32_t mb2_magic, uint32_t mb2_info)
{
    uint64_t highest = 0ull;
    int rc;

    g_ready = 0;
    g_region_count = 0u;
    g_total_frames = 0ull;
    g_used_frames = 0ull;
    g_kernel_start = (uint64_t)(uintptr_t)&_kernel_start[0];
    g_kernel_end = (uint64_t)(uintptr_t)&_kernel_end[0];

    if (mb2_magic != MB2_BOOT_MAGIC) {
        sh_println(SH_ERR, "phys: bad multiboot2 magic, no memory map available");
        return -1;
    }

    rc = collect_regions(mb2_info);
    if (rc < 0) {
        sh_println(SH_ERR, "phys: multiboot2 info has no usable memory map");
        return rc;
    }

    /* 1) 全占用：任何没被证明可用的地址都默认不许碰 */
    for (uint64_t i = 0ull; i < PHYS_BITMAP_WORDS; ++i) {
        g_bitmap[i] = 0xFFFFFFFFFFFFFFFFull;
    }

    /*
     * 帧上限 = 最高的一段"可用"内存的末尾（再按 4GB 位图容量截断）。
     * 不用"所有区域里最高的地址"：PC 的 E820 常在 4GB 附近报一大段
     * reserved（PCI/MMIO 空洞），按它算会让 total 变成 4096MB、
     * used 变成几百万帧，全是假的。reserved 本来就不会被分配，
     * 直接落在管理范围之外更诚实。
     */
    for (uint32_t i = 0u; i < g_region_count; ++i) {
        if (g_regions[i].type != (uint32_t)PHYS_MEM_AVAILABLE) {
            continue;
        }
        const uint64_t end = g_regions[i].base + g_regions[i].length;
        if (end > highest) {
            highest = end;
        }
    }
    g_total_frames = highest / PHYS_PAGE_SIZE;
    if (g_total_frames > PHYS_FRAME_LIMIT) {
        g_total_frames = PHYS_FRAME_LIMIT;
    }
    if (g_total_frames == 0ull) {
        sh_println(SH_ERR, "phys: memory map reports zero usable frames");
        return -3;
    }

    /* 2) 只把 type=1 的区域放出来 */
    for (uint32_t i = 0u; i < g_region_count; ++i) {
        if (g_regions[i].type == (uint32_t)PHYS_MEM_AVAILABLE) {
            free_phys_range(g_regions[i].base, g_regions[i].length);
        }
    }

    /* 3) 收回必须自己留着的地方 */
    reserve_phys_range(0ull, LOW_RESERVED_BYTES);
    reserve_phys_range(g_kernel_start, g_kernel_end - g_kernel_start);
    reserve_phys_range((uint64_t)(uintptr_t)&g_bitmap[0],
                       (uint64_t)sizeof(g_bitmap));
    reserve_phys_range((uint64_t)mb2_info,
                       (uint64_t)((const mb2_info_t *)(uintptr_t)mb2_info)->total_size);

    recount_used();
    g_ready = 1;
    phys_ref_init();   /* [ADD] M15：引用计数表清零（COW fork 要用） */

    /* 启动日志：模仿 Linux 的 "Memory: ... available" 一行 */
    sh_print(SH_INFO, "Memory: ");
    print_dec(phys_total_bytes() >> 20);
    sh_print(SH_PLAIN, " MB total, ");
    print_dec(g_total_frames);
    sh_print(SH_PLAIN, " frames of 4 KB, ");
    print_dec(phys_free_frame_count());
    sh_print(SH_PLAIN, " usable (");
    print_dec((phys_free_frame_count() * PHYS_PAGE_SIZE) >> 20);
    sh_print(SH_PLAIN, " MB)");
    sh_newline();

    sh_print(SH_PLAIN, "[phys] bitmap ");
    print_hex((uint64_t)(uintptr_t)&g_bitmap[0]);
    sh_print(SH_PLAIN, " (");
    print_dec((uint64_t)sizeof(g_bitmap) >> 10);
    sh_print(SH_PLAIN, " KB), kernel ");
    print_hex(g_kernel_start);
    sh_print(SH_PLAIN, "-");
    print_hex(g_kernel_end);
    sh_newline();

    /* 同一句话进内核日志环：开机画面会被 login_screen 清屏，dmesg 里才留得住。 */
    klog_ring("phys: 4 KB frame bitmap allocator online (see memmap)");

    return 0;
}

/* ------------------------------------------------------------------ */
/* 分配 / 释放                                                        */
/* ------------------------------------------------------------------ */

void *phys_alloc_frames(uint64_t count, uint64_t align)
{
    if (!g_ready || count == 0ull || count > g_total_frames) {
        return (void *)0;
    }
    if (align < PHYS_PAGE_SIZE) {
        align = PHYS_PAGE_SIZE;
    }
    align = (align + PHYS_PAGE_SIZE - 1ull) & ~(PHYS_PAGE_SIZE - 1ull);

    const uint64_t step = align / PHYS_PAGE_SIZE;

    for (uint64_t base = 0ull; (base + count) <= g_total_frames; base += step) {
        uint64_t f;
        for (f = base; f < (base + count); ++f) {
            if (!bitmap_is_free(f)) {
                break;
            }
        }
        if (f != (base + count)) {
            continue;   /* 这段不连续 / 被占用，换下一个对齐起点 */
        }

        for (f = base; f < (base + count); ++f) {
            bitmap_set_used(f);
        }
        g_used_frames += count;

        const uint64_t addr = base * PHYS_PAGE_SIZE;
        zero_frames(addr, count);
        return (void *)(uintptr_t)addr;
    }

    return (void *)0;
}

/* ------------------------------------------------------------------ */
/* [M15] 页帧引用计数：给 COW fork 用                                  */
/* ------------------------------------------------------------------ */
/*
 * 没有 COW 之前，每一帧只有一个主（一个页表项指着它），所以"释放"＝"归还"。
 * COW fork 之后，父子的页表项指向**同一帧**，谁先释放都不能真的把帧还给分配器
 * —— 否则另一边就踩到已被别人拿走的帧。所以需要一张引用计数表：
 *
 *   g_frame_ref[frame] = 指着这一帧的页表项个数（新分配的帧记为 1）
 *   phys_share_frame()   ：再多一个主（fork 共享）
 *   phys_release_frame() ：少一个主，减到 0 才真的归还
 *
 * phys_free_frame() / phys_free_frames() 保持原语义（直接归还），
 * 但它们会把计数清 0，避免"归还后残留计数"这种阴魂。
 * 这张表 1MB 帧 × 4 字节 = 4MB 静态 …… 太大，所以只按 PHYS_FRAME_LIMIT 的
 * 实际规模开：128MB 机器 = 32768 帧 = 128KB，可以接受。
 */
static uint32_t g_frame_ref[PHYS_BITMAP_WORDS * 64u];
static int g_ref_ready;

void phys_ref_init(void)
{
    uint64_t i;

    for (i = 0ull; i < (uint64_t)(PHYS_BITMAP_WORDS * 64u); ++i) {
        g_frame_ref[i] = 0u;
    }
    g_ref_ready = 1;
}

static uint64_t ref_index(uint64_t pa)
{
    return pa / PHYS_PAGE_SIZE;
}

void phys_share_frame(uint64_t pa)
{
    uint64_t i;

    if (!g_ref_ready || (pa % PHYS_PAGE_SIZE) != 0ull) {
        return;
    }
    i = ref_index(pa);
    if (i >= (uint64_t)(PHYS_BITMAP_WORDS * 64u)) {
        return;
    }
    if (g_frame_ref[i] == 0u) {
        g_frame_ref[i] = 1u;    /* 只有一个主的帧，第一次共享时补记成 1 */
    }
    ++g_frame_ref[i];
}

int phys_release_frame(uint64_t pa)
{
    uint64_t i;

    if (!g_ref_ready || (pa % PHYS_PAGE_SIZE) != 0ull) {
        phys_free_frame((void *)(uintptr_t)pa);
        return 1;
    }
    i = ref_index(pa);
    if (i >= (uint64_t)(PHYS_BITMAP_WORDS * 64u) || g_frame_ref[i] == 0u) {
        phys_free_frame((void *)(uintptr_t)pa);
        return 1;
    }
    if (g_frame_ref[i] > 1u) {
        --g_frame_ref[i];
        return 0;               /* 还有别人指着，不归还 */
    }
    g_frame_ref[i] = 0u;
    phys_free_frame((void *)(uintptr_t)pa);
    return 1;
}

uint32_t phys_ref_count(uint64_t pa)
{
    uint64_t i;

    if (!g_ref_ready || (pa % PHYS_PAGE_SIZE) != 0ull) {
        return 0u;
    }
    i = ref_index(pa);
    if (i >= (uint64_t)(PHYS_BITMAP_WORDS * 64u)) {
        return 0u;
    }
    return g_frame_ref[i];
}
void *phys_alloc_frame(void)
{
    return phys_alloc_frames(1ull, PHYS_PAGE_SIZE);
}

void phys_free_frames(void *addr, uint64_t count)
{
    const uint64_t a = (uint64_t)(uintptr_t)addr;

    if (!g_ready || addr == (void *)0 || count == 0ull) {
        return;
    }
    if ((a % PHYS_PAGE_SIZE) != 0ull) {
        sh_println(SH_ERR, "phys_free: address not frame-aligned");
        return;
    }
    const uint64_t base = a / PHYS_PAGE_SIZE;
    if ((base + count) > g_total_frames) {
        sh_println(SH_ERR, "phys_free: range beyond managed memory");
        return;
    }
    /* 内核映像与低 1MB 是保留区，任何情况下都不许被放回去 */
    if (a < LOW_RESERVED_BYTES) {
        sh_println(SH_ERR, "phys_free: low 1 MB is reserved");
        return;
    }
    const uint64_t end = a + count * PHYS_PAGE_SIZE;
    if (a < g_kernel_end && end > g_kernel_start) {
        sh_println(SH_ERR, "phys_free: kernel image is reserved");
        return;
    }
    for (uint64_t f = base; f < (base + count); ++f) {
        if (bitmap_is_free(f)) {
            sh_println(SH_ERR, "phys_free: double free detected");
            return;
        }
    }
    for (uint64_t f = base; f < (base + count); ++f) {
        bitmap_set_free(f);
        g_frame_ref[f] = 0u;    /* [ADD] M15：归还的帧不留引用计数 */
    }
    g_used_frames -= count;
}

void phys_free_frame(void *addr)
{
    phys_free_frames(addr, 1ull);
}

/* ------------------------------------------------------------------ */
/* 统计与查询                                                         */
/* ------------------------------------------------------------------ */

uint64_t phys_total_frames(void)
{
    return g_total_frames;
}

uint64_t phys_used_frame_count(void)
{
    return g_used_frames;
}

uint64_t phys_free_frame_count(void)
{
    return (g_total_frames > g_used_frames) ? (g_total_frames - g_used_frames) : 0ull;
}

uint64_t phys_total_bytes(void)
{
    return g_total_frames * PHYS_PAGE_SIZE;
}

uint32_t phys_region_count(void)
{
    return g_region_count;
}

const phys_region_t *phys_region_get(uint32_t index)
{
    if (index >= g_region_count) {
        return (const phys_region_t *)0;
    }
    return &g_regions[index];
}

uint64_t phys_kernel_start(void)
{
    return g_kernel_start;
}

uint64_t phys_kernel_end(void)
{
    return g_kernel_end;
}

/* ------------------------------------------------------------------ */
/* 自测                                                               */
/* ------------------------------------------------------------------ */

int phys_self_test(void)
{
    const uint64_t base_used = phys_used_frame_count();
    void *a = phys_alloc_frame();
    void *b = phys_alloc_frame();
    uint8_t *pa = (uint8_t *)a;

    if (a == (void *)0 || b == (void *)0) {
        return 1;   /* 单帧都拿不到 */
    }
    if (a == b) {
        return 2;   /* 同一个帧发了两次 */
    }
    if ((uint64_t)(uintptr_t)a % PHYS_PAGE_SIZE != 0ull) {
        return 3;   /* 没对齐 */
    }
    /* 分配出来的帧必须是清零的 */
    for (uint64_t i = 0ull; i < PHYS_PAGE_SIZE; ++i) {
        if (pa[i] != 0u) {
            return 4;
        }
    }

    void *c = phys_alloc_frames(4ull, PHYS_PAGE_SIZE);
    if (c == (void *)0) {
        return 5;
    }
    /* 越界 / 不对齐的释放必须被拒（打印红字，但不改位图） */
    phys_free_frames((uint8_t *)a + 16u, 1ull);
    phys_free_frames(a, 1ull);
    phys_free_frames(a, 1ull);   /* 重复释放 */
    phys_free_frame(b);
    phys_free_frames(c, 4ull);

    if (phys_used_frame_count() != base_used) {
        return 6;   /* 分配再释放后计数没有回到原点 */
    }

    /* 2MB 对齐的连续 512 帧：M1/M2 的用户窗口就靠这个 */
    void *big = phys_alloc_frames(512ull, 0x200000ull);
    if (big == (void *)0) {
        return 7;
    }
    if ((uint64_t)(uintptr_t)big % 0x200000ull != 0ull) {
        return 8;
    }
    phys_free_frames(big, 512ull);

    if (phys_used_frame_count() != base_used) {
        return 9;
    }
    return 0;
}
