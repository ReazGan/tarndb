/*
 * tarn: an embedded key-value store that does not lose acknowledged writes.
 *
 * Data lives in append-only segment files. Every record carries a CRC32C, so
 * a record torn by a crash or a bad sector is detected and skipped instead of
 * being returned. An in-memory hash index maps each key to its newest record.
 *
 * A handle is not thread safe; put a mutex around it if threads share it.
 * A directory can be open through one handle at a time.
 */
#ifndef TARN_H
#define TARN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TARN_VERSION "0.1.0"

enum tarn_status {
    TARN_OK = 0,
    TARN_NOT_FOUND,
    TARN_IO,       /* the OS refused a read, write or sync */
    TARN_CORRUPT,  /* a record failed its checksum */
    TARN_NOMEM,
    TARN_INVALID,  /* bad argument */
    TARN_LOCKED,   /* another process has the directory open */
    TARN_TOO_BIG   /* key or value over the limit */
};

enum tarn_sync {
    TARN_SYNC_ALWAYS = 0, /* fsync before every put/delete returns: nothing acknowledged is ever lost */
    TARN_SYNC_NONE        /* leave it to the OS; call tarn_sync() at the points that matter */
};

#define TARN_MAX_KEY ((uint32_t)1 << 16)
#define TARN_MAX_VALUE ((uint32_t)1 << 30)

typedef struct tarn tarn;

typedef struct tarn_options {
    enum tarn_sync sync;
    uint64_t segment_bytes; /* start a new segment file past this size (default 64 MiB) */
} tarn_options;

typedef struct tarn_stats {
    uint64_t keys;
    uint64_t segments;
    uint64_t disk_bytes;
    uint64_t dead_bytes;      /* space held by overwritten or deleted records */
    uint64_t recovered_bytes; /* torn tail cut off when the store was opened */
    uint64_t corrupt_records; /* records skipped because their checksum failed */
} tarn_stats;

tarn_options tarn_default_options(void);

int tarn_open(const char *dir, const tarn_options *opts, tarn **out);
void tarn_close(tarn *db);

int tarn_put(tarn *db, const void *key, size_t klen, const void *val, size_t vlen);

/* On success *val is malloc'd and must be released with tarn_free(). */
int tarn_get(tarn *db, const void *key, size_t klen, void **val, size_t *vlen);
void tarn_free(void *p);

int tarn_delete(tarn *db, const void *key, size_t klen);
int tarn_sync(tarn *db);

/* Rewrites every closed segment keeping only live records. Safe to crash
 * at any point: new files only appear through an atomic rename. */
int tarn_compact(tarn *db);

/* Calls fn for every key, in no particular order. Stop early by returning
 * non-zero from fn; that value is returned. fn may call tarn_get but must
 * not put, delete or compact. */
int tarn_foreach(tarn *db, int (*fn)(const void *key, size_t klen, void *user), void *user);

void tarn_get_stats(tarn *db, tarn_stats *out);
const char *tarn_strerror(int status);

#ifdef __cplusplus
}
#endif

#endif
