/* Glasspole — the ELF loader. See elf.h for why it does so little.
 *
 * Structures are declared here rather than included from <elf.h>, for the same
 * reason tadpole_shim.c declares its kernel structures by hand: the host's
 * headers describe the HOST, and this code parses a 32-bit little-endian ARM
 * file regardless of what it is running on. A Windows build has no <elf.h> at
 * all, and one that came from a package would be the wrong one.
 */
#include "elf.h"
#include "host.h"

#include <string.h>

/* ---- the file format ---------------------------------------------------- */

#define ET_EXEC 2
#define ET_DYN  3
#define EM_ARM  40

#define PT_LOAD    1
#define PT_INTERP  3
#define PT_PHDR    6

#define PF_X 1
#define PF_W 2
#define PF_R 4

typedef struct {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Elf32_Ehdr;

typedef struct {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
} Elf32_Phdr;

/* ---- helpers ------------------------------------------------------------ */

static uint32_t page_down(uint32_t a) { return a & ~(GP_PAGE - 1); }
static uint32_t page_up  (uint32_t a) { return (a + GP_PAGE - 1) & ~(GP_PAGE - 1); }

static int prot_of(uint32_t f) {
    int p = 0;
    if (f & PF_R) p |= GP_PROT_READ;
    if (f & PF_W) p |= GP_PROT_WRITE;
    if (f & PF_X) p |= GP_PROT_EXEC;
    return p;
}

/* ---- loading ------------------------------------------------------------ */

int gp_elf_load(gp_guest *g, const char *path, uint32_t bias, gp_image *out) {
    gp_file *f = NULL;
    int r = gp_open(path, GP_O_RDONLY, 0, &f);
    if (r < 0) { gp_log("elf: cannot open %s (%d)\n", path, r); return r; }

    Elf32_Ehdr eh;
    if (gp_pread(f, &eh, sizeof eh, 0) != (int64_t)sizeof eh) { gp_close(f); return GP_EIO; }

    if (memcmp(eh.ident, "\177ELF", 4) != 0 || eh.ident[4] != 1 /*ELFCLASS32*/ ||
        eh.ident[5] != 1 /*ELFDATA2LSB*/ || eh.machine != EM_ARM) {
        gp_log("elf: %s is not a 32-bit little-endian ARM ELF\n", path);
        gp_close(f);
        return GP_EINVAL;
    }
    if (eh.type != ET_EXEC && eh.type != ET_DYN) { gp_close(f); return GP_EINVAL; }
    /* An ET_EXEC has its addresses baked in and cannot be moved. Silently
     * honouring a bias here would place it somewhere it does not believe it
     * is, and the failure surfaces much later as a wild jump. */
    if (eh.type == ET_EXEC) bias = 0;

    memset(out, 0, sizeof *out);
    out->entry = out->exec_entry = eh.entry + bias;
    out->phent = eh.phentsize;
    out->phnum = eh.phnum;

    uint32_t brk = 0;
    int have_phdr_addr = 0;

    for (uint32_t i = 0; i < eh.phnum; i++) {
        Elf32_Phdr ph;
        if (gp_pread(f, &ph, sizeof ph, eh.phoff + (uint64_t)i * eh.phentsize)
            != (int64_t)sizeof ph) { gp_close(f); return GP_EIO; }

        if (ph.type == PT_INTERP) {
            if (ph.filesz == 0 || ph.filesz >= sizeof out->interp) { gp_close(f); return GP_EINVAL; }
            if (gp_pread(f, out->interp, ph.filesz, ph.offset) != (int64_t)ph.filesz) {
                gp_close(f); return GP_EIO;
            }
            out->interp[ph.filesz] = 0;
            continue;
        }
        if (ph.type == PT_PHDR) { out->phdr = ph.vaddr + bias; have_phdr_addr = 1; continue; }
        if (ph.type != PT_LOAD) continue;

        const uint32_t va   = ph.vaddr + bias;
        const uint32_t lo   = page_down(va);
        const uint32_t hi   = page_up(va + ph.memsz);

        /* Commit writable regardless of the segment's flags: we are about to
         * write the file into it, and bss must be zeroed. The real protection
         * is applied afterwards. */
        r = g->commit(g->ctx, lo, hi - lo, GP_PROT_READ | GP_PROT_WRITE);
        if (r < 0) { gp_close(f); return r; }

        if (ph.filesz) {
            int64_t n = gp_pread(f, g->base + va, ph.filesz, ph.offset);
            if (n != (int64_t)ph.filesz) {
                gp_log("elf: short read on segment %u of %s\n", i, path);
                gp_close(f);
                return GP_EIO;
            }
        }
        /* .bss. The pages came from a fresh commit and are already zero, but
         * the tail of the LAST FILE PAGE is not — it holds whatever followed
         * filesz in the file, and leaving it is a genuine bug that presents as
         * uninitialised globals with plausible-looking values. */
        if (ph.memsz > ph.filesz) {
            uint32_t z    = va + ph.filesz;
            uint32_t zend = va + ph.memsz;
            uint32_t tail = page_up(z) < zend ? page_up(z) : zend;
            memset(g->base + z, 0, tail - z);
        }

        r = g->commit(g->ctx, lo, hi - lo, prot_of(ph.flags));
        if (r < 0) { gp_close(f); return r; }

        if (va + ph.memsz > brk) brk = va + ph.memsz;

        /* AT_PHDR when there is no PT_PHDR: the program headers live inside
         * whichever PT_LOAD covers e_phoff. */
        if (!have_phdr_addr && eh.phoff >= ph.offset &&
            eh.phoff < ph.offset + ph.filesz)
            out->phdr = va + (eh.phoff - ph.offset);
    }

    out->brk = page_up(brk);
    gp_close(f);

    gp_log("elf: %s — entry %08x, %u phdrs at %08x, brk %08x%s%s\n",
           path, out->entry, out->phnum, out->phdr, out->brk,
           out->interp[0] ? ", interp " : "", out->interp);
    return 0;
}

int gp_elf_load_program(gp_guest *g, const char *path, const char *sysroot,
                        uint32_t interp_bias, gp_image *out) {
    int r = gp_elf_load(g, path, 0, out);
    if (r < 0) return r;
    if (!out->interp[0]) return 0;   /* static: we are already done */

    /* /lib/ld-uClibc.so.0 means the rootfs's, not the host's. Getting this
     * wrong would silently load the host's linker into an ARM address space,
     * which fails in a way that looks nothing like the cause. */
    char full[512];
    size_t n = strlen(sysroot);
    if (n + strlen(out->interp) + 1 >= sizeof full) return GP_EINVAL;
    memcpy(full, sysroot, n);
    strcpy(full + n, out->interp);

    gp_image interp;
    r = gp_elf_load(g, full, interp_bias, &interp);
    if (r < 0) return r;

    out->interp_base = interp_bias;
    out->entry       = interp.entry;   /* the interpreter runs first */
    return 0;
}

/* ---- the initial process image ------------------------------------------ */

#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_BASE 7
#define AT_FLAGS 8
#define AT_ENTRY 9
#define AT_UID 11
#define AT_EUID 12
#define AT_GID 13
#define AT_EGID 14
#define AT_HWCAP 16
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_HWCAP2 26
#define AT_EXECFN 31
#define AT_PLATFORM 15

/* EXACTLY WHAT qemu-arm REPORTS, and that is the point: it was measured rather
 * than chosen. The first version of this was a guess — half-word loads, thumb,
 * fast multiply, VFP, EDSP, VFPv3 — which came to 0x20d6 against qemu's
 * 0x0fdfb0d7, missing TLS, NEON, VFPv4, integer divide and SWP among others.
 *
 * HWCAP_TLS (bit 15) is the one that matters most: libc and libgcc read it to
 * decide whether the thread pointer can be fetched straight from CP15 or has
 * to go through the kernel helper page. Every other bit steers a code path
 * too, which is how a wrong value here produces a guest that makes identical
 * syscalls and behaves differently — the hardest kind of difference to see.
 *
 * Claiming a feature obliges dynarmic to execute it. These are the same bits
 * qemu claims and the same guest runs there, so the instruction set is
 * already known to be covered. Regenerate with tests/auxv.S under both if this
 * ever needs revisiting. */
#define GP_HWCAP  0x0fdfb0d7u
#define GP_HWCAP2 0x0000007fu

uint32_t gp_elf_build_stack(gp_guest *g, uint32_t stack_top,
                            int argc, const char *const *argv,
                            const char *const *envp, const gp_image *img) {
    uint32_t p = stack_top;

    /* Strings first, at the very top, then the pointer arrays below them —
     * this is the layout the kernel produces and ld.so walks it by hand. */
    uint32_t argp[64], envp_addr[64];
    if (argc > 63) return 0;

    int envc = 0;
    while (envp && envp[envc]) envc++;
    if (envc > 63) return 0;

    for (int i = envc - 1; i >= 0; i--) {
        size_t n = strlen(envp[i]) + 1;
        p -= (uint32_t)n;
        memcpy(g->base + p, envp[i], n);
        envp_addr[i] = p;
    }
    for (int i = argc - 1; i >= 0; i--) {
        size_t n = strlen(argv[i]) + 1;
        p -= (uint32_t)n;
        memcpy(g->base + p, argv[i], n);
        argp[i] = p;
    }

    /* AT_PLATFORM and AT_EXECFN are strings the guest may read; they live up
     * here with the rest of the string area. */
    static const char platform[] = "v7l";
    p -= sizeof platform;
    uint32_t at_platform = p;
    memcpy(g->base + p, platform, sizeof platform);

    uint32_t at_execfn = argc > 0 ? argp[0] : at_platform;

    /* AT_RANDOM: sixteen bytes ld.so uses to seed the stack guard. Real
     * randomness, because a fixed value here is a defect that never shows up
     * as one. */
    p -= 16;
    uint32_t at_random = p;
    if (gp_random(g->base + p, 16) < 0) memset(g->base + p, 0x5a, 16);

    p &= ~15u;   /* AAPCS wants the stack 8-aligned; 16 costs nothing */

    /* auxv, then a NULL, then envp, then a NULL, then argv, then argc —
     * built downwards, so laid out here in reverse of how the guest reads it. */
    struct { uint32_t k, v; } aux[] = {
        { AT_NULL,   0 },
        { AT_PLATFORM, at_platform },
        { AT_EXECFN, at_execfn },
        { AT_HWCAP2, GP_HWCAP2 },
        { AT_RANDOM, at_random },
        { AT_SECURE, 0 },
        { AT_CLKTCK, 100 },
        { AT_HWCAP,  GP_HWCAP },
        /* 1000, as qemu-arm reports, not 0. A guest that believes it is root
         * takes different branches in anything that checks. */
        { AT_EGID,   1000 },
        { AT_GID,    1000 },
        { AT_EUID,   1000 },
        { AT_UID,    1000 },
        { AT_ENTRY,  img->exec_entry },
        { AT_FLAGS,  0 },
        { AT_BASE,   img->interp_base },
        { AT_PAGESZ, GP_PAGE },
        { AT_PHNUM,  img->phnum },
        { AT_PHENT,  img->phent },
        { AT_PHDR,   img->phdr },
    };
    const int naux = (int)(sizeof aux / sizeof aux[0]);

    uint32_t words = 1                      /* argc */
                   + (uint32_t)argc + 1     /* argv + NULL */
                   + (uint32_t)envc + 1     /* envp + NULL */
                   + (uint32_t)naux * 2;    /* auxv */
    p -= words * 4;
    p &= ~15u;

    uint32_t *w = (uint32_t *)(g->base + p);
    uint32_t  i = 0;
    w[i++] = (uint32_t)argc;
    for (int a = 0; a < argc; a++) w[i++] = argp[a];
    w[i++] = 0;
    for (int e = 0; e < envc; e++) w[i++] = envp_addr[e];
    w[i++] = 0;
    for (int a = naux - 1; a >= 0; a--) { w[i++] = aux[a].k; w[i++] = aux[a].v; }

    return p;
}
