/* syscall.h - 调用号与分发器接口 */

#ifndef ZH_OS_SYSCALL_H
#define ZH_OS_SYSCALL_H

#include <stdint.h>

struct regs;   /* 中断帧，定义在 kernel/isr.h */

/*
 * 系统调用 ABI，刻意贴着 Linux 的 i386 老 ABI，将来接 POSIX 最省事：rax 是调用号，rdi/rsi/rdx
 * 是参数 1/2/3，返回值写回 rax。从 ring3 用 int 0x80 触发。
 * 不用 syscall/sysret 是因为那条路要把 IA32_STAR、IA32_LSTAR、IA32_FMASK 三个 MSR 都配好，
 * sysret 还有个老坑，回到非规范地址就 #GP。int 0x80 只要一个 IDT 门，出事顶多是 #GP 或 #PF，
 * 好查得多。M2 先用 int 0x80 把系统调用跑通，M4 之后再考虑换指令提速。
 */

#define ZH_SYS_WRITE 1u   /* (const char *buf, u64 len) -> 打印的字节数，越界返回 -1 */
#define ZH_SYS_TICKS 2u   /* () -> 开机以来的 PIT tick 数 */
#define ZH_SYS_EXIT  3u   /* (int code) -> 不返回：把 CPU 交回内核 */
#define ZH_SYS_CPL   4u   /* () -> 调用者的 CS & 3（正常应为 3） */

// [ADD] M4：多任务需要的两个新调用
#define ZH_SYS_PUTC  5u   /* (char c) -> 0：往 tty0 打一个字符（不走 sys_write 那一行日志） */
#define ZH_SYS_SLEEP 6u   /* (u64 ticks) -> 实际睡掉的 tick 数：hlt 循环让出 CPU */

// [ADD] M7：进程相关的三个调用（fork / getpid / wait）
#define ZH_SYS_FORK  7u   /* () -> 父进程拿到子进程 pid，子进程拿到 0；失败 -1 */
#define ZH_SYS_GETPID 8u  /* () -> 当前任务 pid */
#define ZH_SYS_WAIT  9u   /* () -> 子进程退出码（会阻塞）；没有子进程返回 -1 */
#define ZH_SYS_EXEC  10u  /* (const char *path) -> 不成功返回 -1；成功则映像被替换、不再是原来的程序 */
#define ZH_SYS_OPEN  11u  /* [M9] (const char *path) -> fd 下标（>=0）；失败 -1 */
#define ZH_SYS_READ  12u  /* [M9] (int fd, char *buf, size_t len) -> 读到的字节数；0 = EOF；失败 -1 */
#define ZH_SYS_CLOSE 13u  /* [M9] (int fd) -> 0 成功，-1 fd 非法 */
#define ZH_SYS_CREAT 14u  /* [M10] (const char *path) -> 写模式 fd（新建或清空）；失败 -1 */
#define ZH_SYS_FWRITE 15u /* [M10] (int fd, const char *buf, size_t len) -> 写入字节数；失败 -1 */
#define ZH_SYS_UNLINK 16u /* [M10] (const char *path) -> 0 成功，-1 失败 */

/*
 * [ADD] M13：shell 降到 ring3 之后，它需要的就不再是"内核里的函数"了，
 * 而是这三个调用 ， 键盘、目录、当前路径。
 */
#define ZH_SYS_READKEY 17u /* [M13] () -> 下一个字符（0x00..0xFF）；缓冲区空返回 0 */
#define ZH_SYS_CHDIR  18u  /* [M13] (const char *path) -> 0 成功，-1 = 不是目录/不存在 */
#define ZH_SYS_GETCWD 19u  /* [M13] (char *buf, size_t maxlen) -> 写进去的字节数 */
/* [M14] (const char *path, uint32_t index, char *name, uint32_t cap)
 *      -> 0 = 文件，1 = 目录，-1 = 枚举结束或出错（Linux getdents 的雏形） */
#define ZH_SYS_READDIR 20u

/*
 * [ADD] M17：信号和 waitpid。kill(pid, sig) 投一个信号，对应 Linux sys_kill；waitpid(pid)
 * 等指定子进程，相当于 sys_wait4 只取 pid 那一个参数。还是 rdi/rsi/rdx 三个参数位。
 */
#define ZH_SYS_WAITPID 21u /* [M17] (int pid, uint32 flags) -> 退出码 / 0x10000|sig / 0 / -1 */
#define ZH_SYS_KILL    22u /* [M17] (int pid, int sig) -> 0 成功，-1 = pid 不存在 */
#define ZH_SYS_SETFG   23u /* [M17] (int pid) -> 0：把 pid 标成前台进程；pid<0 清空前台 */

/*
 * [ADD] M18：信号捕获
 *   signal(sig, handler) ， 装 handler（Linux sys_signal / sigaction 的核心字段）
 *   sigreturn()          ， 从 handler 回来时把整帧还原（Linux __NR_rt_sigreturn）
 *   getstate(pid)        ， 查任务状态（Linux 用 /proc/<pid>/stat，本版直接问内核）
 *
 * 注意 SYS_SIGRETURN 的号 写进了用户态蹦床页的机器码里（`mov eax,24; int 0x80`），
 * 改号就必须同时改 kernel/task.c 的 task_install_trampoline()。
 */
#define ZH_SYS_SIGNAL    24u /* [M18] (int sig, uint64 handler) -> 旧的 handler；-1 失败 */
#define ZH_SYS_SIGRETURN 25u /* [M18] () -> 恢复 sig_frame；不在 handler 里返回 -1 */
#define ZH_SYS_GETSTATE  26u /* [M18] (int pid) -> 0 FREE 1 READY 2 RUNNING 3 DEAD 4 WAIT 5 STOP */

/* [M17] 内核侧：把某个 pid 标成前台进程（zhsh3 在 fork 之后、wait 之前调） */
void syscall_mark_foreground(int pid);

#define ZH_SYSCALL_VECTOR 0x80u

/* 装 IDT 0x80 门（属性 0xEE：P=1 DPL=3 中断门）。必须在 idt_init() 之后调用。 */
void syscall_init(void);

/* 由 isr.c 的 interrupt_dispatch 在向量 0x80 上调用。 */
void syscall_dispatch(struct regs *r);

/* [ADD] M7：控制 sys_write 是否打印"[kernel] sys_write: N bytes"这行日志。
 * 默认开；一次要打十几行的演示程序（forkdemo）先关掉，免得刷屏。 */
void syscall_set_write_log(int on);

/* `syscall` 命令的实现主体：拼一段用户程序 -> 进 ring3 -> 由 sys_exit 回来。 */
void syscall_self_test(void);

#endif /* ZH_OS_SYSCALL_H */
