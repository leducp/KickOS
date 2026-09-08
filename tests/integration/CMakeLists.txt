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
# boards make the CLAIM.

# The board name as a CTest-name prefix, for the gates whose ctest name is the claim rather
# than the image and so cannot take kickos_add_qemu_test's derived name.
string(REPLACE "-" "_" _tag "${KICKOS_BOARD}")

foreach(_gate
    ampping aspacefault aspaceufault cxxtest drvdeath errnoprobe fault faultsurvive
    fp_switch hello initdemo kernelhalf libc_exit mpu_fault panicgate pspguard pubpanic
    rebootdemo reclaimwit ringpriv rootauth rootfault rootgone sched_exit selftest
    simconabi slaypeer stackdepth stackguard stress taskleave tele_flood tele_pingpong
    tlsprobe trapnest uartloop)
  include("${CMAKE_CURRENT_LIST_DIR}/gates/${_gate}.cmake")
endforeach()
