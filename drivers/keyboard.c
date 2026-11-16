/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    drivers/keyboard.c
 * Version: 0.2beta
 * Brief:   PS/2 键盘 IRQ1 驱动：扫描码映射与 256 字节环形缓冲。
 * ============================================================================ */

#include "keyboard.h"

#include <stdbool.h>  // [ADD]
#include "pic.h"

/*
 * PS/2 键盘驱动（扫描码集 1）
 *
 * 设计要点：
 *   - 中断处理函数 keyboard_handle_irq() 只做两件事：读端口 0x60、把字符放进
 *     256 字节环形缓冲区。绝不阻塞、绝不调用可能睡眠的函数。
 *   - 消费侧 keyboard_getchar() 在缓冲区为空时用 hlt 等待下一次中断，
 *     而不是忙等（busy-wait），这样 CPU 在空闲时真正停下来。
 *   - 缓冲区满时丢弃新字符，避免覆盖尚未读取的数据。
 */

#define KBD_DATA_PORT   0x60
#define KBD_STATUS_PORT 0x64

#define KBD_BUFFER_SIZE 256

/* 扫描码集 1：按下（make）码表，0 表示该键不产生可打印字符 */
static const char keymap[128] = {
    0,    0,    '1',  '2',  '3',  '4',  '5',  '6',  /* 0x00 - 0x07 */
    '7',  '8',  '9',  '0',  '-',  '=',  '\b', '\t', /* 0x08 - 0x0F */
    'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',  /* 0x10 - 0x17 */
    'o',  'p',  '[',  ']',  '\n', 0,    'a',  's',  /* 0x18 - 0x1F */
    'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',  /* 0x20 - 0x27 */
    '\'', '`',  0,    '\\', 'z',  'x',  'c',  'v',  /* 0x28 - 0x2F */
    'b',  'n',  'm',  ',',  '.',  '/',  0,    '*',  /* 0x30 - 0x37 */
    0,    ' ',  0,    0,    0,    0,    0,    0,    /* 0x38 - 0x3F */
};

/* Shift 按下时的字符映射（仅覆盖有差异的键） */
static const char keymap_shift[128] = {
    0,    0,    '!',  '@',  '#',  '$',  '%',  '^',  /* 0x00 - 0x07 */
    '&',  '*',  '(',  ')',  '_',  '+',  '\b', '\t', /* 0x08 - 0x0F */
    'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',  /* 0x10 - 0x17 */
    'O',  'P',  '{',  '}',  '\n', 0,    'A',  'S',  /* 0x18 - 0x1F */
    'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':',  /* 0x20 - 0x27 */
    '"',  '~',  0,    '|',  'Z',  'X',  'C',  'V',  /* 0x28 - 0x2F */
    'B',  'N',  'M',  '<',  '>',  '?',  0,    '*',  /* 0x30 - 0x37 */
    0,    ' ',  0,    0,    0,    0,    0,    0,    /* 0x38 - 0x3F */
};

#define SC_LCTRL        0x1D   /* [ADD] M17：Ctrl-C 用 */
#define SC_LSHIFT       0x2A
#define SC_RSHIFT       0x36
#define SC_LSHIFT_REL   0xAA
#define SC_RSHIFT_REL   0xB6
#define SC_CAPSLOCK     0x3A
#define SC_EXTENDED     0xE0

static volatile uint8_t buffer[KBD_BUFFER_SIZE];
static volatile uint16_t buffer_head; /* 写入位置（中断侧） */
static volatile uint16_t buffer_tail; /* 读取位置（消费侧） */

static int shift_left;
static int ctrl_down;      /* [ADD] M17 */
static volatile int ctrl_c_flag;   /* [ADD] M17：键盘中断里置位，isr 里取走 */
static volatile int ctrl_z_flag;   /* [ADD] M18：同上，对应 Ctrl-Z */
static int shift_right;
static int caps_lock;
static int extended;

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void buffer_push(char ch)
{
    const uint16_t next = (uint16_t)((buffer_head + 1) % KBD_BUFFER_SIZE);

    if (next == buffer_tail) {
        /* 缓冲区已满：丢弃新字符，保留尚未处理的旧字符 */
        return;
    }
    buffer[buffer_head] = (uint8_t)ch;
    buffer_head = next;
}

static int buffer_pop(char *out)
{
    if (buffer_tail == buffer_head) {
        return 0;
    }
    *out = (char)buffer[buffer_tail];
    buffer_tail = (uint16_t)((buffer_tail + 1) % KBD_BUFFER_SIZE);
    return 1;
}

/* [ADD] 独立特殊键环形缓冲（容量 8），只保存方向键的按下码 */
#define SPECIAL_BUFFER_SIZE 8 // [ADD]
static volatile uint8_t special_buffer[SPECIAL_BUFFER_SIZE]; // [ADD]
static volatile uint16_t special_head; // [ADD]
static volatile uint16_t special_tail; // [ADD]

/* [ADD] 由 IRQ1 的 0xE0 前缀分支调用：忽略释放码与非方向键扩展码 */
static void keyboard_push_special(uint8_t scancode) // [ADD]
{
    if ((scancode & 0x80u) != 0u) {
        return;
    }
    if (scancode != KEY_UP && scancode != KEY_DOWN &&
        scancode != KEY_LEFT && scancode != KEY_RIGHT) {
        return;
    }
    const uint16_t next = (uint16_t)((special_head + 1u) % SPECIAL_BUFFER_SIZE);
    if (next == special_tail) {
        return;
    }
    special_buffer[special_head] = scancode;
    special_head = next;
}

int keyboard_has_special(void) // [ADD]
{
    return special_tail != special_head;
}

uint8_t keyboard_get_special(void) // [ADD]
{
    uint8_t key;

    if (special_tail == special_head) {
        return (uint8_t)KEY_NONE;
    }
    key = special_buffer[special_tail];
    special_tail = (uint16_t)((special_tail + 1u) % SPECIAL_BUFFER_SIZE);
    return key;
}

void keyboard_handle_irq(void)
{
    const uint8_t scancode = inb(KBD_DATA_PORT);

    /* [ADD] 扩展键改由特殊键缓冲处理；下方原有 0xE0 块一行不动 */
    if (scancode == 0xE0) { extended = true; return; } // [ADD]
    if (extended) { extended = false; keyboard_push_special(scancode); return; } // [ADD]

    /* 0xE0 前缀表示扩展键（方向键等），本步不处理，跳过其后续字节 */
    if (scancode == SC_EXTENDED) {
        extended = 1;
        return;
    }
    if (extended) {
        extended = 0;
        return;
    }

    /* 最高位为 1 表示释放（break）码 */
    const int released = (scancode & 0x80) != 0;
    const uint8_t code = (uint8_t)(scancode & 0x7Fu);

    switch (code) {
    case SC_LCTRL:                 /* [ADD] M17 */
        ctrl_down = !released;
        return;
    case SC_LSHIFT:
        shift_left = !released;
        return;
    case SC_RSHIFT:
        shift_right = !released;
        return;
    case SC_CAPSLOCK:
        if (!released) {
            caps_lock = !caps_lock;
        }
        return;
    default:
        break;
    }

    if (released) {
        return;
    }

    const int shift = shift_left || shift_right;
    const uint8_t idx = (uint8_t)(code & 0x7Fu);
    char ch = keymap[idx];

    if (ch == 0) {
        return;
    }

    /*
     * CapsLock 只影响字母；Shift 与 CapsLock 同时按下时按 XOR 处理，
     * 避免先取大写映射、再翻转一次导致大小写颠倒。
     * 非字母字符只受 Shift 影响（数字 -> 符号等）。
     */
    /* [ADD] M17：Ctrl-C 不当普通字符送出去，而是变成一个"打断"事件 */
    if (ctrl_down && (ch == 'c' || ch == 'C')) {
        ctrl_c_flag = 1;
        return;
    }
    /* [ADD] M18：Ctrl-Z 同理，变成一个"停住前台作业"的事件（SIGTSTP） */
    if (ctrl_down && (ch == 'z' || ch == 'Z')) {
        ctrl_z_flag = 1;
        return;
    }

    const int is_alpha = (ch >= 'a' && ch <= 'z');
    const int upper = is_alpha ? (shift ^ caps_lock) : shift;
    if (upper) {
        ch = keymap_shift[idx];
    }

    buffer_push(ch);
}

char keyboard_getchar(void)
{
    char ch = 0;

    for (;;) {
        if (buffer_pop(&ch)) {
            return ch;
        }
        /* 缓冲区为空：开中断并 hlt，等下一次 IRQ 唤醒（不忙等） */
        __asm__ volatile("sti; hlt");
    }
}

/* [ADD] M17：取走"刚按过 Ctrl-C"这个事件（读后清零，非阻塞）。 */
int keyboard_take_ctrl_c(void)
{
    int was = ctrl_c_flag;
    ctrl_c_flag = 0;
    return was;
}

/* [ADD] M18：取走"刚按过 Ctrl-Z"这个事件（读后清零，非阻塞）。 */
int keyboard_take_ctrl_z(void)
{
    int was = ctrl_z_flag;
    ctrl_z_flag = 0;
    return was;
}

int keyboard_haschar(void)
{
    return buffer_tail != buffer_head;
}

void keyboard_init(void)
{
    buffer_head = 0;
    buffer_tail = 0;
    shift_left = 0;
    ctrl_down = 0;   /* [ADD] M17 */
    shift_right = 0;
    caps_lock = 0;
    extended = 0;

    /* 清空键盘输出缓冲区里可能存在的残留字节（最多 16 次，避免死循环） */
    for (int i = 0; i < 16; ++i) {
        if ((inb(KBD_STATUS_PORT) & 0x01u) == 0) {
            break;
        }
        (void)inb(KBD_DATA_PORT);
    }

    pic_clear_mask(1); /* 打开 IRQ1 */
}
