/* Minimal <string.h> for vendored code (e.g. third_party/minimp3) that
 * includes the standard header -- AnssOS userland builds freestanding,
 * with no host libc headers, so this just forwards to userland/libc.h. */
#ifndef ANSSOS_COMPAT_STRING_H
#define ANSSOS_COMPAT_STRING_H
#include "../libc.h"
#endif
