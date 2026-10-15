/* usage: tarn_bench DIR [N]   (DIR should not exist yet) */
#include "tarn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static double now(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}
#else
#include <time.h>

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
#endif

static uint64_t rnd(uint64_t *s) {
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

static void must(const char *what, int rc) {
    if (rc == TARN_OK) return;
    fprintf(stderr, "%s: %s\n", what, tarn_strerror(rc));
    exit(1);
}

static void report(const char *name, size_t n, double secs) {
    printf("%-22s %9llu ops %9.3f s %12.0f ops/s\n", name, (unsigned long long)n, secs, (double)n / secs);
}

static size_t key(char *buf, size_t i) {
    return (size_t)snprintf(buf, 32, "key%012llu", (unsigned long long)i);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "tarn-bench";
    size_t n = argc > 2 ? (size_t)strtoull(argv[2], NULL, 10) : 1000000;
    size_t durable = n / 500 < 200 ? 200 : n / 500;
    tarn_options o = tarn_default_options();
    uint64_t s = 88172645463325252ull;
    char k[32], val[100];
    tarn *db;
    double t;

    memset(val, 'v', sizeof val);
    o.sync = TARN_SYNC_NONE;
    must("open", tarn_open(dir, &o, &db));
    printf("%llu keys, 16 byte keys, 100 byte values\n\n", (unsigned long long)n);

    t = now();
    for (size_t i = 0; i < n; i++) must("put", tarn_put(db, k, key(k, i), val, sizeof val));
    must("sync", tarn_sync(db));
    report("put, one sync at end", n, now() - t);

    t = now();
    for (size_t i = 0; i < n; i++) {
        void *v;
        size_t vn;
        must("get", tarn_get(db, k, key(k, (size_t)(rnd(&s) % n)), &v, &vn));
        tarn_free(v);
    }
    report("get, random order", n, now() - t);

    for (size_t i = 0; i < n; i += 2) must("put", tarn_put(db, k, key(k, i), val, sizeof val));
    t = now();
    must("compact", tarn_compact(db));
    report("compact", n, now() - t);
    tarn_close(db);

    t = now();
    must("open", tarn_open(dir, &o, &db));
    report("open, full scan", n, now() - t);
    tarn_close(db);

    o.sync = TARN_SYNC_ALWAYS;
    must("open", tarn_open(dir, &o, &db));
    t = now();
    for (size_t i = 0; i < durable; i++) must("put", tarn_put(db, k, key(k, n + i), val, sizeof val));
    report("put, fsync each", durable, now() - t);
    tarn_close(db);
    return 0;
}
