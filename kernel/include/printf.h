/* ---------------------------------------------------------------
 * 简单格式化输出
 * --------------------------------------------------------------- */
#ifndef ZH_PRINTF_H
#define ZH_PRINTF_H

#include "types.h"

void kprintf(const char *fmt, ...);
void kpanic(const char *fmt, ...) NORETURN;

#endif
