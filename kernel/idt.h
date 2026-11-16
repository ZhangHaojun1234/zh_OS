/* idt.h - IDT 门结构、IRQ 向量宏、接口声明 */

#ifndef ZH_OS_IDT_H
#define ZH_OS_IDT_H

#include <stdint.h>

#define IDT_ENTRIES 256
#define IRQ_VECTOR_BASE 32
#define IRQ_VECTOR_END  47

/* 中断门属性：P=1, DPL=0, 类型 0xE(64 位中断门)
 * 中断门进入时会自动清 IF，处理期间不被同级中断打断。 */
#define IDT_GATE_INTERRUPT 0x8E

void idt_init(void);
void idt_set_gate(uint8_t vector, uint64_t handler, uint8_t ist, uint8_t type_attr);

#endif /* ZH_OS_IDT_H */
