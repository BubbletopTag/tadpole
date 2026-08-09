/* Glasspole — the dynarmic glue, and the driver.
 *
 * This is the half of qemu-arm that turns ARM instructions into something the
 * host CPU runs. dynarmic does the work; what lives here is the memory view it
 * reads through and the SVC hook that hands control to syscall.cpp.
 *
 * MEMORY. The guest is 32-bit, so its whole address space is 4 GiB and we
 * reserve all of it up front as one range. A guest address is then simply
 * `base + vaddr` with no lookup, which is also the shape dynarmic's fastmem
 * wants when we switch it on. Reserving is cheap — it is address space, not
 * memory, and nothing is committed until something asks.
 *
 * There is no Linux header included here and there must not be one, or the
 * Win32 backend stops being a swap of a single file.
 */
#include "machine.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/exclusive_monitor.h>

namespace {

constexpr uint64_t GUEST_SPACE = 0x100000000ull;  /* the whole 32-bit range */
constexpr uint32_t STACK_TOP   = 0x40000000;
constexpr uint32_t STACK_SIZE  = 0x00400000;      /* 4 MiB to start with */
constexpr uint32_t MMAP_BASE   = 0x50000000;
constexpr uint32_t INTERP_BASE = 0x60000000;

class Cpu final : public Dynarmic::A32::UserCallbacks {
public:
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
     * locking, it makes every lock unacquirable. uClibc's stdio spins forever
     * in fflush_unlocked's `strex; teq; bne` retry loop and never issues
     * another syscall, which is a hang with no evidence attached to it.
     *
     * A real compare-and-swap on the host is both correct and what makes this
     * work across threads later: the ExclusiveMonitor is shared between
     * processor_ids, so two guest threads contending on the same word resolve
     * against real host atomics. (This is the reason for choosing dynarmic over
     * Unicorn, where per-instance monitors would silently break guest locks.) */
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
        gp_log("dynarmic could not translate %zu instruction(s) at %08x\n", n, pc);
        m->Halt(70);
    }

    void ExceptionRaised(uint32_t pc, Dynarmic::A32::Exception e) override {
        using E = Dynarmic::A32::Exception;
        /* Hints are not faults. The guest is allowed to say "I am idle". */
        if (e == E::Yield || e == E::SendEvent || e == E::SendEventLocal ||
            e == E::PreloadData || e == E::PreloadDataWithIntentToWrite ||
            e == E::PreloadInstruction)
            return;
        if (e == E::WaitForInterrupt || e == E::WaitForEvent) { gp_thread_yield(); return; }
        gp_log("exception %d at pc=%08x\n", static_cast<int>(e), pc);
        m->Halt(71);
    }

    void CallSVC(uint32_t) override { m->syscalls++; gp_syscall(*m); }

    void AddTicks(uint64_t t) override {
        m->ticks_left = t > m->ticks_left ? 0 : m->ticks_left - t;
    }
    uint64_t GetTicksRemaining() override { return m->ticks_left; }
};

int commit_thunk(void *ctx, uint32_t addr, uint32_t len, int prot) {
    return static_cast<Machine *>(ctx)->Commit(addr, len, prot);
}

}  // namespace

/* ---- Machine ------------------------------------------------------------ */

int Machine::Commit(uint32_t addr, uint32_t len, int prot) {
    return gp_commit(base + addr, len, prot);
}

std::string Machine::Str(uint32_t v) {
    const char *p = reinterpret_cast<const char *>(Ptr(v));
    return std::string(p);
}

int Machine::AllocFd(gp_file *f, gp_dir *d, const std::string &path) {
    /* Fields are assigned by NAME, not positionally. A braced initialiser here
     * silently mis-assigns the moment a member is added in the middle — which
     * it was, and every descriptor came back invalid. */
    GuestFd e;
    e.file = f;
    e.dir  = d;
    e.path = path;
    e.used = true;

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
        fds[i].used   = true;
        fds[i].file   = nullptr;      /* the console is not a gp_file */
        fds[i].path   = i == 0 ? "/dev/stdin" : (i == 1 ? "/dev/stdout" : "/dev/stderr");
        fds[i].oflags = i == 0 ? 0u : 1u;
    }
}

GuestFd *Machine::Fd(int fd) {
    if (fd < 0 || static_cast<size_t>(fd) >= fds.size() || !fds[fd].used) return nullptr;
    return &fds[fd];
}

std::string Machine::HostPath(const std::string &guest) {
    if (guest.empty() || guest[0] != '/') return sysroot + "/" + guest;
    return sysroot + guest;
}

void Machine::Halt(int st) {
    exited = true;
    status = st;
    jit->HaltExecution();
}

/* ---- driver ------------------------------------------------------------- */

int main(int argc, char **argv) {
    const char *sysroot = nullptr;
    int i = 1;
    bool trace = false;

    for (; i < argc; i++) {
        if (std::strcmp(argv[i], "--sysroot") == 0 && i + 1 < argc) { sysroot = argv[++i]; continue; }
        if (std::strcmp(argv[i], "--trace") == 0) { trace = true; continue; }
        break;
    }
    if (i >= argc || !sysroot) {
        std::fprintf(stderr,
            "usage: glasspole --sysroot <dir> [--trace] <guest-program> [args...]\n"
            "\n"
            "  <guest-program> is a path INSIDE the sysroot, e.g. /bin/busybox\n");
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

    gp_guest g{ m.base, commit_thunk, &m };
    std::string prog = m.HostPath(argv[i]);
    int r = gp_elf_load_program(&g, prog.c_str(), sysroot, INTERP_BASE, &m.image);
    if (r < 0) { gp_log("could not load %s (%d)\n", prog.c_str(), r); return 1; }
    m.brk_cur = m.image.brk;

    /* argv as the guest sees it: the path inside the sysroot, not the host's. */
    std::vector<const char *> gargv;
    for (int a = i; a < argc; a++) gargv.push_back(argv[a]);
    const char *genv[] = { "LD_LIBRARY_PATH=/lib:/usr/lib", "HOME=/", "PATH=/bin:/usr/bin", nullptr };

    uint32_t sp = gp_elf_build_stack(&g, STACK_TOP - 4096,
                                     static_cast<int>(gargv.size()), gargv.data(),
                                     genv, &m.image);
    if (!sp) { gp_log("could not build the initial stack\n"); return 1; }

    Cpu cpu;
    cpu.m = &m;

    Dynarmic::ExclusiveMonitor monitor{ 1 };

    Dynarmic::A32::UserConfig conf;
    conf.callbacks = &cpu;
    /* The guest is ARMv7-A with Thumb-2 and VFPv2 — readelf -A on AppManager
     * says so. Leaving this at dynarmic's v8 default would accept encodings the
     * real hardware rejected, which surfaces months later as one title behaving
     * oddly for no visible reason. */
    conf.arch_version   = Dynarmic::A32::ArchVersion::v7;
    conf.global_monitor = &monitor;
    conf.processor_id   = 0;
    /* Without CP15 every TLS read compiles to a coprocessor exception, and the
     * dynamic linker dies partway through loading libc. */
    conf.coprocessors[15] = gp_make_cp15(m);

    Dynarmic::A32::Jit jit{ conf };
    m.jit = &jit;

    jit.Regs()[13] = sp;
    jit.Regs()[15] = m.image.entry;
    jit.SetCpsr(0x10);   /* user mode, ARM state */

    gp_log("entering guest at %08x with sp=%08x\n", m.image.entry, sp);

    constexpr uint64_t BUDGET = 200 * 1000 * 1000;   /* instructions per slice */
    uint64_t last_syscalls = 0;
    int      quiet_slices  = 0;

    while (!m.exited) {
        m.ticks_left = BUDGET;
        jit.Run();
        if (m.exited) break;

        if (m.syscalls == last_syscalls) {
            if (++quiet_slices >= 3) {
                const auto &r = jit.Regs();
                gp_log("WATCHDOG: %llu instructions, no syscall. The guest is spinning.\n",
                       (unsigned long long)(BUDGET * quiet_slices));
                gp_log("  pc=%08x  sp=%08x  lr=%08x  cpsr=%08x\n",
                       r[15], r[13], r[14], jit.Cpsr());
                for (int q = 0; q < 13; q += 4)
                    gp_log("  r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x\n",
                           q, r[q], q + 1, r[q + 1], q + 2, r[q + 2], q + 3, r[q + 3]);
                m.Halt(72);
                break;
            }
        } else {
            last_syscalls = m.syscalls;
            quiet_slices  = 0;
        }
    }

    gp_log("guest exited with status %d\n", m.status);
    return m.status;
}
