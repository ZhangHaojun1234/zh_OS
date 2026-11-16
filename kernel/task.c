/* task.c - 进程与调度：TCB 表、引导帧、时钟抢占、环形轮转、异常杀任务 */

#include "task.h"

#include "elf.h"   /* [ADD] M6 ELF 装载器：task_create_elf 用它把程序装进进程 */
#include "isr.h"
#include "paging.h"
#include "syscall.h"
#include "tss.h"
#include "vga.h"
#include "../mm/phys.h"
#include "../shell/shell.h"

/* [M7] fork 出来的子进程记父进程；内核直接造出来的任务用这个"没有父进程"标记。 */
#define TASK_NO_PARENT 0xFFFFFFFFu

/*
 * 到 M4，前面几块该拼起来了：M1 证明能降权进 ring3，M2 证明能陷回内核，M3 知道有多少内存。
 * 这里同时跑多个用户任务，各有各的内核栈，靠时钟强制换人。
 *
 * 切换到底切了什么，这是最容易想错的地方。不用像 Linux 的 switch_to 那样保存 callee-saved：
 * 从 ring3 陷进来时 CPU 已经按 TSS.RSP0 换到该任务的内核栈，isr_common 又把 15 个通用寄存器
 * 加 5 个 CPU 帧字段压成 struct regs。也就是说任务的全部现场此刻就躺在它自己的栈上，
 * 只要记住"这帧在哪个地址"（cur->frame），任务就冻住了；恢复是反过程：rsp 指回那帧、
 * pop 15 个寄存器、跳过 vector/error、iretq。
 * 所以本文件的保存动作只有一行 cur->frame = r，恢复动作是 zh_resume_frame(next->frame)
 * （在 task_switch.asm 里）。
 *
 * 三个坑记着：
 * 换人之前一定要写 TSS.RSP0 = 下一个任务的内核栈顶。忘了的话，下一个任务从 ring3 再陷进来
 * 会用上一个任务的栈，两个任务的帧叠在同一块栈上，跑几十个 tick 必然三重故障，
 * 现象是随机重启，从现象反推原因基本没戏。
 * 任务栈顶要 16 字节对齐（TASK_KSTACK_BYTES 是 8K，天然对齐），否则第一次 call 进 C 就 #GP，
 * movaps 这类指令要求对齐。
 * 帧地址必须是最后一次被换出时的那个 r，不能自己算。任务在 ring0 里被抢占（比如正在
 * sys_sleep 的 hlt 循环里）时，帧压在该任务栈的当前位置，跟"栈顶 - sizeof(struct regs)"并不总相等。
 */

#define VGA_MARKER_ADDR (0xB8000u + (24u * 80u + 26u) * 2u)  /* 第 24 行第 26 列 */
#define TASK_PROG_CAP   4096u                                /* 用户程序上限 */

/* 帧布局必须与 isr_common 的压栈顺序、struct regs 的字段顺序三方一致。 */
typedef char task_frame_size_check[(sizeof(struct regs) == 22u * 8u) ? 1 : -1];

static task_t g_tasks[TASK_MAX];
static uint8_t *g_user_pool;      /* TASK_MAX 个连续的 2MB 窗口 */
static int g_current = -1;        /* 正在跑的任务下标；-1 = 内核（Shell）在跑 */
static int g_inited;
static int g_active;              /* 调度器是否在接管 CPU */
static uint64_t g_quantum_left;
static uint64_t g_switches;
static uint32_t g_created;

/* 小工具                                                             */

static void show_hex64(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char buf[19];

    buf[0] = '0';
    buf[1] = 'x';
    for (uint32_t i = 0u; i < 16u; ++i) {
        buf[2u + i] = digits[(value >> (60u - 4u * i)) & 0xFu];
    }
    buf[18] = '\0';
    sh_print(SH_PLAIN, buf);
}

static void show_field(const char *s, uint32_t width)
{
    uint32_t n = 0u;

    while (s[n] != '\0') {
        ++n;
    }
    sh_print(SH_PLAIN, s);
    while (n < width) {
        sh_print(SH_PLAIN, " ");
        ++n;
    }
}

static const char *state_name(uint32_t state)
{
    switch (state) {
    case TASK_READY:   return "READY";
    case TASK_RUNNING: return "RUNNING";
    case TASK_DEAD:    return "DEAD";
    case TASK_WAIT:    return "WAIT";      /* [M7] 阻塞在 wait() */
    case TASK_FREE:
    default:           return "FREE";
    }
}

/* 引导帧：给一个"还没跑过"的任务造出它第一次被 iretq 时的现场        */

/*
 * 帧放在该任务内核栈的最顶端（栈顶减 176）。任务第一次被恢复时 rsp 正好落在栈顶下方
 * 176 字节，跟它在 ring3 被时钟打断时的栈状态完全一样，后面每个 tick 的现场自然接着
 * 往下长，不用加特殊分支。
 */
static uint64_t make_boot_frame(const task_t *t, uint64_t entry, uint64_t user_sp)
{
    struct regs *f = (struct regs *)(uintptr_t)(t->kstack_top - (uint64_t)sizeof(struct regs));
    uint64_t *raw = (uint64_t *)(void *)f;
    uint32_t i;

    for (i = 0u; i < (uint32_t)(sizeof(struct regs) / 8u); ++i) {
        raw[i] = 0u;
    }

    /* 15 个通用寄存器全 0：用户程序不依赖任何初始寄存器值 */
    f->vector = 0u;                       /* 占位：真正的向量号只在真实中断里有意义 */
    f->error_code = 0u;                    /* 占位 */
    f->rip = entry;                        /* 用户代码入口 */
    f->cs = GDT_USER_CODE_RPL3;            /* 0x1b ， 关键：RPL=3 才会降权 */
    f->rflags = 0x202u;                    /* IF=1：用户态也能被时钟打断，否则永远不会被抢占 */
    f->rsp = user_sp;                      /* 用户栈顶 */
    f->ss = GDT_USER_DATA_RPL3;            /* 0x23 */

    return (uint64_t)(uintptr_t)f;
}

/* 页表：把任务窗口所在的 2MB 大页对 ring3 开放                       */

void task_allow_user_window(uint8_t *win)
{
    const uint64_t phys_mask = 0x000FFFFFFFFFF000ULL;
    const uint64_t cr3 = paging_get_cr3() & phys_mask;
    const uint64_t va = (uint64_t)(uintptr_t)win;

    uint64_t *pml4 = (uint64_t *)(uintptr_t)cr3;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[0] & phys_mask);
    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[0] & phys_mask);
    uint64_t *leaf = &pd[(va >> 21) & 0x1FFu];

    /* 四级都要 U=1：任何一级 U=0，ring3 取指就 #PF（M1 踩过的老坑）。 */
    pml4[0] |= PAGE_USER;
    pdpt[0] |= PAGE_USER;
    *leaf |= PAGE_USER | PAGE_WRITABLE;

    /* 改完页表重载 CR3 刷 TLB，否则旧缓存让改动"看起来没生效"。 */
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

/* 初始化 / 复位 / 建任务                                             */

int task_init(void)
{
    uint32_t i;

    if (g_inited) {
        return 0;
    }

    /* 一次要 TASK_MAX×2MB，且首地址 2MB 对齐：正好凑成连续的大页。 */
    g_user_pool = (uint8_t *)phys_alloc_frames(
        (uint64_t)TASK_MAX * (TASK_USER_WINDOW / PHYS_PAGE_SIZE), TASK_USER_WINDOW);
    if (g_user_pool == (uint8_t *)0) {
        err("task: cannot allocate 8 MB of 2 MB-aligned user windows");
        plain("  run 'memmap' to see what the frame allocator has left");
        return -1;
    }

    for (i = 0u; i < TASK_MAX; ++i) {
        uint64_t *sp = (uint64_t *)(void *)&g_tasks[i].space;
        uint32_t k;

        g_tasks[i].pid = 0u;
        g_tasks[i].state = TASK_FREE;
        g_tasks[i].name = "";
        g_tasks[i].kstack_base = 0u;
        g_tasks[i].kstack_top = 0u;
        g_tasks[i].frame = 0u;
        g_tasks[i].user_base = 0u;
        g_tasks[i].user_sp = 0u;
        g_tasks[i].entry = 0u;
        g_tasks[i].ticks = 0u;
        g_tasks[i].switches = 0u;
        g_tasks[i].exit_code = 0;
        g_tasks[i].fault_vector = 0;
        g_tasks[i].fault_addr = 0u;
        g_tasks[i].fault_rip = 0u;
        g_tasks[i].fault_error = 0u;
        /* [M5] 独立地址空间相关字段 */
        g_tasks[i].cr3 = 0u;
        g_tasks[i].code_pa = 0u;
        g_tasks[i].stack_pa = 0u;
        g_tasks[i].space_ready = 0u;
        for (k = 0u; k < (uint32_t)(sizeof(vmm_space_t) / 8u); ++k) {
            sp[k] = 0u;
        }
    }

    /* 记住内核自己的 CR3：所有任务结束后要靠它回到 Shell 的地址空间。 */
    (void)vmm_kernel_cr3();

    g_inited = 1;
    return 0;
}

/*
 * [M12] 回收一套地址空间并记账。只能在 CR3 已经不在它上面的时候调用，
 * 前置条件见 vmm_space_destroy。
 */
/* [ADD] M12：地址空间回收统计 ， 回收了几套 mm、几个用户页 */
static uint32_t g_reclaim_spaces;
static int g_exec_verbose = 1;  /* [ADD] M12：exec 日志开关 */
static uint32_t g_reclaim_pages;

static void reclaim_space(vmm_space_t *s)
{
    uint32_t pages = 0u;

    if (s == (vmm_space_t *)0 || s->cr3 == 0u) {
        return;
    }
    if (vmm_space_destroy(s, &pages) != 0) {
        return;
    }
    ++g_reclaim_spaces;
    g_reclaim_pages += pages;
}

void task_reset(void)
{
    uint32_t i;

    /*
     * [M12] 先把上一轮留下的地址空间全部还给页帧分配器。此刻 CR3 一定是内核自己的
     * （task_run_all 返回时已经换回来了），所以销毁谁的都是安全的。
     * 命令与命令之间不再漏页表：以前连跑 20 次 elf exec，页帧要少掉上百。
     */
    for (i = 0u; i < TASK_MAX; ++i) {
        if (g_tasks[i].state != TASK_FREE) {
            reclaim_space(&g_tasks[i].space);
            g_tasks[i].cr3 = 0u;
            g_tasks[i].space_ready = 0u;
        }
    }

    /* 只复位"运行态"，保留已分配的用户窗口与内核栈：重复 taskrun 不会漏内存。 */
    for (i = 0u; i < TASK_MAX; ++i) {
        if (g_tasks[i].state != TASK_FREE) {
            g_tasks[i].state = TASK_FREE;
        }
        g_tasks[i].ticks = 0u;
        g_tasks[i].switches = 0u;
        g_tasks[i].exit_code = 0;
        g_tasks[i].fault_vector = 0;
        g_tasks[i].fault_addr = 0u;
        g_tasks[i].fault_rip = 0u;
    }
    g_current = -1;
    g_active = 0;
    g_quantum_left = 0u;
    g_switches = 0u;
    g_created = 0u;
}

uint8_t *task_user_window(uint32_t pid)
{
    if (pid >= TASK_MAX || g_user_pool == (uint8_t *)0) {
        return (uint8_t *)0;
    }
    return g_user_pool + (uint64_t)pid * TASK_USER_WINDOW;
}

int task_create(const char *name, uint64_t entry, uint64_t user_sp)
{
    uint32_t i;
    task_t *t;
    uint8_t *ks;

    if (!g_inited && task_init() != 0) {
        return -1;
    }

    for (i = 0u; i < TASK_MAX; ++i) {
        if (g_tasks[i].state == TASK_FREE) {
            break;
        }
    }
    if (i == TASK_MAX) {
        err("task: no free TCB slot");
        return -1;
    }

    t = &g_tasks[i];

    /* 内核栈：每个任务一块自己的 8KB 栈，换人时写进 TSS.RSP0 */
    if (t->kstack_base == 0u) {
        ks = (uint8_t *)phys_alloc_frames(TASK_KSTACK_BYTES / PHYS_PAGE_SIZE, 16u);
        if (ks == (uint8_t *)0) {
            err("task: no frames left for a kernel stack");
            return -1;
        }
        t->kstack_base = (uint64_t)(uintptr_t)ks;
        t->kstack_top = t->kstack_base + TASK_KSTACK_BYTES;
    }

    t->pid = i;
    t->parent_pid = TASK_NO_PARENT;   /* [M7] 内核造出来的任务没有父进程 */
    t->has_wait_code = 0u;
    file_table_init(t->fds, ZH_MAX_FDS);   /* [M9] 新进程没有任何打开的文件 */
    t->name = name;
    t->state = TASK_READY;
    t->user_base = (uint64_t)(uintptr_t)task_user_window(i);
    t->user_sp = user_sp;
    t->entry = entry;
    t->ticks = 0u;
    t->switches = 0u;
    t->exit_code = 0;
    t->fault_vector = 0;
    t->fault_addr = 0u;
    t->fault_rip = 0u;
    t->frame = make_boot_frame(t, entry, user_sp);

    ++g_created;
    return (int)t->pid;   /* 不能 return i：上面的 sig_handler 循环已经把 i 跑成了 32 */
}

/* [M5] 带独立地址空间的任务                                          */

/*
 * 跟老的 task_create 只差一处：用户的代码页、栈页不再来自整块 2MB 窗口，
 * 而是各自一帧 4KB，映射到自己那张页表里的固定虚拟地址 VMM_USER_BASE。
 * 于是两个进程的虚拟地址完全一样（都是 0x40000000），物理页却各不相同，
 * 这就是进程隔离：一个进程看到的地址跟另一个无关。
 */
int task_create_va(const char *name)
{
    uint32_t i;
    task_t *t;
    uint8_t *ks;
    uint64_t code_pa;
    uint64_t stack_pa;

    if (!g_inited && task_init() != 0) {
        return -1;
    }

    for (i = 0u; i < TASK_MAX; ++i) {
        if (g_tasks[i].state == TASK_FREE) {
            break;
        }
    }
    if (i == TASK_MAX) {
        err("task: no free TCB slot");
        return -1;
    }

    t = &g_tasks[i];

    /* 内核栈：每个任务一块自己的 8KB 栈，换人时写进 TSS.RSP0 */
    if (t->kstack_base == 0u) {
        ks = (uint8_t *)phys_alloc_frames(TASK_KSTACK_BYTES / PHYS_PAGE_SIZE, 16u);
        if (ks == (uint8_t *)0) {
            err("task: no frames left for a kernel stack");
            return -1;
        }
        t->kstack_base = (uint64_t)(uintptr_t)ks;
        t->kstack_top = t->kstack_base + TASK_KSTACK_BYTES;
    }

    /* 地址空间：PML4 + PDPT + PD + PT 各一帧，再加代码页、栈页 = 6 帧 */
    if (!t->space_ready) {
        if (vmm_space_create(&t->space, VMM_USER_BASE) != 0) {
            err("task: no frames left for a 4-level page table");
            return -1;
        }

        code_pa = (uint64_t)(uintptr_t)phys_alloc_frame();
        stack_pa = (uint64_t)(uintptr_t)phys_alloc_frame();
        if (code_pa == 0u || stack_pa == 0u) {
            err("task: no frames left for the user code/stack pages");
            return -1;
        }

        /* 只映射两页：代码页（窗口第一个 4KB）、栈页（窗口最后一页）。
         * 中间 510 页故意留空，用户碰一下就 #PF（error 的 P 位为 0）。 */
        if (vmm_map_4k(&t->space, VMM_USER_BASE, code_pa, 1) != 0 ||
            vmm_map_4k(&t->space,
                       VMM_USER_BASE + VMM_USER_SPAN - VMM_PAGE_SIZE,
                       stack_pa, 1) != 0) {
            err("task: mapping the user pages failed");
            return -1;
        }

        t->code_pa = code_pa;
        t->stack_pa = stack_pa;
        t->space_ready = 1u;
    }

    t->pid = i;
    t->parent_pid = TASK_NO_PARENT;   /* [M7] 内核造出来的任务没有父进程 */
    t->has_wait_code = 0u;
    file_table_init(t->fds, ZH_MAX_FDS);   /* [M9] 新进程没有任何打开的文件 */
    t->name = name;
    t->state = TASK_READY;
    t->cr3 = t->space.cr3;
    t->user_base = VMM_USER_BASE;
    /* [M11] 栈顶往下留 8 字节，让 _start 入口处 rsp ≡ 8 (mod 16)。gcc 认为函数是被
     * call 进来的（入口 rsp+8 对齐），iretq 不会替我们压返回地址；对不上，
     * movaps %xmm0,(%rsp) 就成 #GP（vector 13）。 */
    t->user_sp = VMM_USER_BASE + VMM_USER_SPAN - 8u;
    t->entry = VMM_USER_BASE;
    t->ticks = 0u;
    t->switches = 0u;
    t->exit_code = 0;
    t->fault_vector = 0;
    t->fault_addr = 0u;
    t->fault_rip = 0u;
    t->fault_error = 0u;
    t->sig_pending = 0u;   /* [ADD] M17 */
    t->term_sig = 0u;      /* [ADD] M17 */
    t->sig_mask = 0u;      /* [ADD] M17 */
    t->foreground = 0u;    /* [ADD] M17 */
    t->sig_in_handler = 0u;                /* [ADD] M18 */
    t->sig_handling = 0u;                  /* [ADD] M18 */
    t->tramp_pa = 0u;                      /* [ADD] M18：蹦床页延迟到第一次送达时才建 */
    for (i = 0u; i < ZH_SIG_MAX; ++i) {     /* [ADD] M18：新进程没有任何 handler */
        t->sig_handler[i] = 0u;
    }
    t->frame = make_boot_frame(t, t->entry, t->user_sp);

    ++g_created;
    return (int)t->pid;   /* 不能 return i：上面的 sig_handler 循环已经把 i 跑成了 32 */
}

/*
 * [ADD] M6：按 ELF 文件建进程。跟 task_create_va 只差一处，但这一处最要紧：代码页不再由内核
 * 往里铺机器码，交给 elf_load_into() 按程序头逐段装进来。页数、内容、入口地址全来自那个外部
 * 编译出来的文件，内核从造程序变成装程序。
 */
int task_create_elf(const char *name, const uint8_t *blob, uint64_t size,
                    elf_image_t *img)
{
    uint32_t i;
    task_t *t;
    uint8_t *ks;
    uint64_t stack_pa;

    if (!g_inited && task_init() != 0) {
        return -1;
    }

    for (i = 0u; i < TASK_MAX; ++i) {
        if (g_tasks[i].state == TASK_FREE) {
            break;
        }
    }
    if (i == TASK_MAX) {
        err("task: no free TCB slot");
        return -1;
    }

    t = &g_tasks[i];

    if (t->kstack_base == 0u) {
        ks = (uint8_t *)phys_alloc_frames(TASK_KSTACK_BYTES / PHYS_PAGE_SIZE, 16u);
        if (ks == (uint8_t *)0) {
            err("task: no frames left for a kernel stack");
            return -1;
        }
        t->kstack_base = (uint64_t)(uintptr_t)ks;
        t->kstack_top = t->kstack_base + TASK_KSTACK_BYTES;
    }

    if (!t->space_ready) {
        if (vmm_space_create(&t->space, VMM_USER_BASE) != 0) {
            err("task: no frames left for a 4-level page table");
            return -1;
        }

        /* 只先给用户栈一页；代码页由 ELF 装载器按需分配（这正是按段按页装载）。 */
        stack_pa = (uint64_t)(uintptr_t)phys_alloc_frame();
        if (stack_pa == 0u) {
            err("task: no frames left for the user stack page");
            return -1;
        }
        if (vmm_map_4k(&t->space,
                       VMM_USER_BASE + VMM_USER_SPAN - VMM_PAGE_SIZE,
                       stack_pa, 1) != 0) {
            err("task: mapping the user stack page failed");
            return -1;
        }

        t->stack_pa = stack_pa;
        t->space_ready = 1u;
    }

    /* 真正的装载：段 -> 页 -> 帧 -> 拷贝 -> 清 .bss */
    if (elf_load_into(&t->space, blob, size, img) != 0) {
        err("task: ELF load failed");
        plain(elf_last_error());
        t->state = TASK_FREE;
        return -1;
    }

    t->pid = i;
    t->parent_pid = TASK_NO_PARENT;   /* [M7] 内核造出来的任务没有父进程 */
    t->has_wait_code = 0u;
    file_table_init(t->fds, ZH_MAX_FDS);   /* [M9] 新进程没有任何打开的文件 */
    t->name = name;
    t->state = TASK_READY;
    t->cr3 = t->space.cr3;
    t->user_base = VMM_USER_BASE;
    /* [M11] 栈顶往下留 8 字节，让 _start 入口处 rsp ≡ 8 (mod 16)。gcc 认为函数是被
     * call 进来的（入口 rsp+8 对齐），iretq 不会替我们压返回地址；对不上，
     * movaps %xmm0,(%rsp) 就成 #GP（vector 13）。 */
    t->user_sp = VMM_USER_BASE + VMM_USER_SPAN - 8u;
    t->entry = img->entry;
    t->code_pa = vmm_probe(&t->space, img->entry & ~0xFFFULL);
    t->ticks = 0u;
    t->switches = 0u;
    t->exit_code = 0;
    t->fault_vector = 0;
    t->fault_addr = 0u;
    t->fault_rip = 0u;
    t->fault_error = 0u;
    t->sig_pending = 0u;   /* [ADD] M17 */
    t->term_sig = 0u;      /* [ADD] M17 */
    t->sig_mask = 0u;      /* [ADD] M17 */
    t->foreground = 0u;    /* [ADD] M17 */
    t->sig_in_handler = 0u;                /* [ADD] M18 */
    t->sig_handling = 0u;                  /* [ADD] M18 */
    t->tramp_pa = 0u;                      /* [ADD] M18：蹦床页延迟到第一次送达时才建 */
    for (i = 0u; i < ZH_SIG_MAX; ++i) {     /* [ADD] M18：新进程没有任何 handler */
        t->sig_handler[i] = 0u;
    }
    t->frame = make_boot_frame(t, t->entry, t->user_sp);

    ++g_created;
    return (int)t->pid;   /* 不能 return i：上面的 sig_handler 循环已经把 i 跑成了 32 */
}

/* [M7] fork / wait：进程第一次能"生"出自己                          */


static void schedule_away(void) __attribute__((noreturn));   /* 定义在下面的调度器段 */

static uint32_t g_fork_count;
static uint32_t g_wait_count;
static uint32_t g_exec_count;   /* [M8] */


uint32_t task_current_pid(void)
{
    if (!g_active || g_current < 0 || g_current >= (int)TASK_MAX) {
        return TASK_NO_PARENT;
    }
    return (uint32_t)g_current;
}

uint32_t task_fork_count(void)
{
    return g_fork_count;
}

uint32_t task_wait_count(void)
{
    return g_wait_count;
}

void task_set_exec_verbose(int on)
{
    g_exec_verbose = (on != 0) ? 1 : 0;
}

uint32_t task_reclaim_space_count(void)
{
    return g_reclaim_spaces;
}

uint32_t task_reclaim_page_count(void)
{
    return g_reclaim_pages;
}

uint32_t task_exec_count(void)
{
    return g_exec_count;
}

zfile_t *task_current_fds(void)
{
    if (!g_active || g_current < 0 || g_current >= (int)TASK_MAX) {
        return (zfile_t *)0;
    }
    return g_tasks[g_current].fds;
}

int task_fork_current(struct regs *r)
{
    task_t *parent;
    task_t *child;
    struct regs *cframe;
    uint8_t *ks;
    uint32_t i;
    uint32_t pages = 0u;

    if (!g_active || g_current < 0 || g_current >= (int)TASK_MAX ||
        r == (struct regs *)0) {
        err("fork: no current task");
        return -1;
    }
    parent = &g_tasks[g_current];
    if (!parent->space_ready) {
        err("fork: the parent has no private address space");
        return -1;
    }

    for (i = 0u; i < TASK_MAX; ++i) {
        if (g_tasks[i].state == TASK_FREE) {
            break;
        }
    }
    if (i == TASK_MAX) {
        err("fork: no free TCB slot (TASK_MAX is 4)");
        return -1;
    }
    child = &g_tasks[i];

    if (child->kstack_base == 0u) {
        ks = (uint8_t *)phys_alloc_frames(TASK_KSTACK_BYTES / PHYS_PAGE_SIZE, 16u);
        if (ks == (uint8_t *)0) {
            err("fork: no frames left for a kernel stack");
            return -1;
        }
        child->kstack_base = (uint64_t)(uintptr_t)ks;
        child->kstack_top = child->kstack_base + TASK_KSTACK_BYTES;
    }

    /* 1) 复制地址空间（整页拷贝，不用 COW） */
    if (vmm_space_clone(&child->space, &parent->space, &pages) != 0) {
        err("fork: could not clone the address space");
        return -1;
    }
    child->space_ready = 1u;
    child->cr3 = child->space.cr3;
    child->user_base = parent->user_base;
    child->user_sp = parent->user_sp;
    child->entry = parent->entry;
    child->code_pa = vmm_probe(&child->space, parent->entry & ~0xFFFULL);
    child->stack_pa = vmm_probe(&child->space,
                                VMM_USER_BASE + VMM_USER_SPAN - VMM_PAGE_SIZE);

    /*
     * 子进程的现场直接复制父进程此刻的中断帧，只把 rax 改成 0。这是 fork 的关键：
     * 父子拿的是同一份寄存器快照，所以两边都从 int 0x80 的下一条继续，区别只有返回值
     * （父=子进程 pid，子=0）。Linux 的 copy_thread 也是复制 pt_regs 再把 ax 置 0。
     */
    cframe = (struct regs *)(uintptr_t)(child->kstack_top - sizeof(struct regs));
    *cframe = *r;
    cframe->rax = 0u;
    child->frame = (uint64_t)(uintptr_t)cframe;

    child->pid = i;
    child->name = parent->name;
    child->state = TASK_READY;
    child->ticks = 0u;
    child->switches = 0u;
    child->exit_code = 0;
    child->fault_vector = 0;
    child->fault_addr = 0u;
    child->fault_rip = 0u;
    child->fault_error = 0u;
    child->parent_pid = parent->pid;
    child->has_wait_code = 0u;
    /* [M9] 子进程继承父进程的 fd 表（Linux：fork 之后父子共享同一批打开的文件） */
    file_table_clone(child->fds, parent->fds, ZH_MAX_FDS);

    /* [ADD] M18：信号相关状态。handler 表继承，Linux 的 fork 也一样，除非 handler 是
     * exec 之后装的；pending 清空，没送出去的信号是投给父进程的，不该跟着复制；
     * sig_frame / in_handler 清零，子进程此刻不在 handler 里。 */
    for (i = 0u; i < ZH_SIG_MAX; ++i) {
        child->sig_handler[i] = parent->sig_handler[i];
    }
    i = (uint32_t)child->pid;             /* 上面那个循环用掉了 i，这里必须还原成子进程 pid */
    child->sig_pending = 0u;
    child->sig_mask = parent->sig_mask;
    child->term_sig = 0u;
    child->foreground = 0u;
    child->sig_in_handler = 0u;
    child->sig_handling = 0u;
    child->tramp_pa = vmm_probe(&child->space,
                                VMM_USER_BASE + VMM_USER_SPAN - 2u * VMM_PAGE_SIZE);

    ++g_created;
    ++g_fork_count;

    if (g_exec_verbose) {          /* [M14] fork 的日志也走同一个开关 */
        sh_print(SH_PLAIN, "[kernel] fork: pid ");
        sh_print_u32(SH_PLAIN, parent->pid);
        sh_print(SH_PLAIN, " -> pid ");
        sh_print_u32(SH_PLAIN, i);
        sh_print(SH_PLAIN, "  copied pages=");
        sh_print_u32(SH_PLAIN, pages);
        sh_print(SH_PLAIN, "  child cr3=");
        show_hex64(child->cr3);
        sh_newline();
    }

    return (int)child->pid;   /* [ADD] M18：同理，不能用被 handler 循环/日志用过的 i */
 }

int task_wait_current(struct regs *r)
{
    task_t *cur;
    uint32_t i;

    if (!g_active || g_current < 0 || g_current >= (int)TASK_MAX) {
        return -1;
    }
    cur = &g_tasks[g_current];

    /* 1) 已经有僵尸子进程：直接把退出码交回去，槽位复用 */
    for (i = 0u; i < TASK_MAX; ++i) {
        task_t *c = &g_tasks[i];
        if (c->state == TASK_DEAD && c->parent_pid == cur->pid &&
            c->has_wait_code != 0u) {
            int code = c->exit_code;
            c->has_wait_code = 0u;
            file_table_init(c->fds, ZH_MAX_FDS);   /* [M9] */
            c->state = TASK_FREE;      /* 收尸：槽位释放（它那几帧不回收） */
            ++g_wait_count;
            if (g_exec_verbose) {      /* [M14] 与 fork/exec 同一个开关 */
                sh_print(SH_PLAIN, "[kernel] wait: pid ");
                sh_print_u32(SH_PLAIN, cur->pid);
                sh_print(SH_PLAIN, " collected exit code ");
                sh_print_u32(SH_PLAIN, (uint32_t)(code < 0 ? 0 : code));
                sh_print(SH_PLAIN, " from pid ");
                sh_print_u32(SH_PLAIN, i);
                sh_newline();
            }
            return code;
        }
    }

    /* 2) 还有活着的子进程：睡下，等子进程退出时把自己唤醒 */
    for (i = 0u; i < TASK_MAX; ++i) {
        const task_t *c = &g_tasks[i];
        if (c->parent_pid == cur->pid && c->state != TASK_FREE &&
            c->state != TASK_DEAD) {
            if (g_exec_verbose) {      /* [M14] */
                sh_print(SH_PLAIN, "[kernel] wait: pid ");
                sh_print_u32(SH_PLAIN, cur->pid);
                sh_print(SH_PLAIN, " blocks until pid ");
                sh_print_u32(SH_PLAIN, i);
                sh_println(SH_PLAIN, " exits");
            }
            /*
             * 存现场、标 TASK_WAIT、换人。返回码不在这儿写：子进程退出时会把 code 写进
             * ((struct regs *)cur->frame)->rax，父进程被换回来时 iretq 用的就是这个
             * 带返回值的帧。
             */
            cur->frame = (uint64_t)(uintptr_t)r;
            cur->state = TASK_WAIT;
            schedule_away();
            return -1;                 /* 不可达 */
        }
    }

    return -1;                         /* 没有子进程 */
}

int task_exec_current(struct regs *r, const uint8_t *blob, uint64_t size,
                      elf_image_t *img)
{
    /* [M11] 老接口（无参数）直接转调带参数的版本，argv=NULL / argc=0 */
    return task_exec_current_argv(r, blob, size, img, (char *const *)0, 0u);
}

int task_exec_current_argv(struct regs *r, const uint8_t *blob, uint64_t size,
                           elf_image_t *img, char *const *argv, uint32_t argc)
{
    task_t *t;
    vmm_space_t ns;
    vmm_space_t old_space = { 0u, 0u, 0u, 0u, 0u, 0u, { 0u } };  /* [M12] 旧空间 */
    uint64_t stack_pa;
    uint64_t argv_off[16];      /* [M11] 每个参数字符串在栈页里的偏移 */
    uint64_t arg_rsi = 0u;      /* [M11] 指针数组的虚拟地址（没有参数就是 0） */
    uint32_t argc_used = 0u;    /* [M11] 真正铺进去的参数个数 */
    uint32_t ai;
    uint64_t aj;

    if (!g_active || g_current < 0 || g_current >= (int)TASK_MAX ||
        r == (struct regs *)0) {
        err("exec: no current task");
        return -1;
    }
    t = &g_tasks[g_current];

    /* 1) 建一个新地址空间，装 ELF，铺一页新栈 */
    if (vmm_space_create(&ns, VMM_USER_BASE) != 0) {
        err("exec: no frames left for a page table");
        return -1;
    }
    stack_pa = (uint64_t)(uintptr_t)phys_alloc_frame();
    if (stack_pa == 0u) {
        err("exec: no frames left for the stack");
        return -1;
    }
    if (vmm_map_4k(&ns, VMM_USER_BASE + VMM_USER_SPAN - VMM_PAGE_SIZE,
                   stack_pa, 1) != 0) {
        err("exec: mapping the stack failed");
        return -1;
    }
    if (elf_load_into(&ns, blob, size, img) != 0) {
        err("exec: ELF load failed");
        plain(elf_last_error());
        return -1;
    }

    /* 2) 原地换映像。[M12] 旧的那套 mm 现在会真的被回收 ， 顺序不能反：
     *    必须先把 CR3 换到新空间，再销毁旧的（否则刚释放的页表立刻被 CPU 用）。 */
    old_space = t->space;
    t->space = ns;
    t->cr3 = ns.cr3;
    t->space_ready = 1u;
    t->entry = img->entry;
    t->code_pa = vmm_probe(&t->space, t->entry & ~0xFFFULL);
    t->stack_pa = stack_pa;
    /* [M11] 栈顶往下留 8 字节，让 _start 入口处 rsp ≡ 8 (mod 16)。gcc 认为函数是被
     * call 进来的（入口 rsp+8 对齐），iretq 不会替我们压返回地址；对不上，
     * movaps %xmm0,(%rsp) 就成 #GP（vector 13）。 */
    t->user_sp = VMM_USER_BASE + VMM_USER_SPAN - 8u;

    /*
     * [M11] 把 argv 铺进新栈页底部。参数字符串不能留在旧地址空间（那块 mm 马上要丢掉），
     * 必须写进新栈页。页帧恒等映射，所以直接用物理地址写，但写指针用的是虚拟地址
     * （stack_va + offset），因为程序看到的是 VA。
     * 一页 4KB 的布局：[0, 字符串池) 每个参数一个 NUL 结尾的串；
     * [16 对齐处, 8*(argc+1)) 指针数组，最后一个补 NULL。
     * 最后 rdi=argc、rsi=&argv[0] 交给 _start，老程序不理会这两个寄存器也没事。
     */
    if (argc != 0u && argv != (char *const *)0) {
        uint8_t *sp_mem = (uint8_t *)(uintptr_t)stack_pa;   /* 恒等映射，可直接写 */
        const uint64_t stack_va =
            VMM_USER_BASE + VMM_USER_SPAN - VMM_PAGE_SIZE;
        uint64_t str_off = 0u;
        uint64_t parr_off;

        if (argc > 16u) {
            argc = 16u;   /* 与 argv_off[] 一样大 */
        }
        for (ai = 0u; ai < argc; ++ai) {
            const char *s = argv[ai];
            uint64_t len = 0u;

            if (s == (const char *)0) {
                break;
            }
            while (s[len] != '\0' && len < 96u) {
                ++len;
            }
            if (str_off + len + 1u > (VMM_PAGE_SIZE / 2u)) {
                break;      /* 参数太多太长，到这儿为止 */
            }
            for (aj = 0u; aj < len; ++aj) {
                sp_mem[str_off + aj] = (uint8_t)s[aj];
            }
            sp_mem[str_off + len] = '\0';
            argv_off[argc_used] = str_off;
            str_off += len + 1u;
            ++argc_used;
        }
        parr_off = (str_off + 15u) & ~15u;   /* 指针数组 16 字节对齐 */
        for (ai = 0u; ai < argc_used; ++ai) {
            *(uint64_t *)(uintptr_t)(sp_mem + parr_off + (uint64_t)ai * 8u) =
                stack_va + argv_off[ai];
        }
        *(uint64_t *)(uintptr_t)(sp_mem + parr_off + (uint64_t)argc_used * 8u) =
            0u;   /* argv[argc] == NULL，与 C 标准一致 */
        arg_rsi = stack_va + parr_off;
    }

    /* 3) 改中断帧：这就是 exec 的全部"魔法" ， 返回路径被换成新入口 */
    r->rip = t->entry;
    r->rsp = t->user_sp;
    r->rax = 0u;
    r->rbp = 0u;
    r->rbx = 0u;
    r->rcx = 0u;
    /* [M11] rdi = argc、rsi = &argv[0]：新程序的 _start(argc, argv) 就从这里取 */
    r->rdi = (uint64_t)argc_used;
    r->rsi = arg_rsi;
    r->rdx = 0u;
    r->r8 = 0u;
    r->r9 = 0u;
    r->r10 = 0u;
    r->r11 = 0u;
    r->r12 = 0u;
    r->r13 = 0u;
    r->r14 = 0u;
    r->r15 = 0u;

    /* 4) 换 CR3：新空间的 PDPT[0] 抄的仍是内核那份恒等映射，
     *    所以本函数所在的内核代码、以及当前任务的内核栈，换完照旧可访问。 */
    vmm_load_cr3(t->cr3);

    /* [M12] 到这一行，CR3 已经站在新空间上，旧空间彻底没人用了 ， 还给分配器。
     * 这一步以前是缺的：每 exec 一次漏 4 张页表 + 1 栈页 + N 代码页。 */
    if (old_space.cr3 != 0u) {
        reclaim_space(&old_space);
    }

    ++g_exec_count;

    if (g_exec_verbose) {
        sh_print(SH_PLAIN, "[kernel] exec: pid ");
        sh_print_u32(SH_PLAIN, t->pid);
        sh_println(SH_PLAIN, " image replaced (fresh address space)");
        sh_print(SH_PLAIN, "[kernel] exec: entry=");
        show_hex64(t->entry);
        sh_print(SH_PLAIN, " cr3=");
        show_hex64(t->cr3);
        sh_newline();
    }

    return 0;
}

uint8_t *task_code_phys(uint32_t pid)
{
    if (pid >= TASK_MAX || !g_tasks[pid].space_ready) {
        return (uint8_t *)0;
    }
    return (uint8_t *)(uintptr_t)g_tasks[pid].code_pa;
}

uint64_t task_va_base(uint32_t pid)
{
    if (pid >= TASK_MAX || !g_tasks[pid].space_ready) {
        return 0u;
    }
    return g_tasks[pid].user_base;
}

uint64_t task_cr3(uint32_t pid)
{
    if (pid >= TASK_MAX) {
        return 0u;
    }
    return g_tasks[pid].cr3;
}

/*
 * [ADD] M15：把"当前任务"的地址空间交给调用方。
 * 放 task.c 不放 vmm.c，因为 vmm 不该认识调度器：vmm 只管页表，"现在 CPU 用的是哪张"
 * 是任务层的事。所以实现在这里，声明留在 vmm.h，vmm_cow_fault() 只管调用，不反向依赖 task.h。
 */
int vmm_current_space(vmm_space_t *out)
{
    if (out == (vmm_space_t *)0) {
        return -1;
    }
    if (g_current < 0 || g_current >= (int)TASK_MAX) {
        return -1;
    }
    if (!g_tasks[g_current].space_ready || g_tasks[g_current].space.pt_pa == 0u) {
        return -1;
    }
    *out = g_tasks[g_current].space;
    return 0;
}

uint64_t task_user_window_current(void)
{
    if (g_current < 0 || g_current >= (int)TASK_MAX) {
        return 0u;
    }
    if (!g_tasks[g_current].space_ready) {
        return 0u;             /* 老路径（M1 那种共用页表的窗口）不走这里 */
    }
    return g_tasks[g_current].user_base;
}

const task_t *task_lookup(uint32_t pid)
{
    if (pid >= TASK_MAX) {
        return (const task_t *)0;
    }
    if (g_tasks[pid].state == TASK_FREE && g_tasks[pid].kstack_base == 0u) {
        return (const task_t *)0;
    }
    return &g_tasks[pid];
}

/* 调度器                                                             */

/* 从 from 的下一个开始往后找 READY（环形），找不到返回 -1。 */
static int pick_next(int from)
{
    uint32_t k;
    uint32_t start = (from < 0) ? (uint32_t)TASK_MAX : (uint32_t)from;

    for (k = 1u; k <= TASK_MAX; ++k) {
        uint32_t idx = (start + k) % TASK_MAX;
        if (g_tasks[idx].state == TASK_READY) {
            return (int)idx;
        }
    }
    return -1;
}

int task_sched_active(void)
{
    return g_active;
}

void sched_tick(struct regs *r)
{
    int next;
    task_t *cur;

    if (!g_active || g_current < 0) {
        return;                       /* 内核（Shell）在跑时不抢占 */
    }

    cur = &g_tasks[g_current];
    ++cur->ticks;

    if (g_quantum_left > 1u) {
        --g_quantum_left;
        return;                       /* 时间片还没用完 */
    }
    g_quantum_left = TASK_QUANTUM_TICKS;

    /* 上一行这一句就是"保存现场"的全部：帧已经躺在它自己的栈上。 */
    cur->frame = (uint64_t)(uintptr_t)r;
    cur->state = TASK_READY;

    next = pick_next(g_current);
    if (next < 0 || next == g_current) {
        cur->state = TASK_RUNNING;    /* 没有别的任务可换，继续跑自己 */
        return;
    }

    g_tasks[next].state = TASK_RUNNING;
    ++g_tasks[next].switches;
    g_current = next;
    ++g_switches;

    /* 坑 1：必须换 RSP0，否则下一个任务从 ring3 陷入时用的是旧栈。 */
    tss_set_rsp0(g_tasks[next].kstack_top);

    /* 还得换 CR3。换 CR3 顺带把非全局 TLB 项刷掉，不用 invlpg。换完这一瞬间已经在新地址
     * 空间里执行了，能继续跑是因为每个进程的地址空间都把内核那 1GB 恒等映射（含本函数所在
     * 的内核代码、任务的内核栈）原样挂进去 ， "内核映射共享、用户空间私有"必须如此。 */
    if (g_tasks[next].cr3 != 0u) {
        vmm_load_cr3(g_tasks[next].cr3);
    }

    zh_resume_frame(g_tasks[next].frame);   /* 不返回 */
}

/* 当前任务已经死了（退出或被异常杀掉），切下一个，没有了就回 Shell。
 * 标 noreturn 是实话：要么 zh_resume_frame 换人跑了，要么 zh_sched_leave 把控制流
 * 交还给 Shell 的栈，两条路都不返回到调用者这里。 */
static void schedule_away(void) __attribute__((noreturn));
static void schedule_away(void)
{
    int next;

    for (;;) {
        next = pick_next(g_current);
        if (next < 0) {
            break;                    /* 全死光了 */
        }
        g_tasks[next].state = TASK_RUNNING;
        ++g_tasks[next].switches;
        g_current = next;
        ++g_switches;
        tss_set_rsp0(g_tasks[next].kstack_top);
        if (g_tasks[next].cr3 != 0u) {
            vmm_load_cr3(g_tasks[next].cr3);
        }
        zh_resume_frame(g_tasks[next].frame);   /* 不返回 */
    }

    g_current = -1;

    /* [M5] 回 Shell 之前把 CR3 换回内核自己的：Shell 的代码/数据虽然也在每个
     * 进程地址空间里挂着，但状态还是应该回到内核自己的那一份。 */
    vmm_load_cr3(vmm_kernel_cr3());
    zh_sched_leave();                 /* 回到 task_run_all() 里 call 的下一条指令 */

    /*
     * zh_sched_leave 是 ret 跳走的，编译器角度看它"返回"了，于是 task_exit / task_fault
     * （都标了 noreturn）会被警告 'noreturn' function does return。补个死循环把控制流钉住，
     * 警告没了，语义也如实：走到这行时 CPU 已经在 Shell 的栈上，这个函数体不会再执行。
     */
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

/* [M7] 子进程结束（正常退出或被异常杀掉）时叫醒父进程。做法是直接把返回值写进父进程
 * 保存的那个中断帧的 rax：父进程当初在 wait() 里睡下去时，帧就是它 int 0x80 的帧，
 * 被换回来时 iretq 用的就是这一帧，于是从父进程看，wait() 这个系统调用"返回"了子进程的退出码。 */
static void notify_parent(task_t *cur, int code)
{
    task_t *p;

    cur->exit_code = code;
    cur->has_wait_code = 1u;

    if (cur->parent_pid >= TASK_MAX) {
        return;
    }
    p = &g_tasks[cur->parent_pid];
    if (p->state != TASK_WAIT || p->frame == 0u) {
        return;                        /* 父进程没在等，就留着僵尸位等它 wait */
    }

    ((struct regs *)(uintptr_t)p->frame)->rax = (uint64_t)(int64_t)code;
    p->state = TASK_READY;             /* 唤醒：下一次调度就会轮到它 */
    ++g_wait_count;                    /* 这次 wait 是在阻塞路径上完成的 */
    cur->has_wait_code = 0u;           /* 退出码已经被取走 */
}

void task_exit(int code)
{
    task_t *cur = &g_tasks[g_current];

    cur->state = TASK_DEAD;
    cur->fault_vector = 0;
    notify_parent(cur, code);
    schedule_away();
}

void task_fault(struct regs *r)
{
    task_t *cur = &g_tasks[g_current];

    cur->state = TASK_DEAD;
    cur->fault_vector = (int32_t)r->vector;
    cur->fault_rip = r->rip;
    cur->fault_addr = 0u;
    cur->fault_error = r->error_code;   /* [M5] 0x6 = 页不存在 + 写 + 用户；0x7 = 页存在但被拒 */
    if (r->vector == 14u) {
        uint64_t cr2_value;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2_value));
        cur->fault_addr = cr2_value;
    }

    sh_print(SH_ERR, "[task] killed: pid=");
    sh_print_u32(SH_ERR, cur->pid);
    sh_print(SH_ERR, " name=");
    sh_print(SH_ERR, cur->name);
    sh_print(SH_ERR, " vector=");
    sh_print_u32(SH_ERR, (uint32_t)r->vector);
    sh_print(SH_ERR, " rip=");
    show_hex64(r->rip);
    if (r->vector == 14u) {
        sh_print(SH_ERR, " cr2=");
        show_hex64(cur->fault_addr);
    }
    sh_print(SH_ERR, "  (that task only; the machine keeps running)");
    sh_newline();

    notify_parent(cur, -1);            /* [M7] 父进程的 wait() 会拿到 -1 */
    schedule_away();
}

void task_run_all(void)
{
    int first;

    if (g_current >= 0) {
        err("task: scheduler already running");
        return;
    }

    first = pick_next(-1);
    if (first < 0) {
        err("task: no READY task");
        return;
    }

    g_tasks[first].state = TASK_RUNNING;
    ++g_tasks[first].switches;
    g_current = first;
    g_quantum_left = TASK_QUANTUM_TICKS;
    ++g_switches;

    tss_set_rsp0(g_tasks[first].kstack_top);
    g_active = 1;

    /*
     * 坑，M1 就踩过：zh_sched_enter 一进去就把 rsp 拨到任务栈上、马上 iretq。中间来一个
     * 时钟中断，CPU 会把它中断帧压到我们正要用的那块栈顶，把引导帧踩坏，现象是任务一启动
     * 就 #GP、rip 指向垃圾。所以压帧换栈期间必须 cli，iretq 会按帧里的 RFLAGS 把 IF=1 还回去。
     */
    __asm__ volatile("cli");

    /* [M5] 进第一个任务之前也把 CR3 换成它的（内核映射仍在，换完照旧能跑）。 */
    if (g_tasks[first].cr3 != 0u) {
        vmm_load_cr3(g_tasks[first].cr3);
    }

    zh_sched_enter(g_tasks[first].frame);   /* 保存 Shell 现场后跳进第一个任务 */

    /* 走到这一行 = 所有任务都结束，zh_sched_leave 把 Shell 现场还了回来。 */
    vmm_load_cr3(vmm_kernel_cr3());         /* [M5] 兜底：地址空间回到内核自己的 */
    __asm__ volatile("sti");                /* 回来时 IF 还是 0，不开中断键盘就没反应了 */
    g_active = 0;
    g_current = -1;
}

/* 用户程序：还是直接铺机器码（M1/M2 的老办法，不引汇编器）           */

typedef struct {
    uint8_t *base;
    uint32_t pos;
} emitter_t;

static void put8(emitter_t *e, uint8_t byte)
{
    if (e->pos < TASK_PROG_CAP) {
        e->base[e->pos++] = byte;
    }
}

static void put32(emitter_t *e, uint32_t value)
{
    put8(e, (uint8_t)(value & 0xFFu));
    put8(e, (uint8_t)((value >> 8) & 0xFFu));
    put8(e, (uint8_t)((value >> 16) & 0xFFu));
    put8(e, (uint8_t)((value >> 24) & 0xFFu));
}

/* mov ebx, imm32  -> BB id     （循环计数器，跨 int 0x80 不会丢） */
static void emit_mov_ebx(emitter_t *e, uint32_t imm)
{
    put8(e, 0xBBu);
    put32(e, imm);
}

/* mov edi, imm32  -> BF id     （参数 1） */
static void emit_mov_edi(emitter_t *e, uint32_t imm)
{
    put8(e, 0xBFu);
    put32(e, imm);
}

/* mov eax, imm32  -> B8 id     （系统调用号） */
static void emit_mov_eax(emitter_t *e, uint32_t imm)
{
    put8(e, 0xB8u);
    put32(e, imm);
}

/* int 0x80  -> CD 80 */
static void emit_int80(emitter_t *e)
{
    put8(e, 0xCDu);
    put8(e, 0x80u);
}

/* dec ebx  -> FF CB */
static void emit_dec_ebx(emitter_t *e)
{
    put8(e, 0xFFu);
    put8(e, 0xCBu);
}

/* jnz rel8  -> 75 cb   （向后跳到同窗口内的 label 偏移） */
static void emit_jnz_to(emitter_t *e, uint32_t label)
{
    const uint32_t after = e->pos + 2u;
    const int32_t rel = (int32_t)label - (int32_t)after;

    put8(e, 0x75u);
    put8(e, (uint8_t)(int8_t)rel);
}

/* xor edi, edi  -> 31 FF   （exit 的参数：0） */
static void emit_xor_edi(emitter_t *e)
{
    put8(e, 0x31u);
    put8(e, 0xFFu);
}

/* mov byte ptr [disp32], imm8  -> C6 04 25 disp32 imm8（mod=00 r/m=100 必须带 SIB 0x25） */
static void emit_store_byte(emitter_t *e, uint32_t addr, uint8_t value)
{
    put8(e, 0xC6u);
    put8(e, 0x04u);
    put8(e, 0x25u);
    put32(e, addr);
    put8(e, value);
}

/*
 * 一个任务的用户程序就是这几行：
 *   mov ebx, count
 * loop:
 *   sys_putc(letter)        打印一个字母，内核代打，用户态碰不到显存
 *   sys_sleep(sleep_ticks)  睡几个 tick 让出 CPU，脚底下是 hlt 循环
 *   dec ebx
 *   jnz loop
 *   [可选] mov byte [0xB8F34], 'X'   故意越权写显存，做 #PF
 *   sys_exit(0)
 * 返回写进去的字节数。
 */
static uint32_t build_task_program(uint8_t *win, uint32_t count, char letter,
                                   uint32_t sleep_ticks, int fault_at_end,
                                   uint32_t fault_addr)
{
    emitter_t e;
    uint32_t loop_label;

    e.base = win;
    e.pos = 0u;

    emit_mov_ebx(&e, count);
    loop_label = e.pos;
    emit_mov_edi(&e, (uint32_t)(uint8_t)letter);
    emit_mov_eax(&e, ZH_SYS_PUTC);
    emit_int80(&e);
    emit_mov_edi(&e, sleep_ticks);
    emit_mov_eax(&e, ZH_SYS_SLEEP);
    emit_int80(&e);
    emit_dec_ebx(&e);
    emit_jnz_to(&e, loop_label);

    if (fault_at_end) {
        /* [M5] 越权写的是"自己窗口里故意没映射的那一页"：
         * 页表是 4KB 粒度的，所以这一页真的不存在，用户一写就是 #PF(P=0)。 */
        emit_store_byte(&e, fault_addr, (uint8_t)'X');
    }

    emit_xor_edi(&e);
    emit_mov_eax(&e, ZH_SYS_EXIT);
    emit_int80(&e);

    return e.pos;
}

/* `taskrun` 的验收主体                                               */

int task_demo(int mode)
{
    int pid_a;
    int pid_b;
    uint32_t count_a = 12u;
    uint32_t count_b = 12u;
    const uint32_t sleep_ticks = 3u;
    /* 窗口里的第 255 页：512 页里只映射了第 0 页（代码）和最后一页（栈），
     * 这一页是空的 ， 用户写它就是 #PF，而且 error 的 P 位为 0。 */
    const uint64_t hole_va = VMM_USER_BASE + 0x000FF000ULL;

    if (task_init() != 0) {
        return -1;
    }
    task_reset();

    pid_a = task_create_va("A");
    pid_b = task_create_va("B");
    if (pid_a < 0 || pid_b < 0) {
        err("task: could not create both processes");
        return -1;
    }

    if (mode == 1) {
        count_b = 5u;   /* 故障模式：B 打 5 个字母就去踩那个空页 */
    }

    /* 把程序铺进各自代码页的物理地址（内核恒等映射，可以直接写）。
     * 程序字节是位置无关的，所以同样的代码在两个进程的同一个 VA 上各跑一份。 */
    build_task_program(task_code_phys((uint32_t)pid_a), count_a, 'A',
                       sleep_ticks, 0, 0u);
    build_task_program(task_code_phys((uint32_t)pid_b), count_b, 'B',
                       sleep_ticks, (mode != 0), (uint32_t)hole_va);

    /* 交代现场 */
    info(mode == 0 ? "task self-test (M5): two ring3 processes, private 4 KB page tables"
                   : "task self-test (M5): process B faults on an unmapped page");
    plain("  quantum   : 2 ticks (100Hz) -> 20 ms per slice");
    plain("  per proc  : own PML4+PDPT+PD+PT + own 4 KB code page + own 4 KB stack page");
    plain("  same VA   : both execute at 0x40000000, both stacks at 0x401FF000");
    plain("  isolation : same VA -> different frames -> own CR3 on every switch");
    plain("  hole      : VA 0x400FF000 not mapped (only 2 of 512 pages exist)");

    sh_print(SH_PLAIN, "  task A    : pid=");
    sh_print_u32(SH_PLAIN, (uint32_t)pid_a);
    sh_print(SH_PLAIN, " cr3=");
    show_hex64(task_cr3((uint32_t)pid_a));
    sh_print(SH_PLAIN, " code_pa=");
    show_hex64(g_tasks[pid_a].code_pa);
    sh_newline();
    sh_print(SH_PLAIN, "              kstack_top=");
    show_hex64(g_tasks[pid_a].kstack_top);
    sh_newline();

    sh_print(SH_PLAIN, "  task B    : pid=");
    sh_print_u32(SH_PLAIN, (uint32_t)pid_b);
    sh_print(SH_PLAIN, " cr3=");
    show_hex64(task_cr3((uint32_t)pid_b));
    sh_print(SH_PLAIN, " code_pa=");
    show_hex64(g_tasks[pid_b].code_pa);
    sh_newline();
    sh_print(SH_PLAIN, "              kstack_top=");
    show_hex64(g_tasks[pid_b].kstack_top);
    sh_newline();

    /* 自检：用软件按页表走一遍，确认"切 CR3 之后内核还能取指"。
     * 这是 M5 最容易翻车的地方 ， 内核那一半没挂好，mov cr3 的下一条指令就 #PF。 */
    sh_print(SH_PLAIN, "  self-check: kern ");
    show_hex64(vmm_walk(&g_tasks[pid_a].space, (uint64_t)(uintptr_t)&vmm_load_cr3));
    sh_print(SH_PLAIN, "  code ");
    show_hex64(vmm_walk(&g_tasks[pid_a].space, VMM_USER_BASE));
    sh_print(SH_PLAIN, "  stk ");
    show_hex64(vmm_walk(&g_tasks[pid_a].space, VMM_USER_BASE + VMM_USER_SPAN - VMM_PAGE_SIZE));
    sh_print(SH_PLAIN, "  hole ");
    show_hex64(vmm_walk(&g_tasks[pid_a].space, hole_va));
    sh_newline();

    /* `taskrun dry`：只建进程、只打印，不进 ring3 ， 用来单独检查页表本身。 */
    if (mode == 2) {
        sh_print(SH_PLAIN, "  raw A: pml4[0]=");
        show_hex64(*(uint64_t *)(uintptr_t)g_tasks[pid_a].space.pml4_pa);
        sh_print(SH_PLAIN, " pdpt[0]=");
        show_hex64(*(uint64_t *)(uintptr_t)(g_tasks[pid_a].space.pdpt_pa + 0u));
        sh_print(SH_PLAIN, " pdpt[1]=");
        show_hex64(*(uint64_t *)(uintptr_t)(g_tasks[pid_a].space.pdpt_pa + 8u));
        sh_newline();
        sh_print(SH_PLAIN, "         pd[0]=");
        show_hex64(*(uint64_t *)(uintptr_t)g_tasks[pid_a].space.pd_pa);
        sh_print(SH_PLAIN, " pt[0]=");
        show_hex64(*(uint64_t *)(uintptr_t)g_tasks[pid_a].space.pt_pa);
        sh_print(SH_PLAIN, " pt[511]=");
        show_hex64(*(uint64_t *)(uintptr_t)(g_tasks[pid_a].space.pt_pa + 511u * 8u));
        sh_newline();
        ok("dry run: both processes created, scheduler NOT started");
        return 0;
    }

    warn(mode == 0 ? "-- now running: expect A and B to interleave --"
                   : "-- now running: expect B to die alone, A to finish --");

    /* 开始调度 */
    task_run_all();

    /* 所有任务都结束（或被杀）之后 */
    ok("back in ring 0: all tasks are gone, Shell has the CPU again");

    task_dump();

    if (mode == 0) {
        if (g_tasks[pid_a].exit_code == 0 && g_tasks[pid_a].fault_vector == 0 &&
            g_tasks[pid_b].exit_code == 0 && g_tasks[pid_b].fault_vector == 0 &&
            g_switches >= 10u) {
            ok("M4 PASS: two tasks ran interleaved, both exited normally");
        } else {
            err("M4 FAIL: see the table above");
        }

        /* [M5] 三条硬指标：各有一份 CR3、CR3 互不相同、同一个 VA 落到不同物理页。 */
        if (g_tasks[pid_a].cr3 != 0u && g_tasks[pid_b].cr3 != 0u &&
            g_tasks[pid_a].cr3 != g_tasks[pid_b].cr3 &&
            g_tasks[pid_a].cr3 != vmm_kernel_cr3() &&
            g_tasks[pid_a].user_base == g_tasks[pid_b].user_base &&
            g_tasks[pid_a].code_pa != g_tasks[pid_b].code_pa) {
            ok("M5 PASS: same VA 0x40000000 -> different frames, each process has its own CR3");
        } else {
            err("M5 FAIL: see the address space table above");
        }
    } else {
        if (g_tasks[pid_b].fault_vector == 14 &&
            g_tasks[pid_b].fault_addr == hole_va &&
            g_tasks[pid_a].fault_vector == 0 && g_tasks[pid_a].exit_code == 0) {
            ok("M4 PASS: only the faulting task died, the other one kept running");
        } else {
            err("M4 FAIL: see the table above");
        }

        /* 页表是 4KB 粒度的，所以"窗口里没映射的那一页"真的是空的：
         * error code 6 = bit1(W)=1 + bit2(U)=1 + bit0(P)=0 ， 页不存在，不是权限。 */
        if (g_tasks[pid_b].fault_error == 6u &&
            g_tasks[pid_a].cr3 != g_tasks[pid_b].cr3) {
            ok("M5 PASS: #PF on an unmapped 4 KB page (error 6 = not present+write+user)");
        } else {
            err("M5 FAIL: unexpected page fault error code");
        }
    }

    return 0;
}

/* `tasks` 命令                                                       */

void task_dump(void)
{
    uint32_t i;
    int any = 0;

    info("Task table (M4 preemptive round-robin):");
    plain("  PID  NAME  STATE    TICKS  SWITCH  RESULT");

    for (i = 0u; i < TASK_MAX; ++i) {
        const task_t *t = &g_tasks[i];

        if (t->state == TASK_FREE && t->kstack_base == 0u) {
            continue;
        }
        any = 1;

        sh_print(SH_PLAIN, "  ");
        sh_print_u32(SH_PLAIN, i);
        sh_print(SH_PLAIN, "    ");
        show_field(t->name, 6u);
        show_field(state_name(t->state), 9u);
        sh_print(SH_PLAIN, " ");
        sh_print_u32(SH_PLAIN, (uint32_t)t->ticks);
        sh_print(SH_PLAIN, "    ");
        sh_print_u32(SH_PLAIN, (uint32_t)t->switches);
        sh_print(SH_PLAIN, "      ");

        if (t->fault_vector != 0) {
            sh_print(SH_ERR, "killed by vector ");
            sh_print_u32(SH_ERR, (uint32_t)t->fault_vector);
            if (t->fault_vector == 14) {
                sh_print(SH_ERR, " (#PF) at cr2=");
                show_hex64(t->fault_addr);
            } else if (t->fault_vector == 13) {
                sh_print(SH_ERR, " (#GP)");
            }
            sh_newline();
        } else if (t->state == TASK_DEAD) {
            sh_print(SH_OK, "exit ");
            sh_print_u32(SH_OK, (uint32_t)t->exit_code);
            sh_newline();
        } else {
            sh_print(SH_WARN, (t->state == TASK_RUNNING) ? "on CPU" : "waiting");
            sh_newline();
        }
    }

    if (!any) {
        plain("  (no task yet - run 'taskrun' first)");
        return;
    }

    sh_print(SH_PLAIN, "  context switches: ");
    sh_print_u32(SH_PLAIN, (uint32_t)g_switches);
    sh_print(SH_PLAIN, "   tasks created: ");
    sh_print_u32(SH_PLAIN, g_created);
    sh_newline();

    /* [M5] 每个进程的地址空间 */
    info("Process address spaces (M5, 4 KB pages):");
    plain("  PID  CR3                USER VA              CODE PA              PAGES");
    for (i = 0u; i < TASK_MAX; ++i) {
        const task_t *t = &g_tasks[i];
        uint32_t pages;

        if (!t->space_ready) {
            continue;
        }
        pages = vmm_count_mapped(&t->space);

        sh_print(SH_PLAIN, "  ");
        sh_print_u32(SH_PLAIN, i);
        sh_print(SH_PLAIN, "    ");
        show_hex64(t->cr3);
        sh_print(SH_PLAIN, "  ");
        show_hex64(t->user_base);
        sh_print(SH_PLAIN, "  ");
        show_hex64(t->code_pa);
        sh_print(SH_PLAIN, "  ");
        sh_print_u32(SH_PLAIN, pages);
        sh_print(SH_PLAIN, "/512");
        sh_newline();
    }
    sh_print(SH_PLAIN, "  current CR3: ");
    show_hex64(paging_get_cr3() & 0x000FFFFFFFFFF000ULL);
    sh_print(SH_PLAIN, "   kernel CR3: ");
    show_hex64(vmm_kernel_cr3());
    sh_newline();
}

/* [ADD] M17：信号需要的"可写 TCB"访问点                               */

/* 按下标取可写 TCB：越界或空槽返回 0。signal.c 用它置 pending 位。 */
task_t *task_slot(uint32_t idx)
{
    if (idx >= TASK_MAX) {
        return (task_t *)0;
    }
    if (g_tasks[idx].state == TASK_FREE && g_tasks[idx].kstack_base == 0u) {
        return (task_t *)0;
    }
    return &g_tasks[idx];
}

/* 按 pid 取可写 TCB（本版 pid 就等于槽位下标；分开写是为了以后好改）。 */
task_t *task_slot_pid(uint32_t pid)
{
    return task_slot(pid);
}

/* 当前正在跑的任务 TCB；内核自己在跑时返回 0。 */
task_t *task_current_slot(void)
{
    if (g_current < 0 || g_current >= (int)TASK_MAX) {
        return (task_t *)0;
    }
    return &g_tasks[g_current];
}

/* [M17] 被信号杀死。跟 task_exit 的差别是退出码记成 128 + sig（Linux 的 WIFSIGNALED 语义），
 * 信号号留在 term_sig 里，父进程 wait 回来能分清"正常退出 42"和"被 Ctrl-C 打断(130)"。
 * 这里不把 fault_vector 写成信号号：信号不是异常，进程不算 crash。 */
void task_terminate_by_signal(uint32_t sig)
{
    task_t *cur;
    int code;

    if (g_current < 0 || g_current >= (int)TASK_MAX) {
        for (;;) {
            __asm__ volatile("cli; hlt");
        }
    }
    cur = &g_tasks[g_current];
    code = 128 + (int)sig;

    cur->term_sig = sig;
    cur->fault_vector = 0;
    cur->state = TASK_DEAD;
    cur->foreground = 0u;

    sh_print(SH_ERR, "[kernel] pid ");
    sh_print_u32(SH_ERR, cur->pid);
    sh_print(SH_ERR, " terminated by ");
    sh_print(SH_ERR, task_signal_name(sig));
    sh_print(SH_ERR, " (");
    sh_print_u32(SH_ERR, sig);
    sh_print(SH_ERR, "), exit code ");
    sh_print_u32(SH_ERR, (uint32_t)code);
    sh_newline();

    notify_parent(cur, code);
    schedule_away();                   /* 不返回 */
}

/*
 * [M17] waitpid：只等指定 pid 的子进程。Linux 的 wait4 里 pid 的语义：
 * pid > 0 只等这一个，pid == -1 等任意一个，pid == 0 等同进程组（本版不做）。
 * 实现就是 task_wait_current() 再加一层"这个子进程是不是我要等的"过滤。
 */
int task_waitpid_current(struct regs *r, int32_t pid, uint32_t flags)
{
    task_t *cur;
    uint32_t i;

    if (!g_active || g_current < 0 || g_current >= (int)TASK_MAX) {
        return -1;
    }
    cur = &g_tasks[g_current];

    /* 1) 已经退出的目标子进程：交回退出码并收尸 */
    for (i = 0u; i < (uint32_t)TASK_MAX; ++i) {
        task_t *c = &g_tasks[i];
        if (c->state != TASK_DEAD || c->parent_pid != cur->pid ||
            c->has_wait_code == 0u) {
            continue;
        }
        if (pid >= 0 && (uint32_t)pid != i) {
            continue;
        }
        {
            int code = c->exit_code;
            c->has_wait_code = 0u;
            file_table_init(c->fds, ZH_MAX_FDS);
            c->state = TASK_FREE;
            ++g_wait_count;
            return code;
        }
    }

    /* 2) [ADD] M18：被 SIGSTOP 停住的子进程也算"有状态变化"（Linux 的 WUNTRACED）。
     *    这里不收尸、也不释放槽位：它还活着，只是不在 CPU 上。 */
    if ((flags & ZH_WUNTRACED) != 0u) {
        for (i = 0u; i < (uint32_t)TASK_MAX; ++i) {
            task_t *c = &g_tasks[i];
            if (c->parent_pid != cur->pid || c->state != TASK_STOP) {
                continue;
            }
            if (pid >= 0 && (uint32_t)pid != i) {
                continue;
            }
            return ZH_WSTOPPED | (int)c->term_sig;   /* 0x10000 | 信号号 */
        }
    }

    /* 3) [ADD] M18：WNOHANG ， 没有可收的状态就立刻返回 0，不睡 */
    if ((flags & ZH_WNOHANG) != 0u) {
        for (i = 0u; i < (uint32_t)TASK_MAX; ++i) {
            const task_t *c = &g_tasks[i];
            if (c->parent_pid != cur->pid || c->state == TASK_FREE ||
                c->state == TASK_DEAD) {
                continue;
            }
            if (pid >= 0 && (uint32_t)pid != i) {
                continue;
            }
            return 0;                    /* 这个孩子还活着（跑着或停着） */
        }
        return -1;                       /* 根本没有这个孩子 */
    }

    /* 4) 目标子进程还活着：睡下（子进程退出时把退出码写进本帧的 rax） */
    for (i = 0u; i < (uint32_t)TASK_MAX; ++i) {
        const task_t *c = &g_tasks[i];
        if (c->parent_pid != cur->pid || c->state == TASK_FREE ||
            c->state == TASK_DEAD) {
            continue;
        }
        if (pid >= 0 && (uint32_t)pid != i) {
            continue;
        }
        cur->frame = (uint64_t)(uintptr_t)r;
        cur->state = TASK_WAIT;
        schedule_away();
        return -1;                     /* 不可达 */
    }

    return -1;                         /* 没有这个子进程（Linux 的 ECHILD） */
}

/* [ADD] M18：信号捕获（蹦床页 + sigreturn）与作业控制（STOP / CONT）     */

static uint64_t g_sigreturn_count;
static uint32_t g_stop_count;
static uint32_t g_continue_count;

uint64_t task_sigreturn_count(void) { return g_sigreturn_count; }
uint32_t task_stop_count(void)      { return g_stop_count; }
uint32_t task_continue_count(void)  { return g_continue_count; }

int task_state_of(uint32_t pid)
{
    const task_t *t = task_lookup(pid);

    if (t == (const task_t *)0) {
        return -1;
    }
    return (int)t->state;
}

/*
 * [M18] 信号蹦床页，位置在进程地址空间里栈页下面那一页。
 * 用户 handler 结束时不能直接 ret 回被打断的地方，因为中断帧藏在 TCB 里，只有内核知道
 * 怎么把 22 个寄存器加 rip/cs/rflags/rsp/ss 原样还回去。所以压栈时把"返回地址"写成
 * 这一页上的 7 个字节：
 *     b8 18 00 00 00   mov eax, 24   ; SYS_SIGRETURN
 *     cd 80            int 0x80
 * handler 一 ret 就落到这里，再进内核，由 sys_sigreturn 把整帧扣回去。glibc 的
 * __restore_rt 干的是同一件事，用的是 rt_sigreturn。这一页延迟创建，第一次真要送 handler 才分配。
 */
int task_install_trampoline(task_t *t)
{
    const uint64_t va = VMM_USER_BASE + VMM_USER_SPAN - 2u * VMM_PAGE_SIZE;
    uint64_t pa;
    uint8_t *mem;

    if (t == (task_t *)0) {
        return -1;
    }
    if (t->tramp_pa != 0u) {
        return 0;                       /* 已经有了（fork 继承来的那份） */
    }
    pa = (uint64_t)(uintptr_t)phys_alloc_frame();
    if (pa == 0u) {
        return -1;
    }
    mem = (uint8_t *)(uintptr_t)pa;     /* 恒等映射：内核可以直接写物理页 */
    mem[0] = 0xB8u;                     /* mov eax, imm32 */
    mem[1] = (uint8_t)(ZH_SYS_SIGRETURN & 0xFFu);
    mem[2] = 0u;
    mem[3] = 0u;
    mem[4] = 0u;
    mem[5] = 0xCDu;                     /* int imm8 */
    mem[6] = 0x80u;
    for (unsigned i = 7u; i < PHYS_PAGE_SIZE; ++i) {
        mem[i] = 0x90u;                 /* 后面全填 nop，防止误落进去执行垃圾 */
    }
    if (vmm_map_4k(&t->space, va, pa, 1) != 0) {
        phys_free_frame((void *)(uintptr_t)pa);
        return -1;
    }
    t->tramp_pa = pa;
    return 0;
}

/*
 * [M18] sigreturn：把进 handler 前保存的那一整帧原样扣回中断帧。做完之后 iretq 回到
 * 被信号打断的那条指令，程序完全不知道刚才有 handler 插进来过，就是 Linux 的
 * __NR_rt_sigreturn 干的事。
 */
int task_sigreturn(struct regs *r)
{
    task_t *cur;
    uint32_t i;

    if (g_current < 0 || g_current >= (int)TASK_MAX || r == (struct regs *)0) {
        return -1;
    }
    cur = &g_tasks[g_current];
    if (!cur->sig_in_handler) {
        return -1;                      /* 没人进过 handler，这调用是坏的 */
    }
    for (i = 0u; i < TASK_FRAME_QWORDS; ++i) {
        ((uint64_t *)(void *)r)[i] = cur->sig_frame[i];
    }
    cur->sig_mask &= ~(1u << cur->sig_handling);   /* 解掉 handler 期间的屏蔽 */
    cur->sig_handling = 0u;
    cur->sig_in_handler = 0u;
    ++g_sigreturn_count;
    return 0;
}

/*
 * [M18] SIGSTOP 的默认动作：把自己停住再换人。跟"退出"的区别是不收尸、不释放槽位、
 * 不动 exit_code，只是不再进 pick_next 的候选名单（它只挑 TASK_READY）。
 * 父进程如果正阻塞在 waitpid 里，要叫醒它并告诉它孩子停住了，Linux 里就是 WUNTRACED 的语义。
 */
void task_stop_current(uint32_t sig, struct regs *r)
{
    task_t *cur;
    task_t *p;

    if (g_current < 0 || g_current >= (int)TASK_MAX) {
        for (;;) {
            __asm__ volatile("cli; hlt");
        }
    }
    cur = &g_tasks[g_current];
    /* 唤醒（SIGCONT）时 schedule_away 要靠 TCB 上的 frame 做 iretq，
     * 所以停住之前必须把"现在这一帧"记下来，否则醒来会跳进一个旧的现场。 */
    if (r != (struct regs *)0) {
        cur->frame = (uint64_t)(uintptr_t)r;
    }
    cur->state = TASK_STOP;
    cur->term_sig = sig;                /* 停住它的信号号（WUNTRACED 报这个） */
    cur->foreground = 0u;
    ++g_stop_count;

    sh_print(SH_WARN, "[kernel] pid ");
    sh_print_u32(SH_WARN, cur->pid);
    sh_print(SH_WARN, " stopped by ");
    sh_print(SH_WARN, task_signal_name(sig));
    sh_print(SH_WARN, " -- it stays in memory until SIGCONT");
    sh_newline();

    /* 叫醒在等它的父进程（阻塞 waitpid + WUNTRACED 的组合） */
    if (cur->parent_pid < TASK_MAX) {
        p = &g_tasks[cur->parent_pid];
        if (p->state == TASK_WAIT && p->frame != 0u) {
            ((struct regs *)(uintptr_t)p->frame)->rax =
                (uint64_t)(int64_t)(ZH_WSTOPPED | (int)sig);
            p->state = TASK_READY;
        }
    }
    schedule_away();                    /* 不返回 */
}

/*
 * [M18] SIGCONT：把停住的进程放回就绪队列。返回 1 说明它确实被停过，
 * Linux 里 SIGCONT 对没停的进程是无副作用的。
 */
int task_continue_pid(uint32_t pid)
{
    task_t *t = task_slot_pid(pid);

    if (t == (task_t *)0 || t->state != TASK_STOP) {
        return 0;
    }
    t->state = TASK_READY;
    t->term_sig = 0u;
    ++g_continue_count;

    sh_print(SH_WARN, "[kernel] pid ");
    sh_print_u32(SH_WARN, t->pid);
    sh_print(SH_WARN, " continued (SIGCONT): back in the ready queue");
    sh_newline();
    return 1;
}
