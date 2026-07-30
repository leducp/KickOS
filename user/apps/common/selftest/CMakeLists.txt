# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The M0 verification app + the CI gates. Links the freestanding TAP harness
# (tests/tap) directly; the app self-asserts and emits TAP over the console.
#
# FULL-selftest prerequisite: KICKOS_MAX_HANDLES >= 9 (FIRST_DYNAMIC 4 + 2 permanent
# suite caps + a 3-own-cap test peak; see config/system.h). Below 9 the suite hard-fails
# on cap exhaustion by design; real apps use 1-3 caps and run fine below the floor.
kickos_add_application(selftest
  SOURCES main.cc "${PROJECT_SOURCE_DIR}/tests/tap/tap.cc"
  BOARD   ${KICKOS_BOARD})
target_include_directories(selftest PRIVATE "${PROJECT_SOURCE_DIR}/tests/tap")

# Size-optimise selftest's OWN TUs: at the fleet Debug default its .text overflows the
# 64K-flash parts (stm32f103 bluepill-c8, stm32f302). Only this app's TUs are affected,
# so the kernel/arch/lib archives stay at the board build type.
if(NOT (KICKOS_ARCH STREQUAL "sim"))
  target_compile_options(selftest PRIVATE -Os)
endif()

# The expected-skip list every gate below carries (EXPECT_SKIPS, checked by name in
# tests/check_tap_stream.sh). Empty means "no test may skip", the fleet default.
#
# Under KICKOS_ROOT_PRIVILEGED=OFF no privileged caller exists after boot, so
# mpu_privileged_guard skips; it is registered only under enforcement, hence both
# conditions. KICKOS_HAVE_MPU is 1 by arch on the sim, so this covers the flipped sim.
set(KICKOS_EXPECT_SKIPS "")
if(KICKOS_HAVE_MPU AND NOT KICKOS_ROOT_PRIVILEGED)
  list(APPEND KICKOS_EXPECT_SKIPS mpu_privileged_guard)
endif()
# Comma-joined before it reaches an ENVIRONMENT property: that property is itself a
# CMake list, so an embedded semicolon would split the value into a second bogus
# environment entry. The check script splits on commas.
list(JOIN KICKOS_EXPECT_SKIPS "," KICKOS_EXPECT_SKIPS)

if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  # The guard-probe case is registered only under KICKOS_ENABLE_SELFTEST (the sim
  # preset enables it), so the plan count still matches.
  add_test(NAME selftest
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_sim_selftest.sh" "$<TARGET_FILE:selftest>")
  set_tests_properties(selftest PROPERTIES TIMEOUT 30)
  set_property(TEST selftest APPEND PROPERTY
    ENVIRONMENT "EXPECT_SKIPS=${KICKOS_EXPECT_SKIPS}")

  # The OTHER console posture: the sim's only coverage of the published route
  # (kos_send on stdout cap index 0 -> userspace driver). One provider links per image,
  # so it needs its own build tree; the script makes one.
  add_test(
    NAME    sim_published_console
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_sim_published.sh"
            "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}")
  set_tests_properties(sim_published_console PROPERTIES TIMEOUT 300)

  # Dependency-inversion acceptance: an out-of-tree app builds against the
  # exported KickOS package (find_package + kickos_add_application) and runs.
  add_test(
    NAME    oot_export
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_oot_export.sh"
            "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}"
            "${CMAKE_COMMAND}" "${CMAKE_GENERATOR}")
  set_tests_properties(oot_export PROPERTIES TIMEOUT 120)
endif()

# The same TAP suite on the runnable QEMU targets. Needs KICKOS_ENABLE_SELFTEST (the
# qemu* presets enable it); rr_interleave sizes its RR quantum to the target's clock so
# it survives the coarse QEMU semihosting clock.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
  kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_selftest TARGET selftest BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_qemu_selftest.sh")
  set_property(TEST ${KICKOS_QEMU_MPS2_TAG}_selftest APPEND PROPERTY
    ENVIRONMENT "EXPECT_SKIPS=${KICKOS_EXPECT_SKIPS}")
endif()
# microbit (nrf51) runs the same suite on armv6m with MAX_SEMAPHORES=4 / MAX_THREADS=2.
# Every test must reclaim its semaphores so peak concurrent stays within 4.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "microbit")
  kickos_add_qemu_test(NAME microbit_selftest TARGET selftest BOARD microbit
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_qemu_selftest.sh")
  # The only board with legitimate skips. MAX_THREADS=2 / MAX_SEMAPHORES=4 cannot host
  # the multi-worker choreographies:
  #   mutex_basic, mutex_pi_donation, mutex_chain_boost, mutex_deadlock,
  #   mutex_multi_held: 3-worker PI choreographies on a 2-slot pool
  #   call_infoless_revert (4 workers), call_donation (3)
  #   domain_share: arena cannot spare the shared region
  #   irq_as_event: wants a 4 KiB MMIO page out of 16 KiB SRAM
  # caller_stack and mmio_grant report PARTIAL instead and count as passes, not skips.
  set_property(TEST microbit_selftest APPEND PROPERTY ENVIRONMENT
    "EXPECT_SKIPS=mutex_basic,mutex_pi_donation,mutex_chain_boost,mutex_deadlock,mutex_multi_held,call_infoless_revert,call_donation,domain_share,irq_as_event")
endif()
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv")
  kickos_add_qemu_test(NAME qemu_riscv_selftest TARGET selftest BOARD qemu-riscv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_qemu_selftest.sh")
  set_property(TEST qemu_riscv_selftest APPEND PROPERTY
    ENVIRONMENT "EXPECT_SKIPS=${KICKOS_EXPECT_SKIPS}")
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
  # Registered with a bare add_test rather than through the wrapper, so it needs the
  # build fixture named here: without it ctest -j runs cmake --install on the build
  # dir concurrently with kickos_build and fails about one run in ten.
  set_tests_properties(oot_export_mcu PROPERTIES TIMEOUT 120
                       FIXTURES_REQUIRED kickos_build)
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
