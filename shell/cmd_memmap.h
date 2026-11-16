/* cmd_memmap.h - memmap 命令的声明 */

#ifndef ZH_OS_CMD_MEMMAP_H
#define ZH_OS_CMD_MEMMAP_H

/* memmap：打印 Multiboot2 内存图与页帧分配器状态；memmap test 跑自测。 */
int cmd_memmap(int argc, char **argv);

#endif /* ZH_OS_CMD_MEMMAP_H */
