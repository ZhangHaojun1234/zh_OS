#include "ata.h"
#include "io.h"
#include "string.h"

#define ATA_PRIMARY_IO      0x1F0
#define ATA_PRIMARY_CTRL    0x3F6

#define ATA_REG_DATA        0
#define ATA_REG_ERROR       1
#define ATA_REG_FEATURES    1
#define ATA_REG_SECCOUNT0   2
#define ATA_REG_LBA0        3
#define ATA_REG_LBA1        4
#define ATA_REG_LBA2        5
#define ATA_REG_DRIVE       6
#define ATA_REG_STATUS      7
#define ATA_REG_COMMAND     7

#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_FLUSH       0xE7
#define ATA_CMD_IDENTIFY    0xEC

#define ATA_SR_BSY          0x80
#define ATA_SR_DRDY         0x40
#define ATA_SR_DF           0x20
#define ATA_SR_DRQ          0x08
#define ATA_SR_ERR          0x01

/* 设备控制寄存器 (0x3F6) 的位 */
#define ATA_CTRL_NIEN       0x02    /* 1 = 禁用中断 */
#define ATA_CTRL_SRST       0x04    /* 1 = 软件复位 */

#define ATA_TIMEOUT         1000000u

static int drive_present;
static u32 total_sectors;
static u8 last_status;
static u8 last_error;
static const char *init_reason = "not probed";

static u8 ata_status(void)
{
    return inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
}

static int ata_wait_not_busy(void)
{
    for (u32 i = 0; i < ATA_TIMEOUT; i++) {
        if (!(ata_status() & ATA_SR_BSY)) {
            return 0;
        }
    }
    return -1;
}

static int ata_wait_drq(void)
{
    for (u32 i = 0; i < ATA_TIMEOUT; i++) {
        u8 status = ata_status();
        if (status & ATA_SR_ERR) {
            return -1;
        }
        if (status & ATA_SR_DF) {
            return -1;
        }
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) {
            return 0;
        }
    }
    return -1;
}

static void ata_delay_400ns(void)
{
    for (int i = 0; i < 4; i++) {
        inb(ATA_PRIMARY_CTRL);
    }
}

int ata_init(void)
{
    drive_present = 0;
    total_sectors = 0;
    last_status = 0;
    last_error = 0;

    /* 禁用中断。注意 0x04 是 SRST (软件复位), 不能用在这里 */
    outb(ATA_PRIMARY_CTRL, ATA_CTRL_NIEN);
    ata_delay_400ns();

    /* 选择主通道主盘 */
    outb(ATA_PRIMARY_IO + ATA_REG_DRIVE, 0xA0);
    ata_delay_400ns();

    if (ata_wait_not_busy() != 0) {
        init_reason = "controller busy";
        return -1;
    }

    /* 状态为 0 表示该通道没有设备 */
    if (ata_status() == 0) {
        init_reason = "no device";
        return -1;
    }
    outb(ATA_PRIMARY_IO + ATA_REG_SECCOUNT0, 0);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA0, 0);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA1, 0);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA2, 0);
    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay_400ns();

    /* IDENTIFY 之后状态回到 0 也说明没有设备 */
    if (ata_status() == 0) {
        init_reason = "no device after IDENTIFY";
        return -1;
    }
    if (ata_wait_not_busy() != 0) {
        last_status = ata_status();
        last_error = inb(ATA_PRIMARY_IO + ATA_REG_ERROR);
        init_reason = "timeout waiting BSY to clear";
        return -1;
    }

    /* LBA 中段/高位非 0 说明是 ATAPI (光驱), 这里只支持 ATA 硬盘 */
    u8 mid = inb(ATA_PRIMARY_IO + ATA_REG_LBA1);
    u8 high = inb(ATA_PRIMARY_IO + ATA_REG_LBA2);
    if (mid != 0 || high != 0) {
        init_reason = "ATAPI device, not supported";
        return -1;
    }

    if (ata_wait_drq() != 0) {
        last_status = ata_status();
        last_error = inb(ATA_PRIMARY_IO + ATA_REG_ERROR);
        init_reason = "IDENTIFY failed";
        return -1;
    }

    u16 identify[256];
    for (int i = 0; i < 256; i++) {
        u16 value = inw(ATA_PRIMARY_IO + ATA_REG_DATA);
        identify[i] = (u16)((value >> 8) | (value << 8));
    }

    /* 偏移 60/61 是 28 位可寻址扇区总数 (低/高字) */
    total_sectors = ((u32)identify[61] << 16) | identify[60];
    if (total_sectors == 0) {
        total_sectors = 8192;
    }

    drive_present = 1;
    init_reason = "ok";
    return 0;
}

const char *ata_init_reason(void)
{
    return init_reason;
}

u8 ata_last_status(void)
{
    return last_status;
}

u8 ata_last_error(void)
{
    return last_error;
}

int ata_present(void)
{
    return drive_present;
}

u32 ata_sector_count(void)
{
    return total_sectors;
}

int ata_read_sector(u32 lba, void *buffer)
{
    u16 *buf = (u16 *)buffer;

    if (!drive_present) {
        return -1;
    }
    if (ata_wait_not_busy() != 0) {
        return -1;
    }

    outb(ATA_PRIMARY_IO + ATA_REG_DRIVE, (u8)(0xE0 | ((lba >> 24) & 0x0F)));
    outb(ATA_PRIMARY_IO + ATA_REG_SECCOUNT0, 1);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA0, (u8)(lba & 0xFF));
    outb(ATA_PRIMARY_IO + ATA_REG_LBA1, (u8)((lba >> 8) & 0xFF));
    outb(ATA_PRIMARY_IO + ATA_REG_LBA2, (u8)((lba >> 16) & 0xFF));
    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    ata_delay_400ns();

    if (ata_wait_drq() != 0) {
        return -1;
    }

    for (int i = 0; i < 256; i++) {
        buf[i] = inw(ATA_PRIMARY_IO + ATA_REG_DATA);
    }
    ata_delay_400ns();
    return 0;
}

int ata_write_sector(u32 lba, const void *buffer)
{
    const u16 *buf = (const u16 *)buffer;

    if (!drive_present) {
        return -1;
    }
    if (ata_wait_not_busy() != 0) {
        return -1;
    }

    outb(ATA_PRIMARY_IO + ATA_REG_DRIVE, (u8)(0xE0 | ((lba >> 24) & 0x0F)));
    outb(ATA_PRIMARY_IO + ATA_REG_SECCOUNT0, 1);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA0, (u8)(lba & 0xFF));
    outb(ATA_PRIMARY_IO + ATA_REG_LBA1, (u8)((lba >> 8) & 0xFF));
    outb(ATA_PRIMARY_IO + ATA_REG_LBA2, (u8)((lba >> 16) & 0xFF));
    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    ata_delay_400ns();

    if (ata_wait_drq() != 0) {
        return -1;
    }

    for (int i = 0; i < 256; i++) {
        outw(ATA_PRIMARY_IO + ATA_REG_DATA, buf[i]);
    }
    ata_delay_400ns();
    (void)ata_flush();
    return 0;
}

int ata_read(u32 lba, void *buffer, u32 count)
{
    u8 *p = (u8 *)buffer;

    for (u32 i = 0; i < count; i++) {
        if (ata_read_sector(lba + i, p + (u32)i * ATA_SECTOR_SIZE) != 0) {
            return -1;
        }
    }
    return 0;
}

int ata_write(u32 lba, const void *buffer, u32 count)
{
    const u8 *p = (const u8 *)buffer;

    for (u32 i = 0; i < count; i++) {
        if (ata_write_sector(lba + i, p + (u32)i * ATA_SECTOR_SIZE) != 0) {
            return -1;
        }
    }
    return 0;
}

int ata_flush(void)
{
    if (!drive_present) {
        return -1;
    }
    if (ata_wait_not_busy() != 0) {
        return -1;
    }
    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_FLUSH);
    ata_delay_400ns();
    return ata_wait_not_busy();
}
