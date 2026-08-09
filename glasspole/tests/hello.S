/* Glasspole milestone 0 — the smallest ARM program that proves the spine.
 *
 * Flat binary, no ELF and no libc: it is loaded straight to LOAD_ADDR and
 * entered at offset 0. If this prints, then dynarmic translated ARMv7, our
 * memory callbacks served the instruction stream and the literal, and an SVC
 * came back out through host.h to the host's console. That is the whole
 * emulator in miniature.
 *
 * Deliberately position-independent — `adr` rather than a literal pool — so it
 * does not care where the loader put it.
 */
    .text
    .arm
    .global _start
_start:
    mov  r7, #4                 @ __NR_write (ARM EABI)
    mov  r0, #1                 @ stdout
    adr  r1, msg                @ two adrs and a sub rather than a length
    adr  r2, msg_end            @ constant: `mov r2, #(msg_end - msg)` cannot
    sub  r2, r2, r1             @ assemble, the symbols being forward refs
    svc  #0

    mov  r7, #248               @ __NR_exit_group
    mov  r0, #0
    svc  #0

msg:
    .ascii "hello from an ARM guest\n"
msg_end:
