/* cmd_demand.c - M16 的 demand 命令，看按需分页省了多少帧。装载时 demand_pages = 16，
 * 登记 16 页 .bss 一帧没给；跑完 demand_faults 只有 2，页帧增量两三个（碰过的页加该进程
 * 自己那套页表和栈），不是 16+。登记 16 页只兑现 2 页，这个差最能说明事。 */

#include "cmd_demand.h"

#include "shell.h"
#include "../kernel/elf.h"
#include "../kernel/task.h"
#include "../kernel/vmm.h"
#include "../fs/ramfs.h"
#include "../mm/phys.h"
#include "../kernel/syscall.h"

static uint8_t g_blob[64u * 1024u];

int cmd_demand(int argc, char **argv)
{
    const char *path = "/bin/demanddemo.elf";
    const task_t *t;
    uint64_t len = 0u;
    uint64_t free_before;
    uint64_t free_after_load;
    uint64_t free_after;
    uint32_t faults;
    uint32_t pages;
    elf_image_t img;
    int pid;
    int rc;
    int demo_pid;

    (void)argc;
    (void)argv;

    task_reset();
    syscall_set_write_log(0);
    task_set_exec_verbose(0);

    if (task_init() != 0) {
        err("demand: task subsystem failed to init");
        return SHELL_CONTINUE;
    }

    info("M16 demand paging: a page costs nothing until it is touched");
    sh_print(SH_PLAIN, "  program   : ");
    sh_println(SH_PLAIN, path);
    sh_print(SH_PLAIN, "  frames    : free=");
    sh_print_u32(SH_PLAIN, (uint32_t)phys_free_frame_count());
    sh_newline();
    free_before = phys_free_frame_count();

    rc = ramfs_read_file(path, g_blob, (uint64_t)sizeof(g_blob), &len);
    if (rc != RAMFS_OK) {
        err("demand: cannot read the demo program");
        plain(ramfs_strerror(rc));
        return SHELL_CONTINUE;
    }

    pid = task_create_elf("demand", g_blob, len, &img);
    if (pid < 0) {
        err("demand: could not create the process");
        return SHELL_CONTINUE;
    }

    sh_print(SH_PLAIN, "  image     : segments=");
    sh_print_u32(SH_PLAIN, img.segments);
    sh_print(SH_PLAIN, "  mapped pages=");
    sh_print_u32(SH_PLAIN, img.pages);
    sh_print(SH_PLAIN, "  registered-on-demand=");
    sh_print_u32(SH_PLAIN, img.demand_pages);
    sh_println(SH_PLAIN, "  (these are NOT mapped yet, so they cost no frames)");

    /* [M16] 装载完、还没跑：这时才能看到"装载本身"的成本 */
    free_after_load = phys_free_frame_count();
    sh_print(SH_PLAIN, "  cost@load : ");
    sh_print_u32(SH_PLAIN, (uint32_t)(free_before - free_after_load));
    sh_println(SH_PLAIN, " frames (page tables + the file-backed pages only)");

    task_run_all();

    free_after = phys_free_frame_count();
    faults = vmm_demand_fault_count();
    pages = vmm_demand_page_count();

    sh_print(SH_PLAIN, "  cost@run  : ");
    sh_print_u32(SH_PLAIN, (uint32_t)(free_after_load - free_after));
    sh_print(SH_PLAIN, " frames (only the pages the program actually touched), total ");
    sh_print_u32(SH_PLAIN, (uint32_t)(free_before - free_after));
    sh_println(SH_PLAIN, " frames");
    sh_print(SH_PLAIN, "  demand    : faults=");
    sh_print_u32(SH_PLAIN, faults);
    sh_print(SH_PLAIN, "  pages materialised=");
    sh_print_u32(SH_PLAIN, pages);
    sh_println(SH_PLAIN, "  (the other pages were never in RAM)");

    t = task_lookup((uint32_t)pid);
    demo_pid = pid;
    if (t != (const task_t *)0) {
        sh_print(SH_PLAIN, "  process   : exit code=");
        sh_print_u32(SH_PLAIN, (uint32_t)t->exit_code);
        sh_print(SH_PLAIN, "  faults=");
        sh_print_u32(SH_PLAIN, (uint32_t)t->fault_vector);
        sh_newline();
    } else {
        demo_pid = -1;
    }
    (void)demo_pid;

    if (t == (const task_t *)0 || t->exit_code != 0 || t->fault_vector != 0u) {
        err("demand: the demo program died (dirty page or a real fault)");
        return SHELL_CONTINUE;
    }
    if (img.demand_pages < 2u) {
        err("demand: the loader still materialised every .bss page");
        return SHELL_CONTINUE;
    }
    if (pages >= img.demand_pages || pages > 4u) {
        err("demand: every registered page was faulted in anyway -- no saving");
        return SHELL_CONTINUE;
    }
    ok("M16 PASS: the loader registered the .bss pages instead of allocating them.");
    plain("          only the pages the program actually touched came into RAM,");
    plain("          and the ones it never touched still read as zero when touched later.");
    plain("          (Linux: PTE not present + vm_area -> do_anonymous_page() -> zero page.)");
    return SHELL_CONTINUE;
}
