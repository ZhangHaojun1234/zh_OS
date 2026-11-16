/* elf.h - ELF64 装载的对外结构：程序头解析 + 装进进程地址空间 */

#ifndef ZH_OS_ELF_H
#define ZH_OS_ELF_H

#include <stdint.h>

#include "vmm.h"

/*
 * M6 补的这一层。到 M5 为止，"用户程序"还是内核自己拼出来的字节：task.c 里的 emitter
 * 一条条写 BB、BF、B8、CD 80。跟 Linux 的差别不在指令对不对，而在于程序不是外部产物，
 * 没有编译、没有链接、没有文件格式，也没有装载过程。
 * M6 让用户程序用 gcc 编成标准 ELF64（ET_EXEC），内核只管三件事：校验 ELF 头（magic、
 * class、machine、type、程序头表布局）；按 PT_LOAD 逐段装入进程自己的地址空间（4KB 页，
 * 按需分配帧）；用 e_entry 当 rip 进 ring3。
 * 支持范围是最小集：ET_EXEC + EM_X86_64 + PT_LOAD，无重定位、无动态链接、无解释器，
 * PT_INTERP 一律不支持。差不多是 Linux 0.01 的水平。
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
 * 把 blob 里的 ELF 装进 space：逐段分配 4KB 帧、映射、拷贝，p_memsz 超出 p_filesz 的那部分
 * （.bss）清零。成功 0，失败 -1，原因见 elf_last_error()。
 */
int elf_load_into(vmm_space_t *space, const uint8_t *blob, uint64_t size,
                  elf_image_t *img);

/* 最近一次失败的原因（人类可读的一行）。 */
const char *elf_last_error(void);

/*
 * 只解析不装载：把 ELF 头和每个 PT_LOAD 的关键字段打到屏幕上。elf 命令用它证明内核
 * 真的在读这个文件格式。
 */
void elf_report(const uint8_t *blob, uint64_t size);

#endif /* ZH_OS_ELF_H */
