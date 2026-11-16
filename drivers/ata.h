/* ata.h - ATA PIO 的接口声明（LBA28 / 主通道 0x1F0），只声明驱动和 diskinfo 命令 */

#ifndef ZH_OS_ATA_H
#define ZH_OS_ATA_H

#include <stdint.h>

/* 返回码：成功 0，负数为错误（freestanding 无 errno / errno.h） */
#define ATA_OK      0
#define ATA_ENODEV  (-1)  /* 无磁盘 / 设备不存在（含 IDENTIFY 超时） */
#define ATA_EIO     (-2)  /* I/O 错误（读写出错、忙等超时） */
#define ATA_EROFS   (-3)  /* 介质只读（CD-ROM 等），写被拒绝 */

/* 主通道（primary）IDE 端口；0x3F6 是设备控制 / 备用状态寄存器。 */
#define ATA_PRIMARY_DATA    0x1F0u
#define ATA_PRIMARY_SECTORS 0x1F2u
#define ATA_PRIMARY_LBALO   0x1F3u
#define ATA_PRIMARY_LBAMID  0x1F4u
#define ATA_PRIMARY_LBAHI   0x1F5u
#define ATA_PRIMARY_DRIVE   0x1F6u  /* 驱动器 / 磁头：bit7=1(LBA) bit6=1, bit4=0(主) */
#define ATA_PRIMARY_CMD     0x1F7u  /* 写：命令寄存器；读：状态寄存器 */
#define ATA_PRIMARY_CTRL    0x3F6u  /* 设备控制 / 备用状态（读它可做 400ns 延时） */

/* 命令字节 */
#define ATA_CMD_IDENTIFY  0xECu
#define ATA_CMD_READ      0x20u
#define ATA_CMD_WRITE     0x30u

/* 状态寄存器位 */
#define ATA_SR_BSY  0x80u  /* Busy */
#define ATA_SR_DRDY 0x40u  /* Device Ready */
#define ATA_SR_DRQ  0x08u  /* Data Request（可传输） */
#define ATA_SR_ERR  0x01u  /* Error */

/* 初始化主通道：检测是否有磁盘（发 IDENTIFY 0xEC），超时/出错返回 ATA_ENODEV，
 * 成功返回 ATA_OK 并把"磁盘是否存在的标记"缓存在本模块静态变量里。 */
int ata_init(void);

/* LBA28 读一个扇区（512 字节）到 buf；失败返回负错误码，不 panic。 */
int ata_read_sector(uint32_t lba, uint8_t *buf);

/* LBA28 写一个扇区（512 字节）到 buf；失败返回负错误码（只读介质返回 ATA_EROFS）。 */
int ata_write_sector(uint32_t lba, const uint8_t *buf);

/* 读取 ata_init 缓存的"磁盘是否存在"标记：1=有磁盘，0=无。 */
int ata_disk_present(void);

/* diskinfo 命令（实现见 shell/cmd_disk.c）；声明在此便于 commands.c 引用，
 * 与 cmd_cpuinfo 放在各自驱动头文件里的惯例一致。 */
int cmd_diskinfo(int argc, char **argv);

/* [ADD] B 组第 3 个功能：读主通道「从设备」（slave）的一个扇区。
 * 对应 QEMU 的 -drive ...,if=ide,index=1（主通道第二块盘）。
 * 从设备不存在返回 ATA_ENODEV，I/O 出错返回 ATA_EIO，成功返回 ATA_OK。
 * 纯追加声明，不改动上面的 ata_init / ata_read_sector / ata_write_sector。 */
int ata_read_sector_slave(uint32_t lba, uint8_t *buf);

#endif /* ZH_OS_ATA_H */
