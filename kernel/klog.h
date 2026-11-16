/* ============================================================================
 * zh_OS 1.0 (x86_64)
 * File:    kernel/klog.h
 * Version: 1.0
 * Brief:   内核日志环接口：128 条环形缓冲、极简格式化、dmesg 命令入口。
 * ============================================================================ */

#ifndef ZH_OS_KERNEL_KLOG_H
#define ZH_OS_KERNEL_KLOG_H

#include <stdint.h>

/* 环形缓冲容量（条数）与单条上限（含结尾 '\0'） */
#define KLOG_SIZE     128
#define KLOG_LINE_MAX 160

/* 标记日志环就绪；不清空缓冲，见 klog.c 里的顺序说明。 */
void klog_init(void);

/*
 * 写一条日志到内存环，支持 %s %d %u %x %c %%，带 pit_get_ticks() 时间戳。
 * 只写内存，不调用 vga_print，不与屏幕输出耦合。
 */
void klog(const char *fmt, ...);

/*
 * 与 klog() 完全相同的实现（别名）。
 * 存在原因：kernel/kernel.c 里已经有一个 static klog(enum log_level, const char*)，
 * 该文件若声明本模块的 klog() 会造成同名冲突、字面 klog("...") 也会参数不匹配；
 * 为遵守「只添加，不修改」，kernel.c 通过这个别名把启动日志同步入环。
 */
void klog_ring(const char *fmt, ...);

/* 按时间顺序打印全部日志：格式 "[  tick] 内容"，tick 亮青、内容浅灰。 */
void klog_dump(void);

/* dmesg 命令：亮青标题 + klog_dump()；可选参数 dmesg test 用于灌满环做覆盖自测。 */
int cmd_dmesg(int argc, char **argv);

#endif /* ZH_OS_KERNEL_KLOG_H */
