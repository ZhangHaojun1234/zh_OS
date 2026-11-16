/* ============================================================================
 * zh_OS 1.0 (x86_64)
 * File:    drivers/keyboard.h
 * Version: 1.0
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

/* [ADD] M33：USB HID 键盘用。xhci 驱动解析出字符后从这里入队，
 * 走的还是同一个环形缓冲，上层 ring3 shell 完全察觉不到换了一个键盘。 */
void keyboard_feed_char(char ch);
void keyboard_feed_special(uint8_t scancode);
void keyboard_feed_ctrl_c(void);
void keyboard_feed_ctrl_z(void);

/* [ADD] M17：刚刚有没有按 Ctrl-C（读后清零）。非阻塞，给 isr 的 IRQ1 用。 */
int keyboard_take_ctrl_c(void);

/* [ADD] M18：刚刚有没有按 Ctrl-Z（读后清零）。IRQ1 把它变成 SIGTSTP。 */
int keyboard_take_ctrl_z(void);

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
