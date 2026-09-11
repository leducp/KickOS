# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Every in-tree CTest entry that rides an APP TARGET: a boot gate under an emulator, a run on
# the host sim, or a reader of a linked ELF. An application is not a test, so an app file
# declares its target and stops; the entries live here, beside the scripts they run.
#
# Added by the root CMakeLists.txt AFTER add_subdirectory(user/apps), which is what makes
# `if(TARGET <app>)` below the real answer rather than a guess: the parent that selects an app
# has already decided, so a fragment reads the decision instead of restating its condition.
#
# include() and not add_subdirectory(): ONE directory scope, so the root's add_test wrapper
# arms its decline pass once and reads the `host` label every site here sets, and a fragment
# can read a figure an earlier one computed.
#
# Two board groupings recur, and both are arch/chip facts rather than lists to maintain:
#   the four QEMU MPS2 FPGA images (an386/M4F, an505/M33, an500/M7, an385/M3) are exactly the
#     boards on the `mps2` chip backend, KICKOS_CHIP STREQUAL "mps2";
#   the two emulatable arm64 boards (qemu-arm64, imx8mp-evk) are exactly the armv8a fleet,
#     KICKOS_ARCH STREQUAL "armv8a".
#
# NO board predicate belongs to a gate merely because a board has no emulator:
# kickos_add_qemu_test is total and registers nothing there. A predicate here states which
# boards make the CLAIM, and where the claim is every emulated board but a few, it is spelled
# as the EXCLUSION: a list of the covered boards leaves a new board out and says nothing.

# The board name as a CTest-name prefix, for the gates whose ctest name is the claim rather
# than the image and so cannot take kickos_add_qemu_test's derived name.
string(REPLACE "-" "_" _tag "${KICKOS_BOARD}")

# EVERY fragment in gates/, found rather than listed: a name list is a second place to edit
# and a fragment missing from it registers nothing while the configure stays green.
# CONFIGURE_DEPENDS is what makes an added or deleted fragment re-run this configure instead of
# being picked up at the next unrelated one.
# SORTED, because a fragment may read a figure an earlier one computed and the glob's own order
# is the filesystem's.
file(GLOB _kickos_gates CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/gates/*.cmake")
list(SORT _kickos_gates)
if(NOT _kickos_gates)
  message(FATAL_ERROR "no fragment in ${CMAKE_CURRENT_LIST_DIR}/gates/, so this directory "
                      "would register no integration gate at all")
endif()
foreach(_gate IN LISTS _kickos_gates)
  include("${_gate}")
endforeach()
