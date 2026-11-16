/* cmd_fd.c - M9 的 fd 命令：按路径装 /bin/fddemo.elf，让用户态自己 open/read/close。
 * 跟 M8 的 elf exec 一样，程序是 ramfs 里的文件；不同的是这次读文件的人换成了用户程序自己，
 * 内核只留 open/read/close 三道门，fd 表和读偏移挂在 TCB 里。 */

#include "cmd_fd.h"

#include "shell.h"
#include "../kernel/elf.h"
#include "../kernel/task.h"
#include "../kernel/syscall.h"
#include "../fs/ramfs.h"
#include "../fs/file.h"

static uint8_t g_blob[64u * 1024u];

/* 本地十六进制打印（cmd_elf.c 里的 show_hex64 是那边的 static，这里自己来一份） */
static void fd_show_hex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char buf[16];
    int shift;

    sh_print(SH_PLAIN, "0x");
    for (shift = 60; shift >= 0; shift -= 4) {
        buf[(60 - shift) / 4] = digits[(value >> shift) & 0xFu];
    }
    for (shift = 0; shift < 16; ++shift) {
        char one[2];
        one[0] = buf[shift];
        one[1] = '\0';
        sh_print(SH_PLAIN, one);
    }
}

int cmd_fd(int argc, char **argv)
{
    const char *path = "/bin/fddemo.elf";
    uint64_t len = 0u;
    elf_image_t img;
    const task_t *t;
    int pid;
    int rc;

    (void)argc;
    (void)argv;

    task_reset();
    syscall_set_write_log(0);   /* 演示输出全靠用户态自己 write，不需要逐行内核日志 */

    info("M9 fd table demo: the ring3 program opens a file by itself");
    rc = ramfs_read_file(path, g_blob, (uint64_t)sizeof(g_blob), &len);
    if (rc != RAMFS_OK) {
        err("fd: cannot open the demo program");
        plain(ramfs_strerror(rc));
        return SHELL_CONTINUE;
    }
    sh_print(SH_PLAIN, "  program   : ");
    sh_print(SH_PLAIN, path);
    sh_print(SH_PLAIN, "  ");
    sh_print_u32(SH_PLAIN, (uint32_t)len);
    sh_println(SH_PLAIN, " bytes");

    pid = task_create_elf("fddemo", g_blob, len, &img);
    if (pid < 0) {
        err("fd: could not create the process");
        return SHELL_CONTINUE;
    }
    sh_print(SH_PLAIN, "  created   : pid ");
    sh_print_u32(SH_PLAIN, (uint32_t)pid);
    sh_print(SH_PLAIN, "  entry ");
    fd_show_hex(img.entry);
    sh_print(SH_PLAIN, "  pages ");
    sh_print_u32(SH_PLAIN, img.pages);
    sh_newline();
    plain("run: every byte below is read by the ring3 program, not by the kernel");

    task_run_all();

    t = task_lookup((uint32_t)pid);
    if (t == (const task_t *)0) {
        err("fd: the TCB vanished");
        return SHELL_CONTINUE;
    }

    sh_print(SH_PLAIN, "  fd table of the process (ZH_MAX_FDS=");
    sh_print_u32(SH_PLAIN, (uint32_t)ZH_MAX_FDS);
    sh_println(SH_PLAIN, "):");
    sh_print(SH_PLAIN, "    open ok=");
    sh_print_u32(SH_PLAIN, file_open_ok_count());
    sh_print(SH_PLAIN, "  open denied=");
    sh_print_u32(SH_PLAIN, file_open_fail_count());
    sh_print(SH_PLAIN, "  read=");
    sh_print_u32(SH_PLAIN, file_read_count());
    sh_print(SH_PLAIN, "  close=");
    sh_print_u32(SH_PLAIN, file_close_count());
    sh_newline();
    sh_print(SH_PLAIN, "  after run        : state=");
    sh_print(SH_PLAIN, (t->state == TASK_DEAD) ? "DEAD" : "??");
    sh_print(SH_PLAIN, "  exit code=");
    sh_print_u32(SH_PLAIN, (uint32_t)t->exit_code);
    sh_print(SH_PLAIN, "  faults=");
    sh_print_u32(SH_PLAIN, (uint32_t)t->fault_vector);
    sh_newline();

    if (t->fault_vector != 0u) {
        err("fd: the task died on an exception");
        return SHELL_CONTINUE;
    }
    if (t->exit_code != 0) {
        err("fd: the demo program reported a failed check (exit code != 0)");
        return SHELL_CONTINUE;
    }
    if (file_open_ok_count() < 1u || file_open_fail_count() < 2u ||
        file_read_count() < 2u || file_close_count() < 1u) {
        err("fd: the counters do not match what the program claims to have done");
        return SHELL_CONTINUE;
    }

    ok("M9 PASS: a ring3 program opened an ELF by path, read its 7f 45 4c 46");
    plain("         magic through a fd, read again with the offset moved on,");
    plain("         lost access after close(), and was refused for a missing");
    plain("         file and for a directory. fd is an index into a per-task");
    plain("         table, exactly like Linux.");
    return SHELL_CONTINUE;
}
