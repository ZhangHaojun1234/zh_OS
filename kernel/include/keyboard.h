/* ---------------------------------------------------------------
 * PS/2 键盘 (扫描码集 1)
 * --------------------------------------------------------------- */
#ifndef ZH_KEYBOARD_H
#define ZH_KEYBOARD_H

#include "types.h"

#define KEY_NONE        0
#define KEY_ESCAPE      0x1B
#define KEY_BACKSPACE   '\b'
#define KEY_TAB         '\t'
#define KEY_ENTER       '\n'
#define KEY_UP          0x100
#define KEY_DOWN        0x101
#define KEY_LEFT        0x102
#define KEY_RIGHT       0x103
#define KEY_HOME        0x104
#define KEY_END         0x105
#define KEY_PAGE_UP     0x106
#define KEY_PAGE_DOWN   0x107
#define KEY_DELETE      0x108
#define KEY_F1          0x11A
#define KEY_F2          0x11B
#define KEY_F3          0x11C
#define KEY_F4          0x11D
#define KEY_F5          0x11E
#define KEY_F6          0x11F
#define KEY_F7          0x120
#define KEY_F8          0x121
#define KEY_F9          0x122
#define KEY_F10         0x123
#define KEY_F11         0x124
#define KEY_F12         0x125

void keyboard_init(void);
int keyboard_getchar(void);
int keyboard_has_input(void);
u8 keyboard_modifiers(void);

#define KEY_MOD_SHIFT   0x01
#define KEY_MOD_CTRL    0x02
#define KEY_MOD_ALT     0x04
#define KEY_MOD_CAPS    0x08

#endif
