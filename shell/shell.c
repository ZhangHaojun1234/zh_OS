/* shell.c - Shell 主循环、分段彩色提示符、统一的彩色输出封装 */

#include "shell.h"

#include <stdbool.h>

#include "console.h"
#include "console_hist.h"  // [ADD]
#include "users.h"
#include "vga.h"

/* [ADD] 提示符当前工作目录钩子（ramfs 注册后提示符显示真实路径而非 "~"） */
static shell_cwd_fn_t g_cwd_fn = 0;

void shell_set_cwd_fn(shell_cwd_fn_t fn)
{
    g_cwd_fn = fn;
}

/* 统一输出封装                                                       */

static enum vga_color level_color(enum sh_level level)
{
    switch (level) {
    case SH_ERR:   return VGA_LIGHT_RED;
    case SH_OK:    return VGA_LIGHT_GREEN;
    case SH_WARN:  return VGA_YELLOW;
    case SH_INFO:  return VGA_LIGHT_GREEN;   /* Linux 三色：信息用绿 */
    case SH_LOG:   return VGA_YELLOW;        /* Linux 三色：日志提示用黄 */
    case SH_PLAIN: return VGA_LIGHT_GREY;
    default:       return VGA_LIGHT_GREY;
    }
}

void sh_print(enum sh_level level, const char *s)
{
    if (s == 0) {
        return;
    }
    vga_print(s, level_color(level), VGA_BLACK);
}

void sh_println(enum sh_level level, const char *s)
{
    sh_print(level, s);
    vga_putchar('\n', level_color(level), VGA_BLACK);
}

void sh_print_u32(enum sh_level level, uint32_t value)
{
    char buf[11];
    char tmp[10];
    uint32_t n = 0;
    uint32_t t = 0;

    if (value == 0u) {
        buf[n++] = '0';
    } else {
        while (value != 0u) {
            tmp[t++] = (char)('0' + (value % 10u));
            value /= 10u;
        }
        while (t != 0u) {
            buf[n++] = tmp[--t];
        }
    }
    buf[n] = '\0';
    sh_print(level, buf);
}

void sh_newline(void)
{
    vga_putchar('\n', VGA_LIGHT_GREY, VGA_BLACK);
}

/* 彩色提示符                                                         */

/*
 * [user@zh_OS ~]$ / #  ， Linux 风格红 / 黄 / 绿三色分级
 *   user : root 红（危险/特权），普通用户绿（正常）
 *   @    : 浅灰
 *   zh_OS: 黄（提示/标签）
 *   ~    : 绿
 *   #/$  : 与用户名同色（root 红 / 普通绿）
 *   括号、方括号、空格: 浅灰（默认文本）
 */
void shell_print_prompt(void)
{
    const zh_user_t *u = user_current();
    const char *name = (u != 0) ? u->name : "nobody";
    const bool root = user_is_root();
    const enum vga_color accent = root ? VGA_LIGHT_RED : VGA_LIGHT_GREEN;
    const enum vga_color neutral = VGA_LIGHT_GREY;

    vga_print("[", neutral, VGA_BLACK);
    vga_print(name, accent, VGA_BLACK);
    vga_print("@", neutral, VGA_BLACK);
    vga_print(SHELL_HOSTNAME, VGA_YELLOW, VGA_BLACK);
    vga_print(" ", neutral, VGA_BLACK);
    /* [ADD][CHG] 若有 cwd 钩子则用真实路径（含 ~ 缩写），否则回退原 SHELL_CWD */
    const char *__cwd = (g_cwd_fn != 0) ? g_cwd_fn() : SHELL_CWD;
    vga_print(__cwd, VGA_LIGHT_GREEN, VGA_BLACK);
    vga_print("]", neutral, VGA_BLACK);
    vga_print(root ? "# " : "$ ", accent, VGA_BLACK);
}

/* 本地字符串工具（freestanding 无 libc）                             */

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

/* 按空格 / 制表符就地切分，返回参数个数；argv 指向 line 内部。 */
static int split_arguments(char *line, char **argv, int argv_max)
{
    int argc = 0;
    uint32_t i = 0;

    while (line[i] != '\0' && argc < argv_max) {
        while (line[i] == ' ' || line[i] == '\t') {
            ++i;
        }
        if (line[i] == '\0') {
            break;
        }
        argv[argc++] = &line[i];
        while (line[i] != '\0' && line[i] != ' ' && line[i] != '\t') {
            ++i;
        }
        if (line[i] != '\0') {
            line[i++] = '\0';
        }
    }
    return argc;
}

static const shell_command_t *find_command(const char *name)
{
    for (uint32_t i = 0; i < shell_command_table_size; ++i) {
        if (str_equal(shell_command_table[i].name, name)) {
            return &shell_command_table[i];
        }
    }
    return 0;
}

/* Shell 主循环                                                       */

void shell_run(void)
{
    char line[SHELL_LINE_MAX];
    char *argv[SHELL_ARGV_MAX];

    for (;;) {
        shell_print_prompt();

        /* read_line 的返回值指向共享静态缓冲，必须立刻拷走再切分。 */
        char *input = read_line_with_history(""); // [ADD][CHG] 唯一改动点
        str_copy(line, input, SHELL_LINE_MAX);

        const int argc = split_arguments(line, argv, SHELL_ARGV_MAX);
        if (argc == 0) {
            continue;   /* 空行：只重画提示符 */
        }

        const shell_command_t *cmd = find_command(argv[0]);
        if (cmd == 0) {
            sh_print(SH_ERR, "zh_OS: command not found: ");
            sh_println(SH_ERR, argv[0]);
            warn("Type 'help' for a list of commands.");
            continue;
        }

        if (cmd->fn(argc, argv) == SHELL_LOGOUT) {
            return;     /* kernel_main 会重新调用 login_screen() */
        }
    }
}
