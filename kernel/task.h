/* ============================================================================
 * zh_OS 1.0 (x86_64)
 * File:    kernel/task.h
 * Version: 1.0
 * Brief:   M4 进程与调度：TCB、每任务内核栈、时钟抢占、环形轮转调度。
 * ============================================================================ */

#ifndef ZH_OS_TASK_H
#define ZH_OS_TASK_H

#include <stdint.h>

#include "vmm.h"
#include "../fs/file.h"   /* [ADD] M9：每进程 fd 表 */   /* [M5] 每进程地址空间 */
#include "elf.h"   /* [M6] ELF 装载：task_create_elf 的参数类型 elf_image_t */

struct regs;   /* 中断帧，定义在 kernel/isr.h */

/*
 * M4 要解决什么问题
 * ------------------
 * M1/M2 的用户态是"一次性"的：进 ring3 跑一段，靠 sys_exit 或 #PF 回到内核，
 * 全程只有一个执行流，也没有"谁在跑"的概念。Linux 的多任务由四块拼成：
 *
 *   1) 每个任务有自己的内核栈 —— 用户态一旦陷入内核，CPU 按 TSS.RSP0 换栈。
 *      这就是本文件里 kstack_top 的作用：换任务时把它写进 TSS.RSP0。
 *   2) 每个任务有自己的"完整现场" —— 现场不是几个寄存器，而是中断帧
 *      （struct regs：15 个通用寄存器 + vector/error + rip/cs/rflags/rsp/ss）。
 *      只要记住这帧在哪块栈上，恢复时 rsp 指回去、弹寄存器、iretq 就完事。
 *   3) 有一个抢占点 —— 时钟中断（IRQ0）。每次进来把当前帧的地址记进 TCB，
 *      配额用完就换人。
 *   4) 有一个就绪队列 —— 本版就是一个定长 TCB 数组 + 从当前位置往后找
 *      READY，等价于 Linux 里最简单的 O(n) 轮转，还没有优先级和 CFS。
 *
 * 与 Linux 的差距（M4 之后仍然缺的）
 *   - 没有 fork/exec/wait：任务由内核在启动时"造"出来，不是用户态生出来的；
 *   - 没有独立地址空间：所有任务共用一张页表，各自只被限制在自己的 2MB 大页里
 *     （靠 U/S 位做不到"互相看不见"，只是"内核看不见用户、用户看不见内核"）；
 *   - 没有优先级、没有睡眠队列、没有 SIGCHLD/SIGKILL 这类信号语义；
 *   - 调度发生点在中断里，内核本身不可抢占（没有 preempt_count）。
 * M5 换成 4KB 页表 + 独立 CR3 之后，第 2 条才会被补上。
 */

#define TASK_MAX            4
#define TASK_KSTACK_BYTES   8192ull      /* 每任务内核栈：8192 字节，16 字节对齐 */
#define TASK_QUANTUM_TICKS  2ull         /* 时间片：100Hz 下 2 tick = 20ms */
#define TASK_USER_WINDOW    0x200000ull  /* 每任务 2MB 用户窗口（= 一个 2MB 大页） */

/* 中断帧共 22 个 8 字节槽：15 个通用寄存器 + vector + error + rip/cs/rflags/rsp/ss */
#define TASK_FRAME_QWORDS   22u

/* ---- [ADD] M18：信号常量必须在 task_t 之前，因为 TCB 里按 ZH_SIG_MAX 开 handler 数组 ---- */
#define ZH_SIG_NONE     0u
#define ZH_SIGINT       2u      /* Ctrl-C */
#define ZH_SIGKILL      9u      /* 不可捕获、不可忽略 */
#define ZH_SIGTERM     15u      /* kill 的默认 */
#define ZH_SIGCHLD     17u      /* 子进程退出时给父进程的通知 */
#define ZH_SIGCONT     18u      /* [ADD] M18：让停住的进程继续 */
#define ZH_SIGSTOP     19u      /* [ADD] M18：停住（不可捕获） */
#define ZH_SIGTSTP     20u      /* [ADD] M18：Ctrl-Z —— 可捕获的"停住" */
#define ZH_SIG_MAX     32u

enum task_state {
    TASK_FREE    = 0,   /* 槽位空 */
    TASK_READY   = 1,   /* 就绪，等待被调度 */
    TASK_RUNNING = 2,   /* 正在 CPU 上跑 */
    TASK_DEAD    = 3,   /* 已退出或因异常被杀 */
    TASK_WAIT    = 4,   /* [M7] 阻塞在 wait()：等子进程退出，调度器先跳过它 */
    TASK_STOP    = 5,   /* [ADD] M18：被 SIGSTOP 停住；pick_next 跳过，直到 SIGCONT */
};

typedef struct {
    uint32_t pid;
    uint32_t state;             /* enum task_state */
    const char *name;

    uint64_t kstack_base;       /* 内核栈底（页帧分配器给的地址） */
    uint64_t kstack_top;        /* 内核栈顶 = base + TASK_KSTACK_BYTES，写进 TSS.RSP0 */
    uint64_t frame;             /* 上次被换出时的中断帧地址；首次运行前是造好的引导帧 */

    uint64_t user_base;         /* 2MB 用户窗口基址（代码从 +0 开始，栈在窗口顶端） */
    uint64_t user_sp;
    uint64_t entry;

    uint64_t ticks;             /* 累计占用 CPU 的 tick 数 */
    uint64_t switches;          /* 被换入的次数 */

    int32_t  exit_code;         /* sys_exit 的返回值 */
    int32_t  fault_vector;      /* 0 = 正常；否则是杀它的异常向量（14 = #PF, 13 = #GP） */
    uint64_t fault_addr;        /* #PF 时的 CR2（出错的线性地址） */
    uint64_t fault_rip;
    uint64_t fault_error;       /* [M5] 异常帧里的 error code（0x6 = 页不存在 + 写 + 用户） */

    /* ---- [M5] 每进程独立地址空间 ---- */
    vmm_space_t space;          /* 自己的 PML4/PDPT/PD/PT */
    uint64_t cr3;               /* = space.cr3；0 = 老路径（共用内核页表，不做切换） */
    uint64_t code_pa;           /* 用户代码页的物理地址（内核往里铺机器码用） */
    uint64_t stack_pa;          /* 用户栈页的物理地址 */
    uint32_t space_ready;       /* 地址空间已经建好（重复 taskrun 时复用，不泄漏帧） */

    /* ---- [M7] fork / wait ---- */
    uint32_t parent_pid;        /* fork 出来的子进程记父进程；0xFFFFFFFF = 无父进程 */
    uint32_t has_wait_code;     /* 已经退出、退出码还没被父进程 wait 取走（僵尸位） */

    /* ---- [M9] 每进程文件描述符表 ---- */
    zfile_t  fds[ZH_MAX_FDS];   /* fd 下标 -> (ramfs 节点, 读偏移)；fork 时拷贝一份 */

    /* ---- [ADD] M17：信号 ---- */
    uint32_t sig_pending;       /* 待处理信号位图（bit n = 信号 n） */
    uint32_t term_sig;          /* 非 0 = 是被这个信号杀死的（Linux 的 exit_signal） */
    uint32_t sig_mask;          /* 屏蔽字：这些位上的信号先不送（M17 只做保存/恢复） */
    uint32_t foreground;        /* 1 = 它是前台进程（Ctrl-C 的接收者） */

    /* ---- [ADD] M18：信号捕获（handler + sigreturn） ---- */
    uint64_t sig_handler[ZH_SIG_MAX];      /* 用户 handler 地址；0 = 默认动作 */
    uint64_t sig_frame[TASK_FRAME_QWORDS]; /* 进 handler 之前那一整帧（22 个 qword） */
    uint32_t sig_in_handler;    /* 1 = 正在 handler 里（防嵌套覆盖 sig_frame） */
    uint32_t sig_handling;      /* 正在处理的信号号（sigreturn 时用它解屏蔽） */
    uint64_t tramp_pa;          /* 信号蹦床页物理地址（VA 在栈页下面一页） */
} task_t;

/* ------------------------------------------------------------------ */
/* 内核侧接口                                                         */
/* ------------------------------------------------------------------ */

/* 幂等初始化：向页帧分配器要 TASK_MAX 个连续的 2MB 用户窗口。0 成功，-1 失败。 */
int task_init(void);

/* 把所有槽位复位成 FREE（保留已分配的用户窗口与内核栈，重复运行不泄漏）。 */
void task_reset(void);

/* 造一个任务：分配内核栈 + 造引导帧，返回 pid；失败返回 -1。 */
int task_create(const char *name, uint64_t entry, uint64_t user_sp);

/*
 * [M5] 造一个带独立地址空间的任务：给自己的 PML4/PDPT/PD/PT + 代码页 + 栈页，
 * 代码页映射到 VMM_USER_BASE、栈页映射到窗口最顶上一页，其余 510 页故意不映射
 * （用户一碰就是 #PF，error 的 P 位为 0）。返回 pid，失败 -1。
 * 重复调用复用已建好的地址空间，不会漏帧。
 */
int task_create_va(const char *name);

/*
 * [M6] 用一段 ELF64 文件建进程：地址空间复用 M5 那套（PML4/PDPT/PD/PT + 栈页），
 * 代码页不再由内核铺，而是由 elf_load_into() 按 PT_LOAD 逐段装进来，
 * 入口地址取 ELF 的 e_entry。img 回填装载统计（段数、页数、字节数）。
 * 返回 pid，失败 -1。
 */
int task_create_elf(const char *name, const uint8_t *blob, uint64_t size,
                    elf_image_t *img);

/*
 * [M7] fork：把"当前任务 + 它此刻的现场"复制成第二个任务。
 *   - 地址空间由 vmm_space_clone() 整页复制（暂不做 COW）；
 *   - 子进程的引导帧 = 父进程此刻中断帧的副本，只把 rax 改成 0；
 *   - 于是父子都从 int 0x80 的下一条指令继续跑，父拿到子进程 pid，子拿到 0。
 *     （Linux 的 fork 返回值语义就是这么实现的：一份寄存器映像复制两份。）
 * 返回子进程 pid，失败 -1。
 */
int task_fork_current(struct regs *r);

/*
 * [M7] wait：取一个已退出子进程的退出码。
 *   - 有僵尸子进程 -> 直接返回它的退出码（并释放槽位）；
 *   - 有活着的子进程 -> 把自己标成 TASK_WAIT，调用 schedule_away() 让出 CPU，
 *     子进程退出时会把退出码写回**这个帧的 rax**，父进程被换回时 syscall 就"返回"了；
 *   - 没有子进程 -> 返回 -1。
 * 阻塞分支不会返回到调用者（走 schedule_away 的 noreturn 路径）。
 */
int task_wait_current(struct regs *r);

/*
 * [M8] exec：把**当前任务**的映像换成一段 ELF，pid 不变、地址空间换新的。
 *   - 新建一个 vmm 空间（旧的不回收，写进已知限制），装载 ELF、映射一页新栈；
 *   - 把中断帧里的 rip 改成 e_entry、rsp 改成新栈顶、rax/rbp 等清零 ——
 *     于是 syscall_dispatch 返回后 iretq 直接落在新程序的入口，
 *     旧程序从这一刻起"从内存里消失"（Linux 的 start_thread() 也是改 pt_regs）；
 *   - 最后 mov cr3 到新空间（内核栈在新空间里也照旧映射着）。
 * 成功 0，失败 -1。
 */
int task_exec_current(struct regs *r, const uint8_t *blob, uint64_t size,
                      elf_image_t *img);

/*
 * [M11] 带参数的 exec：Linux 里 execve(path, argv, envp) 的 argv 部分。
 * 内核把每个参数字符串拷到**新**地址空间的栈页底部，再在那页里排一个指针数组，
 * 然后把 rdi=argc、rsi=&argv[0] 交给新程序的 _start —— 于是程序第一次能知道
 * "我叫什么名字、带了什么参数"。argv 为 NULL 时等价于老的 exec(path)。
 */
int task_exec_current_argv(struct regs *r, const uint8_t *blob, uint64_t size,
                           elf_image_t *img, char *const *argv, uint32_t argc);

/* [M7] 当前正在跑的任务 pid；没有任务在跑时返回 0xFFFFFFFF。 */
uint32_t task_current_pid(void);

/* ---- [ADD] M17：信号 API（常量已在文件上方定义） ---- */

/* 信号名（给 kill 命令和日志用）；未知返回 "SIG?"。 */
const char *task_signal_name(uint32_t sig);

/*
 * [M17] 投递一个信号：只是往 sig_pending 上置一位，**不**立刻打断对方。
 * 真正的"送达"发生在那个进程下一次从内核回 ring3 的那一刻
 * （interrupt_dispatch 的出口），这就是 Linux 里 TIF_NOTIFY_SIGNAL /
 *  ret_from_intr 检查 _TIF_SIGPENDING 的位置。
 * 返回 0 成功；-1 = 没有这个 pid。
 */
int task_signal(uint32_t pid, uint32_t sig);

/* [M17] 给"前台进程"发信号（Ctrl-C 走这里）；没有前台进程时返回 -1。 */
int task_signal_foreground(uint32_t sig);

/* [M17] 设置/查询前台进程（shell 在 fork+exec 之后、wait 之前调用）。 */
void task_set_foreground(uint32_t pid);
void task_clear_foreground(void);   /* [M17] 前台没了（子进程已退出）：清零所有标记 */
uint32_t task_foreground_pid(void);

/*
 * [M17] 在"内核 -> 用户"的边界上检查待处理信号。有默认动作要执行时：
 *   - 终止类（SIGKILL/SIGINT/SIGTERM，且没装 handler）-> 自杀并 schedule_away()
 *     （不返回），退出码 = 128 + sig，父进程 wait 能拿到；
 *   - 否则清 pending，让 iretq 照常回 ring3。
 */
void task_check_signals(struct regs *r);

/* [M17] 槽位访问（signal.c 用）：按 pid / 按下标取**可写**的 TCB，取不到返回 0。 */
task_t *task_slot_pid(uint32_t pid);
task_t *task_slot(uint32_t idx);
task_t *task_current_slot(void);

/* [M17] 被信号杀死：标 DEAD、退出码 128+sig、叫醒父进程、换人跑（不返回）。 */
void task_terminate_by_signal(uint32_t sig) __attribute__((noreturn));

/* [M7] 指定 pid 的 waitpid：等**某个**子进程（pid < 0 表示任意一个）。 */
int task_waitpid_current(struct regs *r, int32_t pid, uint32_t flags);

/* [ADD] M18：waitpid 的 flags（与 Linux 同名，只做本版用得到的那两位） */
#define ZH_WNOHANG   0x00000001  /* 不阻塞：没有已结束的子进程就返回 0 */
#define ZH_WUNTRACED 0x00000002  /* 被 SIGSTOP 停住的子进程也算"有状态变化" */
#define ZH_WSTOPPED  0x00010000  /* 返回值里的停止标记：0x10000 | 信号号 */

/* [ADD] M18：sys_sigreturn 的实体：把 sig_frame 原样扣回中断帧（恢复整帧） */
int task_sigreturn(struct regs *r);

/* [ADD] M18：任务状态查询（SYS_GETSTATE 用；相当于 /proc/<pid>/stat 的第一个字母） */
int task_state_of(uint32_t pid);

/* [ADD] M18：SIGSTOP 的默认动作 —— 把自己标成 TASK_STOP 然后让人（不返回）。 */
void task_stop_current(uint32_t sig, struct regs *r) __attribute__((noreturn));

/* [ADD] M18：SIGCONT —— 把停住的进程重新放回就绪队列。返回 1 = 它确实曾被停住。 */
int task_continue_pid(uint32_t pid);

/* [ADD] M18：把一个任务装进蹦床页（第一次送 handler 之前调用；幂等） */
int task_install_trampoline(task_t *t);

/* [ADD] M18：作业控制计数（jobs 的 PASS 判定用） */
uint32_t task_stop_count(void);
uint32_t task_continue_count(void);
uint64_t task_sigreturn_count(void);

/* [M17] 计数：一共投递了几个信号、有几个真的送到了（terminate 或 handler）。 */
uint32_t task_signal_send_count(void);
uint32_t task_signal_deliver_count(void);
/* [M7] 计数：一共 fork 了几次、wait 成功了几次（供 `elf fork` 做 PASS 判定）。 */
uint32_t task_fork_count(void);
uint32_t task_wait_count(void);

/* [M8] 一共成功 exec 了几次（供 `elf exec` 做 PASS 判定）。 */
uint32_t task_exec_count(void);

/* [ADD] M12：exec 日志开关（0 = 只留结论，给 mm 命令用） */
void task_set_exec_verbose(int on);

/* [ADD] M12：地址空间回收统计（回收了几套 mm / 几个用户页） */
uint32_t task_reclaim_space_count(void);
uint32_t task_reclaim_page_count(void);

/* [M9] 当前任务的 fd 表（syscall 层 open/read/close 用）；没有任务在跑时返回 0。 */
zfile_t *task_current_fds(void);

/* [M5] 第 pid 个任务的代码页物理地址（内核在这里铺机器码；恒等映射可直接写）。 */
uint8_t *task_code_phys(uint32_t pid);

/* [M5] 第 pid 个任务的私有窗口虚拟地址基点（本版所有任务都是 VMM_USER_BASE）。 */
uint64_t task_va_base(uint32_t pid);

/* [M5] 第 pid 个任务的 CR3；没有独立地址空间时返回 0。 */
uint64_t task_cr3(uint32_t pid);

/* [M5] 当前正在跑的任务的用户窗口虚拟地址基点；没有任务在跑返回 0。
 * 给 syscall.c 的 copy_from_user 校验用：M5 之后用户指针是虚拟地址，
 * 不能再用 M1 那块 2MB 窗口的物理地址去比。 */
uint64_t task_user_window_current(void);

/* [M5] 按 pid 取 TCB（只读；槽位空返回 0）。给 `vmspace` 命令打印页表用。 */
const task_t *task_lookup(uint32_t pid);

/* 第 pid 个任务的 2MB 用户窗口基址（pid 越界返回 0）。 */
uint8_t *task_user_window(uint32_t pid);

/* 让用户窗口所在的那个 2MB 大页对 ring3 可访问（四级页表 U/S 全置 1）。 */
void task_allow_user_window(uint8_t *win);

/* 从这里开始跑所有 READY 任务；所有任务都结束后返回（回到 Shell）。 */
void task_run_all(void);

/* 调度器是否正在运行（sys_exit / 异常处理据此决定"交还 Shell"还是"杀掉任务"）。 */
int task_sched_active(void);

/* IRQ0 抢占点：由 isr.c 在 pit_irq_handler() 之后调用。可能直接切换、不返回。 */
void sched_tick(struct regs *r);

/* sys_exit：把当前任务标成 DEAD，然后切到下一个任务（不返回）。 */
void task_exit(int code) __attribute__((noreturn));

/* 用户态异常：杀掉当前任务、切到下一个（不返回）。CPU/内核自己的异常不走这里。 */
void task_fault(struct regs *r) __attribute__((noreturn));

/* `tasks` 命令：打印 TCB 表。 */
void task_dump(void);

/* `taskrun` 命令：mode 0 = 正常双任务；mode 1 = 让 B 故意触发 #PF 被单独杀掉。 */
int task_demo(int mode);

/* ------------------------------------------------------------------ */
/* kernel/task_switch.asm 提供的三个"永不按常规返回"的入口            */
/* ------------------------------------------------------------------ */
/*
 * 注意：zh_sched_enter / zh_sched_leave 刻意"不"标 noreturn ——
 *   它们确实是成对返回的（所有任务死光后 zh_sched_leave 会 ret 回 task_run_all），
 *   标了 noreturn 的话 GCC 会把 task_run_all 里 call 之后的收尾代码整段删掉，
 *   那就永远回不到 Shell 了。这与 kernel/ring3.h 里 usermode_enter 的处理一致。
 */
void zh_sched_enter(uint64_t frame);   /* 保存 Shell 现场后跳进第一个任务 */
void zh_sched_leave(void);             /* 所有任务结束后回到 Shell 现场 */
void zh_resume_frame(uint64_t frame) __attribute__((noreturn)); /* 按中断帧恢复某个任务 */

#endif /* ZH_OS_TASK_H */
