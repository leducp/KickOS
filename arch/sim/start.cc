// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// sim startup glue: the host process entry. Brings up the arch backend then
// enters the kernel. On MCU targets the equivalent is the reset handler.

#include <kickos/arch/arch.h>

namespace kickos
{
    int kmain(int argc, char** argv);
}

// Explicitly EMPTY app-ctor window: the host runtime has already run every app ctor
// before main, so kmain's walk must see start == end (walking again would construct
// them twice). kmain's bounds are STRONG, so a target with no window fails the link.
// Labels rather than a zero-length array: two distinct C++ objects are not guaranteed
// to share an address, and start == end must hold exactly.
asm(".pushsection .kickos_app_init_array,\"a\"\n"
    ".globl __kickos_app_init_array_start\n"
    ".globl __kickos_app_init_array_end\n"
    "__kickos_app_init_array_start:\n"
    "__kickos_app_init_array_end:\n"
    ".popsection\n");

int main(int argc, char** argv)
{
    arch_init();
    return kickos::kmain(argc, argv);
}
