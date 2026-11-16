/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    fs/file.h
 * Version: 0.2beta
 * Brief:   M9 文件描述符表：每进程一张，open/read/close 的最小实现。
 *
 * Linux 里 fd 只是一个整数下标，真正的对象在进程的 files_struct 里：
 *   fd  --> struct file --> inode
 * 这里把"struct file"简化成一个 zfile_t：{ 用了没、指向哪个 ramfs 节点、读到哪了 }，
 * 每进程一张 ZH_MAX_FDS 项的表。于是 open 返回的是**下标**，这与 Linux 完全一致
 * （也是为什么 fd 总是 0、1、2 这种小整数，而不是指针）。
 * ============================================================================ */

#ifndef ZH_OS_FILE_H
#define ZH_OS_FILE_H

#include <stdint.h>

#define ZH_MAX_FDS 8   /* 每进程最多同时打开 8 个 */

typedef struct {
    uint32_t used;        /* 0 = 这个 fd 空闲 */
    void    *node;        /* 指向 ramfs_node_t（实际类型在 fs/file.c 里转） */
    uint64_t off;         /* 当前读偏移，read 后自动前移 */
} zfile_t;

/* 把整张表清空（进程创建 / 复位时用）。 */
void file_table_init(zfile_t *tab, int n);

/* fork 时继承父进程的 fd 表（Linux 里子进程共享同一批打开的文件）。 */
void file_table_clone(zfile_t *dst, const zfile_t *src, int n);

/* 按路径打开：成功返回 fd 下标（>=0），失败 -1（不存在 / 是目录 / 没权限 / 表满）。 */
int file_open(zfile_t *tab, int n, const char *path);

/* 从 fd 当前偏移读最多 maxlen 字节进内核缓冲，返回实际读到的字节数（0 = EOF），失败 -1。 */
int64_t file_read(zfile_t *tab, int n, int fd, uint8_t *buf, uint64_t maxlen);

/* 关闭 fd：成功 0，失败 -1（fd 非法或本来就空闲）。 */
int file_close(zfile_t *tab, int n, int fd);

/* [M10] creat：父目录可写才成功；新建或清空，返回写模式的 fd，失败 -1。 */
int file_creat(zfile_t *tab, int n, const char *path);

/* [M10] 向 fd 追加写：成功返回写入的字节数，失败 -1（fd 非法 / 只读 / 无权限）。 */
int64_t file_append(zfile_t *tab, int n, int fd, const uint8_t *buf, uint64_t len);

/* 文件大小（字节），fd 非法返回 0。 */
uint64_t file_size(zfile_t *tab, int n, int fd);

/* [M9] 验收用的计数器 */
uint32_t file_open_ok_count(void);
uint32_t file_open_fail_count(void);
uint32_t file_read_count(void);
uint32_t file_close_count(void);
uint32_t file_creat_count(void);    /* [M10] */
uint32_t file_write_count(void);    /* [M10] 成功 write 的次数 */
uint64_t file_bytes_written(void);  /* [M10] 累计写进文件的字节数 */

#endif /* ZH_OS_FILE_H */
