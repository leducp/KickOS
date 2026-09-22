# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Eleven linker-script bounds (docs/reference/invariants.md,
# link-bounds-stated-or-the-link-fails) must be ASSIGNED by every chip's own linker
# script, empty window or not: the kernel reads each one through a STRONG reference
# (include/kickos/klink.h, KICKOS_LINK_BOUND), so an absent one is a link error and not a
# silent zero. The assignment routinely sits behind #if KICKOS_HAVE_MPU with the *_NONE
# macro on the #else, so the check has to run over the file cpp already resolved.
#
# KICKOS_LINKER_SCRIPT (arch/CMakeLists.txt, set once the chip's script exists) already
# names that exact generated file: the same one the -T flag hands to the real link, built
# by the kickos_ldscript_<chip> target that is part of ALL. This gate reads it rather than
# re-deriving it, so it can never disagree with what actually got linked.
#
# Two targets take no per-chip test here:
#   - the sim has no chip and no linker script at all (KICKOS_LINKER_SCRIPT stays unset);
#   - x86_64's PE32+ image links from arch/x86/x86_64/pe_image.ld by a raw
#     `ld -m i386pep` custom command (cmake/x86_64_boot.cmake), never through the cpp
#     step arch/CMakeLists.txt runs for every other chip, so pe_image.ld's own source IS
#     the file the link reads. It deliberately omits the five app-split bounds: x86_64
#     compiles no address space today (KICKOS_HAVE_ASPACE=0), so nothing in that image
#     references them, and --no-app-split states that exception here rather than the gate
#     silently skipping a chip nobody named.

add_test(NAME ${_tag}_link_bounds_stated_controls
  COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_link_bounds_stated.sh" --controls)
kickos_host_gate(${_tag}_link_bounds_stated_controls)

if(KICKOS_ARCH STREQUAL "x86_64")
  add_test(NAME ${_tag}_link_bounds_stated
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_link_bounds_stated.sh"
            "${PROJECT_SOURCE_DIR}/arch/x86/x86_64/pe_image.ld" --no-app-split)
  kickos_host_gate(${_tag}_link_bounds_stated)
elseif(KICKOS_LINKER_SCRIPT)
  add_test(NAME ${_tag}_link_bounds_stated
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_link_bounds_stated.sh"
            "${KICKOS_LINKER_SCRIPT}")
  kickos_host_gate(${_tag}_link_bounds_stated)
endif()
