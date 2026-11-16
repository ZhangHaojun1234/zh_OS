/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    mm/heap.h
 * Version: 0.2beta
 * Brief:   内核堆分配器接口（bump 分配 + 空闲链表回收）与 meminfo 命令入口。
 * ============================================================================ */

#ifndef ZH_OS_MM_HEAP_H
#define ZH_OS_MM_HEAP_H

#include <stdint.h>

/*
 * 本次为纯增量功能：本头文件与 mm/heap.c 都是新文件，
 * 已有模块只追加 include 与一行调用 / 一行命令注册，不改动任何已有行。
 */

/* 初始化 2MB 内核堆；幂等，重复调用会重置池状态。 */
void heap_init(void);

/* 8 字节对齐的 bump 分配；空间不足时打印亮红错误并返回 NULL，不 panic。 */
void *kmalloc(uint64_t size);

/* 释放并尝试合并相邻空闲块；越界 / 重复 free 打印亮红错误后直接返回。 */
void kfree(void *ptr);

/* kmalloc + 清零。 */
void *kzalloc(uint64_t size);

/* 打印已用 / 剩余 / 块数：亮青标题、浅灰数据、剩余 < 10% 亮黄警告。 */
void heap_stats(void);

/* meminfo 命令实现（放在 heap.c，由命令表末尾追加注册）。 */
int cmd_meminfo(int argc, char **argv);

#endif /* ZH_OS_MM_HEAP_H */
