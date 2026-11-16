/* ============================================================================
 * zh_OS 1.0 (x86_64)
 * File:    drivers/pci.h
 * Version: 1.0
 * Brief:   [ADD] M30：PCI 配置空间访问与设备枚举。
 * ============================================================================ */

#ifndef ZH_OS_PCI_H
#define ZH_OS_PCI_H

#include <stdint.h>

/* 配置空间访问方式：老式的 0xCF8(地址) / 0xCFC(数据) 一对端口。
 * 真机、QEMU 都支持；ACPI 的 ECAM(MMIO) 更快，但要多解析一张 MCFG 表，
 * 这里先用端口版本 —— 枚举几十个设备够快，而且不依赖 ACPI。 */
#define PCI_CONFIG_ADDR 0xCF8u
#define PCI_CONFIG_DATA 0xCFCu

/* 常见 class / subclass */
#define PCI_CLASS_STORAGE   0x01u
#define PCI_SUBCLASS_IDE    0x01u
#define PCI_SUBCLASS_SATA   0x06u    /* AHCI */
#define PCI_SUBCLASS_NVME   0x08u

typedef struct {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor;
    uint16_t device;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  header_type;
} pci_dev_t;

/* 一次 32 位的配置空间读 / 写。off 必须是 4 字节对齐。 */
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void     pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off,
                            uint32_t value);

/* 扫描总线 0..255（遇多功能设备会展开 func），最多填 max 个。
 * 返回实际找到的设备数。 */
uint32_t pci_scan(pci_dev_t *list, uint32_t max);

/* 按 class/subclass 找第一个设备，找到返回 1。 */
int pci_find_class(uint8_t cls, uint8_t sub, pci_dev_t *out);

/* 读 BAR（只用 MMIO 型 BAR：bit0=0）并去掉低 4 位属性。
 * 返回 0 表示这个 BAR 不存在或不是 MMIO。 */
uint32_t pci_bar_mmio(const pci_dev_t *dev, uint8_t index);

/* 64 位版本的 BAR 读法：NVMe 的 BAR0 常常在 4GB 以上，只看低 32 位会错。
 * 非 64 位 BAR 时高 32 位返回 0。 */
uint64_t pci_bar_mmio64(const pci_dev_t *dev, uint8_t index);

/* 打开 IO / MMIO / Bus Master（命令寄存器 bit0/1/2）。
 * AHCI 的 DMA 必须开 bus master，否则命令表读进来全是 0。 */
void pci_enable_mem_busmaster(const pci_dev_t *dev);

#endif /* ZH_OS_PCI_H */
