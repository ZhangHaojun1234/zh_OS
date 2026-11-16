/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    fs/initrd.h
 * Version: 0.2beta
 * Brief:   M8 initrd：把编译时嵌入内核的 ELF 程序铺进 ramfs，让 exec 能按路径找。
 * ============================================================================ */

#ifndef ZH_OS_INITRD_H
#define ZH_OS_INITRD_H

/*
 * 在 ramfs_init() 之后调用一次：建 /bin，并把内核镜像里带的三个 ELF 写进去：
 *   /bin/hello.elf     M6 的那个 hello
 *   /bin/forkdemo.elf  M7 的 fork/wait 演示
 *   /bin/execdemo.elf  M8 的 exec 演示（它自己再 exec 到 /bin/hello.elf）
 *
 * 这一步的意义：到 M7 为止，"程序"还只是内核里的一个 blob，只能由 `elf` 命令
 * 直接喂给装载器。铺进 ramfs 之后，程序第一次变成**文件**，可以按路径打开 ——
 * 这正是 Linux 里 execve("/bin/sh") 的前提。
 */
void initrd_init(void);

#endif /* ZH_OS_INITRD_H */
