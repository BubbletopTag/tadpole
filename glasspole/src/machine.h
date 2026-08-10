/* Glasspole — the state a guest process runs in, split into what is shared and
 * what belongs to one thread.
 *
 * THE SPLIT IS THE POINT. Every clone() in the census was the standard NPTL
 * set — CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|CLONE_SETTLS
 * — so guest threads share absolutely everything except registers and the
 * thread pointer. Machine holds what is shared; Thread holds what is not. A
 * field in the wrong one is a data race that will present as a title behaving
 * differently on a fast machine.
 */
#ifndef GLASSPOLE_MACHINE_H
#define GLASSPOLE_MACHINE_H

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include "host.h"
#include "elf.h"
}

namespace Dynarmic {
class ExclusiveMonitor;
namespace A32 { class Jit; class Coprocessor; }
}  // namespace Dynarmic

struct Machine;

/* A POSIX message queue.
 *
 * IMPLEMENTED HERE RATHER THAN IN host.h ON PURPOSE. Brio uses these heavily —
 * the census counted 728 receives and 309 sends in one run — and every one of
 * them is between threads of a SINGLE guest process. There is nothing for a
 * host primitive to add, and Windows has no mq_* at all, so building it out of
 * the C++ standard library means the Windows backend never needs one.
 *
 * Ordering is by priority, highest first, then by arrival — which is what POSIX
 * promises and what a scheduler built on top of it will assume. */
struct MsgQueue {
    std::mutex mu;
    std::condition_variable cv;
    uint32_t maxmsg  = 10;
    uint32_t msgsize = 8192;
    /* Highest priority first: greater<> as the comparator, so begin() is the
     * message to deliver. */
    std::multimap<uint32_t, std::string, std::greater<uint32_t>> msgs;
};

/* An AF_UNIX socket, entirely ours.
 *
 * Same reasoning as MsgQueue: every socket in this workload is between parts of
 * one guest — AppManager's cartridge task, the DaemonControl channel, the
 * VideoDaemon event socket — so there is nothing for a host socket to add, and
 * Windows never needs Winsock for any of it.
 *
 * It also fixes a real divergence. Returning ENOSYS from socket() is NOT the
 * same failure the guest is used to: under qemu the socket is CREATED and the
 * later connect() is what fails, which the guest is documented to tolerate.
 * Failing earlier takes it down a path nothing tested. */
struct UnixSocket {
    std::mutex mu;
    std::condition_variable cv;
    std::string name;                                  /* bound path, if any */
    bool listening = false;
    bool closed    = false;
    std::deque<std::shared_ptr<UnixSocket>> backlog;   /* server: pending peers */
    std::weak_ptr<UnixSocket>               peer;      /* client/server: the far end */
    std::deque<std::string>                 rx;        /* bytes waiting to be read */
};

/* The guest's descriptor table. Guest fd numbers are OURS — the lowest free
 * slot, exactly as Linux promises — and have no relationship to anything the
 * host numbered. That is what lets the Win32 backend hand back a HANDLE
 * without a single line above this changing. */
struct GuestFd {
    gp_file *file = nullptr;
    gp_dir  *dir  = nullptr;
    std::string path;          /* kept for diagnostics and for fstat on dirs */
    uint32_t oflags = 0;       /* what the guest opened it with, for F_GETFL */
    /* A message queue descriptor is a descriptor: mqd_t is an int, and the
     * guest closes it with close(), so it lives in the same table. */
    /* An entry read from the directory but not yet delivered, because the
     * guest's buffer was full. Without this, getdents DROPS it — and a
     * directory with 110 packages in it does not fit in one 4 KB buffer, so
     * the guest silently sees a truncated filesystem. */
    std::string pending_name;
    uint32_t    pending_is_dir = 0;
    bool        has_pending    = false;
    std::shared_ptr<MsgQueue>   mq;
    std::shared_ptr<UnixSocket> sock;

    /* Which console stream this is, or -1. The standard descriptors are not
     * gp_files — on Windows a console handle is not a file handle — so they
     * were recognised by NUMBER, and write() short-circuited fd 1 and fd 2
     * before it ever looked in this table. Which meant `echo err >&2` did the
     * dup2 correctly and then wrote to stdout anyway: the redirection was
     * invisible because the table it edited was not consulted. Naming the
     * stream here lets a duplicate carry it. */
    int console = -1;

    /* WHO CLOSES THE FILE. dup(), dup2() and F_DUPFD hand the guest a second
     * descriptor onto ONE open file, and closing either must not close the
     * other — that is the whole meaning of the call. `file` stays a raw
     * pointer, because thirty call sites read it and none of them care; this
     * token is what the table owns, and the last descriptor to drop it is the
     * one that calls gp_close.
     *
     * A refcount rather than a shared_ptr<gp_file> because gp_file is opaque
     * by design (see host.h) and only the backend may destroy one. */
    std::shared_ptr<int> share;

    bool     used = false;
};

/* ---- signals ------------------------------------------------------------ */

/* One disposition, in the KERNEL's shape rather than libc's.
 *
 * rt_sigaction's struct is handler, flags, restorer, then a 64-bit mask, with
 * sigsetsize passed as a separate argument. libc's struct sigaction puts a
 * sigset_t between the handler and the flags and its size is a build-time
 * choice of the C library — so a layout copied from the host would put
 * sa_flags in the wrong place and SA_SIGINFO would never arrive. The shim's
 * tadpole_crash.c hit exactly that and documents it; both sides now name the
 * same struct.
 *
 * `handler` is a guest address, or 0 for SIG_DFL and 1 for SIG_IGN. */
struct SigAction {
    uint32_t handler  = 0;
    uint32_t flags    = 0;
    uint32_t restorer = 0;
    uint64_t mask     = 0;
};

/* 1..31 are the classic signals and nothing here uses the realtime range. */
constexpr int GP_NSIG = 32;

/* ---- one guest thread --------------------------------------------------- */

struct Thread {
    Machine *m   = nullptr;
    /* GUARDED BY Machine::lock, and it has to be: the Jit is a local inside
     * the function running this thread, so the pointer is only valid between
     * that frame publishing it and clearing it — and ExitGroup dereferences it
     * from ANOTHER thread to halt this one. The fault handler reads it without
     * the lock, which is unsafe in general and the least of the problems at
     * the point where it runs. */
    Dynarmic::A32::Jit *jit = nullptr;
    uint32_t tid = 0;

    /* Index into the shared ExclusiveMonitor. Scarce — the monitor is sized
     * once — so it is handed back by RetireThread when the thread ends. */
    size_t processor_id = 0;

    /* The ARM thread-pointer registers, read through CP15. PER THREAD, which
     * is the whole reason CP15 is constructed per thread too: dynarmic compiles
     * a direct load from these addresses, so every thread must be pointed at
     * its own. Sharing them would give every thread the main thread's TLS and
     * corrupt errno the instant a second thread ran. */
    uint32_t tls    = 0;
    uint32_t tls_rw = 0;

    /* CLONE_CHILD_CLEARTID. On exit we zero this guest word and futex-wake it,
     * which is exactly how pthread_join learns the thread is gone. Skip it and
     * every join hangs forever. */
    uint32_t clear_child_tid = 0;

    /* Watchdog. dynarmic runs until its tick budget is spent, so a bounded
     * budget gives us somewhere to stand: several budgets burnt without a
     * single syscall means this thread is spinning, and the pc at that moment
     * is the answer. It is what found the strex bug. */
    uint64_t ticks_left = 0;
    uint64_t syscalls   = 0;

    /* THE MASK IS PER THREAD AND THE HANDLERS ARE NOT. sigprocmask is defined
     * to affect only the calling thread, while CLONE_SIGHAND makes the
     * disposition table one per process — so they sit on opposite sides of the
     * split, and putting the mask in Machine would let one thread blocking a
     * signal during a critical section silently block it everywhere.
     *
     * `sigpend` is written by OTHER threads (tkill aimed here, and a
     * process-directed kill), which is why it is atomic while `sigmask` — only
     * ever touched by this thread — is not. */
    uint64_t              sigmask = 0;
    std::atomic<uint64_t> sigpend{ 0 };

    /* Which round of "describe yourself" this thread has already answered.
     * A host SIGQUIT cannot touch the thread list from its handler, so it only
     * bumps Machine::quit_gen and each thread notices for itself — see
     * gp_install_quit_handler in host.h. */
    uint32_t quit_seen = 0;

    bool exited = false;

    void Halt();              /* stop just this thread */
};

/* ---- everything the threads share --------------------------------------- */

struct Machine {
    uint8_t *base = nullptr;                 /* guest address 0 */
    std::string sysroot;
    gp_image    image{};

    Dynarmic::ExclusiveMonitor *monitor = nullptr;

    /* Guards the descriptor table and the allocators below. One lock, because
     * these are cold: the census counted 295 mmaps and 4486 opens across a
     * whole run, against 381,096 gettimeofdays that touch none of this. */
    std::mutex lock;

    /* Where the next anonymous mapping goes. A bump allocator is honest about
     * what it is; nothing here is worth more until munmap needs to reuse. */
    /* The guest's working directory, as a GUEST path. Native titles chdir into
     * their package and then open relative paths — Data/BookConfig.xml and the
     * like — so without this they all resolve against / and the title renders
     * nothing while reporting only that a config failed to load. */
    std::string cwd = "/";

    uint32_t mmap_next = 0;
    uint32_t brk_cur   = 0;
    std::vector<GuestFd> fds;

    /* Named queues outlive the descriptors onto them, exactly as mq_open and
     * mq_unlink require. Guarded by `lock`. */
    std::map<std::string, std::shared_ptr<MsgQueue>> mqs;

    /* Bound socket names. Weak, so a listener that is closed stops answering
     * without needing every name unbound by hand. Guarded by `lock`. */
    std::map<std::string, std::weak_ptr<UnixSocket>> bound;

    /* The LIVE threads, not every thread ever made. It used to be the latter,
     * with the processor id taken from threads.size() — so a title that cycles
     * worker threads hit the ExclusiveMonitor's ceiling after thirty-two
     * CREATIONS, however few were running, and clone() started failing with
     * EAGAIN while the log blamed a limit that was not reached. */
    std::vector<std::unique_ptr<Thread>> threads;
    /* One byte per monitor slot, sized at startup. Guarded by `lock`. */
    std::vector<uint8_t> proc_used;
    std::atomic<uint32_t> next_tid{ 1000 };

    /* SHARED, because every clone() in the census carried CLONE_SIGHAND: what
     * a signal does is a property of the PROCESS, not of the thread that
     * happens to receive it. Guarded by `lock`, except on the fault path —
     * see take_action() in signal.cpp for why that one cannot take it. */
    std::array<SigAction, GP_NSIG> sigactions{};

    /* WHAT IS MAPPED WHERE, and it exists for exactly one reader.
     *
     * The shim's crash handler resolves a guest pc by reading
     * /proc/self/maps and reporting "libFoo.so+0x1234" — the form that makes
     * two crashes from the 79 titles sharing one engine comparable. Under
     * glasspole there was no such file, so HostPath fell through to the HOST's
     * /proc/self/maps and every guest address was matched against the
     * emulator's own mappings: every line of every backtrace came out as
     * "anon+<the address again>", which is a backtrace with the answer
     * removed. Guarded by `lock`. */
    struct GuestMap {
        uint32_t    base = 0, end = 0;
        int         prot = 0;
        std::string path;
    };
    std::vector<GuestMap> maps;
    void AddMap(uint32_t base, uint32_t len, int prot, const std::string &path);
    void WriteProcMaps();                    /* caller holds `lock` */

    /* exit_group, or a fatal fault: every thread stops. Atomic because it is
     * read by each thread's run loop without taking the lock. */
    std::atomic<bool> exiting{ false };

    /* How many times somebody outside has asked the guest to describe itself.
     * Bumped from a host signal handler, which is why it is an atomic counter
     * and not a list of things to do. */
    std::atomic<uint32_t> quit_gen{ 0 };
    int  status = 0;

    bool trace = false;

    uint8_t *Ptr(uint32_t v) { return base + v; }
    std::string Str(uint32_t v);             /* a C string out of guest memory */

    int  Commit(uint32_t addr, uint32_t len, int prot);
    int  AllocFd(gp_file *f, gp_dir *d, const std::string &path);
    void OpenStdio();
    GuestFd *Fd(int fd);                     /* caller holds `lock` */

    /* Guest path -> host path. The sysroot rewrite lives here because it is
     * Tadpole's knowledge, not the host's — host.h only ever sees a real
     * path on a real filesystem. */
    std::string HostPath(const std::string &guest);

    void ExitGroup(int st);                  /* stop every thread */
    /* A finished thread gives back its monitor slot and leaves the live set.
     * Called by the thread itself, as the last thing it does — nothing may
     * touch the Thread afterwards, because this destroys it. */
    void RetireThread(Thread *t);
};

/* The ARM kuser helper page at 0xffff0000. See kuser.cpp — it is not optional,
 * and its absence presents as a jump to a wild address. */
int gp_install_kuser_page(Machine &m);

/* CP15, so a thread can read its own thread pointer. See cp15.cpp. */
std::shared_ptr<Dynarmic::A32::Coprocessor> gp_make_cp15(Thread &t);

/* Spawn a guest thread from clone(). Defined in cpu.cpp, because it is the
 * file that owns dynarmic. Returns the new tid, or a negative errno. */
int gp_spawn_thread(Thread &parent, uint32_t flags, uint32_t child_stack,
                    uint32_t parent_tidptr, uint32_t tls, uint32_t child_tidptr);

/* Called from dynarmic's CallSVC. */
void gp_syscall(Thread &t);

/* ---- signals, all of it in signal.cpp ----------------------------------- */

/* The kernel's sigreturn trampolines, which live in the vectors page beside
 * the kuser helpers. Called by gp_install_kuser_page while the page is still
 * writable. */
void gp_signal_write_trampolines(Machine &m);

int32_t gp_sigaction (Thread &t, int sig, uint32_t act, uint32_t oldact, uint32_t setsize);
int32_t gp_sigprocmask(Thread &t, int how, uint32_t set, uint32_t oldset, uint32_t setsize);

/* kill(37), tkill(238) and tgkill(268). Marks the signal pending on the target
 * thread; gp_signal_deliver is what runs it. */
int32_t gp_signal_send(Thread &t, uint32_t nr, uint32_t a0, uint32_t a1, uint32_t a2);

/* Run the highest-priority deliverable pending signal on THIS thread, if any.
 * Returns true if it did anything. Must be called with Machine::lock NOT held.
 *
 * It is called from two places and needs both: gp_syscall, once the return
 * value is in r0, and the run loop, for a signal another thread aimed here. */
bool gp_signal_deliver(Thread &t);

/* A guest SIGSEGV from a host fault. Returns false if the guest has no handler
 * installed, in which case the caller reports the fault and dies as before. */
bool gp_signal_fault(Thread &t, uint32_t fault_addr);

void gp_sigreturn(Thread &t, bool rt);

#endif /* GLASSPOLE_MACHINE_H */
