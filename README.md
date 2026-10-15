# tarn

A small embedded key-value store in C11 that does not lose writes.

Every put is appended to a segment file and fsynced before the call returns.
After a crash, a `kill -9` or a power cut, tarn comes back with every write it
acknowledged and never hands back a half-written one. No dependencies; builds
on Linux, macOS and Windows.

```c
#include <tarn.h>

tarn *db;
if (tarn_open("data", NULL, &db) != TARN_OK) return 1;

tarn_put(db, "user:42", 7, "{\"name\":\"ada\"}", 14);

void *val;
size_t len;
if (tarn_get(db, "user:42", 7, &val, &len) == TARN_OK) {
    fwrite(val, 1, len, stdout);
    tarn_free(val);
}

tarn_delete(db, "user:42", 7);
tarn_close(db);
```

The whole API is in [include/tarn.h](include/tarn.h).

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

This gives you `libtarn`, the `tarn` command line tool and `tarn_bench`.
To use it from another CMake project, `add_subdirectory(tarn)` and link
against `tarn`.

## How it works

**Segments.** Data lives in append-only files named `00000001.seg`,
`00000002.seg` and so on. Records are only ever added at the end of the
newest one; when it passes 64 MiB a new one is started. Nothing is ever
overwritten in place.

**Records.** Each record has a 16 byte header: a CRC32C of the header itself,
the key length, the value length and a CRC32C of the key and value. A delete
is a record with no value. The header checksum is separate on purpose: if a
value gets damaged, the record boundary is still trustworthy and only that
one record is lost instead of everything after it.

**Index.** An in-memory hash table maps every key to the segment and offset
of its newest record, so a get is one read. The table uses linear probing
with backward-shift deletion (no tombstones, lookups stay short as keys come
and go) and keyed SipHash-2-4, so keys picked by an attacker cannot be made
to collide.

**Recovery.** On open every segment is read front to back and every record
is checked. What the checker finds decides what happens:

- A torn record at the very end of the newest segment is what a crash in
  the middle of a write looks like. It is cut off and appends continue
  from there. Its unfinished bytes are never searched for records, so a
  value that happens to contain a copy of a tarn record cannot be mistaken
  for one.
- A record with a sound header but a bad value is skipped. The header still
  says where the next record starts.
- A damaged header hides where the next record starts. tarn slides forward
  a byte at a time until a header and the record behind it both check out,
  so one bad sector does not take the rest of the file with it.
- A zero-filled tail, which some file systems leave after a power cut, is
  treated as torn.

Skipped records are counted in `tarn_stats.corrupt_records`, bytes cut off in
`recovered_bytes`. Values are checked again on every `tarn_get`, so damage
that happens while the store is open is reported as `TARN_CORRUPT`, never
returned as data.

**Compaction.** `tarn_compact` copies every live record out of the old
segments into one new file, then renames it over the newest of them. The
rename is the commit point. The new file's header records the range of
segments it replaces, so if the process dies after the rename but before the
old files are deleted, the next open finds them and finishes the job. A
deleted key never comes back from a half-finished compaction; there is a
test for exactly that.

## Durability

With the default `TARN_SYNC_ALWAYS`, a put or delete that returned `TARN_OK`
is on disk. With `TARN_SYNC_NONE` writes go to the OS cache and you call
`tarn_sync` when you need a checkpoint; a crash can then lose anything after
the last sync.

Some details that matter here:

- New segment files are synced, and so is their directory, before anything
  is written into them.
- On macOS `fsync` only reaches the drive's cache, so tarn uses
  `F_FULLFSYNC`.
- If a write or fsync fails, the handle refuses all further writes. After a
  failed fsync Linux may have dropped the dirty pages and marked them clean,
  so retrying would report success for data that never made it. Close the
  handle and open the store again to see what really is on disk.
- A directory is locked while it is open (`flock`, or an unshared handle on
  Windows), so two processes cannot write to it at once.

## Testing

The test suite does more than round-trip some keys:

- writes a record, then cuts the file at **every byte** of it and checks that
  each cut recovers cleanly with all earlier data intact
- flips a bit in **every byte** of a record in the middle of a segment and
  checks that only that record is lost
- SIGKILLs a writer at different points and checks that every put it saw
  acknowledged is there afterwards, byte for byte (Linux and macOS)
- rebuilds the state a crash leaves halfway through compaction, and the
  state a delete blocked by another process leaves, and checks that nothing
  deleted comes back
- tears a write whose value carries a valid record image and checks that
  the image is never taken for a record
- runs the hash table through 200,000 random inserts and removes against a
  plain array
- pins CRC32C and SipHash to published test vectors, and the SSE4.2/ARM
  CRC path to the portable one

CI runs it on Linux, macOS and Windows with warnings as errors, again under
AddressSanitizer and UBSan, and fuzzes the recovery code with libFuzzer.

## Command line

```
tarn DIR put KEY VALUE
tarn DIR get KEY
tarn DIR del KEY
tarn DIR keys
tarn DIR check      read back every value and verify it
tarn DIR stat
tarn DIR compact
```

## Performance

`tarn_bench` with a million 16 byte keys and 100 byte values, on a Ryzen 9
9900X with an NVMe SSD under Windows 11:

| operation | rate |
|---|---|
| put, one sync at the end | 537k/s |
| get, random order | 346k/s |
| open (scan and verify 1M records) | 5.7M records/s |
| compact | 2.6M records/s |
| put, fsync on every write | 5.9k/s |

The last line is the real cost of durability: one fsync per write. Batch
your writes with `TARN_SYNC_NONE` and `tarn_sync` if you need more.

## Limits

- Every key has to fit in memory, plus roughly 100 bytes of index per key.
  Values stay on disk.
- Keys up to 64 KiB, values up to 1 GiB.
- One handle per directory. A handle is not thread safe; put a mutex
  around it if threads share it.
- When the disk damages a record header, recovery searches forward for the
  next intact record. If the damaged record's value holds a byte-exact copy
  of a tarn record, that copy can be read back as a record.
- No range scans: keys are hashed, not sorted.

## License

MIT
