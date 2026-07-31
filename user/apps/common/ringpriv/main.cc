// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// PRIVILEGE-RING gate: asserts an unprivileged thread cannot exercise CPU privilege.
// The complement of rootfault/mpu_fault, which prove MEMORY confinement through an
// MPU/PMP fault. This needs no MPU and holds on a board that has none.
//
// A FAULT IS THE WRONG EXPECTATION HERE. On ARMv7-M an unprivileged MSR to a
// privileged special register is architecturally IGNORED, not trapped: the MSR
// pseudocode gates every write in the PRIMASK/BASEPRI/FAULTMASK/CONTROL group behind
// `if CurrentModeIsPrivileged()` with no else clause, and its Exceptions clause reads
// "None" (ARM DDI 0403E.e, B5.2.3). The CONTROL description states it directly: "The
// processor ignores unprivileged write accesses" (B1.4.4). So the assertion is that
// the write DID NOT TAKE EFFECT, read back from the register itself.
//
// Only CONTROL can carry that read-back. MRS of PRIMASK/BASEPRI/FAULTMASK from
// unprivileged Thread mode returns ZERO rather than the live value
// (`R[d]<7:0> = if CurrentModeIsPrivileged() then BASEPRI<7:0> else '00000000'`,
// B5.2.2), so "wrote a mask, read back zero" is vacuous - it reads zero whether or not
// the write landed. MRS of CONTROL carries NO privilege guard in that same pseudocode,
// which is what makes arm 3 an observation and not an artifact.
//
// The interrupt-mask arm that would pair with this (cpsid i must not mask) is absent
// deliberately, not forgotten: its read-back is vacuous for the reason above, and the
// only other observable is interrupt DELIVERY, which neither verification board can
// witness - mps2 derives the monotonic clock from a semihosting SYS_CLOCK call and
// stm32f302 from a free-running TIM2 counter, so both advance with interrupts masked.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys/emit.h>

using kickos::emit;

#ifndef RINGPRIV_EXPECT_RING
#error "RINGPRIV_EXPECT_RING must be 1 (real privilege ring) or 0 (no privilege axis)"
#endif

namespace
{
    constexpr uint32_t CONTROL_NPRIV = 1u << 0;
    constexpr uint32_t CONTROL_SPSEL = 1u << 1;

    int failures = 0;
    int arms = 0;

    // Every arm string must fit msg with its prefix and newline. A truncated arm loses its
    // '\n', merges two arms onto one line and undercounts the runner's per-line arm tally.
    void check(bool ok, char const* what)
    {
        char msg[128];
        if (ok)
        {
            arms = arms + 1;
            ksnprintf(msg, sizeof(msg), "[ringpriv] ok - %s\n", what);
            emit(msg);
            return;
        }
        failures = failures + 1;
        ksnprintf(msg, sizeof(msg), "[ringpriv] ERROR: %s\n", what);
        emit(msg);
    }

    // Called only from the postures whose arms are compiled out, so it is unused in the
    // others.
    [[maybe_unused]] void skip(char const* what)
    {
        char msg[128];
        ksnprintf(msg, sizeof(msg), "[ringpriv] skip - %s\n", what);
        emit(msg);
    }

    void report_u32(char const* what, uint32_t v)
    {
        char msg[96];
        ksnprintf(msg, sizeof(msg), "[ringpriv]   %s=0x%x\n", what,
                  static_cast<unsigned int>(v));
        emit(msg);
    }

    uint32_t read_control()
    {
        uint32_t v = 0;
        __asm volatile("mrs %0, control" : "=r"(v));
        return v;
    }
}

int main(int, char**)
{
    uint32_t const control = read_control();
    report_u32("CONTROL", control);

    // Arm 1 is arm 3's positive control, so it cannot be dropped as redundant. Reset
    // clears CONTROL and exception entry/return touch only SPSEL and FPCA (B1.4.4), so
    // nPRIV holds 1 only because a privileged `msr control` put it there (switch.S).
    // Without that, arm 3's "unchanged" could mean the bit is hardwired or the read is
    // broken.
#if RINGPRIV_EXPECT_RING
    check((control & CONTROL_NPRIV) != 0,
          "CONTROL.nPRIV=1: unprivileged, and off its reset value (a priv msr landed)");
    check((control & CONTROL_SPSEL) != 0,
          "CONTROL.SPSEL=1: on SP_process, a second bit off reset");
#else
    // The kernel believes this thread is unprivileged and the core disagrees. Asserted
    // rather than skipped so the no-ring classification is itself gated: a core that
    // grew a privilege ring, or a board misfiled as no-ring, goes red here.
    check((control & CONTROL_NPRIV) == 0,
          "CONTROL.nPRIV=0: no privilege axis, the unprivileged thread is PRIVILEGED");
#endif

#if defined(__ARM_ARCH) && (__ARM_ARCH >= 7) && RINGPRIV_EXPECT_RING
    // Arm 2. Positive control for the METHOD: an MSR that this same unprivileged thread
    // IS permitted to make does change what MRS reads back. The APSR group carries no
    // privilege guard in the MSR pseudocode (B5.2.3, `when '00000'`), so the flags are
    // writable here. Two distinct patterns, because one would not separate a live
    // read-back from a constant.
    //
    // Single asm block per pattern: the write and the read-back must not be split by a
    // function call that could clobber the flags between them.
    uint32_t apsr_set = 0;
    uint32_t apsr_clr = 0;
    __asm volatile("msr APSR_nzcvq, %1 \n mrs %0, apsr"
                   : "=r"(apsr_set)
                   : "r"(0xF8000000u)
                   : "cc");
    __asm volatile("msr APSR_nzcvq, %1 \n mrs %0, apsr"
                   : "=r"(apsr_clr)
                   : "r"(0x00000000u)
                   : "cc");
    report_u32("APSR after writing 0xF8000000", apsr_set);
    report_u32("APSR after writing 0x00000000", apsr_clr);
    check((apsr_set & 0xF8000000u) == 0xF8000000u and (apsr_clr & 0xF8000000u) == 0,
          "a permitted unprivileged msr DOES move the mrs read-back");

    // Arm 3. THE BOUNDARY. Clearing CONTROL.nPRIV from unprivileged Thread mode would
    // promote this thread to privileged, which is the whole confinement claim.
    //
    // SPSEL and FPCA are PRESERVED in the attempted value, so only bit 0 is at stake. A
    // regression that let the write land would then report cleanly instead of switching
    // this function's stack pointer out from under it mid-frame.
    //
    // The read, the msr, the isb and the read-back are ONE asm block and must stay one.
    // Any syscall in between - an emit() included - re-enters the kernel, and the syscall
    // return path restores CONTROL.nPRIV from ctx.resting_npriv (switch.S), which would
    // paper a successful promotion straight back over to 1 and turn this arm green on a
    // broken system. Both operands are read inside the block for the same reason: the
    // CONTROL captured at entry above is several syscalls old by now. Capture, then print.
    uint32_t pre = 0;
    uint32_t post = 0;
    uint32_t attempt = 0;
    __asm volatile("mrs %0, control  \n"
                   "bic %2, %0, #1   \n"
                   "msr control, %2  \n"
                   "isb              \n"
                   "mrs %1, control  \n"
                   : "=&r"(pre), "=&r"(post), "=&r"(attempt)
                   :
                   : "memory");
    report_u32("CONTROL before the attempt", pre);
    report_u32("CONTROL after attempting to clear nPRIV", post);
    check((post & CONTROL_NPRIV) != 0,
          "the unprivileged write to CONTROL.nPRIV was IGNORED: no self-promotion");
    check(post == pre,
          "the ignored write changed nothing in CONTROL (SPSEL/FPCA intact)");
#elif RINGPRIV_EXPECT_RING
    skip("the msr/mrs arms need armv7m assembler syntax (APSR_nzcvq)");
#else
    skip("the boundary arm needs a privilege ring: nothing here to escape from");
#endif

    char msg[64];
    if (failures != 0)
    {
        ksnprintf(msg, sizeof(msg), "[ringpriv] FAIL (%d)\n", failures);
        emit(msg);
        return 1;
    }
    ksnprintf(msg, sizeof(msg), "[ringpriv] PASS (%d arms)\n", arms);
    emit(msg);
    return 0;
}
