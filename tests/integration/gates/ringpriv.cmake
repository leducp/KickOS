# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The PRIVILEGE-RING arms, riding `ringpriv` (survivable prober) and `ringppb` (terminal on
# the privileged-only PPB read). Every MPU posture: the ring is the fabricated first frame's
# CONTROL.nPRIV and the PPB permission is architectural, so both arms hold with the MPU
# absent, disabled or enforcing.

if(NOT TARGET ringpriv)
  return()
endif()

# Does this core implement a privilege ring? Derived in the top-level CMakeLists.txt, which
# FATAL_ERRORs on an unclassified armv6m board.
set(_ring ${KICKOS_HAVE_PRIV_RING})

# The arm floor the runner enforces, kept beside the expectation that sets it. Must equal
# the number of `ok -` arms main.cc reports for this posture: any slack lets an arm be
# deleted with the gate still green, and the arm most worth deleting is the read-back
# itself.
#
# ONE CLAUSE PER `#if` GUARD in main.cc, because the two blocks are on different axes and a
# single number for both ARM architectures is wrong for one of them:
#   block 1, the CONTROL read-back, is guarded by RINGPRIV_EXPECT_RING alone;
#   block 2, the msr/mrs boundary set, also needs `__ARM_ARCH >= 7`. armv6m takes the
#   `#elif` leg, which calls skip(), and a skip increments no arm counter.
if(_ring)
  set(_arms 2) # nPRIV=1, SPSEL=1
else()
  set(_arms 1) # the no-ring witness only
endif()
if(_ring AND KICKOS_ARCH STREQUAL "armv7m")
  math(EXPR _arms "${_arms} + 3") # APSR method control, write ignored, nothing else changed
endif()

# The same count stated as a whole-posture total, NEVER derived from the clauses above: the
# equality is a check only while both sides are written out, so an arm added to either block
# has to move a number on each side. Checked on every ARM board, including the two postures
# no CTest gate below covers.
set(_arms_posture 1) # no ring: the no-ring witness, and both boundary arms skipped
if(_ring)
  set(_arms_posture 2) # armv6m with a ring: block 1 only
  if(KICKOS_ARCH STREQUAL "armv7m")
    set(_arms_posture 5) # the full set
  endif()
endif()
if(NOT _arms EQUAL _arms_posture)
  message(FATAL_ERROR
    "ringpriv arm count disagrees with itself on board '${KICKOS_BOARD}' (arch="
    "${KICKOS_ARCH} ring=${_ring}): the per-guard clauses sum to ${_arms}, the posture "
    "total says ${_arms_posture}. An arm was added to or removed from one of main.cc's two "
    "`#if` blocks and only one side moved. Fix both in ${CMAKE_CURRENT_LIST_FILE}, do not "
    "relax this check.")
endif()

# Printed on every ARM board that builds the app: armv6m-with-a-ring (picopi) and the flat
# variants register no CTest entry below, so this line is the only place their expectation is
# observable, and an operator capturing that board on silicon reads the floor from here.
message(STATUS
  "KickOS: ringpriv expects exactly ${_arms} arm(s) (priv_ring=${_ring} arch=${KICKOS_ARCH})")

if(KICKOS_CHIP STREQUAL "mps2")
  kickos_add_qemu_test(TARGET ringpriv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_app_arms.sh"
    ARGS ringpriv ${_arms})
  kickos_add_qemu_test(TARGET ringppb
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_ringppb.sh"
    ARGS ${KICKOS_FAULT_OUTCOME})
elseif(KICKOS_BOARD STREQUAL "microbit")
  # The prober arm alone on this board. ringppb is registered on the MPS2 boards only, and
  # that absence is deliberate rather than an oversight.
  kickos_add_qemu_test(TARGET ringpriv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_app_arms.sh"
    ARGS ringpriv ${_arms})
endif()
