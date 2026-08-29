// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// SCAFFOLDING. tools/run-qemu-x86_64-x2.sh builds one image per fault class below and holds
// each report against it.

#ifndef KICKOS_ARCH_X2PROBE_H
#define KICKOS_ARCH_X2PROBE_H

#define KICKOS_X2_FAULT_NONE 0
#define KICKOS_X2_FAULT_UD 1
#define KICKOS_X2_FAULT_PF 2
#define KICKOS_X2_FAULT_GP 3
#define KICKOS_X2_FAULT_DE 4
#define KICKOS_X2_FAULT_SOFT 5
#define KICKOS_X2_FAULT_DF 6
#define KICKOS_X2_FAULT_PFW 7
#define KICKOS_X2_FAULT_PFX 8
#define KICKOS_X2_FAULT_SEL 9

#ifndef KICKOS_X2_FAULT
#define KICKOS_X2_FAULT KICKOS_X2_FAULT_NONE
#endif

// tools/run-qemu-x86_64-x2.sh carries this string too; move both or the arm fails.
#define KICKOS_X2_TOKEN "KICKOS-X2 5be09c14 x86_64/q35 descriptors"

namespace kickos::x86_64
{
    // Returns only for KICKOS_X2_FAULT_NONE.
    void x2_probe(void);
}

#endif
