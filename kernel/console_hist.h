/* ============================================================================
 * zh_OS 1.0 (x86_64)
 * File:    kernel/console_hist.h
 * Version: 1.0
 * Brief:   带命令历史的行编辑器 read_line_with_history() 接口。
 * ============================================================================ */

#ifndef ZH_OS_KERNEL_CONSOLE_HIST_H
#define ZH_OS_KERNEL_CONSOLE_HIST_H

/*
 * 与 console.h 的 read_line() 行为一致（提示、逐字符回显、退格、回车），
 * 额外支持上下方向键浏览命令历史。原 read_line / read_password 原样保留，
 * 登录界面与密码输入仍走原函数，本函数只供 Shell 使用。
 */
char *read_line_with_history(const char *prompt);

#endif /* ZH_OS_KERNEL_CONSOLE_HIST_H */
