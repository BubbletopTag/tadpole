/* Glasspole — the 51.
 *
 * This file is the "Linux" half of the port and it is the half that never
 * changes: syscall numbers, argument order, struct layouts, errno values, path
 * rules. Every line here is identical on Windows. The only thing below it is
 * host.h, and the only thing that changes on Windows is host.h's backend.
 *
 * The set implemented is driven by measurement rather than by reading a
 * syscall table top to bottom. A full run of Cars 2 under `qemu-arm -strace`
 * used 51 distinct calls; anything outside that list reports itself by name
 * and returns -ENOSYS, so the guest tells us what it wanted instead of merely
 * misbehaving.
 */
#include "machine.h"

#include <dynarmic/interface/A32/a32.h>

#include <cstring>

namespace {

/* ARM EABI syscall numbers. */
enum : uint32_t {
    SYS_exit = 1, SYS_read = 3, SYS_write = 4, SYS_open = 5, SYS_close = 6,
    SYS_unlink = 10, SYS_chdir = 12, SYS_lseek = 19, SYS_getpid = 20,
    SYS_access = 33, SYS_rename = 38, SYS_mkdir = 39, SYS_rmdir = 40,
    SYS_brk = 45, SYS_ioctl = 54, SYS_fcntl = 55, SYS_gettimeofday = 78,
    SYS_munmap = 91, SYS_uname = 122, SYS_mprotect = 125, SYS_llseek = 140,
    SYS_writev = 146, SYS_rt_sigaction = 174, SYS_rt_sigprocmask = 175,
    SYS_ugetrlimit = 191, SYS_mmap2 = 192, SYS_stat64 = 195, SYS_fstat64 = 197,
    SYS_getuid32 = 199, SYS_getgid32 = 200, SYS_geteuid32 = 201,
    SYS_getegid32 = 202, SYS_fcntl64 = 221, SYS_exit_group = 248,
    SYS_set_tid_address = 256, SYS_clock_gettime = 263, SYS_readlink = 85,
    SYS_set_robust_list = 338, SYS_getcwd = 183, SYS_nanosleep = 162,
    SYS_sched_yield = 158,
    /* The pre-64-bit stat family. uClibc's ld.so uses THESE, not the *64
     * variants — which is why leaving them out makes every library lookup come
     * back "no such file" and the linker report a missing libc.so.0. */
    SYS_stat = 106, SYS_lstat = 107, SYS_fstat = 108,
    /* ARM private range. */
    SYS_ARM_set_tls = 0xf0005, SYS_ARM_cacheflush = 0xf0002,
    /* Threads. */
    SYS_clone = 120, SYS_futex = 240, SYS_gettid = 224,
    SYS_mknod = 14, SYS_ftruncate = 93, SYS_getdents64 = 217,
};

/* Guest open() flags, which are the same values on ARM Linux as our GP_O_*
 * happen to be — but mapped explicitly, because "they happen to agree" is not
 * something the Windows backend should be relying on. */
int open_flags(uint32_t f) {
    int o = 0;
    switch (f & 3) {
        case 1: o = GP_O_WRONLY; break;
        case 2: o = GP_O_RDWR;   break;
        default: o = GP_O_RDONLY; break;
    }
    if (f & 0x40)   o |= GP_O_CREAT;
    if (f & 0x80)   o |= GP_O_EXCL;
    if (f & 0x200)  o |= GP_O_TRUNC;
    if (f & 0x400)  o |= GP_O_APPEND;
    if (f & 0x10000) o |= GP_O_DIRECTORY;
    return o;
}

/* ARM EABI struct stat64. Laid out by hand with explicit padding because this
 * is the GUEST's layout, not the host's — a host struct stat copied in would
 * be right on Linux and wrong on Windows, which is the entire class of bug
 * this project is built to avoid. */
#pragma pack(push, 1)
struct guest_stat64 {
    uint64_t st_dev;
    uint8_t  pad0[4];
    uint32_t __st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint8_t  pad3[4];
    int64_t  st_size;
    uint32_t st_blksize;
    uint64_t st_blocks;
    uint32_t st_atime_, st_atime_nsec;
    uint32_t st_mtime_, st_mtime_nsec;
    uint32_t st_ctime_, st_ctime_nsec;
    uint64_t st_ino;
};
#pragma pack(pop)

/* The original ARM struct stat — 64 bytes, and note how many fields are
 * `short`. Nothing about it can be derived from the stat64 layout above. */
#pragma pack(push, 1)
struct guest_stat {
    uint32_t st_dev, st_ino;
    uint16_t st_mode, st_nlink, st_uid, st_gid;
    uint32_t st_rdev, st_size, st_blksize, st_blocks;
    uint32_t st_atime_, st_atime_nsec;
    uint32_t st_mtime_, st_mtime_nsec;
    uint32_t st_ctime_, st_ctime_nsec;
    uint32_t unused4, unused5;
};
#pragma pack(pop)
static_assert(sizeof(guest_stat) == 64, "ARM struct stat is 64 bytes");

void fill_stat(const gp_statbuf &st, guest_stat *g) {
    std::memset(g, 0, sizeof *g);
    g->st_dev     = (uint32_t)st.dev;
    g->st_ino     = (uint32_t)st.ino;
    g->st_mode    = (uint16_t)st.mode;
    g->st_nlink   = 1;
    g->st_size    = (uint32_t)st.size;
    g->st_blksize = 4096;
    g->st_blocks  = (uint32_t)((st.size + 511) / 512);
    g->st_mtime_  = (uint32_t)(st.mtime_ns / 1000000000ull);
    g->st_atime_  = g->st_ctime_ = g->st_mtime_;
}

void fill_stat64(const gp_statbuf &st, guest_stat64 *g) {
    std::memset(g, 0, sizeof *g);
    g->st_dev = st.dev;
    g->st_ino = st.ino;
    g->__st_ino = (uint32_t)st.ino;
    g->st_mode    = st.mode;
    g->st_nlink   = 1;
    g->st_size    = (int64_t)st.size;
    g->st_blksize = 4096;
    g->st_blocks  = (st.size + 511) / 512;
    g->st_mtime_  = (uint32_t)(st.mtime_ns / 1000000000ull);
    g->st_atime_  = g->st_ctime_ = g->st_mtime_;
}

const char *name_of(uint32_t nr) {
    switch (nr) {
        case SYS_exit: return "exit";               case SYS_read: return "read";
        case SYS_write: return "write";             case SYS_open: return "open";
        case SYS_close: return "close";             case SYS_brk: return "brk";
        case SYS_ioctl: return "ioctl";             case SYS_mmap2: return "mmap2";
        case SYS_munmap: return "munmap";           case SYS_mprotect: return "mprotect";
        case SYS_stat64: return "stat64";           case SYS_fstat64: return "fstat64";
        case SYS_uname: return "uname";             case SYS_llseek: return "_llseek";
        case SYS_gettimeofday: return "gettimeofday";
        case SYS_clock_gettime: return "clock_gettime";
        case SYS_ARM_set_tls: return "set_tls";     case SYS_writev: return "writev";
        case SYS_access: return "access";           case SYS_readlink: return "readlink";
        case SYS_exit_group: return "exit_group";   case SYS_getcwd: return "getcwd";
        case SYS_clone: return "clone";             case SYS_futex: return "futex";
        case SYS_gettid: return "gettid";           case SYS_fcntl64: return "fcntl64";
        case SYS_mknod: return "mknod";             case SYS_ftruncate: return "ftruncate";
        case SYS_getdents64: return "getdents64";
        case SYS_getuid32: return "getuid32";
        case SYS_nanosleep: return "nanosleep";     case SYS_lstat: return "lstat";
        case SYS_stat: return "stat";               case SYS_fstat: return "fstat";
        default: return "?";
    }
}

}  // namespace

void gp_syscall(Thread &t) {
    Machine &m = *t.m;
    auto &r = t.jit->Regs();
    const uint32_t nr = r[7];
    const uint32_t a0 = r[0], a1 = r[1], a2 = r[2], a3 = r[3];
    int32_t ret = GP_ENOSYS;
    /* Paths, not pointers. A trace line that prints 3fffe140 where the guest
     * asked for a file name is an instrument hiding the fact you needed. */
    std::string tpath;

    /* One lock over everything that touches shared state — the descriptor
     * table and the allocators. Taken up front rather than per case, because
     * "which cases need it" is exactly the question a future edit gets wrong.
     *
     * It is NOT held across anything that blocks: futex and nanosleep unlock
     * first, or one sleeping thread would stop every other thread from making
     * a syscall. gettimeofday is 85% of all calls and takes it uncontended;
     * if that ever shows up in a profile, split it out then, with a
     * measurement rather than a guess. */
    std::unique_lock<std::mutex> guard(m.lock);

    switch (nr) {

    /* ---- process ---- */
    case SYS_exit:
        /* ONE thread stops. CLONE_CHILD_CLEARTID is how pthread_join finds
         * out: zero the guest word the parent is waiting on, then wake it.
         * Skip this and every join hangs forever. */
        if (t.clear_child_tid) {
            uint32_t zero = 0;
            std::memcpy(m.Ptr(t.clear_child_tid), &zero, 4);
            gp_wake(reinterpret_cast<volatile uint32_t *>(m.Ptr(t.clear_child_tid)),
                    0x7fffffff);
        }
        t.Halt();
        return;

    case SYS_exit_group:
        guard.unlock();          /* ExitGroup takes the lock itself */
        m.ExitGroup((int)a0);
        return;

    case SYS_getpid:    ret = 1000; break;
    case SYS_gettid:    ret = (int32_t)t.tid; break;
    case SYS_getuid32:
    case SYS_geteuid32:
    case SYS_getgid32:
    case SYS_getegid32: ret = 0; break;

    case SYS_set_tid_address:
    case SYS_set_robust_list:
    case SYS_rt_sigaction:
    case SYS_rt_sigprocmask:
    case SYS_ARM_cacheflush:
        /* Accepted and ignored. dynarmic manages its own code cache, and the
         * signal calls matter only once something can deliver one. */
        ret = 0;
        break;

    case SYS_ARM_set_tls:
        /* The kernel's ARM TLS helper. dynarmic keeps TPIDRURO for us, and
         * uClibc reads it through the c15 coprocessor path. */
        t.tls = a0;
        ret = 0;
        break;

    case SYS_ugetrlimit:
        /* RLIM_INFINITY for everything asked so far. Writing a real pair keeps
         * uClibc from believing a limit of zero. */
        if (a1) { uint32_t *p = (uint32_t *)m.Ptr(a1); p[0] = 0xffffffffu; p[1] = 0xffffffffu; }
        ret = 0;
        break;

    /* ---- threads ---- */
    case SYS_clone:
        /* flags, child_stack, parent_tidptr, tls, child_tidptr */
        guard.unlock();          /* spawning takes the lock itself */
        ret = gp_spawn_thread(t, a0, a1, a2, a3, r[4]);
        break;

    case SYS_futex: {
        /* uaddr, op, val, timeout, uaddr2, val3. Only WAIT and WAKE appear in
         * the census, and only the PRIVATE variants — the guest is one process,
         * so there is nothing for the shared ones to be shared with. */
        constexpr uint32_t FUTEX_WAIT = 0, FUTEX_WAKE = 1, FUTEX_PRIVATE = 128;
        const uint32_t op = a1 & ~(FUTEX_PRIVATE | 256u /* CLOCK_REALTIME */);
        auto *addr = reinterpret_cast<volatile uint32_t *>(m.Ptr(a0));

        if (op == FUTEX_WAIT) {
            uint64_t ns = 0;
            if (a3) {
                const uint32_t *ts = (const uint32_t *)m.Ptr(a3);
                ns = (uint64_t)ts[0] * 1000000000ull + ts[1];
                if (!ns) ns = 1;     /* 0 means "forever" to gp_wait_on */
            }
            /* MUST NOT hold the lock while blocking. */
            guard.unlock();
            ret = gp_wait_on(addr, a2, ns);
        } else if (op == FUTEX_WAKE) {
            ret = gp_wake(addr, (int)a2);
        } else {
            gp_log("futex op %u is not implemented\n", a1);
            ret = GP_ENOSYS;
        }
        break;
    }

    /* ---- memory ---- */
    case SYS_brk: {
        /* brk(0) asks where the break is. Growing it commits; shrinking is
         * accepted but nothing is returned to the host, which is legal and
         * saves a decommit path nobody exercises. */
        if (a0 == 0) { ret = (int32_t)m.brk_cur; break; }
        if (a0 > m.brk_cur) {
            uint32_t lo = (m.brk_cur + GP_PAGE - 1) & ~(GP_PAGE - 1);
            uint32_t hi = (a0 + GP_PAGE - 1) & ~(GP_PAGE - 1);
            if (hi > lo && m.Commit(lo, hi - lo, GP_PROT_READ | GP_PROT_WRITE) < 0) {
                ret = (int32_t)m.brk_cur; break;
            }
        }
        m.brk_cur = a0;
        ret = (int32_t)m.brk_cur;
        break;
    }

    case SYS_mmap2: {
        /* addr, len, prot, flags, fd, pgoffset. MAP_FIXED is honoured; anything
         * else gets the next slot from the bump allocator, 64 KB aligned
         * because that is what Windows will insist on. */
        const uint32_t len   = (a1 + GP_PAGE - 1) & ~(GP_PAGE - 1);
        const uint32_t flags = a3;
        const int32_t  fd    = (int32_t)r[4];
        const uint32_t pgoff = r[5];
        const bool fixed = (flags & 0x10) != 0;

        uint32_t at = fixed ? (a0 & ~(GP_PAGE - 1)) : m.mmap_next;
        if (!fixed) m.mmap_next = (at + len + GP_ALLOC_GRAN - 1) & ~(GP_ALLOC_GRAN - 1);

        if (m.Commit(at, len, GP_PROT_READ | GP_PROT_WRITE) < 0) { ret = GP_ENOMEM; break; }

        if (fd >= 0) {
            /* File-backed. Read it in rather than mapping: a real shared
             * mapping is the one thing Win32 cannot place inside a reservation
             * without Windows 10 APIs, and the guest's file mappings are
             * private (MAP_PRIVATE for shared objects) in every case the
             * census produced. */
            GuestFd *g = m.Fd(fd);
            if (!g || !g->file) { ret = GP_EBADF; break; }
            std::memset(m.Ptr(at), 0, len);
            gp_pread(g->file, m.Ptr(at), a1, (uint64_t)pgoff * GP_PAGE);
        } else {
            std::memset(m.Ptr(at), 0, len);
        }

        int prot = 0;
        if (a2 & 1) prot |= GP_PROT_READ;
        if (a2 & 2) prot |= GP_PROT_WRITE;
        if (a2 & 4) prot |= GP_PROT_EXEC;
        if (prot) m.Commit(at, len, prot);
        ret = (int32_t)at;
        break;
    }

    case SYS_munmap:
        /* Deliberately a no-op for now. Returning the pages is correct but the
         * bump allocator never reuses them, and a wrong decommit is far more
         * expensive to debug than some retained address space. */
        ret = 0;
        break;

    case SYS_mprotect: {
        int prot = 0;
        if (a2 & 1) prot |= GP_PROT_READ;
        if (a2 & 2) prot |= GP_PROT_WRITE;
        if (a2 & 4) prot |= GP_PROT_EXEC;
        uint32_t lo = a0 & ~(GP_PAGE - 1);
        uint32_t hi = (a0 + a1 + GP_PAGE - 1) & ~(GP_PAGE - 1);
        ret = m.Commit(lo, hi - lo, prot) < 0 ? GP_ENOMEM : 0;
        break;
    }

    /* ---- files ---- */
    case SYS_open: {
        std::string p = m.Str(a0);
        std::string h = m.HostPath(p);
        if (m.trace) tpath = p + " -> " + h;
        gp_file *f = nullptr;
        int r0 = gp_open(h.c_str(), open_flags(a1), a2, &f);
        if (r0 < 0) { ret = r0; break; }
        ret = m.AllocFd(f, nullptr, p);
        if (ret < 0) gp_close(f);
        else m.fds[ret].oflags = a1;
        break;
    }

    case SYS_close: {
        GuestFd *g = m.Fd((int)a0);
        if (!g) { ret = GP_EBADF; break; }
        if (g->file) gp_close(g->file);
        if (g->dir)  gp_dirclose(g->dir);
        *g = GuestFd{};
        ret = 0;
        break;
    }

    case SYS_read: {
        GuestFd *g = m.Fd((int)a0);
        if (!g || !g->file) { ret = GP_EBADF; break; }
        ret = (int32_t)gp_read(g->file, m.Ptr(a1), a2);
        break;
    }

    case SYS_write: {
        if (a0 == 1 || a0 == 2) { ret = (int32_t)gp_console_write((int)a0, m.Ptr(a1), a2); break; }
        GuestFd *g = m.Fd((int)a0);
        if (!g || !g->file) { ret = GP_EBADF; break; }
        ret = (int32_t)gp_write(g->file, m.Ptr(a1), a2);
        break;
    }

    case SYS_writev: {
        /* iovec is two words per entry on 32-bit ARM. */
        int64_t total = 0;
        const uint32_t *iov = (const uint32_t *)m.Ptr(a1);
        for (uint32_t i = 0; i < a2; i++) {
            uint32_t bufp = iov[i * 2], lenp = iov[i * 2 + 1];
            if (!lenp) continue;
            int64_t n;
            if (a0 == 1 || a0 == 2) {
                n = gp_console_write((int)a0, m.Ptr(bufp), lenp);
            } else {
                GuestFd *g = m.Fd((int)a0);
                if (!g || !g->file) { n = GP_EBADF; }
                else n = gp_write(g->file, m.Ptr(bufp), lenp);
            }
            if (n < 0) { total = n; break; }
            total += n;
        }
        ret = (int32_t)total;
        break;
    }

    case SYS_lseek: {
        GuestFd *g = m.Fd((int)a0);
        if (!g || !g->file) { ret = GP_EBADF; break; }
        ret = (int32_t)gp_seek(g->file, (int32_t)a1, (int)a2);
        break;
    }

    case SYS_llseek: {
        /* fd, offset_high, offset_low, result*, whence */
        GuestFd *g = m.Fd((int)a0);
        if (!g || !g->file) { ret = GP_EBADF; break; }
        int64_t off = ((int64_t)a1 << 32) | a2;
        int64_t n = gp_seek(g->file, off, (int)r[4]);
        if (n < 0) { ret = (int32_t)n; break; }
        if (a3) { int64_t *p = (int64_t *)m.Ptr(a3); *p = n; }
        ret = 0;
        break;
    }

    case SYS_stat:
    case SYS_lstat: {
        gp_statbuf st;
        if (m.trace) tpath = m.Str(a0);
        int r0 = gp_stat(m.HostPath(m.Str(a0)).c_str(), &st);
        if (r0 < 0) { ret = r0; break; }
        fill_stat(st, (guest_stat *)m.Ptr(a1));
        ret = 0;
        break;
    }

    case SYS_fstat: {
        GuestFd *g = m.Fd((int)a0);
        if (!g || !g->file) { ret = GP_EBADF; break; }
        gp_statbuf st;
        int r0 = gp_fstat(g->file, &st);
        if (r0 < 0) { ret = r0; break; }
        fill_stat(st, (guest_stat *)m.Ptr(a1));
        ret = 0;
        break;
    }

    case SYS_stat64: {
        gp_statbuf st;
        int r0 = gp_stat(m.HostPath(m.Str(a0)).c_str(), &st);
        if (r0 < 0) { ret = r0; break; }
        fill_stat64(st, (guest_stat64 *)m.Ptr(a1));
        ret = 0;
        break;
    }

    case SYS_fstat64: {
        GuestFd *g = m.Fd((int)a0);
        if (!g || !g->file) { ret = GP_EBADF; break; }
        gp_statbuf st;
        int r0 = gp_fstat(g->file, &st);
        if (r0 < 0) { ret = r0; break; }
        fill_stat64(st, (guest_stat64 *)m.Ptr(a1));
        ret = 0;
        break;
    }

    case SYS_access: {
        gp_statbuf st;
        if (m.trace) tpath = m.Str(a0);
        ret = gp_stat(m.HostPath(m.Str(a0)).c_str(), &st) < 0 ? GP_ENOENT : 0;
        break;
    }

    case SYS_readlink:
        /* Nothing in the rootfs that the guest reads is a symlink we need to
         * resolve, and EINVAL ("not a link") is the answer that makes uClibc
         * fall through rather than retry. */
        ret = GP_EINVAL;
        break;

    case SYS_mknod: {
        /* The shim's five input event nodes. Only FIFOs appear here — a guest
         * asking for a real device node is a fact worth hearing about, not
         * something to quietly succeed at. */
        constexpr uint32_t S_IFMT = 0xf000, S_IFIFO = 0x1000;
        std::string p = m.Str(a0);
        if (m.trace) tpath = p;
        if ((a1 & S_IFMT) != S_IFIFO) {
            gp_log("mknod(%s, mode %o): not a FIFO, and device nodes are not "
                   "implemented — the shim is supposed to have absorbed those\n",
                   p.c_str(), a1);
            ret = GP_EPERM;
            break;
        }
        ret = gp_mkfifo(m.HostPath(p).c_str(), a1 & 07777);
        if (ret == GP_EEXIST) ret = 0;   /* a leftover FIFO is not a failure */
        break;
    }

    case SYS_ftruncate: {
        GuestFd *g = m.Fd((int)a0);
        if (!g || !g->file) { ret = GP_EBADF; break; }
        ret = gp_truncate(g->file, a1);
        break;
    }

    case SYS_getdents64: {
        /* fd, buf, count. The guest's struct linux_dirent64 is
         * { u64 ino; s64 off; u16 reclen; u8 type; char name[]; }, packed, and
         * padded so each record is 8-aligned. */
        GuestFd *g = m.Fd((int)a0);
        if (!g) { ret = GP_EBADF; break; }
        if (!g->dir) {
            int r0 = gp_diropen(m.HostPath(g->path).c_str(), &g->dir);
            if (r0 < 0) { ret = r0; break; }
        }
        uint8_t *out = m.Ptr(a1);
        uint32_t used = 0;
        for (;;) {
            const char *name = nullptr;
            uint32_t is_dir = 0;
            int r0 = gp_dirnext(g->dir, &name, &is_dir);
            if (r0 <= 0) { ret = r0 < 0 ? r0 : (int32_t)used; break; }
            const uint32_t nlen = (uint32_t)std::strlen(name);
            const uint32_t rec  = (19 + nlen + 1 + 7) & ~7u;
            if (used + rec > a2) {
                /* No room. The entry is lost, because this interface cannot
                 * push one back — a real getdents64 would rewind. Files per
                 * directory here are few and buffers are 4 KB, so say it
                 * loudly rather than silently dropping names. */
                gp_log("getdents64: buffer too small, dropped '%s'\n", name);
                ret = (int32_t)used;
                break;
            }
            uint8_t *e = out + used;
            uint64_t ino = 1 + used;
            std::memcpy(e + 0, &ino, 8);
            int64_t off = used + rec;
            std::memcpy(e + 8, &off, 8);
            uint16_t reclen = (uint16_t)rec;
            std::memcpy(e + 16, &reclen, 2);
            e[18] = is_dir ? 4 /*DT_DIR*/ : 8 /*DT_REG*/;
            std::memcpy(e + 19, name, nlen + 1);
            used += rec;
        }
        break;
    }

    case SYS_unlink: ret = gp_unlink(m.HostPath(m.Str(a0)).c_str()); break;
    case SYS_mkdir:  ret = gp_mkdir (m.HostPath(m.Str(a0)).c_str(), a1); break;
    case SYS_rmdir:  ret = gp_rmdir (m.HostPath(m.Str(a0)).c_str()); break;

    case SYS_ioctl:
        /* Every ioctl in the census was TCGETS, libc asking whether a
         * descriptor is a terminal. ENOTTY is the truthful answer and the one
         * that makes stdio pick block buffering. */
        ret = GP_ENOTTY;
        break;

    case SYS_fcntl:
    case SYS_fcntl64: {
        /* F_GETFL is the one that matters: uClibc's stdio asks it to decide
         * whether a stream may be written, and answering 0 for stdout says
         * O_RDONLY. Standard descriptors are not in the table yet, so they are
         * answered directly here rather than pretended about. */
        const uint32_t cmd = a1;
        if (cmd == 3 /*F_GETFL*/) {
            if (a0 == 0)                 ret = 0;   /* O_RDONLY */
            else if (a0 == 1 || a0 == 2) ret = 1;   /* O_WRONLY */
            else { GuestFd *g = m.Fd((int)a0); ret = g ? (int32_t)g->oflags : GP_EBADF; }
            break;
        }
        ret = 0;   /* F_GETFD, F_SETFD, F_SETFL: accepted */
        break;
    }

    case SYS_getcwd: {
        const char *cwd = "/";
        size_t n = std::strlen(cwd) + 1;
        if (a1 < n) { ret = GP_EINVAL; break; }
        std::memcpy(m.Ptr(a0), cwd, n);
        ret = (int32_t)n;
        break;
    }

    /* ---- time ---- */
    case SYS_gettimeofday: {
        if (a0) {
            uint64_t ns = gp_wall_ns();
            uint32_t *tv = (uint32_t *)m.Ptr(a0);
            tv[0] = (uint32_t)(ns / 1000000000ull);
            tv[1] = (uint32_t)((ns % 1000000000ull) / 1000);
        }
        ret = 0;
        break;
    }

    case SYS_clock_gettime: {
        uint64_t ns = (a0 == 1 /*MONOTONIC*/) ? gp_mono_ns() : gp_wall_ns();
        uint32_t *ts = (uint32_t *)m.Ptr(a1);
        ts[0] = (uint32_t)(ns / 1000000000ull);
        ts[1] = (uint32_t)(ns % 1000000000ull);
        ret = 0;
        break;
    }

    case SYS_nanosleep: {
        const uint32_t *ts = (const uint32_t *)m.Ptr(a0);
        uint64_t ns = (uint64_t)ts[0] * 1000000000ull + ts[1];
        guard.unlock();          /* never sleep holding the lock */
        gp_sleep_ns(ns);
        ret = 0;
        break;
    }

    case SYS_sched_yield: gp_thread_yield(); ret = 0; break;

    case SYS_uname: {
        /* Six 65-byte fields. The guest checks the release string to decide
         * which kernel features exist, so it claims what the real device ran. */
        char *u = (char *)m.Ptr(a0);
        std::memset(u, 0, 6 * 65);
        std::strcpy(u + 0 * 65, "Linux");
        std::strcpy(u + 1 * 65, "leappad");
        std::strcpy(u + 2 * 65, "2.6.32");
        std::strcpy(u + 3 * 65, "#1 PREEMPT");
        std::strcpy(u + 4 * 65, "armv7l");
        std::strcpy(u + 5 * 65, "(none)");
        ret = 0;
        break;
    }

    default:
        gp_log("unimplemented syscall %u (%s) at pc=%08x — "
               "args %08x %08x %08x %08x\n",
               nr, name_of(nr), r[15], a0, a1, a2, a3);
        ret = GP_ENOSYS;
        break;
    }

    /* Always carry the NUMBER as well as the name. A trace line reading "?" is
     * an instrument that hides the one fact you needed from it. */
    if (m.trace) {
        if (!tpath.empty())
            gp_log("  [%u] %3u %-14s %s = %d\n", t.tid, nr, name_of(nr), tpath.c_str(), ret);
        else
            gp_log("  [%u] %3u %-14s(%08x, %08x, %08x) = %d\n",
                   t.tid, nr, name_of(nr), a0, a1, a2, ret);
    }

    r[0] = (uint32_t)ret;
}
