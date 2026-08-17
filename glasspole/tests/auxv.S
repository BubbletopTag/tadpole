/* Walk our own initial stack to the auxiliary vector and dump it raw. */
    .text
    .arm
    .global _start
_start:
    mov  r0, sp
    ldr  r1, [r0]               @ argc
    add  r0, r0, #4             @ -> argv[0]
    add  r0, r0, r1, lsl #2     @ skip argv
    add  r0, r0, #4             @ skip the NULL after argv
1:  ldr  r2, [r0]               @ skip envp
    add  r0, r0, #4
    cmp  r2, #0
    bne  1b
    mov  r1, r0                 @ auxv
    mov  r0, #1
    mov  r2, #256
    mov  r7, #4
    svc  #0
    mov  r7, #248
    mov  r0, #0
    svc  #0
