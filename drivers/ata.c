/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    drivers/ata.c
 * Version: 0.2beta
 * Brief:   ATA PIO 驱动（LBA28 / 主通道 0x1F0），最简同步实现。
 *
 * B 组第 2 个功能（ATA PIO + 用户表持久化）的纯增量新增文件。
 * 设计要点：
 *   - 只支持主通道（0x1F0）主设备（master），足够一个教学内核挂一块磁盘。
 *   - 纯 PIO、忙等（polling），不挂中断；所有失败返回负错误码，绝不 panic。
 *   - 每个端口操作前做 ~400ns 延时：读一次 0x3F6（备用状态）约 1us，读 4 次
 *     足以满足 ATA 规范对写命令后/读数据前的插空要求。
 *   - 从 ISO/CD 启动时，主通道往往没有真正的硬盘（CD 是 secondary 的 ATAPI），
 *     因此 ata_init 检测不到磁盘，ata_read/write 直接返回 ATA_ENODEV，
 *     上层据此走"内存态默认账户"分支。
 * ============================================================================ */

#include "ata.h"

/* 真正的不带延时的原始 inb（供延时函数自身使用，避免递归）。 */
static inline uint8_t ata_inb_raw(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* 400ns 延时：连续读 4 次备用状态寄存器（0x3F6）。每次 inb 在真实硬件上
 * 耗时远大于 100ns，4 次合计稳定超过 400ns，且不依赖 PIT/时钟。 */
static void ata_delay_400ns(void)
{
    (void)ata_inb_raw(ATA_PRIMARY_CTRL);
    (void)ata_inb_raw(ATA_PRIMARY_CTRL);
    (void)ata_inb_raw(ATA_PRIMARY_CTRL);
    (void)ata_inb_raw(ATA_PRIMARY_CTRL);
}

/* 端口 IO 全部为模块内 static，避免与其它 .c 里的同名 inline 冲突。
 * 每次读写前先 ata_delay_400ns() 满足 ATA 对端口操作间隔的要求。 */
static inline void ata_outb(uint16_t port, uint8_t value)
{
    ata_delay_400ns();
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t ata_inb(uint16_t port)
{
    ata_delay_400ns();
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint16_t ata_inw(uint16_t port)
{
    ata_delay_400ns();
    uint16_t value;
    __asm__ volatile("inw %w1, %0" : "=a"(value) : "d"(port));
    return value;
}

/* 模块内缓存：ata_init 检测到的"磁盘是否存在"。 */
static int g_ata_present = 0;

/* 等待设备就绪（BSY 清、DRDY 置），超时返回 ATA_EIO。 */
static int ata_wait_ready(void)
{
    for (uint32_t i = 0; i < 1000000u; ++i) {
        const uint8_t st = ata_inb(ATA_PRIMARY_CMD);
        if ((st & ATA_SR_BSY) == 0u && (st & ATA_SR_DRDY) != 0u) {
            return ATA_OK;
        }
        if (st & ATA_SR_ERR) {
            return ATA_EIO;
        }
    }
    return ATA_EIO;  /* 超时 */
}

/* 等待数据就绪（BSY 清、DRQ 置）；可同时检测 ERR。err 返回时 *err=1。 */
static int ata_wait_drq(int *err)
{
    for (uint32_t i = 0; i < 1000000u; ++i) {
        const uint8_t st = ata_inb(ATA_PRIMARY_CMD);
        if (st & ATA_SR_ERR) {
            if (err != 0) {
                *err = 1;
            }
            return ATA_EIO;
        }
        if ((st & ATA_SR_BSY) == 0u && (st & ATA_SR_DRQ) != 0u) {
            return ATA_OK;
        }
    }
    return ATA_EIO;  /* 超时 */
}

int ata_init(void)
{
    g_ata_present = 0;

    /* 选择主设备（master，LBA 模式）：0xE0 = 1110 0000b
     *   bit7=1 恒 1；bit6=1 LBA；bit5=1 恒 1；bit4=0 主设备；bit3..0 = LBA 高 4 位(0)。 */
    ata_outb(ATA_PRIMARY_DRIVE, 0xE0u);

    /* 软件复位握手：先对控制寄存器发 SRST 再清除，确保设备处于已知状态。 */
    ata_outb(ATA_PRIMARY_CTRL, 0x02u);   /* SRST = 1 */
    ata_outb(ATA_PRIMARY_CTRL, 0x00u);   /* SRST = 0 */
    if (ata_wait_ready() != ATA_OK) {
        return ATA_ENODEV;
    }

    /* 发 IDENTIFY 命令（0xEC），看设备是否回送 512 字节识别数据。 */
    ata_outb(ATA_PRIMARY_CMD, ATA_CMD_IDENTIFY);

    int err = 0;
    /* 等待 DRQ（识别数据就绪）或 ERR（不是普通硬盘，例如 ATAPI/CD 会 abort）。 */
    if (ata_wait_drq(&err) != ATA_OK) {
        /* 超时或 ERR：主通道上没有普通 ATA 硬盘。 */
        return ATA_ENODEV;
    }

    /* 识别数据到达：读掉 256 个 16 位字（不关心内容），随后设备回到就绪。 */
    for (uint32_t i = 0; i < 256u; ++i) {
        (void)ata_inw(ATA_PRIMARY_DATA);
    }

    g_ata_present = 1;
    return ATA_OK;
}

int ata_disk_present(void)
{
    return g_ata_present;
}

int ata_read_sector(uint32_t lba, uint8_t *buf)
{
    if (buf == 0) {
        return ATA_EIO;
    }
    if (!g_ata_present) {
        return ATA_ENODEV;
    }

    /* 选择主设备 + LBA 高 4 位（0），并等待就绪。 */
    ata_outb(ATA_PRIMARY_DRIVE, 0xE0u | (uint8_t)((lba >> 24u) & 0x0Fu));
    if (ata_wait_ready() != ATA_OK) {
        return ATA_EIO;
    }

    /* 写 LBA 与扇区数（LBA28）。 */
    ata_outb(ATA_PRIMARY_SECTORS, 0x01u);          /* 读 1 个扇区 */
    ata_outb(ATA_PRIMARY_LBALO, (uint8_t)(lba & 0xFFu));
    ata_outb(ATA_PRIMARY_LBAMID, (uint8_t)((lba >> 8u) & 0xFFu));
    ata_outb(ATA_PRIMARY_LBAHI, (uint8_t)((lba >> 16u) & 0xFFu));

    /* 发读命令（0x20）。 */
    ata_outb(ATA_PRIMARY_CMD, ATA_CMD_READ);

    int err = 0;
    if (ata_wait_drq(&err) != ATA_OK) {
        return ATA_EIO;
    }

    /* DRQ 已置：连续读 256 个 16 位字 = 512 字节。 */
    for (uint32_t i = 0; i < 256u; ++i) {
        const uint16_t w = ata_inw(ATA_PRIMARY_DATA);
        buf[i * 2u] = (uint8_t)(w & 0xFFu);
        buf[i * 2u + 1u] = (uint8_t)((w >> 8u) & 0xFFu);
    }

    /* 等待命令完成（BSY 清），并确认未出错。 */
    for (uint32_t i = 0; i < 1000000u; ++i) {
        const uint8_t st = ata_inb(ATA_PRIMARY_CMD);
        if ((st & ATA_SR_BSY) == 0u) {
            return (st & ATA_SR_ERR) ? ATA_EIO : ATA_OK;
        }
    }
    return ATA_EIO;
}

int ata_write_sector(uint32_t lba, const uint8_t *buf)
{
    if (buf == 0) {
        return ATA_EIO;
    }
    if (!g_ata_present) {
        return ATA_ENODEV;
    }

    ata_outb(ATA_PRIMARY_DRIVE, 0xE0u | (uint8_t)((lba >> 24u) & 0x0Fu));
    if (ata_wait_ready() != ATA_OK) {
        return ATA_EIO;
    }

    ata_outb(ATA_PRIMARY_SECTORS, 0x01u);
    ata_outb(ATA_PRIMARY_LBALO, (uint8_t)(lba & 0xFFu));
    ata_outb(ATA_PRIMARY_LBAMID, (uint8_t)((lba >> 8u) & 0xFFu));
    ata_outb(ATA_PRIMARY_LBAHI, (uint8_t)((lba >> 16u) & 0xFFu));

    ata_outb(ATA_PRIMARY_CMD, ATA_CMD_WRITE);

    int err = 0;
    if (ata_wait_drq(&err) != ATA_OK) {
        /* 写命令被拒绝（典型是只读介质/CD）：映射为 ATA_EROFS 便于上层区分。 */
        return ATA_EROFS;
    }

    for (uint32_t i = 0; i < 256u; ++i) {
        const uint16_t w =
            (uint16_t)((uint16_t)buf[i * 2u] | ((uint16_t)buf[i * 2u + 1u] << 8u));
        __asm__ volatile("outw %0, %w1" : : "a"(w), "d"(ATA_PRIMARY_DATA));
    }

    /* 等待命令完成（BSY 清）；写失败（如介质只读）一律按 ATA_EROFS 报。 */
    for (uint32_t i = 0; i < 1000000u; ++i) {
        const uint8_t st = ata_inb(ATA_PRIMARY_CMD);
        if ((st & ATA_SR_BSY) == 0u) {
            return (st & ATA_SR_ERR) ? ATA_EROFS : ATA_OK;
        }
    }
    return ATA_EROFS;
}

/* ============================================================================
 * [ADD] B 组第 3 个功能：主通道「从设备」（slave）读扇区。
 *
 * 只追加，不改动上面的 ata_init / ata_read_sector / ata_write_sector 任何一行。
 * QEMU 里 -drive file=data.img,if=ide,index=1 把第二块盘放在主通道从设备，
 * 因此这里把驱动器选择字节从 0xE0（master）换成 0xF0（slave），其余时序完全
 * 复用上面已有的 static 辅助函数（ata_outb / ata_inb / ata_inw / ata_wait_*）。
 *
 * 从设备不存在时：选中后读状态寄存器若为 0x00（浮空/无设备）或 0xFF（总线浮高），
 * 直接返回 ATA_ENODEV，避免在不存在设备上做 1e6 次忙等。
 * ============================================================================ */

/* 选中主通道从设备并做存在性判断：存在返回 ATA_OK，不存在返回 ATA_ENODEV。 */
static int ata_select_slave(uint32_t lba)
{
    /* bit7=1, bit6=1(LBA), bit5=1, bit4=1(slave), bit3..0=LBA 高 4 位 */
    ata_outb(ATA_PRIMARY_DRIVE, 0xF0u | (uint8_t)((lba >> 24u) & 0x0Fu));
    ata_delay_400ns();
    const uint8_t st = ata_inb(ATA_PRIMARY_CMD);
    if (st == 0x00u || st == 0xFFu) {
        return ATA_ENODEV;   /* 没有从设备 */
    }
    return ATA_OK;
}

int ata_read_sector_slave(uint32_t lba, uint8_t *buf)
{
    if (buf == 0) {
        return ATA_EIO;
    }
    if (ata_select_slave(lba) != ATA_OK) {
        return ATA_ENODEV;
    }
    if (ata_wait_ready() != ATA_OK) {
        return ATA_EIO;
    }

    /* 与主盘读相同的 LBA28 读数流程，只是设备已在上面选成从盘。 */
    ata_outb(ATA_PRIMARY_SECTORS, 0x01u);
    ata_outb(ATA_PRIMARY_LBALO, (uint8_t)(lba & 0xFFu));
    ata_outb(ATA_PRIMARY_LBAMID, (uint8_t)((lba >> 8u) & 0xFFu));
    ata_outb(ATA_PRIMARY_LBAHI, (uint8_t)((lba >> 16u) & 0xFFu));

    ata_outb(ATA_PRIMARY_CMD, ATA_CMD_READ);

    int err = 0;
    if (ata_wait_drq(&err) != ATA_OK) {
        return ATA_EIO;
    }

    for (uint32_t i = 0; i < 256u; ++i) {
        const uint16_t w = ata_inw(ATA_PRIMARY_DATA);
        buf[i * 2u] = (uint8_t)(w & 0xFFu);
        buf[i * 2u + 1u] = (uint8_t)((w >> 8u) & 0xFFu);
    }

    for (uint32_t i = 0; i < 1000000u; ++i) {
        const uint8_t st = ata_inb(ATA_PRIMARY_CMD);
        if ((st & ATA_SR_BSY) == 0u) {
            return (st & ATA_SR_ERR) ? ATA_EIO : ATA_OK;
        }
    }
    return ATA_EIO;
}
