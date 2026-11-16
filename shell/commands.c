/* commands.c - 内置命令的实现与静态命令表 */

#include "shell.h"

#include <stdbool.h>
#include <stddef.h>

#include "console.h"
#include "login.h"
#include "users.h"
#include "version.h"
#include "vga.h"
#include "../mm/heap.h"  // [ADD]
#include "cmd_time.h"  // [ADD]
#include "klog.h"  // [ADD]
#include "../arch/cpuinfo.h"  // [ADD]
#include "../fs/ramfs.h"  // [ADD]
#include "../drivers/ata.h"  // [ADD]  B 组第 2 个功能：ATA 驱动 + diskinfo 声明
#include "../users/users_persist.h"  // [ADD]  B 组第 2 个功能：用户表持久化
#include "../fs/fat16.h"  // [ADD]  B 组第 3 个功能：FAT16 只读挂载（fatmount/fatls/fatcat）
#include "cmd_pci.h"     // [ADD] M30/M31：pci / ahci
#include "../fs/ext2.h"   // [ADD]  M19：ext2 只读挂载（ext2mount/ext2info/ext2ls/ext2cat）
#include "../fs/vfs.h"
#include "../drivers/fb.h"   // [ADD] M25：videomode    // [ADD]  M20：挂载表（mount/umount/mounts）
#include "cmd_user.h"  // [ADD]  M1 用户态：ring3 命令
#include "cmd_syscall.h"  // [ADD]  M2 系统调用：syscall 命令
#include "cmd_memmap.h"  // [ADD]  M3 物理内存：memmap 命令
#include "cmd_task.h"  // [ADD]  M4 进程与调度：tasks / taskrun 命令
#include "cmd_vmm.h"  // [ADD]  M5 进程地址空间：vmspace 命令
#include "cmd_elf.h"
#include "cmd_fd.h"   // [ADD] M9
#include "cmd_m10.h"  // [ADD] M10
#include "cmd_argv.h" // [ADD] M11
#include "cmd_mm.h"   // [ADD] M12
#include "cmd_sh3.h"
#include "cmd_cow.h"      /* [ADD] M15 */
#include "cmd_demand.h"   /* [ADD] M16 */
#include "cmd_sig.h"      /* [ADD] M17 */


/*
 * 第 5 步的内置命令表。全部输出走 shell.h 的统一输出宏和 sh_print* 封装，屏上的字符串必须是
 * ASCII，VGA CP437 没有中文字模。权限判断在命令层和 users.c 里各做一次：命令层给统一的
 * "Permission denied." 亮红提示，users.c 兜底。
 */

/* useradd 新建账户的初始密码，与第 4 步内置账户保持一致，便于验收登录。 */
#define NEW_USER_PASSWORD "zh"

/* 本地字符串工具（freestanding 无 libc）                             */

static uint32_t str_length(const char *s)
{
    uint32_t n = 0;
    if (s == 0) {
        return 0;
    }
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

static int str_equal(const char *a, const char *b)
{
    if (a == 0 || b == 0) {
        return a == b;
    }
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

/* 带容量上限的拷贝；read_password 共享静态缓冲，读到必须立刻拷走。 */
static void str_copy(char *dst, const char *src, uint32_t cap)
{
    uint32_t i = 0;

    if (cap == 0) {
        return;
    }
    if (src == 0) {
        dst[0] = '\0';
        return;
    }
    while (src[i] != '\0' && i + 1 < cap) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

/* 端口输出，供 reboot 通过 8042 键盘控制器拉低 CPU 复位线。 */
static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void halt_cpu(void)
{
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

/* 命令实现                                                           */

static int cmd_help(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    info("zh_OS shell - built-in commands:");
    for (uint32_t i = 0; i < shell_command_table_size; ++i) {
        const char *name = shell_command_table[i].name;
        uint32_t len;

        sh_print(SH_PLAIN, "  ");
        sh_print(SH_INFO, name);
        len = str_length(name);
        while (len < 12u) {
            sh_print(SH_PLAIN, " ");
            ++len;
        }
        sh_println(SH_PLAIN, shell_command_table[i].help);
    }
    warn("Root-only: useradd. Use 'su root' before creating accounts.");
    return SHELL_CONTINUE;
}

static int cmd_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    vga_clear(VGA_LIGHT_GREY, VGA_BLACK);
    return SHELL_CONTINUE;
}

static int cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (i > 1) {
            sh_print(SH_INFO, " ");
        }
        sh_print(SH_INFO, argv[i]);
    }
    sh_newline();
    return SHELL_CONTINUE;
}

static int cmd_uname(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* 与启动横幅、GRUB 菜单共用同一版本字符串。 */
    ok(ZH_OS_VERSION_TEXT);
    return SHELL_CONTINUE;
}

static int cmd_whoami(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const zh_user_t *u = user_current();
    if (u == 0) {
        err("whoami: no active session.");
        return SHELL_CONTINUE;
    }
    sh_println(SH_OK, u->name);
    return SHELL_CONTINUE;
}

/* 在当前 Shell 里重新走一遍登录流程，返回后提示符按新用户重新上色。 */
static int cmd_login(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    login_screen();
    return SHELL_CONTINUE;
}

/* 请求退出 Shell；kernel_main 会再次调用 login_screen()。 */
static int cmd_logout(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    plain("logout");
    return SHELL_LOGOUT;
}

static int cmd_su(int argc, char **argv)
{
    const char *target = (argc >= 2) ? argv[1] : "root";
    char password[ZH_PASSWORD_MAX];

    char *secret = read_password("Password: ");
    str_copy(password, secret, ZH_PASSWORD_MAX);

    zh_user_t *u = user_auth(target, password);
    if (u == 0) {
        err("su: Authentication failure.");
        return SHELL_CONTINUE;
    }

    /* 与 login.c 一致：直接设置全局会话指针。 */
    zh_current_user = u;
    sh_print(SH_OK, "su: switched to ");
    sh_println(SH_OK, u->name);
    return SHELL_CONTINUE;
}

static int cmd_useradd(int argc, char **argv)
{
    if (argc < 2) {
        err("Usage: useradd <name>");
        return SHELL_CONTINUE;
    }

    if (!user_is_root()) {
        err("Permission denied.");
        return SHELL_CONTINUE;
    }

    /*
     * uid/gid 分配：内置 root=0、zh=1000，从 1000 加当前账户数开始，所以第一个新账户是 1002，
     * 不会跟内置账户撞。账户表是静态数组，不提供删除，递增分配够用。
     */
    const uint32_t uid = 1000u + user_count();
    const int rc = user_add(argv[1], uid, uid, 0, 0,
                            NEW_USER_PASSWORD, false);
    if (rc == USER_ERR_EXISTS) {
        err("useradd: user already exists.");
        return SHELL_CONTINUE;
    }
    if (rc == USER_ERR_FULL) {
        err("useradd: account table is full.");
        return SHELL_CONTINUE;
    }
    if (rc != USER_OK) {
        err("useradd: invalid user name.");
        return SHELL_CONTINUE;
    }

    sh_print(SH_OK, "useradd: user '");
    sh_print(SH_OK, argv[1]);
    sh_print(SH_OK, "' created, uid=");
    sh_print_u32(SH_OK, uid);
    sh_print(SH_OK, ", default password: ");
    sh_println(SH_OK, NEW_USER_PASSWORD);

    int save_rc = users_save(); // [ADD] 落盘；无磁盘返回 ATA_ENODEV
    if (save_rc < 0) { // [ADD] 从 ISO 启动（只读）时写盘失败
        vga_print("[warn] disk read-only, changes lost on reboot\n", VGA_YELLOW, VGA_BLACK); // [ADD]
    } // [ADD]
    return SHELL_CONTINUE;
}

static int cmd_passwd(int argc, char **argv)
{
    const char *target = (argc >= 2) ? argv[1] : 0;
    char first[ZH_PASSWORD_MAX];
    char second[ZH_PASSWORD_MAX];
    char *input;

    if (target != 0 && !user_is_root()) {
        err("Permission denied.");
        return SHELL_CONTINUE;
    }
    if (target != 0 && user_find(target) == 0) {
        err("passwd: user does not exist.");
        return SHELL_CONTINUE;
    }

    /* 两次 read_password 共用静态缓冲，每次读完立刻拷贝。 */
    input = read_password("New password: ");
    str_copy(first, input, ZH_PASSWORD_MAX);

    input = read_password("Retype new password: ");
    str_copy(second, input, ZH_PASSWORD_MAX);

    if (first[0] == '\0' || !str_equal(first, second)) {
        err("passwd: empty password or passwords do not match.");
        return SHELL_CONTINUE;
    }

    const int rc = user_set_password(target, first);
    if (rc == USER_ERR_PERMISSION) {
        err("Permission denied.");
        return SHELL_CONTINUE;
    }
    if (rc != USER_OK) {
        err("passwd: could not update password.");
        return SHELL_CONTINUE;
    }
    ok("passwd: password updated (RAM only, reset on reboot).");

    int save_rc = users_save(); // [ADD] 落盘；无磁盘返回 ATA_ENODEV
    if (save_rc < 0) { // [ADD] 从 ISO 启动（只读）时写盘失败
        vga_print("[warn] disk read-only, changes lost on reboot\n", VGA_YELLOW, VGA_BLACK); // [ADD]
    } // [ADD]
    return SHELL_CONTINUE;
}

/* 把 16 种前景色排成 8 行 x 2 列，一次显示全部色板。 */
static void fill_palette_cell(char *buf, uint32_t idx, const char *name)
{
    uint32_t n = 0;
    const char *p = name;

    buf[n++] = '[';
    buf[n++] = (char)('0' + (idx / 10u));
    buf[n++] = (char)('0' + (idx % 10u));
    buf[n++] = ']';
    buf[n++] = ' ';
    while (*p != '\0') {
        buf[n++] = *p++;
    }
    while (n < 20u) {
        buf[n++] = ' ';
    }
    for (uint32_t i = 0; i < 8u; ++i) {
        buf[n++] = '#';
    }
    buf[n++] = ' ';
    buf[n] = '\0';
}

static int cmd_color(int argc, char **argv)
{
    static const char *const names[16] = {
        "BLACK",          "BLUE",           "GREEN",        "CYAN",
        "RED",            "MAGENTA",        "BROWN",        "LIGHT_GREY",
        "DARK_GREY",      "LIGHT_BLUE",     "LIGHT_GREEN",  "LIGHT_CYAN",
        "LIGHT_RED",      "LIGHT_MAGENTA",  "YELLOW",       "WHITE",
    };

    (void)argc;
    (void)argv;

    info("VGA 16-color palette:");
    for (uint32_t row = 0; row < 8u; ++row) {
        for (uint32_t col = 0; col < 2u; ++col) {
            const uint32_t idx = row + col * 8u;
            char cell[40];

            fill_palette_cell(cell, idx, names[idx]);
            /* 黑色前景在黑底上不可见，这一格改用浅灰背景。 */
            const enum vga_color bg =
                (idx == 0u) ? VGA_LIGHT_GREY : VGA_BLACK;
            vga_print(cell, (enum vga_color)idx, bg);
        }
        sh_newline();
    }
    return SHELL_CONTINUE;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    warn("Rebooting zh_OS ...");
    /* 8042 键盘控制器命令 0xFE：脉冲 CPU 复位线。 */
    outb(0x64u, 0xFEu);
    halt_cpu();
    return SHELL_CONTINUE;  /* 不可达 */
}

static int cmd_shutdown(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    warn("System halted.");
    /* 还没有 ACPI/APM 电源管理，只能停机；QEMU 里用 Ctrl+C 退出。 */
    halt_cpu();
    return SHELL_CONTINUE;  /* 不可达 */
}

/* 命令表                                                             */

const shell_command_t shell_command_table[] = {
    { "help",     cmd_help,     "Show this help" },
    { "clear",    cmd_clear,    "Clear the screen" },
    { "echo",     cmd_echo,     "Print arguments" },
    { "uname",    cmd_uname,    "Print system information" },
    { "whoami",   cmd_whoami,   "Print current user name" },
    { "login",    cmd_login,    "Start a new login session" },
    { "logout",   cmd_logout,   "Log out and return to login" },
    { "su",       cmd_su,       "Switch user (default: root)" },
    { "useradd",  cmd_useradd,  "Create a user (root only)" },
    { "passwd",   cmd_passwd,   "Change a password" },
    { "color",    cmd_color,    "Show the 16-color palette" },
    { "reboot",   cmd_reboot,   "Reboot the machine" },
    { "shutdown", cmd_shutdown, "Halt the machine" },
    { "meminfo",  cmd_meminfo,  "Show kernel heap usage" }, // [ADD]
    { "uptime",   cmd_uptime,   "Show system uptime" }, // [ADD]
    { "date",     cmd_date,     "Show RTC date and time" }, // [ADD]
    { "dmesg",    cmd_dmesg,    "Show kernel log ring" }, // [ADD]
    { "cpuinfo",  cmd_cpuinfo,  "Show CPU vendor and features" }, // [ADD]
    { "ls",       cmd_ls,       "List directory contents" }, // [ADD]
    { "cd",       cmd_cd,       "Change current directory" }, // [ADD]
    { "pwd",      cmd_pwd,      "Print current directory" }, // [ADD]
    { "cat",      cmd_cat,      "Print file contents" }, // [ADD]
    { "mkdir",    cmd_mkdir,    "Create a directory" }, // [ADD]
    { "touch",    cmd_touch,    "Create an empty file" }, // [ADD]
    { "rm",       cmd_rm,       "Remove a file or empty directory" }, // [ADD]
    { "diskinfo", cmd_diskinfo, "Show ATA disk and persistence info" }, // [ADD]
    { "fatmount", cmd_fatmount, "Mount FAT16 on the second IDE disk" }, // [ADD]
    { "fatls",    cmd_fatls,    "List a FAT16 directory" }, // [ADD]
    { "fatcat",   cmd_fatcat,   "Print a FAT16 file" }, // [ADD]
    { "ring3",    cmd_ring3,    "Enter ring 3 (user mode) self-test" }, // [ADD]
    { "syscall",  cmd_syscall,  "Run ring3 demo over int 0x80 (M2)" }, // [ADD]
    { "memmap",   cmd_memmap,   "Show physical memory map and frames" }, // [ADD]
    { "tasks",    cmd_tasks,    "Show the task table (M4)" }, // [ADD]
    { "taskrun",  cmd_taskrun,  "Run two ring3 tasks (M4); 'taskrun fault' kills one" }, // [ADD]
    { "vmspace",  cmd_vmspace,  "Dump each process's page tables (M5)" }, // [ADD]
    { "elf",      cmd_elf,      "Load an embedded ELF64 and run it in ring3 (M6)" }, // [ADD]
    { "fd",       cmd_fd,       "Let a ring3 program open/read/close a file itself (M9)" }, // [ADD]
    { "wr",       cmd_wr,       "A ring3 program creates and writes a file, then reads it back (M10)" }, // [ADD]
    { "spawn",    cmd_spawn,    "fork -> child exec -> parent wait, the shell-command loop (M10)" }, // [ADD]
    { "argv",     cmd_argv,     "exec() carries argc/argv into the new image (M11)" }, // [ADD]
    { "mm",       cmd_mm,       "exec() reclaims the address space it replaces (M12)" }, // [ADD]
    { "sh3",      cmd_sh3,      "start the ring-3 shell (/bin/zhsh3.elf) (M13)" }, // [ADD]
    { "cow",      cmd_cow,      "fork() shares pages instead of copying them (M15)" }, // [ADD]
    { "demand",   cmd_demand,   ".bss pages cost nothing until they are touched (M16)" }, // [ADD]
    { "sig",      cmd_sig,      "kill/waitpid: a queued signal fires on the way back to ring3 (M17)" }, // [ADD]
    { "ext2mount", cmd_ext2mount, "Mount ext2 read-only on the primary IDE disk (M19)" }, // [ADD] M19
    { "ext2info",  cmd_ext2info,  "Show the parsed ext2 superblock geometry (M19)" },      // [ADD] M19
    { "ext2ls",    cmd_ext2ls,    "List an ext2 directory (superblock->gdt->inode->dir) (M19)" }, // [ADD] M19
    { "ext2cat",   cmd_ext2cat,   "Print an ext2 file (direct + indirect blocks) (M19)" }, // [ADD] M19
    { "mount",     cmd_mount,     "Mount a filesystem at a directory: mount ext2 /mnt (M20)" }, // [ADD] M20
    { "umount",    cmd_umount,    "Remove a mount point from the single namespace (M20)" },     // [ADD] M20
    { "mounts",    cmd_mounts,    "Show the mount table, longest-prefix wins (M20)" },          // [ADD] M20
    { "ext2create", cmd_ext2create, "Create a file on ext2 and write its content to disk (M21)" }, // [ADD] M21
    { "ext2free",   cmd_ext2free,   "Show ext2 free blocks / free inodes from the superblock (M21)" }, // [ADD] M21
    { "ext2fill",   cmd_ext2fill,   "Write a >12KB ext2 file, so the indirect block gets used (M22)" }, // [ADD] M22
    { "ext2big",    cmd_ext2big,    "Write a >268KB ext2 file, so double+triple indirect blocks get used (M28)" }, // [ADD] M28
    { "ext2tail",   cmd_ext2tail,   "Read the last bytes of a big ext2 file: walks the indirect tree (M28)" }, // [ADD] M28
    { "pci",        cmd_pci,        "Enumerate PCI devices via 0xCF8/0xCFC config space (M30)" }, // [ADD] M30
    { "ahci",       cmd_ahci,       "Show the AHCI controller/port and read LBA0 through DMA (M31)" }, // [ADD] M31
    { "nvme",       cmd_nvme,       "Show the NVMe controller/namespace and read LBA2 through queues (M32)" }, // [ADD] M32
    { "usb",        cmd_usb,        "Show the xHCI controller, HID keyboard and report counters (M33)" }, // [ADD] M33
    { "ext2rm",     cmd_ext2rm,     "Delete an ext2 file: free its blocks and its inode (M23)" }, // [ADD] M23
    { "videomode",  cmd_videomode,  "Show the framebuffer geometry the kernel got from GRUB (M25)" }, // [ADD] M25
    { "res",        cmd_res,        "Switch resolution at runtime: res 1024x768 (M25)" },            // [ADD] M25
    { "fontscale",  cmd_fontscale,  "Scale the 8x16 font 1..4x for high-DPI screens: fontscale 2 (M34)" }, // [ADD] M34
    { "memtype",    cmd_memtype,    "Dump MTRRdefType and all variable MTRRs; shows how much of the fb is WC (M34)" }, // [ADD] M34
    { "bench",      cmd_bench,      "Time a full-screen framebuffer write in cycles/byte (<10 = WC on) (M34)" }, // [ADD] M34
};

const uint32_t shell_command_table_size =
    (uint32_t)(sizeof(shell_command_table) / sizeof(shell_command_table[0]));
