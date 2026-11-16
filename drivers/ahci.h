/* ahci.h - M31: AHCI（SATA）控制器的接口 */

#ifndef ZH_OS_AHCI_H
#define ZH_OS_AHCI_H

#include <stdint.h>

/* 找控制器 -> 映射 ABAR -> 使能 -> 复位/启动端口。
 * 返回 0 表示成功找到并起了至少一个 SATA 端口，-1 表示没有控制器。 */
int ahci_init(void);

/* 初始化过且有可用端口时为 1。ata_read/write_sector 靠它决定走不走 AHCI。 */
int ahci_ready(void);

/* 一个扇区（512 字节）。返回 0 成功，负数是错误。 */
int ahci_read_sector(uint32_t lba, uint8_t *buf);
int ahci_write_sector(uint32_t lba, const uint8_t *buf);

/* 供 shell 打印用 */
uint32_t ahci_abar(void);
uint32_t ahci_cap(void);
uint32_t ahci_ports_implemented(void);
uint32_t ahci_port_number(void);      /* 实际在用的那个端口号 */
uint32_t ahci_port_signature(void);
uint32_t ahci_port_sig_of(uint32_t i);
uint32_t ahci_port_ssts_of(uint32_t i);
uint32_t ahci_port_cmd_of(uint32_t i);
uint32_t ahci_map_status(void);

#endif /* ZH_OS_AHCI_H */
