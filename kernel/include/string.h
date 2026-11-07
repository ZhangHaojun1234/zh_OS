/* ---------------------------------------------------------------
 * 字符串与内存操作
 * --------------------------------------------------------------- */
#ifndef ZH_STRING_H
#define ZH_STRING_H

#include "types.h"

void *memset(void *dst, int value, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);

int atoi(const char *s);
void itoa_u32(u32 value, char *buf, int base);
void itoa_i32(i32 value, char *buf, int base);

#endif
