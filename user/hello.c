/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    user/hello.c
 * Version: 0.2beta
 * Brief:   M6 的第一个"真程序"：用 gcc 编出来的 ELF64 可执行文件。
 *
 * 它和 kernel/task.c 里那些手拼机器码的任务有本质区别：
 *   - 这个文件是**独立编译**出来的标准 ELF64（ET_EXEC），有自己的
 *     程序头表、.text/.rodata/.data 段、入口符号 _start；
 *   - 内核不认识它，只知道"读 ELF 头 -> 按 PT_LOAD 逐段搬进进程地址空间"；
 *   - 它用 int 0x80 直接做系统调用（ABI：rax=号，rdi/rsi/rdx=参数）。
 * 换句话说：从 M6 起，"程序"不再是内核的一部分，而是内核加载的对象。
 * ============================================================================ */

typedef unsigned long u64;

#define ZH_SYS_WRITE 1ul
#define ZH_SYS_EXIT  3ul

/* 系统调用：与 kernel/syscall.c 的 ABI 一一对应（rax 号，rdi/rsi/rdx 参数）。 */
static u64 sys_call(u64 nr, u64 a1, u64 a2, u64 a3)
{
    u64 ret;

    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                     : "memory", "cc");
    return ret;
}

/* .rodata 里的一段字符串：由内核按 PT_LOAD 搬进进程的地址空间。 */
static const char g_msg[] = "hello from a real ELF64 binary (M6)\n";

void _start(void)
{
    /* 1) 让内核把 .rodata 里的字符串打到屏幕上（走 copy_from_user 校验） */
    sys_call(ZH_SYS_WRITE, (u64)(unsigned long)g_msg, sizeof(g_msg) - 1u, 0u);

    /* 2) 正常退出，退出码 42 —— 内核会把 42 记进 TCB，验收就查这个数 */
    sys_call(ZH_SYS_EXIT, 42u, 0u, 0u);

    /* sys_exit 不该返回；真回来了就原地死等（保证不会跑到未定义的代码上） */
    for (;;) {
        __asm__ volatile("hlt");
    }
}
