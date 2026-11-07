#include "ata.h"
#include "console.h"
#include "descriptors.h"
#include "interrupt.h"
#include "io.h"
#include "keyboard.h"
#include "line.h"
#include "login.h"
#include "paging.h"
#include "pmm.h"
#include "printf.h"
#include "shell.h"
#include "string.h"
#include "timer.h"
#include "user.h"

extern u8 kernel_start[];
extern u8 kernel_end[];

static void print_boot_info(u32 memmap_count)
{
    kprintf("  [ok] console     VGA text 80x25 + COM1\n");
    kprintf("  [ok] interrupts  IDT 256 entries, PIC remapped to 0x20\n");
    kprintf("  [ok] timer       PIT 100 Hz\n");
    kprintf("  [ok] keyboard    PS/2 scancode set 1\n");
    kprintf("  [ok] E820        %u memory map entries\n", memmap_count);
    kprintf("  [ok] phys mem    %u KiB free\n",
            (u32)(pmm_free_bytes() / 1024));
    kprintf("  [ok] paging      identity map %u MiB\n",
            paging_mapped_bytes() / (1024u * 1024u));
    kprintf("  [ok] kernel      %u bytes @ 0x%x - 0x%x\n",
            (u32)((uintptr_t)kernel_end - (uintptr_t)kernel_start),
            (u32)(uintptr_t)kernel_start, (u32)(uintptr_t)kernel_end);
    kprintf("  [ok] ata         %s\n",
            ata_present() ? "primary master detected" : "no disk");
    if (!ata_present()) {
        kprintf("       reason: %s (status=0x%x error=0x%x)\n",
                ata_init_reason(), ata_last_status(), ata_last_error());
    }
    kprintf("  [ok] accounts    %d user(s), store %s\n", user_count(),
            user_is_persistent() ? "on disk" : "memory only");
}

void kmain(void)
{
    console_init();
    console_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    console_write("\n  zh_OS 0.1beta - 32-bit x86 kernel\n");
    console_reset_color();
    console_write("  ------------------------------------------\n");

    gdt_init();
    interrupt_init();

    u32 memmap_count = *(volatile u16 *)0x4F00;
    const struct e820_entry *entries =
        (const struct e820_entry *)(uintptr_t)0x5000;
    pmm_init(memmap_count, entries);

    paging_init();

    timer_init();
    keyboard_init();

    ata_init();
    user_init();

    print_boot_info(memmap_count);

    console_write("  ------------------------------------------\n");
    kprintf("  boot took ~%u ms\n", timer_uptime_ms());

    cpu_sti();

    if (user_is_persistent()) {
        /* 有可写账户库: 走正常的登录流程 */
        struct user_session session;

        for (;;) {
            if (login_prompt(&session) == 0) {
                if (shell_run(&session) != 2) {
                    break;
                }
            } else {
                /* 三次失败: 停在这里, 避免被暴力猜测 */
                console_write("  login blocked, reboot to try again\n");
                for (;;) {
                    cpu_cli();
                    cpu_halt();
                }
            }
        }
    } else {
        /* 磁盘不可用: 退化为无登录的单用户模式, 但账户系统本身仍然可用 */
        console_set_color(COLOR_YELLOW, COLOR_BLACK);
        console_write("  [warn] no writable disk, starting as root "
                      "without login\n");
        console_reset_color();
        shell_run(NULL);
    }
}
