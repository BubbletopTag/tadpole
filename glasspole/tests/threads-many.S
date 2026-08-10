/* Glasspole — do thread slots come BACK?
 *
 * tests/thread.S proves one thread can be made and joined. This proves the
 * thing that broke: that making a thread, letting it finish, and making another
 * can go on indefinitely.
 *
 * It could not. The processor id handed to each new thread was
 * `m.threads.size()`, and that vector only ever grew — so the thirty-second
 * CREATION exhausted the ExclusiveMonitor's slots however few threads were
 * alive, and clone() began returning EAGAIN with the log blaming a ceiling
 * nothing had reached. A title that cycles worker threads — which is most of
 * them, over a play session — would die partway through for no visible reason.
 *
 * So: 96 threads, one at a time, against a monitor sized for 32. Each child
 * exits immediately; the parent waits for CLONE_CHILD_CLEARTID to zero the
 * word and go again. Before the fix this stops at the 32nd and prints FAIL.
 * After it, all 96 run.
 *
 * Raw syscalls, no libc, for the same reason thread.S uses them: a failure here
 * should be about clone and nothing else.
 */
    .text
    .arm
    .global _start
_start:
    @ One stack, reused. Each child is joined before the next is made, so there
    @ is never more than one child on it.
    mov  r7, #192                   @ mmap2
    mov  r0, #0
    mov  r1, #0x10000
    mov  r2, #3                     @ PROT_READ|PROT_WRITE
    mov  r3, #0x22                  @ MAP_PRIVATE|MAP_ANONYMOUS
    mvn  r4, #0                     @ fd = -1
    mov  r5, #0
    svc  #0
    add  r9, r0, #0x10000           @ stacks grow down

    ldr  r10, =tidword
    mov  r11, #96                   @ three times the monitor's size

loop:
    mov  r0, #1                     @ seed the word the child will clear
    str  r0, [r10]

    @ clone(CLONE_VM|FS|FILES|SIGHAND|THREAD|CHILD_CLEARTID, stack, 0, 0, &word)
    mov  r0, #0x00200000            @ CLONE_CHILD_CLEARTID
    orr  r0, r0, #0x00010000        @ CLONE_THREAD
    orr  r0, r0, #0x00000f00        @ CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND
    mov  r1, r9
    mov  r2, #0
    mov  r3, #0
    mov  r4, r10
    mov  r7, #120
    svc  #0
    cmp  r0, #0
    beq  child
    blt  fail                       @ EAGAIN: the slots never came back

wait:
    @ The child may already have finished, in which case the word is 0 and
    @ there is nothing to wait for. That is a correct outcome, not a race.
    ldr  r0, [r10]
    cmp  r0, #0
    beq  next
    mov  r0, r10
    mov  r1, #128                   @ FUTEX_WAIT|FUTEX_PRIVATE
    mov  r2, #1                     @ expected
    mov  r3, #0                     @ no timeout
    mov  r7, #240
    svc  #0
    b    wait

next:
    subs r11, r11, #1
    bne  loop

    mov  r7, #4                     @ write(1, ok)
    mov  r0, #1
    adr  r1, okmsg
    adr  r2, okend
    sub  r2, r2, r1
    svc  #0
    mov  r7, #248                   @ exit_group
    mov  r0, #0
    svc  #0

fail:
    mov  r7, #4
    mov  r0, #1
    adr  r1, badmsg
    adr  r2, badend
    sub  r2, r2, r1
    svc  #0
    mov  r7, #248
    mov  r0, #1
    svc  #0

child:
    mov  r7, #1                     @ exit: THIS thread only
    mov  r0, #0
    svc  #0

    .ltorg

okmsg:
    .ascii "96 threads made and retired\n"
okend:
badmsg:
    .ascii "FAIL: clone ran out of slots\n"
badend:

    /* Writable, for the same reason thread.S says: the loader applies each
     * segment's real protection and the futex word is written. */
    .data
    .align 2
tidword:
    .word 0
