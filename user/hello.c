/* hello.c - M6 的第一个"真程序"，gcc 编出来的 ELF64。跟 task.c 里手拼机器码那些任务不是一回事：
 * 它是独立编译的 ET_EXEC，有自己的程序头表、.text/.rodata/.data 和入口 _start；
 * 内核不认识它，只按 ELF 头读完 PT_LOAD 逐段搬进进程地址空间；
 * 它直接用 int 0x80 做系统调用（rax 放号，rdi/rsi/rdx 放参数）。
 * 从 M6 起，程序不再是内核的一部分，而是内核加载的对象。 */

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

    /* 2) 正常退出，退出码 42 ， 内核会把 42 记进 TCB，验收就查这个数 */
    sys_call(ZH_SYS_EXIT, 42u, 0u, 0u);

    /* sys_exit 不该返回；真回来了就原地死等（保证不会跑到未定义的代码上） */
    for (;;) {
        __asm__ volatile("hlt");
    }
}
