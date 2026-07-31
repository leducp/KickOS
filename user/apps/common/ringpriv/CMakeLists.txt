# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The PRIVILEGE-RING gate, the axis rootfault/mpu_fault cannot reach: those prove MEMORY
# confinement through an MPU/PMP trap, so they say nothing on a board with no MPU. Two
# binaries, because one of them ends the process:
#   ringpriv - survivable register prober, prints its own verdict line.
#   ringppb  - terminal: ends on the privileged-only PPB read.
# Neither is conditioned on KICKOS_HAVE_MPU. That is the point of this gate: the ring is
# the fabricated first frame's CONTROL.nPRIV and the PPB permission is architectural, so
# both arms hold with the MPU absent, disabled, or enforcing. A board configured
# -DKICKOS_HAVE_MPU=0 IS the ring-only posture, which is what makes this gateable in CI
# rather than only capturable on no-MPU silicon.

# ARM only. The privilege axis exists on other ISAs (rv32imac mstatus.MPP, rxv3
# PSW_THREAD_USER) but reaching it needs their own asm and their own trap expectation:
# a U-mode CSR read traps on RISC-V rather than being ignored, so it is a different
# arm, not this one ported. The host sim has no privilege axis at all to probe.
if(NOT (KICKOS_ARCH STREQUAL "armv7m" OR KICKOS_ARCH STREQUAL "armv6m"))
  return()
endif()

# Does this core implement a privilege ring? ARMv7-M always does (CONTROL.nPRIV is
# architectural). ARMv6-M's Unprivileged/Privileged Extension is OPTIONAL and separate
# from the MPU extension: Cortex-M0+ implements it, Cortex-M0 does not, and both are
# KICKOS_ARCH=armv6m - so the answer is per BOARD, not per arch, and there is no
# predefined macro that separates them. Enumerated rather than defaulted: a new armv6m
# port must state its class here instead of silently inheriting one and reporting a
# vacuous pass.
if(KICKOS_ARCH STREQUAL "armv7m")
  set(_ring 1)
elseif(KICKOS_BOARD STREQUAL "microbit")
  set(_ring 0) # nRF51822, Cortex-M0: msr CONTROL is discarded, every thread is privileged
elseif(KICKOS_BOARD STREQUAL "picopi")
  set(_ring 1) # RP2040, Cortex-M0+: implements the extension
else()
  message(FATAL_ERROR
    "ringpriv: armv6m board '${KICKOS_BOARD}' is not classified. Cortex-M0 has no "
    "privilege ring and Cortex-M0+ does; state which this core is (see the table in "
    "docs/design-unprivileged-root.md section 2) rather than letting the gate guess.")
endif()

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
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_qemu_ringpriv.sh"
      ARGS ${_arms})
    kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_ringppb TARGET ringppb
      BOARD ${KICKOS_BOARD}
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_qemu_ringppb.sh")
  elseif(KICKOS_BOARD STREQUAL "microbit")
    kickos_add_qemu_test(NAME microbit_ringpriv TARGET ringpriv BOARD microbit
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_qemu_ringpriv.sh"
      ARGS ${_arms})
  endif()
endif()
