/* cmd_elf.c - M6 的 elf 命令：解析嵌进来的 hello.elf，装载、进 ring3 跑、回来校验 */

#include "cmd_elf.h"

#include "shell.h"
#include "../kernel/elf.h"
#include "../kernel/syscall.h"   /* [ADD] M7：syscall_set_write_log() */
#include "../fs/ramfs.h"   /* [ADD] M8：按路径读 ELF */
#include "../kernel/task.h"

/*
 * build.sh 用 ld -r -b binary hello.elf 把这个 ELF 原样嵌成内核里的一个目标文件，链接器会
 * 自动生成三个符号：_start / _end / _size。相当于 Linux 的 initrd，内核镜像里带着文件内容，
 * 但没有文件系统。
 */
extern const uint8_t _binary_hello_elf_start[];
extern const uint8_t _binary_hello_elf_end[];
extern const uint8_t _binary_forkdemo_elf_start[];
extern const uint8_t _binary_forkdemo_elf_end[];

static void show_hex64(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char buf[19];
    uint32_t i;

    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0u; i < 16u; ++i) {
        buf[2u + i] = digits[(value >> (60u - 4u * i)) & 0xFu];
    }
    buf[18] = '\0';
    sh_print(SH_PLAIN, buf);
}

/* [M7] `elf fork`：跑 forkdemo.elf，验证 fork / wait / 地址空间复制   */

static int run_forkdemo(void)
{
    const uint8_t *blob = _binary_forkdemo_elf_start;
    const uint64_t size =
        (uint64_t)(uintptr_t)(_binary_forkdemo_elf_end - _binary_forkdemo_elf_start);
    elf_image_t img;
    const task_t *t;
    uint32_t forks;
    uint32_t waits;
    int pid;

    task_reset();
    syscall_set_write_log(0);      /* 演示程序要打十几行，关掉逐行内核日志 */

    info("M7 fork/wait demo: loading an ELF that forks and waits");
    sh_print(SH_PLAIN, "  source    : build/forkdemo.elf (user/forkdemo.c, compiled by gcc)");
    sh_newline();
    sh_print(SH_PLAIN, "  blob      : ");
    sh_print_u32(SH_PLAIN, (uint32_t)size);
    sh_print(SH_PLAIN, " bytes");
    pid = task_create_elf("forkdemo", blob, size, &img);
    if (pid < 0) {
        err("elf fork: could not create the parent process");
        syscall_set_write_log(1);
        return SHELL_CONTINUE;
    }
    sh_print(SH_PLAIN, "  load      : segments=");
    sh_print_u32(SH_PLAIN, img.segments);
    sh_print(SH_PLAIN, "  pages=");
    sh_print_u32(SH_PLAIN, img.pages);
    sh_print(SH_PLAIN, "  .bss=");
    sh_print_u32(SH_PLAIN, img.has_bss);
    sh_newline();
    sh_print(SH_PLAIN, "  entry     : ");
    show_hex64(img.entry);
    sh_print(SH_PLAIN, "   stack top ");
    show_hex64(VMM_USER_BASE + VMM_USER_SPAN - 16u);
    sh_newline();
    plain("run: parent forks twice, each child exits with its own code,");
    plain("     and wait() blocks the parent until the child is gone.");

    task_run_all();

    t = task_lookup((uint32_t)pid);
    forks = task_fork_count();
    waits = task_wait_count();
    syscall_set_write_log(1);

    if (t == (const task_t *)0) {
        err("elf fork: the parent TCB vanished");
        return SHELL_CONTINUE;
    }

    sh_print(SH_PLAIN, "  after run        : state=");
    sh_print(SH_PLAIN, (t->state == TASK_DEAD) ? "DEAD" : "??");
    sh_print(SH_PLAIN, "  exit code=");
    sh_print_u32(SH_PLAIN, (uint32_t)t->exit_code);
    sh_print(SH_PLAIN, "  forks=");
    sh_print_u32(SH_PLAIN, forks);
    sh_print(SH_PLAIN, "  waits=");
    sh_print_u32(SH_PLAIN, waits);
    sh_print(SH_PLAIN, "  faults=");
    sh_print_u32(SH_PLAIN, (uint32_t)t->fault_vector);
    sh_newline();

    if (t->fault_vector != 0u) {
        err("elf fork: the parent died on an exception — see `tasks`");
        return SHELL_CONTINUE;
    }
    if (forks < 2u || waits < 2u) {
        err("elf fork: expected at least 2 forks and 2 waits");
        return SHELL_CONTINUE;
    }
    if (t->state != TASK_DEAD || t->exit_code != 0) {
        err("elf fork: the parent did not exit cleanly with 0");
        return SHELL_CONTINUE;
    }

    ok("M7 PASS: fork duplicated a process (address space + register frame),");
    plain("         the parent blocked in wait(), children ran and exited 11 / 22,");
    plain("         and the parent collected both codes before exiting 0.");
    return SHELL_CONTINUE;
}

/* [M8] `elf exec`：按路径从 ramfs 装载，然后让它自己 exec 掉自己      */

static uint8_t g_file_buf[64u * 1024u];

static int run_execdemo(void)
{
    const char *path = "/bin/execdemo.elf";
    uint64_t len = 0u;
    elf_image_t img;
    const task_t *t;
    int rc;
    int pid;

    task_reset();
    syscall_set_write_log(0);   /* 演示有十几行输出，关掉逐行 sys_write 日志 */

    info("M8 exec demo: load /bin/execdemo.elf from ramfs, it will exec /bin/hello.elf");
    rc = ramfs_read_file(path, g_file_buf, (uint64_t)sizeof(g_file_buf), &len);
    if (rc != RAMFS_OK) {
        err("elf exec: cannot open the file");
        plain(ramfs_strerror(rc));
        return SHELL_CONTINUE;
    }
    sh_print(SH_PLAIN, "  opened    : ");
    sh_print(SH_PLAIN, path);
    sh_print(SH_PLAIN, "  ");
    sh_print_u32(SH_PLAIN, (uint32_t)len);
    sh_println(SH_PLAIN, " bytes  (read by path, no fd table yet)");

    pid = task_create_elf("execdemo", g_file_buf, len, &img);
    if (pid < 0) {
        err("elf exec: could not create the process");
        return SHELL_CONTINUE;
    }
    sh_print(SH_PLAIN, "  created   : pid ");
    sh_print_u32(SH_PLAIN, (uint32_t)pid);
    sh_print(SH_PLAIN, "  entry ");
    show_hex64(img.entry);
    sh_print(SH_PLAIN, "  pages ");
    sh_print_u32(SH_PLAIN, img.pages);
    sh_newline();
    plain("run: the process will print its pid, then replace itself via exec()");

    task_run_all();

    t = task_lookup((uint32_t)pid);
    if (t == (const task_t *)0) {
        err("elf exec: the TCB vanished");
        return SHELL_CONTINUE;
    }

    sh_print(SH_PLAIN, "  after run        : pid ");
    sh_print_u32(SH_PLAIN, t->pid);
    sh_print(SH_PLAIN, "  state=");
    sh_print(SH_PLAIN, (t->state == TASK_DEAD) ? "DEAD" : "??");
    sh_print(SH_PLAIN, "  exit code=");
    sh_print_u32(SH_PLAIN, (uint32_t)t->exit_code);
    sh_print(SH_PLAIN, "  execs=");
    sh_print_u32(SH_PLAIN, task_exec_count());
    sh_print(SH_PLAIN, "  faults=");
    sh_print_u32(SH_PLAIN, (uint32_t)t->fault_vector);
    sh_newline();

    if (t->fault_vector != 0u) {
        err("elf exec: the task died on an exception");
        return SHELL_CONTINUE;
    }
    if (task_exec_count() < 1u) {
        err("elf exec: exec() was never performed");
        return SHELL_CONTINUE;
    }
    if (t->exit_code != 42) {
        err("elf exec: exit code is not 42 — the NEW image did not run");
        return SHELL_CONTINUE;
    }

    ok("M8 PASS: exec() opened an ELF by path from ramfs, built a fresh");
    plain("         space, replaced the running image in place (same pid), and the");
    plain("         new program exited 42 -- the old program never came back.");
    return SHELL_CONTINUE;
}

int cmd_elf(int argc, char **argv)
{
    const uint8_t *blob = _binary_hello_elf_start;
    const uint64_t size =
        (uint64_t)(uintptr_t)(_binary_hello_elf_end - _binary_hello_elf_start);
    elf_image_t img;
    const task_t *t;
    int pid;

    /* `elf parse`：只解析不装载。目的很实际 ， VGA 文本只有 25 行，
     * 完整流程的输出会把"解析"那一段滚出屏幕，单独跑一次才看得见。 */
    if (argc >= 2 && argv[1] != 0 && argv[1][0] == 'f') {
        return run_forkdemo();      /* [ADD] M7：elf fork */
    }
    if (argc >= 2 && argv[1] != 0 && argv[1][0] == 'e') {
        return run_execdemo();      /* [ADD] M8：elf exec */
    }

    if (argc >= 2 && argv[1] != 0 && argv[1][0] == 'p') {
        info("ELF parse only (no load, no ring3): build/hello.elf");
        sh_print(SH_PLAIN, "  blob size : ");
        sh_print_u32(SH_PLAIN, (uint32_t)size);
        sh_print(SH_PLAIN, " bytes, embedded in the kernel image (no file system yet)");
        sh_newline();
        elf_report(blob, size);
        plain("next: run `elf` to load it into a process and execute it");
        return SHELL_CONTINUE;
    }

    /* 每次进来都清空 TCB，保证不是"上次那个进程还在跑"的假象 */
    task_reset();

    info("ELF loader (M6): loading an externally compiled executable");
    sh_print(SH_PLAIN, "  source    : build/hello.elf, embedded by `ld -r -b binary`");
    sh_newline();
    sh_print(SH_PLAIN, "  blob      : ");
    sh_print_u32(SH_PLAIN, (uint32_t)size);
    sh_print(SH_PLAIN, " bytes, embedded in the kernel image (no file system yet)");
    sh_newline();

    /* 第一步：只读文件格式，证明内核真的在解析 ELF */
    plain("parse (this is what linux's load_elf_binary() starts with):");
    elf_report(blob, size);

    /* 第二步：建进程 + 按 PT_LOAD 装载 */
    pid = task_create_elf("hello", blob, size, &img);
    if (pid < 0) {
        err("elf: could not create the process");
        return SHELL_CONTINUE;
    }

    plain("load:");
    sh_print(SH_PLAIN, "  PT_LOAD segments : ");
    sh_print_u32(SH_PLAIN, img.segments);
    sh_print(SH_PLAIN, "   pages mapped: ");
    sh_print_u32(SH_PLAIN, img.pages);
    sh_print(SH_PLAIN, " (4 KB each)   .bss: ");
    sh_print_u32(SH_PLAIN, img.has_bss);
    sh_newline();
    sh_print(SH_PLAIN, "  entry            : ");
    show_hex64(img.entry);
    sh_print(SH_PLAIN, "   user stack: ");
    show_hex64(task_va_base((uint32_t)pid) + VMM_USER_SPAN - 16u);
    sh_newline();
    sh_print(SH_PLAIN, "  pid              : ");
    sh_print_u32(SH_PLAIN, (uint32_t)pid);
    sh_print(SH_PLAIN, "   cr3: ");
    show_hex64(task_cr3((uint32_t)pid));
    sh_newline();
    plain("run (ring3, rip = e_entry, own CR3, own stack):");

    /* 第三步：交给调度器，进 ring3 */
    task_run_all();

    /* 第四步：回到内核，看 TCB 里记了什么 */
    t = task_lookup((uint32_t)pid);
    if (t == (const task_t *)0) {
        err("elf: the TCB vanished after the run");
        return SHELL_CONTINUE;
    }

    sh_print(SH_PLAIN, "  after run        : state=");
    sh_print(SH_PLAIN, (t->state == TASK_DEAD) ? "DEAD" : "??");
    sh_print(SH_PLAIN, "  exit code=");
    sh_print_u32(SH_PLAIN, (uint32_t)t->exit_code);
    sh_print(SH_PLAIN, "  ticks=");
    sh_print_u32(SH_PLAIN, (uint32_t)t->ticks);
    sh_print(SH_PLAIN, "  fault vector=");
    sh_print_u32(SH_PLAIN, (uint32_t)t->fault_vector);
    sh_newline();

    if (t->fault_vector != 0u) {
        err("elf: the program died on an exception — see `tasks` for cr2/rip");
        return SHELL_CONTINUE;
    }
    if (t->state != TASK_DEAD) {
        err("elf: the program did not exit");
        return SHELL_CONTINUE;
    }
    if (t->exit_code != 42) {
        err("elf: exit code is not 42 (sys_exit did not carry the argument through)");
        return SHELL_CONTINUE;
    }

    ok("M6 PASS: an externally compiled ELF64 was parsed, loaded segment by segment");
    plain("         into a private 4KB page table, entered at e_entry in ring3,");
    plain("         and exited with code 42 through int 0x80.");
    return SHELL_CONTINUE;
}
