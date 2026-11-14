/* ============================================================================
 * zh_OS 0.1beta (x86_64)
 * File:    drivers/keyboard.h
 * Version: 0.1beta
 * Brief:   键盘驱动接口声明。
 * ============================================================================ */

#ifndef ZH_OS_KEYBOARD_H
#define ZH_OS_KEYBOARD_H

#include <stdint.h>

/* 初始化键盘：清空缓冲区、打开 IRQ1。调用前需已 pic_init 并加载 IDT。 */
void keyboard_init(void);

/* 由中断分发器在 IRQ1 时调用：只读取扫描码并入队，不做阻塞操作。 */
void keyboard_handle_irq(void);

/* 阻塞读取一个字符（缓冲区空时 hlt 等待中断）。 */
char keyboard_getchar(void);

/* 非阻塞探测：缓冲区有字符返回 1。 */
int keyboard_haschar(void);

/* [ADD] 扩展键（方向键）特殊事件：扫描码集 1 中 0xE0 前缀后的按下码 */
enum keyboard_special_key {
    KEY_NONE  = 0,
    KEY_UP    = 0x48,
    KEY_DOWN  = 0x50,
    KEY_LEFT  = 0x4B,
    KEY_RIGHT = 0x4D
};

/* [ADD] 独立特殊键环形缓冲（容量 8）的探测与读取；无事件返回 KEY_NONE */
int keyboard_has_special(void); // [ADD]
uint8_t keyboard_get_special(void); // [ADD]

#endif /* ZH_OS_KEYBOARD_H */
