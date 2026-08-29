// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/arch/x2probe.h>
#include <kickos/chip_com1.h>

#include <stdint.h>

extern "C" void kickos_x86_64_probe_load(uintptr_t addr);
extern "C" void kickos_x86_64_probe_store(uintptr_t addr);
extern "C" void kickos_x86_64_probe_fetch(uintptr_t addr);
extern "C" void kickos_x86_64_probe_selector(uintptr_t selector);
extern "C" void kickos_x86_64_probe_ud(void);
extern "C" void kickos_x86_64_probe_de(void);
extern "C" void kickos_x86_64_probe_soft(void);
extern "C" void kickos_x86_64_probe_df(uintptr_t bad_sp);
extern "C" char kickos_x86_64_probe_load_site[];
extern "C" char kickos_x86_64_probe_store_site[];
extern "C" char kickos_x86_64_probe_selector_site[];
extern "C" char kickos_x86_64_probe_ud_site[];
extern "C" char kickos_x86_64_probe_de_site[];
extern "C" char kickos_x86_64_probe_soft_site[];

namespace kickos::x86_64
{
    namespace
    {
        using namespace kickos::q35;

        // Canonical, and far above anything the UEFI memory map describes.
        constexpr uint64_t unmapped_canonical = 0xffff800000000000ull;
        // The first address whose bits 63:48 stop sign-extending bit 47; the processor
        // refuses it before any table is walked, so it faults #GP.
        constexpr uint64_t non_canonical = 0x0000800000000000ull;
        // Index 8 into a table of 7 entries, ring 0; the whole selector becomes the error code.
        constexpr uint64_t past_the_table = 0x0040;

        [[maybe_unused]] void expect(char const* name, unsigned vector, uintptr_t rip)
        {
            com1_puts("  " KICKOS_X2_TOKEN " probe=");
            com1_puts(name);
            com1_puts(" expect_vector=");
            com1_dec(vector);
            com1_puts(" expect_rip=");
            com1_hex64(rip);
            com1_puts("\n");
        }
    }

    void x2_probe(void)
    {
#if KICKOS_X2_FAULT == KICKOS_X2_FAULT_UD
        expect("ud", 6, reinterpret_cast<uintptr_t>(kickos_x86_64_probe_ud_site));
        kickos_x86_64_probe_ud();
#elif KICKOS_X2_FAULT == KICKOS_X2_FAULT_PF
        expect("pf", 14, reinterpret_cast<uintptr_t>(kickos_x86_64_probe_load_site));
        com1_puts("  " KICKOS_X2_TOKEN " expect_cr2=");
        com1_hex64(unmapped_canonical);
        com1_puts("\n");
        kickos_x86_64_probe_load(unmapped_canonical);
#elif KICKOS_X2_FAULT == KICKOS_X2_FAULT_PFW
        expect("pfw", 14, reinterpret_cast<uintptr_t>(kickos_x86_64_probe_store_site));
        com1_puts("  " KICKOS_X2_TOKEN " expect_cr2=");
        com1_hex64(unmapped_canonical);
        com1_puts("\n");
        kickos_x86_64_probe_store(unmapped_canonical);
#elif KICKOS_X2_FAULT == KICKOS_X2_FAULT_PFX
        // The faulting instruction is at the unmapped address, so rip and cr2 are one word.
        expect("pfx", 14, unmapped_canonical);
        com1_puts("  " KICKOS_X2_TOKEN " expect_cr2=");
        com1_hex64(unmapped_canonical);
        com1_puts("\n");
        kickos_x86_64_probe_fetch(unmapped_canonical);
#elif KICKOS_X2_FAULT == KICKOS_X2_FAULT_GP
        expect("gp", 13, reinterpret_cast<uintptr_t>(kickos_x86_64_probe_load_site));
        kickos_x86_64_probe_load(non_canonical);
#elif KICKOS_X2_FAULT == KICKOS_X2_FAULT_SEL
        expect("sel", 13, reinterpret_cast<uintptr_t>(kickos_x86_64_probe_selector_site));
        com1_puts("  " KICKOS_X2_TOKEN " expect_error=");
        com1_hex64(past_the_table);
        com1_puts("\n");
        kickos_x86_64_probe_selector(past_the_table);
#elif KICKOS_X2_FAULT == KICKOS_X2_FAULT_DE
        expect("de", 0, reinterpret_cast<uintptr_t>(kickos_x86_64_probe_de_site));
        kickos_x86_64_probe_de();
#elif KICKOS_X2_FAULT == KICKOS_X2_FAULT_SOFT
        expect("soft", 64, reinterpret_cast<uintptr_t>(kickos_x86_64_probe_soft_site));
        kickos_x86_64_probe_soft();
#elif KICKOS_X2_FAULT == KICKOS_X2_FAULT_DF
        // A double-fault frame's rip is not architecturally the faulting instruction.
        expect("df", 8, 0);
        kickos_x86_64_probe_df(non_canonical);
#else
        com1_puts("  " KICKOS_X2_TOKEN " probe=none\n");
#endif
    }
}
