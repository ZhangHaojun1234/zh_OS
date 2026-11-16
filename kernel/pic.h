/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    kernel/pic.h
 * Version: 0.2beta
 * Brief:   PIC 接口声明。
 * ============================================================================ */

#ifndef ZH_OS_PIC_H
#define ZH_OS_PIC_H

#include <stdint.h>

/* 8259A 主片/从片端口 */
#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

#define PIC_EOI 0x20

/* 把 IRQ0..15 重映射到中断向量 32..47，并屏蔽全部 IRQ 之外的无用线 */
void pic_init(void);

/* 向主/从片发送 EOI（End Of Interrupt） */
void pic_send_eoi(uint8_t irq);

/* 设置某一根 IRQ 的屏蔽位（irq 取 0..15） */
void pic_set_mask(uint8_t irq);

/* 清除某一根 IRQ 的屏蔽位（irq 取 0..15） */
void pic_clear_mask(uint8_t irq);

#endif /* ZH_OS_PIC_H */
