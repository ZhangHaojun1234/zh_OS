/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    mm/heap.c
 * Version: 0.2beta
 * Brief:   2MB 内核堆：bump 分配、8 字节对齐、空闲链表回收与相邻块合并，
 *          以及 meminfo 命令实现。纯增量模块，不改动任何已有功能。
 * ============================================================================ */

#include "heap.h"

#include <stddef.h>

#include "../shell/shell.h"
#include "klog.h"  // [ADD]

/*
 * 设计说明
 * --------
 * 池：.bss 中的静态数组 heap_pool[2MB]，4K 对齐。它落在 boot.asm 建立的
 *     低 1GB 恒等映射内，不需要新增页表映射；GRUB 加载 ELF 时 .bss 已清零。
 *
 * 块布局：每个块紧跟一个 24 字节头，之后是 8 字节对齐的 payload：
 *     [ header: size | magic | next ][ payload ... ]
 *     size 是 payload 字节数（已按 8 对齐），next 只在空闲链表里有效。
 *     heap_pool 基址 4K 对齐、头大小 24 是 8 的倍数，所以 payload 天然 8 对齐。
 *
 * 分配：先按 first-fit 在空闲链表里找可复用块；找不到再从 heap_brk 处
 *     bump 一块。本版不做块拆分（教学实现，内部碎片可接受）。
 *
 * 回收：kfree 把块挂回空闲链表，heap_coalesce() 反复扫描链表，把物理上
 *     首尾相接的空闲块合并，直到没有可合并项为止。
 *
 * 统计口径：heap_brk 是从池里切走的字节总数；空闲链表里所有块的
 *     (header + size) 之和记为可复用字节。used = brk - 可复用，
 *     free = 2MB - used。所以初始 used=0 / free=2097152，
 *     全部释放并合并后能精确回到这个数字。
 *
 * 出错策略：越界、重复 free、池耗尽一律打印亮红错误并返回，绝不 panic。
 */

#define HEAP_POOL_BYTES   (2u * 1024u * 1024u)
#define HEAP_ALIGN        8u
#define HEAP_MAGIC_ALLOC  0xA110C8EDu
#define HEAP_MAGIC_FREE   0xF2EEB10Cu

typedef struct heap_block {
    uint64_t size;             /* payload 字节数，8 对齐 */
    uint64_t magic;            /* HEAP_MAGIC_ALLOC / HEAP_MAGIC_FREE */
    struct heap_block *next;   /* 空闲链表下一项 */
} heap_block_t;

#define HEADER_SIZE ((uint64_t)sizeof(heap_block_t))

/* 4K 对齐的 2MB 内核堆，位于 .bss，不依赖任何新页表映射。 */
static uint8_t heap_pool[HEAP_POOL_BYTES] __attribute__((aligned(4096)));

static uint8_t *heap_base;
static uint64_t heap_total;
static uint64_t heap_brk;          /* 已从池中切走的字节偏移 */
static heap_block_t *free_head;
static uint64_t alloc_count;
static uint64_t free_count;
static int heap_ready;

/* ------------------------------------------------------------------ */
/* 本地工具（freestanding 无 libc）                                   */
/* ------------------------------------------------------------------ */

static uint8_t *payload_of(heap_block_t *b)
{
    return (uint8_t *)b + HEADER_SIZE;
}

static void zero_bytes(void *p, uint64_t n)
{
    uint8_t *q = (uint8_t *)p;
    for (uint64_t i = 0; i < n; ++i) {
        q[i] = 0u;
    }
}

/* 把 uint64 十进制打印成浅灰文本，供 heap_stats / 自测摘要复用。 */
static void print_num(uint64_t value)
{
    char buf[21];
    char tmp[20];
    uint32_t n = 0;
    uint32_t t = 0;

    if (value == 0u) {
        buf[n++] = '0';
    } else {
        while (value != 0u) {
            tmp[t++] = (char)('0' + (value % 10u));
            value /= 10u;
        }
        while (t != 0u) {
            buf[n++] = tmp[--t];
        }
    }
    buf[n] = '\0';
    sh_print(SH_PLAIN, buf);
}

static uint64_t free_reclaimed_bytes(void)
{
    uint64_t total = 0u;
    for (heap_block_t *b = free_head; b != 0; b = b->next) {
        total += HEADER_SIZE + b->size;
    }
    return total;
}

static uint64_t heap_used_bytes(void)
{
    const uint64_t reclaimed = free_reclaimed_bytes();
    return (reclaimed <= heap_brk) ? (heap_brk - reclaimed) : 0u;
}

/* ------------------------------------------------------------------ */
/* 初始化                                                             */
/* ------------------------------------------------------------------ */

void heap_init(void)
{
    heap_base = heap_pool;
    heap_total = (uint64_t)HEAP_POOL_BYTES;
    heap_brk = 0u;
    free_head = 0;
    alloc_count = 0u;
    free_count = 0u;
    heap_ready = 1;
    klog("heap initialized"); // [ADD]
}

/* ------------------------------------------------------------------ */
/* 分配                                                               */
/* ------------------------------------------------------------------ */

void *kmalloc(uint64_t size)
{
    if (!heap_ready) {
        heap_init();
    }
    if (size == 0u) {
        return 0;
    }
    if (size > heap_total) {
        sh_println(SH_ERR, "kmalloc: requested size exceeds heap pool");
        return 0;
    }

    const uint64_t need = (size + (HEAP_ALIGN - 1u)) & ~((uint64_t)HEAP_ALIGN - 1u);

    /* 1) 优先复用空闲链表里的块（first-fit） */
    heap_block_t **link = &free_head;
    while (*link != 0) {
        heap_block_t *b = *link;
        if (b->size >= need) {
            *link = b->next;
            b->next = 0;
            b->magic = HEAP_MAGIC_ALLOC;
            --free_count;
            ++alloc_count;
            return payload_of(b);
        }
        link = &b->next;
    }

    /* 2) 空闲链表没有合适的块，从 brk 处 bump 一块 */
    const uint64_t chunk = HEADER_SIZE + need;
    if (chunk > heap_total - heap_brk) {
        sh_println(SH_ERR, "kmalloc: out of heap memory");
        return 0;
    }

    heap_block_t *b = (heap_block_t *)(heap_base + heap_brk);
    b->size = need;
    b->magic = HEAP_MAGIC_ALLOC;
    b->next = 0;
    heap_brk += chunk;
    ++alloc_count;
    return payload_of(b);
}

void *kzalloc(uint64_t size)
{
    void *p = kmalloc(size);
    if (p != 0) {
        /* 记录实际对齐后的长度，保证清零覆盖整块 payload。 */
        const uint64_t need =
            (size + (HEAP_ALIGN - 1u)) & ~((uint64_t)HEAP_ALIGN - 1u);
        zero_bytes(p, need);
    }
    return p;
}

/* ------------------------------------------------------------------ */
/* 回收与相邻块合并                                                   */
/* ------------------------------------------------------------------ */

static void heap_coalesce(void)
{
    int merged;

    do {
        merged = 0;
        heap_block_t **outer = &free_head;
        while (*outer != 0 && !merged) {
            heap_block_t *a = *outer;
            heap_block_t **inner = &free_head;

            while (*inner != 0) {
                heap_block_t *b = *inner;
                const uint64_t a_end =
                    (uint64_t)(uintptr_t)a + HEADER_SIZE + a->size;

                if (a != b && a_end == (uint64_t)(uintptr_t)b) {
                    /* a 紧挨着 b：把 b 的头与 payload 一起并进 a。 */
                    a->size += HEADER_SIZE + b->size;
                    *inner = b->next;
                    --free_count;
                    merged = 1;
                    break;
                }
                inner = &(*inner)->next;
            }
            outer = &(*outer)->next;
        }
    } while (merged);
}

void kfree(void *ptr)
{
    if (ptr == 0) {
        return;   /* 与 C 的 free(NULL) 语义一致，静默返回 */
    }
    if (!heap_ready) {
        sh_println(SH_ERR, "kfree: heap not initialized");
        return;
    }

    const uint64_t base = (uint64_t)(uintptr_t)heap_base;
    const uint64_t addr = (uint64_t)(uintptr_t)ptr;
    const uint64_t brk_end = base + heap_brk;

    if (addr < base + HEADER_SIZE || addr >= brk_end) {
        sh_println(SH_ERR, "kfree: pointer outside kernel heap");
        return;
    }
    if (((addr - HEADER_SIZE) & (HEAP_ALIGN - 1u)) != 0u) {
        sh_println(SH_ERR, "kfree: misaligned pointer");
        return;
    }

    heap_block_t *b = (heap_block_t *)(uintptr_t)(addr - HEADER_SIZE);
    if (b->magic == HEAP_MAGIC_FREE) {
        sh_println(SH_ERR, "kfree: double free detected");
        return;
    }
    if (b->magic != HEAP_MAGIC_ALLOC) {
        sh_println(SH_ERR, "kfree: invalid block header");
        return;
    }
    if ((uint64_t)(uintptr_t)b + HEADER_SIZE + b->size > brk_end) {
        sh_println(SH_ERR, "kfree: corrupt block size");
        return;
    }

    b->magic = HEAP_MAGIC_FREE;
    b->next = free_head;
    free_head = b;
    --alloc_count;
    ++free_count;

    heap_coalesce();
}

/* ------------------------------------------------------------------ */
/* 统计输出                                                           */
/* ------------------------------------------------------------------ */

void heap_stats(void)
{
    const uint64_t used = heap_used_bytes();
    const uint64_t free_bytes = heap_total - used;

    sh_println(SH_INFO, "Kernel heap (bump + free list):");
    sh_print(SH_PLAIN, "  pool      : ");
    print_num(heap_total);
    sh_println(SH_PLAIN, " bytes");
    sh_print(SH_PLAIN, "  used      : ");
    print_num(used);
    sh_println(SH_PLAIN, " bytes (incl. headers)");
    sh_print(SH_PLAIN, "  free      : ");
    print_num(free_bytes);
    sh_println(SH_PLAIN, " bytes");
    sh_print(SH_PLAIN, "  blocks    : ");
    print_num(alloc_count);
    sh_print(SH_PLAIN, " allocated, ");
    print_num(free_count);
    sh_println(SH_PLAIN, " free");

    if (free_bytes * 10u < heap_total) {
        sh_println(SH_WARN, "  warning: free space below 10%");
    }
}

/* 自测用的一行紧凑摘要：used / free / 已分配块数 / 空闲块数。 */
static void heap_brief(const char *label)
{
    const uint64_t used = heap_used_bytes();

    sh_print(SH_PLAIN, "  ");
    sh_print(SH_PLAIN, label);
    sh_print(SH_PLAIN, " used=");
    print_num(used);
    sh_print(SH_PLAIN, " free=");
    print_num(heap_total - used);
    sh_print(SH_PLAIN, " blocks=");
    print_num(alloc_count);
    sh_print(SH_PLAIN, "/");
    print_num(free_count);
    sh_newline();
}

/*
 * 可选自测（meminfo test）：跑几次 kmalloc/kzalloc/kfree，
 * 验证统计、相邻块合并、重复 free、越界与池耗尽处理，最后把堆还原。
 * 无参数的 meminfo 不会走这里，只打印 heap_stats() 的结果。
 */
static void heap_selftest(void)
{
    sh_println(SH_INFO, "heap selftest (meminfo test):");
    heap_brief("initial  ");

    uint8_t *a = (uint8_t *)kmalloc(64u);
    uint8_t *b = (uint8_t *)kzalloc(128u);
    uint8_t *c = (uint8_t *)kmalloc(256u);
    heap_brief("3 allocs ");

    int zeroed = (b != 0);
    if (b != 0) {
        for (uint64_t i = 0; i < 128u; ++i) {
            if (b[i] != 0u) {
                zeroed = 0;
                break;
            }
        }
    }
    sh_print(SH_PLAIN, "  kzalloc zeroed : ");
    sh_println(zeroed ? SH_OK : SH_ERR, zeroed ? "yes" : "no");

    kfree(b);
    kfree(b);   /* 重复 free：应打印亮红错误 */
    kfree(a);
    kfree(c);   /* 三个相邻块应合并成一个空闲块 */
    heap_brief("all freed");

    /* 越界指针与超池容量分配：都应亮红报错、不 panic */
    kfree((void *)(uintptr_t)((uint64_t)(uintptr_t)heap_base + heap_total + 4096u));
    (void)kmalloc(heap_total + 1u);

    /* 大分配把剩余压到 10% 以下，触发亮黄警告 */
    void *big = kmalloc(1900u * 1024u);
    heap_brief("big alloc");
    heap_stats();
    kfree(big);
    heap_brief("final    ");
}

/* ------------------------------------------------------------------ */
/* meminfo 命令                                                       */
/* ------------------------------------------------------------------ */

int cmd_meminfo(int argc, char **argv)
{
    (void)argv;

    if (argc >= 2) {
        heap_selftest();
    } else {
        heap_stats();
    }
    return SHELL_CONTINUE;
}
