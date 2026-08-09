/* Glasspole — everything the emulator needs from the operating system beneath
 * it, and nothing else.
 *
 * THE POINT OF THIS FILE
 * ----------------------
 * The syscall layer never calls a host syscall. It calls these, and a backend
 * implements them: host_posix.c on Linux, host_win32.c on Windows.
 *
 * The interface is shaped to what WIN32 can do, not to what Linux can do. That
 * is deliberate and it is the whole reason the design works. A Linux backend
 * that forwarded open() to open() would compile, pass every test, and teach us
 * nothing — and the day the Win32 backend was written we would discover the
 * interface had assumed fd numbers, unlink-while-open, 4 KB granularity and
 * clone(), none of which survive the crossing.
 *
 * So the constraints are honoured HERE, on the side that has an oracle:
 *
 *   - address space is reserved and committed separately, at 64 KB granularity
 *   - files are opaque handles, never integers with inheritance rules attached
 *   - threads are created, not cloned
 *   - waiting is done on an address, with no assumption of a futex word
 *
 * ERRORS ARE NEGATIVE LINUX ERRNO. Every function that can fail returns
 * <0 on failure, and the value is the errno the GUEST should see — the guest is
 * a Linux program and expects Linux numbers. Turning GetLastError() into one of
 * these is the Win32 backend's job. No OS-specific error logic belongs in the
 * syscall layer, which is why none of it can reach there.
 *
 * PATHS ARE UTF-8 HOST PATHS. Guest path resolution — the sysroot rewrite,
 * synthesizing /proc, the shim's device names — is host-independent logic and
 * lives in the syscall layer. By the time a path arrives here it names a real
 * file on the real filesystem. The Win32 backend widens to UTF-16 and calls the
 * W entry points; it does not have to know what /LF/Bulk means.
 */
#ifndef GLASSPOLE_HOST_H
#define GLASSPOLE_HOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- errors ------------------------------------------------------------- */
/* The subset the census actually produced, as Linux/ARM sees them. Add on
 * demand; a number invented here that disagrees with uClibc's headers is a bug
 * that presents as the guest handling the wrong error. */
#define GP_EPERM    -1
#define GP_ENOENT   -2
#define GP_EINTR    -4
#define GP_EIO      -5
#define GP_EBADF    -9
#define GP_EAGAIN   -11
#define GP_ENOMEM   -12
#define GP_EACCES   -13
#define GP_EFAULT   -14
#define GP_EBUSY    -16
#define GP_EEXIST   -17
#define GP_ENODEV   -19
#define GP_ENOTDIR  -20
#define GP_EISDIR   -21
#define GP_EINVAL   -22
#define GP_ENFILE   -23
#define GP_EMFILE   -24
#define GP_ENOTTY   -25
#define GP_ENOSPC   -28
#define GP_ESPIPE   -29
#define GP_EROFS    -30
#define GP_ENOSYS   -38
#define GP_ENOTEMPTY -39
#define GP_ETIMEDOUT -110

/* ---- address space ------------------------------------------------------ */
/*
 * Guest memory is ONE reservation, and we hand pieces of it out ourselves. The
 * alternative — one host mapping per guest mmap — is how you end up needing
 * VirtualAlloc2 and MapViewOfFile3 to place a mapping inside a region you
 * already own, and those are Windows 10 1803+. Doing it this way costs us
 * nothing and buys back every Windows since 7.
 *
 * GP_ALLOC_GRAN is 64 KB because that is Windows' allocation granularity, and
 * it is honoured on Linux too. A guest layout that only works at 4 KB
 * granularity is a bug we want to find here rather than there.
 */
#define GP_ALLOC_GRAN 65536u
#define GP_PAGE       4096u

#define GP_PROT_NONE  0
#define GP_PROT_READ  1
#define GP_PROT_WRITE 2
#define GP_PROT_EXEC  4

/* Reserve `size` bytes of address space without committing any of it. `at` is
 * a hint, or 0 for anywhere. Returns the base, or NULL. */
void *gp_reserve(void *at, size_t size);

/* Commit and protect part of a reservation. `addr` and `len` must be page
 * aligned; `addr` must lie inside a live reservation. */
int   gp_commit(void *addr, size_t len, int prot);
int   gp_protect(void *addr, size_t len, int prot);

/* Return committed pages to the reservation without releasing the address
 * space. This is what a guest munmap() becomes: the range must stay reserved,
 * or a later guest mmap could be handed it by the host allocator behind our
 * back. */
int   gp_decommit(void *addr, size_t len);

/* Release an entire reservation. `size` must match the gp_reserve call, which
 * Win32's MEM_RELEASE requires and Linux does not care about — so we pass it
 * everywhere and the Linux backend ignores it. */
int   gp_release(void *base, size_t size);

/* ---- files -------------------------------------------------------------- */
/*
 * gp_file is opaque on purpose. On Linux it wraps an int, on Windows a HANDLE,
 * and NEITHER is visible to the caller — the guest's fd table is ours, built
 * from these, so guest fd 3 has no relationship to anything the host numbered.
 * That also means dup(), inheritance across (non-existent) forks and the
 * "lowest free descriptor" rule are ours to define rather than to inherit.
 */
typedef struct gp_file gp_file;

#define GP_O_RDONLY   0x0000
#define GP_O_WRONLY   0x0001
#define GP_O_RDWR     0x0002
#define GP_O_CREAT    0x0040
#define GP_O_EXCL     0x0080
#define GP_O_TRUNC    0x0200
#define GP_O_APPEND   0x0400
#define GP_O_DIRECTORY 0x10000

/* Deliberately narrower than struct stat: this is what the census shows the
 * guest actually reads back, and a field we do not carry is a field the Win32
 * backend cannot get wrong. Times are nanoseconds since the Unix epoch. */
struct gp_stat {
    uint64_t size;
    uint64_t mtime_ns;
    uint32_t mode;      /* Linux S_IF* bits plus permissions */
    uint32_t is_dir;
};

int   gp_open(const char *utf8_path, int flags, uint32_t mode, gp_file **out);
int   gp_close(gp_file *f);

/* Short reads and writes are reported, not retried — the guest's libc is
 * responsible for looping, exactly as it would be on real hardware. */
int64_t gp_read (gp_file *f, void *buf, size_t len);
int64_t gp_write(gp_file *f, const void *buf, size_t len);

/* Positional I/O, so a mapping refill or a readahead never disturbs the file
 * position the guest believes in. Windows has no pread; the backend saves and
 * restores, under its own lock. */
int64_t gp_pread(gp_file *f, void *buf, size_t len, uint64_t off);

#define GP_SEEK_SET 0
#define GP_SEEK_CUR 1
#define GP_SEEK_END 2
int64_t gp_seek(gp_file *f, int64_t off, int whence);

int   gp_stat (const char *utf8_path, struct gp_stat *st);
int   gp_fstat(gp_file *f, struct gp_stat *st);
int   gp_truncate(gp_file *f, uint64_t size);
int   gp_sync(gp_file *f);

int   gp_mkdir (const char *utf8_path, uint32_t mode);
int   gp_rmdir (const char *utf8_path);
int   gp_unlink(const char *utf8_path);
int   gp_rename(const char *from_utf8, const char *to_utf8);

/* Directory enumeration, one entry per call, because getdents64 is the only
 * consumer and it is happiest filling its buffer incrementally. `name` points
 * into storage owned by the iterator and dies with the next call.
 * Returns 1 on an entry, 0 at the end, <0 on error. */
typedef struct gp_dir gp_dir;
int   gp_diropen(const char *utf8_path, gp_dir **out);
int   gp_dirnext(gp_dir *d, const char **name, uint32_t *is_dir);
int   gp_dirclose(gp_dir *d);

/* ---- threads ------------------------------------------------------------ */
/*
 * Every clone() in the census was the standard NPTL set —
 * CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|CLONE_SETTLS —
 * i.e. "make a thread". No process creation anywhere, which is why this is a
 * thread interface and not a clone() emulation. If a title ever does fork(),
 * that is a design conversation, not a missing flag.
 */
typedef struct gp_thread gp_thread;

int   gp_thread_create(void (*entry)(void *), void *arg, gp_thread **out);
int   gp_thread_join(gp_thread *t);
int   gp_thread_detach(gp_thread *t);
void  gp_thread_exit(void);
void  gp_thread_yield(void);
uint32_t gp_thread_id(void);   /* stable per thread, used for the guest's tid */

/* ---- waiting ------------------------------------------------------------ */
/*
 * futex, reduced to what it is actually used for. NOT WaitOnAddress: that is
 * Windows 8 and up, and an address-keyed table of CONDITION_VARIABLE + SRWLOCK
 * is about sixty lines and works on Vista. Sixty lines is a cheap price for
 * three Windows versions.
 *
 * gp_wait_on re-checks *addr == expected under the same lock gp_wake takes, so
 * the classic lost-wakeup between the guest's compare and its wait cannot
 * happen. timeout_ns of 0 means wait forever.
 * Returns 0 if woken, GP_EAGAIN if the value had already changed,
 * GP_ETIMEDOUT on timeout.
 */
int   gp_wait_on(volatile uint32_t *addr, uint32_t expected, uint64_t timeout_ns);
int   gp_wake(volatile uint32_t *addr, int count);

/* ---- time --------------------------------------------------------------- */
/*
 * gettimeofday is 85% of every syscall the guest makes — 381,096 of 445,553 in
 * the Cars 2 census — so this is the one host call on a hot path. It must not
 * take a lock and must not allocate. Everything else here is cold.
 */
uint64_t gp_wall_ns(void);   /* since the Unix epoch, for gettimeofday */
uint64_t gp_mono_ns(void);   /* arbitrary origin, never steps backwards */
void     gp_sleep_ns(uint64_t ns);

/* ---- odds and ends ------------------------------------------------------ */
int   gp_random(void *buf, size_t len);          /* /dev/urandom */

/* The guest's stdout and stderr. Separate from gp_write because on Windows a
 * console handle is not a file handle and the difference matters at exactly one
 * place — here. */
int64_t gp_console_write(int fd, const void *buf, size_t len);

/* Diagnostics from the emulator itself, never from the guest. Goes to the
 * host's stderr; the guest cannot reach it. */
void  gp_log(const char *fmt, ...);

#ifdef __cplusplus
}
#endif
#endif /* GLASSPOLE_HOST_H */
