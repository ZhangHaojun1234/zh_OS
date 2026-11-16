/* vmm.c - 每进程地址空间：4 级页表的建立、4KB 页映射、切 CR3 */

#include "vmm.h"

#include "paging.h"
#include "../mm/phys.h"
#include "../shell/shell.h"

/*
 * 恒等映射这个假设从这里开始被打折。到 M4 为止，所有代码都当"虚拟地址 == 物理地址"使：
 * 页帧分配器返回个地址，直接当指针用。M5 之后它只对内核成立：内核（含本文件、所有内核栈、
 * 页帧位图、ramfs）还在低 1GB 的 2MB 大页恒等映射里，指针照旧能用；用户的页在 1GB 处，
 * 虚拟地址和物理地址已经分家，谁再混用谁出事。
 * 本文件里的 `pa` 一律是物理地址，只有画成指针去写页表时才借用恒等映射，
 * 因为页表本身一定在低 1GB 的物理内存里。
 */

#define PHYS_MASK   0x000FFFFFFFFFF000ULL

static uint64_t g_kernel_cr3;       /* 抓到之后不再变 */

/* [ADD] M15：COW 的两个计数器 ， 验收时要看到的"证据"。 */
static uint32_t g_cow_shared;       /* fork 时共享（而不是复制）了几页 */
static uint32_t g_cow_copied;       /* 有几个 COW 页因为被写而真正复制了一份 */
static uint32_t g_cow_faults;       /* 一共处理了几次 COW 缺页 */

/* [ADD] M16：按需分页的计数器 */
static uint32_t g_demand_faults;    /* 处理了几次"页不存在但登记过"的缺页 */
static uint32_t g_demand_pages;     /* 一共让多少页在第一次访问时才出现 */

uint32_t vmm_demand_fault_count(void) { return g_demand_faults; }
uint32_t vmm_demand_page_count(void)  { return g_demand_pages; }

uint32_t vmm_cow_shared_count(void) { return g_cow_shared; }
uint32_t vmm_cow_copy_count(void)   { return g_cow_copied; }
uint32_t vmm_cow_fault_count(void)  { return g_cow_faults; }

void vmm_cow_reset_stats(void) { g_cow_shared = 0u; g_cow_copied = 0u; g_cow_faults = 0u; }

static uint64_t *as_ptr(uint64_t pa)
{
    /* 恒等映射：物理地址可以直接当指针；这是内核侧的"特权"。 */
    return (uint64_t *)(uintptr_t)pa;
}

static void zero_page(uint64_t *page)
{
    uint32_t i;

    for (i = 0u; i < VMM_ENTRIES; ++i) {
        page[i] = 0u;
    }
}

uint64_t vmm_kernel_cr3(void)
{
    if (g_kernel_cr3 == 0u) {
        g_kernel_cr3 = paging_get_cr3() & PHYS_MASK;
    }
    return g_kernel_cr3;
}

void vmm_load_cr3(uint64_t cr3)
{
    if (cr3 == 0u) {
        return;
    }
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

int vmm_space_create(vmm_space_t *s, uint64_t va_base)
{
    uint64_t pml4_pa;
    uint64_t pdpt_pa;
    uint64_t pd_pa;
    uint64_t pt_pa;
    uint64_t kernel_pml4_pa;
    uint64_t kernel_pdpt_pa;
    uint64_t kernel_pd_pa;
    uint64_t *pml4;
    uint64_t *pdpt;
    uint64_t *pd;
    uint64_t *pt;

    if (s == (vmm_space_t *)0) {
        return -1;
    }
    /* 只支持 2MB 对齐、且落在内核恒等映射之外的基点（本版就是 1GB 处）。 */
    if ((va_base & (VMM_USER_SPAN - 1u)) != 0u || va_base < PAGE_1GB) {
        return -1;
    }

    /* 一层一帧，全从页帧分配器拿（分配出来已经是清零的）。 */
    pml4_pa = (uint64_t)(uintptr_t)phys_alloc_frame();
    pdpt_pa = (uint64_t)(uintptr_t)phys_alloc_frame();
    pd_pa = (uint64_t)(uintptr_t)phys_alloc_frame();
    pt_pa = (uint64_t)(uintptr_t)phys_alloc_frame();
    if (pml4_pa == 0u || pdpt_pa == 0u || pd_pa == 0u || pt_pa == 0u) {
        if (pt_pa != 0u) { phys_free_frame((void *)(uintptr_t)pt_pa); }
        if (pd_pa != 0u) { phys_free_frame((void *)(uintptr_t)pd_pa); }
        if (pdpt_pa != 0u) { phys_free_frame((void *)(uintptr_t)pdpt_pa); }
        if (pml4_pa != 0u) { phys_free_frame((void *)(uintptr_t)pml4_pa); }
        return -1;
    }

    pml4 = as_ptr(pml4_pa);
    pdpt = as_ptr(pdpt_pa);
    pd = as_ptr(pd_pa);
    pt = as_ptr(pt_pa);
    zero_page(pml4);
    zero_page(pdpt);
    zero_page(pd);
    zero_page(pt);

    /* 内核那一半：把内核自己的 PDPT 抄过来（0~1GB 的 2MB 大页恒等映射），U/S 保持 0，
     * 用户态访问内核地址就在这一级被挡住。这里是共享同一张 PDPT/PD，不复制：内核映射
     * 每个进程都一样，复制一份既浪费又容易改一处忘一处，Linux 也是共享内核页表上半部分。 */
    kernel_pml4_pa = paging_get_cr3() & PHYS_MASK;
    kernel_pdpt_pa = as_ptr(kernel_pml4_pa)[0] & PHYS_MASK;
    kernel_pd_pa = as_ptr(kernel_pdpt_pa)[0] & PHYS_MASK;
    pdpt[0] = kernel_pd_pa | PAGE_PRESENT | PAGE_WRITABLE;

    /* M5 踩过的坑，第一版就死在这儿。上面那行起初把 pdpt[0] 指向了内核的 PDPT 本身，
     * 于是新表里 PDPT[0] 指向一个 PDPT，可 CPU 认为 PDPT 项一定指向 PD。多抄或少抄一层的
     * 后果是恒等映射整段错位，现象是 mov cr3 的下一条指令就 #PF，而且 CR2 恰好等于 CR3 的值，
     * 取指走的就是那张坏表。正确的做法是新 PDPT[0] 指向内核的 PD，再往下一层。 */

    /* 用户那一半：1GB 处（PDPT 索引 1）指向本进程私有的 PD。 */
    pdpt[1] = pd_pa | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    pd[0] = pt_pa | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;

    /* 顶层必须 U=1，否则用户态连"走到 PDPT[1]"这一步都做不到；
     * 挡不挡得住内核地址，靠的是 pdpt[0] 的 U=0，而不是这一位。 */
    pml4[0] = pdpt_pa | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;

    {
        uint32_t k;
        for (k = 0u; k < VMM_ENTRIES; ++k) {
            s->desire[k] = 0u;      /* [M16] 新空间没有任何按需承诺 */
        }
    }
    s->cr3 = pml4_pa;
    s->pml4_pa = pml4_pa;
    s->pdpt_pa = pdpt_pa;
    s->pd_pa = pd_pa;
    s->pt_pa = pt_pa;
    s->va_base = va_base;

    return 0;
}

int vmm_map_4k(vmm_space_t *s, uint64_t va, uint64_t pa, int writable)
{
    uint64_t flags;
    uint64_t index;

    if (s == (vmm_space_t *)0 || s->pt_pa == 0u) {
        return -1;
    }
    if ((va & (VMM_PAGE_SIZE - 1u)) != 0u || (pa & (VMM_PAGE_SIZE - 1u)) != 0u) {
        return -1;
    }
    if (va < s->va_base || va >= (s->va_base + VMM_USER_SPAN)) {
        return -1;
    }

    flags = PAGE_PRESENT | PAGE_USER;
    if (writable) {
        flags |= PAGE_WRITABLE;
    }

    index = (va - s->va_base) >> 12;
    as_ptr(s->pt_pa)[index] = (pa & PHYS_MASK) | flags;
    s->desire[index] = 0u;      /* [M16] 映射成真的了，撤销按需登记 */
    return 0;
}

uint64_t vmm_probe(const vmm_space_t *s, uint64_t va)
{
    uint64_t i;

    if (s == (vmm_space_t *)0 || s->pt_pa == 0u) {
        return 0u;
    }
    if (va < s->va_base || va >= (s->va_base + VMM_USER_SPAN)) {
        return 0u;
    }
    i = (va - s->va_base) >> 12;
    return as_ptr(s->pt_pa)[i] & PHYS_MASK;
}

/*
 * [ADD] M16：登记一段"将来会存在"的页，不分配帧。之后第一次读写这些页，CPU 报 #PF（P=0），
 * vmm_page_fault() 才真给一帧并清零。这是 Linux demand-zero / anonymous page 的最简版本：
 * 装载时只承诺"这段地址是零"，而不是真把零写进去。
 */
int vmm_space_want(vmm_space_t *s, uint64_t va, uint64_t size, uint32_t flags)
{
    uint64_t first;
    uint64_t last;
    uint64_t i;

    if (s == (vmm_space_t *)0 || s->pt_pa == 0u || size == 0u) {
        return -1;
    }
    if (va < s->va_base || va >= (s->va_base + VMM_USER_SPAN)) {
        return -1;
    }
    first = (va - s->va_base) >> 12;
    last = ((va + size - 1u) - s->va_base) >> 12;
    if (last >= (uint64_t)VMM_ENTRIES) {
        return -1;
    }
    for (i = first; i <= last; ++i) {
        /* 已经映射的页不需要"愿望"：它已经存在了。 */
        if ((as_ptr(s->pt_pa)[i] & PAGE_PRESENT) != 0u) {
            continue;
        }
        s->desire[i] = (uint8_t)flags;
    }
    return 0;
}

uint32_t vmm_count_wanted(const vmm_space_t *s)
{
    uint32_t i;
    uint32_t n = 0u;

    if (s == (vmm_space_t *)0) {
        return 0u;
    }
    for (i = 0u; i < VMM_ENTRIES; ++i) {
        if (s->desire[i] != 0u) {
            ++n;
        }
    }
    return n;
}

/* [ADD] M15：把一帧以"只读 + COW"的方式映射进 s（父子共享的那种页）。 */
int vmm_map_4k_cow(vmm_space_t *s, uint64_t va, uint64_t pa)
{
    uint64_t index;

    if (s == (vmm_space_t *)0 || s->pt_pa == 0u) {
        return -1;
    }
    if ((va & (VMM_PAGE_SIZE - 1u)) != 0u || (pa & (VMM_PAGE_SIZE - 1u)) != 0u) {
        return -1;
    }
    if (va < s->va_base || va >= (s->va_base + VMM_USER_SPAN)) {
        return -1;
    }
    index = (va - s->va_base) >> 12;
    /* 注意：这里不带 PAGE_WRITABLE ， 写就会 #PF，那就是 COW 的入口。 */
    as_ptr(s->pt_pa)[index] = (pa & PHYS_MASK) | PAGE_PRESENT | PAGE_USER | PAGE_COW;
    s->desire[index] = 0u;      /* [M16] 已经存在的页不再需要"愿望" */
    return 0;
}

/*
 * [M15] 处理一次 COW 缺页，Linux do_wp_page() 的雏形。判据全满足才算 COW 缺页：
 * 是 #PF（vector 14）、err bit0=1（页存在，不存在的那叫没映射）、bit1=1（写导致的，
 * 读只读页不会 fault）、bit2=1（来自用户态）、CR2 落在本进程私有窗口内且那一项带 PAGE_COW。
 * 处理是申请一帧新的、拷整页、PT 项改成可写并去掉 COW、老引用计数减一。
 * 返回 1 表示修好了，iretq 回去重执行那条写指令；0 表示不是 COW，交给上层。
 */
int vmm_cow_fault(uint64_t va, uint64_t err)
{
    vmm_space_t space;
    uint64_t *pt;
    uint64_t index;
    uint64_t pte;
    uint64_t old_pa;
    uint64_t new_pa;
    uint8_t *from;
    uint8_t *to;
    uint32_t k;

    /*
     * [M15 踩的坑] 这里一开始只看 U/S 位，要求 err bit2=1 即来自用户态，结果 #PF 从 ring0
     * 进来时被当成不可修，直接 panic。怎么会从 ring0 写 COW 页？因为本版内核没有
     * copy_to_user()，sys_getcwd / sys_readdir 这些是内核直接往用户缓冲区里写的。
     * fork 之后父进程那些页已经只读加 COW，内核代写就撞上同一道墙。
     * 修法是不看特权级：只要 present + 写 + 本进程窗口里的 COW 页，就是一次合法写时复制，
     * 谁写的都一样，都得先有自己的副本。要是放行内核直接写共享帧（比如把 CR0.WP 置 0），
     * 父进程这次写就污染了子进程那份，COW 就成了假的。
     */
    if ((err & 0x1u) == 0u || (err & 0x2u) == 0u) {   /* 必须"存在"且"写" */
        return 0;
    }
    if (vmm_current_space(&space) != 0) {
        return 0;
    }
    if (va < space.va_base || va >= (space.va_base + VMM_USER_SPAN)) {
        return 0;
    }

    pt = as_ptr(space.pt_pa);
    index = (va - space.va_base) >> 12;
    pte = pt[index];
    if ((pte & PAGE_PRESENT) == 0u || (pte & PAGE_COW) == 0u) {
        return 0;                       /* 不是我们标的 COW 页 */
    }

    old_pa = pte & PHYS_MASK;
    new_pa = (uint64_t)(uintptr_t)phys_alloc_frame();
    if (new_pa == 0u) {
        return 0;                       /* 没内存了：交给上层按异常处理 */
    }
    from = (uint8_t *)(uintptr_t)old_pa;
    to = (uint8_t *)(uintptr_t)new_pa;
    for (k = 0u; k < VMM_PAGE_SIZE; ++k) {
        to[k] = from[k];
    }

    pt[index] = (new_pa & PHYS_MASK) | PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE;
    phys_release_frame(old_pa);         /* 少一个主；别人还指着就不真的归还 */

    ++g_cow_faults;
    ++g_cow_copied;
    return 1;
}

/*
 * [ADD] M16：按需分配分支，这一页你从没碰过，但装载时答应过它存在。判据是页不存在
 * （err bit0=0）、落在私有窗口内、desire[index] 非零（装载器用 vmm_space_want() 登记过）。
 * 分配一帧，分配器返回的帧已经清零，正好是 .bss 和匿名页要的全零，再按愿望位决定可写与否。
 * 返回 1 修好了，0 表示不是按需页，可能是野指针、栈溢出或越权。
 */
static int vmm_demand_fault_here(vmm_space_t *s, uint64_t va, uint64_t err)
{
    uint64_t *pt;
    uint64_t index;
    uint64_t pa;
    uint64_t flags;

    if ((err & 0x1u) != 0u) {
        return 0;                   /* 页是存在的：不是"按需"这条路 */
    }
    index = (va - s->va_base) >> 12;
    if (index >= (uint64_t)VMM_ENTRIES || s->desire[index] == 0u) {
        return 0;
    }
    pa = (uint64_t)(uintptr_t)phys_alloc_frame();
    if (pa == 0u) {
        return 0;                   /* 没内存：交给上层按真异常处理 */
    }
    flags = PAGE_PRESENT | PAGE_USER;
    if ((s->desire[index] & VMM_WANT_WRITE) != 0u) {
        flags |= PAGE_WRITABLE;
    }
    pt = as_ptr(s->pt_pa);
    pt[index] = (pa & PHYS_MASK) | flags;
    s->desire[index] = 0u;          /* 这个承诺已经兑现了 */

    ++g_demand_faults;
    ++g_demand_pages;
    return 1;
}

/*
 * [ADD] M16：统一缺页入口，位置相当于 Linux 的 handle_pte_fault，顺序也照它来：
 * 1) 页存在 + 写 + COW，走 do_wp_page()（M15）；
 * 2) 页不存在 + 登记过按需，走 do_anonymous_page()，零页按需出现（M16）；
 * 3) 其他交给上层，ring3 杀进程，ring0 panic。
 */
int vmm_page_fault(uint64_t va, uint64_t err)
{
    vmm_space_t space;
    int fixed;

    if (vmm_current_space(&space) != 0) {
        return 0;
    }
    if (va < space.va_base || va >= (space.va_base + VMM_USER_SPAN)) {
        return 0;
    }

    fixed = vmm_cow_fault(va, err);
    if (fixed != 0) {
        return fixed;
    }
    return vmm_demand_fault_here(&space, va, err);
}

int vmm_space_clone(vmm_space_t *dst, const vmm_space_t *src, uint32_t *pages_out)
{
    uint32_t i;
    uint32_t copied = 0u;
    uint32_t shared = 0u;

    if (dst == (vmm_space_t *)0 || src == (vmm_space_t *)0 || src->pt_pa == 0u) {
        return -1;
    }

    if (vmm_space_create(dst, src->va_base) != 0) {
        return -1;
    }

    /*
     * [M15] COW 的核心就在这儿：一帧都不复制。旧版 M5/M7 对每个已映射页都是
     * phys_alloc_frame() 加整页拷贝，代价跟进程常驻内存成正比。现在改成三步：
     * 子进程 PT 项指向父进程同一帧，只读加 COW；父进程那一项也改成只读加 COW，
     * 否则父进程一写子进程就看见脏数据；这一帧引用计数加一。
     * 谁先写谁触发 #PF，那一刻才复制自己那份。父子都不写的情况下，fork 的用户页
     * 代价是 0 帧，只花 4 张页表帧。
     */
    for (i = 0u; i < VMM_ENTRIES; ++i) {
        uint64_t *src_pt = as_ptr(src->pt_pa);
        uint64_t pte = src_pt[i];
        uint64_t src_pa;
        uint64_t va;

        if ((pte & PAGE_PRESENT) == 0u) {
            continue;                   /* 没映射的页在子进程里也保持"洞" */
        }
        src_pa = pte & PHYS_MASK;
        va = src->va_base + (uint64_t)i * VMM_PAGE_SIZE;

        if (vmm_map_4k_cow(dst, va, src_pa) != 0) {
            return -1;
        }
        /* 父进程这一项：去掉 W、打上 COW（本来就是 COW 页的话只续记共享） */
        src_pt[i] = (src_pa & ~0xFFFULL) | PAGE_PRESENT | PAGE_USER | PAGE_COW;
        phys_share_frame(src_pa);

        ++shared;
    }

    /* [M16] 子进程继承父进程的"按需承诺"：那些还没出现的页，将来在子进程里
     * 也一样按需出现（否则子进程一碰就是真异常）。 */
    for (i = 0u; i < VMM_ENTRIES; ++i) {
        dst->desire[i] = src->desire[i];
    }

    if (pages_out != (uint32_t *)0) {
        *pages_out = copied;        /* COW：fork 时真正复制过的用户页 = 0 */
    }
    g_cow_shared += shared;
    return 0;
}

/*
 * [M12] 销毁一个地址空间，Linux mmput() / free_pgtables() 的雏形。私有窗口只有 2MB，
 * 正好被一个 PT 覆盖（512 项乘 4KB），扫一遍这页 PT 就够：每个 PRESENT 项背后那帧就是
 * 用户真正拿到的内存（代码页、栈页、fork 出来的页），全还回去。
 * 四张页表自己那四帧也得还，这是最容易漏的地方：只还用户页不还页表，连续 exec 会让
 * 页表帧一直涨，每个进程 4 帧看着不多，跑几十条命令就看出来了。
 */
int vmm_space_destroy(vmm_space_t *s, uint32_t *pages_out)
{
    uint64_t *pt;
    uint32_t i;
    uint32_t freed_pages = 0u;

    if (s == (vmm_space_t *)0) {
        return -1;
    }
    if (s->cr3 != 0u && s->cr3 == vmm_kernel_cr3()) {
        err("vmm: refusing to destroy the kernel address space");
        return -1;
    }
    if (s->pt_pa == 0u || s->pml4_pa == 0u) {
        return -1;
    }

    /* 1) 归还私有窗口里的每一个用户页 */
    pt = as_ptr(s->pt_pa);
    for (i = 0u; i < VMM_ENTRIES; ++i) {
        uint64_t entry = pt[i];

        if ((entry & PAGE_PRESENT) == 0u) {
            continue;
        }
        pt[i] = 0u;
        /* [M15] 用引用计数释放：如果这帧还和别的地址空间共享（COW），
         * 就只减一、不归还；减到 0 才真的回分配器。 */
        if (phys_release_frame(entry & PHYS_MASK) != 0) {
            ++freed_pages;
        }
    }

    /* 2) 归还四张页表帧本身（内核共享的那个 PD 不在这里，别碰） */
    phys_free_frame((void *)(uintptr_t)s->pt_pa);
    phys_free_frame((void *)(uintptr_t)s->pd_pa);
    phys_free_frame((void *)(uintptr_t)s->pdpt_pa);
    phys_free_frame((void *)(uintptr_t)s->pml4_pa);

    s->cr3 = 0u;
    s->pml4_pa = 0u;
    s->pdpt_pa = 0u;
    s->pd_pa = 0u;
    s->pt_pa = 0u;

    if (pages_out != (uint32_t *)0) {
        *pages_out = freed_pages;
    }
    return 0;
}
uint32_t vmm_count_mapped(const vmm_space_t *s)
{
    uint32_t i;
    uint32_t n = 0u;

    if (s == (vmm_space_t *)0 || s->pt_pa == 0u) {
        return 0u;
    }
    for (i = 0u; i < VMM_ENTRIES; ++i) {
        if ((as_ptr(s->pt_pa)[i] & PAGE_PRESENT) != 0u) {
            ++n;
        }
    }
    return n;
}

uint64_t vmm_walk(const vmm_space_t *s, uint64_t va)
{
    uint64_t pml4e;
    uint64_t pdpte;
    uint64_t pde;
    uint64_t pte;

    if (s == (vmm_space_t *)0 || s->pml4_pa == 0u) {
        return 0u;
    }

    pml4e = as_ptr(s->pml4_pa)[(va >> 39) & 0x1FFu];
    if ((pml4e & PAGE_PRESENT) == 0u) {
        return 0u;
    }
    pdpte = as_ptr(pml4e & PHYS_MASK)[(va >> 30) & 0x1FFu];
    if ((pdpte & PAGE_PRESENT) == 0u || (pdpte & PAGE_HUGE) != 0u) {
        return 0u;
    }
    pde = as_ptr(pdpte & PHYS_MASK)[(va >> 21) & 0x1FFu];
    if ((pde & PAGE_PRESENT) == 0u) {
        return 0u;
    }
    if ((pde & PAGE_HUGE) != 0u) {
        return (pde & 0x000FFFFFFFE00000ULL) | (va & 0x1FFFFFULL);
    }
    pte = as_ptr(pde & PHYS_MASK)[(va >> 12) & 0x1FFu];
    if ((pte & PAGE_PRESENT) == 0u) {
        return 0u;
    }
    return (pte & PHYS_MASK) | (va & 0xFFFULL);
}
