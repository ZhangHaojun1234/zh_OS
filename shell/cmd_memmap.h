/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    shell/cmd_memmap.h
 * Version: 0.2beta
 * Brief:   M3 物理内存：memmap 命令声明。
 * ============================================================================ */

#ifndef ZH_OS_CMD_MEMMAP_H
#define ZH_OS_CMD_MEMMAP_H

/* memmap：打印 Multiboot2 内存图与页帧分配器状态；memmap test 跑自测。 */
int cmd_memmap(int argc, char **argv);

#endif /* ZH_OS_CMD_MEMMAP_H */
