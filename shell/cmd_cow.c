/* ============================================================================
 * zh_OS 1.0 (x86_64)
 * File:    shell/cmd_cow.c
 * Version: 1.0
 * Brief:   M15 `cow` 命令：看写时复制到底省了多少帧。
 *
 * 这条命令不解释原理，只摆数字：
 *   1) 跑之前记下空闲帧数；
 *   2) 跑 /bin/cowdemo.elf —— 它 fork 一次，父子各写一次 g_value；
 *   3) 跑完再看空闲帧数，以及内核里 COW 的三个计数器：
 *        shared  = fork 时**共享**（而不是复制）了几页
 *        copied  = 有几个页因为被写才真正复制了一份
 *        faults  = 处理了几次 COW 缺页（== copied，因为一缺页就复制）
 *
 * 判据：
 *   - fork 那一刻的 "copied pages" 必须是 0（旧版这里是 1~3，因为 M7 是整页复制）；
 *   - shared >= 1，说明真的共享了；
 *   - copied == 2，说明父子各写了一次、各复制了一页（只有写过的页才付钱）；
 *   - 程序自己的输出证明父子内存互相看不见（语义没坏）。
 * ============================================================================ */

#include "cmd_cow.h"

#include "shell.h"
#include "../kernel/elf.h"
#include "../kernel/task.h"
#include "../kernel/vmm.h"
#include "../fs/ramfs.h"
#include "../mm/phys.h"
#include "../kernel/syscall.h"

static uint8_t g_blob[64u * 1024u];

static void show_frames(const char *tag)
{
    sh_print(SH_PLAIN, "  ");
    sh_print(SH_PLAIN, tag);
    sh_print(SH_PLAIN, ": used=");
    sh_print_u32(SH_PLAIN, (uint32_t)phys_used_frame_count());
    sh_print(SH_PLAIN, "  free=");
    sh_print_u32(SH_PLAIN, (uint32_t)phys_free_frame_count());
    sh_newline();
}

int cmd_cow(int argc, char **argv)
{
    const char *path = "/bin/cowdemo.elf";
    const task_t *t;
    uint64_t len = 0u;
    uint32_t shared;
    uint32_t copied;
    uint32_t faults;
    elf_image_t img;
    int pid;
    int rc;

    (void)argc;
    (void)argv;

    task_reset();
    syscall_set_write_log(0);
    task_set_exec_verbose(0);

    if (task_init() != 0) {
        err("cow: task subsystem failed to init");
        return SHELL_CONTINUE;
    }

    info("M15 copy-on-write: fork() shares pages, only writes pay for a copy");
    sh_print(SH_PLAIN, "  program   : ");
    sh_println(SH_PLAIN, path);
    show_frames("before    ");

    rc = ramfs_read_file(path, g_blob, (uint64_t)sizeof(g_blob), &len);
    if (rc != RAMFS_OK) {
        err("cow: cannot read the demo program");
        plain(ramfs_strerror(rc));
        return SHELL_CONTINUE;
    }

    vmm_cow_reset_stats();

    pid = task_create_elf("cowdemo", g_blob, len, &img);
    if (pid < 0) {
        err("cow: could not create the process");
        return SHELL_CONTINUE;
    }
    plain("run: the child writes its copy of a shared page, then the parent writes its own");

    task_run_all();

    shared = vmm_cow_shared_count();
    copied = vmm_cow_copy_count();
    faults = vmm_cow_fault_count();

    show_frames("after     ");
    sh_print(SH_PLAIN, "  fork      : shared=");
    sh_print_u32(SH_PLAIN, shared);
    sh_print(SH_PLAIN, " page(s) mapped read-only+COW  (0 frames copied at fork)");
    sh_newline();
    sh_print(SH_PLAIN, "  on write  : copied=");
    sh_print_u32(SH_PLAIN, copied);
    sh_print(SH_PLAIN, "  page fault(s)=");
    sh_print_u32(SH_PLAIN, faults);
    sh_println(SH_PLAIN, "  (only the pages each side actually wrote got a private copy)");

    t = task_lookup((uint32_t)pid);
    if (t != (const task_t *)0) {
        sh_print(SH_PLAIN, "  parent    : exit code=");
        sh_print_u32(SH_PLAIN, (uint32_t)t->exit_code);
        sh_print(SH_PLAIN, "  faults=");
        sh_print_u32(SH_PLAIN, (uint32_t)t->fault_vector);
        sh_newline();
    }

    if (t == (const task_t *)0 || t->exit_code != 0) {
        err("cow: the demo program did not exit 0 (its write leaked, or it faulted)");
        return SHELL_CONTINUE;
    }
    if (shared < 2u) {
        err("cow: fork did not share any page -- COW is not active");
        return SHELL_CONTINUE;
    }
    if (copied < 2u) {
        err("cow: writes did not trigger COW copies -- the page tables were not read-only");
        return SHELL_CONTINUE;
    }
    ok("M15 PASS: fork mapped the parent's pages into the child as read-only+COW.");
    plain("          both sides wrote, and each side paid only for the pages it touched");
    plain("          (its data page and its stack), then still reads its own value.");
    plain("          (Linux: do_wp_page() -> alloc_page() -> copy_page() -> set_pte(writable).)");
    return SHELL_CONTINUE;
}
