/* ============================================================================
 * zh_OS 0.1beta (x86_64)
 * File:    users/login.c
 * Version: 0.1beta
 * Brief:   清屏彩色登录横幅、登录循环与 3 次失败重来。
 * ============================================================================ */

#include "login.h"

#include "console.h"
#include "users.h"
#include "version.h"
#include "vga.h"

/*
 * 登录界面（仿 Linux 控制台 getty/login）。
 *
 * 关键约束：
 *   - console.c 的 read_line() / read_password() 共用同一块 128 字节静态缓冲，
 *     第二次调用会覆盖第一次的结果。所以每次读到内容后必须立刻拷进局部数组，
 *     绝不能把两次调用的返回值同时拿来比较。
 *   - 屏幕输出全部是 ASCII（VGA CP437 没有中文字模）。
 *   - 登录失败只打印 "Login incorrect."，不区分「用户不存在」和「密码错误」，
 *     避免泄露账户名是否存在。
 */

#define BANNER_WIDTH   60u  /* 横幅边框内的 '-' 数量 */
#define BANNER_CONTENT 58u  /* "| " 与 " |" 之间可放的字符数 */
#define MAX_ATTEMPTS   3u

/* ------------------------------------------------------------------ */
/* 本地工具                                                           */
/* ------------------------------------------------------------------ */

static uint32_t str_length(const char *s)
{
    uint32_t n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

/* 带容量上限的拷贝，cap 为数组总字节数。 */
static void str_copy(char *dst, const char *src, uint32_t cap)
{
    uint32_t i = 0;
    while (src[i] != '\0' && i + 1 < cap) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static void print_u32(uint32_t value, enum vga_color color)
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
    vga_print(buf, color, VGA_BLACK);
}

static void print_spaces(uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        vga_putchar(' ', VGA_LIGHT_GREY, VGA_BLACK);
    }
}

static void banner_rule(void)
{
    /* "  " + "+" + 60*'-' + "+" = 64 列 */
    vga_print("  +", VGA_LIGHT_GREEN, VGA_BLACK);
    for (uint32_t i = 0; i < BANNER_WIDTH; ++i) {
        vga_putchar('-', VGA_LIGHT_GREEN, VGA_BLACK);
    }
    vga_println("+", VGA_LIGHT_GREEN, VGA_BLACK);
}

static void banner_line(const char *text, enum vga_color color)
{
    /* "  | " (4) + 58 列内容 + " |" (2) = 64 列，与边框对齐 */
    const uint32_t len = str_length(text);
    const uint32_t left = (len < BANNER_CONTENT)
                              ? (BANNER_CONTENT - len) / 2u : 0u;
    const uint32_t right = (len < BANNER_CONTENT)
                               ? (BANNER_CONTENT - len - left) : 0u;

    vga_print("  | ", VGA_LIGHT_GREEN, VGA_BLACK);
    print_spaces(left);
    vga_print(text, color, VGA_BLACK);
    print_spaces(right);
    vga_println(" |", VGA_LIGHT_GREEN, VGA_BLACK);
}

/* 标题分三段上色：zh_OS 亮绿 / 0.1beta 白 / (x86_64) Login 亮青。 */
static void banner_title(void)
{
    /* 拼接结果统一为 "zh_OS 0.1beta (x86_64) Login"。 */
    static const char part_a[] = ZH_OS_NAME;
    static const char part_b[] = " " ZH_OS_VERSION;
    static const char part_c[] = " (" ZH_OS_ARCH ") Login";
    const uint32_t total = str_length(part_a) + str_length(part_b) +
                           str_length(part_c);
    const uint32_t left = (total < BANNER_CONTENT)
                              ? (BANNER_CONTENT - total) / 2u : 0u;
    const uint32_t right = (total < BANNER_CONTENT)
                               ? (BANNER_CONTENT - total - left) : 0u;

    vga_print("  | ", VGA_LIGHT_GREEN, VGA_BLACK);
    print_spaces(left);
    vga_print(part_a, VGA_LIGHT_GREEN, VGA_BLACK);
    vga_print(part_b, VGA_WHITE, VGA_BLACK);
    vga_print(part_c, VGA_YELLOW, VGA_BLACK);
    print_spaces(right);
    vga_println(" |", VGA_LIGHT_GREEN, VGA_BLACK);
}

static void draw_login_banner(void)
{
    vga_clear(VGA_LIGHT_GREY, VGA_BLACK);
    vga_println("", VGA_LIGHT_GREY, VGA_BLACK);
    banner_rule();
    banner_title();
    banner_line("tty0  |  x86_64  |  Multiboot2", VGA_LIGHT_GREY);
    banner_rule();
    vga_println("", VGA_LIGHT_GREY, VGA_BLACK);
    vga_println("  Default accounts: root / zh   (password: zh)",
                VGA_LIGHT_GREY, VGA_BLACK);
    vga_println("", VGA_LIGHT_GREY, VGA_BLACK);
}

static void print_session_info(const zh_user_t *u)
{
    vga_print("  uid=", VGA_LIGHT_GREY, VGA_BLACK);
    print_u32(u->uid, VGA_LIGHT_GREEN);
    vga_print(" gid=", VGA_LIGHT_GREY, VGA_BLACK);
    print_u32(u->gid, VGA_LIGHT_GREEN);
    vga_print(" home=", VGA_LIGHT_GREY, VGA_BLACK);
    vga_print(u->home, VGA_LIGHT_GREEN, VGA_BLACK);
    vga_print(" shell=", VGA_LIGHT_GREY, VGA_BLACK);
    vga_println(u->shell, VGA_LIGHT_GREEN, VGA_BLACK);
    vga_println("", VGA_LIGHT_GREY, VGA_BLACK);
}

/*
 * 内部登出：清空全局当前用户。
 * login_screen() 每次进入都会调用它，logout() 也通过它保证没有残留会话。
 */
static void do_logout(void)
{
    zh_current_user = 0;
}

/* ------------------------------------------------------------------ */
/* 登录流程                                                           */
/* ------------------------------------------------------------------ */

void login_screen(void)
{
    uint32_t failures = 0;

    do_logout();

    for (;;) {
        draw_login_banner();   /* 清屏 + 彩色横幅；失败 3 次后也从这里重来 */

        for (;;) {
            char username[ZH_USER_NAME_MAX];
            char password[ZH_PASSWORD_MAX];

            char *input = read_line("login: ");
            if (input[0] == '\0') {
                /* 空用户名：重新提示，不读密码、不计失败次数 */
                continue;
            }
            /* read_password 会覆盖 read_line 的静态缓冲，必须立刻拷贝 */
            str_copy(username, input, ZH_USER_NAME_MAX);

            char *secret = read_password("Password: ");
            str_copy(password, secret, ZH_PASSWORD_MAX);

            zh_user_t *u = user_auth(username, password);
            if (u != 0) {
                zh_current_user = u;
                vga_print("Welcome back, ", VGA_LIGHT_GREEN, VGA_BLACK);
                vga_print(u->name, VGA_LIGHT_GREEN, VGA_BLACK);
                vga_println("!", VGA_LIGHT_GREEN, VGA_BLACK);
                print_session_info(u);
                return;
            }

            /* 用户不存在与密码错误统一提示，不泄露账户信息 */
            vga_println("Login incorrect.", VGA_LIGHT_RED, VGA_BLACK);
            ++failures;

            if (failures >= MAX_ATTEMPTS) {
                vga_println("Too many failed attempts. Login restarted.",
                            VGA_YELLOW, VGA_BLACK);
                vga_println("Press Enter to try again.",
                            VGA_YELLOW, VGA_BLACK);
                /* 还没有 PIT 定时器，用一次回车代替延时，让黄字提示能被看到 */
                (void)read_line("");
                failures = 0;
                do_logout();
                break;  /* 回到外层：清屏重绘横幅，重新开始 */
            }

            vga_print("Attempt ", VGA_YELLOW, VGA_BLACK);
            print_u32(failures, VGA_YELLOW);
            vga_print(" of ", VGA_YELLOW, VGA_BLACK);
            print_u32(MAX_ATTEMPTS, VGA_YELLOW);
            vga_println(" failed.", VGA_YELLOW, VGA_BLACK);
            vga_println("", VGA_LIGHT_GREY, VGA_BLACK);
        }
    }
}

void logout(void)
{
    /* login_screen() 开头会调用 do_logout()，这里再显式清一次会话语义更清楚。 */
    do_logout();
    login_screen();
}
