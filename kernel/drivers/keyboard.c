#include "keyboard.h"
#include "interrupt.h"
#include "io.h"

#define KBD_BUFFER_SIZE 128

static const char keymap_lower[128] = {
    0, 0x1B, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ',
};

static const char keymap_upper[128] = {
    0, 0x1B, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ',
};

static const int keymap_special[128] = {
    [0x47] = KEY_HOME,
    [0x48] = KEY_UP,
    [0x49] = KEY_PAGE_UP,
    [0x4B] = KEY_LEFT,
    [0x4D] = KEY_RIGHT,
    [0x4F] = KEY_END,
    [0x50] = KEY_DOWN,
    [0x51] = KEY_PAGE_DOWN,
    [0x52] = KEY_DELETE,
    [0x53] = KEY_DELETE,
    [0x3B] = KEY_F1,
    [0x3C] = KEY_F2,
    [0x3D] = KEY_F3,
    [0x3E] = KEY_F4,
    [0x3F] = KEY_F5,
    [0x40] = KEY_F6,
    [0x41] = KEY_F7,
    [0x42] = KEY_F8,
    [0x43] = KEY_F9,
    [0x44] = KEY_F10,
    [0x57] = KEY_F11,
    [0x58] = KEY_F12,
};

static volatile int buffer[KBD_BUFFER_SIZE];
static volatile u32 buffer_head;
static volatile u32 buffer_tail;
static volatile u8 modifiers;
static volatile int extended;

static void buffer_push(int key)
{
    u32 next = (buffer_head + 1) % KBD_BUFFER_SIZE;
    if (next == buffer_tail) {
        return;
    }
    buffer[buffer_head] = key;
    buffer_head = next;
}

static void keyboard_irq(struct registers *regs)
{
    UNUSED(regs);

    u8 scancode = inb(0x60);

    if (scancode == 0xE0) {
        extended = 1;
        return;
    }

    int released = (scancode & 0x80) != 0;
    u8 code = (u8)(scancode & 0x7F);

    if (code == 0x2A || code == 0x36) {
        if (released) {
            modifiers &= (u8)~KEY_MOD_SHIFT;
        } else {
            modifiers |= KEY_MOD_SHIFT;
        }
        extended = 0;
        return;
    }
    if (code == 0x1D || code == 0xE0) {
        if (released) {
            modifiers &= (u8)~KEY_MOD_CTRL;
        } else {
            modifiers |= KEY_MOD_CTRL;
        }
        extended = 0;
        return;
    }
    if (code == 0x38) {
        if (released) {
            modifiers &= (u8)~KEY_MOD_ALT;
        } else {
            modifiers |= KEY_MOD_ALT;
        }
        extended = 0;
        return;
    }
    if (code == 0x3A && !released) {
        modifiers ^= KEY_MOD_CAPS;
        extended = 0;
        return;
    }

    if (released) {
        extended = 0;
        return;
    }

    int key = 0;
    if (extended) {
        key = keymap_special[code];
        extended = 0;
    } else if (code < 128) {
        int shifted = (modifiers & KEY_MOD_SHIFT) != 0;
        int caps = (modifiers & KEY_MOD_CAPS) != 0;
        char ch = shifted ? keymap_upper[code] : keymap_lower[code];
        if (caps && ch >= 'a' && ch <= 'z') {
            ch = (char)(ch - 'a' + 'A');
        } else if (caps && ch >= 'A' && ch <= 'Z') {
            ch = (char)(ch - 'A' + 'a');
        }
        key = (int)(u8)ch;
    }

    if (key) {
        buffer_push(key);
    }
}

void keyboard_init(void)
{
    while (inb(0x64) & 1) {
        inb(0x60);
    }
    irq_register(1, keyboard_irq);
    irq_unmask(1);
}

int keyboard_has_input(void)
{
    return buffer_head != buffer_tail;
}

int keyboard_getchar(void)
{
    while (buffer_head == buffer_tail) {
        cpu_halt();
    }
    int key = buffer[buffer_tail];
    buffer_tail = (buffer_tail + 1) % KBD_BUFFER_SIZE;
    return key;
}

u8 keyboard_modifiers(void)
{
    return modifiers;
}
