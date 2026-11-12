// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The no-access guard of the Teensy / i.MX RT1062 M7 speculative-access fix (ERR011573).
// Reads an address INSIDE the FlexSPI aperture but PAST the populated 8 MiB image, i.e.
// inside the Device + XN + no-access wrap the fix installs.
//
// With the wrap at no-access the MPU denies the read and the reporter prints a clean
// MemManage dump (MPU FAULT, MMFSR DACCVIOL, MMFAR = the address). Should the wrap
// regress to a readable AP, the M7 issues the read to an unbacked FlexSPI slave that
// never responds and the core stalls FOREVER with no fault. The verdict is therefore
// binary: a clean fault dump means the wrap holds, a hang means it regressed. Silicon
// one-shot, and its own binary because the fault ends the process.

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>

#if not KICKOS_MEMORY_ENFORCED
#error "specfault's verdict is a denied read, which only an image enforcing memory can deny"
#endif

// The probe address: inside FlexSPI (0x6000_0000) but beyond the 8 MiB populated image,
// so it lands in the no-access wrap. Overridable for a board with a different aperture.
#ifndef KICKOS_SPECFAULT_ADDR
#define KICKOS_SPECFAULT_ADDR 0x60800000u
#endif

namespace
{
    // armv6m reports the faulting instruction and no address, so its judge holds the fault's PC
    // to the address main announces for this load: the load must stay the body's first
    // instruction, and main calls through g_read_word so no inlined copy or clone runs instead.
    __attribute__((noinline)) uint32_t read_word(volatile uint32_t const* cell)
    {
        return *cell;
    }
    uint32_t (*volatile g_read_word)(volatile uint32_t const*) = read_word;
}

int main(int, char**)
{
    auto const load = g_read_word;
    char msg[128];
    ksnprintf(msg, sizeof(msg),
              "[specfault] reading 0x%x: expect a clean MPU FAULT by the load at %p\n",
              static_cast<unsigned>(KICKOS_SPECFAULT_ADDR), reinterpret_cast<void*>(load));
    kos_print(msg);
    (void)load(reinterpret_cast<volatile uint32_t const*>(KICKOS_SPECFAULT_ADDR));
    // Reached only where the read was PERMITTED, i.e. on a target carrying no wrap at
    // all: on this silicon a permitted read hangs instead of returning.
    kos_print("[specfault] ERROR: read was permitted (wrap not no-access?)\n");
    return 0;
}
