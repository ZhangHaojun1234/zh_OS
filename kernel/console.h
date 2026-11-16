/* ============================================================================
 * zh_OS 1.0 (x86_64)
 * File:    kernel/console.h
 * Version: 1.0
 * Brief:   行输入接口声明。
 * ============================================================================ */

#ifndef ZH_OS_CONSOLE_H
#define ZH_OS_CONSOLE_H

/*
 * 行输入：从键盘读取一行，带提示、逐字符回显与退格删除。
 *
 * 返回的指针指向函数内部的静态缓冲区，下一次调用会被覆盖。
 * 缓冲区容量 128 字节（含结尾 '\0'），超出部分忽略。
 */
char *read_line(const char *prompt);

/*
 * 密码输入：与 read_line 相同，但每次按键回显 '*' 而不显示真实字符。
 */
char *read_password(const char *prompt);

#endif /* ZH_OS_CONSOLE_H */
