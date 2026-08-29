/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The RV64IMAC save-frame geometry, read by BOTH switch.S and arch_rv64imac.cc so the
 * fabricated frame and the one the trap prologue builds are one object.
 *
 * DO NOT RENAME THIS TO A `_trap_stack.h` SUFFIX. tests/static/check_trap_redzone_decls.sh globs
 * that suffix under arch/ to decide which arches it holds to a declaration in
 * tests/static/trap_redzone_roots.txt, and no depth figure here has been measured.
 */

#ifndef KICKOS_ARCH_RV64_FRAME_H
#define KICKOS_ARCH_RV64_FRAME_H

/* Byte offsets from the frame base (ctx.sp), low to high. gp and tp stay OUT of the frame: both
 * are U-mode writable, and the entry re-anchors both instead, at .Ltrap_regs and at .Lrestore.
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

/* The sp the restore epilogue leaves on, so sret resumes the thread there. Four places write it
 * and nothing else may: the entry's three frame builders and arch_context_init. A fabricated
 * frame that leaves it zero resumes on a null sp at the first sret.
 *
 * 248 is spare and is where the prologue stashes t0 before it knows which stack the frame goes
 * on.
 */
#define KICKOS_RV64_F_SP       240
#define KICKOS_RV64_FRAME      256

/* Alignment the RISC-V psABI keeps sp at. FRAME is a multiple of it and arch_context_init aligns
 * every stack top down to it.
 */
#define KICKOS_RV64_SP_ALIGN    16

/* sstatus bits (RISC-V Privileged ISA). SPP is ONE bit at 8, where the machine-mode MPP is two
 * at 11.
 */
#define KICKOS_RV64_SSTATUS_SIE   0x00000002
#define KICKOS_RV64_SSTATUS_SPIE  0x00000020
#define KICKOS_RV64_SSTATUS_SPP   0x00000100
#define KICKOS_RV64_SSTATUS_SUM   0x00040000

/* The one exception cause the entry demuxes by value; an interrupt is demuxed by the sign bit
 * and everything else reaches the reporter.
 */
#define KICKOS_RV64_CAUSE_ECALL_U   8

/* The trusted per-hart trap stack (g_rv64_trap_stack, arch_rv64imac.cc). sscratch holds its top
 * while a thread runs, so the entry swaps onto it before it touches the interrupted sp.
 *
 * THE DEPTH IS PROVISIONAL: nothing has run -fcallgraph-info on this arch and this port ships no
 * record in tests/static/trap_redzone_roots.txt, so it is not a measured figure.
 */
#define KICKOS_RV64_TRAP_NESTED_DEPTH 3840
#define KICKOS_RV64_TRAP_STACK_SIZE \
    (KICKOS_RV64_FRAME + KICKOS_RV64_TRAP_NESTED_DEPTH)

/* The doubleword at the LOW end of each trap-stack row, written by kickos_rv64_init and read by
 * switch.S. It is what tells an S-mode exception outside the row from a descent that ran off the
 * row's bottom, which the interrupted sp cannot carry. Read once per S-mode exception.
 */
#define KICKOS_RV64_TRAP_CANARY 0x54524150

/* struct arch_context field offsets the entry reads as plain doublewords. switch.S .equ's from
 * these and arch_rv64imac.cc static_asserts offsetof against them, so a field inserted ahead of
 * them breaks the build.
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
