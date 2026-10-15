/* The few file system operations tarn needs, with the same meaning on
 * POSIX and Windows. All functions return 0 on success, -1 on failure. */
#ifndef TARN_OS_H
#define TARN_OS_H

#include <stddef.h>
#include <stdint.h>

typedef struct os_file os_file;
typedef struct os_lock os_lock;

enum { OS_READ = 1, OS_WRITE = 2, OS_CREATE = 4 };

int os_open(const char *path, int flags, os_file **out);
int os_pread(os_file *f, void *buf, size_t n, uint64_t off, size_t *got); /* got < n only at end of file */
int os_pwrite(os_file *f, const void *buf, size_t n, uint64_t off);
int os_size(os_file *f, uint64_t *size);
int os_truncate(os_file *f, uint64_t size);
int os_sync(os_file *f); /* data reaches the disk, not just the OS cache */
void os_close(os_file *f);

int os_mkdir(const char *dir); /* succeeds if it already exists */
int os_rmdir(const char *dir);
int os_rename(const char *from, const char *to); /* atomic, replaces `to` */
int os_remove(const char *path);
int os_sync_dir(const char *dir); /* make creates, renames and removes durable */
/* Calls fn for each regular file in dir; a non-zero return from fn stops the
 * walk and is passed back. */
int os_list(const char *dir, int (*fn)(const char *name, void *user), void *user);

int os_lock_dir(const char *path, os_lock **out); /* exclusive, fails at once if taken */
void os_unlock(os_lock *l);

int os_random(void *buf, size_t n);

#endif
