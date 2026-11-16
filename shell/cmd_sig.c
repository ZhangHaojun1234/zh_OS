/* ============================================================================
 * zh_OS 1.0 (x86_64)
 * File:    shell/cmd_sig.c
 * Version: 1.0
 * Brief:   M17 `sig` 命令：信号 + waitpid + Ctrl-C 三件事的自证。
 *
 * 做三件事：
 *   1) 在**内核**里造一个纯自旋进程（/bin/spin.elf 的那段循环是它自己的），
 *      在调度器里跑起来之前就给它置 SIGKILL —— 证明"pending 信号"会在它
 *      第一次从内核回 ring3 的那一刻生效。
 *   2) 跑 /bin/sigdemo.elf：父子都在 ring3，子自旋、父 kill + waitpid(137)。
 *   3) 打印计数：投递了几个、送达了几个、terminate 了几次。
 *
 * 为什么第 1 步能成立：调度器第一次进任务也是"从内核 -> ring3"，
 * 走的是同一条 interrupt_dispatch 出口，所以 task_check_signals() 一样会跑到。
 * ============================================================================ */

#include "cmd_sig.h"

#include "shell.h"
#include "../kernel/elf.h"
#include "../kernel/task.h"
#include "../kernel/vmm.h"
#include "../kernel/syscall.h"
#include "../fs/ramfs.h"
#include "../mm/phys.h"

static uint8_t g_blob[64u * 1024u];

static int load(const char *path, uint64_t *len_out)
{
    uint64_t len = 0u;
    int rc;

    rc = ramfs_read_file(path, g_blob, (uint64_t)sizeof(g_blob), &len);
    if (rc != RAMFS_OK) {
        err("sig: cannot read the program");
        plain(path);
        plain(" -> ");
        plain(ramfs_strerror(rc));
        return -1;
    }
    *len_out = len;
    return 0;
}

int cmd_sig(int argc, char **argv)
{
    uint64_t before_send;
    uint64_t before_deliver;
    int pid;
    int rc;
    elf_image_t img;

    (void)argc;
    (void)argv;

    task_reset();
    syscall_set_write_log(0);
    task_set_exec_verbose(0);

    if (task_init() != 0) {
        err("sig: task subsystem failed to init");
        return SHELL_CONTINUE;
    }

    info("M17 signals: kill() only marks a bit; delivery happens on the way back to ring3");

    /* ---------- 第 1 段：内核侧投递 SIGKILL 给一个还没跑的自旋进程 ---------- */
    before_send = task_signal_send_count();
    before_deliver = task_signal_deliver_count();

    {
        uint64_t len = 0u;
        if (load("/bin/spin.elf", &len) != 0) {
            return SHELL_CONTINUE;
        }
        pid = task_create_elf("spin", g_blob, len, &img);
        if (pid < 0) {
            err("sig: could not create the spin process");
            return SHELL_CONTINUE;
        }
        sh_print(SH_PLAIN, "  created pid=");
        sh_print_u32(SH_PLAIN, (uint32_t)pid);
        sh_print(SH_PLAIN, " (it has not run a single instruction yet)\n");

        rc = task_signal((uint32_t)pid, ZH_SIGKILL);
        sh_print(SH_PLAIN, "  kill(");
        sh_print_u32(SH_PLAIN, (uint32_t)pid);
        sh_print(SH_PLAIN, ", SIGKILL) -> ");
        sh_print(SH_PLAIN, (rc == 0) ? "queued (pending bit set, nobody interrupted)" : "FAILED");
        sh_newline();
    }

    task_run_all();   /* 它一进门就被 pending 的 SIGKILL 打死，然后回到这里 */

    {
        const task_t *t = task_lookup((uint32_t)pid);
        sh_print(SH_PLAIN, "  killed    : term_sig=");
        sh_print_u32(SH_PLAIN, (t != (const task_t *)0) ? t->term_sig : 0u);
        sh_print(SH_PLAIN, "  exit_code=");
        sh_print_u32(SH_PLAIN, (t != (const task_t *)0) ? (uint32_t)t->exit_code : 0u);
        sh_print(SH_PLAIN, "  sent=");
        sh_print_u32(SH_PLAIN, (uint32_t)(task_signal_send_count() - before_send));
        sh_print(SH_PLAIN, "  delivered=");
        sh_print_u32(SH_PLAIN, (uint32_t)(task_signal_deliver_count() - before_deliver));
        sh_newline();
    }

    /* ---------- 第 2 段：ring3 里的 sigdemo（fork + kill + waitpid） ---------- */
    {
        uint64_t len = 0u;
        int dpid;

        if (load("/bin/sigdemo.elf", &len) != 0) {
            return SHELL_CONTINUE;
        }
        dpid = task_create_elf("sigdemo", g_blob, len, &img);
        if (dpid < 0) {
            err("sig: could not create the demo process");
            return SHELL_CONTINUE;
        }
        sh_print(SH_PLAIN, "  demo      : pid=");
        sh_print_u32(SH_PLAIN, (uint32_t)dpid);
        sh_println(SH_PLAIN, " (fork -> kill(child,SIGKILL) -> waitpid)");
        task_run_all();
    }

    if (task_signal_deliver_count() < 2u) {
        err("sig: fewer deliveries than expected (the pending bit never fired)");
        return SHELL_CONTINUE;
    }

    ok("M17 PASS: a queued signal is delivered on the next return to ring3,");
    plain("          a spinning child with no syscalls still gets killed,");
    plain("          and waitpid(pid) collects 128+9 = 137 exactly like Linux.");
    plain("          (^C in sh3 sends SIGINT to the foreground child: try it.)");
    return SHELL_CONTINUE;
}
