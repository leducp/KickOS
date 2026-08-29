/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * SCAFFOLDING: one deliberate memory access, out of line and of a KNOWN ENCODED LENGTH. The
 * reporter resumes past it by that many bytes, nothing in the page-fault frame saying how far
 * to advance.
 */

    .section .text, "ax"

/* uint64_t kickos_x86_64_probe5_load(uintptr_t addr)
 *
 * Faults when `addr` is not mapped, and the reporter steps past the load to the return, which
 * leaves the return register holding whatever was already there. */
    .balign 16
    .globl kickos_x86_64_probe5_load
kickos_x86_64_probe5_load:
    movq (%rdi), %rax
.Lload_end:
    ret

/* The reporter's skip is this figure written in the C file. */
    .if (.Lload_end - kickos_x86_64_probe5_load) != 3
    .error "the load stub is not three bytes, so the reporter's skip would land mid-instruction"
    .endif
