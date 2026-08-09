/* Glasspole — loading an ARM ELF into the guest's address space.
 *
 * This is the job qemu-user does before it executes anything, and it is
 * entirely host-independent: it reads a file through host.h and writes bytes
 * into guest memory. Nothing here knows what operating system it is on, which
 * is the point.
 *
 * WE DO NOT LINK ANYTHING. The rootfs ships its own ld-uClibc.so.0 and it is
 * very good at its job. All we do is put the executable and the interpreter in
 * memory, build the process image the interpreter expects to wake up in, and
 * jump to it — after which the guest resolves its own symbols, using nothing
 * but the syscalls we implement. That is the whole reason a 51-entry checklist
 * is enough to run a program that pulls in twenty shared objects.
 */
#ifndef GLASSPOLE_ELF_H
#define GLASSPOLE_ELF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The guest's address space, as the loader needs to see it: somewhere to write
 * and a way to ask for pages. Passing this rather than reaching for a global
 * keeps elf.c testable and keeps cpu.cpp's Machine private to cpu.cpp. */
typedef struct gp_guest {
    uint8_t *base;   /* host pointer at which guest address 0 lives */
    int    (*commit)(void *ctx, uint32_t addr, uint32_t len, int prot);
    void    *ctx;
} gp_guest;

typedef struct gp_image {
    uint32_t entry;        /* first instruction: the interpreter's, if there is one */
    uint32_t exec_entry;   /* the executable's own entry, for AT_ENTRY */
    uint32_t phdr;         /* guest address of the program headers, for AT_PHDR */
    uint32_t phent, phnum;
    uint32_t brk;          /* end of the highest PT_LOAD, where the heap starts */
    uint32_t interp_base;  /* 0 when the binary is static */
    char     interp[128];  /* the PT_INTERP string, empty when static */
} gp_image;

/* Map `path` at `bias` (0 for ET_EXEC). Returns 0, or a negative errno. */
int gp_elf_load(gp_guest *g, const char *path, uint32_t bias, gp_image *out);

/* Load an executable and, if it names one, its interpreter — the normal case.
 * `sysroot` is prepended to the interpreter's absolute path, because
 * /lib/ld-uClibc.so.0 means the rootfs's, not the host's. */
int gp_elf_load_program(gp_guest *g, const char *path, const char *sysroot,
                        uint32_t interp_bias, gp_image *out);

/* Build the stack the interpreter expects: argc, argv, envp, then the auxiliary
 * vector. Returns the guest sp to start with, or 0 on failure.
 *
 * Getting this wrong is the classic way to spend a day watching ld.so segfault
 * before it has executed a single instruction of the program. */
uint32_t gp_elf_build_stack(gp_guest *g, uint32_t stack_top,
                            int argc, const char *const *argv,
                            const char *const *envp, const gp_image *img);

#ifdef __cplusplus
}
#endif
#endif /* GLASSPOLE_ELF_H */
