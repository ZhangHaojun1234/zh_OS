/* ============================================================================
 * zh_OS 1.0 (x86_64)
 * File:    shell/cmd_argv.c
 * Version: 1.0
 * Brief:   M11 `argv` 命令：从 ramfs 装载 /bin/argvdemo.elf 并跑起来。
 *
 * 这一步证明的是：argc/argv 能**跨过 exec 边界** —— 父进程 fork 出子进程，
 * 子进程 exec 同一个程序但带上三个参数，于是新映像的 _start 里 argc=3、
 * argv[1]="alpha" …… 而这些字符串是内核在 exec 时拷进**新**地址空间栈页的。
 * ============================================================================ */

#include "cmd_argv.h"

#include "shell.h"
#include "../kernel/elf.h"
#include "../kernel/task.h"
#include "../kernel/syscall.h"
#include "../fs/ramfs.h"

static uint8_t g_blob[64u * 1024u];

int cmd_argv(int argc, char **argv)
{
    const char *path = "/bin/argvdemo.elf";
    const task_t *t;
    uint64_t len = 0u;
    elf_image_t img;
    uint32_t forks_before = task_fork_count();
    uint32_t execs_before = task_exec_count();
    uint32_t waits_before = task_wait_count();
    int pid;
    int rc;

    (void)argc;
    (void)argv;

    task_reset();
    syscall_set_write_log(0);

    info("M11 argv demo: exec() now carries argc/argv into the new image");
    rc = ramfs_read_file(path, g_blob, (uint64_t)sizeof(g_blob), &len);
    if (rc != RAMFS_OK) {
        err("argv: cannot open the demo program");
        plain(ramfs_strerror(rc));
        return SHELL_CONTINUE;
    }
    sh_print(SH_PLAIN, "  program   : ");
    sh_print(SH_PLAIN, path);
    sh_print(SH_PLAIN, "  ");
    sh_print_u32(SH_PLAIN, (uint32_t)len);
    sh_println(SH_PLAIN, " bytes");

    pid = task_create_elf("argvdemo", g_blob, len, &img);
    if (pid < 0) {
        err("argv: could not create the process");
        return SHELL_CONTINUE;
    }
    sh_print(SH_PLAIN, "  created   : pid 0, argc=0 (the kernel loader passes no argv)");
    sh_newline();
    plain("run: the parent forks; the child exec()s the same program with 3 args");

    task_run_all();

    t = task_lookup((uint32_t)pid);
    if (t == (const task_t *)0) {
        err("argv: the TCB vanished");
        return SHELL_CONTINUE;
    }
    sh_print(SH_PLAIN, "  after run        : state=");
    sh_print(SH_PLAIN, (t->state == TASK_DEAD) ? "DEAD" : "??");
    sh_print(SH_PLAIN, "  exit code=");
    sh_print_u32(SH_PLAIN, (uint32_t)t->exit_code);
    sh_print(SH_PLAIN, "  faults=");
    sh_print_u32(SH_PLAIN, (uint32_t)t->fault_vector);
    sh_newline();
    sh_print(SH_PLAIN, "  kernel counters  : forks +");
    sh_print_u32(SH_PLAIN, task_fork_count() - forks_before);
    sh_print(SH_PLAIN, "  execs +");
    sh_print_u32(SH_PLAIN, task_exec_count() - execs_before);
    sh_print(SH_PLAIN, "  waits +");
    sh_print_u32(SH_PLAIN, task_wait_count() - waits_before);
    sh_newline();

    if (t->fault_vector != 0u || t->exit_code != 0) {
        err("argv: the demo program reported a failure");
        return SHELL_CONTINUE;
    }
    if (task_exec_count() - execs_before < 1u) {
        err("argv: exec with argv never happened");
        return SHELL_CONTINUE;
    }
    ok("M11 PASS: exec() carried argc=3 and argv[] into the new image.");
    plain("         The strings live in the NEW address space's stack page (the");
    plain("         old mm is gone), and _start(argc, argv) received them in");
    plain("         rdi / rsi -- exactly like the real execve ABI.");
    return SHELL_CONTINUE;
}
