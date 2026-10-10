/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * A second -T of a selftest image's sizing link: the arm records arena_fit.py reads, never
 * loaded.
 */

SECTIONS
{
    .kickos_arena_arm 0 (INFO) : { KEEP(*(.kickos_arena_arm)) }
}
