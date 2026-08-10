/* Glasspole — the Win32 backend of host.h.
 *
 * The other half of the bet host.h makes: everything above it is untouched, and
 * every Linux-ism ends here. GetLastError() becomes a negative Linux errno in
 * exactly one function; UTF-8 paths widen to UTF-16 and go to the W entry
 * points, never the A ones; and nothing below calls anything newer than
 * Windows 7 SP1 — which concretely means no WaitOnAddress (Win8) and no
 * VirtualAlloc2 (Win10 1803), each avoided the way host.h's comments already
 * prescribe.
 *
 * MEASURED, NOT ASSUMED: on a synchronous handle, ReadFile with an OVERLAPPED
 * offset honours the offset but MOVES the handle's file pointer afterwards
 * (verified on this machine, and MSDN agrees: "the system updates the
 * OVERLAPPED offset and the file pointer"). So gp_pread takes the offset from
 * OVERLAPPED — one seek fewer than seek/read/seek-back — but still saves and
 * restores the pointer under the file's lock, or a mapping refill would move
 * the position the guest believes in.
 */
#define WIN32_LEAN_AND_MEAN
#include "host.h"

#include <windows.h>
#include <timeapi.h>   /* timeBeginPeriod; LEAN_AND_MEAN strips it from windows.h */
#include <bcrypt.h>
#include <process.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- GetLastError() -> the number the guest should see -------------------
 * The one table the whole interface exists to contain. Grown from what the
 * calls in this file can actually raise; default EIO, because an error we have
 * not classified is closer to "the device did something wrong" than to any
 * more specific claim. */
static int err_from(DWORD e)
{
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:          return GP_ENOENT;
    case ERROR_ACCESS_DENIED:        return GP_EACCES;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_BUSY:                 return GP_EBUSY;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:       return GP_EEXIST;
    case ERROR_DIR_NOT_EMPTY:        return GP_ENOTEMPTY;
    case ERROR_DIRECTORY:            return GP_ENOTDIR;
    case ERROR_INVALID_HANDLE:       return GP_EBADF;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case ERROR_COMMITMENT_LIMIT:     return GP_ENOMEM;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_FUNCTION:
    case ERROR_NEGATIVE_SEEK:        return GP_EINVAL;
    case ERROR_HANDLE_DISK_FULL:
    case ERROR_DISK_FULL:            return GP_ENOSPC;
    case ERROR_WRITE_PROTECT:        return GP_EROFS;
    case ERROR_TOO_MANY_OPEN_FILES:  return GP_EMFILE;
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED: return GP_ENOSYS;
    case ERROR_INVALID_ADDRESS:      return GP_EFAULT;
    case ERROR_OPERATION_ABORTED:    return GP_EINTR;
    default:                         return GP_EIO;
    }
}
static int err(void) { return err_from(GetLastError()); }

/* ---- paths ---------------------------------------------------------------
 * UTF-8 in, UTF-16 out, W entry points only. MB_ERR_INVALID_CHARS on purpose:
 * a byte sequence that is not UTF-8 names no file this backend can reach, and
 * ENOENT is the honest answer rather than whatever the ANSI codepage would
 * have guessed. */
#define GP_WPATH 4096
static int widen(const char *u8, WCHAR *w, int cap)
{
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u8, -1, w, cap))
        return GP_ENOENT;
    return 0;
}

/* ---- address space ------------------------------------------------------- */

/* THE RESERVATION IS CHUNKED, and that is what makes real shared mappings
 * possible on Windows 7. A single 4 GiB MEM_RESERVE cannot be broken up —
 * MEM_RELEASE frees all of it or none, and MapViewOfFileEx cannot place a
 * view inside it. Pre-1803 Windows has no placeholders. So the guest's space
 * is reserved as one 64 KB VirtualAlloc per chunk: same address span, same
 * protection against the host allocator, but any chunk can individually be
 * released and a file view mapped over it. That is how gp_map_shared gets
 * genuine aliasing — Pet Pad maps its framebuffer at two guest addresses and
 * renders through one while reading the other, which no private copy can
 * reproduce.
 *
 * The cost: one syscall per chunk at reserve time (65,536 for 4 GiB, tens of
 * milliseconds, once), and commit/protect/decommit must walk chunk by chunk
 * because none of the Virtual* calls may span separate allocations.
 *
 * ONE live chunked reservation is supported, because exactly one exists — the
 * guest space. A second gp_reserve while one is live falls back to a plain
 * unsplittable reservation and says so.
 *
 * The release-then-map window in gp_map_shared is a real race against any
 * other thread of OUR process allocating (dynarmic's code cache included).
 * It is accepted: the guest maps its framebuffers during early boot, the
 * window is microseconds, and this is how emulators lived before Windows 10.
 * A lost race fails loudly rather than mapping elsewhere. */
#define GP_CHUNK       GP_ALLOC_GRAN
#define GP_MAX_VIEWS   32

static uint8_t *g_res_base;
static size_t   g_res_chunks;
static uint8_t *g_chunk_is_view;    /* one byte per chunk, calloc'd at reserve */

static struct { void *at; size_t chunks; HANDLE hmap; } g_view[GP_MAX_VIEWS];
static int g_view_n;

static int chunk_of(void *p)
{
    if (!g_res_base || (uint8_t *)p < g_res_base
        || (uint8_t *)p >= g_res_base + g_res_chunks * (size_t)GP_CHUNK)
        return -1;
    return (int)(((uint8_t *)p - g_res_base) / GP_CHUNK);
}

void *gp_reserve(void *at, size_t size)
{
    /* Find a hole first with one big probe, then re-take it chunk by chunk.
     * Between the free and the loop another thread could steal a chunk; that
     * is the same accepted window as in gp_map_shared, and it is rolled back
     * loudly rather than papered over. */
    void *probe;
    uint8_t *b;
    size_t n, i;

    if (g_res_base)     /* the one chunked reservation is taken — see above */
        return VirtualAlloc(at, size, MEM_RESERVE, PAGE_NOACCESS);

    probe = VirtualAlloc(at, size, MEM_RESERVE, PAGE_NOACCESS);
    if (!probe) return NULL;
    if (at && probe != at) { VirtualFree(probe, 0, MEM_RELEASE); return NULL; }
    VirtualFree(probe, 0, MEM_RELEASE);

    b = probe;
    n = (size + GP_CHUNK - 1) / GP_CHUNK;
    for (i = 0; i < n; i++) {
        if (!VirtualAlloc(b + i * (size_t)GP_CHUNK, GP_CHUNK,
                          MEM_RESERVE, PAGE_NOACCESS)) {
            gp_log("reserve: lost chunk %zu of %zu to a racing allocation\n",
                   i, n);
            while (i--)
                VirtualFree(b + i * (size_t)GP_CHUNK, 0, MEM_RELEASE);
            return NULL;
        }
    }
    g_chunk_is_view = calloc(n, 1);
    if (!g_chunk_is_view) {
        for (i = 0; i < n; i++)
            VirtualFree(b + i * (size_t)GP_CHUNK, 0, MEM_RELEASE);
        return NULL;
    }
    g_res_base   = b;
    g_res_chunks = n;
    return b;
}

static DWORD prot_to_win(int prot)
{
    /* PAGE_* protections are an enumeration, not a bitmask, and there is no
     * write-without-read: W rounds up to RW, WX to RWX. The guest cannot tell
     * — ARM Linux rounds the same way. */
    switch (prot & (GP_PROT_READ | GP_PROT_WRITE | GP_PROT_EXEC)) {
    case 0:                                        return PAGE_NOACCESS;
    case GP_PROT_READ:                             return PAGE_READONLY;
    case GP_PROT_WRITE:
    case GP_PROT_READ | GP_PROT_WRITE:             return PAGE_READWRITE;
    case GP_PROT_EXEC:                             return PAGE_EXECUTE;
    case GP_PROT_READ | GP_PROT_EXEC:              return PAGE_EXECUTE_READ;
    default:                                       return PAGE_EXECUTE_READWRITE;
    }
}

/* Apply one operation across [addr, addr+len) without ever spanning two
 * allocations — chunk by chunk inside the chunked reservation, one call
 * outside it. op: 0 commit, 1 protect, 2 decommit. */
static int mem_walk(void *addr, size_t len, int prot, int op)
{
    uint8_t *p   = addr;
    uint8_t *end = p + len;

    if (chunk_of(addr) < 0) {           /* not ours: the old single calls */
        DWORD old;
        switch (op) {
        case 0:  return VirtualAlloc(addr, len, MEM_COMMIT, prot_to_win(prot)) ? 0 : err();
        case 1:  return VirtualProtect(addr, len, prot_to_win(prot), &old) ? 0 : err();
        default: return VirtualFree(addr, len, MEM_DECOMMIT) ? 0 : err();
        }
    }
    while (p < end) {
        uint8_t *cend = g_res_base
            + ((size_t)(chunk_of(p) + 1)) * (size_t)GP_CHUNK;
        size_t piece = (size_t)((cend < end ? cend : end) - p);
        int view = g_chunk_is_view[chunk_of(p)];
        DWORD old;

        switch (op) {
        case 0:
            /* A view's pages are committed by the section itself; commit
             * there means "give it this protection". */
            if (view) {
                if (!VirtualProtect(p, piece, prot_to_win(prot), &old))
                    return err();
            } else if (!VirtualAlloc(p, piece, MEM_COMMIT, prot_to_win(prot)))
                return err();
            break;
        case 1:
            if (!VirtualProtect(p, piece, prot_to_win(prot), &old))
                return err();
            break;
        default:
            /* Decommitting a view is not a thing; go dark instead, which is
             * what the guest's munmap observably wanted. */
            if (view) {
                if (!VirtualProtect(p, piece, PAGE_NOACCESS, &old))
                    return err();
            } else if (!VirtualFree(p, piece, MEM_DECOMMIT))
                return err();
            break;
        }
        p += piece;
    }
    return 0;
}

int gp_commit(void *addr, size_t len, int prot)   { return mem_walk(addr, len, prot, 0); }
int gp_protect(void *addr, size_t len, int prot)  { return mem_walk(addr, len, prot, 1); }
int gp_decommit(void *addr, size_t len)           { return mem_walk(addr, len, 0, 2); }

int gp_release(void *base, size_t size)
{
    size_t i;
    if (base != g_res_base)
        { (void)size; return VirtualFree(base, 0, MEM_RELEASE) ? 0 : err(); }
    for (i = 0; i < (size_t)g_view_n; i++) {
        UnmapViewOfFile(g_view[i].at);
        CloseHandle(g_view[i].hmap);
    }
    g_view_n = 0;
    for (i = 0; i < g_res_chunks; i++)
        if (!g_chunk_is_view[i])
            VirtualFree(g_res_base + i * (size_t)GP_CHUNK, 0, MEM_RELEASE);
    free(g_chunk_is_view);
    g_res_base = NULL; g_res_chunks = 0; g_chunk_is_view = NULL;
    return 0;
}

/* ---- files --------------------------------------------------------------- */

struct gp_file {
    HANDLE  h;
    SRWLOCK lock;    /* orders pread's save/restore against read/write/seek */
    int     append;
    int     pipe;    /* a FIFO stand-in — see the named-pipe table below */
    /* 1 + the g_fifo index when this descriptor is one of possibly several
     * onto a pipe WE serve; 0 when it is not. One-based so calloc's zero
     * already means "not a served FIFO" and no other site has to say so. */
    int     serves;
};

/* ---- FIFOs as named pipes -------------------------------------------------
 *
 * Win32 HAS pipes with FIFO semantics; they are just not filesystem objects
 * an open() can reach. So the mapping lives HERE: gp_mkfifo records the path
 * in a table and gp_open consults it — a registered path opens
 * \\.\pipe\tadpole-<basename> instead of a file, and everything above host.h
 * keeps thinking it is a file, which is the contract.
 *
 * The name is derived from the BASENAME because the two ends of a pipe are
 * different processes that may spell the directory differently (the viewer's
 * TADPOLE_DIR against the guest's drive-relative rewrite). One Tadpole per
 * machine is assumed, as it already is by the .lock file.
 *
 * Ordering and non-blocking, both of which Linux gives for free:
 *   - the two ends start in either order, so whichever opens FOR READING
 *     first becomes the pipe server and the writer connects as a client;
 *   - a writer with no reader must get ENXIO, not a block — the shim's audio
 *     path opens O_WRONLY|O_NONBLOCK and depends on failing fast, retrying
 *     each period, and discarding on pace;
 *   - reads and writes never block (PIPE_NOWAIT): an empty pipe reads as
 *     EAGAIN, a full one writes as EAGAIN, and a vanished peer reads as EOF.
 *     PIPE_NOWAIT is old and deprecated in favour of overlapped I/O, and it
 *     is also EXACTLY the semantics O_NONBLOCK promises, in one flag. */
#define GP_MAX_FIFOS 12
static struct {
    char   path[512];
    char   pipe[80];
    /* The server end, when WE are the reader — one per PATH, however many
     * descriptors the guest holds onto it. See fifo_open for why that is the
     * whole fix. */
    HANDLE server;
    int    refs;
} g_fifo[GP_MAX_FIFOS];
static int g_fifo_n;
/* Guest threads open and close event nodes concurrently, and the table is now
 * mutable state rather than a lookup built once. SRWLOCK_INIT makes a static
 * one valid with no initialisation call. */
static SRWLOCK g_fifo_lock = SRWLOCK_INIT;

static int fifo_lookup(const char *path)
{
    int i;
    for (i = 0; i < g_fifo_n; i++)
        if (strcmp(g_fifo[i].path, path) == 0)
            return i;
    return -1;
}

int gp_mkfifo(const char *path, uint32_t mode)
{
    const char *base = strrchr(path, '/');
    int r = 0;
    (void)mode;
    base = base ? base + 1 : path;
    AcquireSRWLockExclusive(&g_fifo_lock);
    if (fifo_lookup(path) >= 0)
        r = GP_EEXIST;                  /* callers treat this as "fine" */
    else if (g_fifo_n >= GP_MAX_FIFOS)
        r = GP_ENOMEM;
    else {
        snprintf(g_fifo[g_fifo_n].path, sizeof(g_fifo[g_fifo_n].path), "%s", path);
        snprintf(g_fifo[g_fifo_n].pipe, sizeof(g_fifo[g_fifo_n].pipe),
                 "\\\\.\\pipe\\tadpole-%s", base);
        g_fifo[g_fifo_n].server = NULL;
        g_fifo[g_fifo_n].refs   = 0;
        g_fifo_n++;
    }
    ReleaseSRWLockExclusive(&g_fifo_lock);
    return r;
}

/* Open one end of a FIFO stand-in.
 *
 * THE DIRECTION DECIDES THE ROLE, and getting that wrong is why touch did not
 * work on Windows at all.
 *
 * The old rule was "try to connect as a client; if there is nobody there, become
 * the server", with only O_WRONLY forced to stay a client. But the shim opens
 * every event node **O_RDWR|O_NONBLOCK** — tadpole_shim.c:705, "O_RDWR so the
 * open doesn't block waiting for a writer", which on Linux is free and correct.
 * Under the old rule the FIRST such open created the server, and the SECOND one
 * found that server listening and connected to it. The guest was then talking to
 * itself; the single pipe instance was spent; and the viewer's writer got
 * ERROR_PIPE_BUSY for ever, retried once a frame by ev_open_missing(), with no
 * event ever crossing. On Linux a second reader on one FIFO is ordinary and
 * harmless, which is why nothing above host.h guards against it.
 *
 * So a reader is now ALWAYS the server, and never probes for one first. Several
 * guest descriptors onto one path share the single server handle and a
 * refcount, which is also the closest thing to Linux's behaviour available
 * here: there, two readers split one stream; here they read one stream through
 * one handle, and whichever asks first gets the bytes. That is a real
 * difference in a case the shim does not depend on, and it is written down
 * rather than papered over.
 *
 * A writer stays a client and still gets ENXIO when nobody is reading, which
 * the shim's audio path depends on. */
static int fifo_open(int idx, int flags, gp_file **out)
{
    const char *pipename = g_fifo[idx].pipe;
    HANDLE h;
    gp_file *f;
    int served = 0;

    if ((flags & 3) == GP_O_WRONLY) {
        h = CreateFileA(pipename, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            if (e == ERROR_PIPE_BUSY)
                return GP_EBUSY;
            return GP_ENXIO;            /* no reader: Linux's exact answer */
        }
    } else {
        AcquireSRWLockExclusive(&g_fifo_lock);
        if (!g_fifo[idx].server) {
            h = CreateNamedPipeA(pipename,
                                 PIPE_ACCESS_DUPLEX,
                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
                                 1, 64 << 10, 64 << 10, 0, NULL);
            if (h == INVALID_HANDLE_VALUE) {
                /* The name is already served by somebody else — the audio
                 * FIFO, whose reader is the viewer. Joining as a client is
                 * right there, and cannot be the self-connection above,
                 * because our own server would have been found in the table. */
                ReleaseSRWLockExclusive(&g_fifo_lock);
                h = CreateFileA(pipename, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                OPEN_EXISTING, 0, NULL);
                if (h == INVALID_HANDLE_VALUE)
                    return err();
                goto wrap;
            }
            ConnectNamedPipe(h, NULL);  /* NOWAIT: arms, never blocks */
            g_fifo[idx].server = h;
        }
        g_fifo[idx].refs++;
        h = g_fifo[idx].server;
        ReleaseSRWLockExclusive(&g_fifo_lock);
        served = idx + 1;
    }

wrap:
    f = calloc(1, sizeof *f);
    if (!f) {
        if (served) {
            AcquireSRWLockExclusive(&g_fifo_lock);
            if (--g_fifo[idx].refs == 0) { CloseHandle(h); g_fifo[idx].server = NULL; }
            ReleaseSRWLockExclusive(&g_fifo_lock);
        } else {
            CloseHandle(h);
        }
        return GP_ENOMEM;
    }
    f->h      = h;
    f->pipe   = 1;
    f->serves = served;
    InitializeSRWLock(&f->lock);
    *out = f;
    return 0;
}

/* A client that has gone leaves our server holding a broken pipe, and Win32
 * will not accept another until it is disconnected and re-armed.
 *
 * Without this, closing the viewer ended the event stream permanently: the
 * guest's next read returned end of file, its evdev loop concluded the device
 * was gone, and reopening the viewer changed nothing. It is also what makes
 * the guest's O_RDWR meaningful — a FIFO held open for reading and writing
 * never sees end of file on Linux, and now does not here either. */
static void fifo_rearm(gp_file *f)
{
    if (!f->serves) return;
    DisconnectNamedPipe(f->h);
    ConnectNamedPipe(f->h, NULL);
}

/* REAL shared views, on Windows 7 API. The stopgap that lived here — a
 * private copy with a write-back at exit — could not survive contact with
 * Pet Pad, which maps its framebuffer at TWO guest addresses and renders
 * through one while reading the other. Private copies cannot alias; the
 * write-back flushed the copy nothing drew into, and the screenshot was
 * black while the engine ran perfectly.
 *
 * The chunked reservation (see gp_reserve) is what makes the real thing
 * possible without VirtualAlloc2: release exactly the covering chunks and
 * MapViewOfFileEx into the hole. Views of one local file alias through the
 * file's single section, whichever handles they came from — so the guest's
 * double mapping behaves exactly as MAP_SHARED does on Linux, other
 * processes reading the file see live pages, and there is nothing to flush.
 *
 * Both length and the covering hole are rounded up to whole chunks: a view
 * shorter than its chunk span would leave a sub-64 KB tail of unreserved
 * space inside the guest, which MEM_RESERVE cannot take back (it rounds to
 * granularity) and the host allocator eventually would. The file grows to
 * match, which for the shim's ftruncated arenas means at most a chunk of
 * zero tail. */
int gp_map_shared(void *at, size_t len, gp_file *f, uint64_t off, int prot)
{
    size_t first, n, i;
    HANDLE hmap;
    void *v;
    uint64_t end;

    (void)prot;   /* census maps are RW; the view is RW; mprotect may narrow */

    if (g_view_n >= GP_MAX_VIEWS) return GP_ENOMEM;
    if (chunk_of(at) < 0 || ((uintptr_t)at % GP_CHUNK) != 0
        || (off % GP_CHUNK) != 0)
        return GP_EINVAL;

    first = (size_t)chunk_of(at);
    n     = (len + GP_CHUNK - 1) / GP_CHUNK;
    len   = n * (size_t)GP_CHUNK;
    for (i = 0; i < n; i++)
        if (g_chunk_is_view[first + i]) {
            gp_log("map_shared: %p already carries a view — unmap first\n",
                   (void *)((uint8_t *)at + i * GP_CHUNK));
            return GP_EINVAL;
        }

    end = off + len;
    hmap = CreateFileMappingW(f->h, NULL, PAGE_READWRITE,
                              (DWORD)(end >> 32), (DWORD)end, NULL);
    if (!hmap) return err();

    for (i = 0; i < n; i++)
        VirtualFree((uint8_t *)at + i * (size_t)GP_CHUNK, 0, MEM_RELEASE);
    v = MapViewOfFileEx(hmap, FILE_MAP_READ | FILE_MAP_WRITE,
                        (DWORD)(off >> 32), (DWORD)off, len, at);
    if (!v) {
        int e = err();
        gp_log("map_shared: lost %p to a racing allocation — the accepted "
               "window, but it fired; re-reserving\n", at);
        for (i = 0; i < n; i++)
            VirtualAlloc((uint8_t *)at + i * (size_t)GP_CHUNK, GP_CHUNK,
                         MEM_RESERVE, PAGE_NOACCESS);
        CloseHandle(hmap);
        return e;
    }
    for (i = 0; i < n; i++)
        g_chunk_is_view[first + i] = 1;
    g_view[g_view_n].at     = at;
    g_view[g_view_n].chunks = n;
    g_view[g_view_n].hmap   = hmap;
    g_view_n++;
    return 0;
}

int gp_poll_readable(gp_file **fs, int n, int timeout_ms, unsigned char *ready)
{
    /* A regular file is always readable, which host.h blesses as correct. A
     * PIPE is not, and saying it is turns the guest's event loop into a spin:
     * poll answers "ready" at once, the read comes back EAGAIN, and round it
     * goes at the speed of the JIT. Since the FIFO stand-ins now really exist
     * here, they get a real answer — PeekNamedPipe reports how many bytes are
     * waiting without consuming any, on every Windows since NT.
     *
     * Polled rather than waited on, because a NOWAIT pipe has no waitable
     * object and the alternative is overlapped I/O, which would restructure
     * every handle in this file for one caller. The sleep between looks is
     * what the guest's own poll timeout was going to cost anyway. */
    const unsigned slice_ms = 5;
    unsigned waited = 0;
    int i, cnt = 0;

    for (;;) {
        cnt = 0;
        for (i = 0; i < n; i++) {
            DWORD avail = 0;
            if (!fs[i]) { ready[i] = 0; continue; }
            if (fs[i]->pipe)
                ready[i] = PeekNamedPipe(fs[i]->h, NULL, 0, NULL, &avail, NULL)
                         ? (avail > 0) : 1;   /* unpeekable: let the read decide */
            else
                ready[i] = 1;
            if (ready[i]) cnt++;
        }
        if (cnt) return cnt;
        if (timeout_ms == 0) return 0;
        if (timeout_ms > 0 && waited >= (unsigned)timeout_ms) return 0;
        /* "Forever" still returns after a bounded nap: every caller of this
         * re-loops by construction, and one that cannot make progress should
         * be visible rather than parked. */
        if (timeout_ms < 0 && waited >= 100) return 0;
        gp_sleep_ns((uint64_t)slice_ms * 1000000ull);
        waited += slice_ms;
    }
}

int gp_open(const char *path, int flags, uint32_t mode, gp_file **out)
{
    WCHAR w[GP_WPATH];
    DWORD access = 0, create, fattr = FILE_ATTRIBUTE_NORMAL;
    DWORD attr;
    int e, isdir;
    HANDLE h;
    gp_file *f;

    (void)mode;   /* permission bits stop meaning anything at this boundary */

    /* A path gp_mkfifo registered is a pipe wearing a filename. */
    {
        int fi;
        AcquireSRWLockShared(&g_fifo_lock);
        fi = fifo_lookup(path);
        ReleaseSRWLockShared(&g_fifo_lock);
        if (fi >= 0)
            return fifo_open(fi, flags, out);
    }

    if ((e = widen(path, w, GP_WPATH)) < 0) return e;

    attr  = GetFileAttributesW(w);
    isdir = attr != INVALID_FILE_ATTRIBUTES
         && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if ((flags & GP_O_DIRECTORY) && attr != INVALID_FILE_ATTRIBUTES && !isdir)
        return GP_ENOTDIR;
    if (isdir && (flags & 3) != GP_O_RDONLY)
        return GP_EISDIR;             /* Linux: open(dir, O_WRONLY) == EISDIR */

    switch (flags & 3) {
    case GP_O_WRONLY: access = GENERIC_WRITE; break;
    case GP_O_RDWR:   access = GENERIC_READ | GENERIC_WRITE; break;
    default:          access = GENERIC_READ; break;
    }
    if (flags & GP_O_CREAT)
        create = (flags & GP_O_EXCL)  ? CREATE_NEW
               : (flags & GP_O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS;
    else
        create = (flags & GP_O_TRUNC) ? TRUNCATE_EXISTING : OPEN_EXISTING;
    if (isdir)
        fattr |= FILE_FLAG_BACKUP_SEMANTICS;   /* the only way to open a dir */

    /* Full sharing, because the guest is a Linux program: it expects to open
     * a file twice, or rename over one that is open. Windows' default of
     * "exclusive unless stated" is the wrong default for it. */
    h = CreateFileW(w, access,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, create, fattr, NULL);
    if (h == INVALID_HANDLE_VALUE) return err();

    f = calloc(1, sizeof *f);
    if (!f) { CloseHandle(h); return GP_ENOMEM; }
    f->h = h;
    InitializeSRWLock(&f->lock);
    f->append = (flags & GP_O_APPEND) != 0;
    *out = f;
    return 0;
}

int gp_close(gp_file *f)
{
    int r = 0;
    if (!f) return GP_EBADF;
    if (f->serves) {
        /* The LAST descriptor onto a served FIFO closes it. Closing on the
         * first would tear the pipe down under the guest's other reader and
         * hand the viewer a name with no server behind it. */
        int idx = f->serves - 1;
        AcquireSRWLockExclusive(&g_fifo_lock);
        if (--g_fifo[idx].refs <= 0) {
            r = CloseHandle(g_fifo[idx].server) ? 0 : err();
            g_fifo[idx].server = NULL;
            g_fifo[idx].refs   = 0;
        }
        ReleaseSRWLockExclusive(&g_fifo_lock);
    } else {
        r = CloseHandle(f->h) ? 0 : err();
    }
    free(f);
    return r;
}

/* GetLastError -> the FIFO-shaped errno a pipe end expects. NOWAIT pipes
 * speak in these three: empty (or not-yet-connected) is EAGAIN — the same
 * answer O_NONBLOCK gives — and a departed peer is EOF on read, EPIPE-ish on
 * write, for which EIO is the closest number the table carries. */
static int64_t pipe_err(DWORD e, int reading)
{
    if (e == ERROR_NO_DATA || e == ERROR_PIPE_LISTENING)
        return reading ? GP_EAGAIN : GP_EAGAIN;
    if (e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED)
        return reading ? 0 : GP_EPIPE; /* reader: EOF; writer: the exact errno
                                        * Linux gives, which the shim's audio
                                        * retry already knows how to read */
    return err_from(e);
}

int64_t gp_read(gp_file *f, void *buf, size_t len)
{
    DWORD n;
    BOOL ok;
    if (len > 0x7fffffffu) len = 0x7fffffffu;
    AcquireSRWLockExclusive(&f->lock);
    ok = ReadFile(f->h, buf, (DWORD)len, &n, NULL);
    ReleaseSRWLockExclusive(&f->lock);
    if (!ok) {
        DWORD e = GetLastError();
        if (!f->pipe) return err_from(e);
        /* Our own server, and the writer has left: re-arm for the next one and
         * say "nothing yet" rather than "the device is gone". */
        if (f->serves && (e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED)) {
            fifo_rearm(f);
            return GP_EAGAIN;
        }
        return pipe_err(e, 1);
    }
    /* A NOWAIT pipe "succeeds" with 0 bytes when empty; that is EAGAIN, not
     * EOF — EOF is the peer actually leaving, reported above. */
    if (f->pipe && n == 0 && len > 0)
        return GP_EAGAIN;
    return n;      /* file EOF is TRUE with n == 0, exactly read()'s 0 */
}

int64_t gp_write(gp_file *f, const void *buf, size_t len)
{
    DWORD n;
    BOOL ok;
    if (len > 0x7fffffffu) len = 0x7fffffffu;
    AcquireSRWLockExclusive(&f->lock);
    if (f->append && !f->pipe) {
        LARGE_INTEGER end; end.QuadPart = 0;
        SetFilePointerEx(f->h, end, NULL, FILE_END);
    }
    ok = WriteFile(f->h, buf, (DWORD)len, &n, NULL);
    ReleaseSRWLockExclusive(&f->lock);
    if (!ok) {
        DWORD e = GetLastError();
        if (!f->pipe) return err_from(e);
        if (f->serves && (e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED)) {
            fifo_rearm(f);
            return GP_EAGAIN;
        }
        return pipe_err(e, 0);
    }
    /* NOWAIT again: a full pipe "writes" 0 bytes. EAGAIN, so the shim's
     * bounded retry paces the writer exactly as it does on Linux. */
    if (f->pipe && n == 0 && len > 0)
        return GP_EAGAIN;
    return n;
}

int64_t gp_pread(gp_file *f, void *buf, size_t len, uint64_t off)
{
    OVERLAPPED ov;
    LARGE_INTEGER save, zero;
    DWORD n, e = 0;
    BOOL ok;

    if (f->pipe) return GP_ESPIPE;
    if (len > 0x7fffffffu) len = 0x7fffffffu;
    memset(&ov, 0, sizeof ov);
    ov.Offset     = (DWORD)(off & 0xffffffffu);
    ov.OffsetHigh = (DWORD)(off >> 32);
    zero.QuadPart = 0;

    AcquireSRWLockExclusive(&f->lock);
    SetFilePointerEx(f->h, zero, &save, FILE_CURRENT);
    ok = ReadFile(f->h, buf, (DWORD)len, &n, &ov);
    if (!ok) e = GetLastError();
    SetFilePointerEx(f->h, save, NULL, FILE_BEGIN);   /* see file header */
    ReleaseSRWLockExclusive(&f->lock);

    if (!ok) {
        if (e == ERROR_HANDLE_EOF) return 0;   /* pread past EOF reads 0 */
        return err_from(e);
    }
    return n;
}

int64_t gp_seek(gp_file *f, int64_t off, int whence)
{
    LARGE_INTEGER want, got;
    DWORD method = whence == GP_SEEK_CUR ? FILE_CURRENT
                 : whence == GP_SEEK_END ? FILE_END : FILE_BEGIN;
    BOOL ok;
    if (f->pipe) return GP_ESPIPE;
    want.QuadPart = off;
    AcquireSRWLockExclusive(&f->lock);
    ok = SetFilePointerEx(f->h, want, &got, method);
    ReleaseSRWLockExclusive(&f->lock);
    return ok ? got.QuadPart : err();
}

/* FILETIME is 100 ns ticks since 1601; the guest's epoch is 1970. */
static uint64_t ft_ns(FILETIME ft)
{
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ull) * 100u;
}

/* NTFS has no mode bits, so the guest gets a synthesized but self-consistent
 * story: directories 0755, files 0755 minus write when the read-only attribute
 * is set. Exec on everything, because the guest's binaries arrive by rootfs
 * copy and losing the x bit in transit must not make them unrunnable. */
static uint32_t synth_mode(DWORD attr)
{
    if (attr & FILE_ATTRIBUTE_DIRECTORY)
        return 0040000u | 0755u;                       /* S_IFDIR */
    return 0100000u                                    /* S_IFREG */
         | ((attr & FILE_ATTRIBUTE_READONLY) ? 0555u : 0755u);
}

/* (dev, ino) are load-bearing — see host.h. On Windows file identity lives on
 * HANDLES, not paths: GetFileAttributesExW cannot produce the volume serial or
 * file index, so even the path-based stat opens a handle. Zero desired access
 * makes it an attribute-only open that needs no read permission, and
 * BACKUP_SEMANTICS lets it open directories. */
static int fill_from_handle(HANDLE h, struct gp_statbuf *st)
{
    BY_HANDLE_FILE_INFORMATION bi;
    if (!GetFileInformationByHandle(h, &bi)) return err();
    st->size     = ((uint64_t)bi.nFileSizeHigh << 32) | bi.nFileSizeLow;
    st->mtime_ns = ft_ns(bi.ftLastWriteTime);
    st->dev      = bi.dwVolumeSerialNumber;
    st->ino      = ((uint64_t)bi.nFileIndexHigh << 32) | bi.nFileIndexLow;
    st->mode     = synth_mode(bi.dwFileAttributes);
    st->is_dir   = (bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    /* NTFS keeps a link count and it is right for files. For directories it
     * counts hard links only, so it is 1 rather than "2 + subdirectories" —
     * a truthful answer to a different question than the guest is asking.
     * 1 is what a walker reads as "this filesystem's link counts cannot be
     * used", which is the honest signal and makes it recurse instead of
     * trusting arithmetic that does not hold here. */
    st->nlink    = st->is_dir ? 1u : bi.nNumberOfLinks;
    if (st->is_dir) st->size = 0;
    return 0;
}

int gp_stat(const char *path, struct gp_statbuf *st)
{
    WCHAR w[GP_WPATH];
    HANDLE h;
    int e;
    if ((e = widen(path, w, GP_WPATH)) < 0) return e;
    h = CreateFileW(w, 0,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return err();
    e = fill_from_handle(h, st);
    CloseHandle(h);
    return e;
}

int gp_fstat(gp_file *f, struct gp_statbuf *st)
{
    if (f->pipe) {
        /* Pipes have no file information; say S_IFIFO and be done, which is
         * also everything fstat on a Linux FIFO usefully says. */
        memset(st, 0, sizeof *st);
        st->mode  = 0010000u | 0644u;
        st->dev   = 0;
        st->ino   = (uint64_t)(uintptr_t)f;
        st->nlink = 1;
        return 0;
    }
    return fill_from_handle(f->h, st);
}

int gp_truncate(gp_file *f, uint64_t size)
{
    /* SetFileInformationByHandle(FileEndOfFileInfo) is Vista+, inside the
     * floor, and unlike the SetFilePointer/SetEndOfFile dance it does not
     * touch the file position — ftruncate does not either. */
    FILE_END_OF_FILE_INFO info;
    info.EndOfFile.QuadPart = (LONGLONG)size;
    return SetFileInformationByHandle(f->h, FileEndOfFileInfo,
                                      &info, sizeof info) ? 0 : err();
}

int gp_sync(gp_file *f)
{
    return FlushFileBuffers(f->h) ? 0 : err();
}

int gp_chmod(const char *path, uint32_t mode)
{
    WCHAR w[GP_WPATH];
    DWORD attr;
    int e;
    if ((e = widen(path, w, GP_WPATH)) < 0) return e;
    /* Ask the attributes first rather than calling SetFileAttributesW blind:
     * it is what turns "no such file" into ENOENT instead of whatever the set
     * would have complained about, and ENOENT is the answer the one measured
     * caller is waiting for. */
    attr = GetFileAttributesW(w);
    if (attr == INVALID_FILE_ATTRIBUTES) return err();
    if (mode & 0222) attr &= ~(DWORD)FILE_ATTRIBUTE_READONLY;
    else             attr |=  (DWORD)FILE_ATTRIBUTE_READONLY;
    return SetFileAttributesW(w, attr) ? 0 : err();
}

int gp_mkdir(const char *path, uint32_t mode)
{
    WCHAR w[GP_WPATH];
    int e;
    (void)mode;
    if ((e = widen(path, w, GP_WPATH)) < 0) return e;
    return CreateDirectoryW(w, NULL) ? 0 : err();
}

int gp_rmdir(const char *path)
{
    WCHAR w[GP_WPATH];
    int e;
    if ((e = widen(path, w, GP_WPATH)) < 0) return e;
    return RemoveDirectoryW(w) ? 0 : err();
}

int gp_unlink(const char *path)
{
    WCHAR w[GP_WPATH];
    DWORD attr;
    int e;
    if ((e = widen(path, w, GP_WPATH)) < 0) return e;
    if (DeleteFileW(w)) return 0;
    e = err();
    /* Linux says EISDIR for unlink on a directory; Windows says access
     * denied, which would send the guest down the wrong retry path. */
    attr = GetFileAttributesW(w);
    if (e == GP_EACCES && attr != INVALID_FILE_ATTRIBUTES
        && (attr & FILE_ATTRIBUTE_DIRECTORY))
        return GP_EISDIR;
    return e;
}

int gp_rename(const char *from, const char *to)
{
    WCHAR wf[GP_WPATH], wt[GP_WPATH];
    int e;
    if ((e = widen(from, wf, GP_WPATH)) < 0) return e;
    if ((e = widen(to,   wt, GP_WPATH)) < 0) return e;
    /* REPLACE_EXISTING because rename(2) replaces, always. */
    return MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING) ? 0 : err();
}

/* ---- directories --------------------------------------------------------- */

struct gp_dir {
    HANDLE find;
    WIN32_FIND_DATAW fd;
    int    pending;      /* FindFirstFileW's entry, not yet handed out */
    char   name[1024];   /* UTF-8 of the last entry; dies with the next call */
};

int gp_diropen(const char *path, gp_dir **out)
{
    WCHAR w[GP_WPATH];
    DWORD attr;
    size_t n;
    int e;
    gp_dir *d;

    if ((e = widen(path, w, GP_WPATH)) < 0) return e;
    attr = GetFileAttributesW(w);
    if (attr == INVALID_FILE_ATTRIBUTES) return err();
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) return GP_ENOTDIR;

    n = wcslen(w);
    if (n + 3 >= GP_WPATH) return GP_ENOENT;
    w[n] = L'\\'; w[n + 1] = L'*'; w[n + 2] = 0;

    d = calloc(1, sizeof *d);
    if (!d) return GP_ENOMEM;
    d->find = FindFirstFileW(w, &d->fd);
    if (d->find == INVALID_HANDLE_VALUE) { e = err(); free(d); return e; }
    d->pending = 1;      /* "." — FindFirstFileW yields it, like readdir */
    *out = d;
    return 0;
}

int gp_dirnext(gp_dir *d, const char **name, uint32_t *is_dir)
{
    if (!d->pending) {
        if (!FindNextFileW(d->find, &d->fd)) {
            DWORD e = GetLastError();
            return e == ERROR_NO_MORE_FILES ? 0 : err_from(e);
        }
    }
    d->pending = 0;
    if (!WideCharToMultiByte(CP_UTF8, 0, d->fd.cFileName, -1,
                             d->name, sizeof d->name, NULL, NULL))
        return GP_EIO;   /* a name we cannot spell is a name we cannot serve */
    *name   = d->name;
    *is_dir = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    return 1;
}

int gp_dirclose(gp_dir *d)
{
    int r = FindClose(d->find) ? 0 : err();
    free(d);
    return r;
}

/* ---- threads ------------------------------------------------------------- */

struct gp_thread { HANDLE h; void (*entry)(void *); void *arg; };

static unsigned __stdcall thread_trampoline(void *p)
{
    gp_thread *t = p;
    t->entry(t->arg);
    return 0;
}

int gp_thread_create(void (*entry)(void *), void *arg, gp_thread **out)
{
    /* _beginthreadex rather than CreateThread so the C runtime sets up its
     * per-thread state; with a static CRT that is not optional. */
    uintptr_t h;
    gp_thread *t = calloc(1, sizeof *t);
    if (!t) return GP_ENOMEM;
    t->entry = entry;
    t->arg   = arg;
    h = _beginthreadex(NULL, 0, thread_trampoline, t, 0, NULL);
    if (!h) { free(t); return GP_EAGAIN; }
    t->h = (HANDLE)h;
    *out = t;
    return 0;
}

int gp_thread_join(gp_thread *t)
{
    if (WaitForSingleObject(t->h, INFINITE) != WAIT_OBJECT_0) return GP_EINVAL;
    CloseHandle(t->h);
    free(t);
    return 0;
}

int gp_thread_detach(gp_thread *t)
{
    CloseHandle(t->h);
    free(t);
    return 0;
}

void gp_thread_exit(void)  { _endthreadex(0); }
void gp_thread_yield(void) { SwitchToThread(); }

uint32_t gp_thread_id(void) { return (uint32_t)GetCurrentThreadId(); }

/* ---- waiting -------------------------------------------------------------
 * The sixty lines host.h promised. An address hashes to a bucket holding an
 * SRWLOCK and a CONDITION_VARIABLE; gp_wait_on re-checks the word under the
 * bucket lock, which is the same lock gp_wake takes, so the compare and the
 * sleep are atomic against the wake — the futex guarantee.
 *
 * gp_wake wakes the WHOLE bucket even for count == 1. Addresses share
 * buckets, and a single wake could land on a waiter for a different address,
 * which would swallow the wake and leave the intended waiter asleep — a lost
 * wakeup, the one bug this interface exists to prevent. Waking everyone is
 * merely a thundering herd within one bucket: each waiter returns 0, its
 * caller re-checks its word, and the ones woken by accident go back to sleep.
 * A spurious wake is a legal wake; a lost one is not.
 *
 * Both types initialise to all-zero, so a zeroed static array is already a
 * valid table and no once-guard is needed. */
#define GP_WAIT_BUCKETS 64
static struct {
    SRWLOCK lock;
    CONDITION_VARIABLE cv;
} g_wait[GP_WAIT_BUCKETS];

static unsigned bucket_of(volatile uint32_t *addr)
{
    uintptr_t a = (uintptr_t)addr >> 2;
    return (unsigned)((a ^ (a >> 8)) & (GP_WAIT_BUCKETS - 1));
}

int gp_wait_on(volatile uint32_t *addr, uint32_t expected, uint64_t timeout_ns)
{
    unsigned b = bucket_of(addr);
    DWORD ms = INFINITE;
    BOOL ok;

    if (timeout_ns) {
        uint64_t m = (timeout_ns + 999999u) / 1000000u;
        ms = m > 0x7ffffffeu ? 0x7ffffffeu : (DWORD)m;
    }
    AcquireSRWLockExclusive(&g_wait[b].lock);
    if (*addr != expected) {
        ReleaseSRWLockExclusive(&g_wait[b].lock);
        return GP_EAGAIN;
    }
    ok = SleepConditionVariableSRW(&g_wait[b].cv, &g_wait[b].lock, ms, 0);
    ReleaseSRWLockExclusive(&g_wait[b].lock);
    if (!ok)
        return GetLastError() == ERROR_TIMEOUT ? GP_ETIMEDOUT : err();
    return 0;
}

int gp_wake(volatile uint32_t *addr, int count)
{
    unsigned b = bucket_of(addr);
    (void)count;                       /* see the block comment above */
    AcquireSRWLockExclusive(&g_wait[b].lock);
    WakeAllConditionVariable(&g_wait[b].cv);
    ReleaseSRWLockExclusive(&g_wait[b].lock);
    /* A condition variable cannot say how many it woke, so unlike the futex
     * backend this cannot report a count. Nothing above uses it yet; when the
     * futex syscall lands, this is the place that decides what it returns. */
    return 0;
}

/* ---- time ----------------------------------------------------------------
 * The hot one — no lock and no allocation below this line. The one-time
 * resolutions race benignly: every thread computes and stores identical
 * values. */

typedef VOID (WINAPI *precise_time_fn)(LPFILETIME);
static precise_time_fn g_precise;
static int g_time_ready;

uint64_t gp_wall_ns(void)
{
    FILETIME ft;
    if (!g_time_ready) {
        /* GetSystemTimePreciseAsFileTime is Windows 8+, better than the
         * ~15.6 ms tick of the plain call, and the guest calls gettimeofday
         * 381,096 times a run — resolve it if it exists, live without it on
         * Windows 7. GetProcAddress, not a link-time import, is what keeps
         * the floor at 7. */
        g_precise = (precise_time_fn)(void (*)(void))GetProcAddress(
            GetModuleHandleW(L"kernel32.dll"), "GetSystemTimePreciseAsFileTime");
        g_time_ready = 1;
    }
    if (g_precise) g_precise(&ft);
    else           GetSystemTimeAsFileTime(&ft);
    return ft_ns(ft);
}

uint64_t gp_mono_ns(void)
{
    static LARGE_INTEGER freq;   /* constant after boot; benign race */
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    /* Split to dodge the overflow in ticks * 1e9, which arrives after only
     * ~15 minutes of uptime at a 10 MHz QPC. */
    return (uint64_t)(c.QuadPart / freq.QuadPart) * 1000000000ull
         + (uint64_t)(c.QuadPart % freq.QuadPart) * 1000000000ull
           / (uint64_t)freq.QuadPart;
}

void gp_sleep_ns(uint64_t ns)
{
    /* Sleep rounds to the scheduler tick, ~15.6 ms by default — uselessly
     * coarse for a guest pacing frames with nanosleep. timeBeginPeriod(1)
     * pulls it to ~1 ms, process-lifetime, undone by the OS at exit; every
     * emulator ships this call. Benign race, same as above. */
    static int period_set;
    uint64_t ms;
    if (!period_set) { timeBeginPeriod(1); period_set = 1; }
    ms = (ns + 999999u) / 1000000u;
    Sleep(ms > 0x7ffffffeu ? 0x7ffffffeu : (DWORD)ms);
}

/* ---- guest faults --------------------------------------------------------
 * The Windows shape of host_posix.c's SIGSEGV handler. A vectored handler
 * sees the exception before any frame-based (__try) handling, on the faulting
 * thread's own stack — which is intact, because a GUEST stack overrun is an
 * ordinary access violation inside the 4 GiB reservation, not a host stack
 * fault, so no alternate stack is needed where POSIX required one. A host
 * EXCEPTION_STACK_OVERFLOW is not ours to explain and is passed on. */

static gp_fault_fn g_fault_fn;

static LONG CALLBACK fault_veh(EXCEPTION_POINTERS *xp)
{
    if (xp->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    if (g_fault_fn)
        g_fault_fn((void *)(uintptr_t)xp->ExceptionRecord->ExceptionInformation[1]);
    /* TerminateProcess, not exit(): atexit handlers and stdio flushing after
     * a memory fault can fault again and hide what was just printed — the
     * same reasoning as the POSIX backend's _exit(). */
    TerminateProcess(GetCurrentProcess(), 73);
    return EXCEPTION_CONTINUE_SEARCH;   /* unreachable */
}

int gp_install_fault_handler(gp_fault_fn fn)
{
    g_fault_fn = fn;
    /* LAST in the chain (First = 0), reserving the front for the day
     * dynarmic's fastmem handler needs to claim its own faults before us. */
    return AddVectoredExceptionHandler(0, fault_veh) ? 0 : GP_EINVAL;
}

/* ---- odds and ends ------------------------------------------------------- */

int gp_random(void *buf, size_t len)
{
    /* The system RNG without naming an algorithm; Vista+, inside the floor. */
    return BCryptGenRandom(NULL, buf, (ULONG)len,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : GP_EIO;
}

int64_t gp_console_write(int fd, const void *buf, size_t len)
{
    HANDLE h = GetStdHandle(fd == 2 ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    DWORD n;
    if (h == NULL || h == INVALID_HANDLE_VALUE) return GP_EBADF;
    if (len > 0x7fffffffu) len = 0x7fffffffu;
    /* WriteFile serves both a console and a redirection. The console decodes
     * bytes in its output codepage, so non-ASCII guest output can mojibake on
     * screen while staying byte-exact through a pipe — the pipe is what the
     * qemu-arm diffing harness reads, and byte-exact there is what matters. */
    if (!WriteFile(h, buf, (DWORD)len, &n, NULL)) return err();
    return n;
}

void gp_log(const char *fmt, ...)
{
    /* msvcrt's unbuffered stderr writes CHARACTER AT A TIME, which turns a
     * --trace run into an I/O benchmark: ~23 traced syscalls/second, i.e. the
     * tracer costing three orders of magnitude more than the thing it traces.
     * Line-buffer it once. The POSIX backend needs none of this — glibc's
     * unbuffered stderr still writes whole calls. */
    static int buffered;
    va_list ap;
    if (!buffered) {
        setvbuf(stderr, NULL, _IOFBF, 1 << 16);
        buffered = 1;
    }
    va_start(ap, fmt);
    fputs("[glasspole] ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}
