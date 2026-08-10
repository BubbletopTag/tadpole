/* Glasspole — does a guest fault reach the guest's own handler, and can that
 * handler return?
 *
 * WHY IT MATTERS. Tadpole's shim installs a SIGSEGV handler and writes a
 * symbolised backtrace from it; that report is the only account this project
 * has of why a native title died. With no signal delivery, on_guest_fault
 * printed one line and the process vanished, so not one title under Glasspole
 * ever produced a backtrace while every title under qemu did.
 *
 * WHAT IS ACTUALLY PROVED, in order, and each step is load-bearing:
 *
 *   1. rt_sigaction accepts the KERNEL's struct — handler, flags, restorer,
 *      then a 64-bit mask, with sigsetsize 8 — rather than libc's.
 *   2. A fault on an unmapped guest address becomes a guest SIGSEGV rather
 *      than the death of the emulator.
 *   3. The frame is the ARM kernel's: r0 is the signal, r2 points at a
 *      ucontext whose uc_mcontext.fault_address is the address that faulted.
 *   4. The handler can EDIT that context and return, and rt_sigreturn puts it
 *      back — which is the whole reason a handler is allowed to return, and
 *      what lets the shim report a crash and hand it on to AppManager's own
 *      handler instead of ending the process.
 *
 * PASS is one line on stdout and exit 0. A failure says so on stderr and the
 * EXIT STATUS names the step: 1 install, 2 wrong signal, 3 wrong fault
 * address, 4 sigreturn ignored the pc the handler asked for. A hang means
 * sigreturn resumed at the faulting instruction instead.
 *
 * The status carries the detail rather than the message because ARM's `adr`
 * reaches only an 8-bit rotated displacement, and five separate messages put
 * their own labels out of range — which is an assembler error and not worth
 * the shape it forces on the code.
 *
 * The sigaction struct is built ON THE STACK on purpose: this program is
 * loaded with its text read-only, and a .data section would not survive the
 * flat-binary form the other tests here use.
 */

    .equ SIGSEGV,   11
    .equ SA_SIGINFO, 4
    .equ FAULT_AT,  0x44          @ the address a dozen real titles die on

    @ struct ucontext: uc_mcontext is at +20, and inside it arm_r0 is word 3,
    @ so arm_pc (word 18) is at 20 + 72 and fault_address (word 20) at 20 + 80.
    .equ UC_ARM_PC,  92
    .equ UC_FAULT,  100

    .text
    .arm
    .global _start
_start:
    sub  sp, sp, #32
    adr  r0, handler
    str  r0, [sp, #0]           @ .handler
    mov  r0, #SA_SIGINFO
    str  r0, [sp, #4]           @ .flags — no SA_RESTORER, so the kernel's
    mov  r0, #0                 @         trampoline in the vectors page is
    str  r0, [sp, #8]           @ .restorer      what we should come back on
    str  r0, [sp, #12]          @ .mask, low
    str  r0, [sp, #16]          @ .mask, high

    mov  r7, #174               @ __NR_rt_sigaction
    mov  r0, #SIGSEGV
    mov  r1, sp
    mov  r2, #0                 @ no oldact
    mov  r3, #8                 @ sigsetsize: the kernel's, not libc's
    svc  #0
    mov  r6, #1
    cmp  r0, #0
    bne  die

    mov  r0, #FAULT_AT
    ldr  r1, [r0]               @ boom

    @ Reached only if sigreturn resumed where the fault happened, which would
    @ mean the handler's edit to arm_pc was ignored.
    mov  r6, #4
    b    die

/* r0 = signal, r1 = siginfo, r2 = ucontext. */
handler:
    mov  r6, #2
    cmp  r0, #SIGSEGV
    bne  die
    mov  r6, #3
    ldr  r3, [r2, #UC_FAULT]
    cmp  r3, #FAULT_AT
    bne  die

    @ Step over the instruction that faulted by rewriting the pc the kernel
    @ will restore. Returning without this would fault again for ever.
    adr  r3, resume
    str  r3, [r2, #UC_ARM_PC]
    bx   lr

resume:
    mov  r7, #4                 @ __NR_write
    mov  r0, #1
    adr  r1, m_pass
    adr  r2, m_pass_end
    sub  r2, r2, r1
    svc  #0
    mov  r7, #248               @ __NR_exit_group
    mov  r0, #0
    svc  #0

/* r6 says which step failed, and becomes the exit status. */
die:
    mov  r7, #4
    mov  r0, #2
    adr  r1, m_fail
    adr  r2, m_fail_end
    sub  r2, r2, r1
    svc  #0
    mov  r7, #248
    mov  r0, r6
    svc  #0

m_pass:
    .ascii "PASS: SIGSEGV delivered to the guest, and sigreturn resumed it\n"
m_pass_end:
m_fail:
    .ascii "FAIL: see the exit status for which step\n"
m_fail_end:
