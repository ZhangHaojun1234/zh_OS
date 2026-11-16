/* shell.h - 输出级别与宏、命令表类型、Shell 接口声明 */

#ifndef ZH_OS_SHELL_H
#define ZH_OS_SHELL_H

#include <stdint.h>

/*
 * 第 5 步：Shell 主循环和统一输出接口。freestanding 环境没有 libc，命令表是一块静态 const
 * 数组，每个命令签名统一为 int fn(int argc, char argv)。提示符 [user@zh_OS ~]$ 或 # 由
 * shell_print_prompt() 分段上色打印，然后用 read_line("") 传空提示，第 3 步那个 read_line
 * 会把 prompt 固定染成青色，画不了多色提示符，输入本身还是 read_line 逐字符回显。
 * read_line() / read_password() 共用一块静态缓冲，shell_run() 拿到输入必须立刻拷进局部数组
 * 再切分，否则下一次输入会覆盖上一次的结果。命令返回 SHELL_LOGOUT 时 shell_run() 返回，
 * 由 kernel_main 重新调 login_screen()，其余情况继续循环。
 */

#define SHELL_LINE_MAX  128u
#define SHELL_ARGV_MAX  16u
#define SHELL_HOSTNAME  "zh_OS"
#define SHELL_CWD       "~"   /* 本步没有文件系统，路径统一显示 ~ */

#define SHELL_CONTINUE 0
#define SHELL_LOGOUT   1

/* 输出级别：颜色映射见 shell.c 的 level_color() */
enum sh_level {
    SH_ERR = 0,   /* 亮红 */
    SH_OK,        /* 亮绿 */
    SH_WARN,      /* 亮黄 */
    SH_INFO,      /* 绿 */
    SH_LOG,       /* 黄 */
    SH_PLAIN      /* 浅灰 */
};

typedef int (*shell_cmd_fn)(int argc, char **argv);

typedef struct {
    const char *name;
    shell_cmd_fn fn;
    const char *help;
} shell_command_t;

/* 命令表定义在 commands.c，shell.c 查表、help 命令遍历 */
extern const shell_command_t shell_command_table[];
extern const uint32_t shell_command_table_size;

/* 分段上色的彩色提示符：[user@zh_OS ~]$ 或 [user@zh_OS ~]# */
void shell_print_prompt(void);

/* [ADD] 提示符当前工作目录钩子：ramfs 初始化后由 kernel.c 注册，
 * 提示符据此显示真实路径（如 /home/zh 或 ~）而非固定的 SHELL_CWD。 */
typedef const char *(*shell_cwd_fn_t)(void);
void shell_set_cwd_fn(shell_cwd_fn_t fn);

/* 统一输出封装：不换行 / 换行 / 十进制数 / 纯换行 */
void sh_print(enum sh_level level, const char *s);
void sh_println(enum sh_level level, const char *s);
void sh_print_u32(enum sh_level level, uint32_t value);
void sh_newline(void);

/* 统一封装输出宏（Linux 红/黄/绿三色分级）：err 红 / ok 绿 / warn 黄 / info 绿 / log 黄 / plain 浅灰 */
#define err(s)   sh_println(SH_ERR, (s))
#define ok(s)    sh_println(SH_OK, (s))
#define warn(s)  sh_println(SH_WARN, (s))
#define info(s)  sh_println(SH_INFO, (s))
#define log(s)   sh_println(SH_LOG, (s))
#define plain(s) sh_println(SH_PLAIN, (s))

/* 进入 Shell 主循环；logout 时返回，由 kernel_main 重走登录流程 */
void shell_run(void);

#endif /* ZH_OS_SHELL_H */
