#include "login.h"
#include "console.h"
#include "line.h"
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "user.h"

#define PASSWORD_MAX 72
#define DELAY_MS     400

static void login_delay(void)
{
    /* 登录失败时故意放慢, 提高暴力猜测成本 */
    timer_sleep_ms(DELAY_MS);
}

static void print_login_header(void)
{
    console_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    console_write("\n  zh_OS");
    console_reset_color();
    console_write(" login\n\n");
}

int login_prompt(struct user_session *session)
{
    char name[USER_NAME_MAX];
    char password[PASSWORD_MAX];

    user_session_clear(session);
    print_login_header();

    for (int attempt = 0; attempt < LOGIN_MAX_ATTEMPTS; attempt++) {
        console_write("  login: ");
        if (line_read(name, sizeof(name), LINE_ECHO_NORMAL) < 0) {
            continue;
        }
        if (!name[0]) {
            attempt--;
            continue;
        }

        console_write("  password: ");
        if (line_read(password, sizeof(password), LINE_ECHO_MASK) < 0) {
            memset(password, 0, sizeof(password));
            continue;
        }

        int result = user_login(session, name, password);
        memset(password, 0, sizeof(password));

        if (result == 0) {
            console_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
            kprintf("\n  Welcome, %s (uid=%u)\n\n", session->name,
                    session->uid);
            console_reset_color();
            return 0;
        }

        if (result == -1) {
            console_write("  login incorrect: no such user\n");
        } else {
            console_write("  login incorrect\n");
        }
        login_delay();
    }

    console_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
    console_write("\n  too many failed attempts\n");
    console_reset_color();
    return -1;
}

int login_authenticate(const char *name, const char *prompt)
{
    char password[PASSWORD_MAX];
    struct user_account *user = user_find(name);

    if (!user) {
        return -1;
    }

    for (int attempt = 0; attempt < LOGIN_MAX_ATTEMPTS; attempt++) {
        kprintf("%s", prompt);
        if (line_read(password, sizeof(password), LINE_ECHO_MASK) < 0) {
            memset(password, 0, sizeof(password));
            continue;
        }

        int ok = user_check_password(user, password);
        memset(password, 0, sizeof(password));
        if (ok) {
            return 0;
        }

        console_write("  incorrect password\n");
        login_delay();
    }
    return -1;
}
