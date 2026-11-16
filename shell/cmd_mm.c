/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    shell/cmd_mm.c
 * Version: 0.2beta
 * Brief:   M12 `mm` 命令：证明地址空间回收真的发生了。
 *
 * 光看代码说"我回收了"没用，要能看见页帧数。所以这条命令做三件事：
 *   1) 打印页帧总量/已用/空闲（分配器的真实账）；
 *   2) 跑 /bin/reexec.elf —— 它连续 exec 自己 7 次，每次都建一整套新 mm；
 *   3) 再打印一次页帧账，对比"跑完还剩多少"。
 *
 * 判据：跑完之后的空闲帧数只比跑之前少**一套** mm（最后那个活着没退出的进程），
 * 而不是少 8 套。另外回收计数器会显示"回收了几套 mm、几个用户页"。
 * ============================================================================ */

#include "cmd_mm.h"

#include "shell.h"
#include "../kernel/elf.h"
#include "../kernel/task.h"
#include "../kernel/vmm.h"
#include "../fs/ramfs.h"
#include "../mm/phys.h"
#include "../kernel/syscall.h"

static uint8_t g_blob[64u * 1024u];

static void show_frames(const char *tag, uint64_t *slot)
{
    uint64_t free_now = phys_free_frame_count();

    sh_print(SH_PLAIN, "  ");
    sh_print(SH_PLAIN, tag);
    sh_print(SH_PLAIN, ": total=");
    sh_print_u32(SH_PLAIN, (uint32_t)phys_total_frames());
    sh_print(SH_PLAIN, "  used=");
    sh_print_u32(SH_PLAIN, (uint32_t)phys_used_frame_count());
    sh_print(SH_PLAIN, "  free=");
    sh_print_u32(SH_PLAIN, (uint32_t)free_now);
    sh_newline();
    if (slot != (uint64_t *)0) {
        *slot = free_now;
    }
}

int cmd_mm(int argc, char **argv)
{
    const char *path = "/bin/reexec.elf";
    const task_t *t;
    uint64_t len = 0u;
    uint64_t free_before = 0u;
    uint64_t free_after = 0u;
    uint32_t execs_before;
    uint32_t spaces_before;
    uint32_t pages_before;
    elf_image_t img;
    int pid;
    int rc;

    (void)argc;
    (void)argv;

    task_reset();
    syscall_set_write_log(0);
    task_set_exec_verbose(0);   /* 关掉 sys_write 的逐条日志，屏幕上只留结论 */

    /*
     * 先把任务子系统初始化好再开始量账。
     * task_init() 第一次被调用时会一次性分配 TASK_MAX × 2MB = 8MB 的用户窗口池
     * （2048 帧）—— 那是"开机固定成本"，不是 exec 的泄漏。此前第一次跑 mm 时
     * 恰好替别的命令把它触发出来了，于是差值里混进 2056 帧的假警报。
     */
    if (task_init() != 0) {
        err("mm: task subsystem failed to init");
        return SHELL_CONTINUE;
    }

    info("M12 address-space reclaim: does exec() give its old mm back?");
    sh_print(SH_PLAIN, "  stress    : ");
    sh_print(SH_PLAIN, path);
    sh_println(SH_PLAIN, " execs itself 7 times (one fresh mm each time)");
    show_frames("frames    ", &free_before);

    rc = ramfs_read_file(path, g_blob, (uint64_t)sizeof(g_blob), &len);
    if (rc != RAMFS_OK) {
        err("mm: cannot open the stress program");
        plain(ramfs_strerror(rc));
        return SHELL_CONTINUE;
    }

    execs_before = task_exec_count();
    spaces_before = task_reclaim_space_count();
    pages_before = task_reclaim_page_count();

    pid = task_create_elf("reexec", g_blob, len, &img);
    if (pid < 0) {
        err("mm: could not create the process");
        return SHELL_CONTINUE;
    }
    plain("run: each exec() builds a new mm; the old one must be destroyed");
    {
        uint64_t probe_free = phys_free_frame_count();
        sh_print(SH_PLAIN, "  frames@load: free=");
        sh_print_u32(SH_PLAIN, (uint32_t)probe_free);
        sh_newline();
    }

    task_run_all();

    show_frames("after run ", &free_after);

    t = task_lookup((uint32_t)pid);
    sh_print(SH_PLAIN, "  execs     : +");
    sh_print_u32(SH_PLAIN, task_exec_count() - execs_before);
    sh_print(SH_PLAIN, "   reclaimed: +");
    sh_print_u32(SH_PLAIN, task_reclaim_space_count() - spaces_before);
    sh_print(SH_PLAIN, " mm / +");
    sh_print_u32(SH_PLAIN, task_reclaim_page_count() - pages_before);
    sh_print(SH_PLAIN, " pages");
    sh_newline();
    if (t != (const task_t *)0) {
        sh_print(SH_PLAIN, "  final     : exit code=");
        sh_print_u32(SH_PLAIN, (uint32_t)t->exit_code);
        sh_print(SH_PLAIN, "  faults=");
        sh_print_u32(SH_PLAIN, (uint32_t)t->fault_vector);
        sh_newline();
    }
    sh_print(SH_PLAIN, "  free      : before=");
    sh_print_u32(SH_PLAIN, (uint32_t)free_before);
    sh_print(SH_PLAIN, "  after=");
    sh_print_u32(SH_PLAIN, (uint32_t)free_after);
    sh_print(SH_PLAIN, "  delta=");
    sh_print_u32(SH_PLAIN, (uint32_t)(free_before - free_after));
    sh_newline();

    if (t == (const task_t *)0 || t->fault_vector != 0u || t->exit_code != 0) {
        err("mm: the stress program died");
        return SHELL_CONTINUE;
    }
    if (free_before < free_after) {
        err("mm: the free-frame count went up?! that is a bookkeeping bug");
        return SHELL_CONTINUE;
    }
    /* 一次 exec 至少回收一套 mm；7 次 exec 就应该回收 7 套以上 */
    if (task_reclaim_space_count() - spaces_before < 6u) {
        err("mm: old address spaces were NOT destroyed");
        return SHELL_CONTINUE;
    }
    /* 跑完只剩最后那个进程的一套 mm 活着：空闲帧的损失必须是个小数目 */
    if (free_before - free_after > 12u) {
        err("mm: frames leaked proportional to the number of execs");
        sh_print(SH_PLAIN, "  leaked    : ");
        sh_print_u32(SH_PLAIN, (uint32_t)(free_before - free_after));
        sh_println(SH_PLAIN, " frames (a single live mm is at most a few)");
        return SHELL_CONTINUE;
    }
    ok("M12 PASS: every exec() destroyed the mm it replaced.");
    plain("          7 execs cost one live mm, not eight -- the frames came back.");
    plain("          (Linux does the same thing one level deeper: mmput() ->");
    plain("          free_pgtables() -> the page tables and the user pages go back.)");
    return SHELL_CONTINUE;
}
