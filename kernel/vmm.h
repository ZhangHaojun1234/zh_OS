/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    kernel/vmm.h
 * Version: 0.2beta
 * Brief:   M5 进程地址空间：每进程一张 PML4、4KB 页映射、独立 CR3。
 * ============================================================================ */

#ifndef ZH_OS_VMM_H
#define ZH_OS_VMM_H

#include <stdint.h>

/*
 * M5 要解决什么问题
 * ------------------
 * M4 有进程了，但所有任务共用内核那张页表：两个任务只是被塞进两块不同的
 * 2MB 大页里，各自靠 U/S 位"看不见对方"——那是假的，U/S 位只能挡住"用户
 * 访问内核"，挡不住"用户 A 访问用户 B"（两块大页对 ring3 都是 U=1）。
 *
 * Linux 的做法是：每个进程一张自己的页表（独立 CR3），进程能看见的虚拟
 * 地址空间由这张表决定，跟别的进程没有任何关系。本文件就是这一层的雏形。
 *
 * 地址空间布局（本版）
 * -------------------
 *   0x0000000000000000 - 0x000000003FFFFFFF   内核恒等映射（2MB 大页，U=0）
 *                                             —— 所有进程共享同一份
 *   0x0000000040000000 - 0x00000000401FFFFF   本进程私有窗口（4KB 页，U=1）
 *                                             —— 每进程一个 PD + 一个 PT
 *
 * 为什么私有窗口放在 1GB 处？因为内核的恒等映射正好占了 0~1GB（PD 的 512 个
 * 2MB 大页），1GB 之后的 PDPT 槽位是空的，天然适合划给用户空间。
 * 又因为 PML4 索引是 va >> 39，0x40000000 的 PML4 索引是 0，所以每个进程的
 * PML4 只有 [0] 这一项有用：它指向本进程自己的 PDPT，PDPT[0] 抄内核的
 * 恒等映射、PDPT[1] 指向本进程的 PD。这就是"内核映射共享、用户空间私有"。
 */

#define VMM_USER_BASE       0x0000000040000000ULL   /* 用户窗口起点：1GB 处，2MB 对齐 */
#define VMM_USER_SPAN       0x0000000000200000ULL   /* 窗口大小：2MB = 512 个 4KB 页 */
#define VMM_PAGE_SIZE       4096ULL
#define VMM_ENTRIES         512u

/* 一个进程的地址空间。四个 *_pa 只是把页表的物理地址留着，方便 `vmspace` 打印。 */
typedef struct {
    uint64_t cr3;        /* PML4 的物理地址，换进程时 mov cr3 */
    uint64_t pml4_pa;
    uint64_t pdpt_pa;
    uint64_t pd_pa;
    uint64_t pt_pa;
    uint64_t va_base;    /* 私有窗口的虚拟地址基点（本版固定 VMM_USER_BASE） */
    /* [ADD] M16：这一页"将来出现时应该长什么样"。
     * 0 = 不需要（既没映射、也没登记按需求）；
     * VMM_WANT_* 位 = 已登记为"按需页"，第一次访问时才真的分配一帧（零填充）。 */
    uint8_t desire[VMM_ENTRIES];
} vmm_space_t;

/* [M16] 按需页的"愿望位"：告诉 #PF 处理函数这一页该不该可写。 */
#define VMM_WANT_WRITE  0x01u   /* 出现时应为可写（.bss / 栈 / 匿名数据） */

/*
 * 新建一个地址空间：分配 PML4/PDPT/PD/PT 各一帧，把内核恒等映射接进去
 * （共享内核那张 2MB 大页 PD，U=0），再挂上自己的私有窗口。
 * 成功 0，失败 -1（帧不够）。
 */
int vmm_space_create(vmm_space_t *s, uint64_t va_base);

/*
 * 在私有窗口里映射一个 4KB 页：va -> pa（都必须 4KB 对齐，va 必须落在窗口内）。
 * 没调用过的页就是"洞里没有页"——用户一碰就是 #PF（error 位 P=0）。
 * 成功 0，失败 -1。
 */
int vmm_map_4k(vmm_space_t *s, uint64_t va, uint64_t pa, int writable);

/*
 * [M7] 克隆一个地址空间：新建 PML4/PDPT/PD/PT，把 src 私有窗口里**已映射的每一页**
 * 都分配一帧、整页拷贝、映射进 dst（页权限（可写位）照抄）。这就是 fork 需要的
 * "地址空间复制" —— 没有 COW，父子之间从这一刻起各有一份独立的物理页。
 * pages_out 回填复制了多少页（可为 0）。
 * 成功 0，失败 -1（帧不够，已分配的帧不回收，和本内核其它失败路径一致）。
 */
int vmm_space_clone(vmm_space_t *dst, const vmm_space_t *src,
                    uint32_t *pages_out);

/* 查 va 在本空间里的物理地址；没映射返回 0（本内核不用物理地址 0，够用）。 */
/* [ADD] M15：copy-on-write fork 的几件套。 */
int vmm_map_4k_cow(vmm_space_t *s, uint64_t va, uint64_t pa);
int vmm_cow_fault(uint64_t va, uint64_t err);
uint32_t vmm_cow_shared_count(void);
uint32_t vmm_cow_copy_count(void);
uint32_t vmm_cow_fault_count(void);
void vmm_cow_reset_stats(void);

/* [ADD] M16：按需分页（demand paging）。
 * vmm_space_want() 只登记"这一页将来要存在、可写"，**不分配帧**；
 * 用户第一次碰它时 #PF（P=0）→ vmm_page_fault() 才分配一帧并清零。
 * 装载时纯 .bss 的页走这条路：镜像里没有内容、只有"这一段是零"的事实。 */
int vmm_space_want(vmm_space_t *s, uint64_t va, uint64_t size, uint32_t flags);
uint32_t vmm_count_wanted(const vmm_space_t *s);   /* 登记了但还没出现的页数 */
uint32_t vmm_demand_fault_count(void);
uint32_t vmm_demand_page_count(void);

/* 当前任务的地址空间（由 kernel/task.c 实现，避免 vmm 反向依赖 task） */
int vmm_current_space(vmm_space_t *out);

/* [M16] 统一的缺页入口：M15 的 COW 分支 + 新的"按需分配"分支都在这里。 */
int vmm_page_fault(uint64_t va, uint64_t err);

uint64_t vmm_probe(const vmm_space_t *s, uint64_t va);

/*
 * [M12] 销毁一个地址空间：把私有窗口里**已映射的每一页**归还给页帧分配器，
 * 再把 PT/PD/PDPT/PML4 这四帧也还回去（内核共享的那个 PD 不动）。
 * pages_out 回填归还了多少个用户页（可为 NULL）。
 *
 * 调用前提（很重要）：CR3 里装的**不能**是这张表，否则刚释放的页表下一秒
 * 就被 CPU 当成当前页表用 —— 现象是下一行指令 #PF，CR2 等于你刚释放的地址。
 * 所以顺序永远是"先 mov cr3 换到新空间，再销毁旧的"。
 */
int vmm_space_destroy(vmm_space_t *s, uint32_t *pages_out);

/* 私有窗口里已经映射了多少个 4KB 页。 */
uint32_t vmm_count_mapped(const vmm_space_t *s);

/*
 * 软件走页表：在内核里手工按 PML4->PDPT->PD->PT 走一遍，返回 va 对应的物理地址；
 * 任何一级不存在都返回 0。用来在切换 CR3 之前自检"这张表到底对不对"。
 */
uint64_t vmm_walk(const vmm_space_t *s, uint64_t va);

/* 换 CR3。mov cr3 顺带把非全局 TLB 项全刷掉，所以不用额外 invlpg。 */
void vmm_load_cr3(uint64_t cr3);

/* 内核自己的 CR3（第一次调用时抓一次，之后一直用它回去）。 */
uint64_t vmm_kernel_cr3(void);

#endif /* ZH_OS_VMM_H */
