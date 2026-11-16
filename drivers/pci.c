/* ============================================================================
 * zh_OS 1.0 (x86_64)
 * File:    drivers/pci.c
 * Version: 1.0
 * Brief:   [ADD] M30：PCI 配置空间（0xCF8/0xCFC）与设备枚举。
 * ============================================================================ */

#include "pci.h"

#include <stdint.h>

static inline void pci_outl(uint16_t port, uint32_t value)
{
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t pci_inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void pci_outw(uint16_t port, uint16_t value)
{
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t pci_inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* 配置空间地址寄存器格式：
 *   bit31          = 1（使能）
 *   bit30..24      = 保留
 *   bit23..16      = bus
 *   bit15..11      = slot(device)
 *   bit10..8       = func
 *   bit7..2        = offset（4 字节对齐，所以低 2 位是 0）
 */
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16u)
                  | ((uint32_t)(slot & 0x1Fu) << 11u)
                  | ((uint32_t)(func & 0x07u) << 8u)
                  | ((uint32_t)off & 0xFCu);
    pci_outl(PCI_CONFIG_ADDR, addr);
    return pci_inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    uint32_t v = pci_config_read32(bus, slot, func, (uint8_t)(off & 0xFCu));
    uint16_t r = (uint16_t)((v >> ((off & 2u) ? 16u : 0u)) & 0xFFFFu);
    return r;
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off,
                        uint32_t value)
{
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16u)
                  | ((uint32_t)(slot & 0x1Fu) << 11u)
                  | ((uint32_t)(func & 0x07u) << 8u)
                  | ((uint32_t)off & 0xFCu);
    pci_outl(PCI_CONFIG_ADDR, addr);
    pci_outl(PCI_CONFIG_DATA, value);
}

static void pci_fill(pci_dev_t *d, uint8_t bus, uint8_t slot, uint8_t func)
{
    uint32_t id = pci_config_read32(bus, slot, func, 0x00u);
    uint32_t cls = pci_config_read32(bus, slot, func, 0x08u);
    uint32_t hdr = pci_config_read32(bus, slot, func, 0x0Cu);

    d->bus = bus;
    d->slot = slot;
    d->func = func;
    d->vendor = (uint16_t)(id & 0xFFFFu);
    d->device = (uint16_t)(id >> 16u);
    d->class_code = (uint8_t)((cls >> 24u) & 0xFFu);
    d->subclass = (uint8_t)((cls >> 16u) & 0xFFu);
    d->prog_if = (uint8_t)((cls >> 8u) & 0xFFu);
    d->header_type = (uint8_t)((hdr >> 16u) & 0xFFu);
}

uint32_t pci_scan(pci_dev_t *list, uint32_t max)
{
    uint32_t found = 0u;
    uint32_t bus, slot, func;

    for (bus = 0u; bus < 256u && found < max; ++bus) {
        for (slot = 0u; slot < 32u && found < max; ++slot) {
            uint32_t id = pci_config_read32((uint8_t)bus, (uint8_t)slot, 0u, 0x00u);
            if ((id & 0xFFFFu) == 0xFFFFu) {
                continue;                      /* 空槽：读回全 1 */
            }
            pci_fill(&list[found], (uint8_t)bus, (uint8_t)slot, 0u);
            ++found;

            /* 多功能设备：header_type 的 bit7 置位时 func 1..7 也要扫 */
            if ((list[found - 1u].header_type & 0x80u) != 0u && found < max) {
                for (func = 1u; func < 8u && found < max; ++func) {
                    uint32_t idf = pci_config_read32((uint8_t)bus, (uint8_t)slot,
                                                     (uint8_t)func, 0x00u);
                    if ((idf & 0xFFFFu) == 0xFFFFu) {
                        continue;
                    }
                    pci_fill(&list[found], (uint8_t)bus, (uint8_t)slot, (uint8_t)func);
                    ++found;
                }
            }
        }
    }
    return found;
}

uint64_t pci_bar_mmio64(const pci_dev_t *dev, uint8_t index)
{
    uint32_t lo, hi;

    if (dev == (const pci_dev_t *)0 || index > 5u) {
        return 0ull;
    }
    if ((dev->header_type & 0x7Fu) != 0x00u) {
        return 0ull;
    }
    lo = pci_config_read32(dev->bus, dev->slot, dev->func,
                           (uint8_t)(0x10u + index * 4u));
    if ((lo & 0x01u) != 0u) {
        return 0ull;                    /* IO 空间 BAR，不是 MMIO */
    }
    hi = 0u;
    if ((lo & 0x06u) == 0x04u) {        /* 64 位 BAR：高 32 位在下一个 */
        hi = pci_config_read32(dev->bus, dev->slot, dev->func,
                               (uint8_t)(0x10u + (index + 1u) * 4u));
    }
    return ((uint64_t)hi << 32) | (uint64_t)(lo & 0xFFFFFFF0u);
}

uint32_t pci_bar_mmio(const pci_dev_t *dev, uint8_t index)
{
    uint32_t bar;
    uint32_t cls;

    if (dev == (const pci_dev_t *)0 || index > 5u) {
        return 0u;
    }
    /* 只有普通 header_type=0 的设备/BAR 布局是标准的；PCI-to-PCI 桥不碰 */
    if ((dev->header_type & 0x7Fu) != 0x00u) {
        return 0u;
    }
    (void)cls;
    bar = pci_config_read32(dev->bus, dev->slot, dev->func,
                            (uint8_t)(0x10u + index * 4u));
    if ((bar & 0x01u) != 0u) {          /* bit0=1 表示这是 IO 空间 */
        return 0u;
    }
    if ((bar & 0x06u) == 0x04u) {       /* 64 位 BAR：低 4 位是属性，高位在下一个 */
        uint32_t hi = pci_config_read32(dev->bus, dev->slot, dev->func,
                                        (uint8_t)(0x10u + (index + 1u) * 4u));
        (void)hi;                        /* 真机大 BAR 的高 32 位这里用不到 */
    }
    return bar & 0xFFFFFFF0u;
}

void pci_enable_mem_busmaster(const pci_dev_t *dev)
{
    uint32_t cmd;

    if (dev == (const pci_dev_t *)0) {
        return;
    }
    cmd = pci_config_read32(dev->bus, dev->slot, dev->func, 0x04u);
    cmd |= 0x07u;                       /* IO space + Memory space + Bus Master */
    pci_config_write32(dev->bus, dev->slot, dev->func, 0x04u, cmd);
}

int pci_find_class(uint8_t cls, uint8_t sub, pci_dev_t *out)
{
    pci_dev_t list[16];
    uint32_t n, i;

    n = pci_scan(list, 16u);
    for (i = 0u; i < n; ++i) {
        if (list[i].class_code == cls && list[i].subclass == sub) {
            if (out != (pci_dev_t *)0) {
                *out = list[i];
            }
            return 1;
        }
    }
    return 0;
}
