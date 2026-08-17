/* Glasspole milestone 2 — clone, futex and thread exit, with no libc involved.
 *
 * Raw syscalls on purpose. A pthreads test would prove that uClibc works, which
 * is a different claim and a much harder one to debug when it fails: every
 * failure would arrive wrapped in several thousand instructions of library. This
 * exercises exactly the three things that are new and nothing else.
 *
 *   1. clone() with the NPTL flag set makes a second guest thread
 *   2. the child runs on its own stack, shares memory, and writes to the same fd
 *   3. the child's exit clears the CLONE_CHILD_CLEARTID word and futex-wakes it,
 *      which is precisely how pthread_join finds out a thread is gone
 *
 * The expected output is both lines, child first.
 */
    .text
    .arm
    .global _start
_start:
    @ A stack for the child: mmap2(NULL, 64K, RW, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
    mov  r7, #192
    mov  r0, #0
    mov  r1, #0x10000
    mov  r2, #3                     @ PROT_READ|PROT_WRITE
    mov  r3, #0x22                  @ MAP_PRIVATE|MAP_ANONYMOUS
    mvn  r4, #0                     @ fd = -1
    mov  r5, #0
    svc  #0
    add  r9, r0, #0x10000           @ stacks grow down, so start at the top

    @ The word the child will clear on exit. Seed it non-zero so the parent's
    @ futex wait has something to wait for.
    /* ldr =, not adr: .data sits a whole segment away and adr's range is a few
     * hundred bytes. The binary is loaded at a fixed address, so the absolute
     * value in the literal pool is correct. */
    ldr  r8, =tidword
    mov  r0, #1
    str  r0, [r8]

    @ clone(flags, child_stack, parent_tid, tls, child_tid)
    @ CLONE_VM|FS|FILES|SIGHAND|THREAD|CHILD_CLEARTID — the NPTL set, minus the
    @ TLS pieces this test does not need. Built with three ORRs because
    @ 0x210f00 is not a single ARM immediate.
    mov  r0, #0x00200000            @ CLONE_CHILD_CLEARTID
    orr  r0, r0, #0x00010000        @ CLONE_THREAD
    orr  r0, r0, #0x00000f00        @ CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND
    mov  r1, r9
    mov  r2, #0
    mov  r3, #0
    mov  r4, r8
    mov  r7, #120
    svc  #0
    cmp  r0, #0
    beq  child

parent:
    @ futex(&tidword, FUTEX_WAIT|FUTEX_PRIVATE, 1, NULL)
    @ If the child already finished, the word is 0, this returns EAGAIN at once,
    @ and that is a correct outcome rather than a race to lose.
    mov  r0, r8
    mov  r1, #128
    mov  r2, #1
    mov  r3, #0
    mov  r7, #240
    svc  #0

    mov  r7, #4
    mov  r0, #1
    adr  r1, pmsg
    adr  r2, pend
    sub  r2, r2, r1
    svc  #0

    mov  r7, #248                   @ exit_group: the whole guest stops
    mov  r0, #0
    svc  #0

child:
    mov  r7, #4
    mov  r0, #1
    adr  r1, cmsg
    adr  r2, cend
    sub  r2, r2, r1
    svc  #0

    mov  r7, #1                     @ exit: THIS thread only
    mov  r0, #0
    svc  #0

    .ltorg                          @ the literal pool, before the strings

cmsg:
    .ascii "child thread ran\n"
cend:
pmsg:
    .ascii "parent saw the child exit\n"
pend:

    /* .data, not .text. The ELF loader applies each segment's real protection,
     * so the futex word has to live somewhere writable — putting it after the
     * strings faults on the first `str`, exactly as it would on real hardware. */
    .data
    .align 2
tidword:
    .word 0
