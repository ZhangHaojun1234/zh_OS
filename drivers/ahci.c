/* ============================================================================
 * zh_OS 1.0 (x86_64)
 * File:    drivers/ahci.c
 * Version: 1.0
 * Brief:   [ADD] M31：AHCI 端口寄存器、命令表 / FIS / PRDT 与一扇区读写。
 * ============================================================================ */

#include "ahci.h"

#include <stdint.h>

#include "ata.h"
#include "paging.h"
#include "pci.h"

/* --------------------------- 寄存器偏移 --------------------------- */
#define AHCI_CAP        0x00u
#define AHCI_GHC        0x04u
#define AHCI_PI         0x0Cu
#define AHCI_VER        0x10u
#define AHCI_PORT_BASE  0x100u
#define AHCI_PORT_STRIDE 0x80u

#define P_CLB    0x00u
#define P_CLBU   0x04u
#define P_FB     0x08u
#define P_FBU    0x0Cu
#define P_IS     0x10u
#define P_IE     0x14u
#define P_CMD    0x18u
#define P_TFD    0x20u
#define P_SIG    0x24u
#define P_SSTS   0x28u
#define P_SCTL   0x2Cu
#define P_SERR   0x30u
#define P_SACT   0x34u
#define P_CI     0x38u

#define GHC_AE   (1u << 31)
#define CMD_ST   (1u << 0)
#define CMD_FRE  (1u << 4)
#define CMD_FR   (1u << 14)
#define CMD_CR   (1u << 15)
#define TFD_ERR  (1u << 0)
#define TFD_DF   (1u << 5)
#define SSTS_DET_MASK 0x0Fu
#define SSTS_DET_OK   0x03u
#define SIG_SATA      0x00000101u

/* ----------------------- 内存里的三样"表" -----------------------
 * AHCI 的 DMA 只认物理地址。本内核的恒等映射让虚拟地址 == 物理地址，
 * 所以这些静态数组的地址直接当物理地址用。
 * 对齐是硬要求：命令表 1024、FIS 256、命令头(在命令表里)128。
 */
static uint8_t  g_cl[1024] __attribute__((aligned(1024)));    /* command list */
static uint8_t  g_fis[256] __attribute__((aligned(256)));     /* received FIS */
static uint8_t  g_ct[512]  __attribute__((aligned(128)));     /* command table */
static uint8_t  g_dma[512] __attribute__((aligned(16)));      /* 一个扇区的数据 */

static volatile uint32_t *g_abar = (volatile uint32_t *)0;
static uint32_t g_cap = 0u;
static uint32_t g_pi = 0u;
static uint32_t g_port = 0u;
static uint32_t g_port_off = 0u;
static int g_ready = 0;

static inline uint32_t phys_of(const void *p)
{
    return (uint32_t)(uintptr_t)p;      /* 恒等映射：虚拟 == 物理 */
}

static uint32_t ahci_read_reg(uint32_t off)
{
    return g_abar[off / 4u];
}

static void ahci_write_reg(uint32_t off, uint32_t v)
{
    g_abar[off / 4u] = v;
}

static int ahci_port_wait_idle(void)
{
    uint32_t guard = 0u;

    while ((ahci_read_reg(g_port_off + P_CMD) & CMD_CR) != 0u) {
        if (++guard > 100000u) {
            return -1;
        }
    }
    return 0;
}

/* 发一条 ATA 命令：cmd 是指令码（0x25 读 / 0x35 写），写操作要把 W 位置 1。
 * PRDT 只放一个数据段 = 512 字节 = 一个扇区。 */
static int ahci_cmd(uint8_t cmd, uint32_t lba, int is_write)
{
    volatile uint8_t *ct = g_ct;
    volatile uint8_t *fis = g_ct;          /* CFIS 就在命令表偏移 0 */
    volatile uint32_t *hdr = (volatile uint32_t *)g_cl;
    uint32_t guard = 0u;
    uint32_t tfd;

    /* 命令表：CFIS(64) + ACMD(16) + 保留(48) + PRDT(从 0x80 开始) */
    {
        uint32_t i;
        for (i = 0u; i < sizeof(g_ct); ++i) {
            ct[i] = 0u;
        }
    }

    fis[0] = 0x27u;                        /* H2D Register FIS */
    fis[1] = 0x80u;                        /* bit7=1：这是命令，不是控制 */
    fis[2] = cmd;
    fis[3] = 0u;                           /* feature low */
    fis[4] = (uint8_t)(lba & 0xFFu);
    fis[5] = (uint8_t)((lba >> 8u) & 0xFFu);
    fis[6] = (uint8_t)((lba >> 16u) & 0xFFu);
    fis[7] = 0x40u;                        /* LBA 模式 */
    fis[8] = (uint8_t)((lba >> 24u) & 0xFFu);
    fis[9] = 0u;
    fis[10] = 0u;
    fis[11] = 0u;
    fis[12] = 1u;                          /* 1 个扇区 */
    fis[13] = 0u;

    /* PRDT 一项：数据缓冲物理地址 + 字节数-1 */
    {
        uint32_t dba = phys_of(g_dma);
        uint32_t at = 0x80u;
        ct[at + 0u] = (uint8_t)(dba & 0xFFu);
        ct[at + 1u] = (uint8_t)((dba >> 8u) & 0xFFu);
        ct[at + 2u] = (uint8_t)((dba >> 16u) & 0xFFu);
        ct[at + 3u] = (uint8_t)((dba >> 24u) & 0xFFu);
        ct[at + 4u] = 0u;
        ct[at + 5u] = 0u;
        ct[at + 6u] = 0u;
        ct[at + 7u] = 0u;
        /* AHCI 1.3：偏移 8 是保留，DBC 在偏移 12（低 22 位 = 字节数-1，
         * 最高位 I = 传输完成中断）。写错到偏移 8 的话控制器会认为
         * DBC = 0，只搬 1 个字节然后报"完成"，数据缓冲仍然是 0。 */
        ct[at + 8u] = 0u;
        ct[at + 9u] = 0u;
        ct[at + 10u] = 0u;
        ct[at + 11u] = 0u;
        ct[at + 12u] = (uint8_t)(511u & 0xFFu);
        ct[at + 13u] = (uint8_t)((511u >> 8u) & 0xFFu);
        ct[at + 14u] = (uint8_t)((511u >> 16u) & 0x03u);
        ct[at + 15u] = 0x80u;                      /* bit31 = I */
    }

    /* 命令头 DW0：CFL=5（20 字节 FIS）| W(bit5) | PRDTL(bit16..)=1 */
    hdr[0] = 5u | (is_write ? (1u << 5) : 0u) | (1u << 16);
    hdr[1] = 0u;                            /* 描述符字节数回填，发命令前清零 */
    hdr[2] = phys_of(g_ct);
    hdr[3] = 0u;

    /* 启动 DMA：清 IS / SERR，置 CI */
    ahci_write_reg(g_port_off + P_IS, 0xFFFFFFFFu);
    ahci_write_reg(g_port_off + P_SERR, 0xFFFFFFFFu);
    ahci_write_reg(g_port_off + P_CI, 1u);

    while ((ahci_read_reg(g_port_off + P_CI) & 1u) != 0u) {
        if (++guard > 1000000u) {
            return -1;                      /* 超时：端口卡死 */
        }
    }

    tfd = ahci_read_reg(g_port_off + P_TFD);
    if ((tfd & (TFD_ERR | TFD_DF)) != 0u) {
        return -1;
    }
    return 0;
}

static int ahci_port_start(uint32_t port)
{
    uint32_t cmd;

    g_port = port;
    g_port_off = AHCI_PORT_BASE + port * AHCI_PORT_STRIDE;

    /* 1) 停 ST + 关 FRE，等 CR 清零。
     *    FRE 必须先关掉：PxCLB / PxFB 在 FRE=1 时写入会被控制器直接丢弃，
     *    表面上"初始化成功"，实际 FIS 区还指向 0，命令永远不落地。 */
    cmd = ahci_read_reg(g_port_off + P_CMD);
    cmd &= ~(CMD_ST | CMD_FRE);
    ahci_write_reg(g_port_off + P_CMD, cmd);
    if (ahci_port_wait_idle() != 0) {
        return -1;
    }

    /* 2) 设备已就绪就别复位；需要复位时发 COMRESET 清 DET，然后
     *    必须等到 SSTS.DET 回到 3（设备识别完成）才能发命令 ——
     *    少了这一步，命令会被设备直接丢掉：CI 立刻清零、TFD 显示 DRDY、
     *    数据一个字节都不搬，看起来"成功"但缓冲全是 0。 */
    if ((ahci_read_reg(g_port_off + P_SSTS) & SSTS_DET_MASK) != SSTS_DET_OK) {
        ahci_write_reg(g_port_off + P_SCTL, 1u);
        {
            uint32_t guard = 0u;
            while ((ahci_read_reg(g_port_off + P_SCTL) & SSTS_DET_MASK) != 0u) {
                if (++guard > 200000u) {
                    break;
                }
            }
        }
        ahci_write_reg(g_port_off + P_SCTL, 0u);
    }
    {
        uint32_t guard = 0u;
        while ((ahci_read_reg(g_port_off + P_SSTS) & SSTS_DET_MASK) != SSTS_DET_OK) {
            if (++guard > 500000u) {
                return -1;      /* 设备一直没就绪：这个端口放弃 */
            }
        }
    }

    /* 复位后把上一轮残留的中断 / 错误状态清干净 */
    ahci_write_reg(g_port_off + P_IS, 0xFFFFFFFFu);
    ahci_write_reg(g_port_off + P_SERR, 0xFFFFFFFFu);

    /* 3) 命令表 / FIS 的物理地址 */
    ahci_write_reg(g_port_off + P_CLB, phys_of(g_cl));
    ahci_write_reg(g_port_off + P_CLBU, 0u);
    ahci_write_reg(g_port_off + P_FB, phys_of(g_fis));
    ahci_write_reg(g_port_off + P_FBU, 0u);

    /* 4) 先开 FRE（让控制器认下 FIS 区），再开 ST */
    cmd = ahci_read_reg(g_port_off + P_CMD);
    cmd |= CMD_FRE;
    ahci_write_reg(g_port_off + P_CMD, cmd);
    cmd |= CMD_ST;
    ahci_write_reg(g_port_off + P_CMD, cmd);

    /* 5) 回读校验：CLB / FB / CMD 里必须真的是我们写的地址和位 */
    if (ahci_read_reg(g_port_off + P_CLB) != (phys_of(g_cl) & ~0x3FFu)
        || ahci_read_reg(g_port_off + P_FB) != (phys_of(g_fis) & ~0xFFu)
        || (ahci_read_reg(g_port_off + P_CMD) & (CMD_FRE | CMD_ST)) != (CMD_FRE | CMD_ST)) {
        return -1;
    }
    return 0;
}

int ahci_init(void)
{
    pci_dev_t dev;
    uint32_t bar;

    g_ready = 0;
    if (!pci_find_class(PCI_CLASS_STORAGE, PCI_SUBCLASS_SATA, &dev)) {
        return -1;
    }
    bar = pci_bar_mmio(&dev, 5u);           /* AHCI 的 ABAR 固定在 BAR5 */
    if (bar == 0u) {
        return -1;
    }
    /* ABAR 通常在 3GB 以上（例如 0xFEBF0000），低 1GB 的恒等映射盖不到，
     * 所以要先按 2MB 大页把它映射进来，再解引用。 */
    /* 注意：这个函数成功返回 1、失败返回 0（跟常见 0=成功相反）。 */
    if (paging_map_2mb_range((uint64_t)(bar & 0xFFFFF000u), 0x200000u) == 0) {
        return -1;
    }
    g_abar = (volatile uint32_t *)(uintptr_t)bar;

    pci_enable_mem_busmaster(&dev);

    g_cap = ahci_read_reg(AHCI_CAP);
    g_pi = ahci_read_reg(AHCI_PI);
    ahci_write_reg(AHCI_GHC, ahci_read_reg(AHCI_GHC) | GHC_AE);

    /* 挑一个接上了 SATA 盘的端口 */
    {
        uint32_t i;
        for (i = 0u; i < 32u; ++i) {
            uint32_t off, sig, ssts;
            if ((g_pi & (1u << i)) == 0u) {
                continue;
            }
            off = AHCI_PORT_BASE + i * AHCI_PORT_STRIDE;
            sig = ahci_read_reg(off + P_SIG);
            ssts = ahci_read_reg(off + P_SSTS);
            if (sig != SIG_SATA) {
                continue;
            }
            if ((ssts & SSTS_DET_MASK) != SSTS_DET_OK) {
                continue;
            }
            if (ahci_port_start(i) != 0) {
                continue;
            }
            g_ready = 1;
            break;
        }
    }
    return g_ready ? 0 : -1;
}

int ahci_ready(void)
{
    return g_ready;
}

int ahci_read_sector(uint32_t lba, uint8_t *buf)
{
    uint32_t i;

    if (!g_ready || buf == (uint8_t *)0) {
        return ATA_ENODEV;
    }
    if (ahci_cmd(0x25u, lba, 0) != 0) {      /* READ DMA EXT */
        return ATA_EIO;
    }
    for (i = 0u; i < 512u; ++i) {
        buf[i] = g_dma[i];
    }
    return ATA_OK;
}

int ahci_write_sector(uint32_t lba, const uint8_t *buf)
{
    uint32_t i;

    if (!g_ready || buf == (const uint8_t *)0) {
        return ATA_ENODEV;
    }
    for (i = 0u; i < 512u; ++i) {
        g_dma[i] = buf[i];
    }
    if (ahci_cmd(0x35u, lba, 1) != 0) {      /* WRITE DMA EXT */
        return ATA_EIO;
    }
    return ATA_OK;
}

uint32_t ahci_abar(void)              { return (uint32_t)(uintptr_t)g_abar; }
uint32_t ahci_cap(void)               { return g_cap; }
uint32_t ahci_ports_implemented(void) { return g_pi; }
uint32_t ahci_port_number(void)       { return g_port; }
uint32_t ahci_port_signature(void)
{
    return g_ready ? ahci_read_reg(g_port_off + P_SIG) : 0u;
}

/* [ADD] M31：诊断用 —— 初始化失败后也要能把每个端口的寄存器读出来看 */
uint32_t ahci_port_sig_of(uint32_t i)
{
    if (g_abar == (volatile uint32_t *)0 || i > 31u) {
        return 0u;
    }
    return ahci_read_reg(AHCI_PORT_BASE + i * AHCI_PORT_STRIDE + P_SIG);
}

uint32_t ahci_port_ssts_of(uint32_t i)
{
    if (g_abar == (volatile uint32_t *)0 || i > 31u) {
        return 0u;
    }
    return ahci_read_reg(AHCI_PORT_BASE + i * AHCI_PORT_STRIDE + P_SSTS);
}

uint32_t ahci_port_cmd_of(uint32_t i)
{
    if (g_abar == (volatile uint32_t *)0 || i > 31u) {
        return 0u;
    }
    return ahci_read_reg(AHCI_PORT_BASE + i * AHCI_PORT_STRIDE + P_CMD);
}

/* [ADD] M31：诊断 —— 发命令前后关键寄存器 */
uint32_t ahci_dbg_hdr0(void)
{
    return ((volatile uint32_t *)g_cl)[0];
}

uint32_t ahci_dbg_hdr2(void)
{
    return ((volatile uint32_t *)g_cl)[2];
}

uint32_t ahci_dbg_ct0(void)
{
    return ((volatile uint32_t *)g_ct)[0];
}

uint32_t ahci_dbg_ct_prdt(void)
{
    return ((volatile uint32_t *)(g_ct + 0x80))[0];
}

uint32_t ahci_dbg_ct_dbc(void)
{
    return ((volatile uint32_t *)(g_ct + 0x88))[0];
}

/* 映射是否成功（失败时 BAR 值仍然留着，便于对照） */
uint32_t ahci_map_status(void)
{
    return g_abar == (volatile uint32_t *)0 ? 0u : 1u;
}
