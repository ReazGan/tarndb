#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE /* pread, flock and fsync under glibc */
#endif

#include "os.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

struct os_file {
    HANDLE h;
};

struct os_lock {
    HANDLE h;
};

/* Paths are UTF-8 in the API; Windows wants UTF-16. */
static wchar_t *widen(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc((size_t)n * sizeof *w);
    if (w && !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) {
        free(w);
        return NULL;
    }
    return w;
}

static int wrap(HANDLE h, os_file **out) {
    if (h == INVALID_HANDLE_VALUE) return -1;
    os_file *f = malloc(sizeof *f);
    if (!f) {
        CloseHandle(h);
        return -1;
    }
    f->h = h;
    *out = f;
    return 0;
}

int os_open(const char *path, int flags, os_file **out) {
    wchar_t *w = widen(path);
    if (!w) return -1;
    DWORD access = 0;
    if (flags & OS_READ) access |= GENERIC_READ;
    if (flags & OS_WRITE) access |= GENERIC_WRITE;
    HANDLE h = CreateFileW(w, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           (flags & OS_CREATE) ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    return wrap(h, out);
}

static OVERLAPPED at(uint64_t off) {
    OVERLAPPED o;
    memset(&o, 0, sizeof o);
    o.Offset = (DWORD)off;
    o.OffsetHigh = (DWORD)(off >> 32);
    return o;
}

#define CHUNK ((size_t)1 << 30)

int os_pread(os_file *f, void *buf, size_t n, uint64_t off, size_t *got) {
    size_t done = 0;
    while (done < n) {
        DWORD want = (DWORD)(n - done < CHUNK ? n - done : CHUNK), read = 0;
        OVERLAPPED o = at(off + done);
        if (!ReadFile(f->h, (char *)buf + done, want, &read, &o)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            return -1;
        }
        if (read == 0) break;
        done += read;
    }
    *got = done;
    return 0;
}

int os_pwrite(os_file *f, const void *buf, size_t n, uint64_t off) {
    size_t done = 0;
    while (done < n) {
        DWORD want = (DWORD)(n - done < CHUNK ? n - done : CHUNK), wrote = 0;
        OVERLAPPED o = at(off + done);
        if (!WriteFile(f->h, (const char *)buf + done, want, &wrote, &o) || wrote == 0) return -1;
        done += wrote;
    }
    return 0;
}

int os_size(os_file *f, uint64_t *size) {
    LARGE_INTEGER s;
    if (!GetFileSizeEx(f->h, &s)) return -1;
    *size = (uint64_t)s.QuadPart;
    return 0;
}

int os_truncate(os_file *f, uint64_t size) {
    FILE_END_OF_FILE_INFO info;
    info.EndOfFile.QuadPart = (LONGLONG)size;
    return SetFileInformationByHandle(f->h, FileEndOfFileInfo, &info, sizeof info) ? 0 : -1;
}

int os_sync(os_file *f) { return FlushFileBuffers(f->h) ? 0 : -1; }

void os_close(os_file *f) {
    if (!f) return;
    CloseHandle(f->h);
    free(f);
}

int os_mkdir(const char *dir) {
    wchar_t *w = widen(dir);
    if (!w) return -1;
    BOOL ok = CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    free(w);
    return ok ? 0 : -1;
}

int os_rmdir(const char *dir) {
    wchar_t *w = widen(dir);
    BOOL ok = w && RemoveDirectoryW(w);
    free(w);
    return ok ? 0 : -1;
}

int os_rename(const char *from, const char *to) {
    wchar_t *a = widen(from), *b = widen(to);
    BOOL ok = a && b && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(a);
    free(b);
    return ok ? 0 : -1;
}

int os_remove(const char *path) {
    wchar_t *w = widen(path);
    BOOL ok = w && DeleteFileW(w);
    free(w);
    return ok ? 0 : -1;
}

/* NTFS journals directory changes itself and has no directory fsync;
 * MOVEFILE_WRITE_THROUGH already waits for renames. */
int os_sync_dir(const char *dir) {
    (void)dir;
    return 0;
}

int os_list(const char *dir, int (*fn)(const char *name, void *user), void *user) {
    size_t n = strlen(dir);
    char *pattern = malloc(n + 3);
    if (!pattern) return -1;
    memcpy(pattern, dir, n);
    memcpy(pattern + n, "\\*", 3);
    wchar_t *w = widen(pattern);
    free(pattern);
    if (!w) return -1;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int rc = 0;
    do {
        char name[MAX_PATH * 4];
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, (int)sizeof name, NULL, NULL)) continue;
        rc = fn(name, user);
    } while (rc == 0 && FindNextFileW(h, &fd));
    FindClose(h);
    return rc;
}

int os_lock_dir(const char *path, os_lock **out) {
    wchar_t *w = widen(path);
    if (!w) return -1;
    /* no share flags: any other open of this file fails while we hold it */
    HANDLE h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return -1;
    os_lock *l = malloc(sizeof *l);
    if (!l) {
        CloseHandle(h);
        return -1;
    }
    l->h = h;
    *out = l;
    return 0;
}

void os_unlock(os_lock *l) {
    if (!l) return;
    CloseHandle(l->h);
    free(l);
}

int os_random(void *buf, size_t n) {
    return BCRYPT_SUCCESS(BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) ? 0 : -1;
}

#else

#include <dirent.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

struct os_file {
    int fd;
};

struct os_lock {
    int fd;
};

static int wrap(int fd, os_file **out) {
    if (fd < 0) return -1;
    os_file *f = malloc(sizeof *f);
    if (!f) {
        close(fd);
        return -1;
    }
    f->fd = fd;
    *out = f;
    return 0;
}

int os_open(const char *path, int flags, os_file **out) {
    int mode = (flags & OS_WRITE) ? ((flags & OS_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (flags & OS_CREATE) mode |= O_CREAT;
    return wrap(open(path, mode | O_CLOEXEC, 0644), out);
}

int os_pread(os_file *f, void *buf, size_t n, uint64_t off, size_t *got) {
    size_t done = 0;
    while (done < n) {
        ssize_t r = pread(f->fd, (char *)buf + done, n - done, (off_t)(off + done));
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;
        done += (size_t)r;
    }
    *got = done;
    return 0;
}

int os_pwrite(os_file *f, const void *buf, size_t n, uint64_t off) {
    size_t done = 0;
    while (done < n) {
        ssize_t w = pwrite(f->fd, (const char *)buf + done, n - done, (off_t)(off + done));
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        done += (size_t)w;
    }
    return 0;
}

int os_size(os_file *f, uint64_t *size) {
    struct stat st;
    if (fstat(f->fd, &st) != 0) return -1;
    *size = (uint64_t)st.st_size;
    return 0;
}

int os_truncate(os_file *f, uint64_t size) { return ftruncate(f->fd, (off_t)size); }

int os_sync(os_file *f) {
#ifdef __APPLE__
    /* fsync on macOS stops at the drive's own cache; F_FULLFSYNC flushes it */
    if (fcntl(f->fd, F_FULLFSYNC) == 0) return 0;
#endif
    return fsync(f->fd);
}

void os_close(os_file *f) {
    if (!f) return;
    close(f->fd);
    free(f);
}

int os_mkdir(const char *dir) { return (mkdir(dir, 0755) == 0 || errno == EEXIST) ? 0 : -1; }
int os_rmdir(const char *dir) { return rmdir(dir); }
int os_rename(const char *from, const char *to) { return rename(from, to); }
int os_remove(const char *path) { return unlink(path); }

int os_sync_dir(const char *dir) {
    int fd = open(dir, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    close(fd);
    return rc;
}

int os_list(const char *dir, int (*fn)(const char *name, void *user), void *user) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    int rc = 0;
    struct dirent *e;
    while (rc == 0 && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        rc = fn(e->d_name, user);
    }
    closedir(d);
    return rc;
}

int os_lock_dir(const char *path, os_lock **out) {
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    /* flock, not fcntl: fcntl locks never conflict inside one process */
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return -1;
    }
    os_lock *l = malloc(sizeof *l);
    if (!l) {
        close(fd);
        return -1;
    }
    l->fd = fd;
    *out = l;
    return 0;
}

void os_unlock(os_lock *l) {
    if (!l) return;
    close(l->fd);
    free(l);
}

int os_random(void *buf, size_t n) {
    os_file *f;
    size_t got = 0;
    if (os_open("/dev/urandom", OS_READ, &f)) return -1;
    int rc = os_pread(f, buf, n, 0, &got);
    os_close(f);
    return rc == 0 && got == n ? 0 : -1;
}

#endif
