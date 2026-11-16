/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    kernel/signal.c
 * Version: 0.2beta
 * Brief:   M17 信号：pending 位图、默认动作、在"内核 -> 用户"边界上送达。
 *
 * 为什么单独一个文件
 * ------------------
 * Linux 里信号不属于调度器，也不属于系统调用：它是一套**异步通知**。
 *   - signal.c 只管"记下来"（send_sig / sigaddset）；
 *   - 真正送的那一刻在 arch/x86/entry/common.c 的 exit_to_user_mode_loop()：
 *     出门回用户态之前查一次 _TIF_SIGPENDING，有就 get_signal()，
 *     要么调 handler，要么按默认动作杀掉自己。
 *
 * 本文件就是这个拆分的最小版：task_signal() 只置位，
 * task_check_signals() 在 iretq 之前那一下才动手。
 * ============================================================================ */

#include "task.h"

#include "isr.h"
#include "syscall.h"
#include "paging.h"          /* [ADD] M18：VMM_USER_BASE / VMM_USER_SPAN / VMM_PAGE_SIZE */
#include "../shell/shell.h"

/* [M17] 计数：投递了几个、真的送到（终止）了几个 */
static uint32_t g_sig_sent;
static uint32_t g_sig_delivered;

uint32_t task_signal_send_count(void)
{
    return g_sig_sent;
}

uint32_t task_signal_deliver_count(void)
{
    return g_sig_delivered;
}

const char *task_signal_name(uint32_t sig)
{
    switch (sig) {
    case ZH_SIGINT:   return "SIGINT";
    case ZH_SIGKILL:  return "SIGKILL";
    case ZH_SIGTERM:  return "SIGTERM";
    case ZH_SIGCONT:  return "SIGCONT";   /* [ADD] M18 */
    case ZH_SIGSTOP:  return "SIGSTOP";   /* [ADD] M18 */
    case ZH_SIGTSTP:  return "SIGTSTP";   /* [ADD] M18 */
    case ZH_SIGCHLD:  return "SIGCHLD";
    default:          return "SIG?";
    }
}

/*
 * 投递：只置 pending 位。
 * 注意这里**不**碰对方的中断帧 —— 对方可能正躺在它的内核栈上跑内核代码，
 * 也可能在 ring3。改帧是送达那一步的事，而且是"它自己"改自己的帧。
 */
int task_signal(uint32_t pid, uint32_t sig)
{
    task_t *t;

    if (sig == ZH_SIG_NONE || sig >= ZH_SIG_MAX) {
        return -1;
    }
    t = task_slot_pid(pid);
    if (t == (task_t *)0) {
        return -1;
    }
    if (t->state == TASK_FREE || t->state == TASK_DEAD) {
        return -1;                    /* 已经死了，信号没意义（Linux 也是 ESRCH） */
    }

    t->sig_pending |= (1u << sig);
    ++g_sig_sent;

    /*
     * [ADD] M18 的关键一步（Linux 的 signal_wake_up_state）：
     * SIGCONT 不能靠"pending 位等它自己回 ring3 时查"——因为它被停住了，
     * 根本不会再跑到任何检查点。所以对 TASK_STOP 的进程必须**直接**放回就绪队列。
     * 踩坑记录：第一版只置位，结果 `fg 1` 打印完 "SIGCONT" 之后进程一动不动，
     * jobs 里还是 Stopped（屏幕上的 tick 一个都不往下走）。
     */
    if (sig == ZH_SIGCONT && (t->state == TASK_STOP ||
                              t->state == TASK_WAIT)) {
        t->sig_pending &= ~(1u << sig);
        (void)task_continue_pid(t->pid);
    }
    return 0;
}

int task_signal_foreground(uint32_t sig)
{
    uint32_t i;

    for (i = 0u; i < TASK_MAX; ++i) {
        task_t *t = task_slot(i);
        if (t->state != TASK_FREE && t->state != TASK_DEAD && t->foreground != 0u) {
            return task_signal(t->pid, sig);
        }
    }
    return -1;
}

void task_set_foreground(uint32_t pid)
{
    uint32_t i;

    for (i = 0u; i < TASK_MAX; ++i) {
        if (task_slot(i)->pid == pid) {
            task_slot(i)->foreground = 1u;
        }
    }
}

void task_clear_foreground(void)
{
    uint32_t i;

    for (i = 0u; i < TASK_MAX; ++i) {
        task_slot(i)->foreground = 0u;
    }
}

uint32_t task_foreground_pid(void)
{
    uint32_t i;

    for (i = 0u; i < TASK_MAX; ++i) {
        const task_t *t = task_slot(i);
        if (t->state != TASK_FREE && t->state != TASK_DEAD && t->foreground != 0u) {
            return t->pid;
        }
    }
    return 0xFFFFFFFFu;
}

/*
 * [ADD] M18：把一帧改成"去执行用户 handler"。
 *
 * 内核要做的事，和 Linux 的 setup_rt_frame() 一模一样：
 *   1) 把当前整帧（22 个 qword）存进 TCB 的 sig_frame —— handler 返回后要原样还回去；
 *   2) 在**用户栈**上压一个返回地址，值是蹦床页地址（那段 7 字节的 int 0x80）；
 *   3) 把 rip 改成用户 handler，rdi 给信号号（handler(sig)），rax 清 0。
 * handler 最后一条 `ret` 弹出第 2 步压的地址 → 跳进蹦床 → SYS_SIGRETURN → 整帧复原。
 */
static void deliver_to_handler(task_t *cur, struct regs *r, uint32_t sig)
{
    uint64_t rax_saved = r->rax;
    uint64_t slot;
    uint32_t i;

    if (task_install_trampoline(cur) != 0) {
        return;                      /* 没帧可分配：这次算了，留在 pending 里 */
    }

    for (i = 0u; i < TASK_FRAME_QWORDS; ++i) {
        cur->sig_frame[i] = ((uint64_t *)(void *)r)[i];
    }

    /* 返回地址槽：对齐到 16 字节边界再减 8，让 handler 拿到 rsp ≡ 8 (mod 16)
     * （SysV ABI 要求：函数入口处 rsp+8 是 16 的倍数，否则 movaps 会 #GP） */
    slot = (r->rsp & ~0xFULL) - 8u;
    *((uint64_t *)(uintptr_t)slot) =
        VMM_USER_BASE + VMM_USER_SPAN - 2u * VMM_PAGE_SIZE;

    r->rsp = slot;
    r->rip = cur->sig_handler[sig];
    r->rdi = (uint64_t)sig;
    r->rax = 0u;
    (void)rax_saved;

    cur->sig_in_handler = 1u;
    cur->sig_handling = sig;
    cur->sig_mask |= (1u << sig);    /* handler 期间先屏蔽自己（Linux 的 sa_mask） */
}

/*
 * 边界检查：Linux 的 exit_to_user_mode_loop()。
 *
 * [ADD] M18 的改动只有三处，顺序就是 Linux 的 get_signal() 里的顺序：
 *   - 有用户 handler（且不是 SIGKILL）→ 改帧，让它去跑 handler；
 *   - SIGSTOP / SIGTSTP：默认动作 = 停住（作业控制）；
 *   - SIGCONT：默认动作 = 继续（对正在跑的进程是无副作用）。
 * 其余仍然走 M17：SIGINT / SIGTERM / SIGKILL 自杀，退出码 128+sig。
 */
void task_check_signals(struct regs *r)
{
    task_t *cur;
    uint32_t sig;
    uint32_t pending;

    if (!task_sched_active()) {
        return;                        /* 内核（Shell）自己跑的时候没有信号 */
    }
    if (r == (struct regs *)0) {
        return;
    }
    cur = task_current_slot();
    if (cur == (task_t *)0) {
        return;
    }
    if (cur->sig_in_handler != 0u) {
        return;      /* 已经在 handler 里：这一帧就是 handler 的现场，不能再覆盖 */
    }
    pending = cur->sig_pending & (uint32_t)~(1u << ZH_SIGCHLD);
    if (pending == 0u) {
        return;
    }
    /*
     * [ADD] M18 踩过的坑：**只在"回用户态"的那一帧上动手**。
     *
     * 键盘 IRQ 完全可能在进程"陷在内核里"的时候到来（比如 SYS_SLEEP 里的 hlt 循环）。
     * 此时 `r` 是内核里那层的中断帧（cs=0x08、rsp 是内核栈），照它去改 rip/rsp，
     * iretq 就会拿内核的 cs 去跳用户地址 —— #GP，整机 halt（本版真踩过）。
     * Linux 的做法是等最外层 exit_to_user_mode 时才 get_signal()，这里用
     * "(r->cs & 3) == 3" 表达同一件事：pending 位留着，等 syscall/IRQ 一路
     * 退回到用户态边界时再送。
     */
    if ((r->cs & 3u) != 3u) {
        return;
    }

    for (sig = 1u; sig < ZH_SIG_MAX; ++sig) {
        uint32_t bit = 1u << sig;
        if ((pending & bit) == 0u) {
            continue;
        }
        if ((cur->sig_mask & bit) != 0u) {
            continue;                  /* 被屏蔽：留着，等放开再送 */
        }

        /* [M18] 1) 用户装了 handler（SIGKILL 不可捕获）：改帧去跑 handler */
        if (sig != ZH_SIGKILL && cur->sig_handler[sig] != 0u) {
            uint32_t before = cur->sig_pending;
            cur->sig_pending &= ~bit;
            ++g_sig_delivered;
            deliver_to_handler(cur, r, sig);
            if (cur->sig_pending != before) {
                sh_print(SH_WARN, "[kernel] ");
                sh_print(SH_WARN, task_signal_name(sig));
                sh_print(SH_WARN, " -> entering the user handler "
                                  "(saved frame in the TCB)");
                sh_newline();
            }
            return;
        }

        /* [M18] 2) 作业控制：SIGSTOP / SIGTSTP 停住，SIGCONT 继续 */
        if (sig == ZH_SIGSTOP || sig == ZH_SIGTSTP) {
            cur->sig_pending &= ~bit;
            ++g_sig_delivered;
            task_stop_current(sig, r);  /* 不返回（r 是当前帧，停住/唤醒都要它） */
        }
        if (sig == ZH_SIGCONT) {
            cur->sig_pending &= ~bit;
            ++g_sig_delivered;
            continue;                   /* 它正在跑，本来就是"继续"状态 */
        }

        if (sig != ZH_SIGKILL && sig != ZH_SIGINT && sig != ZH_SIGTERM) {
            cur->sig_pending &= ~bit;  /* 其他号本版没有默认动作，丢掉 */
            continue;
        }

        cur->sig_pending &= ~bit;
        ++g_sig_delivered;
        task_terminate_by_signal(sig);   /* 不返回 */
    }
}
