/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    kernel/syscall.c
 * Version: 0.2beta
 * Brief:   M2 系统调用：int 0x80 门（DPL=3）、分发器、copy_from_user 校验，
 *          以及"ring3 借内核打印 -> sys_exit 交还控制权"的整条验收路径。
 * ============================================================================ */

#include "syscall.h"

#include "idt.h"
#include "isr.h"
#include "../drivers/keyboard.h"   // [ADD] M13：SYS_READKEY 从键盘环形缓冲取字符
#include "paging.h"
#include "pit.h"
#include "ring3.h"
#include "task.h"   // [ADD] M4：sys_exit 要先问调度器"现在有没有任务在跑"
#include "tss.h"
#include "usermode.h"   /* usermode_code_base()：M1 那块 2MB 用户窗口 */
#include "vga.h"
#include "../shell/shell.h"
#include "../fs/ramfs.h"   /* [ADD] M8：exec 按路径从 ramfs 打开 ELF */
#include "../fs/file.h"    /* [ADD] M9：每进程 fd 表 */

/*
 * M2 的设计取舍
 * -------------
 * 1) 复用 M1 的 2MB 用户窗口：M1 已经把它独占成一个 2MB 大页、把 U/S 置好了。
 *    M2 不再新占 2MB，直接往同一块窗口里重铺程序（偏移 0x000 放代码、
 *    0x200 放字符串、0x300 放结果槽）。跑完 `ring3` 再跑 `syscall` 也互不干扰，
 *    因为两边每次运行都会重新铺自己的机器码。
 *
 * 2) 门属性 0xEE 而不是 0x8E：DPL=3 才允许 ring3 用 int 0x80 触发这个门。
 *    用 0x8E（DPL=0）的话，ring3 一 int 0x80 就是 #GP —— 这是"系统调用怎么都
 *    不生效"最常见的原因。
 *
 * 3) 中断门（type 0xE）而不是陷门（0xF）：进入时 CPU 自动清 IF，内核处理期间
 *    不会被同级中断打断；iretq 会按帧里的 RFLAGS 把 IF 还原。
 *
 * 4) 所有指针参数都过 copy_from_user：ring3 递进来的地址必须落在自己的窗口里。
 *    不校验的话，用户态只要递一个内核地址过来，内核就会替它读内核内存
 *    （甚至代它打印一段内核数据）—— 这就是最朴素的提权漏洞。
 */

#define USER_WINDOW_BYTES (2u * 1024u * 1024u)  /* 与 M1 的 2MB 用户窗口一致 */
#define USER_CODE_CAP     4096u                 /* 机器码区上限 */
#define USER_MSG_OFF      0x200u                /* 字符串槽 */
#define USER_MSG_MAX      256u                  /* 单个字符串上限 */
#define USER_RES_OFF      0x300u                /* 结果槽：4 个 u32 */
#define USER_RES_COUNT    4u

/* kernel/interrupt.asm 里 ISR_NOERR 128 展开出来的符号。 */
extern uint64_t isr128[];

static uint8_t *g_win;   /* 用户窗口基址 */

/* [ADD] M7：sys_write 的"[kernel] sys_write: N bytes delivered"这行日志。
 * M6 的 elf 命令靠它证明 copy_from_user 真的把用户的 .rodata 读进来了，所以默认开；
 * 但 forkdemo 一次会打十几行，每行都配一条内核日志就把 25 行的屏幕彻底刷没了，
 * 于是给它一个显式开关。 */
static int g_write_log = 1;

/* [ADD] M8：exec 把文件内容读进这块内核缓冲区再交给装载器。
 * 64KB 足够放下本项目的用户程序（hello 4.7KB、forkdemo 6.7KB）。 */
#define SYS_EXEC_MAX_BYTES (64u * 1024u)
static uint8_t g_exec_buf[SYS_EXEC_MAX_BYTES];

/* [ADD] M11：exec 的 argv —— 最多 8 个参数、每个最长 96 字节，读进内核后再交给装载器 */
#define SYS_EXEC_MAX_ARGS 8u
#define SYS_EXEC_ARG_MAX  96u
static char g_exec_args[SYS_EXEC_MAX_ARGS][SYS_EXEC_ARG_MAX + 1u];
static char *g_exec_argv[SYS_EXEC_MAX_ARGS];
static uint32_t g_pos;   /* 机器码写指针 */

/* ------------------------------------------------------------------ */
/* 机器码拼装（不引汇编器，直接往窗口里铺字节）                       */
/* ------------------------------------------------------------------ */

static void put8(uint8_t byte)
{
    if (g_pos < USER_CODE_CAP) {
        g_win[g_pos++] = byte;
    }
}

static void put32(uint32_t value)
{
    put8((uint8_t)(value & 0xFFu));
    put8((uint8_t)((value >> 8) & 0xFFu));
    put8((uint8_t)((value >> 16) & 0xFFu));
    put8((uint8_t)((value >> 24) & 0xFFu));
}

/* mov edi, imm32  ->  BF id          （参数 1：一般是指针） */
static void emit_mov_edi(uint32_t imm)
{
    put8(0xBFu);
    put32(imm);
}

/* mov esi, imm32  ->  BE id          （参数 2：一般是长度） */
static void emit_mov_esi(uint32_t imm)
{
    put8(0xBEu);
    put32(imm);
}

/* mov eax, imm32  ->  B8 id          （rax = 调用号） */
static void emit_mov_eax(uint32_t imm)
{
    put8(0xB8u);
    put32(imm);
}

/* int 0x80        ->  CD 80 */
static void emit_int80(void)
{
    put8(0xCDu);
    put8(0x80u);
}

/* mov [edi+disp8], eax -> 89 47 dd   （把返回值存进结果槽） */
static void emit_store_eax(uint8_t disp)
{
    put8(0x89u);
    put8(0x47u);
    put8(disp);
}

/*
 * 用户程序（按顺序）：
 *   1) sys_write(msg, len)                    合法调用：内核代它打印一行
 *   2) sys_ticks()                            证明返回值真的能带回 ring3
 *   3) sys_cpl()                              证明这次调用确实来自 ring3
 *   4) sys_write(0x100000, 8)                 非法调用：内核指针，必须被拒
 *   5) sys_exit(0)                            交还控制权，回到 Shell
 * 每次调用的返回值写进 res[0..3]，回到内核后由内核检查。
 */
static void build_program(uint32_t msg_addr, uint32_t msg_len, uint32_t res_addr)
{
    const uint32_t kernel_ptr = 0x00100000u;   /* 内核映像首地址：ring3 不该碰 */

    g_pos = 0u;

    /* 1) 合法打印 */
    emit_mov_edi(msg_addr);
    emit_mov_esi(msg_len);
    emit_mov_eax(ZH_SYS_WRITE);
    emit_int80();

    /* 2) 读 tick：edi 换到结果槽，先存 1) 的返回值 */
    emit_mov_edi(res_addr);
    emit_store_eax(0u);

    emit_mov_eax(ZH_SYS_TICKS);
    emit_int80();
    emit_store_eax(4u);

    /* 3) 问内核"我现在是几级权限" */
    emit_mov_eax(ZH_SYS_CPL);
    emit_int80();
    emit_store_eax(8u);

    /* 4) 递一个内核指针：copy_from_user 必须拒绝 */
    emit_mov_edi(kernel_ptr);
    emit_mov_esi(8u);
    emit_mov_eax(ZH_SYS_WRITE);
    emit_int80();

    emit_mov_edi(res_addr);
    emit_store_eax(12u);

    /* 5) 退出：xor edi,edi（code=0）+ sys_exit */
    emit_mov_eax(ZH_SYS_EXIT);
    put8(0x31u);
    put8(0xFFu);
    emit_int80();
}

/* ------------------------------------------------------------------ */
/* 页表：让这块 2MB 用户窗口对 ring3 可读写                           */
/* ------------------------------------------------------------------ */

static void allow_user_window(void)
{
    const uint64_t phys_mask = 0x000FFFFFFFFFF000ULL;
    const uint64_t cr3 = paging_get_cr3() & phys_mask;
    const uint64_t va = (uint64_t)(uintptr_t)g_win;

    /* 恒等映射：表项里的物理地址可以直接当指针用。 */
    uint64_t *pml4 = (uint64_t *)(uintptr_t)cr3;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[0] & phys_mask);
    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[0] & phys_mask);
    uint64_t *leaf = &pd[(va >> 21) & 0x1FFu];

    /* 四级都要 U=1：只在叶子置位，ring3 取指照样 #PF。 */
    pml4[0] |= PAGE_USER;
    pdpt[0] |= PAGE_USER;
    *leaf |= PAGE_USER;

    /* 改完页表必须重载 CR3 刷 TLB，否则旧缓存会让改动"看起来没生效"。 */
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

/* ------------------------------------------------------------------ */
/* 内核侧：参数校验                                                   */
/* ------------------------------------------------------------------ */

/*
 * copy_from_user 的地址校验（M2 版）。
 * 真正的内核会走页表逐级检查 U/S 位；M2 阶段用户空间只有这一块 2MB 窗口，
 * 所以"指针对是否落在窗口内 + 长度不越界 + 不回绕"就已经是完整判据。
 * M3 换成 4KB 页表和独立地址空间后，这里应该升级成 walk_page_table()。
 */
static int user_ptr_ok(uint64_t ptr, uint64_t len)
{
    uint64_t base;
    uint64_t end;
    uint64_t span;

    if (task_sched_active()) {
        /* [M5] 独立地址空间：用户传进来的指针是**虚拟地址**，窗口基点由 TCB 决定，
         * 且窗口只有 VMM_USER_SPAN 大。注意内核此刻正运行在该任务的 CR3 上，
         * 所以 ptr 直接解引用就是对的（页表替我们做地址翻译）。 */
        base = task_user_window_current();
        span = VMM_USER_SPAN;
        if (base == 0u) {
            return 0;
        }
    } else {
        /* M1/M2 老路径：整块 2MB 窗口，地址恰好等于物理地址。 */
        base = (uint64_t)(uintptr_t)g_win;
        span = USER_WINDOW_BYTES;
    }
    end = base + span;

    if (len == 0u) {
        return 0;                       /* 长度为 0 视为无效请求 */
    }
    if (ptr < base) {
        return 0;                       /* 低于窗口：直接是内核地址 */
    }
    if (ptr + len < ptr) {
        return 0;                       /* 64 位回绕 */
    }
    if (ptr + len > end) {
        return 0;                       /* 越过窗口上界 */
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* 具体系统调用                                                       */
/* ------------------------------------------------------------------ */

static uint64_t sys_write(uint64_t ptr, uint64_t len)
{
    uint64_t i;

    if (len > USER_MSG_MAX || !user_ptr_ok(ptr, len)) {
        sh_print(SH_ERR, "[kernel] ");
        sh_println(SH_ERR, "sys_write: buffer outside user window -> denied (-1)");
        return (uint64_t)-1;
    }

    for (i = 0u; i < len; ++i) {
        vga_putchar(*((const char *)(uintptr_t)(ptr + i)), VGA_LIGHT_GREY, VGA_BLACK);
    }

    if (g_write_log != 0) {
        sh_print(SH_PLAIN, "[kernel] sys_write: ");
        sh_print_u32(SH_PLAIN, (uint32_t)len);
        sh_println(SH_PLAIN, " bytes delivered for ring 3");
    }
    return len;
}

static uint64_t sys_ticks(void)
{
    return pit_get_ticks();
}

static uint64_t sys_cpl(uint64_t cs)
{
    return cs & 3u;
}

/* [ADD] M13：rc 在后面才定义，这里先给原型（M9 的函数，M13 也用它） */
static int copy_path_from_user(uint64_t ptr, char *dst, uint32_t cap);

/*
 * [ADD] M13：读一个键。
 * 这是"shell 降到 ring3"的关键一条 —— 以前键盘是内核里的 read_line() 直接拿的，
 * 用户态根本碰不到；现在 shell 只能靠这个调用一个字符一个字符地取。
 *
 * 语义按原样返回（不阻塞）：缓冲区空就返回 0，用户态自己决定要不要 sleep 一下
 * 再问一次。为什么不做成阻塞的？因为内核里阻塞要挂起当前任务、醒来还要有人唤醒，
 * 那是信号量与等待队列的活；这一步先让用户态自己轮询，语义照样正确（只是更笨）。
 */
static uint64_t sys_readkey(void)
{
    if (!keyboard_haschar()) {
        return 0u;
    }
    /* 方向键之类的扩展键在另一个小环形缓冲里，读一个丢掉，免得卡住 */
    while (keyboard_has_special()) {
        (void)keyboard_get_special();
    }
    return (uint64_t)(uint8_t)keyboard_getchar();
}

/* [ADD] M14：按序号枚举目录条目 —— 用户态 ls 的全部信息来源 */
static uint64_t sys_readdir(uint64_t path_ptr, uint64_t index, uint64_t buf_ptr)
{
    /* 本 ABI 只有三个参数位（rdi/rsi/rdx），所以名字缓冲长度就固定 64 —— 
     * 和 ramfs_node_t.name 的容量一致，刚好够用。 */
    const uint64_t cap = 64u;
    char path[128];
    char name[64];
    ramfs_type_t type = RAMFS_FILE;
    uint64_t i;
    int rc;

    if (path_ptr == 0u || !user_ptr_ok(buf_ptr, (uint32_t)cap)) {
        return (uint64_t)-1;
    }
    if (copy_path_from_user(path_ptr, path, (uint32_t)sizeof(path)) != 0) {
        return (uint64_t)-1;
    }
    rc = ramfs_readdir(path, (uint32_t)index, name, (uint32_t)cap, &type);
    if (rc != RAMFS_OK) {
        return (uint64_t)-1;
    }
    for (i = 0u; name[i] != '\0' && i + 1u < cap; ++i) {
        ((char *)(uintptr_t)buf_ptr)[i] = name[i];
    }
    ((char *)(uintptr_t)buf_ptr)[i] = '\0';
    return (type == RAMFS_DIR) ? 1u : 0u;
}
/* [ADD] M13：切换工作目录（直接转发给 ramfs） */
static uint64_t sys_chdir(uint64_t path_ptr)
{
    char path[128];
    int rc;

    if (copy_path_from_user(path_ptr, path, (uint32_t)sizeof(path)) != 0) {
        return (uint64_t)-1;
    }
    rc = ramfs_chdir(path);
    return (rc == RAMFS_OK) ? 0u : (uint64_t)-1;
}

/* [ADD] M13：把当前工作目录拷进用户缓冲区，返回字节数 */
static uint64_t sys_getcwd(uint64_t buf_ptr, uint64_t maxlen)
{
    const char *cwd = ramfs_get_cwd();
    uint64_t len = 0u;
    char *dst;

    if (buf_ptr == 0u || maxlen == 0u) {
        return (uint64_t)-1;
    }
    while (cwd[len] != '\0' && len + 1u < maxlen) {
        ++len;
    }
    if (!user_ptr_ok(buf_ptr, (uint32_t)(len + 1u))) {
        return (uint64_t)-1;
    }
    dst = (char *)(uintptr_t)buf_ptr;
    for (uint64_t i = 0u; i < len; ++i) {
        dst[i] = cwd[i];
    }
    dst[len] = '\0';
    return len;
}

// [ADD] M4：单个字符输出。和 sys_write 的区别是它不打"[kernel] sys_write: ..."
//        那一行日志 —— 任务每秒要打几十个字符，走日志会把屏幕刷爆。
static uint64_t sys_putc(uint64_t ch)
{
    const char c = (char)(uint8_t)ch;
    enum vga_color color = VGA_LIGHT_GREY;

    if (c == 'A') {
        color = VGA_LIGHT_GREEN;
    } else if (c == 'B') {
        color = VGA_LIGHT_CYAN;
    }
    vga_putchar(c, color, VGA_BLACK);
    return 0u;
}

/*
 * [ADD] M4：睡若干个 tick。
 * 为什么用 cli 之后的 sti + hlt，而不是空转：
 *   空转会白占 CPU（时间片照样被烧完，但屏幕上什么也不发生，看起来像卡死）；
 *   hlt 让 CPU 真正停下，等到下一个中断才醒。醒来时可能已经是另一个任务在跑，
 *   到它自己再被换上时，iretq 会回到这条 hlt 循环里继续 —— 因为它的整个现场
 *   （包括这条循环的 rsp 和 rip）都被 sched_tick 存进 TCB 了。
 * 注意：中断门进来时 IF=0，所以这里必须先显式 sti，否则 hlt 永远等不到中断。
 */
static uint64_t sys_sleep(uint64_t ticks)
{
    uint64_t target = pit_get_ticks() + ticks;

    __asm__ volatile("sti");
    while (pit_get_ticks() < target) {
        __asm__ volatile("hlt");
    }
    return ticks;
}

/* ------------------------------------------------------------------ */
/* [M8] exec：按路径打开 ELF，把当前进程的映像换掉                       */
/* ------------------------------------------------------------------ */

/*
 * 与 Linux 的 execve 相比这里少了什么：没有参数（argv/envp）、没有 fd 继承、
 * 没有权限位检查、没有 PT_INTERP。但"按路径打开文件 -> 装载 -> 原地换映像 ->
 * 同一个 pid 继续跑新程序"这条主线是完整的。
 */
static uint64_t sys_exec(struct regs *r, uint64_t path_ptr, uint64_t argv_ptr,
                            uint64_t argc)
{
    char path[128];
    uint64_t i;
    uint64_t len = 0u;
    int rc;
    elf_image_t img;

    if (!task_sched_active()) {
        sh_println(SH_ERR, "[kernel] exec: only a running task can exec");
        return (uint64_t)-1;
    }
    if (!user_ptr_ok(path_ptr, 1u)) {
        sh_println(SH_ERR, "[kernel] exec: path pointer outside user window -> denied");
        return (uint64_t)-1;
    }

    /* 逐字节把路径拷进内核（字符串长度未知，所以一个字节一个字节校验） */
    for (i = 0u; i < sizeof(path) - 1u; ++i) {
        if (!user_ptr_ok(path_ptr + i, 1u)) {
            return (uint64_t)-1;
        }
        path[i] = *((const char *)(uintptr_t)(path_ptr + i));
        if (path[i] == '\0') {
            break;
        }
    }
    path[sizeof(path) - 1u] = '\0';
    if (path[0] == '\0') {
        return (uint64_t)-1;
    }

    rc = ramfs_read_file(path, g_exec_buf, SYS_EXEC_MAX_BYTES, &len);
    if (rc != RAMFS_OK) {
        sh_print(SH_ERR, "[kernel] exec: ");
        sh_print(SH_ERR, path);
        sh_print(SH_ERR, ": ");
        sh_println(SH_ERR, ramfs_strerror(rc));
        return (uint64_t)-1;
    }

    /* [M11] 把 argv 从用户态拷进内核：先读指针数组，再逐个逐字节拷字符串 */
    uint32_t argn = 0u;
    if (argc != 0u && argv_ptr != 0u && user_ptr_ok(argv_ptr, 8u)) {
        uint64_t i;
        if (argc > SYS_EXEC_MAX_ARGS) {
            argc = SYS_EXEC_MAX_ARGS;
        }
        for (i = 0u; i < argc; ++i) {
            uint64_t one = *(const uint64_t *)(uintptr_t)(argv_ptr + i * 8u);
            uint32_t j;
            if (one == 0u || !user_ptr_ok(argv_ptr + i * 8u, 8u)) {
                break;
            }
            for (j = 0u; j < SYS_EXEC_ARG_MAX; ++j) {
                if (!user_ptr_ok(one + j, 1u)) {
                    return (uint64_t)-1;
                }
                g_exec_args[i][j] = *((const char *)(uintptr_t)(one + j));
                if (g_exec_args[i][j] == '\0') {
                    break;
                }
            }
            g_exec_args[i][SYS_EXEC_ARG_MAX] = '\0';
            g_exec_argv[i] = g_exec_args[i];
            ++argn;
        }
    }

    if (task_exec_current_argv(r, g_exec_buf, len, &img, g_exec_argv, argn) != 0) {
        sh_println(SH_ERR, "[kernel] exec: the ELF could not be installed");
        return (uint64_t)-1;
    }

    return 0u;
}

/* ------------------------------------------------------------------ */
/* [M9] open / read / close：用户态第一次自己读写文件                    */
/* ------------------------------------------------------------------ */

#define SYS_READ_MAX_BYTES 4096u

static uint8_t g_read_buf[SYS_READ_MAX_BYTES];

/* 把用户态给的路径逐字节拷进内核（与 exec 同样的做法）。返回 0 成功。 */
static int copy_path_from_user(uint64_t ptr, char *dst, uint32_t cap)
{
    uint32_t i;

    if (!user_ptr_ok(ptr, 1u)) {
        return -1;
    }
    for (i = 0u; i < cap - 1u; ++i) {
        if (!user_ptr_ok(ptr + i, 1u)) {
            return -1;
        }
        dst[i] = *((const char *)(uintptr_t)(ptr + i));
        if (dst[i] == '\0') {
            return (i == 0u) ? -1 : 0;
        }
    }
    dst[cap - 1u] = '\0';
    return 0;
}

static uint64_t sys_open(uint64_t path_ptr)
{
    char path[128];
    zfile_t *tab = task_current_fds();
    int fd;

    if (tab == (zfile_t *)0) {
        sh_println(SH_ERR, "[kernel] open: only a running task has a fd table");
        return (uint64_t)-1;
    }
    if (copy_path_from_user(path_ptr, path, sizeof(path)) != 0) {
        sh_println(SH_ERR, "[kernel] open: bad path pointer -> denied");
        return (uint64_t)-1;
    }
    fd = file_open(tab, ZH_MAX_FDS, path);
    if (fd < 0) {
        sh_print(SH_ERR, "[kernel] open: ");
        sh_print(SH_ERR, path);
        sh_println(SH_ERR, "  -> denied (missing / is a dir / no permission)");
        return (uint64_t)-1;
    }
    return (uint64_t)fd;
}

static uint64_t sys_read(uint64_t fd_arg, uint64_t buf_ptr, uint64_t len)
{
    zfile_t *tab = task_current_fds();
    int64_t got;
    uint64_t i;
    uint64_t want = len;

    if (tab == (zfile_t *)0) {
        return (uint64_t)-1;
    }
    if (want > SYS_READ_MAX_BYTES) {
        want = SYS_READ_MAX_BYTES;
    }
    got = file_read(tab, ZH_MAX_FDS, (int)fd_arg, g_read_buf, want);
    if (got < 0) {
        return (uint64_t)-1;
    }
    /* 逐字节写进用户缓冲，每个字节都过 user_ptr_ok（与 sys_write 同一套规矩） */
    for (i = 0u; i < (uint64_t)got; ++i) {
        if (!user_ptr_ok(buf_ptr + i, 1u)) {
            sh_println(SH_ERR, "[kernel] read: buffer outside user window -> denied");
            return (uint64_t)-1;
        }
        *((char *)(uintptr_t)(buf_ptr + i)) = (char)g_read_buf[i];
    }
    return (uint64_t)got;
}

static uint64_t sys_close(uint64_t fd_arg)
{
    zfile_t *tab = task_current_fds();

    if (tab == (zfile_t *)0) {
        return (uint64_t)-1;
    }
    return (file_close(tab, ZH_MAX_FDS, (int)fd_arg) == 0) ? 0u : (uint64_t)-1;
}

/* ------------------------------------------------------------------ */
/* [M10] creat / write / unlink：让文件真正可写                          */
/* ------------------------------------------------------------------ */

#define SYS_WRITE_FILE_MAX 4096u

static uint8_t g_wr_buf[SYS_WRITE_FILE_MAX];

/* 把用户缓冲逐字节拷进内核（写文件用；长度由用户给，所以每字节都要校验）。 */
static int copy_buf_from_user(uint64_t ptr, uint8_t *dst, uint64_t len)
{
    uint64_t i;

    for (i = 0u; i < len; ++i) {
        if (!user_ptr_ok(ptr + i, 1u)) {
            return -1;
        }
        dst[i] = *((const uint8_t *)(uintptr_t)(ptr + i));
    }
    return 0;
}

static uint64_t sys_creat(uint64_t path_ptr)
{
    char path[128];
    zfile_t *tab = task_current_fds();
    int fd;

    if (tab == (zfile_t *)0) {
        return (uint64_t)-1;
    }
    if (copy_path_from_user(path_ptr, path, sizeof(path)) != 0) {
        sh_println(SH_ERR, "[kernel] creat: bad path pointer -> denied");
        return (uint64_t)-1;
    }
    fd = file_creat(tab, ZH_MAX_FDS, path);
    if (fd < 0) {
        sh_print(SH_ERR, "[kernel] creat: ");
        sh_print(SH_ERR, path);
        sh_println(SH_ERR, "  -> denied (parent not writable / is a dir)");
        return (uint64_t)-1;
    }
    sh_print(SH_PLAIN, "[kernel] creat: ");
    sh_print(SH_PLAIN, path);
    sh_print(SH_PLAIN, " -> fd ");
    sh_print_u32(SH_PLAIN, (uint32_t)fd);
    sh_newline();
    return (uint64_t)fd;
}

static uint64_t sys_fwrite(uint64_t fd_arg, uint64_t buf_ptr, uint64_t len)
{
    zfile_t *tab = task_current_fds();
    uint64_t want = len;
    int64_t wrote;

    if (tab == (zfile_t *)0) {
        return (uint64_t)-1;
    }
    if (want == 0u) {
        return 0u;
    }
    if (want > SYS_WRITE_FILE_MAX) {
        want = SYS_WRITE_FILE_MAX;
    }
    if (copy_buf_from_user(buf_ptr, g_wr_buf, want) != 0) {
        sh_println(SH_ERR, "[kernel] write: buffer outside user window -> denied");
        return (uint64_t)-1;
    }
    wrote = file_append(tab, ZH_MAX_FDS, (int)fd_arg, g_wr_buf, want);
    if (wrote < 0) {
        return (uint64_t)-1;
    }
    sh_print(SH_PLAIN, "[kernel] write: ");
    sh_print_u32(SH_PLAIN, (uint32_t)wrote);
    sh_print(SH_PLAIN, " bytes appended to fd ");
    sh_print_u32(SH_PLAIN, (uint32_t)fd_arg);
    sh_newline();
    return (uint64_t)wrote;
}

static uint64_t sys_unlink(uint64_t path_ptr)
{
    char path[128];
    int rc;

    if (copy_path_from_user(path_ptr, path, sizeof(path)) != 0) {
        return (uint64_t)-1;
    }
    rc = ramfs_rm(path);
    if (rc != RAMFS_OK) {
        sh_print(SH_ERR, "[kernel] unlink: ");
        sh_print(SH_ERR, path);
        sh_print(SH_ERR, "  -> ");
        sh_println(SH_ERR, ramfs_strerror(rc));
        return (uint64_t)-1;
    }
    return 0u;
}

/* ------------------------------------------------------------------ */
/* 分发                                                               */
/* ------------------------------------------------------------------ */

void syscall_dispatch(struct regs *r)
{
    switch (r->rax) {
    case ZH_SYS_WRITE:
        r->rax = sys_write(r->rdi, r->rsi);
        return;
    case ZH_SYS_TICKS:
        r->rax = sys_ticks();
        return;
    case ZH_SYS_CPL:
        r->rax = sys_cpl(r->cs);      /* 帧里的 CS 就是"调用者是谁"的铁证 */
        return;
    // [ADD] M4：多任务用的两个调用
    case ZH_SYS_PUTC:
        r->rax = sys_putc(r->rdi);
        return;
    case ZH_SYS_SLEEP:
        r->rax = sys_sleep(r->rdi);
        return;
    // [ADD] M13：用户态 shell 的三条腿
    case ZH_SYS_READKEY:
        r->rax = sys_readkey();
        return;
    case ZH_SYS_CHDIR:
        r->rax = sys_chdir(r->rdi);
        return;
    case ZH_SYS_GETCWD:
        r->rax = sys_getcwd(r->rdi, r->rsi);
        return;
    case ZH_SYS_READDIR:
        r->rax = sys_readdir(r->rdi, r->rsi, r->rdx);
        return;
    // [ADD] M7：fork / getpid / wait
    case ZH_SYS_FORK: {
        const int pid = task_fork_current(r);
        /* 父进程的返回值就是子进程 pid；子进程的帧已经被复制成 rax=0，它从这里
         * iretq 出去时看到的就是 0 —— 和 Linux 的 fork 语义一致。 */
        r->rax = (pid < 0) ? (uint64_t)-1 : (uint64_t)(uint32_t)pid;
        return;
    }
    case ZH_SYS_GETPID:
        r->rax = (uint64_t)task_current_pid();
        return;
    case ZH_SYS_EXEC:
        /* 成功时这一帧的 rip/rsp 已经被换成新程序的入口，iretq 直接落到那里，
         * 所以"返回值"其实只有失败才会被看到。 */
        r->rax = sys_exec(r, r->rdi, r->rsi, r->rdx);
        return;
    case ZH_SYS_OPEN:      /* [M9] */
        r->rax = sys_open(r->rdi);
        return;
    case ZH_SYS_READ:      /* [M9] */
        r->rax = sys_read(r->rdi, r->rsi, r->rdx);
        return;
    case ZH_SYS_CLOSE:     /* [M9] */
        r->rax = sys_close(r->rdi);
        return;
    case ZH_SYS_CREAT:     /* [M10] */
        r->rax = sys_creat(r->rdi);
        return;
    case ZH_SYS_FWRITE:    /* [M10] */
        r->rax = sys_fwrite(r->rdi, r->rsi, r->rdx);
        return;
    case ZH_SYS_UNLINK:    /* [M10] */
        r->rax = sys_unlink(r->rdi);
        return;
    case ZH_SYS_WAIT:
        /* 注意：没有僵尸子进程时这里会阻塞（schedule_away 不返回），
         * 子进程退出时把退出码写进本帧的 rax，父进程被换回时就从这里"返回"。 */
        r->rax = (uint64_t)(int64_t)task_wait_current(r);
        return;
    /* [ADD] M17：等指定 pid 的子进程；kill(pid, sig) 投递信号 */
    case ZH_SYS_WAITPID:
        /* [ADD] M18：rsi 现在是 flags（WNOHANG / WUNTRACED），M17 的老调用传 0 即可 */
        r->rax = (uint64_t)(int64_t)task_waitpid_current(
            r, (int32_t)(int64_t)r->rdi, (uint32_t)r->rsi);
        return;
    case ZH_SYS_SETFG:
        syscall_mark_foreground((int)(int64_t)r->rdi);
        r->rax = 0u;
        return;
    case ZH_SYS_KILL: {
        const int pid = (int)r->rdi;
        const uint32_t sig = (uint32_t)r->rsi;
        if (pid < 0) {
            /* pid == -1：给所有（非自己）进程发；本版就是"给前台" */
            r->rax = (task_signal_foreground(sig) == 0) ? 0u : (uint64_t)-1;
            return;
        }
        r->rax = (task_signal((uint32_t)pid, sig) == 0) ? 0u : (uint64_t)-1;
        return;
    }

    /* [ADD] M18：信号捕获的三个调用 */
    case ZH_SYS_SIGNAL: {
        const uint32_t sig = (uint32_t)r->rdi;
        const uint64_t handler = r->rsi;
        task_t *t = task_current_slot();
        uint64_t old = 0u;

        if (t == (task_t *)0 || sig == ZH_SIG_NONE || sig >= ZH_SIG_MAX ||
            sig == ZH_SIGKILL || sig == ZH_SIGSTOP) {
            r->rax = (uint64_t)-1;       /* SIGKILL/SIGSTOP 不可捕获（Linux 同理） */
            return;
        }
        old = t->sig_handler[sig];
        t->sig_handler[sig] = handler;   /* 0 = 恢复默认动作（Linux 的 SIG_DFL） */
        r->rax = old;
        return;
    }
    case ZH_SYS_SIGRETURN:
        r->rax = (uint64_t)(int64_t)task_sigreturn(r);
        return;
    case ZH_SYS_GETSTATE: {
        const int pid = (int)r->rdi;
        r->rax = (uint64_t)(int64_t)task_state_of((uint32_t)pid);
        return;
    }
    case ZH_SYS_EXIT:
        // [ADD] M4：有调度器时，退出 = "杀掉这个任务、换下一个"，
        //          而不是"把 CPU 交还给当初调用 usermode_enter 的那段 Shell 代码"。
        if (task_sched_active()) {
            task_exit((int)r->rdi);
        }
        /* 不写 r->rax，也不 iretq：直接把 CPU 交回 for 壳的调用点。 */
        usermode_resume();
        return;
    default:
        sh_print(SH_ERR, "[kernel] unknown syscall: ");
        sh_print_u32(SH_ERR, (uint32_t)r->rax);
        sh_newline();
        r->rax = (uint64_t)-1;
        return;
    }
}

/* ------------------------------------------------------------------ */
/* 注册                                                               */
/* ------------------------------------------------------------------ */

void syscall_set_write_log(int on)
{
    g_write_log = (on != 0) ? 1 : 0;
}

/*
 * [ADD] M17：把一个 pid 标成前台进程。
 * 为什么需要它：Ctrl-C 是内核在键盘中断里收到的，内核并不知道"现在用户在等谁"。
 * Linux 用进程组 + 控制终端（tcgetpgrp）来解决，本版的等价物就是这一位标记：
 * zhsh3 在 fork 拿到子 pid 之后、readkey 等它退出之前，把子进程标成前台。
 */
void syscall_mark_foreground(int pid)
{
    if (pid < 0) {
        task_clear_foreground();   /* [M17] 子进程已收尸：前台清空，^C 不再误伤 */
        return;
    }
    task_set_foreground((uint32_t)pid);
}

void syscall_init(void)
{
    /*
     * 0xEE = P=1 | DPL=3 | type=1110（64 位中断门）。
     * 这里不能用 idt.h 的 IDT_GATE_INTERRUPT（0x8E），那是 DPL=0。
     */
    idt_set_gate((uint8_t)ZH_SYSCALL_VECTOR,
                 (uint64_t)(uintptr_t)isr128, 0, 0xEEu);

    vga_print("syscall: int 0x80 gate installed (DPL=3)\n", VGA_LIGHT_GREY, VGA_BLACK);
}

/* ------------------------------------------------------------------ */
/* 自检主流程：`syscall` 命令                                          */
/* ------------------------------------------------------------------ */

static void put_hex64(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char buf[19];

    buf[0] = '0';
    buf[1] = 'x';
    for (uint32_t i = 0u; i < 16u; ++i) {
        buf[2u + i] = digits[(value >> (60u - 4u * i)) & 0xFu];
    }
    buf[18] = '\0';

    sh_print(SH_PLAIN, buf);
}

void syscall_self_test(void)
{
    static const char message[] = "hello from RING 3 via int 0x80\n";
    const uint32_t msg_off = USER_MSG_OFF;
    const uint32_t res_off = USER_RES_OFF;
    uint64_t i;
    volatile uint32_t *res;
    uint32_t msg_addr;
    uint32_t msg_len;
    uint32_t res_addr;

    g_win = (uint8_t *)(uintptr_t)usermode_code_base();

    /* M3：用户窗口现在是运行时从页帧分配器申请的，可能申请不到。 */
    if (g_win == (uint8_t *)0) {
        err("syscall: no user window available");
        plain("  run 'memmap' to see what the frame allocator has left");
        return;
    }

    /* ---- 1) 铺字符串 ---- */
    msg_len = (uint32_t)(sizeof(message) - 1u);
    if (msg_len > USER_MSG_MAX) {
        err("syscall: message too long");
        return;
    }
    for (i = 0u; i < msg_len; ++i) {
        g_win[msg_off + i] = (uint8_t)message[i];
    }

    /* ---- 2) 铺机器码 ---- */
    msg_addr = (uint32_t)(uintptr_t)(g_win + msg_off);
    res_addr = (uint32_t)(uintptr_t)(g_win + res_off);
    build_program(msg_addr, msg_len, res_addr);

    /* 结果槽清零，免得读到上一轮的残留。 */
    for (i = 0u; i < USER_RES_COUNT; ++i) {
        *((volatile uint32_t *)(uintptr_t)(g_win + res_off + 4u * i)) = 0u;
    }

    /* ---- 3) 页表 + TSS.RSP0 ---- */
    allow_user_window();
    tss_set_rsp0(tss_kernel_stack_top());

    /* ---- 4) 交代现场 ---- */
    info("int 0x80 syscall self-test (M2):");
    plain("  ABI       : rax=number, rdi/rsi=args, return in rax");
    sh_print(SH_PLAIN, "  user code : ");
    put_hex64((uint64_t)(uintptr_t)g_win);
    plain("  (M1's 2MB window, U/S=1)");
    sh_print(SH_PLAIN, "  msg       : ");
    put_hex64((uint64_t)(uintptr_t)msg_addr);
    sh_print(SH_PLAIN, "  len=");
    sh_print_u32(SH_PLAIN, msg_len);
    sh_newline();
    warn("now dropping to ring 3: the next line is printed by the KERNEL");
    plain("   on behalf of ring 3 -> that is sys_write working.");

    /* ---- 5) 走 ---- */
    usermode_enter((uint64_t)(uintptr_t)g_win,
                   (uint64_t)(uintptr_t)(g_win + USER_WINDOW_BYTES - 16u),
                   0x202u,
                   GDT_USER_CODE_RPL3, GDT_USER_DATA_RPL3);

    /* ---- 6) 这一行以下 = sys_exit 把 CPU 送回内核之后 ---- */
    ok("back in ring 0: sys_exit handed control back (no panic)");

    res = (volatile uint32_t *)(uintptr_t)(g_win + res_off);
    sh_print(SH_PLAIN, "  res[0] write ret  : ");
    sh_print_u32(SH_PLAIN, res[0]);
    plain(res[0] == msg_len ? "  (== len, ok)" : "  (unexpected)");

    sh_print(SH_PLAIN, "  res[1] pit ticks  : ");
    sh_print_u32(SH_PLAIN, res[1]);
    sh_newline();

    sh_print(SH_PLAIN, "  res[2] caller cpl : ");
    sh_print_u32(SH_PLAIN, res[2]);
    plain(res[2] == 3u ? "  <- 3 means the call really came from ring 3"
                       : "  <- NOT 3: the call did not come from ring 3");

    sh_print(SH_PLAIN, "  res[3] bad ptr    : ");
    put_hex64((uint64_t)res[3]);
    plain(res[3] == 0xFFFFFFFFu ? "  (-1, bad pointer denied)"
                                : "  (accepted: copy_from_user is broken)");

    if (res[0] == msg_len && res[2] == 3u && res[3] == 0xFFFFFFFFu) {
        ok("M2 PASS: ring 3 -> int 0x80 -> kernel -> sys_exit -> shell");
    } else {
        err("M2 FAIL: see the values above");
    }
}
