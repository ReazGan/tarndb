#include "tarn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void) {
    fputs("usage: tarn DIR put KEY VALUE\n"
          "       tarn DIR get KEY\n"
          "       tarn DIR del KEY\n"
          "       tarn DIR keys\n"
          "       tarn DIR check\n"
          "       tarn DIR stat\n"
          "       tarn DIR compact\n",
          stderr);
}

static int print_key(const void *key, size_t klen, void *user) {
    (void)user;
    fwrite(key, 1, klen, stdout);
    fputc('\n', stdout);
    return 0;
}

struct check {
    tarn *db;
    uint64_t ok, bad;
};

/* Reads every value back; tarn_get verifies the checksum each time. */
static int check_key(const void *key, size_t klen, void *user) {
    struct check *c = user;
    void *v;
    size_t n;
    int rc = tarn_get(c->db, key, klen, &v, &n);
    if (rc == TARN_OK) {
        c->ok++;
        tarn_free(v);
        return 0;
    }
    c->bad++;
    fprintf(stderr, "%.*s: %s\n", (int)klen, (const char *)key, tarn_strerror(rc));
    return rc == TARN_CORRUPT ? 0 : rc;
}

static int arity(const char *cmd) {
    if (!strcmp(cmd, "put")) return 2;
    if (!strcmp(cmd, "get") || !strcmp(cmd, "del")) return 1;
    if (!strcmp(cmd, "keys") || !strcmp(cmd, "check") || !strcmp(cmd, "stat") || !strcmp(cmd, "compact")) return 0;
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 3 || arity(argv[2]) != argc - 3) {
        usage();
        return 2;
    }
    const char *cmd = argv[2];
    tarn *db;
    int rc = tarn_open(argv[1], NULL, &db);
    if (rc) {
        fprintf(stderr, "tarn: %s: %s\n", argv[1], tarn_strerror(rc));
        return 1;
    }

    if (!strcmp(cmd, "put")) {
        rc = tarn_put(db, argv[3], strlen(argv[3]), argv[4], strlen(argv[4]));
    } else if (!strcmp(cmd, "get")) {
        void *v;
        size_t n;
        rc = tarn_get(db, argv[3], strlen(argv[3]), &v, &n);
        if (rc == TARN_OK) {
            fwrite(v, 1, n, stdout);
            fputc('\n', stdout);
            tarn_free(v);
        }
    } else if (!strcmp(cmd, "del")) {
        rc = tarn_delete(db, argv[3], strlen(argv[3]));
    } else if (!strcmp(cmd, "keys")) {
        rc = tarn_foreach(db, print_key, NULL);
    } else if (!strcmp(cmd, "check")) {
        struct check c = {db, 0, 0};
        rc = tarn_foreach(db, check_key, &c);
        printf("%llu ok, %llu bad\n", (unsigned long long)c.ok, (unsigned long long)c.bad);
        if (!rc && c.bad) rc = TARN_CORRUPT;
    } else if (!strcmp(cmd, "stat")) {
        tarn_stats s;
        tarn_get_stats(db, &s);
        printf("keys       %llu\n"
               "segments   %llu\n"
               "disk       %llu bytes\n"
               "dead       %llu bytes\n"
               "recovered  %llu bytes\n"
               "corrupt    %llu records\n",
               (unsigned long long)s.keys, (unsigned long long)s.segments, (unsigned long long)s.disk_bytes,
               (unsigned long long)s.dead_bytes, (unsigned long long)s.recovered_bytes,
               (unsigned long long)s.corrupt_records);
    } else {
        rc = tarn_compact(db);
    }

    tarn_close(db);
    if (rc) {
        fprintf(stderr, "tarn: %s\n", tarn_strerror(rc));
        return 1;
    }
    return 0;
}
