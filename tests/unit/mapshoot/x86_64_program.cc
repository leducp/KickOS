// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A program's span at the fixed user base, absolute as the image link states it.

#include <kickos/arch/program_base.h>

#define KOS_SEAM_TEXT(x) #x
#define KOS_SEAM_VALUE(x) KOS_SEAM_TEXT(x)

__asm__(".globl __kickos_app_rom_start\n"
        ".set __kickos_app_rom_start, " KOS_SEAM_VALUE(KICKOS_X86_64_PROGRAM_BASE) "\n"
        ".globl __kickos_app_sram_end\n"
        ".set __kickos_app_sram_end, " KOS_SEAM_VALUE(KICKOS_X86_64_PROGRAM_BASE) " + 0x10000\n");
