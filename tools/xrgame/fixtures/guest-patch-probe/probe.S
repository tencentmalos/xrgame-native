# SPDX-License-Identifier: GPL-3.0-or-later
# Guest patch targets with known encodings (Windows x64: arguments in ecx, edx).
    .intel_syntax noprefix
    .text

# int probe_add(int a, int b): typed hook target.
    .globl probe_add
    .p2align 4
probe_add:
    sub rsp, 40
    lea eax, [rcx + rdx]
    add rsp, 40
    ret

# int probe_counted(void): its first instructions hold a call (relocated in the trampoline).
    .globl probe_counted
    .p2align 4
probe_counted:
    sub rsp, 40
    call probe_bump
    mov eax, dword ptr [rip + probe_counter]
    add rsp, 40
    ret
probe_bump:
    add dword ptr [rip + probe_counter], 1
    ret

# int probe_read(void): a RIP-relative load as its first instruction.
    .globl probe_read
    .p2align 4
probe_read:
    mov eax, dword ptr [rip + probe_value]
    nop
    ret

# int probe_before(int a): (a + 100) * 3, with a "before" site on the add.
    .globl probe_before, probe_before_site
    .p2align 4
probe_before:
    mov eax, ecx
probe_before_site:
    add eax, 100
    imul eax, eax, 3
    ret

# int probe_replace(int a): a * 65536; a "replace" site stands in for the imul.
    .globl probe_replace, probe_replace_site
    .p2align 4
probe_replace:
    mov eax, ecx
probe_replace_site:
    imul eax, eax, 65536
    ret

# int probe_constant(void): 7, changed by a code patch.
    .globl probe_constant
    .p2align 4
probe_constant:
    mov eax, 7
    ret

# int probe_triple(int a): called by the patch through a function binding.
    .globl probe_triple
    .p2align 4
probe_triple:
    lea eax, [rcx + rcx * 2]
    nop
    nop
    ret

    .data
    .globl probe_counter, probe_value
    .p2align 2
probe_counter:
    .long 0
probe_value:
    .long 5
