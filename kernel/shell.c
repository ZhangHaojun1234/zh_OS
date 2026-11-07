#include "shell.h"
#include "console.h"
#include "io.h"
#include "keyboard.h"
#include "line.h"
#include "login.h"
#include "paging.h"
#include "pmm.h"
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "user.h"

#define SHELL_MAX_LINE 128
#define SHELL_HISTORY  8

static char history[SHELL_HISTORY][SHELL_MAX_LINE];
static int history_count;
static int history_next;

static struct user_session session;

/* ------------------------------------------------------------------ */
/* 提示符与小工具                                                      */
/* ------------------------------------------------------------------ */
static void print_banner(void)
{
    console_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    console_write("\n  zh_OS v0.1beta");
    console_reset_color();
    console_write("  a tiny 32-bit x86 hobby kernel\n");
    console_write("  Type ");
    console_set_color(COLOR_YELLOW, COLOR_BLACK);
    console_write("help");
    console_reset_color();
    console_write(" for commands, ");
    console_set_color(COLOR_YELLOW, COLOR_BLACK);
    console_write("logout");
    console_reset_color();
    console_write(" to switch user.\n\n");
}

static void print_prompt(void)
{
    if (user_is_root(&session)) {
        console_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
    } else {
        console_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    }
    kprintf("%s@zh_OS:%s$ ", session.name, session.home);
    console_reset_color();
}

static void history_add(const char *line)
{
    if (!line[0]) {
        return;
    }
    strncpy(history[history_next], line, SHELL_MAX_LINE - 1);
    history[history_next][SHELL_MAX_LINE - 1] = '\0';
    history_next = (history_next + 1) % SHELL_HISTORY;
    if (history_count < SHELL_HISTORY) {
        history_count++;
    }
}

static void permission_denied(const char *what)
{
    console_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
    kprintf("permission denied: %s requires root\n", what);
    console_reset_color();
}

/* ------------------------------------------------------------------ */
/* 命令: 帮助                                                          */
/* ------------------------------------------------------------------ */
static void cmd_help(void)
{
    static const struct {
        const char *name;
        const char *desc;
        const char *need_root;
    } commands[] = {
        { "help",            "show this command list",        "" },
        { "clear",           "clear the screen",              "" },
        { "about",           "about zh_OS",                   "" },
        { "version",         "version and build info",        "" },
        { "mem",             "physical / virtual memory stats", "" },
        { "memmap",          "dump the E820 memory map",      "" },
        { "uptime",          "milliseconds since boot",       "" },
        { "echo TEXT",       "print TEXT back",               "" },
        { "cpuid",           "show CPU vendor and features",  "" },
        { "history",         "list recent commands",          "" },
        { "whoami",          "print the current user name",   "" },
        { "id",              "print uid / gid / groups",      "" },
        { "users",           "list accounts on this system",  "" },
        { "passwd",          "change your own password",      "" },
        { "su [USER]",       "switch to another user",        "" },
        { "logout",          "log out and return to login",   "" },
        { "useradd USER",    "create a user",                 "root" },
        { "userdel USER",    "delete a user",                 "root" },
        { "passwd USER",     "reset another user password",   "root" },
        { "reboot",          "reboot the machine",            "root" },
        { "halt",            "stop the CPU",                  "root" },
    };

    console_write("\n  Commands:\n");
    for (u32 i = 0; i < ARRAY_SIZE(commands); i++) {
        console_set_color(COLOR_YELLOW, COLOR_BLACK);
        kprintf("    %-16s", commands[i].name);
        console_reset_color();
        kprintf(" %s", commands[i].desc);
        if (commands[i].need_root[0]) {
            console_set_color(COLOR_DARK_GREY, COLOR_BLACK);
            console_write("   [root]");
            console_reset_color();
        }
        console_write("\n");
    }
    console_write("\n");
}

static void cmd_about(void)
{
    console_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    console_write("\n  zh_OS v0.1beta\n");
    console_reset_color();
    console_write("  A small 32-bit x86 kernel written from scratch.\n");
    console_write("  - two stage bootloader written in NASM\n");
    console_write("  - freestanding C kernel, no host OS dependency\n");
    console_write("  - GDT / IDT / PIC / PIT / PS/2 keyboard / paging\n");
    console_write("  - bitmap physical page allocator\n");
    console_write("  - ATA PIO disk driver\n");
    console_write("  - salted SHA-256 password authentication\n");
    console_write("  - Linux style accounts: uid/gid, home, login shell\n\n");
}

static void cmd_version(void)
{
    kprintf("zh_OS 0.1beta (i386)\n");
    kprintf("build    : %s %s\n", __DATE__, __TIME__);
    kprintf("compiler : clang, freestanding -m32\n");
    kprintf("accounts : %s\n",
            user_is_persistent() ? "persistent (ATA)" : "memory only");
}

static void cmd_mem(void)
{
    u64 total = pmm_total_bytes();
    u64 freeb = pmm_free_bytes();

    kprintf("physical total : %u KiB (%u pages)\n", (u32)(total / 1024),
            pmm_total_pages());
    kprintf("physical free  : %u KiB (%u pages)\n", (u32)(freeb / 1024),
            pmm_free_pages());
    kprintf("physical used  : %u KiB (%u pages)\n",
            (u32)((total - freeb) / 1024), pmm_used_pages());
    kprintf("paging         : %s, identity map %u MiB\n",
            paging_enabled() ? "on" : "off",
            paging_mapped_bytes() / (1024u * 1024u));
}

static void cmd_memmap(void)
{
    u32 count = *(volatile u16 *)0x4F00;
    const struct e820_entry *entries =
        (const struct e820_entry *)(uintptr_t)0x5000;

    kprintf("E820 entries: %u\n", count);
    kprintf("  base              length            type\n");
    for (u32 i = 0; i < count && i < 32; i++) {
        kprintf("  %08x%08x  %08x%08x  %u\n",
                (u32)(entries[i].base >> 32), (u32)entries[i].base,
                (u32)(entries[i].length >> 32), (u32)entries[i].length,
                entries[i].type);
    }
}

static void cpuid(u32 leaf, u32 *eax, u32 *ebx, u32 *ecx, u32 *edx)
{
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(0));
}

static void cmd_cpuid(void)
{
    u32 eax, ebx, ecx, edx;
    char vendor[13];

    cpuid(0, &eax, &ebx, &ecx, &edx);
    memcpy(vendor + 0, &ebx, 4);
    memcpy(vendor + 4, &edx, 4);
    memcpy(vendor + 8, &ecx, 4);
    vendor[12] = '\0';

    kprintf("vendor  : %s\n", vendor);

    cpuid(1, &eax, &ebx, &ecx, &edx);
    kprintf("family  : %u model %u stepping %u\n",
            (eax >> 8) & 0xF, (eax >> 4) & 0xF, eax & 0xF);
    kprintf("features: %s%s%s%s\n",
            (edx & (1u << 0)) ? "FPU " : "",
            (edx & (1u << 4)) ? "TSC " : "",
            (edx & (1u << 5)) ? "MSR " : "",
            (edx & (1u << 6)) ? "PAE " : "");
}

static void cmd_reboot(void)
{
    console_write("rebooting...\n");

    /* 1. 键盘控制器 8042 的脉冲复位线, 部分固件/模拟器支持 */
    for (volatile int i = 0; i < 1000000; i++) {
    }
    outb(0x64, 0xFE);
    io_wait();

    /* 2. 三重故障: 清空 IDT 后主动触发异常, CPU 会走复位流程。
          这是最通用的重启方式, QEMU / VirtualBox / VMware 都可靠。 */
    struct {
        u16 limit;
        u32 base;
    } PACKED null_idt = { 0, 0 };

    __asm__ volatile("lidt %0" : : "m"(null_idt));
    __asm__ volatile("int3");

    /* 3. 理论上不会到这里 */
    for (;;) {
        cpu_cli();
        cpu_halt();
    }
}

static void cmd_halt(void)
{
    console_write("CPU halted, you can close the VM window now.\n");
    for (;;) {
        cpu_cli();
        cpu_halt();
    }
}

/* ------------------------------------------------------------------ */
/* 命令: 账户                                                          */
/* ------------------------------------------------------------------ */
static void cmd_whoami(void)
{
    console_write(session.name);
    console_write("\n");
}

static void cmd_id(void)
{
    kprintf("uid=%u(%s) gid=%u(%s)\n", session.uid, session.name, session.gid,
            user_group_name(session.gid));

    /* root 的 gid 就是 root 组, 不要重复打印自己 */
    if (session.gid == GID_ROOT) {
        kprintf("groups=%u(%s)\n", GID_ROOT, user_group_name(GID_ROOT));
    } else {
        kprintf("groups=%u(%s),%u(%s)\n", session.gid,
                user_group_name(session.gid), GID_ROOT,
                user_group_name(GID_ROOT));
    }
}

static void cmd_users(void)
{
    kprintf("  %-16s %-6s %-6s %-8s %s\n", "NAME", "UID", "GID", "ACTIVE",
            "HOME");
    for (int i = 0; i < user_count(); i++) {
        struct user_account *u = user_at(i);
        if (!u) {
            continue;
        }
        console_set_color(u->uid == UID_ROOT ? COLOR_LIGHT_RED : COLOR_YELLOW,
                          COLOR_BLACK);
        kprintf("  %-16s", u->name);
        console_reset_color();
        kprintf(" %-6u %-6u %-8s %s\n", u->uid, u->gid,
                u->active ? "yes" : "no", u->home);
    }
    kprintf("\n  %d account(s), store: %s\n", user_count(),
            user_is_persistent() ? "on disk" : "memory only");
}

static void cmd_passwd(const char *target)
{
    char first[72];
    char second[72];
    const char *name;
    struct user_account *user;

    if (target && target[0]) {
        if (!user_is_root(&session)) {
            permission_denied("passwd USER");
            return;
        }
        name = target;
        user = user_find(name);
        if (!user) {
            kprintf("passwd: user '%s' does not exist\n", name);
            return;
        }
    } else {
        name = session.name;
        user = user_find(name);
        if (!user) {
            console_write("passwd: your account is missing\n");
            return;
        }
        /* 改自己的口令必须先验证旧口令 */
        if (login_authenticate(name, "  current password: ") != 0) {
            console_write("passwd: authentication failed\n");
            return;
        }
    }

    console_write("  new password: ");
    if (line_read(first, sizeof(first), LINE_ECHO_MASK) < 0) {
        return;
    }
    console_write("  repeat password: ");
    if (line_read(second, sizeof(second), LINE_ECHO_MASK) < 0) {
        memset(first, 0, sizeof(first));
        return;
    }

    if (strcmp(first, second) != 0) {
        memset(first, 0, sizeof(first));
        memset(second, 0, sizeof(second));
        console_write("passwd: passwords do not match\n");
        return;
    }

    const char *reason = user_password_policy(name, first);
    if (reason) {
        memset(first, 0, sizeof(first));
        memset(second, 0, sizeof(second));
        kprintf("passwd: %s\n", reason);
        return;
    }

    user_set_password(user, first);
    memset(first, 0, sizeof(first));
    memset(second, 0, sizeof(second));

    if (user_save() != 0) {
        kprintf("passwd: changed in memory but not saved (%s)\n",
                user_store_error());
        return;
    }
    kprintf("passwd: password updated for '%s'\n", name);
}

static void cmd_useradd(const char *name)
{
    char first[72];
    char second[72];
    u32 uid;

    if (!name || !name[0]) {
        console_write("usage: useradd USER\n");
        return;
    }
    if (user_find(name)) {
        kprintf("useradd: user '%s' already exists\n", name);
        return;
    }

    console_write("  new password: ");
    if (line_read(first, sizeof(first), LINE_ECHO_MASK) < 0) {
        return;
    }
    console_write("  repeat password: ");
    if (line_read(second, sizeof(second), LINE_ECHO_MASK) < 0) {
        memset(first, 0, sizeof(first));
        return;
    }

    if (strcmp(first, second) != 0) {
        memset(first, 0, sizeof(first));
        memset(second, 0, sizeof(second));
        console_write("useradd: passwords do not match\n");
        return;
    }

    const char *reason = user_password_policy(name, first);
    if (reason) {
        memset(first, 0, sizeof(first));
        memset(second, 0, sizeof(second));
        kprintf("useradd: %s\n", reason);
        return;
    }

    uid = user_next_uid();

    char home[USER_HOME_MAX];
    strcpy(home, "/home/");
    strncpy(home + 6, name, USER_HOME_MAX - 7);
    home[USER_HOME_MAX - 1] = '\0';

    if (user_add(name, first, uid, GID_USERS, home, "/bin/sh") != 0) {
        memset(first, 0, sizeof(first));
        memset(second, 0, sizeof(second));
        kprintf("useradd: cannot create '%s' (invalid name or no free slot)\n",
                name);
        return;
    }

    memset(first, 0, sizeof(first));
    memset(second, 0, sizeof(second));

    if (user_save() != 0) {
        kprintf("useradd: created in memory but not saved (%s)\n",
                user_store_error());
        return;
    }
    kprintf("useradd: created '%s' with uid=%u, home=%s\n", name, uid, home);
}

static void cmd_userdel(const char *name)
{
    if (!name || !name[0]) {
        console_write("usage: userdel USER\n");
        return;
    }
    if (!strcmp(name, session.name)) {
        console_write("userdel: cannot delete the account you are using\n");
        return;
    }

    int result = user_remove(name);
    if (result == -1) {
        kprintf("userdel: user '%s' does not exist\n", name);
        return;
    }
    if (result == -2) {
        console_write("userdel: refusing to delete root\n");
        return;
    }

    if (user_save() != 0) {
        kprintf("userdel: removed in memory but not saved (%s)\n",
                user_store_error());
        return;
    }
    kprintf("userdel: removed '%s'\n", name);
}

/* su 成功返回 1, 表示需要重新加载会话 */
static int cmd_su(const char *target)
{
    char password[72];
    struct user_account *user;
    const char *name = (target && target[0]) ? target : "root";

    user = user_find(name);
    if (!user) {
        kprintf("su: user '%s' does not exist\n", name);
        return 0;
    }

    if (!strcmp(name, session.name)) {
        console_write("su: you are already that user\n");
        return 0;
    }

    kprintf("  password for %s: ", name);
    if (line_read(password, sizeof(password), LINE_ECHO_MASK) < 0) {
        return 0;
    }

    int ok = user_check_password(user, password);
    memset(password, 0, sizeof(password));

    if (!ok) {
        timer_sleep_ms(400);
        console_write("su: authentication failure\n");
        return 0;
    }

    user_session_clear(&session);
    session.logged_in = 1;
    session.uid = user->uid;
    session.gid = user->gid;
    strncpy(session.name, user->name, USER_NAME_MAX - 1);
    strncpy(session.home, user->home, USER_HOME_MAX - 1);

    kprintf("su: now acting as %s (uid=%u)\n", session.name, session.uid);
    return 1;
}

/* ------------------------------------------------------------------ */
/* 命令分发                                                            */
/* ------------------------------------------------------------------ */
static int execute(char *line)
{
    char *cmd = line;
    while (*cmd == ' ') {
        cmd++;
    }

    char *arg = strchr(cmd, ' ');
    if (arg) {
        *arg++ = '\0';
        while (*arg == ' ') {
            arg++;
        }
    }

    if (!cmd[0]) {
        return 0;
    }

    /* 只有 root 能用的命令集中在这里, 免得每条命令重复判断 */
    static const struct {
        const char *name;
        const char *what;
    } root_only[] = {
        { "useradd",  "useradd" },
        { "userdel",  "userdel" },
        { "reboot",   "reboot" },
        { "halt",     "halt" },
        { "shutdown", "shutdown" },
    };

    for (u32 i = 0; i < ARRAY_SIZE(root_only); i++) {
        if (!strcmp(cmd, root_only[i].name) && !user_is_root(&session)) {
            permission_denied(root_only[i].what);
            return 0;
        }
    }

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
        cmd_help();
    } else if (!strcmp(cmd, "clear") || !strcmp(cmd, "cls")) {
        console_clear();
        print_banner();
    } else if (!strcmp(cmd, "about")) {
        cmd_about();
    } else if (!strcmp(cmd, "version") || !strcmp(cmd, "ver")) {
        cmd_version();
    } else if (!strcmp(cmd, "mem")) {
        cmd_mem();
    } else if (!strcmp(cmd, "memmap")) {
        cmd_memmap();
    } else if (!strcmp(cmd, "uptime")) {
        kprintf("uptime: %u ms (%u ticks)\n", timer_uptime_ms(),
                (u32)timer_ticks());
    } else if (!strcmp(cmd, "echo")) {
        console_write(arg ? arg : "");
        console_write("\n");
    } else if (!strcmp(cmd, "cpuid")) {
        cmd_cpuid();
    } else if (!strcmp(cmd, "history")) {
        for (int i = 0; i < history_count; i++) {
            int idx = (history_next - history_count + i + SHELL_HISTORY)
                      % SHELL_HISTORY;
            kprintf("  %d  %s\n", i + 1, history[idx]);
        }
    } else if (!strcmp(cmd, "whoami")) {
        cmd_whoami();
    } else if (!strcmp(cmd, "id")) {
        cmd_id();
    } else if (!strcmp(cmd, "users")) {
        cmd_users();
    } else if (!strcmp(cmd, "passwd")) {
        cmd_passwd(arg);
    } else if (!strcmp(cmd, "useradd")) {
        cmd_useradd(arg);
    } else if (!strcmp(cmd, "userdel")) {
        cmd_userdel(arg);
    } else if (!strcmp(cmd, "su")) {
        cmd_su(arg);
    } else if (!strcmp(cmd, "logout") || !strcmp(cmd, "exit")) {
        return 2;
    } else if (!strcmp(cmd, "reboot")) {
        cmd_reboot();
    } else if (!strcmp(cmd, "halt") || !strcmp(cmd, "shutdown")) {
        cmd_halt();
    } else {
        console_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        kprintf("unknown command: %s", cmd);
        console_reset_color();
        console_write("  (try 'help')\n");
    }
    return 0;
}

/* 返回值: 0 = 正常, 2 = 用户要求登出 (由 main 重新走登录流程) */
int shell_run(struct user_session *initial)
{
    char line[SHELL_MAX_LINE];

    if (initial) {
        memcpy(&session, initial, sizeof(session));
    } else {
        user_session_clear(&session);
        session.logged_in = 1;
        session.uid = UID_ROOT;
        session.gid = GID_ROOT;
        strcpy(session.name, "root");
        strcpy(session.home, "/root");
    }

    console_write("\n");
    print_banner();

    for (;;) {
        print_prompt();

        int length = line_read(line, sizeof(line), LINE_ECHO_NORMAL);
        if (length < 0) {
            continue;
        }

        history_add(line);

        int action = execute(line);
        if (action == 2) {
            return 2;
        }
    }
}
