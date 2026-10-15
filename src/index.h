#ifndef TARN_INDEX_H
#define TARN_INDEX_H

#include <stddef.h>
#include <stdint.h>

/*
 * Key -> newest record. Open addressing with linear probing; a delete
 * shifts the rest of its run back instead of leaving a tombstone, so
 * lookups stay short no matter how many keys come and go.
 */

struct islot {
    uint64_t hash; /* 0 marks an empty slot */
    char *key;
    uint32_t klen;
    uint32_t seg; /* segment holding the record */
    uint64_t off; /* where in that segment */
    uint32_t len; /* whole record, header included */
};

struct idx {
    struct islot *slots;
    size_t cap; /* power of two */
    size_t count;
    unsigned char seed[16];
};

int idx_init(struct idx *ix, const unsigned char seed[16]);
void idx_free(struct idx *ix);
uint64_t idx_hash(const struct idx *ix, const void *key, size_t klen);
struct islot *idx_find(struct idx *ix, uint64_t hash, const void *key, size_t klen);

/* Makes room for one more key. Kept apart from idx_insert so a caller can
 * fail early, before it writes anything to disk. */
int idx_reserve(struct idx *ix);

/* The key must be absent and idx_reserve must have succeeded since the last
 * insert. Takes ownership of key. */
void idx_insert(struct idx *ix, uint64_t hash, char *key, uint32_t klen, uint32_t seg, uint64_t off, uint32_t len);

void idx_remove(struct idx *ix, struct islot *s);

#endif
