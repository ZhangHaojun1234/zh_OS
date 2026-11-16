/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    kernel/idt.c
 * Version: 0.2beta
 * Brief:   256 项 64 位 IDT 门、idt_set_gate 与 lidt。
 * ============================================================================ */

#include "idt.h"

#include "gdt.h"

/*
 * 64 位 IDT 门描述符，共 16 字节：
 *   offset_low  : 处理函数地址低 16 位
 *   selector    : 代码段选择子（内核代码段 0x08）
 *   ist         : 中断栈表索引（0 表示不使用 IST，沿用当前栈）
 *   type_attr   : P / DPL / 门类型
 *   offset_mid  : 处理函数地址 16..31 位
 *   offset_high : 处理函数地址 32..63 位
 *   zero        : 保留
 */
struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idt_ptr;

/* 由 kernel/interrupt.asm 提供：48 个 stub 地址（0-31 异常，32-47 IRQ） */
extern uint64_t isr_stub_table[];

void idt_set_gate(uint8_t vector, uint64_t handler, uint8_t ist, uint8_t type_attr)
{
    idt[vector].offset_low = (uint16_t)(handler & 0xFFFFu);
    idt[vector].selector = (uint16_t)GDT_KERNEL_CODE;
    idt[vector].ist = (uint8_t)(ist & 0x07u);
    idt[vector].type_attr = type_attr;
    idt[vector].offset_mid = (uint16_t)((handler >> 16) & 0xFFFFu);
    idt[vector].offset_high = (uint32_t)(handler >> 32);
    idt[vector].zero = 0;
}

void idt_init(void)
{
    /* 先把 256 个门清零（P=0 表示未安装） */
    for (uint32_t i = 0; i < IDT_ENTRIES; ++i) {
        idt[i].offset_low = 0;
        idt[i].selector = 0;
        idt[i].ist = 0;
        idt[i].type_attr = 0;
        idt[i].offset_mid = 0;
        idt[i].offset_high = 0;
        idt[i].zero = 0;
    }

    /* 0..31 异常，32..47 IRQ，全部指向 interrupt.asm 中的 stub */
    for (uint32_t v = 0; v <= IRQ_VECTOR_END; ++v) {
        idt_set_gate((uint8_t)v, isr_stub_table[v], 0, IDT_GATE_INTERRUPT);
    }

    idt_ptr.limit = (uint16_t)(sizeof(idt) - 1);
    idt_ptr.base = (uint64_t)&idt[0];

    __asm__ volatile("lidt %0" : : "m"(idt_ptr));
}
