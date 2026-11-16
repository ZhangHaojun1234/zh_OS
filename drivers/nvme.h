/* nvme.h - M32: NVMe（PCIe SSD）驱动的接口 */

#ifndef ZH_OS_NVME_H
#define ZH_OS_NVME_H

#include <stdint.h>

/* 初始化：找控制器 -> 映射 BAR0 -> 复位/使能 -> 建 Admin 队列 ->
 * Identify -> 建一对 I/O 队列。成功返回 0。 */
int nvme_init(void);

/* 控制器有没有起来、能不能读写扇区 */
int nvme_ready(void);

/* 一扇区的读写。成功返回 0。走内部对齐缓冲，所以调用方不用管跨页问题。 */
int nvme_read_sector(uint32_t lba, uint8_t *buf);
int nvme_write_sector(uint32_t lba, const uint8_t *buf);

/* 供 shell 看的几何信息 */
uint32_t nvme_bar_low(void);
uint32_t nvme_version(void);
uint32_t nvme_namespace(void);
uint32_t nvme_block_size(void);
uint32_t nvme_sector_count(void);
const char *nvme_model(void);

#endif /* ZH_OS_NVME_H */
