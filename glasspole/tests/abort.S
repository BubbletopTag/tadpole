/* Glasspole — does a raised SIGABRT actually kill the process?
 *
 * WHY THIS TEST AND NOT A TITLE. Seventeen titles called abort() and SURVIVED
 * it, because tkill and tgkill were unimplemented and uClibc's abort() falls
 * through to _exit(127) when its raise does not kill anything. That is a
 * two-line question buried under a game engine, so ask it directly:
 *
 *     tgkill(getpid(), gettid(), SIGABRT)
 *
 * PASS is the emulator printing "terminated by SIGABRT (6)" and exiting 134.
 * FAIL is this program reaching the writes below, which say so in as many
 * words and exit 127 — the same number the titles reported, so a failure here
 * reads exactly like the bug it is guarding against.
 *
 * Both spellings are exercised: uClibc's raise() uses tgkill where it has a
 * thread group and tkill where it does not, and only one of them being wired
 * up is a bug that hides behind whichever libc happens to be linked.
 */
    .text
    .arm
    .global _start
_start:
    mov  r7, #224               @ __NR_gettid
    svc  #0
    mov  r4, r0                 @ r4 = our tid

    mov  r7, #20                @ __NR_getpid
    svc  #0
    mov  r5, r0                 @ r5 = our pid (the thread group)

    mov  r7, #268               @ __NR_tgkill
    mov  r0, r5                 @ tgid
    mov  r1, r4                 @ tid
    mov  r2, #6                 @ SIGABRT
    svc  #0

    @ Still here: tgkill did not kill us. Try tkill, which is what uClibc
    @ reaches for next, so that a half-implemented pair is still caught.
    mov  r7, #238               @ __NR_tkill
    mov  r0, r4
    mov  r1, #6
    svc  #0

    @ Still here. This is the failure the titles showed, verbatim.
    mov  r7, #4                 @ __NR_write
    mov  r0, #2                 @ stderr
    adr  r1, msg
    adr  r2, msg_end
    sub  r2, r2, r1
    svc  #0

    mov  r7, #248               @ __NR_exit_group
    mov  r0, #127               @ what uClibc's abort() falls through to
    svc  #0

msg:
    .ascii "FAIL: survived SIGABRT, exiting 127 like every aborting title did\n"
msg_end:
