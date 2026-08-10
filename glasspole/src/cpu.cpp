/* Glasspole — the dynarmic glue, and the driver.
 *
 * This is the half of qemu-arm that turns ARM instructions into something the
 * host CPU runs. dynarmic does the work; what lives here is the memory view it
 * reads through, the SVC hook that hands control to syscall.cpp, and thread
 * creation.
 *
 * MEMORY. The guest is 32-bit, so its whole address space is 4 GiB and we
 * reserve all of it up front as one range. A guest address is then simply
 * `base + vaddr` with no lookup, which is also the shape dynarmic's fastmem
 * wants when we switch it on. Reserving is cheap — it is address space, not
 * memory, and nothing is committed until something asks.
 *
 * THREADS. One dynarmic Jit per guest thread, one host thread each, all of them
 * pointed at the same guest memory and sharing ONE ExclusiveMonitor. That last
 * part is what makes ldrex/strex work between them, and it is the reason this
 * project uses dynarmic rather than Unicorn: per-instance monitors would let
 * two guest threads each believe they had taken the same lock.
 *
 * There is no Linux header included here and there must not be one, or the
 * Win32 backend stops being a swap of a single file.
 */
#include "machine.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

extern char **environ;
#include <cstring>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/exclusive_monitor.h>

namespace {

constexpr uint64_t GUEST_SPACE = 0x100000000ull;  /* the whole 32-bit range */
constexpr uint32_t STACK_TOP   = 0x40000000;
/* 64 MiB, matching `qemu-arm -s 67108864` in tadpole.sh, and for the reason
 * recorded there: the default is not enough. Brio and Flash Lite recurse deeply
 * through their scene graphs, and both AppManager and saplayer faulted on
 * `str r1, [sp]` at exactly 8 MB below the stack base. */
constexpr uint32_t STACK_SIZE  = 0x04000000;
constexpr uint32_t MMAP_BASE   = 0x50000000;
constexpr uint32_t INTERP_BASE = 0x60000000;

/* The ExclusiveMonitor is sized once, so this is a hard ceiling on guest
 * threads. The census saw five; thirty-two is room to be wrong by a lot. */
constexpr size_t MAX_THREADS = 32;

constexpr uint64_t BUDGET = 200 * 1000 * 1000;    /* instructions per slice */

class Cpu final : public Dynarmic::A32::UserCallbacks {
public:
    Thread  *t = nullptr;
    Machine *m = nullptr;

    uint8_t  MemoryRead8 (uint32_t a) override { return *m->Ptr(a); }
    uint16_t MemoryRead16(uint32_t a) override { uint16_t v; std::memcpy(&v, m->Ptr(a), 2); return v; }
    uint32_t MemoryRead32(uint32_t a) override { uint32_t v; std::memcpy(&v, m->Ptr(a), 4); return v; }
    uint64_t MemoryRead64(uint32_t a) override { uint64_t v; std::memcpy(&v, m->Ptr(a), 8); return v; }

    void MemoryWrite8 (uint32_t a, uint8_t  v) override { *m->Ptr(a) = v; }
    void MemoryWrite16(uint32_t a, uint16_t v) override { std::memcpy(m->Ptr(a), &v, 2); }
    void MemoryWrite32(uint32_t a, uint32_t v) override { std::memcpy(m->Ptr(a), &v, 4); }
    void MemoryWrite64(uint32_t a, uint64_t v) override { std::memcpy(m->Ptr(a), &v, 8); }

    /* ldrex/strex. dynarmic's defaults for these return FALSE — meaning "the
     * exclusive store failed" — so leaving them unimplemented does not disable
     * locking, it makes every lock unacquirable. uClibc spins forever in
     * fflush_unlocked's `strex; teq; bne` retry loop and never issues another
     * syscall: a hang with no evidence attached to it.
     *
     * A real host compare-and-swap is both correct here and what makes this
     * work across threads, since the monitor is shared between processor ids. */
    template <typename T>
    bool WriteExclusive(uint32_t addr, T value, T expected) {
        std::atomic_ref<T> ref(*reinterpret_cast<T *>(m->Ptr(addr)));
        T exp = expected;
        return ref.compare_exchange_strong(exp, value, std::memory_order_seq_cst);
    }

    bool MemoryWriteExclusive8 (uint32_t a, uint8_t  v, uint8_t  e) override { return WriteExclusive(a, v, e); }
    bool MemoryWriteExclusive16(uint32_t a, uint16_t v, uint16_t e) override { return WriteExclusive(a, v, e); }
    bool MemoryWriteExclusive32(uint32_t a, uint32_t v, uint32_t e) override { return WriteExclusive(a, v, e); }
    bool MemoryWriteExclusive64(uint32_t a, uint64_t v, uint64_t e) override { return WriteExclusive(a, v, e); }

    void InterpreterFallback(uint32_t pc, size_t n) override {
        gp_log("tid %u: dynarmic could not translate %zu instruction(s) at %08x\n",
               t->tid, n, pc);
        m->ExitGroup(70);
    }

    void ExceptionRaised(uint32_t pc, Dynarmic::A32::Exception e) override {
        using E = Dynarmic::A32::Exception;
        /* Hints are not faults. The guest is allowed to say "I am idle". */
        if (e == E::Yield || e == E::SendEvent || e == E::SendEventLocal ||
            e == E::PreloadData || e == E::PreloadDataWithIntentToWrite ||
            e == E::PreloadInstruction)
            return;
        if (e == E::WaitForInterrupt || e == E::WaitForEvent) { gp_thread_yield(); return; }
        gp_log("tid %u: exception %d at pc=%08x\n", t->tid, static_cast<int>(e), pc);
        m->ExitGroup(71);
    }

    void CallSVC(uint32_t) override { t->syscalls++; gp_syscall(*t); }

    void AddTicks(uint64_t n) override {
        t->ticks_left = n > t->ticks_left ? 0 : t->ticks_left - n;
    }
    uint64_t GetTicksRemaining() override { return t->ticks_left; }
};

/* Which guest thread this host thread is running, for the fault handler. A
 * signal arrives on the faulting thread with no argument, so this is the only
 * way to say WHICH guest thread ran off the end of something. */
thread_local Thread *g_current = nullptr;
Machine *g_machine = nullptr;

/* Called from a signal handler. Almost nothing is safe here, so this prints
 * through gp_log and stops — no attempt to recover, no attempt to unwind.
 *
 * The pc reported is the start of the JIT block being executed, not the exact
 * instruction: dynarmic only writes the guest pc back at block boundaries. It
 * still names the function, which is the question being asked. */
void on_guest_fault(void *addr) {
    if (!g_machine || !g_machine->base) { gp_log("FAULT before the guest existed\n"); return; }

    const uint8_t *p    = static_cast<const uint8_t *>(addr);
    const uint8_t *base = g_machine->base;

    if (p < base || p >= base + GUEST_SPACE) {
        gp_log("HOST FAULT at %p — outside the guest's address space entirely, "
               "so this is an emulator bug rather than a guest one\n", addr);
        return;
    }

    const uint32_t guest = static_cast<uint32_t>(p - base);
    gp_log("GUEST FAULT: %08x was never mapped\n", guest);
    if (g_current) {
        gp_log("  thread %u, %llu syscalls in\n", g_current->tid,
               (unsigned long long)g_current->syscalls);
        if (g_current->jit)
            gp_log("  block pc ~%08x  sp %08x  lr %08x\n",
                   g_current->jit->Regs()[15], g_current->jit->Regs()[13],
                   g_current->jit->Regs()[14]);
    }
}

int commit_thunk(void *ctx, uint32_t addr, uint32_t len, int prot) {
    return static_cast<Machine *>(ctx)->Commit(addr, len, prot);
}

/* Everything one host thread needs to bring a guest thread to life. The
 * register state is captured in the PARENT before the host thread starts, so
 * the child never reads a Jit the parent is still running. */
struct Spawn {
    Thread  *t = nullptr;
    std::array<uint32_t, 16> regs{};
    std::array<uint32_t, 64> ext{};
    uint32_t cpsr  = 0x10;
    uint32_t fpscr = 0;
    size_t   processor_id = 0;
    gp_thread *host = nullptr;
};

void RunLoop(Thread &t, Dynarmic::A32::Jit &jit) {
    uint64_t last_syscalls = 0;
    int      quiet_slices  = 0;

    while (!t.exited && !t.m->exiting.load(std::memory_order_relaxed)) {
        t.ticks_left = BUDGET;
        jit.Run();
        if (t.exited || t.m->exiting.load(std::memory_order_relaxed)) break;

        if (t.syscalls == last_syscalls) {
            if (++quiet_slices >= 3) {
                const auto &r = jit.Regs();
                gp_log("WATCHDOG tid %u: %llu instructions, no syscall. Spinning.\n",
                       t.tid, (unsigned long long)(BUDGET * quiet_slices));
                gp_log("  pc=%08x  sp=%08x  lr=%08x  cpsr=%08x\n",
                       r[15], r[13], r[14], jit.Cpsr());
                for (int q = 0; q < 13; q += 4)
                    gp_log("  r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x\n",
                           q, r[q], q + 1, r[q + 1], q + 2, r[q + 2], q + 3, r[q + 3]);
                t.m->ExitGroup(72);
                break;
            }
        } else {
            last_syscalls = t.syscalls;
            quiet_slices  = 0;
        }
    }
}

/* Build a Jit for a thread and run it. Shared by the initial thread and every
 * cloned one, so the two cannot drift apart in configuration. */
void RunThread(Thread &t, const std::array<uint32_t, 16> &regs,
               const std::array<uint32_t, 64> &ext, uint32_t cpsr, uint32_t fpscr,
               size_t processor_id) {
    Cpu cpu;
    cpu.t = &t;
    cpu.m = t.m;
    g_current = &t;

    Dynarmic::A32::UserConfig conf;
    conf.callbacks = &cpu;
    /* The guest is ARMv7-A with Thumb-2 and VFPv2 — readelf -A on AppManager
     * says so. Leaving this at dynarmic's v8 default would accept encodings the
     * real hardware rejected, which surfaces months later as one title behaving
     * oddly for no visible reason. */
    conf.arch_version     = Dynarmic::A32::ArchVersion::v7;
    /* GLASSPOLE_NO_OPT=1 turns every dynarmic optimisation off. A bug that
     * disappears here is a JIT miscompilation rather than anything this
     * emulator did, which is otherwise very hard to tell apart. */
    if (const char *e = std::getenv("GLASSPOLE_NO_OPT"); e && *e == '1')
        conf.optimizations = Dynarmic::no_optimizations;
    conf.global_monitor   = t.m->monitor;
    conf.processor_id     = processor_id;
    /* Per thread, and pointed at THIS thread's tls fields. */
    conf.coprocessors[15] = gp_make_cp15(t);

    Dynarmic::A32::Jit jit{ conf };

    jit.Regs()    = regs;
    jit.ExtRegs() = ext;
    jit.SetCpsr(cpsr);
    jit.SetFpscr(fpscr);

    /* PUBLISHED AND WITHDRAWN UNDER THE LOCK. `jit` lives on this stack frame
     * and ExitGroup halts threads by reaching through this pointer from
     * another one; without the lock on both sides, a thread finishing while
     * exit_group runs is a dereference of a frame that has already gone. */
    { std::lock_guard<std::mutex> g(t.m->lock); t.jit = &jit; }
    RunLoop(t, jit);
    { std::lock_guard<std::mutex> g(t.m->lock); t.jit = nullptr; }
}

void thread_entry(void *arg) {
    Spawn *s = static_cast<Spawn *>(arg);
    Thread  *t = s->t;
    Machine &m = *t->m;
    RunThread(*t, s->regs, s->ext, s->cpsr, s->fpscr, s->processor_id);
    t->exited = true;
    /* Nothing may name this thread after it retires, the fault handler's
     * thread-local pointer included. */
    g_current = nullptr;
    m.RetireThread(t);
    /* `s` and its gp_thread are NOT freed, and that is a known leak of a few
     * dozen bytes per guest thread. Fixing it needs an owner for the
     * gp_thread: gp_thread_create writes the handle into the caller's
     * variable AFTER the thread may already have run, so the child cannot
     * free the block the parent is still about to write to, and the parent
     * has nowhere to do it either without a join it must not perform. That is
     * a host.h conversation, recorded rather than half-done here. */
}

}  // namespace

/* ---- Thread / Machine --------------------------------------------------- */

void Thread::Halt() {
    exited = true;
    if (jit) jit->HaltExecution();
}

int Machine::Commit(uint32_t addr, uint32_t len, int prot) {
    return gp_commit(base + addr, len, prot);
}

std::string Machine::Str(uint32_t v) {
    return std::string(reinterpret_cast<const char *>(Ptr(v)));
}

int Machine::AllocFd(gp_file *f, gp_dir *d, const std::string &path) {
    /* Fields are assigned by NAME, not positionally. A braced initialiser here
     * silently mis-assigns the moment a member is added in the middle — which
     * it was, and every descriptor came back invalid. */
    GuestFd e;
    e.file  = f;
    e.dir   = d;
    e.path  = path;
    e.share = std::make_shared<int>(0);   /* see GuestFd::share */
    e.used  = true;

    /* Lowest free slot, exactly as Linux promises — programs do rely on it. */
    for (size_t i = 0; i < fds.size(); i++) {
        if (!fds[i].used) { fds[i] = e; return static_cast<int>(i); }
    }
    fds.push_back(e);
    return static_cast<int>(fds.size() - 1);
}

/* stdin, stdout and stderr must EXIST, and must be 0, 1 and 2. Without them
 * the first open() is handed fd 0, the guest writes its output into what it
 * believes is stdin, and nothing about the failure points at the cause. */
void Machine::OpenStdio() {
    fds.resize(3);
    for (int i = 0; i < 3; i++) {
        fds[i].used    = true;
        fds[i].file    = nullptr;     /* the console is not a gp_file */
        fds[i].console = i;           /* ...but it is still a stream, see GuestFd */
        fds[i].share   = std::make_shared<int>(0);
        fds[i].path    = i == 0 ? "/dev/stdin" : (i == 1 ? "/dev/stdout" : "/dev/stderr");
        fds[i].oflags  = i == 0 ? 0u : 1u;
    }
}

GuestFd *Machine::Fd(int fd) {
    if (fd < 0 || static_cast<size_t>(fd) >= fds.size() || !fds[fd].used) return nullptr;
    return &fds[fd];
}

std::string Machine::HostPath(const std::string &guest) {
    /* AN EMPTY PATH IS NOT THE SYSROOT. It used to return it, which made
     * stat("") succeed — because the sysroot is a directory and it certainly
     * exists — where every real system answers ENOENT.
     *
     * That one line cost most of a day. BaseUtils::FileExists("") came back
     * true, so CAppManager::LoadNewApp believed a Leapster view frame was
     * present, wrapped the home screen in it, and asserted in LightningJSON
     * shortly after. Identical syscalls to qemu, identical files, and a
     * different answer to a question about a path that was not there. */
    if (guest.empty()) return std::string();
    /* Relative paths resolve against the guest's cwd, not against the sysroot
     * root. Getting this wrong is invisible until a title chdirs. */
    if (guest[0] != '/') {
        std::string base = cwd;
        if (base.empty() || base.back() != '/') base += '/';
        return sysroot + base + guest;
    }

    /* qemu-user's -L semantics, and they matter more than they look. The
     * sysroot wins when it has the file, and otherwise the path is used as it
     * stands — which is what lets an absolute HOST path through.
     *
     * Tadpole depends on exactly this. LD_LIBRARY_PATH points at
     * runtime/shimlibs on the host, and TADPOLE_DIR is /tmp/tadpole; neither
     * exists inside the rootfs, so both have to fall through. Prepending the
     * sysroot unconditionally would hide the shim from the guest's own linker
     * and the failure would look like the shim simply not working. */
    std::string in_root = sysroot + guest;
    struct gp_statbuf st;
    if (gp_stat(in_root.c_str(), &st) == 0) return in_root;
    return guest;
}

void Machine::RetireThread(Thread *t) {
    std::lock_guard<std::mutex> g(lock);
    if (t->processor_id < proc_used.size()) proc_used[t->processor_id] = 0;
    for (auto it = threads.begin(); it != threads.end(); ++it)
        if (it->get() == t) { threads.erase(it); return; }
}

void Machine::ExitGroup(int st) {
    status = st;
    exiting.store(true, std::memory_order_relaxed);
    /* Halt every thread's Jit. HaltExecution is meant to be called from another
     * thread — it is how dynarmic expects to be interrupted. */
    std::lock_guard<std::mutex> g(lock);
    for (auto &th : threads) th->Halt();
}

/* ---- clone -------------------------------------------------------------- */

int gp_spawn_thread(Thread &parent, uint32_t flags, uint32_t child_stack,
                    uint32_t parent_tidptr, uint32_t tls, uint32_t child_tidptr) {
    Machine &m = *parent.m;

    /* Named GP_ because <sched.h> already defines CLONE_* as macros and gets
     * pulled in transitively by the C++ standard library. A macro wins against
     * a constexpr every time, and the error it produces names neither. */
    constexpr uint32_t GP_CLONE_THREAD         = 0x00010000;
    constexpr uint32_t GP_CLONE_SETTLS         = 0x00080000;
    constexpr uint32_t GP_CLONE_PARENT_SETTID  = 0x00100000;
    constexpr uint32_t GP_CLONE_CHILD_CLEARTID = 0x00200000;
    constexpr uint32_t GP_CLONE_CHILD_SETTID   = 0x01000000;

    /* CLONE_THREAD is what makes this a thread rather than a process. Every
     * clone in the census had it, and a process would need a second address
     * space that this design deliberately does not have. */
    if (!(flags & GP_CLONE_THREAD)) {
        gp_log("clone without CLONE_THREAD (flags %08x): process creation is not "
               "implemented, and nothing in the measured workload asks for it\n", flags);
        return GP_ENOSYS;
    }

    auto t = std::make_unique<Thread>();
    t->m   = &m;
    t->tid = m.next_tid.fetch_add(1, std::memory_order_relaxed);
    if (flags & GP_CLONE_SETTLS)         t->tls             = tls;
    if (flags & GP_CLONE_CHILD_CLEARTID) t->clear_child_tid = child_tidptr;

    auto s = std::make_unique<Spawn>();
    /* The child resumes exactly where the parent's svc returns, with its own
     * stack and 0 in r0 — that is what clone() promises its caller. */
    s->regs     = parent.jit->Regs();
    s->ext      = parent.jit->ExtRegs();
    s->cpsr     = parent.jit->Cpsr();
    s->fpscr    = parent.jit->Fpscr();
    s->regs[0]  = 0;
    s->regs[13] = child_stack;
    s->t        = t.get();

    /* SETTID writes the tid now; CLEARTID only records where to zero it on
     * exit. They are different flags and doing the first for the second is a
     * mistake that hides, because NPTL usually passes the same address to both. */
    if (flags & GP_CLONE_PARENT_SETTID) std::memcpy(m.Ptr(parent_tidptr), &t->tid, 4);
    if (flags & GP_CLONE_CHILD_SETTID)  std::memcpy(m.Ptr(child_tidptr),  &t->tid, 4);

    const uint32_t tid = t->tid;
    Thread *tp = t.get();
    {
        std::lock_guard<std::mutex> g(m.lock);
        /* The LOWEST FREE monitor slot, not the thread count. A retired thread
         * hands its slot back, so this is a limit on threads ALIVE AT ONCE —
         * which is what the ExclusiveMonitor's size actually constrains. */
        size_t slot = m.proc_used.size();
        for (size_t i = 0; i < m.proc_used.size(); i++)
            if (!m.proc_used[i]) { slot = i; break; }
        if (slot >= m.proc_used.size()) {
            gp_log("clone: %zu threads are already running, which is the "
                   "ExclusiveMonitor's size — raise MAX_THREADS\n",
                   m.proc_used.size());
            return GP_EAGAIN;
        }
        m.proc_used[slot] = 1;
        tp->processor_id  = slot;
        s->processor_id   = slot;
        m.threads.push_back(std::move(t));
    }

    Spawn *raw = s.release();     /* outlives this call; the thread owns it */
    int r = gp_thread_create(thread_entry, raw, &raw->host);
    if (r < 0) {
        /* The thread never started, so nothing else can be holding either the
         * slot or the table entry — and leaving them taken would make a
         * transient failure permanent. */
        gp_log("clone: host thread creation failed (%d)\n", r);
        m.RetireThread(tp);
        delete raw;
        return r;
    }

    return static_cast<int>(tid);
}

/* ---- driver ------------------------------------------------------------- */

int main(int argc, char **argv) {
    const char *sysroot = nullptr;
    int i = 1;
    bool trace = false;
    std::vector<std::string> envs;

    for (; i < argc; i++) {
        if (std::strcmp(argv[i], "--sysroot") == 0 && i + 1 < argc) { sysroot = argv[++i]; continue; }
        if (std::strcmp(argv[i], "--trace") == 0) { trace = true; continue; }
        /* qemu-arm's own spelling, so this is a DROP-IN REPLACEMENT for it.
         * tadpole.sh already picks its emulator through TADPOLE_QEMU, which
         * means accepting these three flags hands glasspole the entire front
         * end — the viewer, input, audio, the app launcher — for free. */
        if (std::strcmp(argv[i], "-L") == 0 && i + 1 < argc) { sysroot = argv[++i]; continue; }
        if (std::strcmp(argv[i], "-strace") == 0) { trace = true; continue; }
        /* -s is qemu's guest stack size. Ours is fixed at 64 MiB, which is what
         * tadpole.sh asks for anyway, so accept the flag and its argument
         * rather than choking on them. */
        if (std::strcmp(argv[i], "-s") == 0 && i + 1 < argc) { ++i; continue; }
        /* -E, spelled as qemu-arm spells it, because every script that drives
         * this already knows that flag. */
        if (std::strcmp(argv[i], "-E") == 0 && i + 1 < argc) { envs.push_back(argv[++i]); continue; }
        break;
    }
    if (i >= argc || !sysroot) {
        std::fprintf(stderr,
            "usage: glasspole --sysroot <dir> [--trace] <guest-program> [args...]\n"
            "\n"
            "  -E KEY=VALUE    add to the guest's environment (repeatable)\n"
            "\n"
            "  <guest-program> absolute resolves in the sysroot first and then on the\n"
            "                  host, as qemu-arm -L does; relative is a host path\n");
        return 2;
    }

    Machine m;
    m.sysroot = sysroot;
    m.trace   = trace;

    m.base = static_cast<uint8_t *>(gp_reserve(nullptr, GUEST_SPACE));
    if (!m.base) { gp_log("could not reserve the guest's 4 GiB\n"); return 1; }

    if (m.Commit(STACK_TOP - STACK_SIZE, STACK_SIZE, GP_PROT_READ | GP_PROT_WRITE) < 0) {
        gp_log("could not commit the guest stack\n");
        return 1;
    }
    m.mmap_next = MMAP_BASE;
    m.OpenStdio();

    if (gp_install_kuser_page(m) < 0) {
        gp_log("could not map the ARM kuser helper page at 0xffff0000\n");
        return 1;
    }

    g_machine = &m;
    if (gp_install_fault_handler(on_guest_fault) < 0)
        gp_log("warning: no fault handler — a stray guest access will look like "
               "the emulator crashing\n");

    gp_guest g{ m.base, commit_thunk, &m };
    /* An ABSOLUTE path is a GUEST path and goes through the sysroot, which is
     * what /bin/busybox has to mean. A relative one is a host path, so the test
     * programs can be run straight out of the build directory without being
     * copied into a rootfs. */
    std::string prog = argv[i][0] == '/' ? m.HostPath(argv[i]) : argv[i];
    int r = gp_elf_load_program(&g, prog.c_str(), sysroot, INTERP_BASE, &m.image);
    if (r < 0) { gp_log("could not load %s (%d)\n", prog.c_str(), r); return 1; }
    m.brk_cur = m.image.brk;

    /* argv as the guest sees it: the path inside the sysroot, not the host's. */
    std::vector<const char *> gargv;
    for (int a = i; a < argc; a++) gargv.push_back(argv[a]);
    /* THE HOST'S ENVIRONMENT IS PASSED THROUGH, as qemu-arm does. This is not
     * a nicety: qemu hands the guest every variable the host has — around
     * seventy of them — and glasspole was handing it three. LANG among them,
     * and locale is already known to be delicate here; the qemu compatibility
     * sweep has nineteen titles aborting in _S_create_c_locale.
     *
     * A guest that sees a different environment can take a different branch
     * while making identical syscalls, which is the hardest kind of difference
     * to find and exactly what was being hunted.
     *
     * -E REPLACES a default with the same key rather than being appended after
     * it. Appending looks like it should work and does not: uClibc's getenv
     * returns the FIRST match, so a default LD_LIBRARY_PATH left in front of
     * the real one wins, the guest searches only /lib and /usr/lib, and the
     * failure reads as "the library is missing" rather than "the variable
     * never arrived". */
    std::vector<std::string> envstore;
    for (char **e = environ; e && *e; e++) envstore.push_back(*e);
    /* AND NOTHING INVENTED ON TOP. This used to add LD_LIBRARY_PATH, HOME and
     * PATH when the host had none, which reads as helpful and is the same
     * mistake in miniature as the three-variable environment above: the kernel
     * adds nothing, qemu-arm adds nothing, and a variable that exists here and
     * nowhere else is a branch the guest can take under one emulator only.
     * Measured — it was the last remaining difference between the two:
     *
     *     $ diff <(qemu-arm … env -u _|sort) <(glasspole … env -u _|sort)
     *     > LD_LIBRARY_PATH=/lib:/usr/lib
     *
     * The default was never load-bearing: /lib and /usr/lib are already
     * ld-uClibc's own search path, and every real launch comes through
     * tadpole.sh, which sets LD_LIBRARY_PATH to the shim directories itself. */
    for (auto &e : envs) {
        const size_t eq = e.find('=');
        const std::string key = e.substr(0, eq == std::string::npos ? e.size() : eq);
        for (auto it = envstore.begin(); it != envstore.end(); ) {
            if (it->compare(0, key.size(), key) == 0 && it->size() > key.size() &&
                (*it)[key.size()] == '=')
                it = envstore.erase(it);
            else
                ++it;
        }
        envstore.push_back(e);
    }
    std::vector<const char *> genv;
    for (auto &e : envstore) genv.push_back(e.c_str());
    genv.push_back(nullptr);

    uint32_t sp = gp_elf_build_stack(&g, STACK_TOP - 4096,
                                     static_cast<int>(gargv.size()), gargv.data(),
                                     genv.data(), &m.image);
    if (!sp) { gp_log("could not build the initial stack\n"); return 1; }

    Dynarmic::ExclusiveMonitor monitor{ MAX_THREADS };
    m.monitor = &monitor;

    m.proc_used.assign(MAX_THREADS, 0);
    m.proc_used[0] = 1;                       /* the initial thread's slot */

    auto main_thread = std::make_unique<Thread>();
    main_thread->m   = &m;
    main_thread->tid = m.next_tid.fetch_add(1, std::memory_order_relaxed);
    Thread *mt = main_thread.get();
    m.threads.push_back(std::move(main_thread));

    std::array<uint32_t, 16> regs{};
    std::array<uint32_t, 64> ext{};
    regs[13] = sp;

    /* BIT 0 OF THE ENTRY ADDRESS IS THE INSTRUCTION SET, not part of the
     * address: an odd entry means Thumb, and the T bit has to be set in CPSR
     * with the bit cleared from the pc. This rootfs's ld-uClibc.so.0 enters in
     * ARM state, so nothing here exercises it today — which is exactly why it
     * is worth fixing now rather than when some title's interpreter does and
     * the symptom is a wild jump. */
    uint32_t entry = m.image.entry;
    uint32_t cpsr  = 0x10;                    /* user mode */
    if (entry & 1) { cpsr |= 0x20; entry &= ~1u; }

    gp_log("entering guest at %08x with sp=%08x\n", entry, sp);
    regs[15] = entry;
    RunThread(*mt, regs, ext, cpsr, 0, 0);

    /* The main thread returning is exit_group whether or not the guest said so:
     * nothing else can make progress once it is gone. */
    m.exiting.store(true, std::memory_order_relaxed);
    gp_log("guest exited with status %d\n", m.status);

    /* AND THE PROCESS ENDS HERE, without unwinding. Returning from main runs
     * static destructors and destroys `monitor` and `m` — both of which the
     * guest's other threads still hold pointers to, and some of them may be
     * inside jit.Run() at that moment.
     *
     * Joining them instead is the obvious alternative and it is worse: a guest
     * thread parked in an untimed FUTEX_WAIT is waiting for a wake that now
     * will never come, so the join would hang a process whose guest has
     * already exited. The threads poll `exiting` on every timed wait, so a
     * short grace period lets anything mid-syscall notice and finish its own
     * output, and then _Exit leaves without touching anything shared.
     *
     * Nothing is buffered on our side: gp_log flushes, and the guest's console
     * writes go straight to the host. */
    gp_sleep_ns(100000000ull);
    std::_Exit(m.status);
}
