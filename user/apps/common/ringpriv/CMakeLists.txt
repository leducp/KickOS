# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The PRIVILEGE-RING gate, the axis rootfault/mpu_fault cannot reach: those prove MEMORY
# confinement through an MPU/PMP trap, so they say nothing on a board with no MPU. Two
# binaries, because one of them ends the process:
#   ringpriv - survivable register prober, prints its own verdict line.
#   ringppb  - terminal: ends on the privileged-only PPB read.
# Neither is conditioned on KICKOS_HAVE_MPU. That is the point of this gate: the ring is
# the fabricated first frame's CONTROL.nPRIV and the PPB permission is architectural, so
# both arms hold with the MPU absent, disabled, or enforcing. A board's `flat` variant
# IS the ring-only posture, which is what makes this gateable in CI rather than only
# capturable on no-MPU silicon.

# ARM only. The privilege axis exists on other ISAs (rv32imac mstatus.MPP, rxv3
# PSW_THREAD_USER) but reaching it needs their own asm and their own trap expectation:
# a U-mode CSR read traps on RISC-V rather than being ignored, so it is a different
# arm, not this one ported. The host sim has no privilege axis at all to probe.
if(NOT (KICKOS_ARCH STREQUAL "armv7m" OR KICKOS_ARCH STREQUAL "armv6m"))
  return()
endif()

# Does this core implement a privilege ring? The top-level CMakeLists.txt derives it and
# FATAL_ERRORs on an unclassified armv6m board, because fault isolation reads the SAME
# axis: a core with no ring cannot tell a thread's own fault from a kernel bug, so it must
# keep panicking.
set(_ring ${KICKOS_HAVE_PRIV_RING})

kickos_add_diagnostic_app(ringpriv
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET ringpriv)
  return() # diagnostics off -> app not built; skip its tests
endif()
target_compile_definitions(ringpriv PRIVATE RINGPRIV_EXPECT_RING=${_ring})

# The arm floor the runner enforces, kept beside the expectation that sets it. Must equal
# the number of unconditional `ok -` arms main.cc reports for this posture: any slack lets
# an arm be deleted with the gate still green, and the arm most worth deleting is the
# read-back itself.
if(_ring)
  set(_arms 5) # nPRIV, SPSEL, APSR method control, write ignored, nothing else changed
else()
  set(_arms 1) # the no-ring witness only
endif()

# ringppb only where there IS a ring to be refused. On a no-ring core the same read
# succeeds, and correctly so, which would make a confinement gate assert the opposite.
if(_ring)
  kickos_add_diagnostic_app(ringppb
    SOURCES ppb.cc
    BOARD   ${KICKOS_BOARD})
endif()

if(KICKOS_BUILD_TESTS)
  if(KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
    kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_ringpriv TARGET ringpriv
      BOARD ${KICKOS_BOARD}
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_app_arms.sh"
      ARGS ringpriv ${_arms})
    kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_ringppb TARGET ringppb
      BOARD ${KICKOS_BOARD}
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_qemu_ringppb.sh"
      ARGS ${KICKOS_FAULT_OUTCOME})
  elseif(KICKOS_BOARD STREQUAL "microbit")
    kickos_add_qemu_test(NAME microbit_ringpriv TARGET ringpriv BOARD microbit
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_app_arms.sh"
      ARGS ringpriv ${_arms})
  endif()
endif()
