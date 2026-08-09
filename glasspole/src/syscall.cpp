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

#include <chrono>
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
    SYS_ugetrlimit = 191, SYS_mmap2 = 192, SYS_stat64 = 195, SYS_lstat64 = 196,
    SYS_fstat64 = 197,
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
    SYS_mknod = 14, SYS_ftruncate = 93, SYS_getdents64 = 217, SYS_getdents = 141,
    /* POSIX message queues. Brio's task communication runs on these. */
    SYS_mq_open = 274, SYS_mq_unlink = 275, SYS_mq_timedsend = 276,
    SYS_mq_timedreceive = 277, SYS_mq_notify = 278, SYS_mq_getsetattr = 279,
    /* The scheduler family. Brio sets thread priorities at startup and treats
     * a failure here as fatal. */
    SYS_sched_setparam = 154, SYS_sched_getparam = 155,
    SYS_sched_setscheduler = 156, SYS_sched_getscheduler = 157,
    SYS_sched_get_priority_max = 159, SYS_sched_get_priority_min = 160,
    SYS_kill = 37, SYS_poll = 168, SYS_sysinfo = 116,
    SYS_fdatasync = 148, SYS_fsync = 118, SYS_newselect = 142, SYS_statfs = 99,
    /* AF_UNIX sockets, implemented in-process. ARM has direct socket calls
     * rather than the old socketcall multiplexer. */
    SYS_socket = 281, SYS_bind = 282, SYS_connect = 283, SYS_listen = 284,
    SYS_accept = 285, SYS_send = 289, SYS_recv = 291, SYS_shutdown = 293,
    SYS_setsockopt = 294, SYS_getsockopt = 295, SYS_sendto = 290, SYS_recvfrom = 292,
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
    if (f & 0x800)  o |= GP_O_NONBLOCK;
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
        case SYS_lstat64: return "lstat64";
        case SYS_uname: return "uname";             case SYS_llseek: return "_llseek";
        case SYS_gettimeofday: return "gettimeofday";
        case SYS_clock_gettime: return "clock_gettime";
        case SYS_ARM_set_tls: return "set_tls";     case SYS_writev: return "writev";
        case SYS_access: return "access";           case SYS_readlink: return "readlink";
        case SYS_exit_group: return "exit_group";   case SYS_getcwd: return "getcwd";
        case SYS_clone: return "clone";             case SYS_futex: return "futex";
        case SYS_gettid: return "gettid";           case SYS_fcntl64: return "fcntl64";
        case SYS_mknod: return "mknod";             case SYS_ftruncate: return "ftruncate";
        case SYS_getdents64: return "getdents64";   case SYS_getdents: return "getdents";
        case SYS_mq_open: return "mq_open";         case SYS_mq_unlink: return "mq_unlink";
        case SYS_mq_timedsend: return "mq_timedsend";
        case SYS_mq_timedreceive: return "mq_timedreceive";
        case SYS_mq_getsetattr: return "mq_getsetattr";
        case SYS_sched_get_priority_max: return "sched_get_priority_max";
        case SYS_sched_get_priority_min: return "sched_get_priority_min";
        case SYS_sched_getscheduler: return "sched_getscheduler";
        case SYS_sched_setscheduler: return "sched_setscheduler";
        case SYS_socket: return "socket";           case SYS_kill: return "kill";
        case SYS_bind: return "bind";               case SYS_connect: return "connect";
        case SYS_listen: return "listen";           case SYS_accept: return "accept";
        case SYS_send: return "send";               case SYS_recv: return "recv";
        case SYS_poll: return "poll";               case SYS_sysinfo: return "sysinfo";
        case SYS_fdatasync: return "fdatasync";     case SYS_fsync: return "fsync";
        case SYS_newselect: return "_newselect";     case SYS_rename: return "rename";
        case SYS_statfs: return "statfs";
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

    /* ---- scheduling ---- */
    /*
     * Accepted and largely ignored. There is one real host thread per guest
     * thread and the host's scheduler is already running them; what matters is
     * that the RANGE is sane, because Brio computes its own priorities from it
     * and treats a failure as fatal. The numbers below are Linux's.
     */
    case SYS_sched_get_priority_max:
        ret = (a0 == 1 /*SCHED_FIFO*/ || a0 == 2 /*SCHED_RR*/) ? 99 : 0;
        break;
    case SYS_sched_get_priority_min:
        ret = (a0 == 1 || a0 == 2) ? 1 : 0;
        break;
    case SYS_sched_getscheduler:
        ret = 0;      /* SCHED_OTHER */
        break;
    case SYS_sched_setscheduler:
    case SYS_sched_setparam:
        ret = 0;
        break;
    case SYS_sched_getparam:
        if (a1) { uint32_t z = 0; std::memcpy(m.Ptr(a1), &z, 4); }
        ret = 0;
        break;

    case SYS_kill:
        /* Nothing here can deliver a signal yet. Reporting success for a
         * signal that will never arrive is worse than refusing. */
        gp_log("kill(%u, %u) — signals are not implemented\n", a0, a1);
        ret = GP_EPERM;
        break;

    case SYS_poll: {
        /* struct pollfd is { int fd; short events; short revents; } — 8 bytes.
         * Sockets are answered from our own buffers; anything backed by a real
         * file goes to the host, because the shim's event FIFOs are written by
         * the viewer in another process. */
        const uint32_t nfds = a1;
        if (nfds > 64) { ret = GP_EINVAL; break; }
        uint8_t *p = m.Ptr(a0);

        gp_file *hf[64] = {};
        int hidx[64];
        int hn = 0;
        int hits = 0;

        for (uint32_t i = 0; i < nfds; i++) {
            int32_t fd; std::memcpy(&fd, p + i * 8, 4);
            uint16_t rev = 0;
            GuestFd *g = m.Fd(fd);
            if (!g) {
                rev = 0x20;                     /* POLLNVAL */
            } else if (g->sock) {
                std::lock_guard<std::mutex> sg(g->sock->mu);
                if (!g->sock->rx.empty()) rev = 0x01;   /* POLLIN */
            } else if (g->file) {
                hf[hn] = g->file; hidx[hn] = (int)i; hn++;
            } else {
                rev = 0x01;                     /* the console: always ready */
            }
            std::memcpy(p + i * 8 + 6, &rev, 2);
            if (rev) hits++;
        }

        if (hits) { ret = hits; break; }

        /* Nothing ready, and nothing backed by a real file to wait on — every
         * descriptor was a socket or the console. Returning 0 here would be the
         * same false timeout fixed below, AND a busy loop: the guest asks for a
         * second, gets "expired" instantly, and asks again. Sleep out the real
         * timeout instead. */
        if (hn == 0) {
            const int32_t ms = (int32_t)a2;
            guard.unlock();
            if (ms < 0) {
                while (!m.exiting.load(std::memory_order_relaxed)) gp_sleep_ns(50000000ull);
            } else {
                const uint64_t deadline0 = gp_mono_ns() + (uint64_t)ms * 1000000ull;
                while (!m.exiting.load(std::memory_order_relaxed) && gp_mono_ns() < deadline0)
                    gp_sleep_ns(20000000ull);
            }
            ret = 0;
            break;
        }

        /* Nothing ready yet, and real files to wait on.
         *
         * THE WAIT IS SLICED BUT THE TIMEOUT IS NOT. Slicing lets exit_group be
         * noticed instead of slept through; returning 0 at the end of a slice
         * would tell the guest its wait EXPIRED, which is a lie — poll returning
         * 0 means the full timeout elapsed, and a state machine keyed on that
         * will act on a timeout that never happened. So loop until the real
         * deadline, and only then report zero. */
        const int32_t want = (int32_t)a2;
        const uint64_t deadline = want < 0 ? 0 : gp_mono_ns() + (uint64_t)want * 1000000ull;
        guard.unlock();

        for (;;) {
            if (m.exiting.load(std::memory_order_relaxed)) { ret = 0; break; }

            int slice = 100;
            if (deadline) {
                const uint64_t now = gp_mono_ns();
                if (now >= deadline) { ret = 0; break; }
                const uint64_t left_ms = (deadline - now) / 1000000ull;
                if (left_ms < 100) slice = (int)left_ms + 1;
            }

            unsigned char ready[64] = {};
            int r0 = gp_poll_readable(hf, hn, slice, ready);
            if (r0 < 0) { ret = r0; break; }
            if (r0 == 0) continue;              /* slice expired, deadline has not */

            for (int i = 0; i < hn; i++) {
                if (!ready[i]) continue;
                uint16_t rev = 0x01;
                std::memcpy(p + hidx[i] * 8 + 6, &rev, 2);
                hits++;
            }
            ret = hits;
            break;
        }
        break;
    }

    /* ---- AF_UNIX sockets ---- */
    case SYS_socket: {
        /* domain, type, protocol. Only AF_UNIX matters: nothing in this
         * workload talks to a network, and pretending to would be worse than
         * refusing. */
        constexpr uint32_t AF_UNIX = 1;
        if (a0 != AF_UNIX) { ret = GP_EAFNOSUPPORT; break; }
        auto sk = std::make_shared<UnixSocket>();
        ret = m.AllocFd(nullptr, nullptr, "<socket>");
        if (ret >= 0) m.fds[ret].sock = std::move(sk);
        break;
    }

    case SYS_bind: {
        /* fd, sockaddr_un*, len. sockaddr_un is { u16 family; char path[108] }. */
        GuestFd *g = m.Fd((int)a0);
        if (!g || !g->sock) { ret = GP_EBADF; break; }
        std::string name((const char *)m.Ptr(a1 + 2));
        if (m.trace) tpath = name;
        g->sock->name = name;
        m.bound[name] = g->sock;
        /* The guest expects a filesystem entry to appear; some code stats it.
         * A regular file is close enough and unlinks the same way. */
        gp_file *touch = nullptr;
        if (gp_open(m.HostPath(name).c_str(), GP_O_WRONLY | GP_O_CREAT, 0666, &touch) == 0)
            gp_close(touch);
        ret = 0;
        break;
    }

    case SYS_listen: {
        GuestFd *g = m.Fd((int)a0);
        if (!g || !g->sock) { ret = GP_EBADF; break; }
        g->sock->listening = true;
        ret = 0;
        break;
    }

    case SYS_connect: {
        GuestFd *g = m.Fd((int)a0);
        if (!g || !g->sock) { ret = GP_EBADF; break; }
        std::string name((const char *)m.Ptr(a1 + 2));
        if (m.trace) tpath = name;

        auto it = m.bound.find(name);
        std::shared_ptr<UnixSocket> server = it == m.bound.end() ? nullptr : it->second.lock();
        if (!server || !server->listening) {
            /* The documented benign case: DaemonControl with no VideoDaemon
             * behind it. ECONNREFUSED is what the guest is used to seeing. */
            ret = GP_ECONNREFUSED;
            break;
        }

        /* A fresh socket for the server's end, cross-linked with the client's. */
        auto server_end = std::make_shared<UnixSocket>();
        server_end->peer = g->sock;
        g->sock->peer    = server_end;
        {
            std::lock_guard<std::mutex> sg(server->mu);
            server->backlog.push_back(server_end);
        }
        server->cv.notify_all();
        ret = 0;
        break;
    }

    case SYS_accept: {
        GuestFd *g = m.Fd((int)a0);
        if (!g || !g->sock) { ret = GP_EBADF; break; }
        auto server = g->sock;
        guard.unlock();

        std::shared_ptr<UnixSocket> conn;
        {
            std::unique_lock<std::mutex> sg(server->mu);
            while (server->backlog.empty()) {
                if (m.exiting.load(std::memory_order_relaxed)) break;
                server->cv.wait_for(sg, std::chrono::milliseconds(100));
            }
            if (!server->backlog.empty()) {
                conn = server->backlog.front();
                server->backlog.pop_front();
            }
        }
        if (!conn) { ret = GP_EINTR; break; }

        guard.lock();
        ret = m.AllocFd(nullptr, nullptr, "<accepted>");
        if (ret >= 0) m.fds[ret].sock = std::move(conn);
        break;
    }

    case SYS_send:
    case SYS_sendto: {
        GuestFd *g = m.Fd((int)a0);
        if (!g || !g->sock) { ret = GP_EBADF; break; }
        auto peer = g->sock->peer.lock();
        if (!peer) { ret = GP_EPIPE; break; }
        std::string payload((const char *)m.Ptr(a1), a2);
        {
            std::lock_guard<std::mutex> sg(peer->mu);
            peer->rx.push_back(std::move(payload));
        }
        peer->cv.notify_all();
        ret = (int32_t)a2;
        break;
    }

    case SYS_recv:
    case SYS_recvfrom: {
        GuestFd *g = m.Fd((int)a0);
        if (!g || !g->sock) { ret = GP_EBADF; break; }
        auto sk = g->sock;
        guard.unlock();

        std::string msg;
        {
            std::unique_lock<std::mutex> sg(sk->mu);
            while (sk->rx.empty() && !sk->closed) {
                if (m.exiting.load(std::memory_order_relaxed)) break;
                sk->cv.wait_for(sg, std::chrono::milliseconds(100));
            }
            if (!sk->rx.empty()) { msg = std::move(sk->rx.front()); sk->rx.pop_front(); }
        }
        if (msg.empty()) { ret = 0; break; }        /* peer gone: end of stream */
        const uint32_t n = msg.size() < a2 ? (uint32_t)msg.size() : a2;
        std::memcpy(m.Ptr(a1), msg.data(), n);
        ret = (int32_t)n;
        break;
    }

    case SYS_shutdown:
    case SYS_setsockopt:
    case SYS_getsockopt:
        ret = 0;
        break;

    /* ---- message queues ---- */
    /*
     * Entirely ours: no host primitive is involved, because every one of these
     * is between threads of a single guest process. Windows has no mq_* and
     * does not need one.
     */
    case SYS_mq_open: {
        /* name, oflag, mode, attr */
        std::string name = m.Str(a0);
        if (m.trace) tpath = name;
        constexpr uint32_t O_CREAT = 0x40, O_EXCL = 0x80;

        auto it = m.mqs.find(name);
        if (it == m.mqs.end()) {
            if (!(a1 & O_CREAT)) { ret = GP_ENOENT; break; }
            auto q = std::make_shared<MsgQueue>();
            if (a3) {
                /* struct mq_attr is longs: flags, maxmsg, msgsize, curmsgs. */
                const uint32_t *at = (const uint32_t *)m.Ptr(a3);
                if (at[1]) q->maxmsg  = at[1];
                if (at[2]) q->msgsize = at[2];
            }
            it = m.mqs.emplace(name, std::move(q)).first;
        } else if ((a1 & O_CREAT) && (a1 & O_EXCL)) {
            ret = GP_EEXIST;
            break;
        }

        ret = m.AllocFd(nullptr, nullptr, name);
        if (ret >= 0) {
            m.fds[ret].mq     = it->second;
            m.fds[ret].oflags = a1;
        }
        break;
    }

    case SYS_mq_unlink:
        /* The name goes; descriptors already open keep working, because they
         * hold a shared_ptr. That is what POSIX promises. */
        if (m.trace) tpath = m.Str(a0);
        ret = m.mqs.erase(m.Str(a0)) ? 0 : GP_ENOENT;
        break;

    case SYS_mq_timedsend:
    case SYS_mq_timedreceive: {
        /* mqdes, msg_ptr, msg_len, msg_prio(+), abs_timeout */
        GuestFd *g = m.Fd((int)a0);
        if (!g || !g->mq) { ret = GP_EBADF; break; }
        auto q = g->mq;                       /* keep it alive past the unlock */
        const bool sending    = nr == SYS_mq_timedsend;
        const bool nonblock   = (g->oflags & 0x800 /*O_NONBLOCK*/) != 0;
        const uint32_t abstime = r[4];

        /* An ABSOLUTE deadline, unlike futex's relative one. Converting here
         * rather than at the wait keeps the clock read out of the lock. */
        uint64_t deadline_ns = 0;
        if (abstime) {
            const uint32_t *ts = (const uint32_t *)m.Ptr(abstime);
            deadline_ns = (uint64_t)ts[0] * 1000000000ull + ts[1];
        }

        if (sending && a2 > q->msgsize) { ret = GP_EINVAL; break; }

        /* Copy in before unlocking; guest memory is stable but the descriptor
         * table is not. */
        std::string payload;
        if (sending) payload.assign((const char *)m.Ptr(a1), a2);

        guard.unlock();

        std::unique_lock<std::mutex> qg(q->mu);
        for (;;) {
            if (m.exiting.load(std::memory_order_relaxed)) { ret = GP_EINTR; break; }

            if (sending) {
                if (q->msgs.size() < q->maxmsg) {
                    q->msgs.emplace(a3, std::move(payload));
                    q->cv.notify_all();
                    ret = 0;
                    break;
                }
            } else if (!q->msgs.empty()) {
                auto first = q->msgs.begin();
                const uint32_t prio = first->first;
                const std::string msg = std::move(first->second);
                q->msgs.erase(first);
                q->cv.notify_all();
                qg.unlock();
                if (msg.size() > a2) { ret = GP_EMSGSIZE; break; }
                std::memcpy(m.Ptr(a1), msg.data(), msg.size());
                if (a3) { uint32_t p = prio; std::memcpy(m.Ptr(a3), &p, 4); }
                ret = (int32_t)msg.size();
                break;
            }

            if (nonblock) { ret = GP_EAGAIN; break; }

            /* Always a timed wait, even with no deadline: a queue that is never
             * fed would otherwise ignore exit_group and hang the process on the
             * way out. */
            if (deadline_ns) {
                const uint64_t now = gp_wall_ns();
                if (now >= deadline_ns) { ret = GP_ETIMEDOUT; break; }
                uint64_t left = deadline_ns - now;
                if (left > 100000000ull) left = 100000000ull;
                q->cv.wait_for(qg, std::chrono::nanoseconds(left));
            } else {
                q->cv.wait_for(qg, std::chrono::milliseconds(100));
            }
        }
        break;
    }

    case SYS_mq_getsetattr: {
        /* mqdes, newattr, oldattr */
        GuestFd *g = m.Fd((int)a0);
        if (!g || !g->mq) { ret = GP_EBADF; break; }
        if (a2) {
            std::lock_guard<std::mutex> qg(g->mq->mu);
            uint32_t *out = (uint32_t *)m.Ptr(a2);
            out[0] = g->oflags & 0x800;          /* mq_flags: O_NONBLOCK only */
            out[1] = g->mq->maxmsg;
            out[2] = g->mq->msgsize;
            out[3] = (uint32_t)g->mq->msgs.size();
        }
        ret = 0;
        break;
    }

    case SYS_mq_notify:
        /* Nothing in the census ever registered a notification, and answering
         * "no" is better than pretending to deliver one. */
        ret = GP_ENOSYS;
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

        int prot = 0;
        if (a2 & 1) prot |= GP_PROT_READ;
        if (a2 & 2) prot |= GP_PROT_WRITE;
        if (a2 & 4) prot |= GP_PROT_EXEC;

        /* MAP_SHARED on a real file has to be a REAL shared mapping. This is
         * the shim's pixel path: it hands the guest a descriptor onto a host
         * file, the guest maps it, and the viewer maps the same file so both
         * sides see one set of pages. Copying the contents in would run
         * perfectly and leave the screen black. */
        constexpr uint32_t MAP_SHARED = 0x01;
        if (fd >= 0 && (flags & MAP_SHARED)) {
            GuestFd *g = m.Fd(fd);
            if (!g || !g->file) { ret = GP_EBADF; break; }
            int r0 = gp_map_shared(m.Ptr(at), len, g->file,
                                   (uint64_t)pgoff * GP_PAGE, prot);
            if (r0 < 0) { ret = r0; break; }
            ret = (int32_t)at;
            break;
        }

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

    /* lstat64 deliberately FOLLOWS symlinks here, which is not what lstat
     * means anywhere else.
     *
     * runtime/sysroot is a symlink farm: /LF/Base and /bin point at the real
     * rootfs on the host. Answering truthfully — "that is a symlink to
     * /home/..." — hands the guest a host path it cannot use, and anything
     * walking the tree sees links instead of directories. Following them is
     * what makes the sysroot look like a filesystem to the guest.
     *
     * Its absence is what sent AppManager down the CriticalDoom path: `ls`
     * and every directory scan got ENOSYS, so the firmware looked like it was
     * not installed at all. */
    case SYS_lstat64:
    case SYS_stat64: {
        gp_statbuf st;
        if (m.trace) tpath = m.Str(a0);
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

    case SYS_fsync:
    case SYS_fdatasync: {
        /* CAtomicFile writes a temp file, syncs it and renames. Without this
         * it logs "fdatasync failed; returning -1" on every save, and the
         * atomic-rename contract it is built on quietly stops holding. */
        GuestFd *g = m.Fd((int)a0);
        if (!g || !g->file) { ret = GP_EBADF; break; }
        ret = gp_sync(g->file);
        break;
    }

    case SYS_newselect: {
        /* nfds, readfds, writefds, exceptfds, timeout. Only the read set is
         * ever non-empty here, so it is answered through the same path as
         * poll rather than growing a second implementation of the same idea. */
        const uint32_t nfds = a0;
        uint32_t *rfds = a1 ? (uint32_t *)m.Ptr(a1) : nullptr;
        if (!rfds || nfds == 0) {
            /* A pure sleep, which is what select with no descriptors is. */
            uint64_t ns = 0;
            if (r[4]) {
                const uint32_t *tv = (const uint32_t *)m.Ptr(r[4]);
                ns = (uint64_t)tv[0] * 1000000000ull + (uint64_t)tv[1] * 1000ull;
            }
            guard.unlock();
            const uint64_t deadline0 = gp_mono_ns() + ns;
            while (!m.exiting.load(std::memory_order_relaxed) && gp_mono_ns() < deadline0)
                gp_sleep_ns(ns < 20000000ull ? ns : 20000000ull);
            ret = 0;
            break;
        }

        gp_file *hf[64] = {};
        int hidx[64], hn = 0, hits = 0;
        for (uint32_t fd = 0; fd < nfds && fd < 64; fd++) {
            if (!(rfds[fd / 32] & (1u << (fd % 32)))) continue;
            GuestFd *g = m.Fd((int)fd);
            if (g && g->sock) {
                std::lock_guard<std::mutex> sg(g->sock->mu);
                if (!g->sock->rx.empty()) { hits++; continue; }
            } else if (g && g->file) {
                hf[hn] = g->file; hidx[hn] = (int)fd; hn++;
                continue;
            } else if (g) {
                hits++; continue;              /* the console: always ready */
            }
            rfds[fd / 32] &= ~(1u << (fd % 32));
        }
        if (hits || hn == 0) { ret = hits; break; }

        int ms = 100;
        if (r[4]) {
            const uint32_t *tv = (const uint32_t *)m.Ptr(r[4]);
            const uint64_t want = (uint64_t)tv[0] * 1000ull + tv[1] / 1000ull;
            if (want < 100) ms = (int)want;
        }
        guard.unlock();
        unsigned char ready[64] = {};
        int r0 = gp_poll_readable(hf, hn, ms, ready);
        if (r0 < 0) { ret = r0; break; }
        for (int i = 0; i < hn; i++)
            if (ready[i]) hits++; else rfds[hidx[i] / 32] &= ~(1u << (hidx[i] % 32));
        ret = hits;
        break;
    }

    case SYS_ftruncate: {
        GuestFd *g = m.Fd((int)a0);
        if (!g || !g->file) { ret = GP_EBADF; break; }
        ret = gp_truncate(g->file, a1);
        break;
    }

    /* The 32-bit getdents, which qemu-arm shows the guest still using in four
     * places. struct linux_dirent is { u32 ino; u32 off; u16 reclen; char
     * name[]; } with the type byte AFTER the name — an oddity of the old call
     * that is easy to get wrong by analogy with getdents64. */
    case SYS_getdents: {
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
            const uint32_t rec  = (10 + nlen + 1 + 1 + 3) & ~3u;
            if (used + rec > a2) {
                gp_log("getdents: buffer too small, dropped '%s'\n", name);
                ret = (int32_t)used;
                break;
            }
            uint8_t *e = out + used;
            uint32_t ino = 1 + used, off = used + rec;
            std::memcpy(e + 0, &ino, 4);
            std::memcpy(e + 4, &off, 4);
            uint16_t reclen = (uint16_t)rec;
            std::memcpy(e + 8, &reclen, 2);
            std::memcpy(e + 10, name, nlen + 1);
            e[rec - 1] = is_dir ? 4 /*DT_DIR*/ : 8 /*DT_REG*/;
            used += rec;
        }
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
    case SYS_rename: {
        /* CAtomicFile's whole contract: write a temp, sync it, rename over the
         * target. The enum had this number from the start and the case did not
         * exist, so every atomic save was failing at the last step. */
        std::string from = m.Str(a0), to = m.Str(a1);
        if (m.trace) tpath = from + " -> " + to;
        ret = gp_rename(m.HostPath(from).c_str(), m.HostPath(to).c_str());
        break;
    }
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

    case SYS_statfs: {
        /* struct statfs, 32-bit ARM: type, bsize, blocks, bfree, bavail,
         * files, ffree, fsid[2], namelen, frsize, flags, spare[4].
         *
         * The numbers describe the CONSOLE's storage rather than the host's,
         * for the same reason sysinfo does: the guest decides whether it has
         * room to install or save from these, and reporting a 2 TB host disk
         * invites it to believe things that are not true on real hardware. */
        uint32_t *sf = (uint32_t *)m.Ptr(a1);
        std::memset(sf, 0, 64);
        sf[0] = 0x858458f6;                  /* RAMFS_MAGIC, near enough */
        sf[1] = 4096;                        /* bsize */
        sf[2] = 1024u * 1024;                /* blocks: 4 GB */
        sf[3] = 512u * 1024;                 /* bfree:  2 GB */
        sf[4] = 512u * 1024;                 /* bavail */
        sf[5] = 100000;                      /* files */
        sf[6] = 50000;                       /* ffree */
        sf[9] = 255;                         /* namelen */
        ret = 0;
        break;
    }

    case SYS_sysinfo: {
        /* struct sysinfo, 32-bit ARM: uptime, loads[3], totalram, freeram,
         * sharedram, bufferram, totalswap, freeswap, procs, pad, totalhigh,
         * freehigh, mem_unit, then padding to 64 bytes.
         *
         * The device had 128 MB and the guest sizes caches from this, so the
         * numbers are the console's rather than the host's — reporting 32 GB
         * of host RAM would have it allocate accordingly. */
        uint32_t *si = (uint32_t *)m.Ptr(a0);
        std::memset(si, 0, 64);
        si[0]  = (uint32_t)(gp_mono_ns() / 1000000000ull);   /* uptime */
        si[4]  = 128u * 1024 * 1024;                          /* totalram */
        si[5]  =  96u * 1024 * 1024;                          /* freeram */
        si[9]  = 1;                                           /* procs */
        si[12] = 1;                                           /* mem_unit */
        ret = 0;
        break;
    }

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
