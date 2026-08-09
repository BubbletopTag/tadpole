/* Glasspole — the dynarmic glue, and milestone 0's driver.
 *
 * This is the half of qemu-arm that turns ARM instructions into something the
 * host CPU runs. dynarmic does the actual work; what lives here is the memory
 * view it reads through and the SVC hook it calls when the guest asks the
 * kernel for something.
 *
 * MEMORY. The guest is 32-bit, so its entire address space is 4 GiB and we
 * reserve all of it up front as one range. A guest address is then just
 * `guest_base + vaddr` with no lookup at all, which is also precisely the shape
 * dynarmic's fastmem wants when we turn it on later. Reserving is cheap: it is
 * address space, not memory, and nothing is committed until a guest mapping
 * asks for it.
 *
 * Everything below goes through host.h. There is no #include of a Linux header
 * in this file and there must not be one, or the Win32 backend stops being a
 * swap of a single file.
 */
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>

extern "C" {
#include "host.h"
}

namespace {

constexpr uint64_t GUEST_SPACE = 0x100000000ull;  /* the whole 32-bit range */
constexpr uint32_t LOAD_ADDR   = 0x00010000;
constexpr uint32_t STACK_TOP   = 0x40000000;
constexpr uint32_t STACK_SIZE  = 0x00100000;

/* ARM EABI syscall numbers. The full set the census produced lands in
 * syscall.c; milestone 0 needs exactly the two that let a program say
 * something and then stop. */
constexpr uint32_t NR_EXIT       = 1;
constexpr uint32_t NR_WRITE      = 4;
constexpr uint32_t NR_EXIT_GROUP = 248;

class Machine final : public Dynarmic::A32::UserCallbacks {
public:
    uint8_t *base = nullptr;
    Dynarmic::A32::Jit *jit = nullptr;
    bool  exited = false;
    int   status = 0;

    bool Map() {
        base = static_cast<uint8_t *>(gp_reserve(nullptr, GUEST_SPACE));
        if (!base) { gp_log("could not reserve the guest's 4 GiB\n"); return false; }
        return true;
    }

    /* Commit a guest range, rounded out to whole pages. Guest mappings proper
     * will be 64 KB aligned by the mmap layer; this is the raw primitive. */
    bool Commit(uint32_t addr, uint32_t len, int prot) {
        uint32_t lo = addr & ~(GP_PAGE - 1);
        uint32_t hi = (addr + len + GP_PAGE - 1) & ~(GP_PAGE - 1);
        int r = gp_commit(base + lo, hi - lo, prot);
        if (r < 0) { gp_log("commit %08x+%x failed: %d\n", lo, hi - lo, r); return false; }
        return true;
    }

    uint8_t *Ptr(uint32_t vaddr) { return base + vaddr; }

    /* ---- dynarmic's memory view ---------------------------------------- */
    /* Unaligned access is legal here; memcpy says so to the compiler without
     * inviting it to emit an aligned load. */
    uint8_t  MemoryRead8 (uint32_t a) override { return *Ptr(a); }
    uint16_t MemoryRead16(uint32_t a) override { uint16_t v; std::memcpy(&v, Ptr(a), 2); return v; }
    uint32_t MemoryRead32(uint32_t a) override { uint32_t v; std::memcpy(&v, Ptr(a), 4); return v; }
    uint64_t MemoryRead64(uint32_t a) override { uint64_t v; std::memcpy(&v, Ptr(a), 8); return v; }

    void MemoryWrite8 (uint32_t a, uint8_t  v) override { *Ptr(a) = v; }
    void MemoryWrite16(uint32_t a, uint16_t v) override { std::memcpy(Ptr(a), &v, 2); }
    void MemoryWrite32(uint32_t a, uint32_t v) override { std::memcpy(Ptr(a), &v, 4); }
    void MemoryWrite64(uint32_t a, uint64_t v) override { std::memcpy(Ptr(a), &v, 8); }

    void InterpreterFallback(uint32_t pc, size_t n) override {
        gp_log("interpreter fallback at %08x for %zu instructions — "
               "dynarmic could not translate this\n", pc, n);
        Halt(70);
    }

    void ExceptionRaised(uint32_t pc, Dynarmic::A32::Exception e) override {
        using E = Dynarmic::A32::Exception;
        /* The hint instructions are not faults; the guest is allowed to say
         * "I am idle" and we are allowed to ignore it. */
        if (e == E::Yield || e == E::SendEvent || e == E::SendEventLocal ||
            e == E::PreloadData || e == E::PreloadDataWithIntentToWrite ||
            e == E::PreloadInstruction)
            return;
        if (e == E::WaitForInterrupt || e == E::WaitForEvent) { gp_thread_yield(); return; }
        gp_log("exception %d at pc=%08x\n", static_cast<int>(e), pc);
        Halt(71);
    }

    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return static_cast<uint64_t>(-1); }

    /* ---- the kernel ----------------------------------------------------- */
    void CallSVC(uint32_t) override {
        /* EABI: the number is in r7, arguments in r0-r5, result back in r0.
         * The immediate in the SVC encoding is unused, which is why the
         * parameter is ignored. */
        auto &r = jit->Regs();
        const uint32_t nr = r[7];

        switch (nr) {
        case NR_WRITE: {
            const uint32_t fd = r[0], buf = r[1], len = r[2];
            if (fd != 1 && fd != 2) { r[0] = static_cast<uint32_t>(GP_EBADF); break; }
            int64_t n = gp_console_write(static_cast<int>(fd), Ptr(buf), len);
            r[0] = static_cast<uint32_t>(n);
            break;
        }
        case NR_EXIT:
        case NR_EXIT_GROUP:
            Halt(static_cast<int>(r[0]));
            break;
        default:
            gp_log("unimplemented syscall %u at pc=%08x\n", nr, r[15]);
            r[0] = static_cast<uint32_t>(GP_ENOSYS);
            break;
        }
    }

    void Halt(int st) {
        exited = true;
        status = st;
        jit->HaltExecution();
    }
};

/* Load a flat binary — no ELF yet, that is the next milestone. */
bool LoadFlat(Machine &m, const char *path, uint32_t at) {
    gp_file *f = nullptr;
    int r = gp_open(path, GP_O_RDONLY, 0, &f);
    if (r < 0) { gp_log("cannot open %s: %d\n", path, r); return false; }

    struct gp_stat st;
    if (gp_fstat(f, &st) < 0) { gp_close(f); return false; }

    if (!m.Commit(at, static_cast<uint32_t>(st.size),
                  GP_PROT_READ | GP_PROT_WRITE | GP_PROT_EXEC)) {
        gp_close(f);
        return false;
    }

    int64_t n = gp_read(f, m.Ptr(at), static_cast<size_t>(st.size));
    gp_close(f);
    if (n != static_cast<int64_t>(st.size)) {
        gp_log("short read on %s: %lld of %llu\n", path,
               static_cast<long long>(n), static_cast<unsigned long long>(st.size));
        return false;
    }
    gp_log("loaded %s — %llu bytes at %08x\n", path,
           static_cast<unsigned long long>(st.size), at);
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: glasspole <flat-arm-binary>\n");
        return 2;
    }

    Machine m;
    if (!m.Map()) return 1;

    if (!m.Commit(STACK_TOP - STACK_SIZE, STACK_SIZE, GP_PROT_READ | GP_PROT_WRITE))
        return 1;
    if (!LoadFlat(m, argv[1], LOAD_ADDR)) return 1;

    Dynarmic::A32::UserConfig conf;
    conf.callbacks    = &m;
    /* The guest is ARMv7-A with Thumb-2 and VFPv2 — readelf -A on AppManager
     * says so. Leaving this at the v8 default would let dynarmic accept
     * encodings the real hardware would have rejected, which is the kind of
     * difference that shows up months later as one title behaving oddly. */
    conf.arch_version = Dynarmic::A32::ArchVersion::v7;

    Dynarmic::A32::Jit jit{conf};
    m.jit = &jit;

    jit.Regs()[13] = STACK_TOP;      /* sp */
    jit.Regs()[15] = LOAD_ADDR;      /* pc */
    jit.SetCpsr(0x10);               /* user mode, ARM state, interrupts on */

    while (!m.exited)
        jit.Run();

    gp_log("guest exited with status %d\n", m.status);
    return m.status;
}
