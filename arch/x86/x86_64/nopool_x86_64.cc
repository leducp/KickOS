// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// These panic: returning 0 is what arch.h calls exhaustion, and a call here is a
// build-composition defect.

#include <kickos/chip_com1.h>

#include <kickos/arch/arch.h>

extern "C"
{
    void kfault_terminate(void) __attribute__((noreturn));
}

namespace
{
    [[noreturn]] void refuse(char const* what)
    {
        kickos::q35::com1_puts("\nx86_64 frame pool: ");
        kickos::q35::com1_puts(what);
        kickos::q35::com1_puts("\n");
        kfault_terminate();
    }
}

extern "C"
{

arch_phys_addr_t kickos_frame_alloc(void)
{
    refuse("kickos_frame_alloc, but this build carries no frame pool "
           "(the chip selects no HAS_ASPACE)");
}

void kickos_frame_free(arch_phys_addr_t frame)
{
    (void)frame;
    refuse("kickos_frame_free, but this build carries no frame pool "
           "(the chip selects no HAS_ASPACE)");
}

}
