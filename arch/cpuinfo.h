/* cpuinfo.h - CPUID 封装和 cpuinfo 命令的声明。本文件只声明接口，cpuid() 的内联汇编和
 * cpuinfo_print() 的实现在 cpuinfo.c（freestanding 没有 libc，格式化都自己写）。
 * cmd_cpuinfo 签名跟命令表其它命令一样，返回 SHELL_CONTINUE。 */

#ifndef ZH_OS_CPUINFO_H
#define ZH_OS_CPUINFO_H

#include <stdint.h>

/* Shell 命令：打印 CPU 厂商、型号、特性等系统信息。无参数。 */
int cmd_cpuinfo(int argc, char **argv);

/* 真正的打印逻辑（被 cmd_cpuinfo 调用，也可被内核其它地方按需调用）。 */
void cpuinfo_print(void);

#endif /* ZH_OS_CPUINFO_H */
