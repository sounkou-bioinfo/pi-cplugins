#include <stddef.h>
#include <stdint.h>
#include <zlib.h>

const char* linked_zlib_version(void) {
  return zlibVersion();
}

uint32_t linked_zlib_crc32(const uint8_t* bytes, size_t length) {
  uLong checksum = crc32(0L, Z_NULL, 0);
  while (length > 0u) {
    const uInt chunk = length > UINT32_MAX ? UINT32_MAX : (uInt)length;
    checksum = crc32(checksum, (const Bytef*)bytes, chunk);
    bytes += chunk;
    length -= chunk;
  }
  return (uint32_t)checksum;
}
