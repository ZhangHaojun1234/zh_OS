#include "printf.h"
#include "console.h"
#include "io.h"
#include "string.h"

/* ------------------------------------------------------------------ */
/* 最小化 vsnprintf: %c %s %d %i %u %x %X %p %b %%                       */
/* ------------------------------------------------------------------ */
static int format(char *out, size_t size, const char *fmt, __builtin_va_list ap)
{
    size_t pos = 0;
    int total = 0;

#define EMIT(ch) do {                       \
        if (pos + 1 < size) {               \
            out[pos++] = (ch);              \
        }                                   \
        total++;                            \
    } while (0)

#define EMIT_PAD(ch, count) do {            \
        for (int _i = 0; _i < (count); _i++) { \
            EMIT(ch);                       \
        }                                   \
    } while (0)

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            EMIT(*p);
            continue;
        }

        p++;
        int zero_pad = 0;
        int show_sign = 0;
        int left_align = 0;
        int width = 0;
        int is_long = 0;

        while (*p) {
            if (*p == '0') {
                zero_pad = 1;
            } else if (*p == '+') {
                show_sign = 1;
            } else if (*p == '-') {
                left_align = 1;
            } else if (*p == 'l') {
                is_long = 1;
            } else {
                break;
            }
            p++;
        }
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }
        UNUSED(is_long);

        char tmp[66];
        const char *text = NULL;
        int len = 0;
        int negative = 0;
        int pad_char = zero_pad ? '0' : ' ';
        int prefix_len = 0;

        switch (*p) {
        case '%':
            EMIT('%');
            continue;
        case 'c':
            tmp[0] = (char)__builtin_va_arg(ap, int);
            tmp[1] = '\0';
            text = tmp;
            break;
        case 's':
            text = __builtin_va_arg(ap, const char *);
            if (!text) {
                text = "(null)";
            }
            break;
        case 'd':
        case 'i': {
            int value = __builtin_va_arg(ap, int);
            if (value < 0) {
                negative = 1;
                itoa_u32((u32)(-value), tmp, 10);
            } else {
                itoa_u32((u32)value, tmp, 10);
            }
            text = tmp;
            break;
        }
        case 'u':
            itoa_u32(__builtin_va_arg(ap, unsigned int), tmp, 10);
            text = tmp;
            break;
        case 'x':
            itoa_u32(__builtin_va_arg(ap, unsigned int), tmp, 16);
            {
                int n = 0;
                while (tmp[n]) {
                    if (tmp[n] >= 'A' && tmp[n] <= 'Z') {
                        tmp[n] = (char)(tmp[n] - 'A' + 'a');
                    }
                    n++;
                }
            }
            text = tmp;
            break;
        case 'X':
            itoa_u32(__builtin_va_arg(ap, unsigned int), tmp, 16);
            text = tmp;
            break;
        case 'p':
            EMIT('0');
            EMIT('x');
            itoa_u32((u32)__builtin_va_arg(ap, void *), tmp, 16);
            text = tmp;
            break;
        case 'b':
            itoa_u32(__builtin_va_arg(ap, unsigned int), tmp, 2);
            text = tmp;
            break;
        default:
            EMIT('%');
            if (*p) {
                EMIT(*p);
            }
            continue;
        }

        len = (int)strlen(text);
        prefix_len = (negative ? 1 : 0) + (show_sign && !negative ? 1 : 0);
        int total_len = len + prefix_len;

        if (!left_align) {
            if (zero_pad && negative) {
                EMIT('-');
            } else if (zero_pad && show_sign) {
                EMIT('+');
            }
            EMIT_PAD(pad_char, (width > total_len) ? (width - total_len) : 0);
        }
        if (!zero_pad) {
            if (negative) {
                EMIT('-');
            } else if (show_sign) {
                EMIT('+');
            }
        }
        for (int i = 0; i < len; i++) {
            EMIT(text[i]);
        }
        if (left_align) {
            EMIT_PAD(' ', (width > total_len) ? (width - total_len) : 0);
        }
    }

    if (size > 0) {
        out[pos < size ? pos : size - 1] = '\0';
    }
    return total;

#undef EMIT
#undef EMIT_PAD
}

void kprintf(const char *fmt, ...)
{
    char buffer[512];
    __builtin_va_list ap;

    __builtin_va_start(ap, fmt);
    format(buffer, sizeof(buffer), fmt, ap);
    __builtin_va_end(ap);

    console_write(buffer);
}

void kpanic(const char *fmt, ...)
{
    char buffer[512];
    __builtin_va_list ap;

    console_set_color(COLOR_WHITE, COLOR_RED);
    console_write("\n*** KERNEL PANIC ***\n");
    console_write("  ");

    __builtin_va_start(ap, fmt);
    format(buffer, sizeof(buffer), fmt, ap);
    __builtin_va_end(ap);

    console_write(buffer);
    console_write("\n\nSystem halted.\n");

    for (;;) {
        cpu_cli();
        cpu_halt();
    }
}
