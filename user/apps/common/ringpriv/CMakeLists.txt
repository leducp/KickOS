# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The PRIVILEGE-RING gate, the axis rootfault/mpu_fault cannot reach: those prove MEMORY
# confinement through an MPU/PMP trap, so they say nothing on a board with no MPU. Two
# binaries, because one of them ends the process:
#   ringpriv - survivable register prober, prints its own verdict line.
#   ringppb  - terminal: ends on the privileged-only PPB read.
# Every MPU posture: the ring is the fabricated first frame's CONTROL.nPRIV and the PPB
# permission is architectural, so both arms hold with the MPU absent, disabled or enforcing.
# A board's `flat` variant IS the ring-only posture, which is what makes this gateable in CI.
#
# ARM only. The privilege axis exists on other ISAs (rv32imac mstatus.MPP, rxv3
# PSW_THREAD_USER) but reaching it needs their own asm and their own trap expectation:
# a U-mode CSR read traps on RISC-V rather than being ignored, so it is a different
# arm, not this one ported. The host sim has no privilege axis at all to probe.

# Does this core implement a privilege ring? Derived in the top-level CMakeLists.txt, which
# FATAL_ERRORs on an unclassified armv6m board.
set(_ring ${KICKOS_HAVE_PRIV_RING})

kickos_add_app_target(ringpriv main.cc)
target_link_libraries(ringpriv PRIVATE kickos)
kickos_emit_image(ringpriv)
target_compile_definitions(ringpriv PRIVATE RINGPRIV_EXPECT_RING=${_ring})

# ringppb only where there IS a ring to be refused. On a no-ring core the same read
# succeeds, and correctly so, which would make a confinement gate assert the opposite.
if(_ring)
  kickos_add_app_target(ringppb ppb.cc)
  target_link_libraries(ringppb PRIVATE kickos)
  kickos_emit_image(ringppb)
endif()
