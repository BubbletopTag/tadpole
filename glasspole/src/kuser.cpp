/* Glasspole — the ARM kuser helper page.
 *
 * The Linux kernel maps a page at 0xffff0000 containing three routines that
 * userspace calls by ABSOLUTE ADDRESS. They exist because ARMv5 had no atomic
 * primitives, so the kernel published implementations that work on whatever
 * hardware is actually underneath. Nothing declares a dependency on them and
 * nothing links against them — the addresses are simply baked into libgcc and
 * uClibc.
 *
 * So an emulator that does not provide this page looks fine until the guest
 * reads a __thread variable through libgcc's path, and then jumps to
 * 0xffff0fe0 and dies. That is exactly what AppManager did, and it took the
 * fault handler to say so: "GUEST FAULT: ffff0fe0 was never mapped".
 *
 *   0xffff0fa0  __kuser_memory_barrier
 *   0xffff0fc0  __kuser_cmpxchg
 *   0xffff0fe0  __kuser_get_tls
 *   0xffff0ffc  __kuser_helper_version
 *
 * The code below is real ARM, assembled rather than guessed. To regenerate it,
 * assemble this and read back the encodings:
 *
 *     barrier:  dmb sy ; bx lr
 *     cmpxchg:  1: ldrex r3,[r2] ; subs r3,r3,r0 ; strexeq r3,r1,[r2]
 *                  teqeq r3,#1 ; beq 1b ; rsbs r0,r3,#0 ; dmb sy ; bx lr
 *     get_tls:  mrc p15,0,r0,c13,c0,3 ; bx lr
 *
 * cmpxchg goes through ldrex/strex, so it lands on the same host
 * compare-and-swap the guest's own inline atomics use, and the two agree by
 * construction rather than by luck.
 */
#include "machine.h"

#include <cstring>

namespace {

constexpr uint32_t KUSER_BASE = 0xffff0000;

const uint32_t kuser_barrier[] = {
    0xf57ff05f,   /* dmb sy */
    0xe12fff1e,   /* bx  lr */
};

const uint32_t kuser_cmpxchg[] = {
    0xe1923f9f,   /* ldrex   r3, [r2]     */
    0xe0533000,   /* subs    r3, r3, r0   */
    0x01823f91,   /* strexeq r3, r1, [r2] */
    0x03330001,   /* teqeq   r3, #1       */
    0x0afffffa,   /* beq     back to ldrex */
    0xe2730000,   /* rsbs    r0, r3, #0   */
    0xf57ff05f,   /* dmb     sy           */
    0xe12fff1e,   /* bx      lr           */
};

const uint32_t kuser_get_tls[] = {
    0xee1d0f70,   /* mrc p15, 0, r0, c13, c0, 3 */
    0xe12fff1e,   /* bx  lr */
};

}  // namespace

int gp_install_kuser_page(Machine &m) {
    int r = m.Commit(KUSER_BASE, GP_PAGE, GP_PROT_READ | GP_PROT_WRITE);
    if (r < 0) return r;

    std::memset(m.Ptr(KUSER_BASE), 0, GP_PAGE);
    std::memcpy(m.Ptr(0xffff0fa0), kuser_barrier, sizeof kuser_barrier);
    std::memcpy(m.Ptr(0xffff0fc0), kuser_cmpxchg, sizeof kuser_cmpxchg);
    std::memcpy(m.Ptr(0xffff0fe0), kuser_get_tls, sizeof kuser_get_tls);

    /* Helper version 5, which is what a 2.6.32-era kernel published and what
     * the guest's libgcc was built against. */
    const uint32_t version = 5;
    std::memcpy(m.Ptr(0xffff0ffc), &version, 4);

    /* Read and execute, not write. The guest must not be able to rewrite the
     * kernel's helpers, and a write here would be a bug worth catching. */
    return m.Commit(KUSER_BASE, GP_PAGE, GP_PROT_READ | GP_PROT_EXEC);
}
