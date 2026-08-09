/* Glasspole — the Linux backend of host.h.
 *
 * Written to the Win32 contract on purpose. Where Linux would let us be lazy
 * we are not, because the laziness is what would not survive the crossing:
 * reservations are separate from commits, handles are boxed rather than being
 * bare ints, and every mapping is 64 KB aligned even though Linux would take
 * 4 KB. See the header for why.
 */
#define _GNU_SOURCE
#include "host.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <linux/futex.h>

/* Host errno -> the number the guest should see. They agree on Linux, so this
 * is the identity — but it goes through a function anyway so that the call
 * sites read the same in both backends and the Win32 one has somewhere
 * obvious to put its GetLastError() table. */
static int err(void) { return -errno; }

/* ---- address space ------------------------------------------------------ */

void *gp_reserve(void *at, size_t size) {
    /* PROT_NONE + MAP_NORESERVE is Linux's reservation: address space claimed,
     * nothing committed, and a touch faults until gp_commit says otherwise. */
    void *p = mmap(at, size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if (at && p != at) { munmap(p, size); return NULL; }
    return p;
}

static int prot_to_posix(int prot) {
    int p = 0;
    if (prot & GP_PROT_READ)  p |= PROT_READ;
    if (prot & GP_PROT_WRITE) p |= PROT_WRITE;
    if (prot & GP_PROT_EXEC)  p |= PROT_EXEC;
    return p ? p : PROT_NONE;
}

int gp_commit(void *addr, size_t len, int prot) {
    return mprotect(addr, len, prot_to_posix(prot)) == 0 ? 0 : err();
}

int gp_protect(void *addr, size_t len, int prot) {
    return mprotect(addr, len, prot_to_posix(prot)) == 0 ? 0 : err();
}

int gp_decommit(void *addr, size_t len) {
    /* MADV_DONTNEED drops the pages; mprotect PROT_NONE keeps the range ours.
     * Doing only the first would leave the address space reservable by the host
     * allocator, which is exactly the bug this interface exists to prevent. */
    if (madvise(addr, len, MADV_DONTNEED) != 0) return err();
    return mprotect(addr, len, PROT_NONE) == 0 ? 0 : err();
}

int gp_release(void *base, size_t size) {
    return munmap(base, size) == 0 ? 0 : err();
}

/* ---- files -------------------------------------------------------------- */

struct gp_file { int fd; };

static int flags_to_posix(int f) {
    int o = 0;
    switch (f & 3) {
        case GP_O_WRONLY: o = O_WRONLY; break;
        case GP_O_RDWR:   o = O_RDWR;   break;
        default:          o = O_RDONLY; break;
    }
    if (f & GP_O_CREAT)     o |= O_CREAT;
    if (f & GP_O_EXCL)      o |= O_EXCL;
    if (f & GP_O_TRUNC)     o |= O_TRUNC;
    if (f & GP_O_APPEND)    o |= O_APPEND;
    if (f & GP_O_DIRECTORY) o |= O_DIRECTORY;
    return o;
}

int gp_open(const char *path, int flags, uint32_t mode, gp_file **out) {
    int fd = open(path, flags_to_posix(flags) | O_CLOEXEC, (mode_t)mode);
    if (fd < 0) return err();
    gp_file *f = calloc(1, sizeof *f);
    if (!f) { close(fd); return GP_ENOMEM; }
    f->fd = fd;
    *out = f;
    return 0;
}

int gp_close(gp_file *f) {
    if (!f) return GP_EBADF;
    int r = close(f->fd) == 0 ? 0 : err();
    free(f);
    return r;
}

int64_t gp_read(gp_file *f, void *buf, size_t len) {
    ssize_t n = read(f->fd, buf, len);
    return n < 0 ? err() : n;
}

int64_t gp_write(gp_file *f, const void *buf, size_t len) {
    ssize_t n = write(f->fd, buf, len);
    return n < 0 ? err() : n;
}

int64_t gp_pread(gp_file *f, void *buf, size_t len, uint64_t off) {
    ssize_t n = pread(f->fd, buf, len, (off_t)off);
    return n < 0 ? err() : n;
}

int64_t gp_seek(gp_file *f, int64_t off, int whence) {
    int w = whence == GP_SEEK_CUR ? SEEK_CUR
          : whence == GP_SEEK_END ? SEEK_END : SEEK_SET;
    off_t n = lseek(f->fd, (off_t)off, w);
    return n < 0 ? err() : n;
}

static void fill_stat(const struct stat *s, struct gp_stat *st) {
    st->size     = (uint64_t)s->st_size;
    st->mtime_ns = (uint64_t)s->st_mtim.tv_sec * 1000000000ull + s->st_mtim.tv_nsec;
    st->mode     = s->st_mode;
    st->is_dir   = S_ISDIR(s->st_mode) ? 1 : 0;
}

int gp_stat(const char *path, struct gp_stat *st) {
    struct stat s;
    if (stat(path, &s) != 0) return err();
    fill_stat(&s, st);
    return 0;
}

int gp_fstat(gp_file *f, struct gp_stat *st) {
    struct stat s;
    if (fstat(f->fd, &s) != 0) return err();
    fill_stat(&s, st);
    return 0;
}

int gp_truncate(gp_file *f, uint64_t size) {
    return ftruncate(f->fd, (off_t)size) == 0 ? 0 : err();
}

int gp_sync(gp_file *f) {
    return fdatasync(f->fd) == 0 ? 0 : err();
}

int gp_mkdir (const char *p, uint32_t m) { return mkdir(p, (mode_t)m) == 0 ? 0 : err(); }
int gp_rmdir (const char *p)             { return rmdir(p)            == 0 ? 0 : err(); }
int gp_unlink(const char *p)             { return unlink(p)           == 0 ? 0 : err(); }
int gp_rename(const char *a, const char *b) { return rename(a, b)     == 0 ? 0 : err(); }

struct gp_dir { DIR *d; };

int gp_diropen(const char *path, gp_dir **out) {
    DIR *d = opendir(path);
    if (!d) return err();
    gp_dir *g = calloc(1, sizeof *g);
    if (!g) { closedir(d); return GP_ENOMEM; }
    g->d = d;
    *out = g;
    return 0;
}

int gp_dirnext(gp_dir *g, const char **name, uint32_t *is_dir) {
    errno = 0;
    struct dirent *e = readdir(g->d);
    if (!e) return errno ? err() : 0;
    *name   = e->d_name;
    *is_dir = e->d_type == DT_DIR;
    return 1;
}

int gp_dirclose(gp_dir *g) {
    int r = closedir(g->d) == 0 ? 0 : err();
    free(g);
    return r;
}

/* ---- threads ------------------------------------------------------------ */

struct gp_thread { pthread_t t; void (*entry)(void *); void *arg; };

static void *thread_trampoline(void *p) {
    gp_thread *t = p;
    t->entry(t->arg);
    return NULL;
}

int gp_thread_create(void (*entry)(void *), void *arg, gp_thread **out) {
    gp_thread *t = calloc(1, sizeof *t);
    if (!t) return GP_ENOMEM;
    t->entry = entry;
    t->arg   = arg;
    int r = pthread_create(&t->t, NULL, thread_trampoline, t);
    if (r != 0) { free(t); return -r; }
    *out = t;
    return 0;
}

int gp_thread_join(gp_thread *t) {
    int r = pthread_join(t->t, NULL);
    if (r == 0) free(t);
    return -r;
}

int gp_thread_detach(gp_thread *t) {
    int r = pthread_detach(t->t);
    if (r == 0) free(t);
    return -r;
}

void gp_thread_exit(void)  { pthread_exit(NULL); }
void gp_thread_yield(void) { sched_yield(); }

uint32_t gp_thread_id(void) { return (uint32_t)syscall(SYS_gettid); }

/* ---- waiting ------------------------------------------------------------ */

int gp_wait_on(volatile uint32_t *addr, uint32_t expected, uint64_t timeout_ns) {
    struct timespec ts, *tp = NULL;
    if (timeout_ns) {
        ts.tv_sec  = (time_t)(timeout_ns / 1000000000ull);
        ts.tv_nsec = (long)  (timeout_ns % 1000000000ull);
        tp = &ts;
    }
    long r = syscall(SYS_futex, (uint32_t *)addr, FUTEX_WAIT_PRIVATE,
                     expected, tp, NULL, 0);
    if (r == 0) return 0;
    if (errno == EAGAIN)    return GP_EAGAIN;
    if (errno == ETIMEDOUT) return GP_ETIMEDOUT;
    if (errno == EINTR)     return 0;   /* a spurious wake is a legal wake */
    return err();
}

int gp_wake(volatile uint32_t *addr, int count) {
    long r = syscall(SYS_futex, (uint32_t *)addr, FUTEX_WAKE_PRIVATE,
                     count, NULL, NULL, 0);
    return r < 0 ? err() : (int)r;
}

/* ---- time --------------------------------------------------------------- */
/* The hot one. 381,096 of 445,553 syscalls in the census were gettimeofday, so
 * no lock and no allocation may appear below this line. */

uint64_t gp_wall_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t gp_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void gp_sleep_ns(uint64_t ns) {
    struct timespec ts = { (time_t)(ns / 1000000000ull),
                           (long)  (ns % 1000000000ull) };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { }
}

/* ---- odds and ends ------------------------------------------------------ */

int gp_random(void *buf, size_t len) {
    gp_file *f;
    int r = gp_open("/dev/urandom", GP_O_RDONLY, 0, &f);
    if (r < 0) return r;
    int64_t n = gp_read(f, buf, len);
    gp_close(f);
    return n < 0 ? (int)n : (n == (int64_t)len ? 0 : GP_EIO);
}

int64_t gp_console_write(int fd, const void *buf, size_t len) {
    ssize_t n = write(fd == 2 ? 2 : 1, buf, len);
    return n < 0 ? err() : n;
}

void gp_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("[glasspole] ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
