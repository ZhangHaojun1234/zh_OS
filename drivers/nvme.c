/* nvme.c - M32: NVMe 控制器寄存器、Admin / I-O 队列、一扇区读写。
 * 跟 AHCI 是两套完全不同的模型：AHCI 靠命令表 + FIS + PRDT，写 PxCI 触发；
 * NVMe 靠环形队列，64 字节命令写进 SQ，控制器把 16 字节完成项写回 CQ，两边各一个门铃，
 * 看 CQ 项的 phase 位判断有没有新完成。省了 FIS 和 128 字节对齐，代价是自己维护队列和相位。 */

#include "nvme.h"

#include <stdint.h>

#include "ata.h"
#include "paging.h"
#include "pci.h"

/* 控制器寄存器 */
#define NV_REG_CAP     0x00u
#define NV_REG_VS      0x08u
#define NV_REG_INTMS   0x0Cu
#define NV_REG_INTMC   0x10u
#define NV_REG_CC      0x14u
#define NV_REG_CSTS    0x1Cu
#define NV_REG_AQA     0x24u
#define NV_REG_ASQ     0x28u
#define NV_REG_ACQ     0x30u
#define NV_DOORBELL    0x1000u

#define CC_EN       (1u << 0)
#define CC_CSS_NVM  (0u << 4)
#define CC_MPS_4K   (0u << 7)
#define CC_IOSQES   (6u << 16)      /* 2^6 = 64 字节 */
#define CC_IOCQES   (4u << 20)      /* 2^4 = 16 字节 */
#define CSTS_RDY    (1u << 0)
#define CSTS_CFS    (1u << 1)

/* 管理命令 opcode */
#define ADM_IDENTIFY      0x06u
#define ADM_CREATE_IOCQ   0x05u
#define ADM_CREATE_IOSQ   0x01u
/* I/O 命令 opcode */
#define IO_WRITE          0x01u
#define IO_READ           0x02u

/* Identify 的 CNS 参数 */
#define ID_CNS_NAMESPACE  0x00u
#define ID_CNS_CONTROLLER 0x01u
#define ID_CNS_NSLIST     0x02u

#define QUEUE_ENTRIES 8u            /* 都是 8 项的小队列，够读一个扇区 */

/* 内存里的队列与缓冲
 * 同样靠恒等映射，把数组地址直接当物理地址交给控制器。
 * 规范要求 SQ / CQ 的基址 4KB 对齐（页大小）。
 */
static uint8_t g_asq[4096] __attribute__((aligned(4096)));
static uint8_t g_acq[4096] __attribute__((aligned(4096)));
static uint8_t g_isq[4096] __attribute__((aligned(4096)));
static uint8_t g_icq[4096] __attribute__((aligned(4096)));
static uint8_t g_id[4096]  __attribute__((aligned(4096)));
static uint8_t g_dma[512]  __attribute__((aligned(16)));

static volatile uint8_t *g_reg = (volatile uint8_t *)0;
static int g_ready = 0;
static uint32_t g_ns = 1u;              /* 命名空间 ID */
static uint32_t g_block = 512u;         /* 逻辑块大小 */
static uint32_t g_sectors = 0u;         /* 总扇区数（按 512 计） */
static char g_model[41];
static uint32_t g_bar_low = 0u;
static uint32_t g_sq1_tail = 0u;        /* 两个队列的门铃游标 */
static uint32_t g_cq1_head = 0u;
static uint32_t g_sq0_tail = 0u;
static uint32_t g_cq0_head = 0u;
static uint32_t g_cq1_phase = 1u;       /* CQ 初始相位为 1 */
static uint32_t g_cq0_phase = 1u;
static uint32_t g_cid = 1u;


static uint32_t reg_read(uint32_t off)
{
    return *(volatile uint32_t *)(g_reg + off);
}

static void reg_write(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(g_reg + off) = v;
}

static void reg_write64(uint32_t off, uint64_t v)
{
    *(volatile uint32_t *)(g_reg + off) = (uint32_t)(v & 0xFFFFFFFFull);
    *(volatile uint32_t *)(g_reg + off + 4u) = (uint32_t)(v >> 32);
}

static uint32_t phys_of(const void *p)
{
    return (uint32_t)(uintptr_t)p;      /* 恒等映射：虚拟 == 物理 */
}

/* 门铃：SQ 用前半（偏移 +0），CQ 用后半（偏移 +4），每个队列 8 字节 */
static void ring_sq(uint32_t qid, uint32_t tail)
{
    *(volatile uint32_t *)(g_reg + NV_DOORBELL + qid * 8u) = tail;
}

static void ring_cq(uint32_t qid, uint32_t head)
{
    *(volatile uint32_t *)(g_reg + NV_DOORBELL + qid * 8u + 4u) = head;
}

/* 提交一条 64 字节的命令到指定 SQ，返回分配的 CID */
static uint32_t submit(uint32_t qid, void *sq_base, uint32_t *tail,
                       const void *cmd64)
{
    uint8_t *slot = (uint8_t *)sq_base + (*tail) * 64u;
    uint32_t i;
    uint32_t cid = g_cid++;

    for (i = 0u; i < 64u; ++i) {
        slot[i] = ((const uint8_t *)cmd64)[i];
    }
    /* CID 放在 DW0 的 bit31..16 */
    *(volatile uint32_t *)slot = (*(volatile uint32_t *)slot & 0x0000FFFFu)
                               | (cid << 16);
    *tail = (*tail + 1u) % QUEUE_ENTRIES;
    ring_sq(qid, *tail);
    return cid;
}

/* 等一条完成项。成功返回 0，超时 / 状态码非 0 返回负值。
 * phase 位每绕队列一圈翻转一次，这是"有没有新完成"的唯一判据。 */
static int wait_completion(uint32_t qid, uint32_t *head, uint32_t *phase,
                           uint32_t *out_cid)
{
    uint32_t guard = 0u;
    volatile uint8_t *entry;

    while (1) {
        uint32_t dw3;
        uint32_t p;
        uint32_t sc;

        entry = (volatile uint8_t *)((qid == 0u ? g_acq : g_icq))
              + (*head) * 16u;
        dw3 = *(volatile uint32_t *)(entry + 12u);
        p = (dw3 >> 16) & 1u;
        if (p == *phase) {
            sc = (dw3 >> 17) & 0xFFu;
            if (out_cid != (uint32_t *)0) {
                *out_cid = dw3 & 0xFFFFu;
            }
            *head = (*head + 1u) % QUEUE_ENTRIES;
            if (*head == 0u) {
                *phase ^= 1u;
            }
            ring_cq(qid, *head);
            return (sc == 0u) ? 0 : -(int)sc;
        }
        if (++guard > 20000000u) {
            return -1000;               /* 超时：控制器没回完成项 */
        }
    }
}

static void fill_cmd(uint8_t *cmd, uint8_t opcode, uint32_t nsid, uint32_t cid)
{
    uint32_t i;
    for (i = 0u; i < 64u; ++i) {
        cmd[i] = 0u;
    }
    *(uint32_t *)(cmd + 0u) = opcode | (cid << 16);
    *(uint32_t *)(cmd + 4u) = nsid;
}

/* 初始化 */
static int nvme_create_io_queues(void)
{
    uint8_t cmd[64];
    uint32_t cid;

    /* 1) 建 I/O 完成队列（QID 1，8 项，物理连续） */
    fill_cmd(cmd, ADM_CREATE_IOCQ, 0u, 0u);
    *(uint32_t *)(cmd + 40u) = 1u | ((QUEUE_ENTRIES - 1u) << 16);   /* QID | QSIZE-1 */
    *(uint32_t *)(cmd + 44u) = 1u;                                   /* PC=1 */
    *(uint32_t *)(cmd + 24u) = phys_of(g_icq);          /* PRP1 = DW6 */
    cid = submit(0u, g_asq, &g_sq0_tail, cmd);
    if (wait_completion(0u, &g_cq0_head, &g_cq0_phase, (uint32_t *)0) != 0) {
        return -1;
    }
    (void)cid;

    /* 2) 建 I/O 提交队列（QID 1，绑定到刚建的 CQ 1） */
    fill_cmd(cmd, ADM_CREATE_IOSQ, 0u, 0u);
    *(uint32_t *)(cmd + 40u) = 1u | ((QUEUE_ENTRIES - 1u) << 16);
    *(uint32_t *)(cmd + 44u) = 1u | (1u << 16);      /* PC=1 | CQID=1 */
    *(uint32_t *)(cmd + 24u) = phys_of(g_isq);          /* PRP1 = DW6 */
    cid = submit(0u, g_asq, &g_sq0_tail, cmd);
    if (wait_completion(0u, &g_cq0_head, &g_cq0_phase, (uint32_t *)0) != 0) {
        return -1;
    }
    (void)cid;
    return 0;
}

int nvme_init(void)
{
    pci_dev_t dev;
    uint64_t bar;
    uint32_t cap_lo, csts, guard;
    uint8_t cmd[64];
    uint32_t cid;
    uint32_t lbads;

    g_ready = 0;
    g_reg = (volatile uint8_t *)0;

    if (!pci_find_class(PCI_CLASS_STORAGE, PCI_SUBCLASS_NVME, &dev)) {
        return -1;                      /* 没有 NVMe 控制器 */
    }
    bar = pci_bar_mmio64(&dev, 0u);     /* NVMe 的寄存器组固定在 BAR0 */
    if (bar == 0ull) {
        return -1;
    }
    g_bar_low = (uint32_t)(bar & 0xFFFFFFFFull);

    /* BAR0 通常也在 3GB 以上，低 1GB 的恒等映射盖不到，先按 2MB 大页映进来。
     * 注意 paging_map_2mb_range 成功返回 1、失败返回 0。 */
    if (paging_map_2mb_range(bar & ~0xFFFull, 0x200000u) == 0) {
        return -1;
    }
    g_reg = (volatile uint8_t *)(uintptr_t)bar;
    pci_enable_mem_busmaster(&dev);

    /* 1) 关控制器，等它真的停 */
    reg_write(NV_REG_CC, reg_read(NV_REG_CC) & ~CC_EN);
    for (guard = 0u; guard < 2000000u; ++guard) {
        if ((reg_read(NV_REG_CSTS) & CSTS_RDY) == 0u) {
            break;
        }
    }
    if ((reg_read(NV_REG_CSTS) & CSTS_RDY) != 0u) {
        return -1;                      /* 停不下来：控制器有问题 */
    }

    cap_lo = reg_read(NV_REG_CAP);
    (void)cap_lo;

    /* 2) 队列基址与大小：Admin 各 8 项 */
    reg_write(NV_REG_AQA, (QUEUE_ENTRIES - 1u) | ((QUEUE_ENTRIES - 1u) << 16));
    reg_write64(NV_REG_ASQ, (uint64_t)phys_of(g_asq));
    reg_write64(NV_REG_ACQ, (uint64_t)phys_of(g_acq));

    /* 3) 开控制器：4KB 页、64 字节 SQ 项、16 字节 CQ 项 */
    reg_write(NV_REG_CC, CC_EN | CC_CSS_NVM | CC_MPS_4K | CC_IOSQES | CC_IOCQES);
    for (guard = 0u; guard < 2000000u; ++guard) {
        csts = reg_read(NV_REG_CSTS);
        if ((csts & CSTS_CFS) != 0u) {
            return -1;                  /* 致命状态：固件报错 */
        }
        if ((csts & CSTS_RDY) != 0u) {
            break;
        }
    }
    if ((reg_read(NV_REG_CSTS) & CSTS_RDY) == 0u) {
        return -1;
    }

    /* 4) Identify Controller：拿型号字符串 */
    fill_cmd(cmd, ADM_IDENTIFY, 0u, 0u);
    *(uint32_t *)(cmd + 40u) = ID_CNS_CONTROLLER;
    *(uint32_t *)(cmd + 24u) = phys_of(g_id);            /* PRP1 = DW6 */
    cid = submit(0u, g_asq, &g_sq0_tail, cmd);
    if (wait_completion(0u, &g_cq0_head, &g_cq0_phase, (uint32_t *)0) != 0) {
        return -1;
    }
    (void)cid;
    {
        uint32_t i;
        for (i = 0u; i < 40u; ++i) {
            g_model[i] = (char)g_id[24u + i];
        }
        g_model[40] = '\0';
    }

    /* 5) Identify Namespace List：取第一个活动命名空间 */
    fill_cmd(cmd, ADM_IDENTIFY, 0u, 0u);
    *(uint32_t *)(cmd + 40u) = ID_CNS_NSLIST;
    *(uint32_t *)(cmd + 24u) = phys_of(g_id);            /* PRP1 = DW6 */
    cid = submit(0u, g_asq, &g_sq0_tail, cmd);
    if (wait_completion(0u, &g_cq0_head, &g_cq0_phase, (uint32_t *)0) != 0) {
        return -1;
    }
    (void)cid;
    g_ns = *(uint32_t *)g_id;
    if (g_ns == 0u) {
        g_ns = 1u;                      /* 有些控制器列表为空，退回 nsid=1 */
    }

    /* 6) Identify Namespace：块大小与容量 */
    fill_cmd(cmd, ADM_IDENTIFY, g_ns, 0u);
    *(uint32_t *)(cmd + 40u) = ID_CNS_NAMESPACE;
    *(uint32_t *)(cmd + 24u) = phys_of(g_id);            /* PRP1 = DW6 */
    cid = submit(0u, g_asq, &g_sq0_tail, cmd);
    if (wait_completion(0u, &g_cq0_head, &g_cq0_phase, (uint32_t *)0) != 0) {
        return -1;
    }
    (void)cid;
    {
        uint64_t nsze = *(uint64_t *)g_id;                 /* 逻辑块总数 */
        uint8_t  flbas = g_id[26];
        lbads = g_id[128 + (flbas & 0x0Fu) * 4u + 2u];      /* 每块字节数 = 2^lbads */
        if (lbads < 9u || lbads > 12u) {
            lbads = 9u;                                     /* 兜底 512 */
        }
        g_block = 1u << lbads;
        g_sectors = (uint32_t)(nsze * (g_block / 512u));
    }

    /* 7) 建一对 I/O 队列 */
    if (nvme_create_io_queues() != 0) {
        return -1;
    }

    g_ready = 1;
    return 0;
}

int nvme_ready(void)
{
    return g_ready;
}

/* 一扇区读写 */
static int nvme_rw(uint32_t lba, int is_write)
{
    uint8_t cmd[64];
    uint32_t cid;

    if (!g_ready) {
        return -1;
    }
    fill_cmd(cmd, is_write ? IO_WRITE : IO_READ, g_ns, 0u);
    *(uint32_t *)(cmd + 40u) = lba;                      /* CDW10: SLBA 低 32 位 */
    *(uint32_t *)(cmd + 44u) = 0u;                       /* CDW11: SLBA 高 32 位 */
    *(uint32_t *)(cmd + 24u) = phys_of(g_dma);           /* PRP1 = DW6 */
    *(uint32_t *)(cmd + 48u) = 0u;                       /* CDW12: NLB=0 -> 1 块 */
    cid = submit(1u, g_isq, &g_sq1_tail, cmd);
    (void)cid;
    if (wait_completion(1u, &g_cq1_head, &g_cq1_phase, (uint32_t *)0) != 0) {
        return -1;
    }
    return 0;
}

int nvme_read_sector(uint32_t lba, uint8_t *buf)
{
    uint32_t i;

    if (buf == (uint8_t *)0) {
        return -1;
    }
    if (nvme_rw(lba, 0) != 0) {
        return -1;
    }
    for (i = 0u; i < 512u; ++i) {
        buf[i] = g_dma[i];
    }
    return 0;
}

int nvme_write_sector(uint32_t lba, const uint8_t *buf)
{
    uint32_t i;

    if (buf == (const uint8_t *)0) {
        return -1;
    }
    for (i = 0u; i < 512u; ++i) {
        g_dma[i] = buf[i];
    }
    return nvme_rw(lba, 1);
}

uint32_t nvme_bar_low(void)      { return g_bar_low; }
uint32_t nvme_version(void)      { return g_reg ? reg_read(NV_REG_VS) : 0u; }
uint32_t nvme_namespace(void)    { return g_ns; }
uint32_t nvme_block_size(void)   { return g_block; }
uint32_t nvme_sector_count(void) { return g_sectors; }
const char *nvme_model(void)     { return g_model; }
