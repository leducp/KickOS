# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The NESTED-TRAP witness. rv32imac only: it is the one backend that runs syscall dispatch
# on the caller's own stack AND builds its trap frames in software, so it is the only one
# where an interrupt taken mid-dispatch can put a frame at an unbounded depth on a thread's
# stack. rxv3 switches to the ISP on interrupt acceptance and armv7m forces SP_main in
# handler mode, so on both the nested frame lands on a trusted stack by hardware. main.cc
# #errors if an image is built elsewhere.
if(NOT KICKOS_ARCH STREQUAL "rv32imac")
  return()
endif()

# The witness the arm reads is kept OUT of a bench build: its call site is inside what the
# injected-IRQ bench measures. So is the arm, which would report traps=0 and fail for a
# reason that is not a regression.
if(KICKOS_BENCH)
  return()
endif()

kickos_add_diagnostic_app(trapnest
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET trapnest)
  return() # diagnostics off -> app not built; skip its test
endif()

# The figures the arm parks at live in the backend's own header, on this target only, so no
# other app picks up an arch-private include path.
target_include_directories(trapnest PRIVATE
  "${PROJECT_SOURCE_DIR}/arch/riscv/rv32imac/include")

if(NOT KICKOS_BUILD_TESTS)
  return()
endif()
if(NOT KICKOS_BOARD STREQUAL "qemu-riscv")
  return() # the one rv32imac board with a runner
endif()

# The interrupt KERNEL DESCENT, read out of the header the prologue itself reads. The gate
# compares the reported room BELOW the nested frame against it, so the frame's own bytes must
# not be in the figure: what has to fit under that frame is the ISR alone.
set(_tn_hdr
    "${PROJECT_SOURCE_DIR}/arch/riscv/rv32imac/include/kickos/arch/rv_trap_stack.h")
foreach(_tn_macro TRAP_KERNEL_DEPTH)
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
kickos_add_qemu_test(NAME qemu_riscv_trapnest TARGET trapnest
  BOARD ${KICKOS_BOARD}
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_trapnest.sh"
  ARGS ${_tn_TRAP_KERNEL_DEPTH})
