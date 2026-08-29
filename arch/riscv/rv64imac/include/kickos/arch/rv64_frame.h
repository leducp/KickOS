/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The RV64IMAC save-frame geometry, read by BOTH switch.S and arch_rv64imac.cc so the
 * fabricated frame and the one the trap prologue builds are one object.
 *
 * NOT NAMED WITH THE _trap_stack.h SUFFIX ON PURPOSE.
 * tests/static/check_trap_redzone_decls.sh globs that suffix under arch/ to discover which
 * arches it holds to a full declaration in tests/static/trap_redzone_roots.txt, and no depth
 * figure here has been measured under -fcallgraph-info. A header under that name would enrol
 * this arch on figures copied from a 32-bit port.
 *
 * WHICH STACK A FRAME GOES ON, three destinations and no fourth. A U-mode entry puts the
 * frame on the interrupted thread's own kernel block (ctx.kernel_sp), so the sp the thread
 * chose is never written through. An INTERRUPT from S-mode keeps the interrupted sp, that being
 * a privileged thread's own stack or the kernel block a dispatch was already descending, and it
 * is a frame that may BECOME that thread's saved context through a switch booked from ISR
 * context. Every other S-mode trap pops its frame and srets, so its frame and the C below it go
 * on the trusted per-hart trap stack. The frame's F_SP slot carries the sp the restore epilogue
 * leaves on, which is what lets one epilogue serve all three.
 *
 * AND ONE CASE THAT GETS NO FRAME AT ALL: an S-mode trap whose interrupted sp is already INSIDE
 * the trap stack. That is a fault in the dispatch or the reporter, or a descent that ran off the
 * row, and a fourth frame would be the second of an unbounded series. switch.S's .Ltrap_reentry
 * reports it and stops the machine instead.
 */

#ifndef KICKOS_ARCH_RV64_FRAME_H
#define KICKOS_ARCH_RV64_FRAME_H

/* Byte offsets from the frame base (ctx.sp), low to high. gp and tp stay OUT of the frame:
 * both are U-mode writable, so a saved copy would be a value the outgoing thread chose, and the
 * entry re-anchors BOTH instead, at .Ltrap_regs for the kernel C it calls and at .Lrestore for
 * the resume. gp comes from the link-time __global_pointer$; tp is forced to zero, which is its
 * canonical value while this arch selects no ARCH_HAS_TLS and nothing reads it. A step that
 * seats a thread pointer per thread must re-seat it FROM THE CONTEXT at those same two points,
 * under #if KICKOS_TLS, which is also where ctx.tls_base becomes readable.
 */
#define KICKOS_RV64_F_SEPC       0
#define KICKOS_RV64_F_SSTATUS    8
#define KICKOS_RV64_F_RA        16
#define KICKOS_RV64_F_T0        24
#define KICKOS_RV64_F_T1        32
#define KICKOS_RV64_F_T2        40
#define KICKOS_RV64_F_S0        48
#define KICKOS_RV64_F_S1        56
#define KICKOS_RV64_F_A0        64
#define KICKOS_RV64_F_A1        72
#define KICKOS_RV64_F_A2        80
#define KICKOS_RV64_F_A3        88
#define KICKOS_RV64_F_A4        96
#define KICKOS_RV64_F_A5       104
#define KICKOS_RV64_F_A6       112
#define KICKOS_RV64_F_A7       120
#define KICKOS_RV64_F_S2       128
#define KICKOS_RV64_F_S3       136
#define KICKOS_RV64_F_S4       144
#define KICKOS_RV64_F_S5       152
#define KICKOS_RV64_F_S6       160
#define KICKOS_RV64_F_S7       168
#define KICKOS_RV64_F_S8       176
#define KICKOS_RV64_F_S9       184
#define KICKOS_RV64_F_S10      192
#define KICKOS_RV64_F_S11      200
#define KICKOS_RV64_F_T3       208
#define KICKOS_RV64_F_T4       216
#define KICKOS_RV64_F_T5       224
#define KICKOS_RV64_F_T6       232

/* The sp the restore epilogue leaves on, so sret resumes the thread there. FOUR places
 * write it and nothing else may: the entry's three frame builders and arch_context_init. A
 * fabricated frame that leaves it zero resumes on a null sp at the first sret.
 *
 * The 30 saved words reach 232, so this sits in the space the 16-byte-aligned frame size
 * already had spare; 248 stays unused and is where the prologue stashes t0 before it knows
 * which stack the frame goes on.
 */
#define KICKOS_RV64_F_SP       240
#define KICKOS_RV64_FRAME      256

/* Alignment the RISC-V psABI keeps sp at. FRAME is a multiple of it and arch_context_init
 * aligns every stack top down to it, so every frame base the entry picks is aligned.
 */
#define KICKOS_RV64_SP_ALIGN    16

/* sstatus bits (RISC-V Privileged ISA). SPP is ONE bit at 8, where the machine-mode MPP is
 * two at 11. SUM would let S-mode load and store the pages a space marks U-accessible; NOTHING
 * SETS IT, the kernel reaching memory a process owns through the kaccess seam instead
 * (kickos/aspace.h). The name is kept because .Lrestore writes sstatus whole from the frame, so
 * a step that does need a window has to mask the bit there.
 */
#define KICKOS_RV64_SSTATUS_SIE   0x00000002
#define KICKOS_RV64_SSTATUS_SPIE  0x00000020
#define KICKOS_RV64_SSTATUS_SPP   0x00000100
#define KICKOS_RV64_SSTATUS_SUM   0x00040000

/* The one exception cause the entry demuxes by value; every INTERRUPT cause is demuxed by the
 * sign bit instead and goes to the ISR leg. Everything else reaches the reporter.
 *
 * ECALL-FROM-S (cause 9) HAS NO SPELLING HERE ON PURPOSE. startup.S leaves medeleg bit 9 clear,
 * so the cause never arrives at stvec at all: it is a machine-mode refusal there, and a name
 * for it in this entry would assert a capability the port no longer has.
 */
#define KICKOS_RV64_CAUSE_ECALL_U   8

/* The trusted per-hart trap stack (g_rv64_trap_stack, arch_rv64imac.cc). sscratch holds its
 * top while a thread runs, so the entry swaps onto it before it touches the interrupted sp.
 *
 * PROVISIONAL, and flagged rather than left to read as measured: the depth is the fault
 * reporter's C descent, nothing has run -fcallgraph-info on this arch, and this port ships
 * no record in tests/static/trap_redzone_roots.txt to measure it against. It is set at twice
 * the 32-bit port's enforced figure, that port's pointers and frame slots being half as
 * wide.
 */
#define KICKOS_RV64_TRAP_NESTED_DEPTH 3840
#define KICKOS_RV64_TRAP_STACK_SIZE \
    (KICKOS_RV64_FRAME + KICKOS_RV64_TRAP_NESTED_DEPTH)

/* The doubleword at the LOW end of each row, written by kickos_rv64_init and read by
 * switch.S's .Ltrap_reentry. It does not stop an overflow and is not checked per trap: what it
 * buys is that the terminal report names WHICH of the two happened, a fault inside the reporter
 * or a descent that ran off the row, since a store past the row's bottom lands in ordinary .bss
 * and takes no trap of its own. Eight bytes off a depth that is provisional anyway.
 */
#define KICKOS_RV64_TRAP_CANARY 0x54524150

/* struct arch_context field offsets the entry reads as plain doublewords. switch.S .equ's
 * from these and arch_rv64imac.cc static_asserts offsetof against them, so a field inserted
 * ahead of them breaks the build rather than leaving the entry to read trace_tid as
 * stack_lo.
 */
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
#define KICKOS_RV64_CTX_OFF_SP 0
#define KICKOS_RV64_CTX_OFF_TRACE_TID 8
#define KICKOS_RV64_CTX_OFF_STACK_LO 16
#define KICKOS_RV64_CTX_OFF_STACK_HI 24
#if defined(KICKOS_TLS) && KICKOS_TLS
#define KICKOS_RV64_CTX_OFF_TLS_BASE 32
#define KICKOS_RV64_CTX_OFF_KERNEL_SP 40
#else
#define KICKOS_RV64_CTX_OFF_KERNEL_SP 32
#endif
#else
#define KICKOS_RV64_CTX_OFF_SP 0
#define KICKOS_RV64_CTX_OFF_STACK_LO 8
#define KICKOS_RV64_CTX_OFF_STACK_HI 16
#if defined(KICKOS_TLS) && KICKOS_TLS
#define KICKOS_RV64_CTX_OFF_TLS_BASE 24
#define KICKOS_RV64_CTX_OFF_KERNEL_SP 32
#else
#define KICKOS_RV64_CTX_OFF_KERNEL_SP 24
#endif
#endif

#endif /* KICKOS_ARCH_RV64_FRAME_H */
