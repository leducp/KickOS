// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/arch/desc.h>
#include <kickos/arch/x2probe.h>
#include <kickos/chip_com1.h>

#include <stdint.h>

extern "C" void kickos_x86_64_landed(uintptr_t ram_base, uint64_t ram_size)
{
    (void)ram_base;
    (void)ram_size;

    kickos::x86_64::desc_init();
    kickos::q35::com1_puts("  " KICKOS_X2_TOKEN " descriptors loaded\n");
    kickos::x86_64::desc_report();

    kickos::x86_64::x2_probe();
}
