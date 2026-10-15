#include "tarn.h"

#include "crc32c.h"
#include "index.h"
#include "os.h"
#include "siphash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static int checks, failures;

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            failures++;                                                                                                \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c);                                      \
        }                                                                                                              \
    } while (0)

#define CHECK_RC(e, want)                                                                                              \
    do {                                                                                                               \
        int rc_ = (e);                                                                                                 \
        checks++;                                                                                                      \
        if (rc_ != (want)) {                                                                                           \
            failures++;                                                                                                \
            fprintf(stderr, "%s:%d: %s = %s, want %s\n", __FILE__, __LINE__, #e, tarn_strerror(rc_),                   \
                    tarn_strerror(want));                                                                              \
        }                                                                                                              \
    } while (0)

/* Repeated here on purpose: the tests pin the on-disk layout. */
#define SEG_HDR 20
#define REC_HDR 16

static char base_dir[400];

static uint64_t rnd(uint64_t *s) {
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

static int rm_entry(const char *name, void *dir) {
    char p[600];
    snprintf(p, sizeof p, "%s/%s", (const char *)dir, name);
    os_remove(p);
    return 0;
}

/* An empty directory for one test. */
static const char *fresh(const char *name) {
    static char paths[4][512];
    static unsigned next;
    char *p = paths[next++ % 4];
    snprintf(p, 512, "%s/%s", base_dir, name);
    os_list(p, rm_entry, p);
    os_mkdir(p);
    return p;
}

static const char *in(const char *dir, const char *name) {
    static char paths[4][600];
    static unsigned next;
    char *p = paths[next++ % 4];
    snprintf(p, 600, "%s/%s", dir, name);
    return p;
}

static unsigned char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    *n = 0;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(size > 0 ? (size_t)size : 1);
    if (b) *n = fread(b, 1, (size_t)size, f);
    fclose(f);
    return b;
}

static void spit(const char *path, const char *mode, const void *data, size_t n) {
    FILE *f = fopen(path, mode);
    if (!f) return;
    fwrite(data, 1, n, f);
    fclose(f);
}

static int exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static const char *key_for(uint32_t i) {
    static char k[32];
    snprintf(k, sizeof k, "key-%u", (unsigned)i);
    return k;
}

/* A value of up to max bytes that depends only on i and round. */
static size_t value_for(uint32_t i, uint32_t round, unsigned char *buf, size_t max) {
    uint64_t s = (uint64_t)i * 0x9e3779b97f4a7c15ull + round + 1;
    size_t n = (size_t)(rnd(&s) % max);
    for (size_t k = 0; k < n; k++) buf[k] = (unsigned char)rnd(&s);
    return n;
}

static int has(tarn *db, const char *key, const void *want, size_t wn) {
    void *v;
    size_t n;
    if (tarn_get(db, key, strlen(key), &v, &n) != TARN_OK) return 0;
    int ok = n == wn && (n == 0 || memcmp(v, want, n) == 0);
    tarn_free(v);
    return ok;
}

static int absent(tarn *db, const char *key) {
    void *v;
    size_t n;
    return tarn_get(db, key, strlen(key), &v, &n) == TARN_NOT_FOUND;
}

static tarn_options fast(uint64_t segment_bytes) {
    tarn_options o = tarn_default_options();
    o.sync = TARN_SYNC_NONE;
    o.segment_bytes = segment_bytes;
    return o;
}

static void put_range(tarn *db, uint32_t from, uint32_t to, uint32_t round, size_t max) {
    unsigned char v[3000];
    for (uint32_t i = from; i < to; i++) {
        size_t n = value_for(i, round, v, max);
        const char *k = key_for(i);
        CHECK_RC(tarn_put(db, k, strlen(k), v, n), TARN_OK);
    }
}

static void check_range(tarn *db, uint32_t from, uint32_t to, uint32_t round, size_t max) {
    unsigned char v[3000];
    for (uint32_t i = from; i < to; i++) {
        size_t n = value_for(i, round, v, max);
        CHECK(has(db, key_for(i), v, n));
    }
}

static void test_crc32c(void) {
    unsigned char b[300];
    uint64_t s = 1;

    CHECK(crc32c(0, "123456789", 9) == 0xe3069283u);
    /* RFC 3720, appendix B.4 */
    memset(b, 0, 32);
    CHECK(crc32c(0, b, 32) == 0x8a9136aau);
    memset(b, 0xff, 32);
    CHECK(crc32c(0, b, 32) == 0x62a8ab43u);
    for (int i = 0; i < 32; i++) b[i] = (unsigned char)i;
    CHECK(crc32c(0, b, 32) == 0x46dd794eu);

    for (size_t i = 0; i < sizeof b; i++) b[i] = (unsigned char)rnd(&s);
    for (size_t a = 0; a < 8; a++)
        for (size_t n = 0; a + n <= sizeof b; n += 3) CHECK(crc32c(0, b + a, n) == crc32c_portable(0, b + a, n));
    CHECK(crc32c(crc32c(0, b, 100), b + 100, 200) == crc32c(0, b, 300));
}

static void test_siphash(void) {
    unsigned char k[16], m[15];
    for (int i = 0; i < 16; i++) k[i] = (unsigned char)i;
    for (int i = 0; i < 15; i++) m[i] = (unsigned char)i;
    /* vectors from the SipHash paper */
    CHECK(siphash24(k, m, 0) == 0x726fdb47dd0e0e31ull);
    CHECK(siphash24(k, m, 15) == 0xa129ca6149be45e5ull);
}

/* Random inserts and removes against a plain array, enough to drive the
 * table through several resizes and long probe runs. */
static void test_index(void) {
    enum { N = 3000 };
    static unsigned char present[N];
    unsigned char seed[16] = {0};
    struct idx ix;
    uint64_t r = 42;
    size_t count = 0;

    CHECK(idx_init(&ix, seed) == 0);
    for (int op = 0; op < 200000; op++) {
        uint32_t u = (uint32_t)(rnd(&r) % N);
        const char *k = key_for(u);
        size_t kl = strlen(k);
        uint64_t h = idx_hash(&ix, k, kl);
        struct islot *s = idx_find(&ix, h, k, kl);
        CHECK((s != NULL) == present[u]);
        if (s) {
            CHECK(s->off == u);
            idx_remove(&ix, s);
            present[u] = 0;
        } else {
            char *copy = malloc(kl);
            CHECK(copy && idx_reserve(&ix) == 0);
            memcpy(copy, k, kl);
            idx_insert(&ix, h, copy, (uint32_t)kl, 1, u, 0);
            present[u] = 1;
        }
    }
    for (uint32_t u = 0; u < N; u++) {
        const char *k = key_for(u);
        CHECK((idx_find(&ix, idx_hash(&ix, k, strlen(k)), k, strlen(k)) != NULL) == present[u]);
        count += present[u];
    }
    CHECK(count == ix.count);
    idx_free(&ix);
}

static void test_basic(void) {
    static char big[TARN_MAX_KEY + 1];
    const char *d = fresh("basic");
    tarn *db = NULL, *other = NULL;
    tarn_stats st;
    void *v;
    size_t n;

    CHECK_RC(tarn_open(d, NULL, &db), TARN_OK);
    if (!db) return;
    CHECK_RC(tarn_put(db, "a", 1, "one", 3), TARN_OK);
    CHECK_RC(tarn_put(db, "b", 1, "two", 3), TARN_OK);
    CHECK_RC(tarn_put(db, "a", 1, "uno", 3), TARN_OK);
    CHECK_RC(tarn_put(db, "empty", 5, NULL, 0), TARN_OK);
    CHECK(has(db, "a", "uno", 3));
    CHECK(has(db, "empty", "", 0));
    CHECK_RC(tarn_delete(db, "b", 1), TARN_OK);
    CHECK_RC(tarn_delete(db, "b", 1), TARN_NOT_FOUND);
    CHECK_RC(tarn_get(db, "b", 1, &v, &n), TARN_NOT_FOUND);

    CHECK_RC(tarn_put(db, "", 0, "x", 1), TARN_INVALID);
    CHECK_RC(tarn_put(db, "k", 1, NULL, 5), TARN_INVALID);
    memset(big, 'k', sizeof big);
    CHECK_RC(tarn_put(db, big, sizeof big, "x", 1), TARN_TOO_BIG);
    CHECK_RC(tarn_put(db, big, TARN_MAX_KEY, "x", 1), TARN_OK);

    CHECK_RC(tarn_open(d, NULL, &other), TARN_LOCKED);
    tarn_close(db);

    CHECK_RC(tarn_open(d, NULL, &db), TARN_OK);
    if (!db) return;
    CHECK(has(db, "a", "uno", 3));
    CHECK(absent(db, "b"));
    CHECK(has(db, "empty", "", 0));
    CHECK_RC(tarn_get(db, big, TARN_MAX_KEY, &v, &n), TARN_OK);
    CHECK(n == 1);
    tarn_free(v);
    tarn_get_stats(db, &st);
    CHECK(st.keys == 3);
    CHECK(st.recovered_bytes == 0 && st.corrupt_records == 0);
    tarn_close(db);
}

static void test_rollover(void) {
    static unsigned char big[300000];
    const char *d = fresh("rollover");
    tarn_options o = fast(4096);
    tarn *db = NULL;
    tarn_stats st;

    for (size_t i = 0; i < sizeof big; i++) big[i] = (unsigned char)(i * 7 + (i >> 9));
    CHECK_RC(tarn_open(d, &o, &db), TARN_OK);
    if (!db) return;
    put_range(db, 0, 300, 0, 3000);
    CHECK_RC(tarn_put(db, "big", 3, big, sizeof big), TARN_OK);
    tarn_get_stats(db, &st);
    CHECK(st.segments > 50);
    tarn_close(db);

    CHECK_RC(tarn_open(d, &o, &db), TARN_OK);
    if (!db) return;
    check_range(db, 0, 300, 0, 3000);
    CHECK(has(db, "big", big, sizeof big));
    tarn_close(db);
}

/* 400 keys, the even ones overwritten, every fifth one deleted. */
static void mixed_fill(tarn *db) {
    put_range(db, 0, 400, 0, 500);
    for (uint32_t i = 0; i < 400; i += 2) {
        unsigned char v[500];
        size_t n = value_for(i, 1, v, sizeof v);
        CHECK_RC(tarn_put(db, key_for(i), strlen(key_for(i)), v, n), TARN_OK);
    }
    for (uint32_t i = 0; i < 400; i += 5) CHECK_RC(tarn_delete(db, key_for(i), strlen(key_for(i))), TARN_OK);
}

static void mixed_check(tarn *db) {
    unsigned char v[500];
    for (uint32_t i = 0; i < 400; i++) {
        if (i % 5 == 0) {
            CHECK(absent(db, key_for(i)));
            continue;
        }
        size_t n = value_for(i, (uint32_t)(i % 2 == 0), v, sizeof v);
        CHECK(has(db, key_for(i), v, n));
    }
}

static void test_compact(void) {
    const char *d = fresh("compact");
    tarn_options o = fast(4096);
    tarn *db = NULL;
    tarn_stats before, after;

    CHECK_RC(tarn_open(d, &o, &db), TARN_OK);
    if (!db) return;
    mixed_fill(db);
    tarn_get_stats(db, &before);
    CHECK(before.dead_bytes > 0);
    CHECK_RC(tarn_compact(db), TARN_OK);
    tarn_get_stats(db, &after);
    CHECK(after.keys == before.keys);
    CHECK(after.dead_bytes == 0);
    CHECK(after.segments == 2);
    CHECK(after.disk_bytes < before.disk_bytes);
    mixed_check(db);
    tarn_close(db);

    /* deletes must survive compaction and a reopen: nothing comes back */
    CHECK_RC(tarn_open(d, &o, &db), TARN_OK);
    if (!db) return;
    mixed_check(db);
    put_range(db, 400, 500, 0, 500);
    CHECK_RC(tarn_compact(db), TARN_OK);
    CHECK_RC(tarn_compact(db), TARN_OK);
    tarn_close(db);

    CHECK_RC(tarn_open(d, &o, &db), TARN_OK);
    if (!db) return;
    mixed_check(db);
    check_range(db, 400, 500, 0, 500);
    tarn_get_stats(db, &after);
    CHECK(after.dead_bytes == 0);
    tarn_close(db);
}

struct snapshot {
    const char *dir;
    char names[256][16];
    unsigned char *data[256];
    size_t size[256], n;
};

static int take(const char *name, void *user) {
    struct snapshot *s = user;
    if (strlen(name) != 12 || strcmp(name + 8, ".seg") != 0 || s->n == 256) return 0;
    snprintf(s->names[s->n], 16, "%s", name);
    s->data[s->n] = slurp(in(s->dir, name), &s->size[s->n]);
    s->n++;
    return 0;
}

/* The state a crash leaves between compaction's rename and its deletes:
 * the new file is in place and the files it replaced are still there. */
static void test_compact_leftovers(void) {
    const char *d = fresh("leftovers");
    tarn_options o = fast(4096);
    struct snapshot snap;
    tarn *db = NULL;
    tarn_stats st;
    size_t restored = 0;

    CHECK_RC(tarn_open(d, &o, &db), TARN_OK);
    if (!db) return;
    mixed_fill(db);
    tarn_close(db);

    memset(&snap, 0, sizeof snap);
    snap.dir = d;
    os_list(d, take, &snap);

    CHECK_RC(tarn_open(d, &o, &db), TARN_OK);
    if (!db) return;
    CHECK_RC(tarn_compact(db), TARN_OK);
    tarn_close(db);

    for (size_t i = 0; i < snap.n; i++) {
        const char *p = in(d, snap.names[i]);
        if (!exists(p)) {
            spit(p, "wb", snap.data[i], snap.size[i]);
            restored++;
        }
        free(snap.data[i]);
    }
    CHECK(restored > 10);
    spit(in(d, "compact.tmp"), "wb", "half a file", 11);

    CHECK_RC(tarn_open(d, &o, &db), TARN_OK);
    if (!db) return;
    mixed_check(db);
    tarn_get_stats(db, &st);
    CHECK(st.segments == 2);
    CHECK(st.dead_bytes == 0);
    tarn_close(db);
    CHECK(!exists(in(d, "compact.tmp")));
}

/* Cut the file at every byte of the last record, as a crash in the middle
 * of the write would. Everything before it must survive untouched and the
 * torn record must never be returned. */
static void test_torn_tail(void) {
    const char *src = fresh("torn-src");
    tarn_options o = fast(0);
    unsigned char *full;
    size_t before, size;
    tarn *db = NULL;
    tarn_stats st;

    CHECK_RC(tarn_open(src, &o, &db), TARN_OK);
    if (!db) return;
    put_range(db, 0, 9, 0, 300);
    tarn_close(db);
    free(slurp(in(src, "00000001.seg"), &before));

    CHECK_RC(tarn_open(src, &o, &db), TARN_OK);
    if (!db) return;
    put_range(db, 9, 10, 0, 300);
    tarn_close(db);
    full = slurp(in(src, "00000001.seg"), &size);
    CHECK(full && size > before);

    for (size_t cut = before; full && cut < size; cut++) {
        const char *d = fresh("torn");
        spit(in(d, "00000001.seg"), "wb", full, cut);
        CHECK_RC(tarn_open(d, &o, &db), TARN_OK);
        if (!db) continue;
        check_range(db, 0, 9, 0, 300);
        CHECK(absent(db, key_for(9)));
        tarn_get_stats(db, &st);
        CHECK(st.recovered_bytes == cut - before);
        CHECK(st.corrupt_records == 0);
        /* and the store carries on from there */
        put_range(db, 9, 10, 0, 300);
        tarn_close(db);
        CHECK_RC(tarn_open(d, &o, &db), TARN_OK);
        if (!db) continue;
        check_range(db, 0, 10, 0, 300);
        tarn_close(db);
    }
    free(full);
}

/* Damage every byte of a record in the middle of the file in turn. Only
 * that record may be lost, and its neighbours must come through intact. */
static void test_bitflip(void) {
    const char *src = fresh("flip-src");
    tarn_options o = fast(0);
    unsigned char a[40], b[40], c[40];
    unsigned char *full;
    size_t size, boff = SEG_HDR + REC_HDR + 1 + sizeof a, blen = REC_HDR + 1 + sizeof b;
    tarn *db = NULL;
    tarn_stats st;

    memset(a, 'A', sizeof a);
    memset(b, 'B', sizeof b);
    memset(c, 'C', sizeof c);
    CHECK_RC(tarn_open(src, &o, &db), TARN_OK);
    if (!db) return;
    CHECK_RC(tarn_put(db, "a", 1, a, sizeof a), TARN_OK);
    CHECK_RC(tarn_put(db, "b", 1, b, sizeof b), TARN_OK);
    CHECK_RC(tarn_put(db, "c", 1, c, sizeof c), TARN_OK);
    tarn_close(db);
    full = slurp(in(src, "00000001.seg"), &size);
    CHECK(full && size == SEG_HDR + 3 * blen);
    if (!full || size != SEG_HDR + 3 * blen) return;

    for (size_t k = 0; k < blen; k++) {
        const char *d = fresh("flip");
        full[boff + k] ^= (unsigned char)(1u << (k % 8));
        spit(in(d, "00000001.seg"), "wb", full, size);
        full[boff + k] ^= (unsigned char)(1u << (k % 8));
        CHECK_RC(tarn_open(d, &o, &db), TARN_OK);
        if (!db) continue;
        CHECK(has(db, "a", a, sizeof a));
        CHECK(absent(db, "b"));
        CHECK(has(db, "c", c, sizeof c));
        tarn_get_stats(db, &st);
        CHECK(st.corrupt_records == 1);
        CHECK(st.recovered_bytes == 0);
        tarn_close(db);
    }
    free(full);
}

static void test_rot_after_open(void) {
    const char *d = fresh("rot");
    tarn *db = NULL;
    void *v;
    size_t n;

    CHECK_RC(tarn_open(d, NULL, &db), TARN_OK);
    if (!db) return;
    CHECK_RC(tarn_put(db, "k", 1, "value-123", 9), TARN_OK);
    FILE *f = fopen(in(d, "00000001.seg"), "r+b");
    CHECK(f != NULL);
    if (f) {
        fseek(f, SEG_HDR + REC_HDR + 1 + 3, SEEK_SET);
        fputc('X', f);
        fclose(f);
    }
    CHECK_RC(tarn_get(db, "k", 1, &v, &n), TARN_CORRUPT);
    tarn_close(db);
}

/* Some file systems come back from a power cut with the file size updated
 * but the data never written, which reads as zeros. */
static void test_zero_tail(void) {
    static unsigned char zeros[4096];
    const char *d = fresh("zeros");
    tarn_options o = fast(0);
    tarn *db = NULL;
    tarn_stats st;

    CHECK_RC(tarn_open(d, &o, &db), TARN_OK);
    if (!db) return;
    put_range(db, 0, 20, 0, 200);
    tarn_close(db);
    spit(in(d, "00000001.seg"), "ab", zeros, sizeof zeros);
    CHECK_RC(tarn_open(d, &o, &db), TARN_OK);
    if (!db) return;
    check_range(db, 0, 20, 0, 200);
    tarn_get_stats(db, &st);
    CHECK(st.recovered_bytes == sizeof zeros);
    tarn_close(db);
}

static void test_segment_headers(void) {
    const char *d = fresh("hdr");
    tarn_options o = fast(0);
    tarn *db = NULL;

    /* a segment whose header never reached the disk holds nothing acknowledged */
    CHECK_RC(tarn_open(d, &o, &db), TARN_OK);
    if (!db) return;
    put_range(db, 0, 5, 0, 100);
    tarn_close(db);
    spit(in(d, "00000002.seg"), "wb", "\0\0\0\0\0\0\0", 7);
    CHECK_RC(tarn_open(d, &o, &db), TARN_OK);
    if (!db) return;
    check_range(db, 0, 5, 0, 100);
    tarn_close(db);
    CHECK(!exists(in(d, "00000002.seg")));

    /* a damaged header in front of real data is not a crash; refuse to guess */
    FILE *f = fopen(in(d, "00000001.seg"), "r+b");
    CHECK(f != NULL);
    if (f) {
        fputc('X', f);
        fclose(f);
    }
    CHECK_RC(tarn_open(d, &o, &db), TARN_CORRUPT);
    if (db) tarn_close(db);
}

#ifndef _WIN32
/* SIGKILL a writer at a random point. Every put it saw acknowledged has to
 * be there afterwards, byte for byte. */
static void test_kill(void) {
    for (uint32_t round = 0; round < 6; round++) {
        const char *d = fresh("kill");
        int fds[2];
        uint32_t got, acked = 0, want = 20 + round * 25;
        int any = 0;

        CHECK(pipe(fds) == 0);
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            tarn_options o = tarn_default_options();
            tarn *db;
            unsigned char v[600];
            close(fds[0]);
            o.segment_bytes = 16384;
            if (tarn_open(d, &o, &db)) _exit(1);
            for (uint32_t i = 0;; i++) {
                size_t n = value_for(i, round, v, sizeof v);
                if (tarn_put(db, key_for(i), strlen(key_for(i)), v, n)) _exit(1);
                if (write(fds[1], &i, sizeof i) != (ssize_t)sizeof i) _exit(1);
            }
        }
        close(fds[1]);
        while (read(fds[0], &got, sizeof got) == (ssize_t)sizeof got) {
            acked = got;
            any = 1;
            if (got >= want) break;
        }
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        while (read(fds[0], &got, sizeof got) == (ssize_t)sizeof got) acked = got;
        close(fds[0]);
        CHECK(any);

        tarn *db = NULL;
        CHECK_RC(tarn_open(d, NULL, &db), TARN_OK);
        if (!db) continue;
        check_range(db, 0, acked + 1, round, 600);
        tarn_close(db);
    }
}
#endif

int main(int argc, char **argv) {
    snprintf(base_dir, sizeof base_dir, "%s", argc > 1 ? argv[1] : "tarn-test-tmp");
    os_mkdir(base_dir);

    test_crc32c();
    test_siphash();
    test_index();
    test_basic();
    test_rollover();
    test_compact();
    test_compact_leftovers();
    test_torn_tail();
    test_bitflip();
    test_rot_after_open();
    test_zero_tail();
    test_segment_headers();
#ifndef _WIN32
    test_kill();
#endif

    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
