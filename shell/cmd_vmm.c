/* ============================================================================
 * zh_OS 1.0 (x86_64)
 * File:    shell/cmd_vmm.c
 * Version: 1.0
 * Brief:   M5 进程地址空间：`vmspace` 命令，把页表本身打印出来看。
 * ============================================================================ */

#include "cmd_vmm.h"

#include "shell.h"
#include "../kernel/paging.h"
#include "../kernel/task.h"
#include "../kernel/vmm.h"

static void show_hex(uint64_t value)
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

int cmd_vmspace(int argc, char **argv)
{
    uint32_t pid;
    int any = 0;

    (void)argc;
    (void)argv;

    info("Process address spaces (M5): 4 KB pages, one PML4 per process");
    sh_print(SH_PLAIN, "  kernel CR3 : ");
    show_hex(vmm_kernel_cr3());
    sh_print(SH_PLAIN, "   current CR3 : ");
    show_hex(paging_get_cr3() & 0x000FFFFFFFFFF000ULL);
    sh_newline();
    plain("  layout: [0, 1GB) kernel identity 2MB pages (U=0, shared by every process)");
    sh_print(SH_PLAIN, "          [");
    show_hex(VMM_USER_BASE);
    sh_print(SH_PLAIN, ", ");
    show_hex(VMM_USER_BASE + VMM_USER_SPAN);
    sh_println(SH_PLAIN, ") private window, 4 KB pages, U=1)");
    plain("  walk:   PML4[0] -> own PDPT -> [0]=kernel 2MB PD (U=0)");
    plain("                         -> [1]=own PD -> PD[0]=own PT -> 512 x 4 KB");

    for (pid = 0u; pid < TASK_MAX; ++pid) {
        const task_t *t = task_lookup(pid);
        uint32_t index;
        uint32_t mapped;

        if (t == (const task_t *)0 || !t->space_ready) {
            continue;
        }
        any = 1;

        sh_newline();
        sh_print(SH_PLAIN, "  pid ");
        sh_print_u32(SH_PLAIN, pid);
        sh_print(SH_PLAIN, "  ");
        sh_print(SH_PLAIN, t->name);
        sh_print(SH_PLAIN, "   cr3=");
        show_hex(t->cr3);
        sh_newline();
        sh_print(SH_PLAIN, "    pml4=");
        show_hex(t->space.pml4_pa);
        sh_print(SH_PLAIN, "  pdpt=");
        show_hex(t->space.pdpt_pa);
        sh_newline();
        sh_print(SH_PLAIN, "    pd  =");
        show_hex(t->space.pd_pa);
        sh_print(SH_PLAIN, "  pt  =");
        show_hex(t->space.pt_pa);
        sh_newline();

        mapped = vmm_count_mapped(&t->space);
        sh_print(SH_PLAIN, "    mapped pages: ");
        sh_print_u32(SH_PLAIN, mapped);
        sh_println(SH_PLAIN, " of 512");
        for (index = 0u; index < VMM_ENTRIES; ++index) {
            const uint64_t va = t->space.va_base + (uint64_t)index * VMM_PAGE_SIZE;
            const uint64_t pa = vmm_probe(&t->space, va);

            if (pa == 0u) {
                continue;
            }
            sh_print(SH_PLAIN, "      VA ");
            show_hex(va);
            sh_print(SH_PLAIN, " -> PA ");
            show_hex(pa);
            sh_newline();
        }
    }

    if (!any) {
        plain("  (no process with a private address space - run 'taskrun' first)");
    }
    return SHELL_CONTINUE;
}
