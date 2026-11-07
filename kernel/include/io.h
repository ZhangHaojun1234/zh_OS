/* ---------------------------------------------------------------
 * x86 端口 I/O
 * --------------------------------------------------------------- */
#ifndef ZH_IO_H
#define ZH_IO_H

#include "types.h"

static inline void outb(u16 port, u8 value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline u8 inb(u16 port)
{
    u8 value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(u16 port, u16 value)
{
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline u16 inw(u16 port)
{
    u16 value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outl(u16 port, u32 value)
{
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline u32 inl(u16 port)
{
    u32 value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void io_wait(void)
{
    outb(0x80, 0);
}

static inline void cpu_halt(void)
{
    __asm__ volatile("hlt");
}

static inline void cpu_cli(void)
{
    __asm__ volatile("cli");
}

static inline void cpu_sti(void)
{
    __asm__ volatile("sti");
}

static inline u32 irq_save(void)
{
    u32 flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(u32 flags)
{
    __asm__ volatile("pushl %0; popfl" : : "r"(flags) : "memory", "cc");
}

#endif
