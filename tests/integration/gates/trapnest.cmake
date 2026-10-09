# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The NESTED-TRAP arm, riding `trapnest`. qemu-riscv is the only rv32imac board with a
# runner, and the helper registers nothing on a board with no emulator, so no board
# predicate is spelled here.

if(NOT TARGET trapnest)
  return()
endif()

# THE DEEP SYSCALL IS A SPAWN THE KERNEL MUST REFUSE, and what refuses it is the region
# admissibility Rule 7 enforces on a grant outside the arena. With no MPU backend nothing
# refuses that region, so the attempt SUCCEEDS: a second thread starts on main.cc's worker
# entry, parks its sp at the first worker's stack_lo, and its own ecall is then correctly
# refused as an sp outside that thread's stack. The arm would report that refusal as a failure
# of the entry, which is the one thing it is not, so user/apps/common/CMakeLists.txt builds no
# image on such a variant.

# The interrupt KERNEL DESCENT and the band each thread poisons, read out of the header the
# prologue itself reads. The descent is the ISR's alone, without the frame's own bytes.
set(_tn_hdr
    "${PROJECT_SOURCE_DIR}/arch/riscv/rv32imac/include/kickos/arch/rv_trap_stack.h")
foreach(_tn_macro TRAP_KERNEL_DEPTH TRAP_KERNEL_DEPTH_SYS TRAP_FRAME_SYS)
  file(STRINGS "${_tn_hdr}" _tn_hit
       REGEX "^#define KICKOS_RV_${_tn_macro} +[0-9]+$")
  list(LENGTH _tn_hit _tn_n)
  if(NOT _tn_n EQUAL 1)
    message(FATAL_ERROR
      "trapnest: KICKOS_RV_${_tn_macro} is not one plain integer in ${_tn_hdr}, so the "
      "expectation cannot be derived from the figure the prologue enforces")
  endif()
  string(REGEX REPLACE "^.* " "" _tn_${_tn_macro} "${_tn_hit}")
endforeach()
math(EXPR _tn_band_words "(${_tn_TRAP_FRAME_SYS} + ${_tn_TRAP_KERNEL_DEPTH_SYS}) / 4")
kickos_add_qemu_test(TARGET trapnest
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_trapnest.sh"
  ARGS ${_tn_TRAP_KERNEL_DEPTH} ${_tn_band_words} "${CMAKE_NM}")
kickos_app_judge(trapnest tests/integration/check_trapnest.sh
  ARGS ${_tn_TRAP_KERNEL_DEPTH} ${_tn_band_words})
