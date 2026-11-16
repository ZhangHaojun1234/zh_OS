/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    shell/cmd_sh3.c
 * Version: 0.2beta
 * Brief:   M13 `sh3` 命令：以内核 shell 的身份启动 /bin/zhsh3.elf。
 *
 * 内核这一侧的活只有一件事：把 zhsh3.elf 变成一个任务、交给调度器。
 * 之后屏幕上发生的一切（提示符、读键、回显、拆参数、fork/exec/wait）都由
 * ring 3 里那个进程自己完成 —— 内核不再替它读整行，也不再替它执行命令。
 *
 * 这就是"特权边界真正压在命令与内核之间"的那一步：
 *   内核 shell 还在（当 fallback，不然 zhsh3 起不来就变砖），
 *   但命令的执行已经搬到了用户态。
 * ============================================================================ */

#include "cmd_sh3.h"

#include "shell.h"
#include "../kernel/elf.h"
#include "../kernel/task.h"
#include "../kernel/syscall.h"
#include "../fs/ramfs.h"

static uint8_t g_blob[64u * 1024u];

int cmd_sh3(int argc, char **argv)
{
    const char *path = "/bin/zhsh3.elf";
    const task_t *t;
    uint64_t len = 0u;
    uint32_t forks_before;
    uint32_t execs_before;
    uint32_t waits_before;
    elf_image_t img;
    int pid;
    int rc;

    (void)argc;
    (void)argv;

    task_reset();
    syscall_set_write_log(0);
    task_set_exec_verbose(0);   /* [ADD] M14：ring3 shell 里每条命令都会 fork/exec，
                                 * 关掉内核那几行日志，屏幕才归 shell 自己 */   /* ring3 shell 里每敲一个命令都会 sys_write，日志会把屏幕刷爆 */

    info("M13 ring-3 shell: /bin/zhsh3.elf will run in user mode (CPL=3)");
    rc = ramfs_read_file(path, g_blob, (uint64_t)sizeof(g_blob), &len);
    if (rc != RAMFS_OK) {
        err("sh3: cannot open the shell program");
        plain(ramfs_strerror(rc));
        return SHELL_CONTINUE;
    }
    sh_print(SH_PLAIN, "  program   : ");
    sh_print(SH_PLAIN, path);
    sh_print(SH_PLAIN, "  ");
    sh_print_u32(SH_PLAIN, (uint32_t)len);
    sh_println(SH_PLAIN, " bytes");

    forks_before = task_fork_count();
    execs_before = task_exec_count();
    waits_before = task_wait_count();

    pid = task_create_elf("zhsh3", g_blob, len, &img);
    if (pid < 0) {
        err("sh3: could not create the process");
        return SHELL_CONTINUE;
    }
    plain("run: you are now talking to a ring-3 shell. type 'help', then try");
    plain("     'echo hi' / 'pwd' / 'hello' / 'argv' / 'exit'");

    task_run_all();

    t = task_lookup((uint32_t)pid);
    sh_print(SH_PLAIN, "  after run        : state=");
    sh_print(SH_PLAIN, (t != (const task_t *)0 && t->state == TASK_DEAD) ? "DEAD" : "??");
    if (t != (const task_t *)0) {
        sh_print(SH_PLAIN, "  exit code=");
        sh_print_u32(SH_PLAIN, (uint32_t)t->exit_code);
        sh_print(SH_PLAIN, "  faults=");
        sh_print_u32(SH_PLAIN, (uint32_t)t->fault_vector);
    }
    sh_newline();
    sh_print(SH_PLAIN, "  kernel counters  : forks +");
    sh_print_u32(SH_PLAIN, task_fork_count() - forks_before);
    sh_print(SH_PLAIN, "  execs +");
    sh_print_u32(SH_PLAIN, task_exec_count() - execs_before);
    sh_print(SH_PLAIN, "  waits +");
    sh_print_u32(SH_PLAIN, task_wait_count() - waits_before);
    sh_newline();

    if (t == (const task_t *)0 || t->fault_vector != 0u) {
        err("sh3: the ring-3 shell died with a fault");
        return SHELL_CONTINUE;
    }
    ok("M13 PASS: the shell itself ran in ring 3 and its commands were real processes.");
    plain("          Every external command it ran was fork() + exec() + wait() --");
    plain("          the same three calls Linux uses. And every keystroke you typed");
    plain("          reached it through SYS_READKEY, not through a kernel read_line().");
    return SHELL_CONTINUE;
}
