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

/* The guest's descriptor table. Guest fd numbers are OURS — the lowest free
 * slot, exactly as Linux promises — and have no relationship to anything the
 * host numbered. That is what lets the Win32 backend hand back a HANDLE
 * without a single line above this changing. */
struct GuestFd {
    gp_file *file = nullptr;
    gp_dir  *dir  = nullptr;
    std::string path;          /* kept for diagnostics and for fstat on dirs */
    uint32_t oflags = 0;       /* what the guest opened it with, for F_GETFL */
    bool     used = false;
};

/* ---- one guest thread --------------------------------------------------- */

struct Thread {
    Machine *m   = nullptr;
    Dynarmic::A32::Jit *jit = nullptr;
    uint32_t tid = 0;

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
    uint32_t mmap_next = 0;
    uint32_t brk_cur   = 0;
    std::vector<GuestFd> fds;

    std::vector<std::unique_ptr<Thread>> threads;
    std::atomic<uint32_t> next_tid{ 1000 };

    /* exit_group, or a fatal fault: every thread stops. Atomic because it is
     * read by each thread's run loop without taking the lock. */
    std::atomic<bool> exiting{ false };
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
};

/* CP15, so a thread can read its own thread pointer. See cp15.cpp. */
std::shared_ptr<Dynarmic::A32::Coprocessor> gp_make_cp15(Thread &t);

/* Spawn a guest thread from clone(). Defined in cpu.cpp, because it is the
 * file that owns dynarmic. Returns the new tid, or a negative errno. */
int gp_spawn_thread(Thread &parent, uint32_t flags, uint32_t child_stack,
                    uint32_t parent_tidptr, uint32_t tls, uint32_t child_tidptr);

/* Called from dynarmic's CallSVC. */
void gp_syscall(Thread &t);

#endif /* GLASSPOLE_MACHINE_H */
