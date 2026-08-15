/* Glasspole — guest signals.
 *
 * WHY THIS FILE EXISTS AT ALL, given that the census found the guest sending
 * almost none. Because the guest does not have to SEND one for signals to be
 * the difference between a bug report and a mystery:
 *
 *   * uClibc's abort() is a signal. It raises SIGABRT with tkill/tgkill, and
 *     when nothing kills the process it falls through to _exit(127). With no
 *     signals at all, seventeen titles called abort(), SURVIVED it, and left
 *     "guest exited with status 127" as the entire account of themselves.
 *   * Tadpole's shim installs SIGSEGV/SIGABRT handlers that write a symbolised
 *     backtrace to $TADPOLE_CRASHDIR/crash.log. Nothing could deliver to them,
 *     so not one title anywhere produced a backtrace — and the largest group
 *     in the compatibility sweep was "crashed, no diagnosis".
 *
 * So the product of this file is not compatibility with some title that sends
 * SIGUSR1. It is that a crashing guest gets to say where it crashed.
 *
 * THE ABI IS THE KERNEL'S, NOT LIBC'S. rt_sigaction takes the kernel's
 * struct — handler, flags, restorer, then a 64-bit mask — with sigsetsize
 * passed separately; libc's sigaction has a 128-byte sigset_t in the middle
 * and does not match. tadpole_crash.c already learned this the hard way and
 * says so at length, so the two sides now agree by construction.
 *
 * WHAT A DELIVERY IS. Exactly what the ARM kernel does in setup_rt_frame():
 * push a siginfo and a ucontext onto the guest's own stack, point r0/r1/r2 at
 * signal/siginfo/ucontext, set lr to a trampoline that calls rt_sigreturn, and
 * jump to the handler. The guest's handler then returns THROUGH the kernel, so
 * a handler that returns normally resumes the interrupted code — which is what
 * makes the shim's "report and hand on to AppManager's handler" path work.
 *
 * WHAT IS DELIBERATELY NOT HERE. Signal queueing (this is not sigqueue),
 * sigaltstack, stop/continue, and process groups. Nothing in the workload asks
 * for any of them, and each would need state whose only test would be a test
 * written to exercise it.
 */
#include "machine.h"

#include <dynarmic/interface/A32/a32.h>

#include <cstring>

namespace {

/* ---- the kernel's sa_flags ----------------------------------------------
 *
 * THESE ARE THE GUEST'S NUMBERS AND MUST NOT BECOME THE HOST'S. bionic exposes
 * SA_SIGINFO and friends to this translation unit as MACROS, out of the NDK's
 * asm-generic/signal-defs.h, where glibc does not — so on Android the four
 * declarations below expanded into their own values and the compiler reported
 * `error: expected unqualified-id`, which names neither the macro nor the
 * header it came from.
 *
 * Undefining them is right rather than merely expedient: what follows is a
 * decoder for the ARM guest's sigaction flags, and the host's happen to agree
 * today only because both are Linux. Taking the host's definition would be
 * borrowing a constant from the wrong kernel. */
#undef SA_SIGINFO
#undef SA_RESTORER
#undef SA_NODEFER
#undef SA_RESETHAND

constexpr uint32_t SA_SIGINFO   = 0x00000004;
constexpr uint32_t SA_RESTORER  = 0x04000000;
constexpr uint32_t SA_NODEFER   = 0x40000000;
constexpr uint32_t SA_RESETHAND = 0x80000000;

/* ---- the frame the kernel builds ----------------------------------------
 *
 * ARM's rt_sigframe is { siginfo, { ucontext, retcode[2] } } and its plain
 * sigframe is { ucontext, retcode[2] } — the same ucontext in both, at a
 * different offset. Which one is used depends on SA_SIGINFO, and sigreturn has
 * to look in the matching place, which is the whole reason there are two
 * sigreturn syscalls.
 *
 * struct ucontext, from arch/arm/include/asm/ucontext.h:
 *
 *      0   uc_flags
 *      4   uc_link
 *      8   uc_stack       (ss_sp, ss_flags, ss_size)
 *     20   uc_mcontext    struct sigcontext, 21 words
 *    104   uc_sigmask     the kernel's 64-bit sigset_t
 *    112   __unused[30]   room for glibc's 1024-bit one
 *    232   uc_regspace[128]   coprocessor state, zero-terminated
 *
 * uc_regspace is left ZERO on purpose: a zero where a magic number would be
 * is how the kernel's own restore_sigframe learns there is no VFP block to
 * restore, so the guest sees a truthful "nothing saved" rather than garbage.
 */
constexpr uint32_t SIGINFO_SIZE  = 128;
constexpr uint32_t UC_MCONTEXT   = 20;    /* byte offset inside the ucontext */
constexpr uint32_t UC_SIGMASK    = 104;
constexpr uint32_t UCONTEXT_SIZE = 744;
constexpr uint32_t RETCODE_SIZE  = 8;

/* struct sigcontext, in words from the start of uc_mcontext. */
constexpr uint32_t SC_TRAPNO = 0, SC_ERRCODE = 1, SC_OLDMASK = 2;
constexpr uint32_t SC_R0 = 3;             /* r0..r15 run to SC_R0 + 15 */
constexpr uint32_t SC_CPSR = 19, SC_FAULT = 20;

/* WHERE THE RETURN TRAMPOLINE LIVES, and it is the kernel's address, not one
 * we invented. A 2.6.32 ARM kernel puts its sigreturn codes in the vectors
 * page at 0xffff0500 and points lr there — which is why the page we already
 * map for the kuser helpers is the right home for these too. Anything that
 * unwinds a guest stack (gdb, and the shim's own scan) recognises the address.
 *
 * The words are the kernel's sigreturn_codes[] in its own order and at its own
 * indices: setup_return() computes `idx = thumb << 1` and adds 3 for a
 * SA_SIGINFO handler, so ARM lands on 0 and 3 and Thumb on 2 and 5. A Thumb
 * entry is ONE word holding TWO halfword instructions, which is why the layout
 * cannot be read as four neat pairs. */
constexpr uint32_t SIGRETURN_CODE = 0xffff0500;

const uint32_t sigreturn_codes[7] = {
    0xe3a07077,   /* [0] mov r7, #119  (__NR_sigreturn)        */
    0xef000000,   /* [1] svc 0         — see below             */
    0xdf002777,   /* [2] thumb: movs r7, #119 ; svc 0          */
    0xe3a070ad,   /* [3] mov r7, #173  (__NR_rt_sigreturn)     */
    0xef000000,   /* [4] svc 0                                 */
    0xdf0027ad,   /* [5] thumb: movs r7, #173 ; svc 0          */
    0x00000000,   /* [6] the kernel's spare, read as idx+1 at 5 */
};
/* The kernel encodes the syscall NUMBER into the swi immediate as well, for the
 * benefit of an OABI binary. Ours is plain `svc 0` because CallSVC ignores the
 * immediate and reads r7, which is what EABI means — and the `mov r7` above is
 * therefore the load-bearing half. */

/* ---- default actions ----------------------------------------------------- */

/* The four signals whose default disposition is not to kill. CONT is in the
 * list because a process that cannot be stopped has nothing to continue. */
bool default_ignores(int sig) {
    return sig == 17 /*CHLD*/ || sig == 18 /*CONT*/ ||
           sig == 23 /*URG*/  || sig == 28 /*WINCH*/;
}

const char *signame(int sig) {
    switch (sig) {
        case 1:  return "SIGHUP";   case 2:  return "SIGINT";
        case 3:  return "SIGQUIT";  case 4:  return "SIGILL";
        case 5:  return "SIGTRAP";  case 6:  return "SIGABRT";
        case 7:  return "SIGBUS";   case 8:  return "SIGFPE";
        case 9:  return "SIGKILL";  case 10: return "SIGUSR1";
        case 11: return "SIGSEGV";  case 12: return "SIGUSR2";
        case 13: return "SIGPIPE";  case 14: return "SIGALRM";
        case 15: return "SIGTERM";  case 17: return "SIGCHLD";
        default: return "signal";
    }
}

/* Read a guest sigset_t. sigsetsize is the kernel's, and it validates it — but
 * every caller here passes 8 and the two words are all we model. */
uint64_t read_set(Machine &m, uint32_t p) {
    if (!p) return 0;
    const uint32_t *s = reinterpret_cast<const uint32_t *>(m.Ptr(p));
    return (uint64_t)s[0] | ((uint64_t)s[1] << 32);
}

void write_set(Machine &m, uint32_t p, uint64_t v) {
    if (!p) return;
    uint32_t *s = reinterpret_cast<uint32_t *>(m.Ptr(p));
    s[0] = (uint32_t)v;
    s[1] = (uint32_t)(v >> 32);
}

/* ---- entering the handler ------------------------------------------------ */

/* Build the frame and point the thread at the handler. The caller has already
 * decided that this signal is being delivered and has updated the blocked mask;
 * `oldmask` is what it was before, and it is what sigreturn will restore.
 *
 * `fault` is the address that faulted, for SIGSEGV/SIGBUS, and zero otherwise.
 * The shim prints it as the first line of a crash report, so a wrong one is
 * worse than no report at all. */
void enter_handler(Thread &t, int sig, const SigAction &sa,
                   uint32_t fault, uint64_t oldmask) {
    Machine &m  = *t.m;
    auto    &r  = t.jit->Regs();
    const uint32_t cpsr = t.jit->Cpsr();

    const bool rt     = (sa.flags & SA_SIGINFO) != 0;
    const bool thumb  = (sa.handler & 1) != 0;
    const uint32_t sz = (rt ? SIGINFO_SIZE : 0) + UCONTEXT_SIZE + RETCODE_SIZE;

    /* EIGHT-BYTE ALIGNED, which the kernel requires and sys_rt_sigreturn
     * checks: it refuses a frame whose sp has the low three bits set. */
    const uint32_t frame = (r[13] - sz) & ~7u;
    const uint32_t uc    = rt ? frame + SIGINFO_SIZE : frame;
    const uint32_t rc    = uc + UCONTEXT_SIZE;

    std::memset(m.Ptr(frame), 0, sz);

    if (rt) {
        /* siginfo_t: si_signo, si_errno, si_code, then the union. SI_KERNEL(0x80)
         * for a fault, SI_TKILL(-6) for one a thread sent itself — the shim
         * ignores both, but anything that does look is entitled to the truth. */
        uint32_t *si = reinterpret_cast<uint32_t *>(m.Ptr(frame));
        si[0] = (uint32_t)sig;
        si[1] = 0;
        si[2] = fault ? 0x80u : (uint32_t)-6;
        if (fault) si[4] = fault;              /* si_addr, for SIGSEGV/SIGBUS */
    }

    uint32_t *u  = reinterpret_cast<uint32_t *>(m.Ptr(uc));
    uint32_t *sc = u + UC_MCONTEXT / 4;
    sc[SC_TRAPNO]  = fault ? 14u : 0u;         /* a data abort, as ARM numbers it */
    sc[SC_ERRCODE] = 0;
    sc[SC_OLDMASK] = (uint32_t)oldmask;
    /* r0..r15 land in arm_r0..arm_r10, arm_fp, arm_ip, arm_sp, arm_lr, arm_pc:
     * the kernel's sigcontext names them individually but they are consecutive
     * and in register order, which is the only reason this is a loop. */
    for (int i = 0; i < 16; i++) sc[SC_R0 + i] = r[i];
    sc[SC_CPSR]  = cpsr;
    sc[SC_FAULT] = fault;
    u[UC_SIGMASK / 4]     = (uint32_t)oldmask;
    u[UC_SIGMASK / 4 + 1] = (uint32_t)(oldmask >> 32);

    /* Where the handler returns to. SA_RESTORER is libc supplying its own
     * trampoline and it wins; otherwise it is the kernel's, in the vectors
     * page, at the index setup_return() would have used. */
    uint32_t retaddr;
    if ((sa.flags & SA_RESTORER) && sa.restorer) {
        retaddr = sa.restorer;
    } else {
        const unsigned idx = (thumb ? 2u : 0u) + (rt ? 3u : 0u);
        /* Written into the frame as well as being in the page, exactly as the
         * kernel does — a stack walker that expects to find them there is not
         * wrong to. */
        std::memcpy(m.Ptr(rc), &sigreturn_codes[idx], 4);
        std::memcpy(m.Ptr(rc + 4), &sigreturn_codes[idx + 1], 4);
        retaddr = SIGRETURN_CODE + (idx << 2) + (thumb ? 1u : 0u);
    }

    r[0]  = (uint32_t)sig;
    if (rt) { r[1] = frame; r[2] = uc; }       /* &info, &uc — SA_SIGINFO's 2nd and 3rd */
    r[13] = frame;
    r[14] = retaddr;
    r[15] = sa.handler & (thumb ? ~1u : ~3u);

    /* The flags are cleared and the instruction set comes from the handler's
     * low bit, which is ARM's calling convention rather than a choice. */
    uint32_t next = cpsr & ~0xff000200u;
    if (thumb) next |= 0x20u; else next &= ~0x20u;
    t.jit->SetCpsr(next);
}

/* Read the disposition for a signal about to be delivered, applying
 * SA_RESETHAND as the kernel does — before the handler runs, so that a fault
 * inside the handler goes to the default action instead of looping.
 *
 * The table is per PROCESS (every clone in the census carries CLONE_SIGHAND)
 * so it lives in Machine, under Machine::lock. `unlocked` is the fault path,
 * which cannot take a lock: it runs inside a host signal handler on a thread
 * that may already be holding it, and a racing rt_sigaction at the moment of
 * a crash is not a race worth deadlocking over. */
SigAction take_action(Machine &m, int sig, bool unlocked) {
    if (!unlocked) m.lock.lock();
    SigAction sa = m.sigactions[sig];
    if (sa.handler > 1 && (sa.flags & SA_RESETHAND)) m.sigactions[sig] = SigAction{};
    if (!unlocked) m.lock.unlock();
    return sa;
}

}  // namespace

/* ---- the page the trampolines live in ------------------------------------ */

void gp_signal_write_trampolines(Machine &m) {
    std::memcpy(m.Ptr(SIGRETURN_CODE), sigreturn_codes, sizeof sigreturn_codes);
}

/* ---- rt_sigaction / rt_sigprocmask --------------------------------------- */

/* CALLED WITH Machine::lock HELD, from gp_syscall, which is what guards the
 * disposition table against another thread installing a handler at the same
 * moment. */
int32_t gp_sigaction(Thread &t, int sig, uint32_t act, uint32_t oldact,
                     uint32_t setsize) {
    Machine &m = *t.m;
    if (sig < 1 || sig >= GP_NSIG) return GP_EINVAL;
    if (setsize != 8) return GP_EINVAL;      /* what the kernel's sigset_t is */
    /* KILL and STOP cannot be caught, and a libc that is told otherwise will
     * install a handler it can never reach. */
    if (act && (sig == 9 || sig == 19)) return GP_EINVAL;

    if (oldact) {
        uint32_t *o = reinterpret_cast<uint32_t *>(m.Ptr(oldact));
        const SigAction &s = m.sigactions[sig];
        o[0] = s.handler;
        o[1] = s.flags;
        o[2] = s.restorer;
        o[3] = (uint32_t)s.mask;
        o[4] = (uint32_t)(s.mask >> 32);
    }
    if (act) {
        const uint32_t *a = reinterpret_cast<const uint32_t *>(m.Ptr(act));
        SigAction s;
        s.handler  = a[0];
        s.flags    = a[1];
        s.restorer = a[2];
        s.mask     = (uint64_t)a[3] | ((uint64_t)a[4] << 32);
        m.sigactions[sig] = s;
    }
    return 0;
}

int32_t gp_sigprocmask(Thread &t, int how, uint32_t set, uint32_t oldset,
                       uint32_t setsize) {
    Machine &m = *t.m;
    if (setsize != 8) return GP_EINVAL;
    if (oldset) write_set(m, oldset, t.sigmask);
    if (set) {
        const uint64_t s = read_set(m, set);
        switch (how) {
            case 0: t.sigmask |=  s; break;                /* SIG_BLOCK   */
            case 1: t.sigmask &= ~s; break;                /* SIG_UNBLOCK */
            case 2: t.sigmask  =  s; break;                /* SIG_SETMASK */
            default: return GP_EINVAL;
        }
        /* KILL and STOP are never blocked, whatever was asked. */
        t.sigmask &= ~((1ull << 8) | (1ull << 18));
    }
    return 0;
}

/* ---- raising ------------------------------------------------------------- */

/* kill / tkill / tgkill, which differ only in how they name the target.
 *
 * CALLED WITH Machine::lock HELD, by gp_syscall, which is holding it anyway —
 * the thread list is walked here and taking the lock again would deadlock on a
 * mutex that is deliberately not recursive.
 *
 * A signal is MARKED PENDING rather than delivered here, because the delivery
 * has to happen after the syscall's own return value is in r0 — the context
 * the handler saves is the one sigreturn resumes, and a guest that came back
 * from raise() to find r0 still holding its own arguments would be entitled to
 * be confused. gp_syscall calls gp_signal_deliver once it has written r0.
 *
 * A signal aimed at ANOTHER thread halts that thread's Jit, so its run loop
 * notices on the way round rather than at some unbounded time in the future. */
int32_t gp_signal_send(Thread &t, uint32_t nr, uint32_t a0, uint32_t a1, uint32_t a2) {
    Machine &m = *t.m;
    constexpr uint32_t SYS_kill = 37, SYS_tkill = 238;
    const int  sig = (int)(nr == SYS_kill ? a1 : (nr == SYS_tkill ? a1 : a2));
    const uint32_t tid = nr == SYS_kill ? 0 : (nr == SYS_tkill ? a0 : a1);

    if (sig < 0 || sig >= GP_NSIG) return GP_EINVAL;
    if (sig == 0) return 0;                /* the "does it exist" probe */

    /* kill() names the process, and this emulator is one process: getpid()
     * answers 1000 and so must this. A process-directed signal goes to the
     * calling thread, which is where Linux would put it too when the caller
     * has it unblocked — and the one caller that matters, the shim's
     * re-raise from inside its own crash handler, is exactly that case. */
    if (nr == SYS_kill) {
        if ((int32_t)a0 != 1000 && (int32_t)a0 != 0 && (int32_t)a0 != -1)
            return GP_ESRCH;
        t.sigpend.fetch_or(1ull << (sig - 1), std::memory_order_relaxed);
        return 0;
    }

    if (tid == t.tid) {
        t.sigpend.fetch_or(1ull << (sig - 1), std::memory_order_relaxed);
        return 0;
    }

    for (auto &th : m.threads) {
        if (th->tid != tid) continue;
        th->sigpend.fetch_or(1ull << (sig - 1), std::memory_order_relaxed);
        /* Interrupt it so the signal is acted on now. The run loop checks for
         * pending signals every time Run() comes back. */
        if (th->jit) th->jit->HaltExecution();
        return 0;
    }
    return GP_ESRCH;
}

/* ---- delivery ------------------------------------------------------------ */

bool gp_signal_deliver(Thread &t) {
    Machine &m = *t.m;
    if (!t.jit) return false;

    /* A quit asked from outside becomes a guest SIGQUIT here, on the thread
     * that is going to run it, rather than in the host handler that counted
     * it. Checked at EVERY delivery point and not only in the run loop: an
     * idle guest spends its time inside a syscall callback and does not leave
     * jit.Run() until its whole instruction budget is spent, which for a
     * sleeping title is minutes — long enough that the tool looks broken. */
    const uint32_t gen = m.quit_gen.load(std::memory_order_relaxed);
    if (gen != t.quit_seen) {
        t.quit_seen = gen;
        t.sigpend.fetch_or(1ull << 2 /* SIGQUIT */, std::memory_order_relaxed);
    }

    for (;;) {
        const uint64_t take = t.sigpend.load(std::memory_order_relaxed) & ~t.sigmask;
        if (!take) return false;

        int sig = 1;
        while (!(take & (1ull << (sig - 1)))) sig++;
        t.sigpend.fetch_and(~(1ull << (sig - 1)), std::memory_order_relaxed);

        const SigAction sa = take_action(m, sig, false);

        if (sa.handler == 1) continue;                    /* SIG_IGN */

        if (sa.handler == 0) {
            if (default_ignores(sig)) continue;
            /* THE DEFAULT ACTION IS THE POINT. Without it uClibc's abort()
             * raises SIGABRT, survives, resets the handler, raises again,
             * survives again, and _exit(127)s — which is the "guest exited
             * with status 127" that five titles reported instead of dying.
             * 128+signal is the number a shell reports for a signal death and
             * the one the sweep's triage already knows how to read. */
            gp_log("tid %u: terminated by %s (%d)\n", t.tid, signame(sig), sig);
            m.ExitGroup(128 + sig);
            t.Halt();
            return true;
        }

        const uint64_t oldmask = t.sigmask;
        t.sigmask |= sa.mask;
        if (!(sa.flags & SA_NODEFER)) t.sigmask |= 1ull << (sig - 1);
        enter_handler(t, sig, sa, 0, oldmask);

        /* dynarmic only leaves a block when it is told to. The pc we just wrote
         * is read by the dispatcher on the way back in, so without this the
         * handler is set up and then never entered — the guest carries on from
         * the instruction after the svc as though nothing had happened. */
        if (t.jit->IsExecuting()) t.jit->HaltExecution();
        return true;
    }
}

/* A fault, from the host's own SIGSEGV handler.
 *
 * NO LOCKS AND NO ALLOCATION: this runs on an alternate signal stack with the
 * faulting thread stopped mid-instruction, and the thread may well be holding
 * the machine lock already. Returns false if there is no guest handler to run,
 * in which case the caller reports the fault and the process dies as before.
 *
 * The register state is what dynarmic last wrote back, so the pc names the
 * BLOCK rather than the instruction. That is the same approximation the fault
 * dump has always printed, and it still names the function — which is the
 * question a crash report is asked. */
bool gp_signal_fault(Thread &t, uint32_t fault_addr) {
    Machine &m = *t.m;
    if (!t.jit) return false;

    const SigAction sa = take_action(m, 11 /*SIGSEGV*/, true);
    if (sa.handler <= 1) return false;      /* SIG_DFL or SIG_IGN: nothing to run */

    const uint64_t oldmask = t.sigmask;
    /* A synchronous fault is delivered whether or not it is blocked — the
     * kernel forces it, because the alternative is a thread looping on an
     * instruction it cannot execute. */
    t.sigmask |= sa.mask;
    if (!(sa.flags & SA_NODEFER)) t.sigmask |= 1ull << 10;
    enter_handler(t, 11, sa, fault_addr, oldmask);
    return true;
}

/* ---- sigreturn ----------------------------------------------------------- */

/* The other half of enter_handler: put back everything it saved. sp is at the
 * frame because the handler's own epilogue restored it, which is what makes
 * this work without recording anything on our side. */
void gp_sigreturn(Thread &t, bool rt) {
    Machine &m = *t.m;
    auto    &r = t.jit->Regs();

    const uint32_t frame = r[13];
    const uint32_t uc    = rt ? frame + SIGINFO_SIZE : frame;
    const uint32_t *u    = reinterpret_cast<const uint32_t *>(m.Ptr(uc));
    const uint32_t *sc   = u + UC_MCONTEXT / 4;

    for (int i = 0; i < 16; i++) r[i] = sc[SC_R0 + i];
    t.jit->SetCpsr(sc[SC_CPSR]);
    t.sigmask = (uint64_t)u[UC_SIGMASK / 4] | ((uint64_t)u[UC_SIGMASK / 4 + 1] << 32);

    if (t.jit->IsExecuting()) t.jit->HaltExecution();
}
