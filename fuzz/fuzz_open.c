/* libFuzzer target: arbitrary bytes as a segment file, then open, read
 * every key back, write once and close. Nothing here may crash or leak. */
#include "tarn.h"

#include "crc32c.h"
#include "os.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char dir[64], seg[96];

static int read_back(const void *key, size_t klen, void *user) {
    void *v;
    size_t n;
    if (tarn_get(user, key, klen, &v, &n) == TARN_OK) tarn_free(v);
    return 0;
}

static void header(unsigned char *h) {
    static const unsigned char fixed[16] = {'T', 'A', 'R', 'N', 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    uint32_t c = crc32c(0, fixed, 16);
    memcpy(h, fixed, 16);
    for (int i = 0; i < 4; i++) h[16 + i] = (unsigned char)(c >> (8 * i));
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
    unsigned char h[20];
    tarn_options o = tarn_default_options();
    tarn *db;

    if (!dir[0]) {
        snprintf(dir, sizeof dir, "/tmp/tarn-fuzz-%d", (int)getpid());
        snprintf(seg, sizeof seg, "%s/00000001.seg", dir);
        os_mkdir(dir);
    }
    /* first byte picks whether the input gets a valid header in front */
    FILE *f = fopen(seg, "wb");
    if (!f) return 0;
    if (size && (data[0] & 1)) {
        header(h);
        fwrite(h, 1, sizeof h, f);
    }
    if (size) fwrite(data + 1, 1, size - 1, f);
    fclose(f);

    o.sync = TARN_SYNC_NONE;
    o.segment_bytes = 4096;
    if (tarn_open(dir, &o, &db) == TARN_OK) {
        tarn_foreach(db, read_back, db);
        tarn_put(db, "k", 1, "v", 1);
        tarn_close(db);
    }
    return 0;
}
