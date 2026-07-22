// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// F1 guard (Teensy / i.MX RT1062 M7 speculative-access fix, ERR011573). Reads an
// address INSIDE the FlexSPI aperture but PAST the populated 8 MiB image -- i.e. inside
// the Device + XN + no-access wrap the fix installs (docs/design-teensy-mpu-hang.md).
//
// With the fix, the MPU denies the access and the reporter prints a clean MemManage
// dump (MPU FAULT, MMFSR DACCVIOL, MMFAR = the address). WITHOUT the fix -- or if the
// wrap ever regresses to AP=RW (fable finding F1) -- the M7 issues the read to an
// unbacked FlexSPI slave that never responds and the core stalls FOREVER with no fault
// (the F1 DoS). So: a clean fault dump == the wrap is correctly no-access; a hang ==
// F1 regressed. Own binary because a fault ends the process; silicon one-shot (no
// wrapped external window exists on the sim/qemu gates, so there is no ctest here).

#include <kickos/kos.h>

// The probe address: inside FlexSPI (0x6000_0000) but beyond the 8 MiB populated image,
// so it lands in the no-access wrap. Overridable for a board with a different aperture.
#ifndef KICKOS_SPECFAULT_ADDR
#define KICKOS_SPECFAULT_ADDR 0x60800000u
#endif

int main(int, char**)
{
    kos_print("[specfault] reading unbacked wrapped FlexSPI -- expect a clean MPU FAULT\n");
    volatile uint32_t const* p = reinterpret_cast<volatile uint32_t const*>(KICKOS_SPECFAULT_ADDR);
    volatile uint32_t v = *p; // denied by the wrap -> MemManage; never returns
    (void)v;
    // Unreachable if the wrap is correct. Reaching here means the read was PERMITTED
    // (wrap regressed to a readable AP) -- and on real silicon that read would have
    // hung, not returned; this line only fires on a target without the wrap at all.
    kos_print("[specfault] ERROR: read was permitted (wrap not no-access?)\n");
    return 0;
}
