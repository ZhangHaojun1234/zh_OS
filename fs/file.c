/* ============================================================================
 * zh_OS 1.0 (x86_64)
 * File:    fs/file.c
 * Version: 1.0
 * Brief:   M9 文件描述符表实现：把"fd 整数"映射到 ramfs 的节点 + 读偏移。
 *
 * 关键点（对照 Linux）：
 *   - fd 是**下标**，不是指针。用户态只拿到 0、1、2…，拿不到内核地址，
 *     这正是 Linux 的设计：fd 是进程私有的索引，越界/伪造的 fd 一律 EINVAL。
 *   - 每个 fd 带自己的**读偏移**（对应 struct file 的 f_pos），read 之后前移，
 *     所以"读一段、再读一段"能连续读到文件的不同位置。
 *   - 进程退出 / close 时只需把这一项标成空闲 —— 这与 Linux 的
 *     "close(fd) 只是减 struct file 的引用计数"在最小模型里等价。
 * ============================================================================ */

#include "file.h"
#include "ramfs.h"

/* [ADD] M9 验收计数器（`fd` 命令据此判 PASS） */
static uint32_t g_open_ok;
static uint32_t g_open_fail;
static uint32_t g_read;
static uint32_t g_close;
/* [ADD] M10 */
static uint32_t g_creat;
static uint32_t g_write;
static uint64_t g_bytes_written;

void file_table_init(zfile_t *tab, int n)
{
    int i;

    if (tab == (zfile_t *)0) {
        return;
    }
    for (i = 0; i < n; ++i) {
        tab[i].used = 0u;
        tab[i].node = (void *)0;
        tab[i].off = 0u;
    }
}

void file_table_clone(zfile_t *dst, const zfile_t *src, int n)
{
    int i;

    if (dst == (zfile_t *)0 || src == (const zfile_t *)0) {
        return;
    }
    /* 只复制"表项"：父子各自持有一份偏移，与 Linux 里 dup 语义一致 */
    for (i = 0; i < n; ++i) {
        dst[i].used = src[i].used;
        dst[i].node = src[i].node;
        dst[i].off = src[i].off;
    }
}

static int fd_alloc(zfile_t *tab, int n)
{
    int i;

    for (i = 0; i < n; ++i) {
        if (tab[i].used == 0u) {
            tab[i].used = 1u;
            tab[i].node = (void *)0;
            tab[i].off = 0u;
            return i;
        }
    }
    return -1;   /* 表满，相当于 Linux 的 EMFILE */
}

int file_open(zfile_t *tab, int n, const char *path)
{
    ramfs_node_t *node = (ramfs_node_t *)0;
    int rc;
    int fd;

    if (tab == (zfile_t *)0 || path == (const char *)0) {
        return -1;
    }
    /* 权限、类型检查都在 ramfs 这边做，与 `cat` 命令的规则完全一致 */
    rc = ramfs_open_check(path, &node);
    if (rc != RAMFS_OK || node == (ramfs_node_t *)0) {
        ++g_open_fail;
        return -1;
    }
    fd = fd_alloc(tab, n);
    if (fd < 0) {
        return -1;
    }
    tab[fd].node = node;
    tab[fd].off = 0u;
    ++g_open_ok;
    return fd;
}

int64_t file_read(zfile_t *tab, int n, int fd, uint8_t *buf, uint64_t maxlen)
{
    uint64_t got = 0u;
    int rc;

    if (tab == (zfile_t *)0 || buf == (uint8_t *)0) {
        return -1;
    }
    if (fd < 0 || fd >= n || tab[fd].used == 0u || tab[fd].node == (void *)0) {
        return -1;   /* 越界或已关闭的 fd：EBADF */
    }
    rc = ramfs_node_read_at((ramfs_node_t *)tab[fd].node, tab[fd].off, buf, maxlen,
                            &got);
    if (rc != RAMFS_OK) {
        return -1;
    }
    tab[fd].off += got;
    ++g_read;
    return (int64_t)got;
}

int file_close(zfile_t *tab, int n, int fd)
{
    if (tab == (zfile_t *)0) {
        return -1;
    }
    if (fd < 0 || fd >= n || tab[fd].used == 0u) {
        return -1;
    }
    tab[fd].used = 0u;
    tab[fd].node = (void *)0;
    tab[fd].off = 0u;
    ++g_close;
    return 0;
}

/* [M10] creat：走 ramfs 的建文件/截断，拿到节点后分配一个 fd（写模式） */
int file_creat(zfile_t *tab, int n, const char *path)
{
    ramfs_node_t *node = (ramfs_node_t *)0;
    int rc;
    int fd;

    if (tab == (zfile_t *)0 || path == (const char *)0) {
        return -1;
    }
    rc = ramfs_create_file(path, &node);
    if (rc != RAMFS_OK || node == (ramfs_node_t *)0) {
        ++g_open_fail;
        return -1;
    }
    fd = fd_alloc(tab, n);
    if (fd < 0) {
        return -1;
    }
    tab[fd].node = node;
    tab[fd].off = 0u;
    ++g_creat;
    return fd;
}

/* [M10] 追加写：内容真的进了 ramfs 节点的 data 区（可被别的进程再 open 读出来） */
int64_t file_append(zfile_t *tab, int n, int fd, const uint8_t *buf, uint64_t len)
{
    int rc;

    if (tab == (zfile_t *)0 || buf == (const uint8_t *)0) {
        return -1;
    }
    if (fd < 0 || fd >= n || tab[fd].used == 0u || tab[fd].node == (void *)0) {
        return -1;
    }
    rc = ramfs_node_append((ramfs_node_t *)tab[fd].node, buf, len);
    if (rc != RAMFS_OK) {
        return -1;
    }
    tab[fd].off += len;
    ++g_write;
    g_bytes_written += len;
    return (int64_t)len;
}

uint32_t file_creat_count(void)
{
    return g_creat;
}

uint32_t file_write_count(void)
{
    return g_write;
}

uint64_t file_bytes_written(void)
{
    return g_bytes_written;
}

uint64_t file_size(zfile_t *tab, int n, int fd)
{
    if (tab == (zfile_t *)0 || fd < 0 || fd >= n || tab[fd].used == 0u ||
        tab[fd].node == (void *)0) {
        return 0u;
    }
    return ramfs_node_size((ramfs_node_t *)tab[fd].node);
}

uint32_t file_open_ok_count(void)
{
    return g_open_ok;
}

uint32_t file_open_fail_count(void)
{
    return g_open_fail;
}

uint32_t file_read_count(void)
{
    return g_read;
}

uint32_t file_close_count(void)
{
    return g_close;
}
