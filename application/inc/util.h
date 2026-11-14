#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdint.h>

/* CRC-32 (IEEE 802.3, reflected, init/xorout 0xFFFFFFFF) */
uint32_t crc32(const uint8_t *data, size_t len);

#endif /* UTIL_H */
