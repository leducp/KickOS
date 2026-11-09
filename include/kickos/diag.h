// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The diagnostic catalogue: every kernel and arch message.
//
// The FAULT BANNERS tests/lib/panic.ere matches are raw kprintf literals at their emit sites. Do
// not restate that set here: tests/static/check_panic_banners.sh derives it from the emit sites
// and is the only place it is written down. Three entries below bear on it: KDIAG_F_MPU_FAULT's
// fixed prefix IS matched, and KDIAG_F_THREAD_FAULT and KDIAG_F_TASK_PANIC deliberately are not,
// gates asserting them present while asserting no panic occurred.

#ifndef KICKOS_DIAG_H
#define KICKOS_DIAG_H

#if defined(__has_include) && __has_include(<kickos/board_config.h>)
#include <kickos/board_config.h>
#endif

// Panic and diagnostic prose, reached as ::kickos::diag::<name>. kickos/console_tx.h includes
// this header from a C-facing one to reach KICKOS_DIAG_LINE_MAX, so the block is guarded rather
// than the whole file being C++ only (tests/static/check_public_headers.sh).
#ifdef __cplusplus
namespace kickos
{
    namespace diag
    {
        inline constexpr char const kBootIdleStack[] = "kmain: no arena for the idle stack";
        inline constexpr char const kBootRootStack[] = "kmain: no arena for the root stack";
        inline constexpr char const kBootRootRun[] = "kmain: no capability run for root";
        inline constexpr char const kConsoleAttach[] = "console_buffer_init: irq_attach failed";
        inline constexpr char const kBlockInIsr[] = "kickos: blocking operation from ISR context";
        inline constexpr char const kResumeNoSwitch[] = "wq_confirm_resume: switch never landed";
        inline constexpr char const kUserPanicNoMsg[] = "user panic (no readable message)";
        inline constexpr char const kRootExitRefused[] = "root: exit shutdown refused";
        inline constexpr char const kPastExitCurrent[] =
            "unreachable: thread continued past exit_current";
        inline constexpr char const kRebootRp2040[] =
            "arch_reboot: rp2040 _reset_to_usb_boot returned";
        inline constexpr char const kRebootRp2350[] = "arch_reboot: rp2350 bootrom reboot returned";
        inline constexpr char const kRebootImxrt[] =
            "arch_reboot: imxrt1062 bkpt resumed (no MKL02?)";
        inline constexpr char const kResetNotTaken[] =
            "arch_reboot: the reset was requested and did not take";
        inline constexpr char const kParkNoKind[] =
            "unreachable: park abort on a thread with no wait edge";
        inline constexpr char const kKstackOverflow[] =
            "kernel stack canary broken on this thread's slot";
        inline constexpr char const kBootFramePool[] =
            "kmain: the frame carve is too small to describe";
        inline constexpr char const kBootAmpPort[] =
            "kmain: a port CONFIG_KICKOS_AMP_PORTS names could not be seated \n"
            "       into root; this node could not serve or reach it";
        inline constexpr char const kBootAmpSlot[] =
            "kmain: a partition port landed off its derived capability slot \n"
            "       (a dynamic install into root ran before the partition's)";
        inline constexpr char const kBootAmpShare[] =
            "kmain: the partition's user share could not be seated into root \n"
            "       (KICKOS_AMP_USER_SHARE_SIZE not whole granules?)";
        inline constexpr char const kDcacheWraps[] =
            "arch_dcache: a maintenance range wraps the address space";
        inline constexpr char const kBootRegionsOverlap[] =
            "kmain: idle's or root's regions overlap where the MPU decides \n"
            "       otherwise (mpu_overlap_expressible)";
        inline constexpr char const kMmuNoGranule[] = "armv8a: the MMU offers no 4 KiB granule";
        inline constexpr char const kMmuPaNarrow[] =
            "armv8a: the MMU's physical range is narrower than TCR_EL1.IPS";
        inline constexpr char const kUguardMixed[] =
            "armv8a: a core disagrees with the boot core on PAN";
        inline constexpr char const kUguardNotTaken[] =
            "armv8a: PSTATE.PAN or SCTLR_EL1.SPAN did not take the write";
        inline constexpr char const kUguardNoAsid[] =
            "armv8a: no PAN and no ASID, so EL1 cannot be kept out of user pages";
        inline constexpr char const kSumNotCleared[] =
            "rv64imac: sstatus.SUM did not take the clear";
        inline constexpr char const kWordNoRecord[] =
            "unreachable: a word waiter found every record in use";
        inline constexpr char const kPmpCodeWindow[] =
            "rv32imac: the PMP did not take the user code window";
        inline constexpr char const kBannerRule[] =
            "  ==============================================\n";
    }
}
#endif

// These stay macros: format(printf) checks the argument list only against a literal AT THE CALL
// SITE, so a constexpr array would silently retire -Wformat on every one of them.

// Boot banner.
#define KDIAG_F_BANNER_NAME  "   KickOS %s  -  microkernel RTOS\n"
#define KDIAG_F_BANNER_BOARD "   board   %s\n"
#define KDIAG_F_BANNER_ARCH  "   arch    %s\n"
#define KDIAG_F_BANNER_CPU   "   cpu     %s\n"
#define KDIAG_F_BANNER_MPU   "   mpu     %s\n"
#define KDIAG_F_BANNER_MMU   "   mmu     enforce\n"
#define KDIAG_F_BANNER_UGUARD "   uaccess %s\n"
#define KDIAG_F_BANNER_WITNESS "   witness uaccess\n"
#define KDIAG_F_BANNER_WITNESS_REWIND "   witness rewind\n"
#define KDIAG_F_BANNER_WITNESS_NODE "   witness node\n"
#define KDIAG_F_BANNER_SCHED "   sched   %s\n"
#define KDIAG_F_BANNER_BUILD "   build   %s\n"
#define KDIAG_F_BANNER_APP   "   app     %s\n"
#define KDIAG_F_BANNER_COMMIT "   commit  %s\n"
#define KDIAG_F_BANNER_HEAP  "   heap    %u KiB available\n"
#define KDIAG_F_BANNER_NOHEAP "   heap    none\n"
#define KDIAG_F_BANNER_KSTACK "   kstack  %u B x %u = %u B\n"

// The ordinary kprintf line buffer, and what the console TX ring is sized from
// (kickos/console_tx.h). A line past it truncates.
#ifndef KICKOS_DIAG_LINE_MAX
#define KICKOS_DIAG_LINE_MAX 256
#endif
// The bytes of a user console write the kernel takes as one whole insert: a write no longer than
// this reaches the wire unbroken beside any other writer's. The 128 caps the syscall frame that
// holds a chunk on the arches no red-zone class walks.
#if KICKOS_DIAG_LINE_MAX - 1 < 128
#define KICKOS_KCONSOLE_CHUNK (KICKOS_DIAG_LINE_MAX - 1)
#else
#define KICKOS_KCONSOLE_CHUNK 128
#endif

// Thread fault (kernel/init/fault.cc). The reporter formats into a stack array of exactly
// KDIAG_FAULT_LINE_MAX bytes, on a descent the trap red-zone gate measures as EXITK (and as
// EXIT where no kernel block is carved), so this bound is a stack figure. Measured on
// qemu-riscv by moving it: 96 -> 256 takes EXITK from 384 to 528. tests/unit/faultline proves
// every record below fits at its worst case.
#define KDIAG_FAULT_LINE_MAX 96
// The room a held fault record reserves: its banner, a PC line and an ADDR line.
#define KDIAG_FAULT_RECORD_MAX (3 * (KDIAG_FAULT_LINE_MAX - 1))
#define KDIAG_F_THREAD_FAULT "\n=== THREAD FAULT === thread '%s' killed, system continues\n"
#define KDIAG_F_FAULT_PC_LOST "  PC lost to a later fault\n"
#define KDIAG_F_FAULT_PC      "  PC=%p\n"
#define KDIAG_F_FAULT_PC_STAT "  PC=%p %s=0x%llx\n"
#define KDIAG_F_FAULT_NO_PC   "  PC unknown %s=0x%llx\n"
#define KDIAG_F_FAULT_ADDR    "  ADDR=%p\n"
// A kos_panic that ends only its caller's task (kernel/syscall/syscall.cc), on the same record
// path and bound. Not a fault banner: the system runs on.
#define KDIAG_F_TASK_PANIC     "\n=== TASK PANIC === thread '%s' ended its task\n"
#define KDIAG_F_TASK_PANIC_MSG "  %s\n"

// MPU fault report (kernel/init/console.cc).
#define KDIAG_F_MPU_FAULT "\nMPU FAULT: thread '%s' attempted %s at %p, reported\n"
#define KDIAG_F_MPU_FAULT_STACK "  its stack %p-%p\n"

// ARM fault dumps.
#define KDIAG_F_ARM_REGS1 "  PC=0x%x LR=0x%x xPSR=0x%x (%s)\n"
#define KDIAG_F_ARM_REGS2 "  R0=0x%x R1=0x%x R2=0x%x R3=0x%x R12=0x%x\n"
#define KDIAG_F_ARM_NOFRAME "  frame not read: its stacking or unstacking at 0x%x (%s) faulted\n"
#define KDIAG_F_ARM_UNVERIFIED \
    "  frame at its stack's limit 0x%x (%s): an entry that violated it stacked nothing, so the " \
    "words below are unverified\n"
#define KDIAG_F_ARM_FRAME_FOREIGN \
    "  frame not read: 0x%x (%s) is not on MSP or a stack this thread owns\n"
#define KDIAG_F_ARM_CFSR  "  CFSR=0x%x HFSR=0x%x\n"
#define KDIAG_F_ARM_DIV0  "  DIVBYZERO: the UDF of __aeabi_idiv0/__aeabi_ldiv0\n"
#define KDIAG_F_ARM_MMFAR "  MMFAR=0x%x\n"
#define KDIAG_F_ARM_BFAR  "  BFAR=0x%x\n"
#define KDIAG_F_ARM_IMPRECISE "  (imprecise bus fault: PC/regs are post-fault, not the culprit)\n"

// RISC-V trap dump.
#define KDIAG_F_RV_CAUSE  "  mcause=0x%x mepc=0x%x\n"
#define KDIAG_F_RV_STATUS "  mtval=0x%x mstatus=0x%x\n"

// RISC-V supervisor trap dump (rv64imac). The conversions are %lx: at XLEN 64 a CSR does not fit
// an int.
#define KDIAG_F_RV64_CAUSE  "  scause=0x%lx sepc=0x%lx\n"
#define KDIAG_F_RV64_STATUS "  stval=0x%lx sstatus=0x%lx\n"
#define KDIAG_F_RV64_FRAME  "  SP=0x%lx RA=0x%lx\n"
#define KDIAG_F_RV64_FROM   "  taken from %s-mode\n"

// AArch64 EL1 exception dump. The conversions are %lx: a register does not fit an int.
#define KDIAG_F_A64_VECTOR "  vector=%s LR=0x%lx\n"
#define KDIAG_F_A64_SYND   "  ESR=0x%lx ELR=0x%lx\n"
#define KDIAG_F_A64_SPSR   "  SPSR=0x%lx\n"
#define KDIAG_F_A64_FAR    "  FAR=0x%lx\n"
#define KDIAG_F_A64_FAR_NA "  FAR not valid for this exception class\n"

#endif
