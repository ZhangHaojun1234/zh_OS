/* ============================================================================
 * zh_OS 0.1beta (x86_64)
 * File:    arch/cpuinfo.h
 * Version: 0.1beta
 * Brief:   CPUID 指令封装与 cpuinfo 命令声明。
 *
 * 设计要点：
 *   - 本文件只声明对外接口；cpuid() 内联汇编与 cpuinfo_print() 的实现在
 *     cpuinfo.c 中（freestanding 环境没有 libc，所有格式化都自己实现）。
 *   - cmd_cpuinfo 是 Shell 命令，签名与命令表其它命令一致，
 *     返回 SHELL_CONTINUE（继续 Shell 循环）。
 * ============================================================================ */

#ifndef ZH_OS_CPUINFO_H
#define ZH_OS_CPUINFO_H

#include <stdint.h>

/* Shell 命令：打印 CPU 厂商、型号、特性等系统信息。无参数。 */
int cmd_cpuinfo(int argc, char **argv);

/* 真正的打印逻辑（被 cmd_cpuinfo 调用，也可被内核其它地方按需调用）。 */
void cpuinfo_print(void);

#endif /* ZH_OS_CPUINFO_H */
