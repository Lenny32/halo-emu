/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * setjmp/longjmp for the Zephyr context on native_sim x86-64 (ticket 0005).
 *
 * The firmware builds with picolibc (matching hardware), whose x86-64
 * jmp_buf is 8 * 8 = 64 bytes (machine/setjmp.h: _JBLEN 8) — but the
 * picolibc Zephyr module ships no x86-64 setjmp implementation, so the
 * final link silently resolved setjmp/longjmp from the HOST glibc. Glibc's
 * exported setjmp is the signal-mask-saving variant with a ~200-byte
 * jmp_buf: it wrote far past the 64-byte buffer, smashing whatever the
 * caller kept next to it. For Lua that meant luaD_rawrunprotected()'s
 * on-stack `struct lua_longjmp.status` was overwritten with 1
 * (__mask_was_saved) on every setjmp, so lua_newstate() looped forever in
 * luaD_closeprotected() and boot never reached the REPL.
 *
 * This is the minimal System-V implementation for picolibc's layout:
 * callee-saved GPRs + rsp + return address, no signal mask (Zephyr code
 * has no host signal state to save). Slots:
 *   0:rbx 1:rbp 2:r12 3:r13 4:r14 5:r15 6:rsp(after return) 7:rip
 */

#if !defined(__x86_64__)
#error "emulator setjmp shim is x86-64 only (BOARD=native_sim needs an i386 version)"
#endif

    .text

    .globl setjmp
    .type  setjmp, @function
setjmp:
    .cfi_startproc
    endbr64
    movq %rbx,  0(%rdi)
    movq %rbp,  8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    leaq 8(%rsp), %rax      /* rsp as it will be after ret */
    movq %rax, 48(%rdi)
    movq (%rsp), %rax       /* return address */
    movq %rax, 56(%rdi)
    xorl %eax, %eax
    ret
    .cfi_endproc
    .size setjmp, . - setjmp

    .globl longjmp
    .type  longjmp, @function
longjmp:
    .cfi_startproc
    endbr64
    movq  0(%rdi), %rbx
    movq  8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movl %esi, %eax
    testl %eax, %eax
    jnz 1f
    incl %eax               /* longjmp(env, 0) must return 1 */
1:
    movq 48(%rdi), %rsp
    jmpq *56(%rdi)
    .cfi_endproc
    .size longjmp, . - longjmp

/* picolibc declares _setjmp/_longjmp as aliases of the same layout */
    .globl _setjmp
    .set   _setjmp, setjmp
    .globl _longjmp
    .set   _longjmp, longjmp

    .section .note.GNU-stack,"",@progbits
