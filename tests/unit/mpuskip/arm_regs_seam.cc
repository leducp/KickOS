// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// See arm_regs_seam.h.

#include "arm_regs_seam.h"

#include <kickos/arch/arch.h>

#include "regs.h"

extern "C" void kickos_arm_mpu_program(struct arch_mpu_encoded const* img);
extern "C" struct arch_mpu_encoded const* kickos_arm_mpu_pending(void);

namespace kickos
{
    namespace testfix
    {
        FakeMpu g_mpu = {};

        namespace
        {
            kickos_arm_mpu_fixed_region const* g_fixed_rows = nullptr;
            size_t g_fixed_rows_n = 0;

            // MPU_TYPE is read-only silicon configuration, so it is rebuilt on each read
            // rather than stored: DREGION is the only field the backend looks at.
            uint32_t g_type_shadow = 0;
            uint32_t g_sink = 0;
        }

        void mpu_reset(unsigned dregion)
        {
            g_mpu = FakeMpu{};
            g_mpu.dregion = dregion;
            g_fixed_rows = nullptr;
            g_fixed_rows_n = 0;
        }

        void mpu_clear_trace()
        {
            for (size_t i = 0; i < MPU_HW_SLOTS; i++)
            {
                g_mpu.desc[i].touches = 0;
            }
            g_mpu.dmb = 0;
            g_mpu.dsb = 0;
            g_mpu.isb = 0;
            g_mpu.unknown_reads = 0;
            g_mpu.commit_irq_depth = 0;
        }

        void mpu_set_fixed(kickos_arm_mpu_fixed_region const* rows, size_t n)
        {
            g_fixed_rows = rows;
            g_fixed_rows_n = n;
        }
    }
}

namespace kickos
{
    namespace arm
    {
        volatile uint32_t& reg32(uintptr_t addr)
        {
            using namespace kickos::testfix;
            if (addr == MPU_TYPE)
            {
                g_type_shadow = static_cast<uint32_t>(g_mpu.dregion) << 8;
                return reinterpret_cast<volatile uint32_t&>(g_type_shadow);
            }
            if (addr == MPU_CTRL)
            {
                return reinterpret_cast<volatile uint32_t&>(g_mpu.ctrl);
            }
            if (addr == MPU_RNR)
            {
                return reinterpret_cast<volatile uint32_t&>(g_mpu.rnr);
            }
            if (addr == SCB_SHCSR)
            {
                return reinterpret_cast<volatile uint32_t&>(g_mpu.shcsr);
            }
            if (addr == MPU_RBAR or addr == MPU_RASR)
            {
                size_t slot = g_mpu.rnr;
                if (slot >= MPU_HW_SLOTS)
                {
                    g_mpu.unknown_reads++;
                    return reinterpret_cast<volatile uint32_t&>(g_sink);
                }
                g_mpu.desc[slot].touches++;
                if (addr == MPU_RBAR)
                {
                    return reinterpret_cast<volatile uint32_t&>(g_mpu.desc[slot].rbar);
                }
                return reinterpret_cast<volatile uint32_t&>(g_mpu.desc[slot].rasr);
            }
            g_mpu.unknown_reads++;
            return reinterpret_cast<volatile uint32_t&>(g_sink);
        }

        void barrier_dmb(void)
        {
            kickos::testfix::g_mpu.dmb++;
        }

        void barrier_dsb(void)
        {
            kickos::testfix::g_mpu.dsb++;
        }

        void barrier_isb(void)
        {
            kickos::testfix::g_mpu.isb++;
        }

        // Only the fixed-region overflow arm reaches this, and it spins there on silicon.
        // A case that reaches it hangs and times out, which is the verdict it deserves.
        void wait_for_interrupt(void)
        {
        }
    }
}

extern "C" size_t kickos_arm_mpu_fixed(struct kickos_arm_mpu_fixed_region const** out)
{
    *out = kickos::testfix::g_fixed_rows;
    return kickos::testfix::g_fixed_rows_n;
}

// The shipped kickos_arch_mpu_commit brackets the descriptor writes in cpsid and is one
// inline-asm statement away from compiling here; what the gate needs of it is the PendSV
// epilogue's SHAPE, which is to program whatever the stash holds. The stash and the writer
// it reaches are both the shipping ones.
extern "C" void kickos_arch_mpu_commit(void)
{
    kickos::testfix::g_mpu.commit_irq_depth = kickos::testfix::g_mpu.irq_depth;
    kickos_arm_mpu_program(kickos_arm_mpu_pending());
}

// Nesting depth rather than a flag: the restore must put back what its own save found, and
// a commit driven outside the epilogue nests its bracket inside the caller's IrqLock.
extern "C" arch_irq_state_t arch_irq_save(void)
{
    arch_irq_state_t const was = kickos::testfix::g_mpu.irq_depth;
    kickos::testfix::g_mpu.irq_depth++;
    return was;
}

extern "C" void arch_irq_restore(arch_irq_state_t state)
{
    kickos::testfix::g_mpu.irq_depth = static_cast<unsigned>(state);
}
