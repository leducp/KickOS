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

// The probe address: inside FlexSPI (0x6000_0000) but beyond the 8 MiB populated image,
// so it lands in the no-access wrap. Overridable for a board with a different aperture.
#ifndef KICKOS_SPECFAULT_ADDR
#define KICKOS_SPECFAULT_ADDR 0x60800000u
#endif

int main(int, char**)
{
    kos_print("[specfault] reading unbacked wrapped FlexSPI: expect a clean MPU FAULT\n");
    volatile uint32_t const* p = reinterpret_cast<volatile uint32_t const*>(KICKOS_SPECFAULT_ADDR);
    volatile uint32_t v = *p; // denied by the wrap -> MemManage; never returns
    (void)v;
    // Reached only where the read was PERMITTED, i.e. on a target carrying no wrap at
    // all: on this silicon a permitted read hangs instead of returning.
    kos_print("[specfault] ERROR: read was permitted (wrap not no-access?)\n");
    return 0;
}
