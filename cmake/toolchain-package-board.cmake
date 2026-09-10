# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Adopting an INSTALLED package's board, for every cross toolchain file. Included
# list-dir-relative right after the shipped descriptor, so it ships beside them in the
# package (root CMakeLists reads the toolchain file and installs the toolchain-*.cmake
# fragments it includes).
#
# A package holds one arch, one chip and one linker script. The FORCE below is what lets a
# consumer configure with the shipped toolchain and no -DKICKOS_BOARD at all, and it is also
# what makes a genuine cross-board request dangerous: unrefused, -DKICKOS_BOARD=<other> is
# overwritten here and the consumer builds the packaged board's image believing it is the
# one they named. KICKOSConfig.cmake's own single-board guard cannot see that request,
# because this file has already replaced it.
#
# KICKOS_TOOLCHAIN_DEFAULT_BOARD is the including file's own default, which is a value the
# caller did not ask for and so must not be refused.

if(NOT DEFINED KICKOS_TOOLCHAIN_DEFAULT_BOARD)
  message(FATAL_ERROR "KickOS: toolchain-package-board.cmake was included without "
    "KICKOS_TOOLCHAIN_DEFAULT_BOARD, so it cannot tell a cross-board request from this "
    "toolchain file's own default and would refuse a plain configure.")
endif()
if(NOT DEFINED KICKOS_BOARD_ID)
  message(FATAL_ERROR "KickOS: the shipped board descriptor states no KICKOS_BOARD_ID, so "
    "the package does not say which board it is for.")
endif()
if(NOT KICKOS_BOARD STREQUAL "${KICKOS_TOOLCHAIN_DEFAULT_BOARD}"
   AND NOT KICKOS_BOARD STREQUAL "${KICKOS_BOARD_ID}")
  message(FATAL_ERROR "KickOS: this package provides board '${KICKOS_BOARD_ID}', "
    "not '${KICKOS_BOARD}': a KickOS package is single-board")
endif()
set(KICKOS_BOARD "${KICKOS_BOARD_ID}" CACHE STRING "Target board" FORCE)
