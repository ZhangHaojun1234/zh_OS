/* ---------------------------------------------------------------
 * ATA PIO 磁盘驱动 (主 IDE 通道, 28 位 LBA)
 *
 * 账户数据库需要掉电保持，因此必须能真正读写磁盘。
 * 这里只实现最基础的单扇区 PIO 读写，够用且易于理解。
 * --------------------------------------------------------------- */
#ifndef ZH_ATA_H
#define ZH_ATA_H

#include "types.h"

#define ATA_SECTOR_SIZE 512

/* 返回 0 表示检测到可用的主盘 */
int ata_init(void);
int ata_present(void);

/* 诊断信息: 初始化失败的原因、最后状态与错误寄存器 */
const char *ata_init_reason(void);
u8 ata_last_status(void);
u8 ata_last_error(void);

/* 读/写一个扇区; 返回 0 成功, -1 失败 */
int ata_read_sector(u32 lba, void *buffer);
int ata_write_sector(u32 lba, const void *buffer);

/* 连续读写 count 个扇区 */
int ata_read(u32 lba, void *buffer, u32 count);
int ata_write(u32 lba, const void *buffer, u32 count);

/* 把缓存刷到盘上 */
int ata_flush(void);

u32 ata_sector_count(void);

#endif
