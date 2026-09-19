// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A PMSAv7 MPU the host can look inside, standing in for the silicon one under
// KICKOS_ARM_REGS_FROM_SEAM. The backend under test writes the REAL register addresses out
// of arch/arm/common/regs.h; this file is the window they land in, so the register map is
// never copied and a moved address is a failed lookup here rather than a silent pass.
//
// Descriptor banking is modelled, not faked: a write to MPU_RBAR/MPU_RASR lands in the
// descriptor MPU_RNR last selected, exactly as the hardware routes it.

#ifndef KICKOS_TESTS_UNIT_MPUSKIP_ARM_REGS_SEAM_H
#define KICKOS_TESTS_UNIT_MPUSKIP_ARM_REGS_SEAM_H

#include <stddef.h>
#include <stdint.h>

#include "mpu.h" // kickos_arm_mpu_fixed_region, the chip hook this seam answers

namespace kickos
{
    namespace testfix
    {
        // Descriptors the modelled silicon implements. 16 is the widest in the fleet (the
        // Cortex-M7 on imxrt1062); a narrower part is spelled by lowering dregion below.
        constexpr size_t MPU_HW_SLOTS = 16;

        struct FakeDescriptor
        {
            uint32_t rbar;
            uint32_t rasr;
            // How many times a commit REACHED this descriptor's registers, which is what a
            // skipped descriptor has to leave at zero.
            unsigned touches;
        };

        struct FakeMpu
        {
            FakeDescriptor desc[MPU_HW_SLOTS];
            uint32_t rnr;
            uint32_t ctrl;
            uint32_t shcsr;
            unsigned dregion; // MPU_TYPE DREGION, the descriptors this part implements
            unsigned dmb;
            unsigned dsb;
            unsigned isb;
            // An address the seam does not know. Non-zero fails the case rather than
            // letting a silently-dropped write read as a skip.
            unsigned unknown_reads;
            // The interrupt-mask nesting the seam has been asked for, and what it stood at
            // when the last commit ran. A commit driven from OUTSIDE the switch epilogue
            // swaps the pending stash around itself, and only this says it did so masked.
            unsigned irq_depth;
            unsigned commit_irq_depth;
        };

        extern FakeMpu g_mpu;

        // Back to reset: descriptors zeroed, MPU off, 8 descriptors implemented.
        void mpu_reset(unsigned dregion = 8);

        // Zero every touch count, barrier count and commit record, leaving the descriptor
        // words alone.
        void mpu_clear_trace();

        // Seat the chip fixed-region hook kickos_arm_mpu_fixed answers with.
        void mpu_set_fixed(struct kickos_arm_mpu_fixed_region const* rows, size_t n);
    }
}

#endif
