/* cmd_elf.h - elf 命令的声明 */

#ifndef ZH_OS_CMD_ELF_H
#define ZH_OS_CMD_ELF_H

/*
 * `elf`：把编译时嵌入内核的那个 hello.elf 交给 ELF 装载器，
 * 装进一个新进程的地址空间，用 e_entry 进 ring3 跑完，最后校验退出码 42。
 */
int cmd_elf(int argc, char **argv);

#endif /* ZH_OS_CMD_ELF_H */
