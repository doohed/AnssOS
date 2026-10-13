/* The <string.h> minimp3 (../../vendor/minimp3/) includes. AnssOS
 * userland builds freestanding, with no C library at all, so this
 * declares just the three functions minimp3 calls. Rust's
 * compiler_builtins defines them, and they're linked in with libplay.a. */
#ifndef ANSSOS_PLAY_STRING_H
#define ANSSOS_PLAY_STRING_H
#include <stddef.h>
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memset(void *dest, int c, size_t n);
#endif
