# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The M0 verification app + the CI gates. Links the freestanding TAP harness
# (tests/tap) directly; the app self-asserts and emits TAP over the console.
#
# FULL-selftest prerequisite: KICKOS_MAX_HANDLES >= 9 (FIRST_DYNAMIC 4 + 2 permanent
# suite caps + a 3-own-cap test peak; see config/system.h). The four tiny boards floor
# at 9 for this reason. A below-9 build (e.g. a manual -DKICKOS_MAX_HANDLES=6 override)
# is not a full-selftest target: its suite hard-fails on cap exhaustion by design. Real
# apps use 1-3 caps and run fine below the floor; only the full suite needs 9.
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

# Boot-ctor-split guard: on an enforced armv7m link (.kickos_app_init_array exists),
# assert no closed-set kernel ctor leaked into the app-ctor window (would run after
# kmain). Runs against this selftest ELF -- a plain app is enough: the check needs
# the section symbols plus the four kernel archives, not app/libstdc++ ctors.
if(KICKOS_BUILD_TESTS AND KICKOS_HAVE_MPU AND KICKOS_ARCH STREQUAL "armv7m")
  add_test(
    NAME    kernel_ctor_placement
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_kernel_ctor_placement.sh"
            "$<TARGET_FILE:selftest>" "${CMAKE_NM}" "${CMAKE_OBJCOPY}"
            "$<TARGET_FILE:kickos_kernel>"
            "$<TARGET_FILE:kickos_arch_${KICKOS_ARCH}>"
            "$<TARGET_FILE:kickos_chip_${KICKOS_CHIP}>"
            "$<TARGET_FILE:kickos_lib>")
  set_tests_properties(kernel_ctor_placement PROPERTIES TIMEOUT 60)
endif()

# RISC-V-under-PMP small-data guard: the gp window sits inside the app grant, so the
# KickOS libs (built -msmall-data-limit=0) must emit ZERO .sdata/.sbss -- a stray
# kernel global there is reachable/writable by an unprivileged thread. Assert per
# archive (docs/design-riscv-gp-split.md, Option 4).
if(KICKOS_BUILD_TESTS AND KICKOS_HAVE_MPU AND KICKOS_ARCH STREQUAL "rv32imac")
  add_test(
    NAME    riscv_no_smalldata
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_riscv_no_smalldata.sh"
            "${CMAKE_OBJDUMP}"
            "$<TARGET_FILE:kickos_kernel>"
            "$<TARGET_FILE:kickos_arch_${KICKOS_ARCH}>"
            "$<TARGET_FILE:kickos_chip_${KICKOS_CHIP}>"
            "$<TARGET_FILE:kickos_lib>")
  set_tests_properties(riscv_no_smalldata PROPERTIES TIMEOUT 60)
endif()
