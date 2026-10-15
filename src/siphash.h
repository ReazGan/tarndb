#ifndef TARN_SIPHASH_H
#define TARN_SIPHASH_H

#include <stddef.h>
#include <stdint.h>

/* SipHash-2-4 (Aumasson and Bernstein). With a secret key, nobody outside
 * the process can pick keys that land in the same hash table slot. */
uint64_t siphash24(const unsigned char key[16], const void *data, size_t n);

#endif
