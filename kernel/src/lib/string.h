#ifndef LIB_STRING_H
#define LIB_STRING_H

#include <stddef.h>

/* Freestanding: no libc, so we implement the handful of mem-family and */
/* str-family functions the kernel needs itself. */

void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *dest, int c, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);

#endif
