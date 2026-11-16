/* initrd.h - M8 initrd 的接口，让 exec 能按路径找到这些程序 */

#ifndef ZH_OS_INITRD_H
#define ZH_OS_INITRD_H

/*
 * ramfs_init() 之后调一次：建 /bin，把内核镜像里带的三个 ELF 写进去。
 *   /bin/hello.elf     M6 那个 hello
 *   /bin/forkdemo.elf  M7 的 fork/wait 演示
 *   /bin/execdemo.elf  M8 的 exec 演示，它自己再 exec 到 /bin/hello.elf
 * 到 M7 为止程序还只是内核里的一个 blob，只能由 elf 命令直接喂给装载器。铺进 ramfs 之后
 * 程序第一次变成文件，可以按路径打开，这也是 Linux 里 execve("/bin/sh") 的前提。
 */
void initrd_init(void);

#endif /* ZH_OS_INITRD_H */
