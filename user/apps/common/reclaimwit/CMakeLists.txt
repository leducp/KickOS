# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Board-agnostic console-reclaim and terminate-drain witness.
if(KICKOS_CONSOLE STREQUAL "none" OR KICKOS_CONSOLE STREQUAL "rtt")
  message(STATUS
    "KickOS: reclaimwit skipped: KICKOS_CONSOLE=${KICKOS_CONSOLE} leaves the chip console "
    "path compiled out, so a console reclaim is not observable. Build it with "
    "-DKICKOS_CONSOLE=chip.")
  return()
endif()

if(NOT KICKOS_SERVICE_LIST STREQUAL "kickos_services_none")
  message(STATUS
    "KickOS: reclaimwit built, but KICKOS_SERVICE_LIST=${KICKOS_SERVICE_LIST} may already "
    "publish a console; the app refuses at run time if it does. It needs "
    "-DKICKOS_SERVICE_LIST=kickos_services_none to give a verdict.")
endif()

kickos_add_diagnostic_app(reclaimwit
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET reclaimwit)
  return() # diagnostics off -> app not built
endif()
target_compile_definitions(reclaimwit PRIVATE KICKOS_RW_MODE=0)

# A separate binary because it ENDS the system. Always built alongside the park arm, so
# both #if branches of main.cc stay compiled.
kickos_add_diagnostic_app(reclaimwit_drain
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
target_compile_definitions(reclaimwit_drain PRIVATE KICKOS_RW_MODE=1)

# With `both`, RTT carries kernel writes in every ownership state, so the MUTE line reaches
# an RTT viewer even on a correct run.
if(KICKOS_CONSOLE STREQUAL "both")
  target_compile_definitions(reclaimwit PRIVATE KICKOS_RW_RTT=1)
  target_compile_definitions(reclaimwit_drain PRIVATE KICKOS_RW_RTT=1)
endif()

# These gate the ownership state machine, not a per-chip reclaim body rewriting real UART
# registers, which stays an operator reading a silicon wire.
# The service list must be kickos_services_none or the app refuses at run time, so the gate
# would fail by construction instead of witnessing anything.
if(KICKOS_BUILD_TESTS AND KICKOS_SERVICE_LIST STREQUAL "kickos_services_none")
  if(KICKOS_ARCH STREQUAL "sim")
    add_test(NAME sim_reclaimwit_park
      COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_reclaimwit.sh"
              "$<TARGET_FILE:reclaimwit>" park)
    add_test(NAME sim_reclaimwit_drain
      COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_reclaimwit.sh"
              "$<TARGET_FILE:reclaimwit_drain>" drain)
    set_tests_properties(sim_reclaimwit_park sim_reclaimwit_drain PROPERTIES TIMEOUT 120)
  elseif(KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS
         OR KICKOS_BOARD STREQUAL "microbit"
         OR KICKOS_BOARD STREQUAL "qemu-riscv")
    # Board name as a CTest-name prefix, as the qemu gates spell it.
    string(REPLACE "-" "_" _rw_tag "${KICKOS_BOARD}")
    # The park arm is polled and killed, so its TIMEOUT must clear the poll bound
    # (QEMU_TIMEOUT * 5 polls at 0.2 s) rather than the boot.
    kickos_add_qemu_test(NAME ${_rw_tag}_reclaimwit_park TARGET reclaimwit
      BOARD ${KICKOS_BOARD} TIMEOUT 120
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_reclaimwit.sh" ARGS park)
    kickos_add_qemu_test(NAME ${_rw_tag}_reclaimwit_drain TARGET reclaimwit_drain
      BOARD ${KICKOS_BOARD} TIMEOUT 120
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_reclaimwit.sh" ARGS drain)
  endif()
endif()
