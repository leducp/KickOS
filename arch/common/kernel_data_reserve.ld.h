/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a C header: these macros expand to GNU ld statements. Included from every enforcing
 * chip linker script, which arch/CMakeLists.txt runs through cpp before the link.
 */
#ifndef KICKOS_ARCH_COMMON_KERNEL_DATA_RESERVE_LD_H
#define KICKOS_ARCH_COMMON_KERNEL_DATA_RESERVE_LD_H

/* The app window is pow2-sized and pow2-ALIGNED, so basing it on the alignment above kernel
 * .bss makes ONE BYTE of kernel .bss cost a WHOLE window: the window, the newlib heap and the
 * user-RAM arena above them all slide one _appdata_size higher and the link stays green.
 * The gap that opens is arbitrary modulo the window, so no threshold on it is principled.
 * The base is
 * DECLARED instead, _kernel_data_top = <kernel data base> + _kernel_data_reserve, and the
 * window, the heap and the arena then stop moving with kernel .bss at all.
 *
 * A chip states its reserve as a whole number of windows, through a KICKOS_KERNEL_DATA_RESERVE
 * default a preset can raise, so a posture that does not fit pays the arena where the cost is
 * visible rather than losing a window unremarked.
 *
 * THE PIN ITSELF STAYS PER-CHIP, as MAX(., _kernel_data_top) in the .appdata section's own
 * ADDRESS expression, because that section's region and AT clause differ per chip. Two ld
 * facts fix that spelling. A bare address fails the link with ld's backwards-move error the
 * moment kernel .bss overflows, and ld evaluates every ASSERT AFTER layout, so an overflowing
 * link must still lay out (one window higher) for the ASSERT below to be the thing that
 * prints. And the same MAX written as a pad INSIDE an output section body does not work: an
 * expression assigned to `.` inside a body is taken as an OFFSET FROM THE SECTION START
 * whenever it evaluates absolute, so `. = MAX(., <absolute>)` lands the section a whole image
 * higher and reports `will not fit in region` at a nonsense address, while a purely absolute
 * right-hand side with no `.` in it is refused outright as `invalid assignment to location
 * counter`. ABSOLUTE(.) rescues neither spelling.
 */

/* Invoked where the base is in scope: at file scope on a chip whose kernel .data starts at
 * ORIGIN(RAM), inside SECTIONS on one whose base is a symbol defined there. */
#define KICKOS_KERNEL_DATA_RESERVE_DECL(base)              \
    _kernel_data_reserve = KICKOS_KERNEL_DATA_RESERVE;     \
    _kernel_data_top = (base) + _kernel_data_reserve;

/* The kernel .bss end is an argument because the RX ABI spells every C identifier with an
 * extra leading underscore, so rx72m.ld passes __ebss where the others pass _ebss. */
#define KICKOS_KERNEL_DATA_RESERVE_ASSERT(bss_end)                      \
    ASSERT((bss_end) <= _kernel_data_top,                               \
           "KickOS: kernel .data/.bss overflow _kernel_data_reserve: the app window base is _appdata_size-aligned, so the window, the newlib heap and the user-RAM arena would all slide one whole _appdata_size higher and the arena would lose that many bytes with no other diagnostic. Raise KICKOS_KERNEL_DATA_RESERVE by one _appdata_size in this board's configure preset and pay the arena, or cut kernel .bss") \
    ASSERT((bss_end) + _appdata_size > _kernel_data_top,                \
           "KickOS: _kernel_data_reserve is a whole _appdata_size larger than kernel .data/.bss needs, so the user-RAM arena is short by that much for nothing. Lower KICKOS_KERNEL_DATA_RESERVE by one _appdata_size, or drop this preset's override to take the chip default") \
    ASSERT(_kernel_data_top == ALIGN(_kernel_data_top, _appdata_size),  \
           "KickOS: _kernel_data_top (the kernel data base plus _kernel_data_reserve) is not _appdata_size-aligned, so .appdata's own ALIGN moves the window off the address the reserve names (keep KICKOS_KERNEL_DATA_RESERVE a whole multiple of _appdata_size)")

#endif
