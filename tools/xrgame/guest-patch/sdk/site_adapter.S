/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Mid-function site adapter, expanded by the builder per site. Ported from shadPS4
 * guest/custom/v1/site_adapter.S with FXSAVE instead of the YMM save (see xrg_site.h).
 * The stack alignment at a site is unknown: the frame is realigned to 16 bytes from
 * the saved rsp. Windows has no red zone; the 128 bytes skipped first keep the
 * adapter safe for code that was compiled with one anyway. The handler is payload
 * code with the compiler's default (SysV) convention: the context goes in rdi. */
.macro XRG_SITE_ADAPTER entry, handler, continuation
.text
.p2align 4
.globl \entry
.type \entry,@function
\entry:
    lea -128(%rsp), %rsp
    pushfq
    push %r15
    push %r14
    push %r13
    push %r12
    push %r11
    push %r10
    push %r9
    push %r8
    push %rbp
    push %rdi
    push %rsi
    push %rdx
    push %rcx
    push %rbx
    push %rax
    mov %rsp, %r12
    and $-16, %rsp
    sub $576, %rsp
    fxsave64 (%rsp)
    /* MXCSR stays the guest's: handler float math rounds like the code it replaces. */
    fninit
    cld
    /* XrgSiteContext at 512: rsp, gpr, fxsave, reserved */
    lea 256(%r12), %rax
    mov %rax, 512(%rsp)
    mov %r12, 520(%rsp)
    mov %rsp, 528(%rsp)
    movq $0, 536(%rsp)
    lea 512(%rsp), %rdi
    call \handler
    fxrstor64 (%rsp)
    mov %r12, %rsp
    pop %rax
    pop %rbx
    pop %rcx
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rbp
    pop %r8
    pop %r9
    pop %r10
    pop %r11
    pop %r12
    pop %r13
    pop %r14
    pop %r15
    popfq
    lea 128(%rsp), %rsp
    jmp \continuation
.size \entry, .-\entry
.endm
