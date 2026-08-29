// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The diagnostic catalogue: every kernel and arch message in both of its forms, prose and short
// code. KICKOS_DIAG_TERSE picks the column.
//
// The FAULT BANNERS tests/lib/panic.ere matches are raw kprintf literals at their emit sites and
// have no terse variant. Do not restate that set here: tests/static/check_panic_banners.sh
// derives it from the emit sites and is the only place it is written down. Two entries below
// bear on it: KDIAG_F_MPU_FAULT's fixed prefix IS matched and is spelled identically in both
// columns, and KDIAG_F_THREAD_FAULT deliberately is not, gates asserting it is present while
// asserting no panic occurred.

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

// Panic and diagnostic prose. Reached as ::kickos::diag::<name>. The short column is the grep
// key: P<nn> is unique to one row and to one message.
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
    X(kTimeoutNotEp,    "unreachable: endpoint_wait_abort on a non-endpoint park",   "P12")       \
    X(kRebootRp2040,    "arch_reboot: rp2040 _reset_to_usb_boot returned",           "P13")       \
    X(kRebootRp2350,    "arch_reboot: rp2350 bootrom reboot returned",               "P14")       \
    X(kRebootImxrt,     "arch_reboot: imxrt1062 bkpt resumed (no MKL02?)",           "P15")       \
    X(kParkNoKind,      "unreachable: park abort on a thread with no wait edge",     "P16")       \
    X(kKstackOverflow,  "kernel stack canary broken on this thread's slot",          "P17")       \
    X(kBootFramePool,   "kmain: the frame carve is too small to describe",           "P18")       \
    X(kBannerRule,      "  ==============================================\n",        "\n")

namespace kickos
{
    namespace diag
    {
#define KICKOS_DIAG_DEFINE(name, full, terse) inline constexpr char const name[] = KICKOS_DIAG_PICK(full, terse);
        KICKOS_DIAG_MSG_TABLE(KICKOS_DIAG_DEFINE)
#undef KICKOS_DIAG_DEFINE
    }
}

// These stay macros: format(printf) checks the argument list only against a literal AT THE CALL
// SITE, so a constexpr array would silently retire -Wformat on every one of them. The terse
// column keeps the arity and every conversion specifier; only prose is cut.

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
#define KDIAG_F_BANNER_KSTACK KICKOS_DIAG_PICK("   kstack  %u B x %u = %u B\n", "k %u %u %u\n")

// Thread fault (kernel/init/fault.cc).
#define KDIAG_F_THREAD_FAULT KICKOS_DIAG_PICK("\n=== THREAD FAULT === thread '%s' killed, system continues\n", \
                                              "\n=== THREAD FAULT === thread '%s' killed\n")
#define KDIAG_F_FAULT_PC_LOST KICKOS_DIAG_PICK("  PC lost to a later fault\n", "F1\n")
#define KDIAG_F_FAULT_PC      KICKOS_DIAG_PICK("  PC=%p\n", "F2 %p\n")
#define KDIAG_F_FAULT_PC_STAT KICKOS_DIAG_PICK("  PC=%p %s=0x%llx\n", "F3 %p %s %llx\n")
#define KDIAG_F_FAULT_ADDR    KICKOS_DIAG_PICK("  ADDR=%p\n", "ADDR=%p\n")
#define KDIAG_F_FAULT_STUB_DEEP KICKOS_DIAG_PICK("  STUB DEEP: %u bytes below the stack top\n", \
                                                "F4 %u\n")

// MPU fault report (kernel/init/console.cc).
#define KDIAG_F_MPU_FAULT KICKOS_DIAG_PICK("\nMPU FAULT: thread '%s' attempted %s at %p, reported\n", \
                                           "\nMPU FAULT: thread '%s' attempted %s at %p\n")
#define KDIAG_F_MPU_FAULT_STACK KICKOS_DIAG_PICK("  its stack %p-%p\n", "st %p %p\n")

// ARM fault dumps.
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

// RISC-V supervisor trap dump (rv64imac). The conversions are %lx: at XLEN 64 a CSR does not fit
// an int.
#define KDIAG_F_RV64_CAUSE  KICKOS_DIAG_PICK("  scause=0x%lx sepc=0x%lx\n", "W1 %lx %lx\n")
#define KDIAG_F_RV64_STATUS KICKOS_DIAG_PICK("  stval=0x%lx sstatus=0x%lx\n", "W2 %lx %lx\n")
#define KDIAG_F_RV64_FRAME  KICKOS_DIAG_PICK("  SP=0x%lx RA=0x%lx\n", "W3 %lx %lx\n")
#define KDIAG_F_RV64_FROM   KICKOS_DIAG_PICK("  taken from %s-mode\n", "W4 %s\n")

// AArch64 EL1 exception dump. The conversions are %lx: a register does not fit an int.
#define KDIAG_F_A64_VECTOR KICKOS_DIAG_PICK("  vector=%s LR=0x%lx\n", "A1 %s %lx\n")
#define KDIAG_F_A64_SYND   KICKOS_DIAG_PICK("  ESR=0x%lx ELR=0x%lx\n", "A2 %lx %lx\n")
#define KDIAG_F_A64_SPSR   KICKOS_DIAG_PICK("  SPSR=0x%lx\n", "A3 %lx\n")
#define KDIAG_F_A64_FAR    KICKOS_DIAG_PICK("  FAR=0x%lx\n", "A4 %lx\n")
#define KDIAG_F_A64_FAR_NA KICKOS_DIAG_PICK("  FAR not valid for this exception class\n", "A5\n")

#endif
