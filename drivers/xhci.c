/* xhci.c - M33: xHCI 控制器加 HID boot 键盘，三样东西撑着：
 * 命令环 ， 软件写 16 字节命令 TRB，敲门铃让控制器来取；
 * 事件环 ， 控制器写事件 TRB，靠 cycle 位判断有没有新的；
 * 设备上下文 ， 每个 slot 一块，slot ctx 加各端点 ctx。
 * 地址都按物理地址使，本内核恒等映射，虚拟地址直接当物理地址。 */

#include "xhci.h"

#include <stdint.h>

#include "keyboard.h"
#include "paging.h"
#include "pci.h"
#include "pit.h"

/* 寄存器 */
#define XC_CAPLEN      0x00u
#define XC_HCSPARAMS1  0x04u
#define XC_HCCPARAMS1  0x10u
#define XC_DBOFF       0x14u
#define XC_RTSOFF      0x18u

#define OP_USBCMD      0x00u
#define OP_USBSTS      0x04u
#define OP_CRCR        0x18u
#define OP_DCBAAP      0x30u
#define OP_CONFIG      0x38u
#define OP_PORT_BASE   0x400u
#define OP_PORT_STRIDE 0x10u

#define RT_ERSTSZ      0x08u
#define RT_ERSTBA      0x10u
#define RT_ERDP        0x18u

#define CMD_RS         (1u << 0)
#define CMD_HCRST      (1u << 1)
#define STS_HCH        (1u << 0)
#define STS_CNR        (1u << 11)

#define PORT_CCS       (1u << 0)
#define PORT_PED       (1u << 1)
#define PORT_PR        (1u << 4)
#define PORT_PP        (1u << 9)
#define PORT_CSC       (1u << 17)
#define PORT_PEC       (1u << 19)
#define PORT_WRC       (1u << 20)
#define PORT_PRC       (1u << 21)

/* TRB 类型 */
#define TRB_NORMAL        1u
#define TRB_SETUP         2u
#define TRB_DATA          3u
#define TRB_STATUS        4u
#define TRB_LINK          6u
#define TRB_EVT_TRANSFER  32u
#define TRB_EVT_CMD       33u
#define TRB_EVT_PORT      34u
#define CMD_ADDRESS_DEV   11u
#define CMD_CONFIGURE_EP  12u

#define TRB_CYCLE  (1u << 0)
#define TRB_IOC    (1u << 5)
#define TRB_IDT    (1u << 6)
#define TRB_TC     (1u << 1)
#define TRB_TYPE_SHIFT 10u

/* 队列与上下文 */
#define CMD_TRBS   16u
#define EVT_TRBS   16u
#define EP0_TRBS   32u
#define INTR_TRBS  256u
#define CTL_BUF    512u

static uint8_t g_cmd_ring[CMD_TRBS * 16u]     __attribute__((aligned(64)));
static uint8_t g_evt_ring[EVT_TRBS * 16u]     __attribute__((aligned(64)));
static uint8_t g_erst[16]                     __attribute__((aligned(64)));
static uint8_t g_ep0_ring[EP0_TRBS * 16u]     __attribute__((aligned(64)));
static uint8_t g_intr_ring[INTR_TRBS * 16u]   __attribute__((aligned(64)));
static uint8_t g_dcbaa[256]                   __attribute__((aligned(64)));
static uint8_t g_dev_ctx[2048]                __attribute__((aligned(64)));
static uint8_t g_input_ctx[1024]              __attribute__((aligned(64)));
static uint8_t g_report[64]                   __attribute__((aligned(64)));  /* 4 份 8 字节报告 */
static uint8_t g_ctl_buf[CTL_BUF]             __attribute__((aligned(64)));

static volatile uint8_t *g_bar = (volatile uint8_t *)0;
static uint32_t g_op;
static uint32_t g_rt;
static uint32_t g_db;

static int g_ready = 0;
static int g_err = 0;
static uint32_t g_max_ports = 0;
static uint32_t g_max_slots = 0;
static uint32_t g_speed = 0;
static uint32_t g_slot = 1;
static uint32_t g_ep_addr = 0;
static uint32_t g_dci = 0;
static uint32_t g_iface = 0;          /* HID 接口号（GET_REPORT 的 wIndex） */
static uint64_t g_last_tick = 0;      /* 上一次 GET_REPORT 的时刻 */
static uint32_t g_ep_mps_unused = 0;
static uint32_t g_ep_interval_unused = 0;
static uint32_t g_bar_low = 0;
static volatile uint32_t g_reports = 0;
static volatile uint32_t g_presses = 0;   /* 按下（边沿）事件计数 */
static volatile uint32_t g_chars = 0;     /* 真正送进键盘缓冲的字符计数 */
static uint32_t g_last_key = 0;   /* 诊断用：最近一次按下的 usage id */

static uint32_t g_cmd_enq = 0;
static uint32_t g_cmd_cycle = 1;
static uint32_t g_ep0_enq = 0;
static uint32_t g_ep0_cycle = 1;
static uint32_t g_intr_enq = 0;
static uint32_t g_intr_out = 0;      /* 已挂出、还没完成的 IN 传输数 */
static uint32_t g_intr_wrap = 0;     /* 到环尾了，等排空再绕回 */
static uint32_t g_intr_cycle = 1;
static uint32_t g_evt_deq = 0;
static uint32_t g_evt_cycle = 1;
static uint32_t g_last_evt_dw3 = 0;   /* 最近一条事件的 DW3（取 slot id 用） */

static uint8_t g_mod;              /* 当前修饰键 */
static uint8_t g_keys[6];          /* 上一份报告里的 6 个按键码 */

static void memzero(void *p, uint32_t n)
{
    uint8_t *b = (uint8_t *)p;
    uint32_t i;
    for (i = 0u; i < n; ++i) {
        b[i] = 0u;
    }
}

static uint32_t phys_of(const void *p)
{
    return (uint32_t)(uintptr_t)p;
}

static uint32_t rd(uint32_t off)          { return *(volatile uint32_t *)(g_bar + off); }
static void     wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(g_bar + off) = v; }

static void wr64(uint32_t off, uint64_t v)
{
    *(volatile uint32_t *)(g_bar + off) = (uint32_t)(v & 0xFFFFFFFFull);
    *(volatile uint32_t *)(g_bar + off + 4u) = (uint32_t)(v >> 32);
}

/* TRB */
static void trb_write(uint8_t *base, uint32_t idx, uint32_t dw0, uint32_t dw1,
                      uint32_t dw2, uint32_t dw3)
{
    uint8_t *t = base + idx * 16u;
    *(volatile uint32_t *)(t + 0u) = dw0;
    *(volatile uint32_t *)(t + 4u) = dw1;
    *(volatile uint32_t *)(t + 8u) = dw2;
    *(volatile uint32_t *)(t + 12u) = dw3;
}

static int evt_pending(void)
{
    volatile uint8_t *t = g_evt_ring + g_evt_deq * 16u;
    uint32_t dw3 = *(volatile uint32_t *)(t + 12u);
    return ((dw3 & TRB_CYCLE) == g_evt_cycle) ? 1 : 0;
}

static uint32_t evt_type(void)
{
    volatile uint8_t *t = g_evt_ring + g_evt_deq * 16u;
    uint32_t dw3 = *(volatile uint32_t *)(t + 12u);
    return (dw3 >> TRB_TYPE_SHIFT) & 0x3Fu;
}

static uint32_t evt_status(void)
{
    volatile uint8_t *t = g_evt_ring + g_evt_deq * 16u;
    uint32_t dw2 = *(volatile uint32_t *)(t + 8u);
    return (dw2 >> 24) & 0xFFu;
}

static uint32_t evt_dw3(void)
{
    volatile uint8_t *t = g_evt_ring + g_evt_deq * 16u;
    return *(volatile uint32_t *)(t + 12u);
}

static uint32_t evt_param_lo(void)
{
    volatile uint8_t *t = g_evt_ring + g_evt_deq * 16u;
    return *(volatile uint32_t *)(t + 0u);
}

static void evt_advance(void)
{
    g_evt_deq = (g_evt_deq + 1u) % EVT_TRBS;
    if (g_evt_deq == 0u) {
        g_evt_cycle ^= 1u;
    }
    /* ERDP 前移，并置 EHB(bit3) 表示"已处理完中断" */
    wr64(g_rt + RT_ERDP,
         (uint64_t)phys_of(g_evt_ring + g_evt_deq * 16u) | (1ull << 3));
}

/* 环推进 */
/*
 * 环的最后一项固定是 Link TRB（指回环首、TC=1）。控制器读到就跳回开头并把 cycle 取反，
 * 所以软件绕回时也要翻转 cycle、按新 cycle 把 Link TRB 重写一遍。不重写的话，下一圈第一条
 * TRB 会被当成"还没轮到"，永远不执行。
 */
static void ring_link(uint8_t *base, uint32_t ntrbs, uint32_t cycle)
{
    trb_write(base, ntrbs - 1u, (uint32_t)phys_of(base), 0u, 0u,
              (TRB_LINK << TRB_TYPE_SHIFT) | TRB_TC | cycle);
}

static uint32_t ring_next(uint32_t idx, uint32_t *cycle,
                          uint8_t *base, uint32_t ntrbs)
{
    uint32_t next = idx + 1u;

    (void)base;
    if (next >= ntrbs - 1u) {
        next = 0u;
        /* Link TRB 带 TC=1，控制器跳回环首后会自己翻 cycle，软件只要同步翻自己那份就行。
         * 千万别去改 Link 的 cycle 位：改早了控制器正读到一半，会以为这条 Link 不属于本轮，
         * 于是停住。 */
        *cycle ^= 1u;
    }
    return next;
}

/* 命令环 */
static void cmd_doorbell(void)
{
    *(volatile uint32_t *)(g_bar + g_db) = 0u;   /* target 0 = command ring */
}

static int wait_cmd(uint32_t guard)
{
    uint32_t i;

    for (i = 0u; i < guard; ++i) {
        uint32_t type, code;
        if (!evt_pending()) {
            continue;
        }
        type = evt_type();
        if (type != TRB_EVT_CMD) {
            evt_advance();
            continue;
        }
        code = evt_status();
        g_last_evt_dw3 = evt_dw3();
        evt_advance();
        return (code == 1u) ? 0 : -(int)code;
    }
    return -1000;
}

static int cmd_submit(uint32_t dw0, uint32_t dw1, uint32_t dw2, uint32_t dw3)
{
    uint32_t idx = g_cmd_enq;
    int rc;

    trb_write(g_cmd_ring, idx, dw0, dw1, dw2, dw3 | (TRB_CYCLE * g_cmd_cycle));
    g_cmd_enq = ring_next(idx, &g_cmd_cycle, g_cmd_ring, CMD_TRBS);
    cmd_doorbell();
    rc = wait_cmd(2000000u);
    return rc;
}

/* 上下文 */
static uint32_t *in_icc(void)  { return (uint32_t *)(g_input_ctx + 0u); }
static uint32_t *in_slot(void) { return (uint32_t *)(g_input_ctx + 32u); }
static uint32_t *in_ep(uint32_t dci) { return (uint32_t *)(g_input_ctx + 32u + dci * 32u); }

/* EP0 控制传输 */
static int ep0_transfer(uint8_t bmRequestType, uint8_t bRequest,
                        uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                        const uint8_t *out_data, uint8_t *in_data)
{
    uint32_t i;
    uint32_t idx;
    uint32_t dir;
    int rc = -1001;

    if (wLength != 0u) {
        if (out_data != (const uint8_t *)0) {
            for (i = 0u; i < (uint32_t)wLength; ++i) {
                g_ctl_buf[i] = out_data[i];
            }
        } else {
            memzero(g_ctl_buf, (uint32_t)wLength);
        }
    }

    /* Setup Stage */
    idx = g_ep0_enq;
    trb_write(g_ep0_ring, idx,
              (uint32_t)bmRequestType | ((uint32_t)bRequest << 8)
                  | ((uint32_t)wValue << 16),
              (uint32_t)wIndex | ((uint32_t)wLength << 16),
              8u,
              (TRB_SETUP << TRB_TYPE_SHIFT) | TRB_IDT
                  | (TRB_CYCLE * g_ep0_cycle));
    g_ep0_enq = ring_next(idx, &g_ep0_cycle, g_ep0_ring, EP0_TRBS);

    if (wLength != 0u) {
        dir = (out_data == (const uint8_t *)0) ? 1u : 0u;   /* 1 = IN */
        idx = g_ep0_enq;
        trb_write(g_ep0_ring, idx, (uint32_t)phys_of(g_ctl_buf), 0u,
                  (uint32_t)wLength,
                  (TRB_DATA << TRB_TYPE_SHIFT) | (dir << 16)
                      | (TRB_CYCLE * g_ep0_cycle));
        g_ep0_enq = ring_next(idx, &g_ep0_cycle, g_ep0_ring, EP0_TRBS);
    }

    /* Status Stage：方向与数据阶段相反 */
    dir = (wLength != 0u && out_data == (const uint8_t *)0) ? 0u : 1u;
    idx = g_ep0_enq;
    trb_write(g_ep0_ring, idx, 0u, 0u, 0u,
              (TRB_STATUS << TRB_TYPE_SHIFT) | (dir << 16) | TRB_IOC
                  | (TRB_CYCLE * g_ep0_cycle));
    g_ep0_enq = ring_next(idx, &g_ep0_cycle, g_ep0_ring, EP0_TRBS);

    *(volatile uint32_t *)(g_bar + g_db + 4u) = 1u;   /* EP0 的 DCI = 1 */

    for (i = 0u; i < 4000000u; ++i) {
        uint32_t type, code;
        if (!evt_pending()) {
            continue;
        }
        type = evt_type();
        if (type != TRB_EVT_TRANSFER) {
            evt_advance();
            continue;
        }
        code = evt_status();
        evt_advance();
        rc = (code == 1u) ? 0 : -(int)code;             /* 1 = Success */
        break;
    }

    if (rc == 0 && wLength != 0u && in_data != (uint8_t *)0) {
        for (i = 0u; i < (uint32_t)wLength; ++i) {
            in_data[i] = g_ctl_buf[i];
        }
    }
    return rc;
}

/* 端口 */
static int reset_port(uint32_t port, uint32_t *speed)
{
    uint32_t off = g_op + OP_PORT_BASE + (port - 1u) * OP_PORT_STRIDE;
    uint32_t v = rd(off);
    uint32_t i;

    if ((v & PORT_CCS) == 0u) {
        return -1;
    }
    if ((v & PORT_PP) == 0u) {
        wr(off, (v | PORT_PP) & ~PORT_PRC);
        v = rd(off);
    }

    wr(off, (v & ~(PORT_PRC | PORT_CSC | PORT_PEC | PORT_WRC)) | PORT_PR);
    for (i = 0u; i < 100000u; ++i) {
        v = rd(off);
        if ((v & PORT_PR) == 0u) {
            break;
        }
    }
    for (i = 0u; i < 100000u; ++i) {
        v = rd(off);
        if ((v & PORT_PRC) != 0u) {
            break;
        }
    }
    *speed = (v >> 10) & 0x0Fu;
    /* 写 1 清 CSC/PRC/PEC/WRC；PP 与 PED 保持 */
    wr(off, (v & ~(PORT_PRC | PORT_CSC | PORT_PEC | PORT_WRC))
           | (v & PORT_PP));
    return 0;
}

/* 初始化 */
static int setup_controller(void)
{
    uint32_t i;
    uint32_t v;

    /* 1) 先停：USBCMD.RS=0，等 HCH=1 */
    wr(g_op + OP_USBCMD, 0u);
    for (i = 0u; i < 100000u; ++i) {
        if ((rd(g_op + OP_USBSTS) & STS_HCH) != 0u) {
            break;
        }
    }
    /* 2) 复位控制器 */
    wr(g_op + OP_USBCMD, CMD_HCRST);
    for (i = 0u; i < 100000u; ++i) {
        v = rd(g_op + OP_USBCMD);
        if ((v & CMD_HCRST) == 0u) {
            break;
        }
    }
    for (i = 0u; i < 100000u; ++i) {
        if ((rd(g_op + OP_USBSTS) & STS_CNR) == 0u) {
            break;
        }
    }

    /* 3) 使能 slot 数、设备上下文基址 */
    wr(g_op + OP_CONFIG, g_max_slots);
    memzero(g_dcbaa, sizeof(g_dcbaa));
    wr64(g_op + OP_DCBAAP, (uint64_t)phys_of(g_dcbaa));

    /* 4) 命令环：RCS=1 */
    memzero(g_cmd_ring, sizeof(g_cmd_ring));
    g_cmd_enq = 0u;
    g_cmd_cycle = 1u;
    ring_link(g_cmd_ring, CMD_TRBS, g_cmd_cycle);
    wr64(g_op + OP_CRCR, (uint64_t)phys_of(g_cmd_ring) | 1ull);

    /* 5) 事件环：一个段，16 项 */
    memzero(g_evt_ring, sizeof(g_evt_ring));
    *(uint64_t *)(g_erst + 0u) = (uint64_t)phys_of(g_evt_ring);
    *(uint32_t *)(g_erst + 8u) = EVT_TRBS;
    *(uint32_t *)(g_erst + 12u) = 0u;
    wr(g_rt + RT_ERSTSZ, 1u);
    wr64(g_rt + RT_ERSTBA, (uint64_t)phys_of(g_erst));
    wr64(g_rt + RT_ERDP, (uint64_t)phys_of(g_evt_ring));
    g_evt_deq = 0u;
    g_evt_cycle = 1u;

    /* 6) 开跑 */
    wr(g_op + OP_USBCMD, CMD_RS);
    for (i = 0u; i < 100000u; ++i) {
        if ((rd(g_op + OP_USBSTS) & STS_HCH) == 0u) {
            break;
        }
    }
    return 0;
}

/* 先要一个 slot：Enable Slot 命令（type 9），完成事件 DW3[31:24] 是 slot id */
static int enable_slot(void)
{
    int rc;
    uint32_t id;

    g_last_evt_dw3 = 0u;
    rc = cmd_submit(0u, 0u, 0u, (9u << TRB_TYPE_SHIFT));
    if (rc != 0) {
        return rc;
    }
    id = (g_last_evt_dw3 >> 24) & 0xFFu;
    if (id == 0u) {
        return -3;
    }
    g_slot = id;
    /* 这个 slot 的设备上下文地址要写进 DCBAA */
    *(uint64_t *)(g_dcbaa + g_slot * 8u) = (uint64_t)phys_of(g_dev_ctx);
    return 0;
}

/* 地址分配：填输入上下文 + Address Device 命令 */
static int address_device(uint32_t port, uint32_t speed)
{
    uint32_t *icc, *slot, *ep0;
    int rc;

    memzero(g_input_ctx, sizeof(g_input_ctx));

    /* ICC：Add Slot Context + Add EP0 */
    icc = in_icc();
    icc[1] = 0x3u;

    slot = in_slot();
    slot[0] = (speed << 20) | (1u << 27);          /* Context Entries = 1 */
    slot[1] = (port << 16);                        /* Root Hub Port Number */
    slot[2] = 0u;
    slot[3] = 0u;                                  /* Device Address = 0 */

    /* EP0 上下文（DCI = 1）：Control，MaxPacketSize 8 */
    ep0 = in_ep(1u);
    ep0[0] = 1u;                                   /* EP State = Running */
    ep0[1] = (4u << 3) | (3u << 1);                /* EP Type=Control[5:3]，CErr=3[2:1] */
    ep0[2] = phys_of(g_ep0_ring) | 1u;             /* TR Dequeue + DCS */
    ep0[3] = 0u;
    ep0[4] = (8u << 16);                           /* Max Packet Size 在 [31:16] */

    memzero(g_ep0_ring, sizeof(g_ep0_ring));
    g_ep0_enq = 0u;
    g_ep0_cycle = 1u;
    ring_link(g_ep0_ring, EP0_TRBS, g_ep0_cycle);

    rc = cmd_submit(phys_of(g_input_ctx), 0u, 0u,
                    (CMD_ADDRESS_DEV << TRB_TYPE_SHIFT) | (g_slot << 24));
    return rc;
}

/* 枚举：读设备描述符、配置描述符，找 HID 键盘接口与端点 */
static int enumerate_hid(uint32_t *out_iface, uint32_t *out_conf_value,
                         uint32_t *out_ep_addr, uint32_t *out_ep_mps,
                         uint32_t *out_ep_interval)
{
    uint8_t dev[18];
    uint8_t cfg[256];
    uint32_t total;
    uint32_t off;
    int rc;
    uint32_t iface = 0xFFFFFFFFu;
    uint32_t conf_value = 0u;
    uint32_t ep_addr = 0u;
    uint32_t ep_mps = 0u;
    uint32_t ep_interval = 0u;

    rc = ep0_transfer(0x80u, 6u, 0x0100u, 0u, 18u, (const uint8_t *)0, dev);
    if (rc != 0) {
        return rc;
    }

    rc = ep0_transfer(0x80u, 6u, 0x0200u, 0u, 9u, (const uint8_t *)0, cfg);
    if (rc != 0) {
        return rc;
    }
    total = (uint32_t)cfg[2] | ((uint32_t)cfg[3] << 8);
    if (total > sizeof(cfg)) {
        total = sizeof(cfg);
    }
    rc = ep0_transfer(0x80u, 6u, 0x0200u, 0u, (uint16_t)total,
                      (const uint8_t *)0, cfg);
    if (rc != 0) {
        return rc;
    }

    /* 走一遍描述符链：找 class 3 / subclass 1 / protocol 1 的接口 */
    off = 0u;
    while (off + 2u <= total) {
        uint32_t len = cfg[off];
        uint32_t type = cfg[off + 1u];
        if (len == 0u) {
            break;
        }
        if (type == 2u) {                              /* Configuration */
            conf_value = cfg[off + 5u];
        } else if (type == 4u && len >= 9u) {           /* Interface */
            if (cfg[off + 5u] == 3u && cfg[off + 6u] == 1u
                && cfg[off + 7u] == 1u) {
                iface = cfg[off + 2u];
            }
        } else if (type == 5u && len >= 7u) {           /* Endpoint */
            uint32_t addr = cfg[off + 2u];
            uint32_t attr = cfg[off + 3u] & 0x03u;
            if (iface != 0xFFFFFFFFu && attr == 3u
                && (addr & 0x80u) != 0u) {
                ep_addr = addr;
                ep_mps = (uint32_t)cfg[off + 4u]
                       | (((uint32_t)cfg[off + 5u] & 0x1Fu) << 8);
                ep_interval = cfg[off + 6u];
            }
        }
        off += len;
    }

    if (iface == 0xFFFFFFFFu || ep_addr == 0u) {
        return -2;                                    /* 不是键盘 */
    }
    *out_iface = iface;
    *out_conf_value = conf_value;
    *out_ep_addr = ep_addr;
    *out_ep_mps = ep_mps;
    *out_ep_interval = ep_interval;
    return 0;
}

/* 把中断端点加进设备上下文（Configure Endpoint） */
static int configure_interrupt_ep(uint32_t ep_addr, uint32_t mps, uint32_t interval)
{
    uint32_t dci = ((ep_addr & 0x0Fu) * 2u) + 1u;   /* IN 端点 */
    uint32_t *icc, *slot, *ep;

    memzero(g_input_ctx, sizeof(g_input_ctx));
    icc = in_icc();
    icc[1] = (1u << 0) | (1u << dci);               /* Add Slot + Add EP */

    slot = in_slot();
    slot[0] = (g_speed << 20) | (dci << 27);        /* Context Entries = dci */
    slot[1] = 1u << 16;

    ep = in_ep(dci);
    ep[0] = 1u | ((interval & 0xFFu) << 16);        /* Running + bInterval[23:16] */
    ep[1] = (7u << 3) | (3u << 1);                  /* EP Type=Interrupt IN[5:3]，CErr=3 */
    ep[2] = phys_of(g_intr_ring) | 1u;
    ep[3] = 0u;
    ep[4] = ((mps & 0xFFFFu) << 16) | 8u;          /* MPS[31:16] + 平均 TRB 长 8 */

    memzero(g_intr_ring, sizeof(g_intr_ring));
    g_intr_enq = 0u;
    g_intr_cycle = 1u;
    g_intr_out = 0u;
    g_intr_wrap = 0u;
    ring_link(g_intr_ring, INTR_TRBS, g_intr_cycle);
    g_dci = dci;
    g_ep_addr = ep_addr;

    return cmd_submit(phys_of(g_input_ctx), 0u, 0u,
                      (CMD_CONFIGURE_EP << TRB_TYPE_SHIFT) | (g_slot << 24));
}

/* 给中断端点挂一个 IN 传输：等 8 字节报告 */
/*
 * 中断端点环一次只挂一个 IN 传输，取走一个再挂下一个。键盘是低频设备，10ms 一份报告，
 * 完全够用，并发多个反而容易踩"多份报告共用缓冲"的坑。
 * 绕回时不碰 Link TRB 的 cycle：那个窗口里控制器可能正读着 Link，改了就是敲几个键之后彻底
 * 没反应。走到环尾就重新 Configure Endpoint，让控制器从全新的环首开始，代价是每 255 个键
 * 多发一条命令，能接受。
 */
static void intr_arm(void)
{
    uint32_t idx = g_intr_enq;

    if (idx >= INTR_TRBS - 1u) {
        /* 环用完了：排空后重配端点，重新拿到一个干净的环 */
        if (g_intr_out != 0u) {
            return;
        }
        if (configure_interrupt_ep(g_ep_addr, g_ep_mps_unused,
                                   g_ep_interval_unused) != 0) {
            g_ready = 0;
            return;
        }
        g_intr_out = 0u;     /* 端点被重置，之前挂着的传输都作废了 */
        idx = 0u;
    }

    trb_write(g_intr_ring, idx,
              (uint32_t)phys_of(g_report + (idx % 4u) * 8u), 0u, 8u,
              (TRB_NORMAL << TRB_TYPE_SHIFT) | TRB_IOC
                  | (TRB_CYCLE * g_intr_cycle));
    g_intr_enq = idx + 1u;
    ++g_intr_out;
    *(volatile uint32_t *)(g_bar + g_db + 4u * g_dci) = g_dci;
}

int xhci_init(void)
{
    pci_dev_t dev;
    uint64_t bar;
    uint32_t hcs1, caplen, i;
    uint32_t iface = 0u, conf_value = 0u, ep_addr = 0u;
    uint32_t ep_mps = 0u, ep_interval = 0u;
    int rc;

    g_ready = 0;
    g_err = 0;

    if (!pci_find_class(0x0Cu, 0x03u, &dev)) {
        g_err = 1;
        return -1;                       /* 没有 USB 控制器 */
    }
    bar = pci_bar_mmio64(&dev, 0u);
    if (bar == 0ull) {
        g_err = 2;
        return -1;
    }
    g_bar_low = (uint32_t)(bar & 0xFFFFFFFFull);
    if (paging_map_2mb_range(bar & ~0xFFFull, 0x200000u) == 0) {
        g_err = 3;
        return -1;
    }
    g_bar = (volatile uint8_t *)(uintptr_t)bar;
    pci_enable_mem_busmaster(&dev);

    caplen = (uint32_t)(*(volatile uint8_t *)(g_bar + XC_CAPLEN));
    g_op = caplen & 0xFFu;
    /* runtime 区开头是 MFINDEX(0x00)，第一个 interrupter 寄存器组从 +0x20 开始，
     * IMAN/IMOD/ERSTSZ/ERSTBA/ERDP 都相对它算。我第一版按 +0x00 算，ERSTSZ 写进了 MFINDEX，
     * 事件环压根没登记上，命令全部超时。 */
    g_rt = (rd(XC_RTSOFF) & 0xFFFFFFE0u) + 0x20u;
    g_db = rd(XC_DBOFF) & 0xFFFFFFF8u;

    hcs1 = rd(XC_HCSPARAMS1);
    g_max_slots = hcs1 & 0xFFu;
    g_max_ports = (hcs1 >> 24) & 0xFFu;
    if (g_max_slots == 0u) {
        g_max_slots = 1u;
    }

    if (setup_controller() != 0) {
        g_err = 4;
        return -1;
    }

    /* 找第一个有设备的端口 */
    {
        uint32_t port = 0u;
        for (i = 1u; i <= g_max_ports; ++i) {
            if ((rd(g_op + OP_PORT_BASE + (i - 1u) * OP_PORT_STRIDE)
                 & PORT_CCS) != 0u) {
                port = i;
                break;
            }
        }
        if (port == 0u) {
            g_err = 5;
            return -1;                   /* 没有插入设备 */
        }
        if (reset_port(port, &g_speed) != 0) {
            g_err = 6;
            return -1;
        }
        rc = enable_slot();
        if (rc != 0) {
            g_err = 12;
            return -1;
        }
        rc = address_device(port, g_speed);
        if (rc != 0) {
            g_err = 7;
            return -1;
        }
    }

    rc = enumerate_hid(&iface, &conf_value, &ep_addr, &ep_mps, &ep_interval);
    if (rc != 0) {
        g_err = 8;
        return -1;
    }

    /* SET_CONFIGURATION */
    rc = ep0_transfer(0x00u, 9u, (uint16_t)conf_value, 0u, 0u,
                      (const uint8_t *)0, (uint8_t *)0);
    if (rc != 0) {
        g_err = 9;
        return -1;
    }
    /* SET_PROTOCOL(boot)：让键盘发 8 字节定长报告 */
    rc = ep0_transfer(0x21u, 0x0Bu, 0u, (uint16_t)iface, 0u,
                      (const uint8_t *)0, (uint8_t *)0);
    if (rc != 0) {
        g_err = 10;
        return -1;
    }

    g_iface = iface;
    g_ep_addr = ep_addr;
    g_ep_mps_unused = ep_mps;
    g_ep_interval_unused = ep_interval;

    rc = configure_interrupt_ep(ep_addr, ep_mps, ep_interval);
    if (rc != 0) {
        g_err = 11;
        return -1;
    }
    intr_arm();
    g_ready = 1;
    return 0;
}

int usb_kbd_ready(void) { return g_ready; }

/* HID 报告 -> 字符 */
/* usage id 0x00..0x67。只填真正会产生字符的键，其余留 0。 */
static const char hid_map[104] = {
    [0x04] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd', [0x08] = 'e',
    [0x09] = 'f', [0x0A] = 'g', [0x0B] = 'h', [0x0C] = 'i', [0x0D] = 'j',
    [0x0E] = 'k', [0x0F] = 'l', [0x10] = 'm', [0x11] = 'n', [0x12] = 'o',
    [0x13] = 'p', [0x14] = 'q', [0x15] = 'r', [0x16] = 's', [0x17] = 't',
    [0x18] = 'u', [0x19] = 'v', [0x1A] = 'w', [0x1B] = 'x', [0x1C] = 'y',
    [0x1D] = 'z',
    [0x1E] = '1', [0x1F] = '2', [0x20] = '3', [0x21] = '4', [0x22] = '5',
    [0x23] = '6', [0x24] = '7', [0x25] = '8', [0x26] = '9', [0x27] = '0',
    [0x28] = '\n', [0x29] = 0, [0x2A] = '\b', [0x2B] = '\t', [0x2C] = ' ',
    [0x2D] = '-', [0x2E] = '=', [0x2F] = '[', [0x30] = ']', [0x31] = '\\',
    [0x32] = '`', [0x33] = ',', [0x34] = '.', [0x35] = '/', [0x36] = '\'',
    [0x37] = '*'
};

static const char hid_map_shift[104] = {
    [0x04] = 'A', [0x05] = 'B', [0x06] = 'C', [0x07] = 'D', [0x08] = 'E',
    [0x09] = 'F', [0x0A] = 'G', [0x0B] = 'H', [0x0C] = 'I', [0x0D] = 'J',
    [0x0E] = 'K', [0x0F] = 'L', [0x10] = 'M', [0x11] = 'N', [0x12] = 'O',
    [0x13] = 'P', [0x14] = 'Q', [0x15] = 'R', [0x16] = 'S', [0x17] = 'T',
    [0x18] = 'U', [0x19] = 'V', [0x1A] = 'W', [0x1B] = 'X', [0x1C] = 'Y',
    [0x1D] = 'Z',
    [0x1E] = '!', [0x1F] = '@', [0x20] = '#', [0x21] = '$', [0x22] = '%',
    [0x23] = '^', [0x24] = '&', [0x25] = '*', [0x26] = '(', [0x27] = ')',
    [0x2D] = '_', [0x2E] = '+', [0x2F] = '{', [0x30] = '}', [0x31] = '|',
    [0x32] = '~', [0x33] = '<', [0x34] = '>', [0x35] = '?', [0x36] = '"',
    [0x37] = '*'
};

static int has_key(const uint8_t *keys, uint8_t code)
{
    uint32_t i;
    for (i = 0u; i < 6u; ++i) {
        if (keys[i] == code) {
            return 1;
        }
    }
    return 0;
}

static void handle_report(const uint8_t *rep)
{
    const uint8_t mod = rep[0];
    uint8_t keys[6];
    const int shift = (mod & 0x22u) != 0u;
    const int ctrl = (mod & 0x11u) != 0u;
    uint32_t i;

    for (i = 0u; i < 6u; ++i) {
        keys[i] = rep[2u + i];
    }

    for (i = 0u; i < 6u; ++i) {
        uint8_t code = keys[i];
        char ch;

        if (code == 0u || has_key(g_keys, code)) {
            continue;                       /* 一直是按住的，不重复产生字符 */
        }
        g_last_key = code;
        ++g_presses;

        /* 方向键（HID usage 0x4F..0x52）走特殊键通道 */
        if (code >= 0x4Fu && code <= 0x52u) {
            uint8_t k = (uint8_t)KEY_NONE;
            if (code == 0x4Fu) { k = (uint8_t)KEY_RIGHT; }
            if (code == 0x50u) { k = (uint8_t)KEY_LEFT; }
            if (code == 0x51u) { k = (uint8_t)KEY_DOWN; }
            if (code == 0x52u) { k = (uint8_t)KEY_UP; }
            keyboard_feed_special(k);
            continue;
        }

        if (code >= 104u) {
            continue;
        }
        ch = shift ? hid_map_shift[code] : hid_map[code];
        if (ch == 0) {
            continue;
        }
        if (ctrl && (ch == 'c' || ch == 'C')) {
            keyboard_feed_ctrl_c();
            continue;
        }
        if (ctrl && (ch == 'z' || ch == 'Z')) {
            keyboard_feed_ctrl_z();
            continue;
        }
        ++g_chars;
        keyboard_feed_char(ch);
    }

    for (i = 0u; i < 6u; ++i) {
        g_keys[i] = keys[i];
    }
    g_mod = mod;
}

void usb_kbd_poll(void)
{
    uint32_t guard = 0u;

    if (!g_ready) {
        return;
    }

    while (evt_pending() && guard < 16u) {
        uint32_t type = evt_type();
        uint32_t trb = evt_param_lo();
        uint32_t code;

        ++guard;
        code = evt_status();
        evt_advance();
        if (type != TRB_EVT_TRANSFER) {
            continue;
        }
        if (g_intr_out != 0u) {
            --g_intr_out;
        }
        if (code == 1u) {
            uint32_t slot = (trb - (uint32_t)phys_of(g_intr_ring)) / 16u;
            ++g_reports;
            if (slot < INTR_TRBS) {
                handle_report(g_report + (slot % 4u) * 8u);
            }
        }
    }

    while (g_ready && g_intr_out < 4u) {
        intr_arm();                       /* 常驻 4 个，取走一个补一个 */
    }
}

/* 诊断 */
int      xhci_last_error(void)  { return g_err; }
uint32_t xhci_bar_low(void)     { return g_bar_low; }
uint32_t xhci_max_ports(void)   { return g_max_ports; }
uint32_t xhci_max_slots(void)   { return g_max_slots; }
uint32_t xhci_port_speed(void)  { return g_speed; }
uint32_t xhci_slot_id(void)     { return g_slot; }
uint32_t xhci_hid_ep_addr(void) { return g_ep_addr; }
uint32_t xhci_reports(void)     { return g_reports; }
void xhci_intr_state(uint32_t *enq, uint32_t *out, uint32_t *evt_deq)
{
    if (enq != (uint32_t *)0)     { *enq = g_intr_enq; }
    if (out != (uint32_t *)0)     { *out = g_intr_out; }
    if (evt_deq != (uint32_t *)0) { *evt_deq = g_evt_deq; }
}

uint32_t xhci_last_key(void)    { return g_last_key; }
