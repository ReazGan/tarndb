#include "index.h"

#include "siphash.h"

#include <stdlib.h>
#include <string.h>

#define MIN_CAP 64

int idx_init(struct idx *ix, const unsigned char seed[16]) {
    memset(ix, 0, sizeof *ix);
    memcpy(ix->seed, seed, sizeof ix->seed);
    ix->slots = calloc(MIN_CAP, sizeof *ix->slots);
    if (!ix->slots) return -1;
    ix->cap = MIN_CAP;
    return 0;
}

void idx_free(struct idx *ix) {
    for (size_t i = 0; i < ix->cap; i++) free(ix->slots[i].key);
    free(ix->slots);
    ix->slots = NULL;
    ix->cap = ix->count = 0;
}

uint64_t idx_hash(const struct idx *ix, const void *key, size_t klen) {
    uint64_t h = siphash24(ix->seed, key, klen);
    return h ? h : 1;
}

struct islot *idx_find(struct idx *ix, uint64_t hash, const void *key, size_t klen) {
    size_t mask = ix->cap - 1;
    for (size_t i = (size_t)hash & mask;; i = (i + 1) & mask) {
        struct islot *s = &ix->slots[i];
        if (!s->hash) return NULL;
        if (s->hash == hash && s->klen == klen && memcmp(s->key, key, klen) == 0) return s;
    }
}

static void place(struct islot *slots, size_t mask, const struct islot *e) {
    size_t i = (size_t)e->hash & mask;
    while (slots[i].hash) i = (i + 1) & mask;
    slots[i] = *e;
}

int idx_reserve(struct idx *ix) {
    if ((ix->count + 1) * 4 <= ix->cap * 3) return 0;
    size_t cap = ix->cap * 2;
    struct islot *slots = calloc(cap, sizeof *slots);
    if (!slots) return -1;
    for (size_t i = 0; i < ix->cap; i++)
        if (ix->slots[i].hash) place(slots, cap - 1, &ix->slots[i]);
    free(ix->slots);
    ix->slots = slots;
    ix->cap = cap;
    return 0;
}

void idx_insert(struct idx *ix, uint64_t hash, char *key, uint32_t klen, uint32_t seg, uint64_t off, uint32_t len) {
    struct islot e;
    e.hash = hash;
    e.key = key;
    e.klen = klen;
    e.seg = seg;
    e.off = off;
    e.len = len;
    place(ix->slots, ix->cap - 1, &e);
    ix->count++;
}

void idx_remove(struct idx *ix, struct islot *s) {
    size_t mask = ix->cap - 1;
    size_t hole = (size_t)(s - ix->slots);
    free(s->key);
    for (size_t j = (hole + 1) & mask; ix->slots[j].hash; j = (j + 1) & mask) {
        size_t home = (size_t)ix->slots[j].hash & mask;
        /* The entry at j may move into the hole only if the hole sits on its
           probe path, somewhere from its home slot up to j. */
        if (((j - home) & mask) >= ((j - hole) & mask)) {
            ix->slots[hole] = ix->slots[j];
            hole = j;
        }
    }
    memset(&ix->slots[hole], 0, sizeof ix->slots[hole]);
    ix->count--;
}
