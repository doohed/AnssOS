#ifndef LIB_CRC32_H
#define LIB_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* The CRC-32 GPT headers and partition arrays are checked with (IEEE
 * 802.3, reflected, as zlib's crc32()). */
uint32_t crc32(const void *data, size_t len);

#endif
