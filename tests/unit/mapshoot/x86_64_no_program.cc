// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// An image with no program span: both bounds name one byte, so aspace_init checks no span.

extern "C"
{
    unsigned char kickos_seam_program[1] = {};
    extern unsigned char __kickos_app_rom_start[1] __attribute__((alias("kickos_seam_program")));
    extern unsigned char __kickos_app_sram_end[1] __attribute__((alias("kickos_seam_program")));
}
