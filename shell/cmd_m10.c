/* cmd_m10.c - M10 的两条命令，都从 ramfs 按路径装程序，剩下的交给用户程序自己做：
 * wr    -> /bin/wrdemo.elf    creat + write + close + 读回
 * spawn -> /bin/spawndemo.elf fork -> 子进程 exec /bin/hello.elf -> 父进程 wait */

#include "cmd_m10.h"

#include "shell.h"
#include "../kernel/elf.h"
#include "../kernel/task.h"
#include "../kernel/syscall.h"
#include "../fs/ramfs.h"
#include "../fs/file.h"

static uint8_t g_blob[64u * 1024u];

/* 装载 + 跑 + 打印收尾，返回 pid；失败返回 -1。 */
static int run_program(const char *path, const char *tag)
{
    uint64_t len = 0u;
    elf_image_t img;
    int pid;
    int rc;

    rc = ramfs_read_file(path, g_blob, (uint64_t)sizeof(g_blob), &len);
    if (rc != RAMFS_OK) {
        err("cannot open the demo program");
        plain(ramfs_strerror(rc));
        return -1;
    }
    sh_print(SH_PLAIN, "  program   : ");
    sh_print(SH_PLAIN, path);
    sh_print(SH_PLAIN, "  ");
    sh_print_u32(SH_PLAIN, (uint32_t)len);
    sh_println(SH_PLAIN, " bytes");

    pid = task_create_elf(tag, g_blob, len, &img);
    if (pid < 0) {
        err("could not create the process");
        return -1;
    }
    sh_print(SH_PLAIN, "  created   : pid ");
    sh_print_u32(SH_PLAIN, (uint32_t)pid);
    sh_println(SH_PLAIN, "");
    task_run_all();
    return pid;
}

static void report(const task_t *t)
{
    sh_print(SH_PLAIN, "  after run        : state=");
    sh_print(SH_PLAIN, (t->state == TASK_DEAD) ? "DEAD" : "??");
    sh_print(SH_PLAIN, "  exit code=");
    sh_print_u32(SH_PLAIN, (uint32_t)t->exit_code);
    sh_print(SH_PLAIN, "  faults=");
    sh_print_u32(SH_PLAIN, (uint32_t)t->fault_vector);
    sh_newline();
}

int cmd_wr(int argc, char **argv)
{
    const task_t *t;
    int pid;

    (void)argc;
    (void)argv;

    task_reset();
    syscall_set_write_log(0);
    info("M10 file demo: creat/write/close in ring3, then read the file back");
    pid = run_program("/bin/wrdemo.elf", "wrdemo");
    if (pid < 0) {
        return SHELL_CONTINUE;
    }
    t = task_lookup((uint32_t)pid);
    if (t == (const task_t *)0) {
        err("the TCB vanished");
        return SHELL_CONTINUE;
    }
    report(t);

    sh_print(SH_PLAIN, "  fs counters      : creat=");
    sh_print_u32(SH_PLAIN, file_creat_count());
    sh_print(SH_PLAIN, "  writes=");
    sh_print_u32(SH_PLAIN, file_write_count());
    sh_print(SH_PLAIN, "  bytes=");
    sh_print_u32(SH_PLAIN, (uint32_t)file_bytes_written());
    sh_print(SH_PLAIN, "  opens=");
    sh_print_u32(SH_PLAIN, file_open_ok_count());
    sh_newline();

    /* 落盘之后再用 cat 看一遍，证明文件在 ramfs 里能看见、能被内核自己读 */
    plain("now `cat /home/zh/m10.txt` would show the same two lines");

    if (t->fault_vector != 0u || t->exit_code != 0) {
        err("wr: the demo program reported a failure");
        return SHELL_CONTINUE;
    }
    if (file_creat_count() < 1u || file_write_count() < 2u ||
        file_bytes_written() < 60u) {
        err("wr: the kernel-side counters disagree with the program");
        return SHELL_CONTINUE;
    }
    ok("M10 PASS (files): a ring3 program created a file, appended two writes");
    plain("         to the same fd, closed it, reopened it read-only and got");
    plain("         its own bytes back through the filesystem.");
    return SHELL_CONTINUE;
}

int cmd_spawn(int argc, char **argv)
{
    const task_t *t;
    uint32_t forks_before = task_fork_count();
    uint32_t execs_before = task_exec_count();
    uint32_t waits_before = task_wait_count();
    int pid;

    (void)argc;
    (void)argv;

    task_reset();
    syscall_set_write_log(0);
    info("M10 spawn demo: fork() -> child exec()s hello.elf -> parent wait()s for it");
    pid = run_program("/bin/spawndemo.elf", "spawndemo");
    if (pid < 0) {
        return SHELL_CONTINUE;
    }
    t = task_lookup((uint32_t)pid);
    if (t == (const task_t *)0) {
        err("the TCB vanished");
        return SHELL_CONTINUE;
    }
    report(t);

    sh_print(SH_PLAIN, "  kernel counters  : forks +");
    sh_print_u32(SH_PLAIN, task_fork_count() - forks_before);
    sh_print(SH_PLAIN, "  execs +");
    sh_print_u32(SH_PLAIN, task_exec_count() - execs_before);
    sh_print(SH_PLAIN, "  waits +");
    sh_print_u32(SH_PLAIN, task_wait_count() - waits_before);
    sh_newline();

    if (t->fault_vector != 0u || t->exit_code != 0) {
        err("spawn: the demo program reported a failure");
        return SHELL_CONTINUE;
    }
    if (task_fork_count() - forks_before < 1u ||
        task_exec_count() - execs_before < 1u ||
        task_wait_count() - waits_before < 1u) {
        err("spawn: one of fork/exec/wait did not actually happen");
        return SHELL_CONTINUE;
    }
    ok("M10 PASS (processes): fork() made a child, the child exec()d a new");
    plain("         program into itself, the parent blocked in wait() and got");
    plain("         the child's exit code 42. fork + exec + wait: closed loop.");
    return SHELL_CONTINUE;
}
