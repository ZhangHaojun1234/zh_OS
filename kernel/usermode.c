/* usermode.c - 2MB 用户窗口、现场拼机器码、页表 U/S 置位、iretq 降到 ring3 */

#include "usermode.h"

#include "paging.h"
#include "../mm/phys.h"  // [ADD]  M3：用户窗口从页帧分配器申请，不再用 .bss 静态数组
#include "isr.h"
#include "tss.h"
#include "vga.h"
#include "../shell/shell.h"

/*
 * 用户窗口为什么要 2MB 对齐，而且正好 2MB？M1 阶段还在用 paging.c 那套"512 个 2MB 大页
 * 恒等映射"，没有 4KB 页表。2MB 大页的 U/S 位在页目录项上，一置就是 2MB 一起置，所以让
 * 用户窗口独占一个大页，这块里除了用户代码和栈什么都没有，不会顺带把内核数据暴露给 ring3。
 * M3 换成 4KB 页表以后这个限制就没了，可以按页精确保护。
 * 代价是 .bss 多占 2MB（386 行全是零页，ISO 体积不变），QEMU -m 128 够用；
 * M3 之后这个代价也没了，窗口改成运行时从页帧分配器申请。
 */

#define USER_WINDOW_BYTES 0x200000ULL

/*
 * M3 的改动：用户窗口不再是 .bss 里的 2MB 静态数组，改成运行时向 mm/phys.c 的页帧分配器
 * 要 512 个连续、2MB 对齐的物理帧。恒等映射下物理地址等于虚拟地址，拿到的指针可以直接当
 * 内存写；memmap 里也能看到这 2MB 真的登记成 used，而不是凭空多出来的 .bss。
 * 页表这层没变，还是 2MB 大页，U/S 一置就是整 2MB，M5 才换 4KB 页表。
 */
static uint8_t *g_user_window;

#define USER_CODE_BASE  ((uint64_t)(uintptr_t)g_user_window)
#define USER_STACK_TOP  (USER_CODE_BASE + USER_WINDOW_BYTES - 16u)

#define USER_CODE_CAP  4096u      /* 用户代码区上限，超出即视为拼装错误 */

/* VGA 文本缓冲：第 24 行第 26 列 = 0xb8000 + (24*80 + 26) * 2 */
#define VGA_MARKER_ADDR (0xB8000u + (24u * 80u + 26u) * 2u)

static uint32_t g_code_len;

/* 手写机器码（不引汇编器，直接往用户窗口里铺字节）                   */

static void emit8(uint8_t byte)
{
    if (g_code_len < USER_CODE_CAP) {
        g_user_window[g_code_len++] = byte;
    }
}

static void emit32(uint32_t value)
{
    emit8((uint8_t)(value & 0xFFu));
    emit8((uint8_t)((value >> 8) & 0xFFu));
    emit8((uint8_t)((value >> 16) & 0xFFu));
    emit8((uint8_t)((value >> 24) & 0xFFu));
}

/*
 * mov byte ptr [disp32], imm8。64 位模式下 mod=00、r/m=100 必须配 SIB 字节 0x25
 * （无基址、无索引），后面跟 32 位绝对地址。ring3 直接改 VGA 显存用的就是这条：
 *    C6 04 25 <disp32> <imm8>
 * 不能用 mod=00、r/m=101，那是 RIP 相对寻址。
 */
static void emit_store_byte(uint32_t addr, uint8_t value)
{
    emit8(0xC6u);
    emit8(0x04u);
    emit8(0x25u);
    emit32(addr);
    emit8(value);
}

/* 页表：给用户窗口所在的那一个 2MB 页把 U/S 位打开                   */

static void allow_user_huge_page(uint64_t va)
{
    const uint64_t phys_mask = 0x000FFFFFFFFFF000ULL;
    const uint64_t cr3 = paging_get_cr3() & phys_mask;

    /* 恒等映射：虚拟地址 == 物理地址，表项里的物理地址可以直接当指针用。 */
    uint64_t *pml4 = (uint64_t *)(uintptr_t)cr3;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[0] & phys_mask);
    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[0] & phys_mask);
    uint64_t *leaf = &pd[(va >> 21) & 0x1FFu];

    /*
     * 关键：四级都要 U=1。任何一级是 U=0，ring3 访问就 #PF，
     * 只在最后一级置位是最常见的失败方式。
     */
    pml4[0] |= PAGE_USER;
    pdpt[0] |= PAGE_USER;
    *leaf |= PAGE_USER;

    /* 重载 CR3 刷新 TLB（改页表后不刷 TLB，旧缓存会让改动看起来没生效）。 */
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

/* 进 ring3                                                           */

static void __attribute__((noreturn))
iret_to_ring3(uint64_t rip, uint64_t rsp, uint64_t rflags, uint64_t cs, uint64_t ss)
{
    __asm__ volatile(
        "cli\n\t"
        "pushq %[ss]\n\t"       /* SS  = 0x23（用户数据段 RPL3） */
        "pushq %[usp]\n\t"      /* RSP = 用户栈顶 */
        "pushq %[flg]\n\t"      /* RFLAGS，IF=1 让用户态也能被时钟打断 */
        "pushq %[cs]\n\t"       /* CS  = 0x1b（用户代码段 RPL3） */
        "pushq %[rip]\n\t"      /* RIP = 用户代码入口 */
        "iretq\n\t"             /* 返回 = 降权进 ring3 */
        :
        : [ss] "r"(ss), [usp] "r"(rsp), [flg] "r"(rflags),
          [cs] "r"(cs), [rip] "r"(rip)
        : "memory");

    __builtin_unreachable();
}

/* 日志辅助                                                           */

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

/*
 * 惰性分配用户窗口：M1 的自检和 M2 的 syscall 都要这块 2MB，谁先用到谁触发分配。
 * 只分配一次，之后复用同一个窗口。
 */
static void usermode_window_ensure(void)
{
    if (g_user_window != (uint8_t *)0) {
        return;
    }
    g_user_window = (uint8_t *)phys_alloc_frames(
        USER_WINDOW_BYTES / PHYS_PAGE_SIZE, USER_WINDOW_BYTES);
}

uint64_t usermode_code_base(void)
{
    usermode_window_ensure();
    return USER_CODE_BASE;
}

/* ring3 自检主流程                                                   */

void usermode_self_test(void)
{
    /* 0) 先要到用户窗口（M3：512 个连续、2MB 对齐的物理帧） */
    usermode_window_ensure();
    if (g_user_window == (uint8_t *)0) {
        err("ring3: no 2 MB-aligned run of 512 free frames");
        plain("  run 'memmap' to see what the frame allocator has left");
        return;
    }
    /* 分配器已经把这块内存清零，这就是用户代码区的初始状态。 */

    /* 1) 拼用户代码 */
    g_code_len = 0u;

    /* (a) 合法：往自己的窗口里写两个字节。页表这一页 U=1，ring3 写得动。
     *     这两条各 8 字节，执行后 RIP 会推进到 USER_CODE_BASE + 16。 */
    emit_store_byte((uint32_t)(USER_CODE_BASE + 0x100u), 0xAAu);
    emit_store_byte((uint32_t)(USER_CODE_BASE + 0x101u), 0x55u);

    /* (b) 原地等一会儿（mov ecx,imm32 / pause / loop $）。这段时间里 PIT 以 100Hz 反复
     *     打断 ring3：每次 CPU 都要靠 TSS.RSP0 切到内核栈、跑完 IRQ 再 iretq 回用户态。
     *     RSP0 填错的话这里直接三重故障。验收办法是拿下面打印的中断计数和 panic 里的总数
     *     比一比，差值就是 ring3 期间被正确处理的时钟中断次数。 */
    emit8(0xB9u);              /* mov ecx, imm32 */
    emit32(0x000FFFFFu);
    emit8(0xF3u);              /* pause */
    emit8(0x90u);
    emit8(0xE2u);              /* loop -> pause */
    emit8(0xFCu);

    /* (c) 非法：往 VGA 文本缓冲写 'X'。0xB8000 落在 U=0 的 2MB 大页里，
     *     ring3 一写就保护性 #PF ， 用户态连屏幕都碰不到，只能求内核代印。 */
    emit_store_byte(VGA_MARKER_ADDR, 'X');

    emit8(0xF4u);              /* hlt  ， 到不了这里 */
    emit8(0xEBu);              /* jmp $ ， 死循环兜底 */
    emit8(0xFEu);

    /* 2) 页表与 TSS */
    allow_user_huge_page(USER_CODE_BASE);   /* 2MB 大页置 U/S=1 */
    tss_set_rsp0(tss_kernel_stack_top());   /* ring3 进中断时的内核栈 */

    /* 3) 打印交代 */
    info("ring3 self-test (M1):");
    sh_print(SH_PLAIN, "  TSS.RSP0   : ");
    put_hex64(tss_get_rsp0());
    sh_newline();
    sh_print(SH_PLAIN, "  user code  : ");
    put_hex64(USER_CODE_BASE);
    sh_println(SH_PLAIN, "  (2MB window, U/S=1 at all 4 levels)");
    sh_print(SH_PLAIN, "  user stack : ");
    put_hex64(USER_STACK_TOP);
    sh_newline();
    sh_print(SH_PLAIN, "  stub bytes : ");
    sh_print_u32(SH_PLAIN, g_code_len);
    sh_println(SH_PLAIN, "  (2 legal stores + pause loop + VGA store + hlt)");
    sh_print(SH_PLAIN, "  irq count  : ");
    sh_print_u32(SH_PLAIN, (uint32_t)interrupt_count());
    sh_println(SH_PLAIN, "  (compare with the panic dump: the delta = ticks");
    plain("   handled while CPL=3, i.e. TSS.RSP0 stack switching works)");
    warn("Expect #PF: cr2=0xb8f34, error=0x7, rip=user code + 25, cs=0x001b");
    plain("  error 0x7 = present + write + user -> kernel page denied to ring3");
    plain("  rip past entry -> the legal stores and the wait really ran in ring3");
    plain("(kernel halts on purpose. Once M4 adds a scheduler, a user");
    plain(" fault will kill that task instead of stopping the machine.)");

    /* 4) 走 */
    iret_to_ring3(USER_CODE_BASE, USER_STACK_TOP, 0x202u,
                  GDT_USER_CODE_RPL3, GDT_USER_DATA_RPL3);
}
