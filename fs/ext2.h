/* ext2.h - M19 ext2 只读挂载的接口与盘上结构定义。
 * 块大小 bs = 1024 << s_log_block_size，取 1024 / 2048 / 4096。
 * 布局：LBA 0 引导块（ext2 不用）| offset 1024 超级块 | 块 2 起块组描述符表（每项 32 字节，
 * 含 bg_block_bitmap / bg_inode_bitmap / bg_inode_table，bs=1024 时块 2 落在 LBA 4）|
 * 每块组一张 inode 表（项长 s_inode_size）| 目录是一串 inode(4) rec_len(2) name_len(1) type(1) name |
 * 文件数据在数据块里，i_block[0..11] 直接块、[12] 一级间接、[13] 二级间接。 */

#ifndef ZH_OS_FS_EXT2_H
#define ZH_OS_FS_EXT2_H

#include <stdint.h>

/* 返回码：0 成功，负数为错误（freestanding 无 errno） */
#define EXT2_OK             0
#define EXT2_ERR_NODEV      (-1)  /* 主通道主设备不存在 / 读不到超级块 */
#define EXT2_ERR_NOTEXT2    (-2)  /* 魔数不是 0xEF53 */
#define EXT2_ERR_SB         (-3)  /* 超级块字段非法（块大小 / inode 大小等） */
#define EXT2_ERR_IO         (-4)  /* 读盘过程 I/O 错误 */
#define EXT2_ERR_NOENT      (-5)  /* 路径不存在 */
#define EXT2_ERR_NOTDIR     (-6)  /* 路径中段不是目录 */
#define EXT2_ERR_ISDIR      (-7)  /* 期望文件却遇到目录 */
#define EXT2_ERR_NOTMOUNTED (-8)  /* 尚未 ext2mount */
#define EXT2_ERR_CORRUPT    (-9)  /* inode / 目录项内容自相矛盾 */
#define EXT2_ERR_NOSPC      (-10) /* [ADD] M21：位图里没有空块 / 空 inode，或目录块满了 */
#define EXT2_ERR_EXIST      (-11) /* [ADD] M21：同名文件已经存在 */

/* 挂载主通道主设备上的 ext2：读 offset 1024 处的超级块并校验魔数 0xEF53，
 * 成功返回 EXT2_OK 并把几何信息（块大小、每组块数/inode 数、inode 大小）缓存。 */
int ext2_mount(void);

/* 是否已挂载（1/0）：给 ext2info 之类命令用 */
int ext2_mounted(void);

/* 列目录：path 为 "/" 或子目录路径；目录亮蓝带 '/'，文件浅灰，每行 4 个。 */
int ext2_list(const char *path);

/* 读文件：跟随直接块 + 一级间接块，最多 maxlen 字节写入 buf，*outlen 为实际字节数。 */
int ext2_read(const char *path, uint8_t *buf, uint64_t maxlen, uint64_t *outlen);

/* 取文件大小（字节），供 ext2cat 判断截断 */
int ext2_size(const char *path, uint64_t *size);

/* 挂载后缓存的几何信息（ext2info 打印用） */
typedef struct {
    uint32_t block_size;
    uint32_t blocks_count;
    uint32_t inodes_count;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t first_ino;
    uint32_t gdt_block;      /* 块组描述符表起始块号 */
    uint32_t inode_table;    /* 第 0 组 inode 表起始块号 */
    uint16_t magic;
    uint16_t rev_level;
} ext2_geom_t;

int ext2_geometry(ext2_geom_t *out);

/* Shell 命令（在 commands.c 末尾追加注册，与 ramfs/FAT16 隔离） */
int cmd_ext2mount(int argc, char **argv);
int cmd_ext2info(int argc, char **argv);
int cmd_ext2ls(int argc, char **argv);
int cmd_ext2cat(int argc, char **argv);

/* [ADD] M21：写路径                                                   */

/* 几何查询（写路径与 shell 层用；返回挂载时缓存的值） */
uint32_t ext2_block_size(void);
uint32_t ext2_blocks_count(void);
uint32_t ext2_inodes_count(void);
uint32_t ext2_blocks_per_group(void);
uint32_t ext2_inodes_per_group(void);
uint32_t ext2_first_data_block(void);
uint32_t ext2_inode_bytes(void);

/* 超级块里的空闲计数 */
uint32_t ext2_free_blocks(void);
uint32_t ext2_free_inodes(void);

/* 一个 ext2 块的整块读写（逐扇区，write-through） */
int ext2_read_block(uint32_t blk, uint8_t *buf);
int ext2_write_block(uint32_t blk, const uint8_t *buf);

/* inode 的整块读 / 写（raw 长度 = inode_size） */
int ext2_read_inode_raw(uint32_t ino, uint8_t *raw);
int ext2_write_inode_raw(uint32_t ino, const uint8_t *raw);

/* 建一个普通文件并写入内容（只支持 <= 12 个直接块，即 bs*12 字节）：
 * 分配 inode -> 分配数据块 -> 写 inode -> 父目录里插目录项。 */
int ext2_create(const char *path, const uint8_t *data, uint64_t len,
                uint32_t *out_ino);

/* [ADD] M21 的两个新命令 */
int cmd_ext2create(int argc, char **argv);
int cmd_ext2free(int argc, char **argv);


/* [ADD] M22：写大文件（12 个直接块 + 一级间接块，最大 block_size*(12+block_size/4)） */
int ext2_create_big(const char *path, const uint8_t *data, uint64_t len,
                    uint32_t *out_ino, uint32_t *out_blocks, uint32_t *out_indirect);

/* [ADD] M22 的命令 */
int cmd_ext2fill(int argc, char **argv);

/* [ADD] M28：二级/三级间接块 ， 单文件上限从 268KB 变成 ~16GB */
typedef void (*ext2_fill_fn)(uint64_t off, uint8_t *buf, uint32_t n, void *ctx);

/* 流式建大文件：每写一个数据块回调一次 gen(off, buf, n, ctx) 填内容，不要求调用方先把整份
 * 数据放进内存。out_ptrs 是这次新分配了几个指针块，out_i1/i2/i3 是三级指针树里各自首次用
 * 到的那块，没用到就是 0。 */
int ext2_create_stream(const char *path, uint64_t len, ext2_fill_fn gen, void *ctx,
                       uint32_t *out_ino, uint32_t *out_blocks,
                       uint32_t *out_ptrs, uint32_t *out_i1,
                       uint32_t *out_i2, uint32_t *out_i3);

/* [ADD] M28：按偏移读（能直接读大文件的尾巴，不用整份装内存） */
int ext2_read_at(const char *path, uint64_t off, uint8_t *buf, uint64_t maxlen,
                 uint64_t *outlen);

/* [ADD] M28 的命令 */
int cmd_ext2big(int argc, char **argv);
int cmd_ext2tail(int argc, char **argv);

/* [ADD] M23：删文件（释放数据块 -> 清 inode -> 摘目录项） */
int ext2_unlink(const char *path, uint32_t *out_ino, uint32_t *out_freed);

/* [ADD] M23 的命令 */
int cmd_ext2rm(int argc, char **argv);
#endif /* ZH_OS_FS_EXT2_H */
