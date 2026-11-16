/* font8x16.h - 字体的尺寸常量和 extern 声明 */

#ifndef ZH_OS_FONT8X16_H
#define ZH_OS_FONT8X16_H

#include <stdint.h>

#define FONT_W      8u
#define FONT_H      16u
#define FONT_FIRST  32u
#define FONT_LAST   126u
#define FONT_GLYPHS 95u

extern const uint8_t g_font8x16[FONT_GLYPHS][FONT_H];

#endif /* ZH_OS_FONT8X16_H */
