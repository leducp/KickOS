# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

cmake_minimum_required(VERSION 3.24)
# write_basic_package_version_file demands a numeric version; find_package(KickOS) matches
# against it.
project(KickOS VERSION 0.5.1 LANGUAGES C CXX ASM)
# A point release spells 0.<milestone>.<submilestone>-<point>, which project() rejects, so
# KICKOS_VERSION has to be set outright once there is one.
set(KICKOS_VERSION "${PROJECT_VERSION}")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(cmake/kickos.cmake)
include(cmake/kconfig.cmake)
# Must precede add_subdirectory(user/apps) (a selftest expectation keys on the chunk
# granule) and the width sum at the bottom of this file.
include(cmake/cap_geometry.cmake)

# KICKOS is what an add_subdirectory(KickOS) consumer tests with if(KICKOS).
set(KICKOS TRUE CACHE INTERNAL "Building against KickOS")
add_compile_definitions(__KickOS__=1)

# --- Board / arch selection -------------------------------------------------
kickos_board_names(_kickos_board_names)
list(JOIN _kickos_board_names "|" _kickos_board_help)
set(KICKOS_BOARD "sim" CACHE STRING "Target board: ${_kickos_board_help}")
# The cross toolchain file already read the same board descriptor pre-project for the CPU
# flags.
kickos_load_board_descriptor("${KICKOS_BOARD}" KICKOS_ARCH KICKOS_CHIP KICKOS_ARCH_FAMILY)
message(STATUS "KickOS: board=${KICKOS_BOARD} family=${KICKOS_ARCH_FAMILY} arch=${KICKOS_ARCH} chip=${KICKOS_CHIP}")

include_directories("${PROJECT_SOURCE_DIR}/include")

# Must be added HERE, before any add_subdirectory: a subdirectory copies the include
# directories in force when it is added, and config/cap_width.h is not written until the
# bottom of this file.
include_directories("${PROJECT_BINARY_DIR}/generated/include")

# --- Board provisioning -----------------------------------------------------
if(KICKOS_CHIP)
  set(KICKOS_CHIP_INCLUDE_DIR
      "${CMAKE_CURRENT_SOURCE_DIR}/arch/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/include")
  if(NOT EXISTS "${KICKOS_CHIP_INCLUDE_DIR}/kickos/chip_limits.h")
    message(FATAL_ERROR
      "Chip '${KICKOS_CHIP}' ships no ${KICKOS_CHIP_INCLUDE_DIR}/kickos/chip_limits.h. "
      "Every chip states its own interrupt-line count there, unconditionally; without "
      "it config/board.h has no KICKOS_MAX_IRQ and refuses rather than guessing 32.")
  endif()
  include_directories("${KICKOS_CHIP_INCLUDE_DIR}")
endif()

# Present only on the boards whose chip backend reads it.
set(KICKOS_BOARD_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/${KICKOS_BOARD}/include")
if(EXISTS "${KICKOS_BOARD_INCLUDE_DIR}")
  include_directories("${KICKOS_BOARD_INCLUDE_DIR}")
else()
  set(KICKOS_BOARD_INCLUDE_DIR "")
endif()

# --- Configuration from Kconfig --------------------------------------------
# KICKOS_CONFIG_VARIANT selects boards/<board>/configs/<variant>/defconfig; each variant
# defconfig is a complete statement, not a delta on another.
set(KICKOS_CONFIG_VARIANT "base" CACHE STRING
    "Configuration variant under boards/<board>/configs/")
set(KICKOS_DEFCONFIG
    "${CMAKE_CURRENT_SOURCE_DIR}/boards/${KICKOS_BOARD}/configs/${KICKOS_CONFIG_VARIANT}/defconfig")
if(EXISTS "${KICKOS_DEFCONFIG}")

  # Every arm must translate in BOTH directions: sending only the positive one lets a
  # defconfig that enables something outrank a command line asking to turn it off.
  set(_kcfg_req "")
  # A name not in this list never becomes a request; CMake merely reports it unused.
  # KICKOS_MAX_HANDLES is the ANSWER of the sum in cmake/cap_table.cmake, not an input.
  foreach(_knob KICKOS_MAX_THREADS KICKOS_USER_STACK_SIZE KICKOS_IDLE_STACK_SIZE
                KICKOS_ROOT_STACK_SIZE KICKOS_MAX_SEMAPHORES KICKOS_MAX_MUTEXES
                KICKOS_MAX_ENDPOINTS KICKOS_CAP_TABLE_SUPPLY KICKOS_MAX_SPAWN_GRANTS
                KICKOS_MAX_IRQ_HANDLES KICKOS_RTT_CH1_SIZE KICKOS_USER_HEAP_SIZE
                KICKOS_MAX_INSTANCES KICKOS_PANIC_DUMP KICKOS_DIAG_TERSE)
    if(DEFINED ${_knob})
      list(APPEND _kcfg_req "CONFIG_${_knob}=${${_knob}}")
    endif()
  endforeach()
  if(DEFINED KICKOS_CONSOLE)
    string(TOUPPER "${KICKOS_CONSOLE}" _kcfg_console)
    list(APPEND _kcfg_req "CONFIG_CONSOLE_${_kcfg_console}=y")
  endif()
  if(DEFINED KICKOS_TELEMETRY)
    string(TOUPPER "${KICKOS_TELEMETRY}" _kcfg_telem)
    list(APPEND _kcfg_req "CONFIG_TELEMETRY_${_kcfg_telem}=y")
  endif()
  # A knob the fragment sets but this translation omits is one the fragment silently
  # overwrites.
  foreach(_str KICKOS_SERVICE_LIST KICKOS_BOARD_PINMAP)
    if(DEFINED ${_str})
      list(APPEND _kcfg_req "CONFIG_${_str}=\"${${_str}}\"")
    endif()
  endforeach()
  foreach(_flag KICKOS_ENABLE_SELFTEST KICKOS_DEBUG KICKOS_BENCH
                KICKOS_SHUTDOWN_TO_BOOTLOADER KICKOS_MULTI_INSTANCE KICKOS_TLS
                CONFIG_SCHED_PERIODIC_TICK)
    if(DEFINED ${_flag})
      string(REGEX REPLACE "^CONFIG_" "" _sym "${_flag}")
      set(_val "n")
      if(${_flag})
        set(_val "y")
      endif()
      list(APPEND _kcfg_req "CONFIG_${_sym}=${_val}")
    endif()
  endforeach()

  kickos_kconfig_generate("${CMAKE_CURRENT_SOURCE_DIR}" "${KICKOS_DEFCONFIG}"
                          "${PROJECT_BINARY_DIR}/generated" "${_kcfg_req}")

  # Saved before the fragment include() below overwrites them.
  set(_cmake_board "${KICKOS_BOARD}")
  set(_cmake_arch "${KICKOS_ARCH}")
  set(_cmake_chip "${KICKOS_CHIP}")
  set(_cmake_family "${KICKOS_ARCH_FAMILY}")
  include("${PROJECT_BINARY_DIR}/generated/kickos_config.cmake")
  kickos_kconfig_agree("the board" "${_cmake_board}" "${KICKOS_BOARD}")
  kickos_kconfig_agree("the arch" "${_cmake_arch}" "${KICKOS_ARCH}")
  kickos_kconfig_agree("the arch family" "${_cmake_family}" "${KICKOS_ARCH_FAMILY}")
  kickos_kconfig_agree("the chip" "${_cmake_chip}" "${KICKOS_CHIP}")

  # .config is written during configure, before the build system, so depending on it does
  # not loop.
  set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" APPEND
               PROPERTY CMAKE_CONFIGURE_DEPENDS
               ${KICKOS_KCONFIG_SOURCES} "${PROJECT_BINARY_DIR}/generated/.config")
  message(STATUS "KickOS: configuration from ${KICKOS_DEFCONFIG}")

  kickos_find_kconfig(_kcfg_py _kcfg_found)
  if(_kcfg_found)
    add_custom_target(menuconfig
      COMMAND "${CMAKE_COMMAND}" -E env
              "KCONFIG_CONFIG=${PROJECT_BINARY_DIR}/generated/.config"
              "${_kcfg_py}" "${KICKOS_KCONFIG_BIN_MENUCONFIG}"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      USES_TERMINAL
      COMMENT "Editing ${PROJECT_BINARY_DIR}/generated/.config (rebuild to apply)")
    add_custom_target(savedefconfig
      COMMAND "${CMAKE_COMMAND}" -E env
              "KCONFIG_CONFIG=${PROJECT_BINARY_DIR}/generated/.config"
              "${_kcfg_py}" "${KICKOS_KCONFIG_BIN_SAVEDEFCONFIG}"
              --kconfig Kconfig --out "${KICKOS_DEFCONFIG}"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      COMMENT "Writing the minimal defconfig to ${KICKOS_DEFCONFIG}")
    add_custom_target(defconfig
      COMMAND "${CMAKE_COMMAND}" -E rm -f "${PROJECT_BINARY_DIR}/generated/.config"
      COMMAND "${_kcfg_py}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/kconfig/genconfig.py"
              "${CMAKE_CURRENT_SOURCE_DIR}" "${KICKOS_DEFCONFIG}"
              "${PROJECT_BINARY_DIR}/generated" ${_kcfg_req}
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      COMMENT "Reloading the live configuration from ${KICKOS_DEFCONFIG}")
  endif()
else()
  file(GLOB _variants
       "${CMAKE_CURRENT_SOURCE_DIR}/boards/${KICKOS_BOARD}/configs/*/defconfig")
  set(_have "")
  foreach(_v ${_variants})
    get_filename_component(_vdir "${_v}" DIRECTORY)
    get_filename_component(_vname "${_vdir}" NAME)
    list(APPEND _have "${_vname}")
  endforeach()
  message(FATAL_ERROR
    "Board '${KICKOS_BOARD}' has no configuration variant '${KICKOS_CONFIG_VARIANT}': "
    "${KICKOS_DEFCONFIG} does not exist. Variants it does ship: ${_have}. A preset "
    "selects a board and a variant and nothing else, so a new posture is a new "
    "defconfig under boards/${KICKOS_BOARD}/configs/, not a -D.")
endif()

# --- Config knobs (architecture.md) ----------------------------------------
# CMP0077 (NEW at this cmake_minimum_required) makes option() defer to the normal variables
# the fragment already set.
# Every knob below that becomes a compile definition is defined 0 or 1, never left
# undefined, so each `#if` site is -Wundef-clean.
option(CONFIG_SCHED_PERIODIC_TICK "Force a classic periodic scheduler tick" OFF)
option(KICKOS_BUILD_APPS          "Build in-tree demo applications"          ON)
option(KICKOS_BUILD_TESTS         "Register CTest cases"                     ON)
option(KICKOS_BUILD_INTEGRATION_TESTS
       "Register the CTest cases that RUN a KickOS image (sim, qemu, out-of-tree export)" ON)

set(_kickos_gtest_found OFF)
if(KICKOS_ARCH STREQUAL "sim")
  # GTest is IMPORTED for one configuration (Release, from Conan and distro packages alike)
  # and its interface properties are gated on $<CONFIG:Release>. Without this mapping a
  # MinSizeRel sim build compiles with no include path and fails as a missing gtest/gtest.h.
  foreach(_cfg MINSIZEREL RELWITHDEBINFO DEBUG)
    if(NOT DEFINED CMAKE_MAP_IMPORTED_CONFIG_${_cfg})
      set(CMAKE_MAP_IMPORTED_CONFIG_${_cfg} ${_cfg} Release "")
    endif()
  endforeach()
  find_package(GTest QUIET CONFIG)
  if(GTest_FOUND)
    set(_kickos_gtest_found ON)
  endif()
endif()
option(KICKOS_BUILD_UNIT_TESTS "Build the host unit tests (needs GTest)" ${_kickos_gtest_found})
if(KICKOS_BUILD_UNIT_TESTS AND NOT _kickos_gtest_found)
  if(NOT KICKOS_ARCH STREQUAL "sim")
    message(FATAL_ERROR
      "KICKOS_BUILD_UNIT_TESTS=ON on board '${KICKOS_BOARD}' (arch ${KICKOS_ARCH}). The host "
      "unit tests are native executables and are registered on the host arch only; a board "
      "build must never need GTest. Configure the sim preset to run them.")
  endif()
  message(FATAL_ERROR
    "KICKOS_BUILD_UNIT_TESTS=ON but find_package(GTest CONFIG) found nothing. Install it with "
    "Conan and point the configure at the generators folder:\n"
    "  conan install ${CMAKE_CURRENT_SOURCE_DIR}/conan/conanfile.py --output-folder=<gen> "
    "-s compiler.cppstd=20 --build=missing\n"
    "  cmake --preset sim -DCMAKE_PREFIX_PATH=<gen>")
endif()
if(KICKOS_ARCH STREQUAL "sim" AND NOT _kickos_gtest_found)
  message(STATUS
    "KickOS: GTest not found, host unit tests OFF. Get it from Conan "
    "(conan install conan/conanfile.py --output-folder=<gen>) and add <gen> to CMAKE_PREFIX_PATH.")
endif()
# option() writes the cache ONCE, so a directory first configured without GTest keeps
# KICKOS_BUILD_UNIT_TESTS=OFF after one is installed.
if(_kickos_gtest_found AND NOT KICKOS_BUILD_UNIT_TESTS)
  message(STATUS
    "KickOS: GTest was found but KICKOS_BUILD_UNIT_TESTS is OFF in the cache. Re-run with "
    "-DKICKOS_BUILD_UNIT_TESTS=ON, or configure a fresh build directory.")
endif()
# Exposes the guard-probe syscall (kos_guard_addr) used by the MPU privilege self-test;
# out of the production syscall ABI.
option(KICKOS_ENABLE_SELFTEST     "Expose test-only self-test syscalls"      OFF)
# PendSV brackets the switch body with DWT CYCCNT, which adds instructions to the switch
# path.
option(KICKOS_BENCH               "Cycle-accurate context-switch microbench"  OFF)
if(KICKOS_BENCH)
  add_compile_definitions(KICKOS_BENCH=1)
else()
  add_compile_definitions(KICKOS_BENCH=0)
endif()

if(KICKOS_ENABLE_SELFTEST)
  # Directory-wide so kernel dispatch, the user stub, and the app agree.
  add_compile_definitions(KICKOS_ENABLE_SELFTEST=1)
endif()

# --- Terminating images hand the chip back to its bootloader ----------------
# OFF: a terminating image halts, and reflashing an RP2040/RP2350 needs a physical BOOTSEL
# press. ON: both terminal dead-ends in kernel/init/console.cc call arch_reboot first, so
# the board returns to its bootloader at every termination, which also ends a soak run.
option(KICKOS_SHUTDOWN_TO_BOOTLOADER
       "Terminating images reboot into the chip bootloader instead of halting" OFF)
if(KICKOS_SHUTDOWN_TO_BOOTLOADER)
  if(NOT KICKOS_ENABLE_SELFTEST)
    message(FATAL_ERROR
      "KICKOS_SHUTDOWN_TO_BOOTLOADER=ON requires KICKOS_ENABLE_SELFTEST=ON: "
      "arch_reboot (the declining fallback in arch/common and every chip "
      "chip backend) is compiled out of a production image, so nothing would define "
      "the symbol. Use an -st preset, or leave this knob OFF.")
  endif()
  add_compile_definitions(KICKOS_SHUTDOWN_TO_BOOTLOADER=1)
  message(STATUS "KickOS: terminating images REBOOT TO BOOTLOADER (no BOOTSEL press; "
                 "a chip with no arch_reboot backend still halts)")
else()
  add_compile_definitions(KICKOS_SHUTDOWN_TO_BOOTLOADER=0)
endif()

# CMake's stock MinSizeRel flags drop -g.
add_compile_options($<$<CONFIG:MinSizeRel>:-g>)

# Internal-consistency guards (the intrusive-list membership checks in <kickos/list.h>).
# It changes an inline in a shipped header, so it is carried to consumers for the same ODR
# reason as KICKOS_HAVE_MPU.
option(KICKOS_DEBUG "Build the internal-consistency guards (list membership, ...)" OFF)
if(KICKOS_DEBUG)
  add_compile_definitions(KICKOS_DEBUG=1)
else()
  add_compile_definitions(KICKOS_DEBUG=0)
endif()

# --- Console backend selection (docs/reference/console.md) -----------------
# `chip` = the board's native transport (UART / semihosting / USIC); `rtt` = SEGGER RTT,
# which needs a debug probe.
# NEVER cached: a cache entry written from .config would come back as a request on the next
# configure and outrank a changed defconfig.
if(KICKOS_CONSOLE STREQUAL "chip")
  set(_con_chip 1)
  set(_con_rtt 0)
elseif(KICKOS_CONSOLE STREQUAL "rtt")
  set(_con_chip 0)
  set(_con_rtt 1)
elseif(KICKOS_CONSOLE STREQUAL "both")
  set(_con_chip 1)
  set(_con_rtt 1)
elseif(KICKOS_CONSOLE STREQUAL "none")
  set(_con_chip 0)
  set(_con_rtt 0)
else()
  message(FATAL_ERROR "KICKOS_CONSOLE must be chip|rtt|both|none (got '${KICKOS_CONSOLE}')")
endif()
# Bare-metal UART consoles need '\n' lowered to CR+LF. The sim must stay raw, else '\r'
# leaks into the TAP stream and breaks the test harness.
if(KICKOS_ARCH STREQUAL "sim")
  set(_con_crlf 0)
else()
  set(_con_crlf 1)
endif()
add_compile_definitions(KICKOS_CONSOLE_CHIP=${_con_chip} KICKOS_CONSOLE_RTT=${_con_rtt} KICKOS_CONSOLE_CRLF=${_con_crlf})
message(STATUS "KickOS: console=${KICKOS_CONSOLE} (chip=${_con_chip} rtt=${_con_rtt} crlf=${_con_crlf})")

# Several independent kernels in one address space (instance_local.h). Carried to consumers
# for the same ODR reason as KICKOS_HAVE_MPU: it changes kernel() in a shipped header. The
# COUNT rides in the generated board_config.h with the other provisioning knobs.
if(KICKOS_MULTI_INSTANCE)
  add_compile_definitions(KICKOS_MULTI_INSTANCE=1)
else()
  add_compile_definitions(KICKOS_MULTI_INSTANCE=0)
endif()

# The TLS stride. Unprivileged ARM code has no register that differs per thread except SP,
# so __aeabi_read_tp is SP masked down to the thread's own stack block, and one image has
# ONE mask: root and every pool stack must round to the SAME power of two.
include("${CMAKE_SOURCE_DIR}/cmake/boot_arena.cmake")
if(KICKOS_TLS)
  kickos_pow2_ceil("${KICKOS_USER_STACK_SIZE}" _tls_stride)
  kickos_pow2_ceil("${KICKOS_ROOT_STACK_SIZE}" _tls_root_stride)
  if(NOT _tls_stride EQUAL _tls_root_stride)
    message(FATAL_ERROR
      "KickOS: KICKOS_TLS is on and this board's stacks do not share a stride. "
      "KICKOS_USER_STACK_SIZE ${KICKOS_USER_STACK_SIZE} rounds to ${_tls_stride} and "
      "KICKOS_ROOT_STACK_SIZE ${KICKOS_ROOT_STACK_SIZE} rounds to ${_tls_root_stride}. "
      "The ARM thread pointer is SP masked down to the stack block and one image has one "
      "mask, so a thread on the smaller stride would read its neighbour's thread_local. "
      "State both at the same power of two in "
      "boards/${KICKOS_BOARD}/configs/<variant>/defconfig, or set KICKOS_TLS=n there and "
      "let a thread_local fail the link instead.")
  endif()
  # AND THEY MUST BE THE STRIDE, not merely round to it: tls_stack_admissible requires a
  # block of EXACTLY one stride, because two smaller blocks fit inside one stride and mask to
  # the same base. A non-power-of-two here has every spawn refused at runtime, not here.
  foreach(_k KICKOS_USER_STACK_SIZE KICKOS_ROOT_STACK_SIZE)
    if(NOT ${${_k}} EQUAL _tls_stride)
      message(FATAL_ERROR
        "KickOS: KICKOS_TLS is on and ${_k} is ${${_k}}, which is not a power of two: it "
        "rounds to a stride of ${_tls_stride}. The thread pointer is SP masked down to that "
        "stride and a block must BE one, because two smaller blocks fit inside a single "
        "stride and would mask to the same base. State ${_tls_stride} in "
        "boards/${KICKOS_BOARD}/configs/<variant>/defconfig, or set KICKOS_TLS=n there.")
    endif()
  endforeach()
  set(_tls_log2 0)
  set(_tls_p 1)
  while(_tls_p LESS _tls_stride)
    math(EXPR _tls_p "${_tls_p} * 2")
    math(EXPR _tls_log2 "${_tls_log2} + 1")
  endwhile()
  add_compile_definitions(KICKOS_TLS=1
                          KICKOS_TLS_STRIDE=${_tls_stride}
                          KICKOS_TLS_STRIDE_LOG2=${_tls_log2})
  message(STATUS "KickOS: TLS stride ${_tls_stride} (1 << ${_tls_log2})")
else()
  # Defined either way so `#if KICKOS_TLS` in a TU that forgot defined() reads false rather
  # than reading an undefined identifier as zero by luck. The knob reaches C HERE and not
  # through the generated board_config.h, which is why genconfig lists it among the bool knobs.
  add_compile_definitions(KICKOS_TLS=0)
endif()

# Positive identification only: a new arch must land in the 0 branch of every
# `#if KICKOS_ARCH_SIM`.
if(KICKOS_ARCH STREQUAL "sim")
  set(_arch_sim 1)
else()
  set(_arch_sim 0)
endif()
add_compile_definitions(KICKOS_ARCH_SIM=${_arch_sim})

# --- Telemetry backend selection (docs/reference/telemetry.md) -----------------
# `rtt` requires both a trace clock (arch_trace_now, KICKOS_HAVE_TRACE_CLOCK) and the RTT
# transport (KICKOS_CONSOLE in {rtt,both}); either missing is a hard configuration error.
# KICKOS_TRACE_ARCH MUST match the ArchId enum in include/kickos/trace/record.h, which the
# decoder tools/kicktrace.py follows; a static_assert there pins ARCH_RISCV == 5.
# The chip's caps.cmake is included FIRST so a chip value wins over the arch ladders below,
# which guard their set with NOT DEFINED. A board or preset that pre-defines
# KICKOS_HAVE_TRACE_CLOCK wins over both and must still agree with the Kconfig declaration.
if(KICKOS_CHIP)
  set(_caps_optin "${CMAKE_CURRENT_SOURCE_DIR}/arch/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/caps.cmake")
  if(EXISTS "${_caps_optin}")
    include("${_caps_optin}")
  endif()
endif()
# KICKOS_FAULT_ISOLATION is never overridable: a backend that defines
# arch_fault_is_user_thread / arch_fault_redirect_to_exit and is not listed here links BOTH
# its definition and the fallback into one archive, resolved by member order.
# A core with no privilege ring cannot tell a thread's own fault from a kernel bug in code it
# called, and must keep panicking.
# Per BOARD, not per arch: the ARMv6-M Unprivileged/Privileged Extension is OPTIONAL and
# separate from the MPU extension, Cortex-M0 omits it and Cortex-M0+ implements it, and both
# are KICKOS_ARCH=armv6m with no predefined macro between them (microbit's msr CONTROL is
# discarded; docs/reference/boards.md).
if(KICKOS_ARCH STREQUAL "armv6m")
  if(KICKOS_BOARD STREQUAL "microbit")
    set(KICKOS_HAVE_PRIV_RING 0) # nRF51822, Cortex-M0
  elseif(KICKOS_BOARD STREQUAL "picopi")
    set(KICKOS_HAVE_PRIV_RING 1) # RP2040, Cortex-M0+
  else()
    message(FATAL_ERROR
      "armv6m board '${KICKOS_BOARD}' is not classified. Cortex-M0 has no privilege ring "
      "and Cortex-M0+ does; state which this core is (see the table in "
      "docs/design-unprivileged-root.md section 2) rather than letting it be guessed.")
  endif()
elseif(KICKOS_ARCH STREQUAL "lx6")
  # PS.UM is 1 for kernel and thread alike and arch_context_init discards `privileged`.
  set(KICKOS_HAVE_PRIV_RING 0)
else()
  set(KICKOS_HAVE_PRIV_RING 1)
endif()

# sim is x86_64-only: arch_fault_is_user_thread reads a host layout it only knows there.
set(KICKOS_FAULT_ISOLATION 0)
set(KICKOS_FAULT_OUTCOME panic)
if(KICKOS_HAVE_PRIV_RING
    AND (KICKOS_ARCH STREQUAL "armv7m"
      OR KICKOS_ARCH STREQUAL "armv6m"
      OR KICKOS_ARCH STREQUAL "rv32imac"
      OR KICKOS_ARCH STREQUAL "rxv3"
      OR (KICKOS_ARCH STREQUAL "sim" AND CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "x86_64")))
  set(KICKOS_FAULT_ISOLATION 1)
  set(KICKOS_FAULT_OUTCOME thread-kill)
endif()
if(NOT DEFINED KICKOS_TRACE_ARCH)
  set(KICKOS_TRACE_ARCH 0)
  if(KICKOS_ARCH STREQUAL "armv7m")
    set(KICKOS_TRACE_ARCH 1)
  elseif(KICKOS_ARCH STREQUAL "armv6m")
    set(KICKOS_TRACE_ARCH 2)
  elseif(KICKOS_ARCH STREQUAL "lx6")
    set(KICKOS_TRACE_ARCH 3)
  elseif(KICKOS_ARCH STREQUAL "rxv3")
    set(KICKOS_TRACE_ARCH 4)
  elseif(KICKOS_ARCH STREQUAL "rv32imac")
    set(KICKOS_TRACE_ARCH 5)
  endif()
endif()
# rv32imac and armv6m are per-chip; see the HAS_TRACE_CLOCK help in Kconfig.
if(NOT DEFINED KICKOS_HAVE_TRACE_CLOCK)
  set(KICKOS_HAVE_TRACE_CLOCK 0)
  if(KICKOS_ARCH STREQUAL "sim" OR KICKOS_ARCH STREQUAL "armv7m"
     OR KICKOS_ARCH STREQUAL "rxv3" OR KICKOS_ARCH STREQUAL "lx6")
    set(KICKOS_HAVE_TRACE_CLOCK 1) # sim clock/1000; armv7m DWT fallback; rxv3 CMTW1; lx6 CCOUNT
  endif()
endif()

if(NOT KICKOS_HAVE_TRACE_CLOCK EQUAL KICKOS_TRACE_CLOCK_DECLARED)
  message(FATAL_ERROR
    "The two declarations of the trace clock disagree on board '${KICKOS_BOARD}' "
    "(arch=${KICKOS_ARCH} chip=${KICKOS_CHIP}): Kconfig resolves HAS_TRACE_CLOCK to "
    "${KICKOS_TRACE_CLOCK_DECLARED}, while arch/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/"
    "caps.cmake plus the arch ladder in CMakeLists.txt compute "
    "KICKOS_HAVE_TRACE_CLOCK=${KICKOS_HAVE_TRACE_CLOCK}. Fix whichever is wrong: the arch "
    "default and the per-chip `select HAS_TRACE_CLOCK` live in Kconfig and arch/Kconfig, "
    "the CMake half in the chip's caps.cmake (KICKOS_HAVE_TRACE_CLOCK) and the ladder "
    "above. A chip that gained or lost arch_trace_now has to be stated in both.")
endif()

# A chip declares an enforcing MPU by shipping arch/<family>/chip/<chip>/mpu.cmake, which
# also picks the PMSAv8 backend source; the sim enforces via host mprotect, with no chip and
# no linker script.
set(KICKOS_CHIP_ENFORCES_MPU OFF)
if(KICKOS_ARCH STREQUAL "sim")
  set(KICKOS_CHIP_ENFORCES_MPU ON)
elseif(KICKOS_CHIP)
  set(_mpu_optin "${CMAKE_CURRENT_SOURCE_DIR}/arch/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/mpu.cmake")
  if(EXISTS "${_mpu_optin}")
    include("${_mpu_optin}")
  endif()
endif()
if(KICKOS_HAVE_MPU AND NOT KICKOS_CHIP_ENFORCES_MPU)
  kickos_enforcing_mpu_boards(_kickos_mpu_boards)
  message(FATAL_ERROR
    "The configuration resolved the enforcing posture on board '${KICKOS_BOARD}' "
    "(arch=${KICKOS_ARCH} chip=${KICKOS_CHIP}), which ships no mpu.cmake, so the two "
    "declarations of one fact disagree: arch/Kconfig selects HAS_MPU for this chip and "
    "arch/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/mpu.cmake does not exist. "
    "Enforcement would be a silent no-op (arch_domain_static_regions grants zero "
    "regions because the linker script carves no KICKOS_HAVE_MPU .appdata window). "
    "Enforcement-capable boards today: ${_kickos_mpu_boards}. Either drop the select, "
    "or bring the port up: add a KICKOS_HAVE_MPU .appdata window to its linker script "
    "and drop a mpu.cmake (KICKOS_CHIP_ENFORCES_MPU ON) beside it once enforcement is "
    "validated on hardware.")
endif()

add_compile_definitions(KICKOS_HAVE_MPU=${KICKOS_HAVE_MPU})

# Which MPU design the v6-M/v7-M arch is built for. A chip's mpu.cmake above names its
# backend by setting KICKOS_ARM_MPU_BACKEND; PMSAv7 is the fallback for the core v6-M/v7-M
# fleet. The selector is TOTAL: a build with no MPU resolves to NONE rather than falling
# through to a backend it does not program, so every comparison below and in
# arch/arm/{armv6m,armv7m}/include/kickos/arch/mpu_encoded.h and arch/arm/common/ needs no
# KICKOS_HAVE_MPU conjunction to mean what it says.
if(NOT KICKOS_HAVE_MPU)
  set(_kickos_arm_mpu 0)
elseif(KICKOS_ARM_MPU_BACKEND STREQUAL "SYSMPU")
  set(_kickos_arm_mpu 3)
elseif(KICKOS_ARM_MPU_BACKEND STREQUAL "PMSAV8")
  set(_kickos_arm_mpu 2)
elseif(NOT KICKOS_ARM_MPU_BACKEND OR KICKOS_ARM_MPU_BACKEND STREQUAL "PMSAV7")
  set(_kickos_arm_mpu 1)
else()
  message(FATAL_ERROR
    "KICKOS_ARM_MPU_BACKEND must be unset, PMSAV7, PMSAV8 or SYSMPU on board "
    "'${KICKOS_BOARD}' (got '${KICKOS_ARM_MPU_BACKEND}')")
endif()
add_compile_definitions(
  KICKOS_ARM_MPU_NONE=0
  KICKOS_ARM_MPU_PMSAV7=1
  KICKOS_ARM_MPU_PMSAV8=2
  KICKOS_ARM_MPU_SYSMPU=3
  KICKOS_ARM_MPU=${_kickos_arm_mpu})

# --- Trap-handler IPC fastpath (KICKOS_ARCH_HAS_IPC_FASTPATH) ---------------
# Set by an arch backend that completes the register-carrying kos_call inside the syscall
# trap, redirecting the exception return into the target thread instead of into the
# privileged-thread trampoline. It gates three things that must not drift apart: the stub's
# register arm, arch_syscall_reg with its result-store seam, and the kernel's shared leaf.
#
# A backend declares it by shipping ipc_fastpath.cmake beside its switch.S, included here in
# this scope, so a plain set is what it reads. The value must be what that backend's trap
# implements, so the opt-in file is the only thing that sets it.
#
# The 0 arm names exactly two arches, sim and xtensa/lx6, each stating why at its own backend
# (arch/sim/sim.cc, arch/xtensa/lx6/arch_xtensa.cc).
set(KICKOS_ARCH_HAS_IPC_FASTPATH 0)
set(_ipc_fastpath_optin
  "${CMAKE_CURRENT_SOURCE_DIR}/arch/${KICKOS_ARCH_FAMILY}/${KICKOS_ARCH}/ipc_fastpath.cmake")
if(KICKOS_ARCH_FAMILY STREQUAL KICKOS_ARCH)
  # The sim's backend sits at arch/sim, with no family level.
  set(_ipc_fastpath_optin
    "${CMAKE_CURRENT_SOURCE_DIR}/arch/${KICKOS_ARCH}/ipc_fastpath.cmake")
endif()
if(EXISTS "${_ipc_fastpath_optin}")
  include("${_ipc_fastpath_optin}")
endif()
add_compile_definitions(KICKOS_ARCH_HAS_IPC_FASTPATH=${KICKOS_ARCH_HAS_IPC_FASTPATH})

# --- Per-arch caller-provided-stack floor (KICKOS_MIN_STACK_SIZE) -----------
# The floor must cover the arch's deepest dispatch, the thread-exit teardown, which runs on
# the CALLING thread's stack: a caller-provided stack below it passes the spawn check in
# syscall_thread.cc and then silently overflows on exit. An ARCH property, promptless in
# Kconfig; the measured figures and the red-zone relation are in the declaration there.
if(NOT DEFINED KICKOS_MIN_STACK_SIZE)
  message(FATAL_ERROR
    "KickOS: no KICKOS_MIN_STACK_SIZE. The generated kickos_config.cmake carries it, "
    "derived per arch in Kconfig, so an empty value here means the fragment was not "
    "read or the arch resolved to none.")
endif()
add_compile_definitions(KICKOS_MIN_STACK_SIZE=${KICKOS_MIN_STACK_SIZE})
message(STATUS "KickOS: caller-stack floor KICKOS_MIN_STACK_SIZE=${KICKOS_MIN_STACK_SIZE} (arch=${KICKOS_ARCH})")

# KICKOS_SERVICE_LIST names the CMake target that DEFINES kickos_board_services;
# KICKOS_SERVICE_LIST_LIBS is that target plus every driver and class leaf its start()s
# reference. They all back-reference kickos_user, so they must join the rescan link group
# below or the link fails on a strongly-connected-component cycle naming the driver lib.
# The closure is computed further down, once add_subdirectory(system) has created those
# targets; a custom provider may pre-set KICKOS_SERVICE_LIST_LIBS here with its own deps.
message(STATUS "KickOS: service list = ${KICKOS_SERVICE_LIST}")

# The USB device clock tree (PLL_USB, clk_usb, the USB block reset) is brought up in chip
# init, only for an image that carries a USB driver. Never cached: caching it would keep the
# clock tree on in a build directory whose service list was later switched back.
if(NOT DEFINED KICKOS_USB_CONSOLE)
  set(KICKOS_USB_CONSOLE OFF)
  # Unanchored: any list with a "_usbcdc" stem needs the same clock tree. An anchored match
  # would leave PLL_USB off on a suffixed variant and the device would never enumerate.
  if(KICKOS_SERVICE_LIST MATCHES "_usbcdc")
    set(KICKOS_USB_CONSOLE ON)
  endif()
endif()
if(KICKOS_USB_CONSOLE)
  add_compile_definitions(KICKOS_USB_CONSOLE=1)
  message(STATUS "KickOS: USB device clock tree = on (chip init brings up PLL_USB)")
endif()

# KICKOS_BOARD_PINMAP names the CMake target that DEFINES kickos_board_pinmap; it
# back-references kickos_user and is read by kickos_default_init, so it must join the rescan
# link group below. Each provider is a pure data lib with no further deps, so the closure is
# just the selected target; a custom provider may pre-set KICKOS_BOARD_PINMAP_LIBS.
if(NOT DEFINED KICKOS_BOARD_PINMAP_LIBS)
  set(KICKOS_BOARD_PINMAP_LIBS "${KICKOS_BOARD_PINMAP}")
endif()
message(STATUS "KickOS: board pin-map = ${KICKOS_BOARD_PINMAP} (libs: ${KICKOS_BOARD_PINMAP_LIBS})")

# RX MPU wedge-localizer trace (arch/rx/rxv3/arch_rxv3.cc): a raw polled SCI6 marker,
# forwarded to the compiler only when set on the command line.
if(DEFINED KICKOS_RX_MPU_TRACE)
  add_compile_definitions(KICKOS_RX_MPU_TRACE=${KICKOS_RX_MPU_TRACE})
endif()

if(KICKOS_TELEMETRY STREQUAL "off")
  set(_telem 0)
elseif(KICKOS_TELEMETRY STREQUAL "rtt")
  set(_telem 1)
  if(NOT KICKOS_HAVE_TRACE_CLOCK)
    message(FATAL_ERROR "KICKOS_TELEMETRY=rtt requires a trace clock (arch_trace_now), "
      "but board '${KICKOS_BOARD}' (arch=${KICKOS_ARCH} chip=${KICKOS_CHIP}) does not "
      "provide one (KICKOS_HAVE_TRACE_CLOCK unset)")
  endif()
  if(NOT (KICKOS_CONSOLE STREQUAL "rtt" OR KICKOS_CONSOLE STREQUAL "both"))
    message(FATAL_ERROR "KICKOS_TELEMETRY=rtt needs the RTT transport: set "
      "KICKOS_CONSOLE=rtt or both (got '${KICKOS_CONSOLE}')")
  endif()
else()
  message(FATAL_ERROR "KICKOS_TELEMETRY must be off|rtt (got '${KICKOS_TELEMETRY}')")
endif()
add_compile_definitions(KICKOS_TELEMETRY=${_telem} KICKOS_TELEMETRY_RTT=${_telem}
                        KICKOS_TRACE_ARCH=${KICKOS_TRACE_ARCH})
set(KICKOS_TELEMETRY_ON ${_telem}) # consumed by component CMakeLists (asm/FP flags)
message(STATUS "KickOS: telemetry=${KICKOS_TELEMETRY} (have_trace_clock=${KICKOS_HAVE_TRACE_CLOCK} arch_id=${KICKOS_TRACE_ARCH})")

# RTT publish barrier override (docs/reference/telemetry.md G6): a weakly-ordered core
# injects its real fence here; unset keeps the compiler-only barrier in rtt.h. Pass the FULL
# call form, e.g. -DKICKOS_RTT_PUBLISH_BARRIER_DEF="__atomic_thread_fence(__ATOMIC_RELEASE)".
if(DEFINED KICKOS_RTT_PUBLISH_BARRIER_DEF)
  add_compile_definitions("KICKOS_RTT_PUBLISH_BARRIER()=${KICKOS_RTT_PUBLISH_BARRIER_DEF}")
endif()

# Passed as a BARE token (-DKICKOS_APP_TZ=+0200), not a quoted string: embedded escaped
# quotes double up on a reconfigure of an existing build dir, so app.h stringizes the token
# in C. `date` failing leaves the define out, and app.h stays offset-free.
execute_process(COMMAND date +%z OUTPUT_VARIABLE _apptz OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT _apptz STREQUAL "")
  add_compile_definitions(KICKOS_APP_TZ=${_apptz})
endif()

# Must precede the components: their $<INSTALL_INTERFACE:...> uses CMAKE_INSTALL_INCLUDEDIR.
include(GNUInstallDirs)

if(KICKOS_BUILD_TESTS)
  enable_testing()

  if(KICKOS_BUILD_UNIT_TESTS)
    include(GoogleTest)
  endif()

  # The panic/fault marker set, read from the one file this and tests/lib/gate.sh both read
  # (same ERE dialect). It must not contain a `;`: FAIL_REGULAR_EXPRESSION is a CMake list
  # of regexes.
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/tests/lib/panic.ere" KICKOS_PANIC_REGEX LIMIT_COUNT 1)
  if(KICKOS_PANIC_REGEX STREQUAL "")
    message(FATAL_ERROR "tests/lib/panic.ere is empty; the sim panic gates would register "
                        "an empty regex and no fault would ever fail a test")
  endif()

  # One CTest fixture (SETUP) so no test can run a stale binary; every test registered below
  # is auto-wired to require it by the add_test wrapper just after.
  add_test(NAME kickos_build COMMAND "${CMAKE_COMMAND}" --build "${PROJECT_BINARY_DIR}")
  set_tests_properties(kickos_build PROPERTIES FIXTURES_SETUP kickos_build TIMEOUT 900)

  # A macro, not a function, so set_property runs in the caller's directory scope, the only
  # scope where the test it just added exists. kickos_build is registered above this wrapper
  # and so never requires itself. Every test here uses the add_test(NAME <n> COMMAND ...) form.
  macro(add_test)
    _add_test(${ARGV})
    if("${ARGV0}" STREQUAL "NAME")
      set(_kickos_test_name "${ARGV1}")
    else()
      set(_kickos_test_name "${ARGV0}")
    endif()
    set_property(TEST "${_kickos_test_name}" APPEND PROPERTY FIXTURES_REQUIRED kickos_build)
    if(NOT KICKOS_BUILD_INTEGRATION_TESTS)
      # cmake_language(DEFER CALL) expands its arguments when the call RUNS, so a per-test
      # defer would see the LAST name registered in the directory and decline only that one.
      set_property(DIRECTORY APPEND PROPERTY kickos_declinable "${_kickos_test_name}")
      get_property(_kickos_armed DIRECTORY PROPERTY kickos_decline_armed)
      if(NOT _kickos_armed)
        set_property(DIRECTORY PROPERTY kickos_decline_armed ON)
        # End of this directory: the earliest point where every site's own
        # set_tests_properties has run and the `host` label is readable.
        cmake_language(DEFER CALL kickos_decline_image_tests)
      endif()
    endif()
  endmacro()

  function(kickos_decline_image_tests)
    get_property(_names DIRECTORY PROPERTY kickos_declinable)
    foreach(_name IN LISTS _names)
      get_test_property("${_name}" LABELS _labels)
      if(NOT "host" IN_LIST _labels)
        set_tests_properties("${_name}" PROPERTIES DISABLED TRUE)
      endif()
    endforeach()
  endfunction()

  # Which side each test program falls on is DECLARED in tests/static/test_classes.txt and
  # gated by test_labels below.
  # Fixtures are resolved AFTER label filtering, so `kickos_build` carries no label and
  # appears in BOTH partitions.

  # Registered with _add_test, so it does not require the build fixture: the gate reads the
  # source tree through `git ls-files` and never opens the build directory. WORKING_DIRECTORY
  # because the script refuses to run anywhere but the repo root.
  _add_test(NAME doc_names
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_doc_names.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(doc_names PROPERTIES TIMEOUT 120 LABELS host)

  _add_test(NAME extern_c_linkage
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_extern_c_linkage.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(extern_c_linkage PROPERTIES TIMEOUT 120 LABELS host)

  _add_test(NAME ascii
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_ascii.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(ascii PROPERTIES TIMEOUT 120 LABELS host)

  _add_test(NAME spdx
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_spdx.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(spdx PROPERTIES TIMEOUT 120 LABELS host)

  _add_test(NAME include_guards
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_include_guards.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(include_guards PROPERTIES TIMEOUT 120 LABELS host)

  # Takes the GENERATED board config, not ${KICKOS_NUM_CORES}: the gate must read the knob
  # spelling C reads.
  _add_test(NAME cpu_id_fold
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_cpu_id_fold.sh"
                    "${PROJECT_BINARY_DIR}/generated/include/kickos/board_config.h"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(cpu_id_fold PROPERTIES TIMEOUT 120 LABELS host)

  _add_test(NAME forever_loop
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_forever_loop.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(forever_loop PROPERTIES TIMEOUT 120 LABELS host)

  _add_test(NAME dash_punct
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_dash_punct.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(dash_punct PROPERTIES TIMEOUT 120 LABELS host)

  # A banner tests/lib/panic.ere misses turns every assert_no_panic into a pass on a board
  # that died.
  _add_test(NAME panic_banners
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_panic_banners.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(panic_banners PROPERTIES TIMEOUT 120 LABELS host)

  # The death path's seating on the per-thread kernel block. Registered on every preset: it
  # derives which arches owe the arm from arch/Kconfig and reads source, so a board whose own
  # KICKOS_KERNEL_STACKS is 0 still gates the four backends that carry it.
  _add_test(NAME death_stack_seating
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_death_stack_seating.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(death_stack_seating PROPERTIES TIMEOUT 120 LABELS host)

  # --- The trap red-zone gate --------------------------------------------------
  # It configures and cross-builds a scratch tree of its own, so it is keyed on the PRESET
  # (board plus variant) and not on this build directory. A pair it does not declare is
  # refused by the gate itself, so an unlisted variant registers nothing rather than
  # measuring the wrong configuration.
  #
  # trap_redzone_roots.txt declares which (board, variant) pairs are measurable, and states
  # the rule for it. This block DERIVES the preset name and asks that file whether it is
  # declared, so a new preset registers by being declared once rather than in two places.
  #
  # THE NAMING RULE IS THE PRESETS' OWN: the base variant is the board name and every other
  # variant is <board>-<variant>, which holds for all 52 pairs in cmake/presets/*.json. A
  # preset that broke the rule would fail to configure under its derived name rather than
  # register the wrong tree, the gate taking the name as an argument.
  set(_fs_redzone_roots "${PROJECT_SOURCE_DIR}/tests/static/trap_redzone_roots.txt")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_fs_redzone_roots}")
  if(KICKOS_CONFIG_VARIANT STREQUAL "base")
    set(_fs_redzone_candidate "${KICKOS_BOARD}")
  else()
    set(_fs_redzone_candidate "${KICKOS_BOARD}-${KICKOS_CONFIG_VARIANT}")
  endif()
  set(_fs_redzone_preset "")
  # REGEX rather than a substring test: "preset armv7m qemu" must not match qemu-flat, and the
  # trailing anchor is what stops it.
  file(STRINGS "${_fs_redzone_roots}" _fs_redzone_decls
       REGEX "^preset[ \t]+${KICKOS_ARCH}[ \t]+${_fs_redzone_candidate}[ \t]*$")
  if(NOT _fs_redzone_decls STREQUAL "")
    set(_fs_redzone_preset "${_fs_redzone_candidate}")
  endif()
  if(NOT _fs_redzone_preset STREQUAL "")
    # _add_test, not the wrapper: the gate builds a tree of its own, so the kickos_build
    # fixture would only build one it never opens.
    _add_test(NAME trap_redzone
              COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_trap_redzone.sh"
                      "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}"
                      "${_fs_redzone_preset}" "${KICKOS_ARCH}")
    # The timeout covers a cold cross build of the scratch tree.
    set_tests_properties(trap_redzone PROPERTIES TIMEOUT 1800 LABELS host)
  endif()

  # Outside the _fs_redzone_preset guard on purpose: it checks that the declaration set
  # covers the fleet, which is exactly the question a board whose preset is UNDECLARED cannot
  # ask by registering trap_redzone. It configures nothing and needs no cross toolchain.
  _add_test(NAME trap_redzone_decls
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_trap_redzone_decls.sh"
                    "${CMAKE_COMMAND}" "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(trap_redzone_decls PROPERTIES TIMEOUT 120 LABELS host)

  _add_test(NAME atomic_rmw
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_atomic_rmw.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(atomic_rmw PROPERTIES TIMEOUT 120 LABELS host)

  _add_test(NAME service_lists
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_service_lists.sh"
                    "${CMAKE_COMMAND}" "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(service_lists PROPERTIES TIMEOUT 120 LABELS host)

  # Unconditional: kickos_kconfig_generate above already FATALs on a box without the
  # interpreter, so this cannot be reached without one.
  kickos_find_kconfig(_kconfig_py _kconfig_found)
  _add_test(NAME kconfig_gen
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_kconfig_gen.sh"
                    "${_kconfig_py}" "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(kconfig_gen PROPERTIES TIMEOUT 120 LABELS host)

  # Reads the forwarding lists above out of this very file, so it is registered from every
  # board and never from a generated copy.
  _add_test(NAME kconfig_forwarding
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_kconfig_forwarding.sh"
                    "${_kconfig_py}" "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(kconfig_forwarding PROPERTIES TIMEOUT 120 LABELS host)

  # Unlike the gates above this one DOES require the build fixture: it classifies each test
  # by the program ctest resolved for it, and ctest emits no command at all for a program
  # that is not on disk yet.
  set(_kickos_declined no)
  if(NOT KICKOS_BUILD_INTEGRATION_TESTS)
    set(_kickos_declined yes)
  endif()
  # <complete> asserts this tree registers EVERY build-rooted program test_classes.txt names.
  set(_kickos_complete no)
  if(KICKOS_ARCH STREQUAL "sim" AND KICKOS_BUILD_UNIT_TESTS)
    set(_kickos_complete yes)
  endif()
  _add_test(NAME test_labels
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_test_labels.sh"
                    "${CMAKE_CTEST_COMMAND}" "${CMAKE_COMMAND}"
                    "${PROJECT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}"
                    "${_kickos_declined}" "${_kickos_complete}")
  set_tests_properties(test_labels PROPERTIES TIMEOUT 60 LABELS host
                       FIXTURES_REQUIRED kickos_build)
endif()

foreach(_dir lib arch kernel user system)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_dir}/CMakeLists.txt")
    add_subdirectory(${_dir})
  endif()
endforeach()

# A host tool.
if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/telemetry/CMakeLists.txt")
  add_subdirectory(tests/unit/telemetry)
endif()

# Host-only: which slot an object lands in is visible only inside the kernel, so a host
# program is what observes it.
if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/slotpool/CMakeLists.txt")
  add_subdirectory(tests/unit/slotpool)
endif()

# Host-only; the reason is in tests/unit/kstring/CMakeLists.txt.
if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/kstring/CMakeLists.txt")
  add_subdirectory(tests/unit/kstring)
endif()

# Host-only; the reason is in tests/unit/ktime/CMakeLists.txt.
if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/ktime/CMakeLists.txt")
  add_subdirectory(tests/unit/ktime)
endif()

# Host-only; the reason is in tests/unit/tlscarve/CMakeLists.txt.
if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/tlscarve/CMakeLists.txt")
  add_subdirectory(tests/unit/tlscarve)
endif()

# Host-only: the chunk directory mapping and the free-slot list length are kernel-internal,
# so a host program is what observes them. The per-geometry split is in
# tests/unit/captable/CMakeLists.txt.
if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/captable/CMakeLists.txt")
  add_subdirectory(tests/unit/captable)
endif()

# Host-only; the reason is in tests/unit/uartclass/CMakeLists.txt.
if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/uartclass/CMakeLists.txt")
  add_subdirectory(tests/unit/uartclass)
endif()

# Host-only; the reason is in tests/unit/capreply/CMakeLists.txt.
if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/capreply/CMakeLists.txt")
  add_subdirectory(tests/unit/capreply)
endif()

# Host-only: ARCH_MPU_NOCACHE_REFUSED is reachable only from a host build, so this gate is
# where the refusal is exercised.
if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/grantnocache/CMakeLists.txt")
  add_subdirectory(tests/unit/grantnocache)
endif()

# Host-only: every arm needs a syscall that FAILS on demand, and the readiness poll needs
# its full sleep budget to cost nothing, both of which the host seam provides.
if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/drvbringup/CMakeLists.txt")
  add_subdirectory(tests/unit/drvbringup)
endif()

# Host-only; the reason is in tests/unit/consoleown/CMakeLists.txt.
if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/consoleown/CMakeLists.txt")
  add_subdirectory(tests/unit/consoleown)
endif()

# The K-seam host fixture must precede its consumers below: they call the function it
# defines.
if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/kfixture/CMakeLists.txt")
  add_subdirectory(tests/unit/kfixture)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/schedwake/CMakeLists.txt")
    add_subdirectory(tests/unit/schedwake)
  endif()
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/taskdeath/CMakeLists.txt")
    add_subdirectory(tests/unit/taskdeath)
  endif()
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/parkresult/CMakeLists.txt")
    add_subdirectory(tests/unit/parkresult)
  endif()
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/capsweep/CMakeLists.txt")
    add_subdirectory(tests/unit/capsweep)
  endif()
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/capprobe/CMakeLists.txt")
    add_subdirectory(tests/unit/capprobe)
  endif()
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/cancelkind/CMakeLists.txt")
    add_subdirectory(tests/unit/cancelkind)
  endif()
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/faultband/CMakeLists.txt")
    add_subdirectory(tests/unit/faultband)
  endif()
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/slayhook/CMakeLists.txt")
    add_subdirectory(tests/unit/slayhook)
  endif()
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/wildstack/CMakeLists.txt")
    add_subdirectory(tests/unit/wildstack)
  endif()
endif()

# Host-only; the reason is in tests/unit/consoletx/CMakeLists.txt. Not a K-seam consumer:
# it compiles kernel/init/console_tx.cc with its own arch seam, and the K-seam's would be a
# second definition of arch_irq_save.
if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/consoletx/CMakeLists.txt")
  add_subdirectory(tests/unit/consoletx)
endif()

# Set before kickos_core below, whose LINK_GROUP pulls the provider archive.
set(KICKOS_INIT_PROVIDER "kickos_default_init" CACHE STRING
  "CMake target providing kickos_init_entry (the userspace init service). Default = the built-in kickos_system service; a power user names their own target.")

if(TARGET kickos_kernel)
  # Consumers link a leaf (kickos / kickos_cxx), never kickos_core directly.
  add_library(kickos_core INTERFACE)
  # Group order below: the kernel references kickos_init_entry across the rescan boundary, so
  # KICKOS_INIT_PROVIDER sits right after kickos_kernel; KICKOS_SERVICE_LIST_LIBS follows it
  # because the list TU's drivers back-reference kickos_user and a class leaf. The whole
  # closure must be in-group to rescan-resolve.
  # The closure is derived HERE and not at the knob: the walk reads the providers' and
  # drivers' own link edges, which exist only after add_subdirectory(system) above.
  if(NOT DEFINED KICKOS_SERVICE_LIST_LIBS)
    kickos_service_libs_closure("${KICKOS_SERVICE_LIST}" KICKOS_SERVICE_LIST_LIBS)
    message(STATUS "KickOS: service-list closure = ${KICKOS_SERVICE_LIST_LIBS}")
  endif()
  # CMake forbids one target's closure from carrying a library in two groups, so the group is
  # built per posture leaf below, where they differ in which toolchain runtime joins them.
  if(KICKOS_CHIP AND TARGET kickos_chip_${KICKOS_CHIP})
    set(_kickos_group "kickos_user,kickos_kernel,${KICKOS_INIT_PROVIDER},${KICKOS_SERVICE_LIST_LIBS},${KICKOS_BOARD_PINMAP_LIBS},kickos_chip_${KICKOS_CHIP},kickos_arch_${KICKOS_ARCH},kickos_lib")
  else()
    set(_kickos_group "kickos_user,kickos_kernel,${KICKOS_INIT_PROVIDER},${KICKOS_SERVICE_LIST_LIBS},${KICKOS_BOARD_PINMAP_LIBS},kickos_arch_${KICKOS_ARCH},kickos_lib")
  endif()
  # kickos_system is INTERFACE (contributes no archive), so it must NOT sit in the RESCAN
  # group; link it separately.
  target_link_libraries(kickos_core INTERFACE kickos_system)
  # The app writes `int main`, renamed to the kernel-called symbol; app.h is force-included
  # below so the renamed C++ main gets C linkage (unmangled).
  target_compile_definitions(kickos_core INTERFACE main=kickos_app_main)
  # ODR: an out-of-tree TU including a kernel-internal header must see the same posture as
  # the prebuilt libs. KICKOS_TELEMETRY gates fields in struct Kernel (instance.h) and
  # arch_context (context.h), KICKOS_ARCH_SIM gates one in struct Thread (thread.h),
  # KICKOS_HAVE_MPU branches KOS_STACK_DEFINE (kos.h),
  # KICKOS_ARM_MPU (plus the four name constants it is compared against) picks the
  # arch_mpu_encoded layout, and without KICKOS_ARCH_SIM a sim package (which ships no
  # chip_limits.h) hits config/board.h's #error.
  set(_kos_iface_sim 0)
  if(KICKOS_ARCH STREQUAL "sim")
    set(_kos_iface_sim 1)
  endif()
  target_compile_definitions(kickos_core INTERFACE
    KICKOS_TELEMETRY=${_telem} KICKOS_TELEMETRY_RTT=${_telem}
    KICKOS_TRACE_ARCH=${KICKOS_TRACE_ARCH}
    KICKOS_HAVE_MPU=${KICKOS_HAVE_MPU}
    KICKOS_ARM_MPU_NONE=0
    KICKOS_ARM_MPU_PMSAV7=1
    KICKOS_ARM_MPU_PMSAV8=2
    KICKOS_ARM_MPU_SYSMPU=3
    KICKOS_ARM_MPU=${_kickos_arm_mpu}
    KICKOS_ARCH_SIM=${_kos_iface_sim}
    KICKOS_MULTI_INSTANCE=$<BOOL:${KICKOS_MULTI_INSTANCE}>
    KICKOS_DEBUG=$<BOOL:${KICKOS_DEBUG}>)
  target_compile_options(kickos_core INTERFACE -include kickos/app.h)
  # Force-include the seam header so an app-TU-defined kickos_init_entry gets C linkage.
  # SHELL: keeps this second -include from being de-duplicated against the app.h one above.
  target_compile_options(kickos_core INTERFACE "SHELL:-include kickos/sys/init.h")
  if(KICKOS_ARCH STREQUAL "sim")
    find_package(Threads REQUIRED)
    target_link_libraries(kickos_core INTERFACE Threads::Threads rt)
  endif()
  # The force-refs below name objects nothing references, which --gc-sections would
  # otherwise drop.
  if(NOT (KICKOS_ARCH STREQUAL "sim") AND KICKOS_LINKER_SCRIPT)
    target_link_options(kickos_core INTERFACE
      -nostartfiles
      -Wl,--gc-sections
      -Wl,--no-warn-rwx-segments
      # Every input section must be named by a rule in the chip script. An unnamed one is a
      # link failure printing the section, not a warning: an ALLOC orphan lands at an address
      # nobody chose. arch/common/sections.ld.h carries the shared vocabulary.
      -Wl,--orphan-handling=error
      # ARM's vector table (g_isr_vector) is a data object nothing references. RX pulls its
      # vector tables via ENTRY(_start) + KEEP(), and forcing a symbol it does not define
      # would leave a dangling undefined.
      "$<$<STREQUAL:${KICKOS_ARCH_FAMILY},arm>:-Wl,-u,g_isr_vector>"
      # Force our libc syscall stubs in (libc's raise->_exit). The RX psABI prefixes
      # C identifiers with a leading underscore, so C `_exit` is asm `__exit`; forcing
      # `_exit` there would pull nothing and drop newlib_stubs.o (RX sbrk/__dso_handle).
      "$<$<STREQUAL:${KICKOS_ARCH_FAMILY},rx>:-Wl,-u,__exit>"
      "$<$<NOT:$<STREQUAL:${KICKOS_ARCH_FAMILY},rx>>:-Wl,-u,_exit>"
      # A chip with a checksummed flash second stage (RP2040 boot2) must force its
      # .boot2 blob object in too; nothing references it.
      $<$<BOOL:${KICKOS_BOOT2_FORCE_SYMBOL}>:-Wl,-u,${KICKOS_BOOT2_FORCE_SYMBOL}>
      "$<BUILD_INTERFACE:SHELL:-T ${KICKOS_LINKER_SCRIPT}>"
      "$<INSTALL_INTERFACE:SHELL:-T $<INSTALL_PREFIX>/${CMAKE_INSTALL_LIBDIR}/kickos/${KICKOS_CHIP}.ld>")
    # ...and the same script as a real link DEPENDENCY: -T is an opaque driver option, so
    # without this an edited .ld does not relink and a stale image gets flashed. The paths
    # must mirror the -T above, and INTERFACE_LINK_DEPENDS rides the export.
    set_property(TARGET kickos_core APPEND PROPERTY INTERFACE_LINK_DEPENDS
      "$<BUILD_INTERFACE:${KICKOS_LINKER_SCRIPT}>"
      "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_LIBDIR}/kickos/${KICKOS_CHIP}.ld>")
  endif()

  # A pure-C / freestanding app links `kickos`; a full-C++ (exceptions/STL/RTTI) app links
  # `kickos_cxx`.
  add_library(kickos INTERFACE)
  target_link_libraries(kickos INTERFACE kickos_core)
  add_library(kickos_cxx INTERFACE)
  target_link_libraries(kickos_cxx INTERFACE kickos_core)
  # On MCU the toolchain runtime must join the group: newlib's `_sbrk_r` references `_sbrk`
  # (provided on demand by libkickos_user.a), and the g++ driver appends its libraries after
  # everything CMake emits, so outside the group the linker meets `_sbrk_r` too late to go
  # back for the member.
  if(KICKOS_ARCH STREQUAL "sim")
    target_link_libraries(kickos INTERFACE "$<LINK_GROUP:RESCAN,${_kickos_group}>")
    target_link_libraries(kickos_cxx INTERFACE "$<LINK_GROUP:RESCAN,${_kickos_group}>")
  else()
    target_link_libraries(kickos INTERFACE "$<LINK_GROUP:RESCAN,${_kickos_group},c>")
    target_link_libraries(kickos_cxx INTERFACE
      "$<LINK_GROUP:RESCAN,${_kickos_group},stdc++,supc++,c>")
  endif()
  # In-tree TUs get __KickOS__ from the add_compile_definitions above; this is what carries
  # it to find_package consumers.
  target_compile_definitions(kickos_core INTERFACE __KickOS__=1)
  if(NOT (KICKOS_ARCH STREQUAL "sim"))
    # Freestanding (default): -nostdlib++ drops libsupc++/libstdc++, so a stray operator new
    # or EH table fails the link loud.
    target_compile_options(kickos INTERFACE
      "$<$<COMPILE_LANGUAGE:CXX>:${KICKOS_FREESTANDING_CXX_FLAGS}>")
    target_link_options(kickos INTERFACE -nostdlib++)
    # Full C++: omitting -nostdlib++ lets the g++ driver auto-link the toolchain
    # libstdc++/libsupc++ (EH, RTTI, new/delete) over newlib.
    target_compile_options(kickos_cxx INTERFACE
      "$<$<COMPILE_LANGUAGE:CXX>:-fexceptions;-frtti>")
    # A second rescan group, one layer out: the driver-appended libstdc++'s operator
    # new must be able to reach down to `_sbrk`, so the chain needs a group holding
    # all three providers. Listed after kickos_core so the core group resolves first.
    target_link_libraries(kickos_cxx INTERFACE
      "$<LINK_GROUP:RESCAN,stdc++,supc++,c,kickos_user>")

    # A strong __gnu_cxx::__verbose_terminate_handler keeps the linker from extracting
    # libstdc++'s vterminate.o (~65K of demangle + float dtoa). It must be a real object on
    # the link line, not an archive member, to win over the driver-appended libstdc++.
    # INTERFACE flags on kickos_cxx do not reach this object lib's own compile, hence the
    # direct flags below; this TU needs -fexceptions -frtti to compile at all.
    add_library(kickos_cxx_rt OBJECT system/cxx/vterminate.cc)
    target_compile_features(kickos_cxx_rt PRIVATE cxx_std_20)
    target_compile_options(kickos_cxx_rt PRIVATE ${KICKOS_WARN_FLAGS} -fexceptions -frtti)
    # An object library links nothing: kickos_user/kickos_kernel here are usage
    # requirements (include dirs) only, and PRIVATE keeps them off consumers.
    target_link_libraries(kickos_cxx_rt PRIVATE kickos_core kickos_user kickos_kernel)
    target_link_libraries(kickos_cxx INTERFACE $<TARGET_OBJECTS:kickos_cxx_rt>)
  endif()

  # kickos_core is a link dependency of both leaves, so it must be in the export set.
  install(TARGETS kickos_core kickos kickos_cxx EXPORT KickOSTargets)
  # $<TARGET_OBJECTS:kickos_cxx_rt> lands verbatim in KickOSTargets.cmake, so the
  # object lib must be in the export set or an OOT find_package(KickOS) breaks.
  if(TARGET kickos_cxx_rt)
    install(TARGETS kickos_cxx_rt EXPORT KickOSTargets
      OBJECTS DESTINATION "${CMAKE_INSTALL_LIBDIR}/kickos")
  endif()
endif()

# Apps need a chip target (or the sim); a board whose chip is named but not yet brought up
# builds the libraries only.
if(KICKOS_BUILD_APPS
   AND ((KICKOS_ARCH STREQUAL "sim") OR (KICKOS_CHIP AND TARGET kickos_chip_${KICKOS_CHIP}))
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/user/apps/CMakeLists.txt")
  add_subdirectory(user/apps)
endif()

# Validated LATE: a provider defined in user/apps or a superproject is not a target yet at
# the kickos_core site above. Guarded on kickos_core so a libraries-only configure does not
# demand a provider it never links.
if(TARGET kickos_core)
  if(NOT TARGET ${KICKOS_INIT_PROVIDER})
    message(FATAL_ERROR "KICKOS_INIT_PROVIDER='${KICKOS_INIT_PROVIDER}' is not a CMake target. Name a target that defines kickos_init_entry, or unset to use the default kickos_default_init.")
  endif()
  if(NOT KICKOS_INIT_PROVIDER STREQUAL "kickos_default_init")
    message(STATUS "KickOS: init provider = ${KICKOS_INIT_PROVIDER} (non-default). "
      "For an installed/exported package it must also be install(EXPORT KickOSTargets).")
  endif()
  # Late for the same reason: a service-list provider under system/init/<board>/ is not a
  # target at the kickos_core site above.
  if(NOT TARGET ${KICKOS_SERVICE_LIST})
    message(FATAL_ERROR "KICKOS_SERVICE_LIST='${KICKOS_SERVICE_LIST}' is not a CMake target. Name a target that defines kickos_board_services, or use the default kickos_services_none (count = 0).")
  endif()
  # Without this a misspelled target name reaches the LINK_GROUP as a bare -l, so the board
  # silently keeps kickos_pinmap_none and boots with unmuxed pins.
  if(NOT TARGET ${KICKOS_BOARD_PINMAP})
    message(FATAL_ERROR "KICKOS_BOARD_PINMAP='${KICKOS_BOARD_PINMAP}' is not a CMake target. Name a target that defines kos_board_pinmap, or use the default kickos_pinmap_none.")
  endif()
endif()

# Must stay AFTER add_subdirectory(user/apps): that is the last place an app can declare its
# peak.
kickos_cap_table_resolve("${KICKOS_SERVICE_LIST}" KICKOS_CAP_TABLE_SLOTS
                         KICKOS_CAP_TABLE_CHUNK KICKOS_CAP_TABLE_CHILD_WIDTH
                         KICKOS_CAP_TABLE_REPLY_MAX)
configure_file("${PROJECT_SOURCE_DIR}/kernel/include/kickos/config/cap_width.h.in"
               "${PROJECT_BINARY_DIR}/generated/include/kickos/config/cap_width.h" @ONLY)
if(TARGET kickos_core)
  # The in-tree include_directories at the top of this file does not reach an
  # add_subdirectory/FetchContent consumer's own targets; this does.
  target_include_directories(kickos_core INTERFACE
    "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
endif()

# Registered HERE and not beside the static gates above: it needs KICKOS_ARCH_INCLUDE_DIR
# (set by add_subdirectory(arch)) and the header generated just above.
if(KICKOS_BUILD_TESTS)
  if(NOT CMAKE_C_COMPILER)
    message(FATAL_ERROR "No CMAKE_C_COMPILER, so the C-facing headers cannot be compiled as C "
                        "and check_c_headers.sh would have nothing to run. project() declares "
                        "LANGUAGES C; a toolchain file that unsets the C compiler must be fixed.")
  endif()
  set(_kos_c_hdr_roots "${PROJECT_BINARY_DIR}/generated/include")
  # Several arch directories provide the same kickos/arch/context.h, so the gate takes the
  # one this board builds instead of deriving all of them.
  foreach(_d KICKOS_ARCH_INCLUDE_DIR KICKOS_CHIP_INCLUDE_DIR KICKOS_BOARD_INCLUDE_DIR)
    if(${_d})
      list(APPEND _kos_c_hdr_roots "${${_d}}")
    endif()
  endforeach()
  _add_test(NAME c_headers
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_c_headers.sh"
                    "${CMAKE_C_COMPILER}" ${_kos_c_hdr_roots}
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(c_headers PROPERTIES TIMEOUT 120 LABELS host)
endif()

# --- Install / export: the dependency-inversion package --------------------
include(CMakePackageConfigHelpers)

set(KICKOS_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/KickOS")

# CeCILL-C is file-level copyleft: ship the license with the package.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/${PROJECT_NAME}")

# The arch backend's include dir is the path the arch subdir reports (arch/sim/include vs
# arch/arm/armv7m/include), not an arch/${KICKOS_ARCH}/include guess.
install(DIRECTORY
          "${CMAKE_CURRENT_SOURCE_DIR}/include/"
          "${CMAKE_CURRENT_SOURCE_DIR}/arch/include/"
          "${KICKOS_ARCH_INCLUDE_DIR}/"
          "${CMAKE_CURRENT_SOURCE_DIR}/kernel/include/"
          "${CMAKE_CURRENT_SOURCE_DIR}/lib/include/"
          "${CMAKE_CURRENT_SOURCE_DIR}/user/include/"
          "${CMAKE_CURRENT_SOURCE_DIR}/system/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        PATTERN "*.h.in" EXCLUDE)

# An installed consumer that includes a kernel header must take the same width as the
# prebuilt libraries, or its sizeof(Thread) does not match theirs.
install(FILES "${PROJECT_BINARY_DIR}/generated/include/kickos/config/cap_width.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/kickos/config")

# The generated provisioning header is the one the prebuilt libs were compiled against.
install(FILES "${PROJECT_BINARY_DIR}/generated/include/kickos/board_config.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/kickos")

# The chip's constants, which config/board.h requires for KICKOS_MAX_IRQ (see the check at
# the top of this file).
if(KICKOS_CHIP_INCLUDE_DIR)
  install(DIRECTORY "${KICKOS_CHIP_INCLUDE_DIR}/"
          DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
endif()

if(KICKOS_BOARD_INCLUDE_DIR)
  install(DIRECTORY "${KICKOS_BOARD_INCLUDE_DIR}/"
          DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
endif()

# No NAMESPACE: kickos_add_application() links and asserts the bare target names
# (kickos_arch_<arch>, ...), identically in-tree and installed. Do not add one.
install(EXPORT KickOSTargets
        FILE KickOSTargets.cmake
        DESTINATION "${KICKOS_CMAKE_DIR}")

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/KickOSConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/KickOSConfig.cmake"
  INSTALL_DESTINATION "${KICKOS_CMAKE_DIR}")

# EXACT, not SameMajorVersion: at 0.x CMake treats the whole 0 series as one compatibility
# bucket, so a request for an older 0.x configures green against this one. The ABI moves at
# SUBMILESTONE granularity before the freeze, so no two versions are interchangeable and a
# consumer must pin the one it built against; relax at the freeze.
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/KickOSConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY ExactVersion)

# kickos.cmake include()s cap_table.cmake list-dir-relative, so that must land beside it.
install(FILES
          "${CMAKE_CURRENT_BINARY_DIR}/KickOSConfig.cmake"
          "${CMAKE_CURRENT_BINARY_DIR}/KickOSConfigVersion.cmake"
          "${CMAKE_CURRENT_SOURCE_DIR}/cmake/kickos.cmake"
          "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cap_table.cmake"
        DESTINATION "${KICKOS_CMAKE_DIR}")

# An out-of-tree consumer configures with
# -DCMAKE_TOOLCHAIN_FILE=<prefix>/.../toolchain-<family>*.cmake. This board's descriptor
# ships beside it as board.cmake, so the toolchain finds the baseline with no boards/ tree.
if(NOT KICKOS_ARCH STREQUAL "sim")
  if(KICKOS_ARCH_FAMILY STREQUAL "rx")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-rx-elf.cmake"
            DESTINATION "${KICKOS_CMAKE_DIR}")
  elseif(KICKOS_ARCH_FAMILY STREQUAL "xtensa")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-xtensa-esp32-elf.cmake"
            DESTINATION "${KICKOS_CMAKE_DIR}")
  elseif(KICKOS_ARCH_FAMILY STREQUAL "riscv")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-riscv-none-elf.cmake"
                  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-cxx-runtime-check.cmake"
            DESTINATION "${KICKOS_CMAKE_DIR}")
  else()
    # The compiler-capability gate is include()d list-dir-relative by the ARM +
    # RISC-V toolchain files, so it must land beside them in the package.
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-arm-none-eabi.cmake"
                  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-cxx-runtime-check.cmake"
            DESTINATION "${KICKOS_CMAKE_DIR}")
  endif()
  # The CPU baseline is BAKED IN: a package has no arch/ tree for the toolchain file to read
  # a cpu.cmake out of. Only the knobs the descriptor LEAVES to the chip are appended, so the
  # shipped file never states one twice.
  set(_kos_baked "${PROJECT_BINARY_DIR}/generated/board.cmake")
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/boards/${KICKOS_BOARD}/board.cmake" _kos_desc)
  set(_kos_bake "")
  if(DEFINED KICKOS_MCPU AND NOT _kos_desc MATCHES "(^|\n)set\\(KICKOS_MCPU[ \t]")
    string(JOIN " " _kos_mcpu_txt ${KICKOS_MCPU})
    string(APPEND _kos_bake "set(KICKOS_MCPU ${_kos_mcpu_txt})\n")
  endif()
  if(DEFINED KICKOS_MFLOAT_ABI AND NOT _kos_desc MATCHES "(^|\n)set\\(KICKOS_MFLOAT_ABI[ \t]")
    string(APPEND _kos_bake "set(KICKOS_MFLOAT_ABI ${KICKOS_MFLOAT_ABI})\n")
  endif()
  if(NOT _kos_bake STREQUAL "")
    string(APPEND _kos_desc
      "\n# Appended at install: the CPU baseline this package was built with, which in tree\n"
      "# comes from arch/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/cpu.cmake.\n"
      "${_kos_bake}")
  endif()
  file(WRITE "${_kos_baked}" "${_kos_desc}")
  install(FILES "${_kos_baked}" DESTINATION "${KICKOS_CMAKE_DIR}")
endif()
