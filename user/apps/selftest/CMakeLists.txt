# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The M0 verification app + the CI gates. Links the freestanding TAP harness
# (tests/tap) directly; the app self-asserts and emits TAP over the console.
kickos_add_application(selftest
  SOURCES main.cc "${PROJECT_SOURCE_DIR}/tests/tap/tap.cc"
  BOARD   ${KICKOS_BOARD})
target_include_directories(selftest PRIVATE "${PROJECT_SOURCE_DIR}/tests/tap")

if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  # Full M0 verification: every scheduler/sync/time/MPU bullet as a TAP test.
  # The guard-probe test needs KICKOS_ENABLE_SELFTEST (the sim preset enables it);
  # the case is registered only then, so the plan count still matches.
  # Gate on the TAP stream: run_all() prints the completion marker only after a
  # full clean run, so a mid-run fault (which exits 0 via the reported-fault
  # path) is caught as a truncated run, not a false pass. CTest checks FAIL
  # before PASS, so a "not ok" still fails.
  add_test(NAME selftest COMMAND "$<TARGET_FILE:selftest>")
  set_tests_properties(selftest PROPERTIES
    TIMEOUT 30
    PASS_REGULAR_EXPRESSION "# all tests passed"
    FAIL_REGULAR_EXPRESSION "not ok")

  # Dependency-inversion acceptance: an out-of-tree app builds against the
  # exported KickOS package (find_package + kickos_add_application) and runs.
  add_test(
    NAME    oot_export
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_oot_export.sh"
            "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}"
            "${CMAKE_COMMAND}" "${CMAKE_GENERATOR}")
  set_tests_properties(oot_export PROPERTIES TIMEOUT 120)
endif()

# The SAME TAP suite on the runnable QEMU targets -- the identical binary rebuilt
# per target, exercising the real ISA switch mechanism (armv7m/armv6m PendSV,
# rv32imac msip), not just the sim. Needs KICKOS_ENABLE_SELFTEST (the qemu* presets
# enable it); rr_interleave sizes its RR quantum to the target's clock so it runs
# even on the coarse QEMU semihosting clock.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu")
  add_test(NAME qemu_selftest
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_qemu_selftest.sh" "$<TARGET_FILE:selftest>")
  set_tests_properties(qemu_selftest PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
endif()
# microbit (nrf51) runs the SAME suite on armv6m. Its small pool (MAX_SEMAPHORES=4 /
# MAX_THREADS=2) is precisely why the suite is pool-honest -- every test reclaims its
# semaphores, so peak concurrent stays within 4. Gating armv6m here keeps that platform
# from regressing silently (a deferral would be a silent hole).
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "microbit")
  add_test(NAME microbit_selftest
    COMMAND "${CMAKE_COMMAND}" -E env QEMU_MACHINE=microbit
            "${PROJECT_SOURCE_DIR}/tests/check_qemu_selftest.sh" "$<TARGET_FILE:selftest>")
  set_tests_properties(microbit_selftest PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
endif()
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv")
  add_test(NAME qemu_riscv_selftest
    COMMAND "${CMAKE_COMMAND}" -E env QEMU=qemu-system-riscv32 QEMU_MACHINE=virt "QEMU_EXTRA=-bios none"
            "${PROJECT_SOURCE_DIR}/tests/check_qemu_selftest.sh" "$<TARGET_FILE:selftest>")
  set_tests_properties(qemu_riscv_selftest PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
endif()

# The same acceptance criterion for a bare-metal package: install the MCU
# package, then build an out-of-tree app against it with the SHIPPED cross
# toolchain (build-only; an MCU ELF can't run on the host). Registered on the
# qemu board -- the runnable ARM CI target -- so the MCU packaging surface
# (shipped toolchain + board.cmake, the INSTALL_INTERFACE linker script, the
# single-board guard) gets real coverage the sim gate can't give.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu")
  add_test(
    NAME    oot_export_mcu
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_oot_export_mcu.sh"
            "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}"
            "${CMAKE_COMMAND}" "${CMAKE_GENERATOR}")
  set_tests_properties(oot_export_mcu PROPERTIES TIMEOUT 120)
endif()
