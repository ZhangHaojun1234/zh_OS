/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    kernel/elf.h
 * Version: 0.2beta
 * Brief:   M6 ELF64 可执行文件加载器：解析程序头、按 PT_LOAD 装入进程地址空间。
 * ============================================================================ */

#ifndef ZH_OS_ELF_H
#define ZH_OS_ELF_H

#include <stdint.h>

#include "vmm.h"

/*
 * M6 为什么要有这一层
 * -------------------
 * 到 M5 为止，"用户程序"其实还是内核自己拼出来的字节：task.c 里的 emitter 一条条
 * 写 BB/BF/B8/CD 80。这跟 Linux 的区别不在于指令对不对，而在于**程序不是外部
 * 产物**——没有编译、没有链接、没有文件格式、没有装载过程。
 *
 * M6 把这一块补上：用户程序用 gcc 编成标准 ELF64（ET_EXEC），内核只做三件事：
 *   1) 校验 ELF 头（magic / class / machine / type / 程序头表布局）；
 *   2) 按 PT_LOAD 逐段装入进程自己的地址空间（4KB 页，按需分配帧）；
 *   3) 用 e_entry 当 rip 进 ring3。
 *
 * 支持范围（够用就行的最小集）：ET_EXEC + EM_X86_64 + PT_LOAD，无重定位、
 * 无动态链接、无解释器（PT_INTERP 一律不支持）。这正是 Linux 0.01 时代的水平。
 */

#define ELF_MAX_PHDRS   16u

/* 一次装载的结果，用来打印和验收。 */
typedef struct {
    uint64_t entry;       /* e_entry */
    uint32_t segments;    /* 装了几个 PT_LOAD */
    uint32_t pages;       /* 一共映射了多少个 4KB 页 */
    uint32_t file_bytes;  /* p_filesz 之和 */
    uint32_t mem_bytes;   /* p_memsz 之和 */
    uint32_t has_bss;     /* 有 p_memsz > p_filesz 的段（即 .bss） */
    uint32_t demand_pages;/* [ADD] M16：纯 .bss 页里有多少页登记成"按需"（装载时没分配） */
} elf_image_t;

/*
 * 把 blob 里的 ELF 装进 space：逐段分配 4KB 帧、映射、拷贝，并把 p_memsz 超出
 * p_filesz 的部分（.bss）清零。返回 0 成功；失败返回 -1，原因见 elf_last_error()。
 */
int elf_load_into(vmm_space_t *space, const uint8_t *blob, uint64_t size,
                  elf_image_t *img);

/* 最近一次失败的原因（人类可读的一行）。 */
const char *elf_last_error(void);

/*
 * 只解析不装载：把 ELF 头和每个 PT_LOAD 的关键字段打到屏幕上。
 * `elf` 命令用它证明"内核真的在读这个文件格式"。
 */
void elf_report(const uint8_t *blob, uint64_t size);

#endif /* ZH_OS_ELF_H */
