/* ---------------------------------------------------------------
 * zh_OS 基础类型定义
 * --------------------------------------------------------------- */
#ifndef ZH_TYPES_H
#define ZH_TYPES_H

typedef unsigned char       u8;
typedef signed char         i8;
typedef unsigned short      u16;
typedef signed short        i16;
typedef unsigned int        u32;
typedef signed int          i32;
typedef unsigned long long  u64;
typedef signed long long    i64;

typedef u32 size_t;
typedef u32 uintptr_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PACKED __attribute__((packed))
#define NORETURN __attribute__((noreturn))
#define UNUSED(x) ((void)(x))
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#endif
