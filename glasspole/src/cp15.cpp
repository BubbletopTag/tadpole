/* Glasspole — CP15, the ARM system coprocessor, reduced to the three things
 * userspace is allowed to touch.
 *
 * WHY THIS EXISTS. uClibc reads the thread pointer with
 *
 *     mrc p15, 0, rN, c13, c0, 3      @ TPIDRURO
 *
 * on every access to a __thread variable, which on ARMv7 is how TLS works and
 * therefore happens constantly. Without a coprocessor attached, dynarmic
 * compiles a coprocessor exception, and the guest dies before the dynamic
 * linker has finished loading libc — which is exactly what it did.
 *
 * dynarmic lets us answer with a POINTER rather than a callback, so the JIT
 * compiles a plain load from our field. That matters: this is on the hot path
 * of every threaded program, and a function call per TLS access would be a
 * tax on everything.
 *
 * Only registers real userspace can reach are answered. Everything else falls
 * through to std::monostate and still raises the exception, because a guest
 * poking at a system register is a fact worth learning about rather than
 * papering over.
 */
#include "machine.h"

#include <dynarmic/interface/A32/coprocessor.h>

using Dynarmic::A32::CoprocReg;

namespace {

/* Barriers. dynarmic's memory model plus the host's own ordering already give
 * us more than ARMv7 promises, so DMB/DSB/ISB have nothing to do — but they
 * must not fault, and the guest issues them around every lock. */
uint64_t barrier(void *, uint32_t, uint32_t) { return 0; }

}  // namespace

class Cp15 final : public Dynarmic::A32::Coprocessor {
public:
    explicit Cp15(Machine &m) : m_(m) {}

    std::optional<Callback> CompileInternalOperation(bool, unsigned, CoprocReg,
                                                     CoprocReg, CoprocReg,
                                                     unsigned) override {
        return std::nullopt;
    }

    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, CoprocReg CRn,
                                               CoprocReg CRm, unsigned opc2) override {
        if (two) return std::monostate{};

        /* c7 is the cache and barrier group: ISB (c5,4), DSB (c10,4),
         * DMB (c10,5). Accept and do nothing. */
        if (opc1 == 0 && CRn == CoprocReg::C7)
            return Callback{ barrier, std::nullopt };

        /* TPIDRURW — the thread pointer userspace may write. */
        if (opc1 == 0 && CRn == CoprocReg::C13 && CRm == CoprocReg::C0 && opc2 == 2)
            return &m_.tls_rw;

        return std::monostate{};
    }

    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, CoprocReg) override {
        return std::monostate{};
    }

    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, CoprocReg CRn,
                                              CoprocReg CRm, unsigned opc2) override {
        if (two) return std::monostate{};

        if (opc1 == 0 && CRn == CoprocReg::C13 && CRm == CoprocReg::C0) {
            if (opc2 == 2) return &m_.tls_rw;   /* TPIDRURW */
            if (opc2 == 3) return &m_.tls;      /* TPIDRURO — what set_tls sets */
        }
        return std::monostate{};
    }

    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, CoprocReg) override {
        return std::monostate{};
    }

    std::optional<Callback> CompileLoadWords(bool, bool, CoprocReg,
                                             std::optional<std::uint8_t>) override {
        return std::nullopt;
    }

    std::optional<Callback> CompileStoreWords(bool, bool, CoprocReg,
                                              std::optional<std::uint8_t>) override {
        return std::nullopt;
    }

private:
    Machine &m_;
};

std::shared_ptr<Dynarmic::A32::Coprocessor> gp_make_cp15(Machine &m) {
    return std::make_shared<Cp15>(m);
}
