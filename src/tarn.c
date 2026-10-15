#include "tarn.h"

#include "crc32c.h"
#include "index.h"
#include "os.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * On-disk format. All integers are little-endian.
 *
 * A segment file is named after its id in hex (00000001.seg) and starts with
 *
 *     u32 magic "TARN"    u16 version    u16 flags (0)
 *     u32 id              u32 base       u32 crc32c of the 16 bytes before it
 *
 * base equals id, except in a file written by compaction, which stands in
 * for every segment from base up to id.
 *
 * Records follow back to back:
 *
 *     u32 hcrc    crc32c of klen, vlen and pcrc
 *     u32 klen    1 .. TARN_MAX_KEY
 *     u32 vlen    0 .. TARN_MAX_VALUE, or 0xffffffff for a delete
 *     u32 pcrc    crc32c of key and value
 *     key, value
 *
 * The header has a checksum of its own, so when a value is damaged the
 * record boundary is still known and only that one record is lost.
 */

#define SEG_MAGIC 0x4e524154u
#define SEG_VERSION 1u
#define SEG_HDR 20u
#define REC_HDR 16u
#define TOMBSTONE 0xffffffffu
#define IO_BUF ((size_t)64 << 10)
#define SMALL_VALUE ((size_t)4 << 10) /* values up to this go out in one write with their header */
#define DEFAULT_SEGMENT ((uint64_t)64 << 20)
#define MIN_SEGMENT ((uint64_t)4 << 10)
#define COMPACT_TMP "compact.tmp"

enum { REC_GOOD, REC_BAD_HEADER, REC_BAD_BODY, REC_SHORT };

struct seg {
    uint32_t id;
    uint32_t base;
    uint64_t size;
    os_file *f;
};

struct tarn {
    char *dir;
    os_lock *lock;
    tarn_options opt;
    struct seg *segs; /* sorted by id; appends go to the last one */
    size_t nsegs, segcap;
    struct idx ix;
    uint64_t live; /* bytes taken by the records the index points at */
    uint64_t recovered, corrupt;
    int failed;
    unsigned char *rec; /* one record header and key, plus room for a small value */
    unsigned char *io;  /* read window while scanning or compacting */
};

static void put16(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *p, uint32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}

static uint32_t get16(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const unsigned char *p) { return get16(p) | get16(p + 2) << 16; }

static size_t chunk(uint64_t left) { return left < IO_BUF ? (size_t)left : IO_BUF; }

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static char *join(const char *dir, const char *name) {
    size_t a = strlen(dir), b = strlen(name);
    char *p = malloc(a + b + 2);
    if (!p) return NULL;
    memcpy(p, dir, a);
    p[a] = '/';
    memcpy(p + a + 1, name, b + 1);
    return p;
}

static char *seg_path(const tarn *db, uint32_t id) {
    char name[16];
    snprintf(name, sizeof name, "%08" PRIx32 ".seg", id);
    return join(db->dir, name);
}

static void drop_file(const tarn *db, uint32_t id) {
    char *p = seg_path(db, id);
    if (p) os_remove(p);
    free(p);
}

static int parse_seg_name(const char *name, uint32_t *id) {
    uint32_t v = 0;
    if (strlen(name) != 12 || strcmp(name + 8, ".seg") != 0) return 0;
    for (int i = 0; i < 8; i++) {
        char c = name[i];
        uint32_t d;
        if (c >= '0' && c <= '9')
            d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = (uint32_t)(c - 'a' + 10);
        else
            return 0;
        v = v << 4 | d;
    }
    *id = v;
    return 1;
}

static void seg_header(unsigned char *h, uint32_t id, uint32_t base) {
    put32(h, SEG_MAGIC);
    put16(h + 4, SEG_VERSION);
    put16(h + 6, 0);
    put32(h + 8, id);
    put32(h + 12, base);
    put32(h + 16, crc32c(0, h, 16));
}

static int seg_header_ok(const unsigned char *h, uint32_t id, uint32_t *base) {
    if (get32(h) != SEG_MAGIC || get32(h + 16) != crc32c(0, h, 16)) return 0;
    if (get16(h + 4) != SEG_VERSION || get32(h + 8) != id) return 0;
    *base = get32(h + 12);
    return *base <= id;
}

/* Creates or empties path and leaves a synced segment header in it. */
static int seg_write_new(const char *path, uint32_t id, uint32_t base, os_file **out) {
    unsigned char h[SEG_HDR];
    os_file *f;
    if (os_open(path, OS_READ | OS_WRITE | OS_CREATE, &f)) return -1;
    seg_header(h, id, base);
    if (os_truncate(f, 0) || os_pwrite(f, h, SEG_HDR, 0) || os_sync(f)) {
        os_close(f);
        return -1;
    }
    *out = f;
    return 0;
}

static int segs_push(tarn *db, const struct seg *s) {
    if (db->nsegs == db->segcap) {
        size_t cap = db->segcap ? db->segcap * 2 : 8;
        struct seg *n = realloc(db->segs, cap * sizeof *n);
        if (!n) return -1;
        db->segs = n;
        db->segcap = cap;
    }
    db->segs[db->nsegs++] = *s;
    return 0;
}

static struct seg *active(tarn *db) { return &db->segs[db->nsegs - 1]; }

static struct seg *seg_by_id(tarn *db, uint32_t id) {
    size_t lo = 0, hi = db->nsegs;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (db->segs[mid].id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < db->nsegs && db->segs[lo].id == id ? &db->segs[lo] : NULL;
}

/*
 * After a failed write or fsync the file is in an unknown state. Worse, on
 * Linux a failed fsync may drop the dirty pages and mark them clean, so a
 * retry reports success for data that never reached the disk. Refuse all
 * further writes; reopening rereads the files and keeps what is really there.
 */
static int poison(tarn *db) {
    db->failed = 1;
    return TARN_IO;
}

/* Closes the active segment for writing and starts the next one. */
static int rotate(tarn *db) {
    struct seg *a = active(db);
    struct seg s;
    if (a->id == UINT32_MAX) return poison(db);
    if (os_sync(a->f)) return poison(db);
    s.id = s.base = a->id + 1;
    s.size = SEG_HDR;
    s.f = NULL;
    char *path = seg_path(db, s.id);
    if (!path) return TARN_NOMEM;
    int bad = seg_write_new(path, s.id, s.base, &s.f);
    free(path);
    if (bad || os_sync_dir(db->dir) || segs_push(db, &s)) {
        os_close(s.f);
        return poison(db);
    }
    return TARN_OK;
}

/* Appends one record to the active segment and says where it landed. */
static int append(tarn *db, const void *key, uint32_t klen, const void *val, uint32_t vlen, uint32_t *seg,
                  uint64_t *off, uint32_t *len) {
    uint32_t vbytes = vlen == TOMBSTONE ? 0 : vlen;
    uint32_t total = REC_HDR + klen + vbytes;
    unsigned char *h = db->rec;
    size_t first = REC_HDR + klen;
    struct seg *a;

    if (db->failed) return TARN_IO;
    a = active(db);
    if (a->size > SEG_HDR && a->size + total > db->opt.segment_bytes) {
        int rc = rotate(db);
        if (rc) return rc;
        a = active(db);
    }

    put32(h + 4, klen);
    put32(h + 8, vlen);
    put32(h + 12, crc32c(crc32c(0, key, klen), val, vbytes));
    put32(h, crc32c(0, h + 4, 12));
    memcpy(h + REC_HDR, key, klen);
    if (vbytes <= SMALL_VALUE) {
        if (vbytes) memcpy(h + first, val, vbytes);
        first += vbytes;
    }
    if (os_pwrite(a->f, h, first, a->size) ||
        (first < total && os_pwrite(a->f, (const unsigned char *)val, vbytes, a->size + first))) {
        /* cut off whatever part of the record made it, so the next append starts clean */
        return os_truncate(a->f, a->size) ? poison(db) : TARN_IO;
    }
    if (db->opt.sync == TARN_SYNC_ALWAYS && os_sync(a->f)) return poison(db);

    *seg = a->id;
    *off = a->size;
    *len = total;
    a->size += total;
    return TARN_OK;
}

/*
 * Reading a segment record by record would cost a few system calls per
 * record. A reader keeps a window of the file in memory instead and serves
 * reads from it, refilling only when a read falls outside.
 */
struct reader {
    os_file *f;
    unsigned char *buf; /* IO_BUF bytes */
    uint64_t base;
    size_t have;
};

static void reader_init(struct reader *r, os_file *f, unsigned char *buf) {
    r->f = f;
    r->buf = buf;
    r->base = 0;
    r->have = 0;
}

/* Points *p at up to max bytes of the file at off. *n == 0 means end of file. */
static int rd_span(struct reader *r, uint64_t off, size_t max, const unsigned char **p, size_t *n) {
    if (off < r->base || off - r->base >= r->have) {
        r->base = off;
        if (os_pread(r->f, r->buf, IO_BUF, off, &r->have)) {
            r->have = 0;
            return -1;
        }
    }
    size_t at = (size_t)(off - r->base);
    *p = r->buf + at;
    *n = r->have - at < max ? r->have - at : max;
    return 0;
}

static int rd(struct reader *r, uint64_t off, void *dst, size_t n, size_t *got) {
    size_t done = 0;
    while (done < n) {
        const unsigned char *p;
        size_t k;
        if (rd_span(r, off + done, n - done, &p, &k)) return -1;
        if (k == 0) break;
        memcpy((unsigned char *)dst + done, p, k);
        done += k;
    }
    *got = done;
    return 0;
}

/*
 * Classifies the record at off. When it is REC_GOOD, its header and key are
 * left in db->rec. The value is checksummed straight out of the read window,
 * so a huge value is never held in memory. Returns -1 if the file cannot be
 * read at all.
 */
static int check_record(tarn *db, struct reader *r, uint64_t off, uint64_t end, uint32_t *len) {
    unsigned char *h = db->rec;
    size_t got;

    if (end - off < REC_HDR) return REC_SHORT;
    if (rd(r, off, h, REC_HDR, &got)) return -1;
    if (got < REC_HDR) return REC_SHORT;

    uint32_t klen = get32(h + 4), vlen = get32(h + 8);
    if (get32(h) != crc32c(0, h + 4, 12) || klen == 0 || klen > TARN_MAX_KEY ||
        (vlen > TARN_MAX_VALUE && vlen != TOMBSTONE))
        return REC_BAD_HEADER;
    uint64_t total = (uint64_t)REC_HDR + klen + (vlen == TOMBSTONE ? 0 : vlen);
    if (total > end - off) return REC_SHORT;
    *len = (uint32_t)total;

    if (rd(r, off + REC_HDR, h + REC_HDR, klen, &got)) return -1;
    if (got < klen) return REC_SHORT;
    uint32_t crc = crc32c(0, h + REC_HDR, klen);
    for (uint64_t p = off + REC_HDR + klen, stop = off + total; p < stop;) {
        const unsigned char *s;
        size_t n;
        if (rd_span(r, p, chunk(stop - p), &s, &n)) return -1;
        if (n == 0) return REC_SHORT;
        crc = crc32c(crc, s, n);
        p += n;
    }
    return crc == get32(h + 12) ? REC_GOOD : REC_BAD_BODY;
}

/*
 * A damaged header hides where the next record starts. Slide forward a byte
 * at a time until a header checks out and so does the record behind it.
 * Sets *next to that offset, or to end if nothing intact follows.
 */
static int resync(tarn *db, struct reader *r, uint64_t from, uint64_t end, uint64_t *next) {
    *next = end;
    for (uint64_t p = from; p + REC_HDR <= end; p++) {
        unsigned char h[REC_HDR];
        size_t got;
        uint32_t len;
        if (rd(r, p, h, REC_HDR, &got)) return TARN_IO;
        if (got < REC_HDR) break;
        if (get32(h) != crc32c(0, h + 4, 12)) continue;
        int c = check_record(db, r, p, end, &len);
        if (c < 0) return TARN_IO;
        if (c == REC_GOOD) {
            *next = p;
            break;
        }
    }
    return TARN_OK;
}

/* Feeds the record just read by check_record into the index. */
static int apply(tarn *db, uint32_t seg, uint64_t off, uint32_t len) {
    const unsigned char *h = db->rec;
    const unsigned char *key = h + REC_HDR;
    uint32_t klen = get32(h + 4);
    uint64_t hash = idx_hash(&db->ix, key, klen);
    struct islot *s = idx_find(&db->ix, hash, key, klen);

    if (get32(h + 8) == TOMBSTONE) {
        if (s) {
            db->live -= s->len;
            idx_remove(&db->ix, s);
        }
        return TARN_OK;
    }
    if (s) {
        db->live -= s->len;
        s->seg = seg;
        s->off = off;
        s->len = len;
    } else {
        char *copy = malloc(klen);
        if (!copy || idx_reserve(&db->ix)) {
            free(copy);
            return TARN_NOMEM;
        }
        memcpy(copy, key, klen);
        idx_insert(&db->ix, hash, copy, klen, seg, off, len);
    }
    db->live += len;
    return TARN_OK;
}

/* The end of the newest segment is the only place a crash can leave a
 * half-written record. Cut it off so appends continue from solid ground. */
static int cut_tail(tarn *db, struct seg *s, uint64_t off) {
    db->recovered += s->size - off;
    if (os_truncate(s->f, off) || os_sync(s->f)) return TARN_IO;
    s->size = off;
    return TARN_OK;
}

static int scan(tarn *db, struct seg *s, int last) {
    struct reader rdr;
    uint64_t off = SEG_HDR;
    reader_init(&rdr, s->f, db->io);
    while (off < s->size) {
        uint32_t len = 0;
        uint64_t next;
        int rc, r = check_record(db, &rdr, off, s->size, &len);
        if (r < 0) return TARN_IO;
        if (r == REC_GOOD) {
            rc = apply(db, s->id, off, len);
            if (rc) return rc;
            off += len;
            continue;
        }
        if (r == REC_BAD_BODY) {
            if (last && off + len == s->size) return cut_tail(db, s, off);
            /* the header is sound, so the damage stops at this record */
            db->corrupt++;
            off += len;
            continue;
        }
        /* bad header, or a record running past the end of the file */
        rc = resync(db, &rdr, off + 1, s->size, &next);
        if (rc) return rc;
        if (next == s->size && last) return cut_tail(db, s, off);
        db->corrupt++;
        off = next;
    }
    return TARN_OK;
}

struct listing {
    uint32_t *ids;
    size_t n, cap;
    int stale_tmp, nomem;
};

static int collect(const char *name, void *user) {
    struct listing *l = user;
    uint32_t id;
    if (strcmp(name, COMPACT_TMP) == 0) {
        l->stale_tmp = 1;
        return 0;
    }
    if (!parse_seg_name(name, &id)) return 0;
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 16;
        uint32_t *ids = realloc(l->ids, cap * sizeof *ids);
        if (!ids) {
            l->nomem = 1;
            return 1;
        }
        l->ids = ids;
        l->cap = cap;
    }
    l->ids[l->n++] = id;
    return 0;
}

static int cmp_id(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static int open_segments(tarn *db, const struct listing *l) {
    for (size_t i = 0; i < l->n; i++) {
        unsigned char h[SEG_HDR];
        size_t got = 0;
        struct seg s;
        char *path = seg_path(db, l->ids[i]);
        if (!path) return TARN_NOMEM;
        s.id = l->ids[i];
        s.f = NULL;
        if (os_open(path, OS_READ, &s.f) || os_size(s.f, &s.size) || os_pread(s.f, h, SEG_HDR, 0, &got)) {
            os_close(s.f);
            free(path);
            return TARN_IO;
        }
        if (got < SEG_HDR || !seg_header_ok(h, s.id, &s.base)) {
            os_close(s.f);
            if (i + 1 == l->n && s.size <= SEG_HDR) {
                /* The crash came while this segment was being created. Its
                   header is synced before any record goes in, so nothing in
                   it was ever acknowledged. */
                os_remove(path);
                free(path);
                db->recovered += s.size;
                continue;
            }
            free(path);
            return TARN_CORRUPT;
        }
        free(path);
        if (segs_push(db, &s)) {
            os_close(s.f);
            return TARN_NOMEM;
        }
    }
    return TARN_OK;
}

/*
 * A compaction that renamed its output into place but crashed before
 * deleting the segments it replaced leaves them behind. The base field of
 * the output names them; finish the job.
 */
static int drop_replaced(tarn *db) {
    size_t keep = 0;
    int dropped = 0;
    for (size_t i = 0; i < db->nsegs; i++) {
        struct seg s = db->segs[i];
        int replaced = 0;
        for (size_t j = i + 1; j < db->nsegs; j++)
            if (db->segs[j].base <= s.id && s.id < db->segs[j].id) replaced = 1;
        if (replaced) {
            os_close(s.f);
            drop_file(db, s.id);
            dropped = 1;
        } else {
            db->segs[keep++] = s;
        }
    }
    db->nsegs = keep;
    return dropped && os_sync_dir(db->dir) ? TARN_IO : TARN_OK;
}

static int load(tarn *db) {
    struct listing l;
    int rc;

    memset(&l, 0, sizeof l);
    rc = os_list(db->dir, collect, &l);
    if (rc < 0 || l.nomem) {
        free(l.ids);
        return l.nomem ? TARN_NOMEM : TARN_IO;
    }
    if (l.stale_tmp) {
        /* output of a compaction that never committed */
        char *p = join(db->dir, COMPACT_TMP);
        if (p) os_remove(p);
        free(p);
    }
    if (l.n) qsort(l.ids, l.n, sizeof *l.ids, cmp_id);
    rc = open_segments(db, &l);
    free(l.ids);
    if (!rc) rc = drop_replaced(db);
    if (rc) return rc;

    if (db->nsegs == 0) {
        struct seg s;
        char *path = seg_path(db, 1);
        if (!path) return TARN_NOMEM;
        s.id = s.base = 1;
        s.size = SEG_HDR;
        s.f = NULL;
        rc = seg_write_new(path, 1, 1, &s.f);
        free(path);
        if (rc || os_sync_dir(db->dir)) {
            os_close(s.f);
            return TARN_IO;
        }
        if (segs_push(db, &s)) {
            os_close(s.f);
            return TARN_NOMEM;
        }
        return TARN_OK;
    }

    /* only the newest segment is ever written to */
    struct seg *a = active(db);
    char *path = seg_path(db, a->id);
    if (!path) return TARN_NOMEM;
    os_close(a->f);
    a->f = NULL;
    rc = os_open(path, OS_READ | OS_WRITE, &a->f);
    free(path);
    if (rc) return TARN_IO;

    for (size_t i = 0; i < db->nsegs; i++) {
        rc = scan(db, &db->segs[i], i + 1 == db->nsegs);
        if (rc) return rc;
    }
    return TARN_OK;
}

static void release(tarn *db) {
    for (size_t i = 0; i < db->nsegs; i++) os_close(db->segs[i].f);
    free(db->segs);
    idx_free(&db->ix);
    os_unlock(db->lock);
    free(db->dir);
    free(db->rec);
    free(db->io);
    free(db);
}

tarn_options tarn_default_options(void) {
    tarn_options o;
    o.sync = TARN_SYNC_ALWAYS;
    o.segment_bytes = DEFAULT_SEGMENT;
    return o;
}

int tarn_open(const char *dir, const tarn_options *opts, tarn **out) {
    unsigned char seed[16];
    char *lock;
    tarn *db;
    int rc;

    if (!dir || !*dir || !out) return TARN_INVALID;
    *out = NULL;
    db = calloc(1, sizeof *db);
    if (!db) return TARN_NOMEM;
    db->opt = opts ? *opts : tarn_default_options();
    if (db->opt.segment_bytes == 0) db->opt.segment_bytes = DEFAULT_SEGMENT;
    if (db->opt.segment_bytes < MIN_SEGMENT) db->opt.segment_bytes = MIN_SEGMENT;
    if (db->opt.sync != TARN_SYNC_ALWAYS && db->opt.sync != TARN_SYNC_NONE) {
        free(db);
        return TARN_INVALID;
    }

    rc = TARN_NOMEM;
    db->dir = dup_str(dir);
    db->rec = malloc(REC_HDR + TARN_MAX_KEY + SMALL_VALUE);
    db->io = malloc(IO_BUF);
    if (!db->dir || !db->rec || !db->io) goto fail;

    rc = TARN_IO;
    if (os_mkdir(dir) || os_random(seed, sizeof seed)) goto fail;
    lock = join(dir, "LOCK");
    if (!lock) {
        rc = TARN_NOMEM;
        goto fail;
    }
    if (os_lock_dir(lock, &db->lock)) {
        free(lock);
        rc = TARN_LOCKED;
        goto fail;
    }
    free(lock);
    if (idx_init(&db->ix, seed)) {
        rc = TARN_NOMEM;
        goto fail;
    }
    rc = load(db);
    if (rc) goto fail;
    *out = db;
    return TARN_OK;

fail:
    release(db);
    return rc;
}

void tarn_close(tarn *db) {
    if (!db) return;
    if (!db->failed && db->nsegs) os_sync(active(db)->f);
    release(db);
}

int tarn_put(tarn *db, const void *key, size_t klen, const void *val, size_t vlen) {
    if (!db || !key || !klen || (!val && vlen)) return TARN_INVALID;
    if (klen > TARN_MAX_KEY || vlen > TARN_MAX_VALUE) return TARN_TOO_BIG;

    uint64_t hash = idx_hash(&db->ix, key, klen);
    struct islot *s = idx_find(&db->ix, hash, key, klen);
    char *copy = NULL;
    if (!s) {
        /* everything that can fail in memory fails before the disk is touched */
        copy = malloc(klen);
        if (!copy || idx_reserve(&db->ix)) {
            free(copy);
            return TARN_NOMEM;
        }
        memcpy(copy, key, klen);
    }

    uint32_t seg, len;
    uint64_t off;
    int rc = append(db, key, (uint32_t)klen, val, (uint32_t)vlen, &seg, &off, &len);
    if (rc) {
        free(copy);
        return rc;
    }
    if (s) {
        db->live -= s->len;
        s->seg = seg;
        s->off = off;
        s->len = len;
    } else {
        idx_insert(&db->ix, hash, copy, (uint32_t)klen, seg, off, len);
    }
    db->live += len;
    return TARN_OK;
}

int tarn_get(tarn *db, const void *key, size_t klen, void **val, size_t *vlen) {
    if (!db || !key || !klen || !val || !vlen) return TARN_INVALID;
    *val = NULL;
    *vlen = 0;
    if (klen > TARN_MAX_KEY) return TARN_NOT_FOUND;

    const struct islot *s = idx_find(&db->ix, idx_hash(&db->ix, key, klen), key, klen);
    if (!s) return TARN_NOT_FOUND;
    const struct seg *g = seg_by_id(db, s->seg);
    if (!g || !g->f) return TARN_IO;

    unsigned char *buf = malloc(s->len);
    size_t got = 0;
    if (!buf) return TARN_NOMEM;
    if (os_pread(g->f, buf, s->len, s->off, &got)) {
        free(buf);
        return TARN_IO;
    }
    /* checked on every read: a bit flipped on disk since open is caught here */
    if (got != s->len || get32(buf) != crc32c(0, buf + 4, 12) || get32(buf + 4) != klen ||
        memcmp(buf + REC_HDR, key, klen) != 0 || get32(buf + 12) != crc32c(0, buf + REC_HDR, s->len - REC_HDR)) {
        free(buf);
        return TARN_CORRUPT;
    }
    size_t n = s->len - REC_HDR - klen;
    memmove(buf, buf + REC_HDR + klen, n);
    *val = buf;
    *vlen = n;
    return TARN_OK;
}

void tarn_free(void *p) { free(p); }

int tarn_delete(tarn *db, const void *key, size_t klen) {
    if (!db || !key || !klen) return TARN_INVALID;
    if (klen > TARN_MAX_KEY) return TARN_NOT_FOUND;

    struct islot *s = idx_find(&db->ix, idx_hash(&db->ix, key, klen), key, klen);
    if (!s) return TARN_NOT_FOUND;
    uint32_t seg, len;
    uint64_t off;
    int rc = append(db, key, (uint32_t)klen, NULL, TOMBSTONE, &seg, &off, &len);
    if (rc) return rc;
    db->live -= s->len;
    idx_remove(&db->ix, s);
    return TARN_OK;
}

int tarn_sync(tarn *db) {
    if (!db) return TARN_INVALID;
    if (db->failed) return TARN_IO;
    return os_sync(active(db)->f) ? poison(db) : TARN_OK;
}

/* Batches the many small writes of a compaction into large ones. */
struct writer {
    os_file *f;
    unsigned char *buf; /* IO_BUF bytes */
    size_t used;
    uint64_t off;
};

static int wr_flush(struct writer *w) {
    if (w->used && os_pwrite(w->f, w->buf, w->used, w->off)) return -1;
    w->off += w->used;
    w->used = 0;
    return 0;
}

static int wr(struct writer *w, const unsigned char *p, size_t n) {
    while (n) {
        size_t k = IO_BUF - w->used < n ? IO_BUF - w->used : n;
        memcpy(w->buf + w->used, p, k);
        w->used += k;
        p += k;
        n -= k;
        if (w->used == IO_BUF && wr_flush(w)) return -1;
    }
    return 0;
}

/* Copies one record, checking its checksums on the way through. */
static int copy_record(struct reader *r, uint64_t off, uint32_t len, struct writer *w) {
    unsigned char h[REC_HDR];
    size_t got;
    uint32_t crc = 0;
    if (rd(r, off, h, REC_HDR, &got)) return TARN_IO;
    if (got < REC_HDR || get32(h) != crc32c(0, h + 4, 12)) return TARN_CORRUPT;
    if (wr(w, h, REC_HDR)) return TARN_IO;
    for (uint64_t p = off + REC_HDR, stop = off + len; p < stop;) {
        const unsigned char *s;
        size_t n;
        if (rd_span(r, p, chunk(stop - p), &s, &n)) return TARN_IO;
        if (n == 0) return TARN_CORRUPT;
        crc = crc32c(crc, s, n);
        if (wr(w, s, n)) return TARN_IO;
        p += n;
    }
    return crc == get32(h + 12) ? TARN_OK : TARN_CORRUPT;
}

static int by_location(const void *a, const void *b) {
    const struct islot *x = *(const struct islot *const *)a, *y = *(const struct islot *const *)b;
    if (x->seg != y->seg) return x->seg < y->seg ? -1 : 1;
    return (x->off > y->off) - (x->off < y->off);
}

/* The live records in segments up to hi, in file order, so the copy reads
 * every old segment front to back and the output keeps the write order. */
static struct islot **live_records(tarn *db, uint32_t hi, size_t *n) {
    struct islot **v = malloc((db->ix.count ? db->ix.count : 1) * sizeof *v);
    *n = 0;
    if (!v) return NULL;
    for (size_t i = 0; i < db->ix.cap; i++) {
        struct islot *s = &db->ix.slots[i];
        if (s->hash && s->seg <= hi) v[(*n)++] = s;
    }
    if (*n) qsort(v, *n, sizeof *v, by_location);
    return v;
}

static int write_compacted(tarn *db, const char *path, uint32_t lo, uint32_t hi, struct islot **live, size_t n,
                           uint64_t *size) {
    struct reader r;
    struct writer w;
    os_file *out;
    int rc = TARN_OK;

    w.buf = malloc(IO_BUF);
    if (!w.buf) return TARN_NOMEM;
    if (seg_write_new(path, hi, lo, &out)) {
        free(w.buf);
        return TARN_IO;
    }
    w.f = out;
    w.used = 0;
    w.off = SEG_HDR;
    reader_init(&r, NULL, db->io);
    for (size_t i = 0; i < n && rc == TARN_OK; i++) {
        const struct islot *s = live[i];
        const struct seg *g = seg_by_id(db, s->seg);
        if (!g || !g->f) {
            rc = TARN_IO;
            break;
        }
        if (r.f != g->f) reader_init(&r, g->f, db->io);
        rc = copy_record(&r, s->off, s->len, &w);
    }
    if (rc == TARN_OK && (wr_flush(&w) || os_sync(out))) rc = TARN_IO;
    os_close(out);
    free(w.buf);
    *size = w.off;
    return rc;
}

static int reopen_closed(tarn *db, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char *p = seg_path(db, db->segs[i].id);
        int bad = !p || os_open(p, OS_READ, &db->segs[i].f);
        free(p);
        if (bad) return -1;
    }
    return 0;
}

static int commit(tarn *db, const char *tmp, const char *dst, size_t nold, uint32_t lo, uint32_t hi,
                  struct islot **live, size_t n, uint64_t size) {
    /* Windows will not rename over a file that is still open */
    for (size_t i = 0; i < nold; i++) {
        os_close(db->segs[i].f);
        db->segs[i].f = NULL;
    }
    if (os_rename(tmp, dst)) return reopen_closed(db, nold) ? poison(db) : TARN_IO;

    /* Committed: the file at hi now stands for every segment from lo to hi.
       The old files go only once the rename is durable; if they outlive a
       crash, the next open removes them. */
    int durable = os_sync_dir(db->dir) == 0;
    if (durable)
        for (size_t i = 0; i + 1 < nold; i++) drop_file(db, db->segs[i].id);

    struct seg c;
    c.id = hi;
    c.base = lo;
    c.size = size;
    c.f = NULL;
    int opened = os_open(dst, OS_READ, &c.f) == 0;

    uint64_t pos = SEG_HDR;
    for (size_t i = 0; i < n; i++) {
        live[i]->seg = hi;
        live[i]->off = pos;
        pos += live[i]->len;
    }
    db->segs[0] = c;
    db->segs[1] = db->segs[nold];
    db->nsegs = 2;
    return durable && opened ? TARN_OK : poison(db);
}

int tarn_compact(tarn *db) {
    int rc = TARN_OK;
    if (!db) return TARN_INVALID;
    if (db->failed) return TARN_IO;
    if (active(db)->size > SEG_HDR) rc = rotate(db);
    if (rc || db->nsegs < 2) return rc;

    size_t nold = db->nsegs - 1, n = 0;
    uint32_t lo = db->segs[0].id, hi = db->segs[nold - 1].id;
    uint64_t size = 0;
    char *tmp = join(db->dir, COMPACT_TMP), *dst = seg_path(db, hi);
    struct islot **live = live_records(db, hi, &n);
    if (!tmp || !dst || !live)
        rc = TARN_NOMEM;
    else
        rc = write_compacted(db, tmp, lo, hi, live, n, &size);
    if (rc == TARN_OK) rc = commit(db, tmp, dst, nold, lo, hi, live, n, size);
    if (rc != TARN_OK && tmp) os_remove(tmp);
    free(live);
    free(tmp);
    free(dst);
    return rc;
}

int tarn_foreach(tarn *db, int (*fn)(const void *key, size_t klen, void *user), void *user) {
    if (!db || !fn) return TARN_INVALID;
    for (size_t i = 0; i < db->ix.cap; i++) {
        const struct islot *s = &db->ix.slots[i];
        if (!s->hash) continue;
        int r = fn(s->key, s->klen, user);
        if (r) return r;
    }
    return 0;
}

void tarn_get_stats(tarn *db, tarn_stats *out) {
    uint64_t records = 0;
    memset(out, 0, sizeof *out);
    if (!db) return;
    out->keys = db->ix.count;
    out->segments = db->nsegs;
    for (size_t i = 0; i < db->nsegs; i++) {
        out->disk_bytes += db->segs[i].size;
        records += db->segs[i].size - SEG_HDR;
    }
    out->dead_bytes = records - db->live;
    out->recovered_bytes = db->recovered;
    out->corrupt_records = db->corrupt;
}

const char *tarn_strerror(int status) {
    switch (status) {
    case TARN_OK:
        return "ok";
    case TARN_NOT_FOUND:
        return "not found";
    case TARN_IO:
        return "i/o error";
    case TARN_CORRUPT:
        return "checksum mismatch";
    case TARN_NOMEM:
        return "out of memory";
    case TARN_INVALID:
        return "invalid argument";
    case TARN_LOCKED:
        return "directory is in use by another process";
    case TARN_TOO_BIG:
        return "key or value too large";
    }
    return "unknown error";
}
