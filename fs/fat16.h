/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    fs/fat16.h
 * Version: 0.2beta
 * Brief:   FAT16 只读挂载接口（读主通道从设备，8.3 短名，根目录 + 一层子目录）。
 *
 * B 组第 3 个功能，纯增量「只添加，不修改」：本文件与 fat16.c 都是新文件，
 * 只读驱动复用 drivers/ata 的 PIO 接口。FAT16 用独立的 fatls / fatcat 命令，
 * 不覆盖 ramfs 的 ls / cat，两个文件系统互不干扰。
 *
 * ============================ BPB 字段偏移表（LBA 0 引导扇区）================
 *  偏移   长度  字段名            说明
 *  0x00   3     BS_jmpBoot        跳转指令（EB xx 90）
 *  0x03   8     BS_OEMName        OEM 名
 *  0x0B   2     BPB_BytsPerSec    每扇区字节数（本实现要求 512）
 *  0x0D   1     BPB_SecPerClus    每簇扇区数（2 的幂）
 *  0x0E   2     BPB_RsvdSecCnt    保留扇区数（FAT 表之前的扇区）
 *  0x10   1     BPB_NumFATs       FAT 表个数（通常 2）
 *  0x11   2     BPB_RootEntCnt    根目录项数（FAT16 根目录是固定区域）
 *  0x13   2     BPB_TotSec16      总扇区数（<64K 时有效，为 0 用 0x20）
 *  0x15   1     BPB_Media         介质描述符（0xF8/0xF0）
 *  0x16   2     BPB_FATSz16       每个 FAT 表的扇区数（FAT16 用这个）
 *  0x18   2     BPB_SecPerTrk     每磁道扇区数
 *  0x1A   2     BPB_NumHeads      磁头数
 *  0x1C   4     BPB_HiddSec       分区前/隐藏扇区数（本镜像为 0）
 *  0x20   4     BPB_TotSec32      总扇区数（>=64K 时有效）
 *  0x24   1     BS_DrvNum         驱动器号（FAT16 扩展区）
 *  0x25   1     BS_Reserved1      保留
 *  0x26   1     BS_BootSig        引导签名（0x29）
 *  0x27   4     BS_VolID          卷序列号
 *  0x2B   11    BS_VolLab         卷标
 *  0x2C   4     BPB_RootClus      根目录起始簇（仅 FAT32 有意义；FAT16 根目录是
 *                                 固定区域，本实现读取该字段但不用于定位根目录）
 *  0x36   8     BS_FilSysType     文件系统类型（"FAT16   "）
 * ============================================================================ */

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

/* ------------------------------------------------------------------ */
/* 基于 FAT16 的 Shell 命令（命令表末尾追加注册，与 ramfs 的 ls/cat 隔离） */
/* ------------------------------------------------------------------ */
int cmd_fatmount(int argc, char **argv);
int cmd_fatls(int argc, char **argv);
int cmd_fatcat(int argc, char **argv);

#endif /* ZH_OS_FS_FAT16_H */
