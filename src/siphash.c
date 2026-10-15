#include "siphash.h"

#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))

#define ROUND                                                                                                          \
    do {                                                                                                               \
        v0 += v1;                                                                                                      \
        v1 = ROTL(v1, 13);                                                                                             \
        v1 ^= v0;                                                                                                      \
        v0 = ROTL(v0, 32);                                                                                             \
        v2 += v3;                                                                                                      \
        v3 = ROTL(v3, 16);                                                                                             \
        v3 ^= v2;                                                                                                      \
        v0 += v3;                                                                                                      \
        v3 = ROTL(v3, 21);                                                                                             \
        v3 ^= v0;                                                                                                      \
        v2 += v1;                                                                                                      \
        v1 = ROTL(v1, 17);                                                                                             \
        v1 ^= v2;                                                                                                      \
        v2 = ROTL(v2, 32);                                                                                             \
    } while (0)

static uint64_t load64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

uint64_t siphash24(const unsigned char key[16], const void *data, size_t n) {
    const unsigned char *p = data;
    uint64_t k0 = load64(key), k1 = load64(key + 8);
    uint64_t v0 = 0x736f6d6570736575ull ^ k0;
    uint64_t v1 = 0x646f72616e646f6dull ^ k1;
    uint64_t v2 = 0x6c7967656e657261ull ^ k0;
    uint64_t v3 = 0x7465646279746573ull ^ k1;
    size_t whole = n & ~(size_t)7;

    for (size_t i = 0; i < whole; i += 8) {
        uint64_t m = load64(p + i);
        v3 ^= m;
        ROUND;
        ROUND;
        v0 ^= m;
    }

    uint64_t last = (uint64_t)n << 56;
    for (size_t i = 0; i < (n & 7); i++) last |= (uint64_t)p[whole + i] << (8 * i);
    v3 ^= last;
    ROUND;
    ROUND;
    v0 ^= last;

    v2 ^= 0xff;
    ROUND;
    ROUND;
    ROUND;
    ROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}
