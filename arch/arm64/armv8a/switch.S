/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The AArch64 context switcher, and the exception frame it shares with the trap entries.
 *
 * ONE FRAME FORMAT FOR EVERY PATH: a preempted thread and a thread that switched of its own
 * accord resume through the same instructions, at the cost of the caller-saved half a
 * cooperative switch does not need. Every resume is an `eret`, EL1 to EL1 included, so the
 * frame carries ELR and SPSR rather than a saved x30 to `ret` through.
 *
 * ALL THIRTY-TWO VECTOR REGISTERS, because an asynchronous entry interrupted code that
 * called nothing, so anything the AAPCS lets a call clobber may be live in it. No FP
 * opt-in gates this: AArch64 GCC stages a wide struct copy through q registers with no
 * source-level FP in sight, so the callee-saved subset would corrupt any thread
 * interrupted mid-copy. FPCR and FPSR are thread state for the same reason.
 */

#include <kickos/board_config.h>

#define FRAME_SIZE  800
#define F_X30       240
#define F_ELR       248
#define F_SPSR      256
#define F_SP_EL0    264     /* the thread's OWN sp when it runs at EL0 */
#define F_FPCR      272
#define F_FPSR      280
#define F_FP        288     /* q0..q31, 16-byte aligned */

/* static_asserted against offsetof in arch_armv8a.cc. */
#define CTX_SP      0

/* Displacements into the per-CPU block, whose base is a link-time address at one core and
   TPIDR_EL1 above one (percpu.h). static_asserted against offsetof in arch_armv8a.cc. */
#define PERCPU_CTX_CURRENT  8
#define PERCPU_SWITCH_TO    16

/* Ends the kernel lock's span over the swap. CALL ONLY where the outgoing frame is parked and
   sp already names the incoming one: the call frame then lands on the incoming thread's own
   stack below its saved frame, and RESTORE_FRAME_AND_ERET reloads every register the call is
   free to clobber. */
    .macro  SWITCH_UNLOCK
#if KICKOS_KERNEL_CORES > 1
    bl      kickos_switch_unlock
#endif
    .endm

/* One cell of this core's block, addressed. Above one core the base is TPIDR_EL1, seated on
   each core before that core turns translation on (secondary.S, arch_armv8a.cc). */
    .macro  PERCPU_CELL reg, disp
#if KICKOS_NUM_CORES > 1
    mrs     \reg, tpidr_el1
    add     \reg, \reg, #\disp
#else
    adrp    \reg, kickos_armv8a_percpu + \disp
    add     \reg, \reg, #:lo12:kickos_armv8a_percpu + \disp
#endif
    .endm

    .macro  SAVE_FRAME
    sub     sp, sp, #FRAME_SIZE
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30, [sp, #F_X30]
    /* x16/x17, already in the frame here: x0/x1 still hold switch_now's live arguments. */
    mrs     x16, fpcr
    mrs     x17, fpsr
    stp     x16, x17, [sp, #F_FPCR]
    stp     q0, q1, [sp, #F_FP]
    stp     q2, q3, [sp, #F_FP + 32]
    stp     q4, q5, [sp, #F_FP + 64]
    stp     q6, q7, [sp, #F_FP + 96]
    stp     q8, q9, [sp, #F_FP + 128]
    stp     q10, q11, [sp, #F_FP + 160]
    stp     q12, q13, [sp, #F_FP + 192]
    stp     q14, q15, [sp, #F_FP + 224]
    stp     q16, q17, [sp, #F_FP + 256]
    stp     q18, q19, [sp, #F_FP + 288]
    stp     q20, q21, [sp, #F_FP + 320]
    stp     q22, q23, [sp, #F_FP + 352]
    stp     q24, q25, [sp, #F_FP + 384]
    stp     q26, q27, [sp, #F_FP + 416]
    stp     q28, q29, [sp, #F_FP + 448]
    stp     q30, q31, [sp, #F_FP + 480]
    .endm

/* x0 is the scratch until it is the last register loaded. */
    .macro  RESTORE_FRAME_AND_ERET
    ldr     x0, [sp, #F_SPSR]
    msr     spsr_el1, x0
    ldr     x0, [sp, #F_ELR]
    msr     elr_el1, x0
    ldr     x0, [sp, #F_SP_EL0]
    msr     sp_el0, x0
    ldp     x0, x1, [sp, #F_FPCR]
    msr     fpcr, x0
    msr     fpsr, x1
    ldp     q0, q1, [sp, #F_FP]
    ldp     q2, q3, [sp, #F_FP + 32]
    ldp     q4, q5, [sp, #F_FP + 64]
    ldp     q6, q7, [sp, #F_FP + 96]
    ldp     q8, q9, [sp, #F_FP + 128]
    ldp     q10, q11, [sp, #F_FP + 160]
    ldp     q12, q13, [sp, #F_FP + 192]
    ldp     q14, q15, [sp, #F_FP + 224]
    ldp     q16, q17, [sp, #F_FP + 256]
    ldp     q18, q19, [sp, #F_FP + 288]
    ldp     q20, q21, [sp, #F_FP + 320]
    ldp     q22, q23, [sp, #F_FP + 352]
    ldp     q24, q25, [sp, #F_FP + 384]
    ldp     q26, q27, [sp, #F_FP + 416]
    ldp     q28, q29, [sp, #F_FP + 448]
    ldp     q30, q31, [sp, #F_FP + 480]
    ldr     x30, [sp, #F_X30]
    ldp     x28, x29, [sp, #224]
    ldp     x26, x27, [sp, #208]
    ldp     x24, x25, [sp, #192]
    ldp     x22, x23, [sp, #176]
    ldp     x20, x21, [sp, #160]
    ldp     x18, x19, [sp, #144]
    ldp     x16, x17, [sp, #128]
    ldp     x14, x15, [sp, #112]
    ldp     x12, x13, [sp, #96]
    ldp     x10, x11, [sp, #80]
    ldp     x8, x9, [sp, #64]
    ldp     x6, x7, [sp, #48]
    ldp     x4, x5, [sp, #32]
    ldp     x2, x3, [sp, #16]
    ldp     x0, x1, [sp, #0]
    add     sp, sp, #FRAME_SIZE
    eret
    .endm

    .section .text.switch, "ax"

/* --- The IRQ path (vectors.S branches here) --------------------------------
 * The deferred switch below hands this exact frame to the scheduler as a suspended thread,
 * so a cheaper entry would owe a second resume path.
 */
    .global kickos_armv8a_irq_entry
kickos_armv8a_irq_entry:
    SAVE_FRAME
    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x0, x1, [sp, #F_ELR]

    bl      kickos_armv8a_irq

    /* The PendSV/msip model without the software interrupt: the interrupted thread's state
       is already in the frame below sp, so the swap is two stores and two loads. */
    PERCPU_CELL x0, PERCPU_SWITCH_TO
    ldr     x1, [x0]
    cbz     x1, .Lirq_resume
    str     xzr, [x0]                       /* consume the request */
    PERCPU_CELL x2, PERCPU_CTX_CURRENT
    ldr     x3, [x2]
    mov     x4, sp
    str     x4, [x3, #CTX_SP]               /* park the frame on the context that built it */
    str     x1, [x2]                        /* the incoming one is now the physical current */
    ldr     x4, [x1, #CTX_SP]
    mov     sp, x4                          /* stand on the incoming thread's frame */
    SWITCH_UNLOCK
.Lirq_resume:
    RESTORE_FRAME_AND_ERET

/* --- The EL0 side ---------------------------------------------------------
 * SP_EL1 ALREADY NAMES THIS THREAD'S KERNEL BLOCK, so the entry seats nothing and spends no
 * scratch register. EL0 cannot write SP_EL1, and every path that puts a thread at EL0 is an
 * eret off a frame that stood on its own block: arch_context_init builds an EL0 thread's
 * first frame there (arch_armv8a.cc), and every frame after it is one this macro pushed. The
 * frame is popped before the eret, so SP_EL1 is at the block top whenever EL0 is running.
 *
 * This is what releases TPIDR_EL1, which the per-CPU block is reached through above one core
 * (percpu.h). The reporting slots in vectors.S still seat SP_EL1 from that block, because a
 * slot this port never enters proves nothing about the register.
 *
 * The transfer is all this arch writes: no red-zone check and no validation of the
 * interrupted sp, the sp being replaced being SP_EL0 and the one stored through SP_EL1.
 */
    .macro  ENTER_FROM_EL0
    SAVE_FRAME
    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x0, x1, [sp, #F_ELR]
    mrs     x0, sp_el0
    str     x0, [sp, #F_SP_EL0]
    .endm

/* Synchronous from EL0: every such exception an EL0 thread raises, faults included, so the
 * class is decoded before anything is treated as a call. Skip it and a data abort dispatches
 * as a syscall, its faulting registers read as a call number and arguments.
 */
    .global kickos_armv8a_el0_sync_entry
kickos_armv8a_el0_sync_entry:
    ENTER_FROM_EL0
    mrs     x0, esr_el1
    lsr     x0, x0, #26                     /* ESR_EL1.EC */
    cmp     x0, #0x15                       /* SVC instruction executed in AArch64 state */
    b.ne    .Lel0_fault
    /* The psABI put nr and a0..a3 in x0..x4 at the caller and the frame holds them, so this
       reloads what the mrs pair above clobbered and nothing else. syscall_dispatch runs on
       this thread's kernel block in privileged THREAD context, as arch.h requires:
       arch_in_isr counts IRQ depth alone, so it reads false throughout. */
    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldr     x4, [sp, #32]
    bl      syscall_dispatch
    /* uint64_t returns in x0 at this width, so the arch_syscall64 pair collapses. */
    str     x0, [sp, #0]
    RESTORE_FRAME_AND_ERET

/* A fault. The frame ENTER_FROM_EL0 just built is resumable, so containment is an ordinary
 * exception return off it: kickos_fault_kill_thread rewrites ELR and SPSR when the kill rule
 * holds (arch_fault_redirect_to_exit) and the eret lands in kickos_thread_fault_exit at EL1.
 *
 * NOTHING MAY PRINT ABOVE THIS CALL. kickos_armv8a_exception enters kpanic_enter, whose
 * console reclaim is permanent, and this fault is meant to be survivable.
 *
 * The call clobbers x0-x18 and x30, all of which the frame holds and the restore reloads, so
 * the panic arm below reads x30 back out of the frame rather than trusting the register.
 */
.Lel0_fault:
    mov     x0, sp
    bl      kickos_fault_kill_thread
    cbz     w0, .Lel0_fault_panic
    RESTORE_FRAME_AND_ERET
.Lel0_fault_panic:
    mov     x0, #8                          /* the vector slot, for the dump to name */
    ldr     x1, [sp, #F_X30]
    b       kickos_armv8a_exception

/* IRQ from EL0: an unprivileged thread preempted. Same deferred switch as the EL1 slot. */
    .global kickos_armv8a_el0_irq_entry
kickos_armv8a_el0_irq_entry:
    ENTER_FROM_EL0
    bl      kickos_armv8a_irq
    PERCPU_CELL x0, PERCPU_SWITCH_TO
    ldr     x1, [x0]
    cbz     x1, .Lel0_irq_resume
    str     xzr, [x0]
    PERCPU_CELL x2, PERCPU_CTX_CURRENT
    ldr     x3, [x2]
    mov     x4, sp
    str     x4, [x3, #CTX_SP]
    str     x1, [x2]
    ldr     x4, [x1, #CTX_SP]
    mov     sp, x4
    SWITCH_UNLOCK
.Lel0_irq_resume:
    RESTORE_FRAME_AND_ERET

/* --- The user side of the trap --------------------------------------------
 * arch_syscall(nr, a0, a1, a2, a3). arch_syscall64 is the same entry, the seam's psABI pair
 * collapsing at this width.
 *
 * TWO COPIES, one per half of a split image. The ordinary names are the APP's and link
 * into the app's low text, which an EL0 thread executes and which therefore carries
 * privileged-execute-never; the kernel takes the private name, in its own half, mapped
 * through TTBR1 in every space by construction. Both references are explicit calls, so
 * the rename is a source fix and needs no objcopy pass.
 *
 * .apptrap and not .text.*: virt_arm64.ld selects the kernel half by ARCHIVE, and this
 * archive is kernel-side, so the app's copy needs a section name those selectors do not
 * match.
 */
    .section .apptrap, "ax"
    .global arch_syscall
    .global arch_syscall64
arch_syscall:
arch_syscall64:
    svc     #0
    ret

    .section .text.switch, "ax"
/* The kernel's own trap leaf (kickos/arch/arch.h). root_entry descends through it for
 * shutdown and panic, root not always being privileged. */
    .global karch_syscall
    .global karch_syscall64
karch_syscall:
karch_syscall64:
    svc     #0
    ret

/* --- arch_switch's immediate half -----------------------------------------
 * void kickos_armv8a_switch_now(struct arch_context* from, struct arch_context* to)
 * Thread context, so there is no exception frame to inherit and this one is synthesised:
 * ELR is the return address, SPSR the state to resume with.
 */
    .global kickos_armv8a_switch_now
kickos_armv8a_switch_now:
    SAVE_FRAME
    str     x30, [sp, #F_ELR]               /* resume where our caller expects to continue */
    mrs     x2, daif                        /* keep the caller's interrupt state exactly */
    mov     x3, #0x5                        /* M[3:0] = EL1h: same level, its own SP.
                                               Not an orr immediate: 0b101 does not repeat,
                                               and only repeating patterns encode. */
    orr     x2, x2, x3
    str     x2, [sp, #F_SPSR]
    mov     x2, sp
    str     x2, [x0, #CTX_SP]
    ldr     x2, [x1, #CTX_SP]
    mov     sp, x2
    SWITCH_UNLOCK
    RESTORE_FRAME_AND_ERET

/* --- arch_start -----------------------------------------------------------
 * void kickos_armv8a_start(struct arch_context* first)
 * The boot context is abandoned, as arch.h permits: the system ends through arch_shutdown
 * and never by unwinding to boot.
 */
    .global kickos_armv8a_start
kickos_armv8a_start:
    ldr     x1, [x0, #CTX_SP]
    mov     sp, x1
    RESTORE_FRAME_AND_ERET

/* Seated in the frame's x30 by arch_context_init. */
    .global kickos_armv8a_thread_exit
kickos_armv8a_thread_exit:
    b       kickos_thread_return

    .end
