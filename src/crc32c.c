#include "crc32c.h"

#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#define HW_X86 1
#include <nmmintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#define TARGET_SSE42
#else
#define TARGET_SSE42 __attribute__((target("sse4.2")))
#endif
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#define HW_ARM 1
#include <arm_acle.h>
#endif

#define POLY 0x82f63b78u /* reversed Castagnoli polynomial */

/* table[k][b] is the CRC of byte b followed by k zero bytes, which lets
 * the loop below fold eight bytes per step */
static uint32_t table[8][256];

#if defined(HW_X86) || defined(HW_ARM)
static int use_hw;
#endif

static void init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (POLY & (0u - (c & 1u)));
        table[0][i] = c;
    }
    for (int t = 1; t < 8; t++)
        for (uint32_t i = 0; i < 256; i++) table[t][i] = (table[t - 1][i] >> 8) ^ table[0][table[t - 1][i] & 0xffu];
#if defined(HW_X86) && defined(_MSC_VER)
    int r[4];
    __cpuid(r, 1);
    use_hw = (r[2] >> 20) & 1;
#elif defined(HW_X86)
    __builtin_cpu_init();
    use_hw = __builtin_cpu_supports("sse4.2");
#elif defined(HW_ARM)
    use_hw = 1;
#endif
}

#ifdef _WIN32
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_once(PINIT_ONCE o, PVOID param, PVOID *ctx) {
    (void)o;
    (void)param;
    (void)ctx;
    init();
    return TRUE;
}

static void ensure(void) { InitOnceExecuteOnce(&once, init_once, NULL, NULL); }
#else
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void ensure(void) { pthread_once(&once, init); }
#endif

static uint32_t sw(uint32_t c, const unsigned char *p, size_t n) {
    while (n >= 8) {
        uint32_t a = c ^ ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
        c = table[7][a & 0xffu] ^ table[6][(a >> 8) & 0xffu] ^ table[5][(a >> 16) & 0xffu] ^ table[4][a >> 24] ^
            table[3][p[4]] ^ table[2][p[5]] ^ table[1][p[6]] ^ table[0][p[7]];
        p += 8;
        n -= 8;
    }
    while (n--) c = table[0][(c ^ *p++) & 0xffu] ^ (c >> 8);
    return c;
}

#if defined(HW_X86)
TARGET_SSE42 static uint32_t hw(uint32_t c, const unsigned char *p, size_t n) {
    uint64_t c64 = c;
    while (n >= 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        c64 = _mm_crc32_u64(c64, v);
        p += 8;
        n -= 8;
    }
    c = (uint32_t)c64;
    while (n--) c = _mm_crc32_u8(c, *p++);
    return c;
}
#elif defined(HW_ARM)
static uint32_t hw(uint32_t c, const unsigned char *p, size_t n) {
    while (n >= 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        c = __crc32cd(c, v);
        p += 8;
        n -= 8;
    }
    while (n--) c = __crc32cb(c, *p++);
    return c;
}
#endif

uint32_t crc32c(uint32_t crc, const void *data, size_t n) {
    ensure();
#if defined(HW_X86) || defined(HW_ARM)
    if (use_hw) return ~hw(~crc, data, n);
#endif
    return ~sw(~crc, data, n);
}

uint32_t crc32c_portable(uint32_t crc, const void *data, size_t n) {
    ensure();
    return ~sw(~crc, data, n);
}
