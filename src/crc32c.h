#ifndef TARN_CRC32C_H
#define TARN_CRC32C_H

#include <stddef.h>
#include <stdint.h>

/* CRC-32C (Castagnoli), the one with hardware support on x86 and ARM.
 * Calls chain: crc32c(crc32c(0, a), b) == crc32c(0, a followed by b). */
uint32_t crc32c(uint32_t crc, const void *data, size_t n);

/* Table-driven version, always available; the tests hold the two against each other. */
uint32_t crc32c_portable(uint32_t crc, const void *data, size_t n);

#endif
