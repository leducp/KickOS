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

// The app-ctor window kmain walks. On an MCU the chip linker script defines these
// bounds around its .kickos_app_init_array bucket; the sim has no linker script and
// wants none -- it exists to test a board+app and the kernel cheaply on the host, and
// MCU link machinery would defeat that. Nor does it need one: this is a hosted ELF, so
// the HOST runtime owns ctor execution and has already run every app ctor before main
// is entered. Walking them again from root_entry would construct them twice.
//
// So state that explicitly, as an intentionally EMPTY window: two labels at the same
// address in their own section. kmain's bounds are STRONG, which is what makes this a
// statement rather than a formality -- it distinguishes "nothing to walk here"
// (start == end, silent) from "this target's linker script forgot to partition
// .init_array" (undefined symbol, link error). Emitted as labels rather than a
// zero-length array because two distinct C++ objects are not guaranteed to share an
// address, and start == end must hold exactly.
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
