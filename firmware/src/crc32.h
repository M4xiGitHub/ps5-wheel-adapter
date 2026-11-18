// Standard reflected CRC-32 (poly 0xEDB88320, init/xorout 0xFFFFFFFF), as zlib.
#ifndef CRC32_H
#define CRC32_H

#include <stddef.h>
#include <stdint.h>

uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len);  // start with crc = 0
uint32_t crc32_calc(const uint8_t* data, size_t len);

#endif
