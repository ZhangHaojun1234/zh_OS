#include "line.h"
#include "console.h"
#include "keyboard.h"

int line_read(char *buffer, u32 size, int flags)
{
    u32 length = 0;
    int mask = (flags & LINE_ECHO_MASK) != 0;

    if (size == 0) {
        return 0;
    }

    for (;;) {
        int key = keyboard_getchar();

        if (key == KEY_ENTER) {
            console_putc('\n');
            buffer[length] = '\0';
            return (int)length;
        }
        if (key == KEY_BACKSPACE) {
            if (length > 0) {
                length--;
                console_backspace();
            }
            continue;
        }
        if (key == 0x03) {              /* Ctrl+C */
            console_write("^C\n");
            buffer[length] = '\0';
            return -1;
        }
        if (key >= 0x100) {
            continue;
        }
        if (key == KEY_TAB) {
            continue;
        }
        if (key < 0x20 || key > 0x7E) {
            continue;
        }
        if (length + 1 >= size) {
            continue;
        }

        buffer[length++] = (char)key;
        if (mask) {
            console_putc('*');
        } else {
            console_putc((char)key);
        }
    }
}
