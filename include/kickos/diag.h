// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The diagnostic catalogue: every kernel and arch message in both of its forms, prose
// and short code, on one line. A code cannot be added without the sentence it stands
// for, and a reader decodes a terse image by grepping the code in this file.
//
// KICKOS_DIAG_TERSE picks the column. It is off everywhere but the 64 KiB parts, which
// set it in their board defconfig.
//
// The FAULT BANNERS tests/lib/panic.ere matches are NOT entries here. Four of the five
// ("KERNEL PANIC: ", "=== HARD FAULT", "=== RISC-V TRAP", "ISOLATION FAULT:") are raw
// literals at their emit sites and never reach KICKOS_DIAG_PICK, so no terse variant of
// them exists to go looking for; the fifth, "MPU FAULT: task", is the fixed prefix of
// KDIAG_F_MPU_FAULT and is spelled identically in both its columns.
//
// The same rule binds every token a test PARSES rather than merely detects: "CFSR=0x",
// "MMFAR=0x", "BFAR=0x", "ADDR=0x", "mstatus=0x", "attempted <dir> at 0x", and
// "=== THREAD FAULT === task '<name>' killed" (tests/lib/gate.sh, check_faultsurvive.sh,
// check_qemu_ringppb.sh). Those DO have entries below, and both columns carry the token
// verbatim so this posture stays safe to select on ANY board, not only on the two that
// carry no such gate today.

#ifndef KICKOS_DIAG_H
#define KICKOS_DIAG_H

#if defined(__has_include) && __has_include(<kickos/board_config.h>)
#include <kickos/board_config.h>
#endif

#ifndef KICKOS_DIAG_TERSE
#define KICKOS_DIAG_TERSE 0
#endif

#if KICKOS_DIAG_TERSE
#define KICKOS_DIAG_PICK(full, terse) terse
#else
#define KICKOS_DIAG_PICK(full, terse) full
#endif

// Panic and diagnostic prose. Reached as ::kickos::diag::<name>.
// The short column is the grep key: P<nn> is unique to one row and to one message.
#define KICKOS_DIAG_MSG_TABLE(X)                                                                  \
    X(kBootIdleStack,   "kmain: no arena for the idle stack",                        "P01")       \
    X(kBootRootStack,   "kmain: no arena for the root stack",                        "P02")       \
    X(kBootRootRun,     "kmain: no capability run for root",                         "P03")       \
    X(kConsoleAttach,   "console_buffer_init: irq_attach failed",                    "P04")       \
    X(kBlockInIsr,      "kickos: blocking operation from ISR context",               "P05")       \
    X(kResumeNoSwitch,  "wq_confirm_resume: switch never landed",                    "P06")       \
    X(kDeadlineNoTimer, "expired deadline on a park that cannot time out",           "P07")       \
    X(kUserPanicNoMsg,  "user panic (no readable message)",                          "P08")       \
    X(kPublishNoDrain,  "console_publish: chip-writer drain did not converge",       "P09")       \
    X(kRootExitRefused, "root: exit shutdown refused",                               "P10")       \
    X(kPastExitCurrent, "unreachable: thread continued past exit_current",           "P11")       \
    X(kTimeoutNotEp,    "unreachable: endpoint_wait_timeout on a non-endpoint park", "P12")       \
    X(kRebootRp2040,    "arch_reboot: rp2040 _reset_to_usb_boot returned",           "P13")       \
    X(kRebootRp2350,    "arch_reboot: rp2350 bootrom reboot returned",               "P14")       \
    X(kRebootImxrt,     "arch_reboot: imxrt1062 bkpt resumed (no MKL02?)",           "P15")       \
    X(kBannerRule,      "  ==============================================\n",        "\n")

namespace kickos
{
    namespace diag
    {
// inline, so the one definition is shared; a message no linked object names is emitted
// by nobody and costs nothing, which is what keeps the catalogue free where it is off.
#define KICKOS_DIAG_DEFINE(name, full, terse) inline constexpr char const name[] = KICKOS_DIAG_PICK(full, terse);
        KICKOS_DIAG_MSG_TABLE(KICKOS_DIAG_DEFINE)
#undef KICKOS_DIAG_DEFINE
    }
}

// Format strings stay macros rather than joining the table above: format(printf) checks
// the argument list only against a literal AT THE CALL SITE, so a constexpr array would
// silently retire -Wformat on every one of these.
// The terse column keeps the arity and every conversion specifier; only prose is cut.

// Boot banner.
#define KDIAG_F_BANNER_NAME  KICKOS_DIAG_PICK("   KickOS %s  -  microkernel RTOS\n", "K %s\n")
#define KDIAG_F_BANNER_BOARD KICKOS_DIAG_PICK("   board   %s\n", "b %s\n")
#define KDIAG_F_BANNER_ARCH  KICKOS_DIAG_PICK("   arch    %s\n", "a %s\n")
#define KDIAG_F_BANNER_MPU   KICKOS_DIAG_PICK("   mpu     %s\n", "m %s\n")
#define KDIAG_F_BANNER_SCHED KICKOS_DIAG_PICK("   sched   %s\n", "s %s\n")
#define KDIAG_F_BANNER_BUILD KICKOS_DIAG_PICK("   build   %s\n", "t %s\n")
#define KDIAG_F_BANNER_APP   KICKOS_DIAG_PICK("   app     %s\n", "p %s\n")
#define KDIAG_F_BANNER_COMMIT KICKOS_DIAG_PICK("   commit  %s\n", "c %s\n")
#define KDIAG_F_BANNER_HEAP  KICKOS_DIAG_PICK("   heap    %u KiB available\n", "h %u\n")
#define KDIAG_F_BANNER_NOHEAP KICKOS_DIAG_PICK("   heap    none\n", "h 0\n")

// Thread fault (kernel/init/fault.cc).
#define KDIAG_F_THREAD_FAULT KICKOS_DIAG_PICK("\n=== THREAD FAULT === task '%s' killed, system continues\n", \
                                              "\n=== THREAD FAULT === task '%s' killed\n")
#define KDIAG_F_FAULT_PC_LOST KICKOS_DIAG_PICK("  PC lost to a later fault\n", "F1\n")
#define KDIAG_F_FAULT_PC      KICKOS_DIAG_PICK("  PC=%p\n", "F2 %p\n")
#define KDIAG_F_FAULT_PC_STAT KICKOS_DIAG_PICK("  PC=%p %s=0x%x\n", "F3 %p %s %x\n")
#define KDIAG_F_FAULT_ADDR    KICKOS_DIAG_PICK("  ADDR=%p\n", "ADDR=%p\n")

// MPU fault report (kernel/init/console.cc).
#define KDIAG_F_MPU_FAULT KICKOS_DIAG_PICK("\nMPU FAULT: task '%s' attempted %s at %p -- reported\n", \
                                           "\nMPU FAULT: task '%s' attempted %s at %p\n")

// ARM fault dumps. The "=== ... ===" banner line is emitted separately, not from here.
#define KDIAG_F_ARM_REGS1 KICKOS_DIAG_PICK("  PC=0x%x LR=0x%x xPSR=0x%x (%s)\n", "R1 %x %x %x %s\n")
#define KDIAG_F_ARM_REGS2 KICKOS_DIAG_PICK("  R0=0x%x R1=0x%x R2=0x%x R3=0x%x R12=0x%x\n", \
                                           "R2 %x %x %x %x %x\n")
#define KDIAG_F_ARM_CFSR  KICKOS_DIAG_PICK("  CFSR=0x%x HFSR=0x%x\n", "CFSR=0x%x H=%x\n")
#define KDIAG_F_ARM_MMFAR KICKOS_DIAG_PICK("  MMFAR=0x%x\n", "MMFAR=0x%x\n")
#define KDIAG_F_ARM_BFAR  KICKOS_DIAG_PICK("  BFAR=0x%x\n", "BFAR=0x%x\n")
#define KDIAG_F_ARM_IMPRECISE KICKOS_DIAG_PICK( \
    "  (imprecise bus fault: PC/regs are post-fault, not the culprit)\n", "R6\n")

// RISC-V trap dump.
#define KDIAG_F_RV_CAUSE  KICKOS_DIAG_PICK("  mcause=0x%x mepc=0x%x\n", "V1 %x %x\n")
#define KDIAG_F_RV_STATUS KICKOS_DIAG_PICK("  mtval=0x%x mstatus=0x%x\n", "V2 %x mstatus=0x%x\n")

#endif
