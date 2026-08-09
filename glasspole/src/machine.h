/* Glasspole — the state one guest process runs in.
 *
 * Shared between cpu.cpp (which owns dynarmic) and syscall.cpp (which owns the
 * 51). Kept deliberately small: anything that belongs to the host goes through
 * host.h, and anything that belongs to the guest lives in guest memory.
 */
#ifndef GLASSPOLE_MACHINE_H
#define GLASSPOLE_MACHINE_H

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include "host.h"
#include "elf.h"
}

namespace Dynarmic::A32 { class Jit; }

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

struct Machine {
    uint8_t *base = nullptr;                 /* guest address 0 */
    Dynarmic::A32::Jit *jit = nullptr;

    std::string sysroot;
    gp_image    image{};

    /* Where the next anonymous mapping goes. A bump allocator is honest about
     * what it is: the guest calls mmap 295 times in a whole run, so there is
     * nothing here worth an allocator until munmap actually needs reusing. */
    uint32_t mmap_next = 0;
    uint32_t brk_cur   = 0;

    /* The two ARM thread-pointer registers, read through CP15. `tls` is
     * TPIDRURO, which the set_tls syscall writes and every __thread access
     * reads; `tls_rw` is the one userspace may write itself. dynarmic compiles
     * a direct load from these fields, so they must outlive the Jit. */
    uint32_t tls    = 0;
    uint32_t tls_rw = 0;

    std::vector<GuestFd> fds;

    bool exited = false;
    int  status = 0;

    /* Watchdog. dynarmic runs until its tick budget is spent, so a bounded
     * budget gives us a place to stand: if the guest burns through several of
     * them without making a single syscall, it is spinning, and the PC at that
     * moment is the answer. Without this a hang is a black box — and a hang is
     * exactly what qemu-arm would let us diff against on Linux and nothing
     * would on Windows. */
    uint64_t ticks_left = 0;
    uint64_t syscalls   = 0;

    /* Diagnostics. TADPOLE-style: say what the guest asked for by name the
     * first time, so an unimplemented call reports itself rather than looking
     * like a hang. */
    bool trace = false;

    uint8_t *Ptr(uint32_t v) { return base + v; }
    std::string Str(uint32_t v);             /* a C string out of guest memory */

    int  Commit(uint32_t addr, uint32_t len, int prot);
    int  AllocFd(gp_file *f, gp_dir *d, const std::string &path);
    void OpenStdio();
    GuestFd *Fd(int fd);

    /* Guest path -> host path. The sysroot rewrite lives here because it is
     * Tadpole's knowledge, not the host's — host.h only ever sees a real
     * path on a real filesystem. */
    std::string HostPath(const std::string &guest);

    void Halt(int st);
};

namespace Dynarmic::A32 { class Coprocessor; }
/* CP15, so the guest can read its thread pointer. See cp15.cpp. */
std::shared_ptr<Dynarmic::A32::Coprocessor> gp_make_cp15(Machine &m);

/* Called from dynarmic's CallSVC. Reads and writes the guest's registers. */
void gp_syscall(Machine &m);

#endif /* GLASSPOLE_MACHINE_H */
