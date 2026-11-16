/* fat16.h - FAT16 只读挂载的接口与 BPB 字段偏移定义（LBA 0 引导扇区）。
 * 用到的偏移：0x0B 每扇区字节数（本实现要求 512）0x0D 每簇扇区数 0x0E 保留扇区数
 * 0x10 FAT 表个数 0x11 根目录项数 0x13 / 0x20 总扇区数 0x16 每个 FAT 的扇区数
 * 0x2B 卷标 0x36 文件系统类型。FAT16 的根目录是固定区域，BPB_RootClus 只读不用。 */

#ifndef ZH_OS_FS_FAT16_H
#define ZH_OS_FS_FAT16_H

#include <stdint.h>

/* 返回码：0 成功，负数为错误（freestanding 无 errno / libc） */
#define FAT16_OK             0
#define FAT16_ERR_NODEV      (-1)  /* 主通道从设备不存在 / 读引导扇区失败 */
#define FAT16_ERR_NOTFAT     (-2)  /* 不是 FAT16（文件系统类型串与簇数都不符） */
#define FAT16_ERR_BPB        (-3)  /* BPB 非法（每扇区字节数 / 每簇扇区数等不合规） */
#define FAT16_ERR_IO         (-4)  /* 读盘过程 I/O 错误 */
#define FAT16_ERR_NOENT      (-5)  /* 路径不存在 */
#define FAT16_ERR_NOTDIR     (-6)  /* 路径中段不是目录 */
#define FAT16_ERR_ISDIR      (-7)  /* 期望文件却遇到目录 */
#define FAT16_ERR_NOTMOUNTED (-8)  /* 尚未 fatmount */

/* 挂载主通道从设备上的 FAT16（读 LBA 0 解析并校验 BPB）。
 * 成功返回 FAT16_OK 并把几何信息缓存到静态变量；失败返回上面的负错误码。 */
int fat16_mount(void);

/* 列目录：path 为 "/"、"" 或子目录路径。目录亮蓝带 '/'，文件浅灰，每行 4 个。
 * 未挂载返回 FAT16_ERR_NOTMOUNTED，不是目录返回 FAT16_ERR_NOTDIR。 */
int fat16_list(const char *path);

/* 读文件内容：跟随簇链，最多读 maxlen 字节写入 buf，*outlen 为实际读取字节数。
 * 支持根目录下与一层子目录（实现上允许最多 4 段路径）。 */
int fat16_read(const char *path, uint8_t *buf, uint64_t maxlen, uint64_t *outlen);

/* 取文件大小（字节），供 fatcat 判断是否需要提示「截断」。 */
int fat16_size(const char *path, uint64_t *size);

/* 基于 FAT16 的 Shell 命令（命令表末尾追加注册，与 ramfs 的 ls/cat 隔离） */
int cmd_fatmount(int argc, char **argv);
int cmd_fatls(int argc, char **argv);
int cmd_fatcat(int argc, char **argv);

#endif /* ZH_OS_FS_FAT16_H */
