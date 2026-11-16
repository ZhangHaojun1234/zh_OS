/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    shell/cmd_vmm.h
 * Version: 0.2beta
 * Brief:   M5 进程地址空间：vmspace 命令声明。
 * ============================================================================ */

#ifndef ZH_OS_CMD_VMM_H
#define ZH_OS_CMD_VMM_H

/* `vmspace`：打印每个进程的页表物理地址、已映射的 4KB 页（VA -> PA）、CR3。 */
int cmd_vmspace(int argc, char **argv);

#endif /* ZH_OS_CMD_VMM_H */
