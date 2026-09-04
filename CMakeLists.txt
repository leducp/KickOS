# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

cmake_minimum_required(VERSION 3.24)
project(KickOS VERSION 0.5.1 LANGUAGES C CXX ASM)
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

set(KICKOS_BOARD_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/${KICKOS_BOARD}/include")
if(EXISTS "${KICKOS_BOARD_INCLUDE_DIR}")
  include_directories("${KICKOS_BOARD_INCLUDE_DIR}")
else()
  set(KICKOS_BOARD_INCLUDE_DIR "")
endif()

# --- Configuration from Kconfig --------------------------------------------
# Each variant defconfig is a complete statement, not a delta on another.
set(KICKOS_CONFIG_VARIANT "base" CACHE STRING
    "Configuration variant under boards/<board>/configs/")
set(KICKOS_DEFCONFIG
    "${CMAKE_CURRENT_SOURCE_DIR}/boards/${KICKOS_BOARD}/configs/${KICKOS_CONFIG_VARIANT}/defconfig")
if(EXISTS "${KICKOS_DEFCONFIG}")

  # Every arm must translate in BOTH directions: sending only the positive one lets a
  # defconfig that enables something outrank a command line asking to turn it off.
  set(_kcfg_req "")
  # A name not in this list never becomes a request; CMake merely reports it unused.
  # KICKOS_MAX_HANDLES is computed by the sum in cmake/cap_table.cmake.
  foreach(_knob KICKOS_MAX_THREADS KICKOS_USER_STACK_SIZE KICKOS_IDLE_STACK_SIZE
                KICKOS_ROOT_STACK_SIZE KICKOS_MAX_SEMAPHORES KICKOS_MAX_MUTEXES
                KICKOS_MAX_ENDPOINTS KICKOS_CAP_TABLE_SUPPLY KICKOS_MAX_SPAWN_GRANTS
                KICKOS_MAX_IRQ_HANDLES KICKOS_MAX_FRAME_RUNS KICKOS_ASPACE_RANGES
                KICKOS_RTT_CH1_SIZE
                KICKOS_USER_HEAP_SIZE
                KICKOS_MAX_INSTANCES KICKOS_PANIC_DUMP KICKOS_DIAG_TERSE
                KICKOS_AMP_NODES KICKOS_AMP_NODE_ID KICKOS_ISOLATED_CORES)
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

# KICKOS_LIBC_REENT and KICKOS_LINKER_WEAK_UNDEF are published to C++ as 0/1 so every `#if`
# site is -Wundef-clean; KICKOS_HAVE_LIBC gates which apps enter the build graph.
# KICKOS_LINKER_WEAK_UNDEF: `ld -m i386pep` builds no global offset table and x86_64 reaches
# a weak undefined symbol GOT-indirect, so there such a reference loads the symbol's own bytes.
set(KICKOS_HAVE_LIBC 1)
set(KICKOS_LIBC_REENT 1)
set(KICKOS_LINKER_WEAK_UNDEF 1)
if(KICKOS_ARCH STREQUAL "sim" OR KICKOS_ARCH STREQUAL "x86_64")
  set(KICKOS_LIBC_REENT 0)
endif()
if(KICKOS_ARCH STREQUAL "x86_64")
  set(KICKOS_HAVE_LIBC 0)
  set(KICKOS_LINKER_WEAK_UNDEF 0)
endif()
add_compile_definitions(KICKOS_LIBC_REENT=${KICKOS_LIBC_REENT}
                        KICKOS_LINKER_WEAK_UNDEF=${KICKOS_LINKER_WEAK_UNDEF})

# --- Config knobs ----------------------------------------------------------
# Every knob below that becomes a compile definition is defined 0 or 1, never left
# undefined, so each `#if` site is -Wundef-clean.
option(CONFIG_SCHED_PERIODIC_TICK "Force a classic periodic scheduler tick" OFF)
option(KICKOS_BUILD_APPS          "Build in-tree demo applications"          ON)
option(KICKOS_BUILD_TESTS         "Register CTest cases"                     ON)
option(KICKOS_BUILD_INTEGRATION_TESTS
       "Register the CTest cases that RUN a KickOS image (sim, qemu, out-of-tree export)" ON)

set(_kickos_gtest_found OFF)
if(KICKOS_ARCH STREQUAL "sim")
  # GTest is IMPORTED for Release only and its interface properties are gated on
  # $<CONFIG:Release>; without this mapping a MinSizeRel sim build gets no include path.
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
option(KICKOS_ENABLE_SELFTEST     "Expose test-only self-test syscalls"      OFF)
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

# Changes an inline in a shipped header (<kickos/list.h>), so it must reach consumers (ODR).
option(KICKOS_DEBUG "Build the internal-consistency guards (list membership, ...)" OFF)
if(KICKOS_DEBUG)
  add_compile_definitions(KICKOS_DEBUG=1)
else()
  add_compile_definitions(KICKOS_DEBUG=0)
endif()

# A per-core event ring for diagnosing a park that never woke (kickos/smptrace.h). OFF by
# default and compiling to nothing: it is a diagnostic knob, not a posture, and no preset
# selects it. Configure with -DKICKOS_SMP_TRACE=ON to turn it on for one build.
option(KICKOS_SMP_TRACE "Record a per-core park/wake event ring for SMP diagnosis" OFF)
if(KICKOS_SMP_TRACE)
  add_compile_definitions(KICKOS_SMP_TRACE=1)
else()
  add_compile_definitions(KICKOS_SMP_TRACE=0)
endif()

# --- Console backend selection ---------------------------------------------
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

# Changes kernel() in a shipped header, so it must reach consumers (ODR). The instance COUNT
# rides in the generated board_config.h with the other provisioning knobs.
if(KICKOS_MULTI_INSTANCE)
  add_compile_definitions(KICKOS_MULTI_INSTANCE=1)
else()
  add_compile_definitions(KICKOS_MULTI_INSTANCE=0)
endif()

include("${CMAKE_SOURCE_DIR}/cmake/boot_arena.cmake")
# Always defined, so every `#if KICKOS_TLS_FROM_SP` site is -Wundef-clean.
if(NOT DEFINED KICKOS_TLS_FROM_SP)
  set(KICKOS_TLS_FROM_SP 0)
endif()
add_compile_definitions(KICKOS_TLS_FROM_SP=${KICKOS_TLS_FROM_SP})
if(KICKOS_TLS)
  kickos_pow2_ceil("${KICKOS_USER_STACK_SIZE}" _tls_stride)
  kickos_pow2_ceil("${KICKOS_ROOT_STACK_SIZE}" _tls_root_stride)
  if(KICKOS_TLS_FROM_SP AND NOT _tls_stride EQUAL _tls_root_stride)
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
  foreach(_k KICKOS_USER_STACK_SIZE KICKOS_ROOT_STACK_SIZE)
    if(KICKOS_TLS_FROM_SP AND NOT ${${_k}} EQUAL _tls_stride)
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
  if(KICKOS_TLS_FROM_SP)
    message(STATUS "KickOS: the thread pointer is SP masked to the stride")
  endif()
  message(STATUS "KickOS: TLS stride ${_tls_stride} (1 << ${_tls_log2})")
else()
  # Always defined, so every `#if KICKOS_TLS` site is -Wundef-clean.
  add_compile_definitions(KICKOS_TLS=0)
endif()

# Positive identification: a new arch lands in the 0 branch of every `#if KICKOS_ARCH_SIM`.
if(KICKOS_ARCH STREQUAL "sim")
  set(_arch_sim 1)
else()
  set(_arch_sim 0)
endif()
add_compile_definitions(KICKOS_ARCH_SIM=${_arch_sim})

# --- Capability opt-in files (arch/<family>/chip/<chip>/*.cmake) --------------
# A chip declares a capability by shipping a named file beside its sources: caps.cmake for
# the trace clock, mpu.cmake for the enforcing region posture, aspace.cmake for a
# translating backend, terminate.cmake for an exiting fault dead-end. An arch declares one
# the same way, ipc_fastpath.cmake beside its switch.S. Each is include()d below, by the
# block that reads it.
#
# The include lands in THIS scope, so a chip file states its capability with a plain set()
# and no PARENT_SCOPE, and a variable it sets is inherited by arch/CMakeLists.txt through
# add_subdirectory(arch) as well.
#
# A capability is an explicit opt-in, never inferred from an arch, a chip name or a linker
# script. Where Kconfig declares the same fact, the floor beside the include REFUSES a chip
# that selects it and ships no file, so the two declarations cannot silently disagree.
#
# A chip file itself carries only what is true of THAT chip: which capability it opts into,
# and what the chip ships that makes the claim true.

# --- Telemetry backend selection -----------------------------------------------
# KICKOS_TRACE_ARCH MUST match the ArchId enum in include/kickos/trace/record.h, which the
# decoder tools/kicktrace.py follows; a static_assert there pins ARCH_RISCV == 5.
# The chip's caps.cmake is included first so a chip value wins over the arch ladders below,
# which guard their set with NOT DEFINED.
if(KICKOS_CHIP)
  set(_caps_optin "${CMAKE_CURRENT_SOURCE_DIR}/arch/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/caps.cmake")
  if(EXISTS "${_caps_optin}")
    include("${_caps_optin}")
  endif()
endif()
# KICKOS_FAULT_ISOLATION is never overridable: a backend that defines
# arch_fault_is_user_thread / arch_fault_redirect_to_exit and is not listed here links BOTH
# its definition and the fallback into one archive, resolved by member order.
if(KICKOS_ARCH STREQUAL "armv6m")
  if(KICKOS_BOARD STREQUAL "microbit")
    set(KICKOS_HAVE_PRIV_RING 0) # nRF51822, Cortex-M0
  elseif(KICKOS_BOARD STREQUAL "picopi")
    set(KICKOS_HAVE_PRIV_RING 1) # RP2040, Cortex-M0+
  else()
    message(FATAL_ERROR
      "armv6m board '${KICKOS_BOARD}' is not classified. ARMv6-M's Unprivileged/Privileged "
      "Extension is optional and separate from the MPU extension: a Cortex-M0+ implements it, a "
      "Cortex-M0 does not, and on an M0 the npriv fields of the fabricated frame are written and "
      "then discarded by the hardware. One armv6m backend spans both cores, so the ring is a "
      "per-board fact: add a branch above setting KICKOS_HAVE_PRIV_RING to 1 for a Cortex-M0+ "
      "core or 0 for a Cortex-M0 one.")
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
      OR KICKOS_ARCH STREQUAL "armv8a"
      OR KICKOS_ARCH STREQUAL "rv64imac"
      OR KICKOS_ARCH STREQUAL "x86_64"
      OR (KICKOS_ARCH STREQUAL "sim" AND CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "x86_64")))
  set(KICKOS_FAULT_ISOLATION 1)
  set(KICKOS_FAULT_OUTCOME thread-kill)
endif()
add_compile_definitions(KICKOS_FAULT_ISOLATION=${KICKOS_FAULT_ISOLATION})
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
# also picks the PMSAv8 backend source; the sim enforces via host mprotect.
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

# A chip declares a translating backend by shipping arch/<family>/chip/<chip>/aspace.cmake.
# No chip ships both that and mpu.cmake.
set(KICKOS_CHIP_TRANSLATES OFF)
if(KICKOS_CHIP)
  set(_aspace_optin
      "${CMAKE_CURRENT_SOURCE_DIR}/arch/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/aspace.cmake")
  if(EXISTS "${_aspace_optin}")
    include("${_aspace_optin}")
  endif()
endif()
if(KICKOS_HAVE_ASPACE AND NOT KICKOS_CHIP_TRANSLATES)
  message(FATAL_ERROR
    "The configuration resolved a translating backend on board '${KICKOS_BOARD}' "
    "(arch=${KICKOS_ARCH} chip=${KICKOS_CHIP}), which ships no aspace.cmake, so the two "
    "declarations of one fact disagree: arch/Kconfig selects HAS_ASPACE for this chip and "
    "arch/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/aspace.cmake does not exist. The kernel "
    "would build the frame allocator over a pool the linker script never carved. Either "
    "drop the select, or carve __kickos_frame_pool_start/_end and drop an aspace.cmake "
    "(KICKOS_CHIP_TRANSLATES ON) beside it.")
endif()
if(KICKOS_CHIP_TRANSLATES AND KICKOS_HAVE_MPU)
  message(FATAL_ERROR
    "Board '${KICKOS_BOARD}' resolved BOTH the enforcing region posture and a translating "
    "backend. A chip opts into exactly one memory family: the flat non-translating region set "
    "(arch_mpu_*, declared by mpu.cmake) or translation (arch_aspace_*, declared by "
    "aspace.cmake). Under translation the descriptors that map already carry AP, UXN and PXN, "
    "so no region set is left to gate and no backend implements both call families. Drop one "
    "of the two opt-ins in arch/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/.")
endif()
add_compile_definitions(KICKOS_HAVE_ASPACE=${KICKOS_HAVE_ASPACE})

# The third opt-in of the set, and the one with no Kconfig half to disagree with: a chip
# ships terminate.cmake when its arch_shutdown reaches a debug or emulator channel that
# turns the status into a process exit. arch/CMakeLists.txt then links the exiting
# kfault_terminate in place of the LED fallback.
set(KICKOS_CHIP_EXITS_ON_FAULT OFF)
if(KICKOS_CHIP)
  set(_terminate_optin
      "${CMAKE_CURRENT_SOURCE_DIR}/arch/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/terminate.cmake")
  if(EXISTS "${_terminate_optin}")
    include("${_terminate_optin}")
  endif()
endif()

# Read only by kickos_split_image_tu (cmake/kickos.cmake); empty leaves that function inert.
# Set here because kernel/CMakeLists.txt calls the same function and cannot see arch/'s scope.
set(KICKOS_SPLIT_IMAGE_CODE_MODEL "")
if(KICKOS_ARCH STREQUAL "armv8a")
  set(KICKOS_SPLIT_IMAGE_CODE_MODEL -mcmodel=large)
endif()

# Whether memory protection is LIVE, which admission policy keys on. Derived, so no board can
# state it and disagree with its backend.
set(KICKOS_MEMORY_ENFORCED 0)
if(KICKOS_HAVE_MPU OR KICKOS_HAVE_ASPACE)
  set(KICKOS_MEMORY_ENFORCED 1)
endif()
add_compile_definitions(KICKOS_MEMORY_ENFORCED=${KICKOS_MEMORY_ENFORCED})

# Which MPU design the v6-M/v7-M arch is built for. A chip's mpu.cmake names its backend in
# KICKOS_ARM_MPU_BACKEND; PMSAv7 is the fallback. The selector is total: a build with no MPU
# resolves to NONE, so no comparison against it needs a KICKOS_HAVE_MPU conjunction.
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
# trap. It gates three things that must not drift apart: the stub's register arm,
# arch_syscall_reg with its result-store seam, and the kernel's shared leaf.
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

# --- Shared-kernel hardware predicate ---------------------------------------
include(smp_predicate)
kickos_smp_predicate(
  SOURCE_DIR   "${CMAKE_CURRENT_SOURCE_DIR}"
  ARCH         "${KICKOS_ARCH}"
  ARCH_FAMILY  "${KICKOS_ARCH_FAMILY}"
  CHIP         "${KICKOS_CHIP}"
  BOARD        "${KICKOS_BOARD}"
  NUM_CORES    "${KICKOS_NUM_CORES}"
  MODEL_SHARED "${KICKOS_MULTICORE_MODEL_SHARED}")

# The generated header guards its own define with #ifndef, so this wins where both are seen.
add_compile_definitions(KICKOS_NUM_CORES=${KICKOS_NUM_CORES})
# Beside the count, so an APP TU can ask how many cores one kernel schedules on: a claim about
# two threads' relative progress is a claim about that number.
add_compile_definitions(KICKOS_KERNEL_CORES=${KICKOS_KERNEL_CORES})

# Beside them again, for an app TU that has to ask which MODEL it runs under.
add_compile_definitions(KICKOS_MULTICORE_MODEL_SHARED=${KICKOS_MULTICORE_MODEL_SHARED})

# --- Isolated cores ----------------------------------------------------------
# The knob depends on the shared model, so it is absent from the generated header on every
# other posture and the C side carries the same zero default.
if(NOT DEFINED KICKOS_ISOLATED_CORES OR KICKOS_ISOLATED_CORES STREQUAL "")
  set(KICKOS_ISOLATED_CORES 0)
endif()
include(isolated_cores)
kickos_isolated_cores_check(
  ISOLATED      "${KICKOS_ISOLATED_CORES}"
  KERNEL_CORES  "${KICKOS_KERNEL_CORES}"
  ORIGIN        "${KICKOS_DEFCONFIG}")
# An APP TU asks this to know which cores it may pin onto.
add_compile_definitions(KICKOS_ISOLATED_CORES=${KICKOS_ISOLATED_CORES})

# The AMP posture and the partition's width, for an app TU that has to ask whether it runs on
# a node at all and how many peers it may sweep. Both live in the generated board config,
# which an app does not include.
add_compile_definitions(KICKOS_AMP_NODE=${KICKOS_AMP_NODE})
add_compile_definitions(KICKOS_AMP_NODES=${KICKOS_AMP_NODES})

# The doorbell a publication rings is arch_ipi_send(1 << node), a HARDWARE CORE MASK spent on
# a node index. Refused here rather than rung at the wrong core.
if(KICKOS_AMP_NODE AND KICKOS_AMP_OWN_IMAGE)
  message(FATAL_ERROR
    "KickOS: the own-image AMP posture is refused. amp::send in kernel/amp/ampwindow.cc "
    "rings its peer with arch_ipi_send(1u << to), where `to` is a NODE index and the "
    "argument is a hardware CORE mask. The two coincide only under the shared-image "
    "posture, where the node's identity IS the core register. Under one image per node they "
    "are unrelated, the image driving one core while naming peers it never launches, so the "
    "doorbell would ring whichever local core carries the target's index, silently and with "
    "no refusal. What is missing is a partition-owned node-to-core map: the partition layout "
    "stating, per node, which core carries it, and amp::send taking its doorbell target from "
    "that map instead of from the node index. Until a part declares one, build this board "
    "with CONFIG_KICKOS_AMP_POSTURE_SHARED_IMAGE.")
endif()

# The microbench accumulates into one set of phase cells for the whole image, and its own
# contract is that every write happens under the interrupt lock or on one thread. A switch span
# breaks that above one kernel core: it closes when the outgoing thread is next RESUMED, which
# the swap does after releasing the kernel lock and possibly on a different core, so two cores
# closing the same phase race unlocked. Refused rather than measured wrong. What it wants is one
# accumulator per core plus an aggregation at report time.
if(KICKOS_BENCH AND KICKOS_KERNEL_CORES GREATER 1)
  message(FATAL_ERROR
    "KickOS: KICKOS_BENCH=ON with KICKOS_KERNEL_CORES=${KICKOS_KERNEL_CORES}. The phase "
    "accumulators are one set for the image and a switch span closes on the resuming core "
    "outside the kernel lock, so two cores race on the same cells. Give kernel/bench/bench.cc "
    "one accumulator per core and aggregate when the report is emitted.")
endif()

# --- Per-arch caller-provided-stack floor (KICKOS_MIN_STACK_SIZE) -----------
# The floor must cover the thread-exit teardown, which runs on the CALLING thread's stack.
# An arch property; the measured figures are in the Kconfig declaration.
if(NOT DEFINED KICKOS_MIN_STACK_SIZE)
  message(FATAL_ERROR
    "KickOS: no KICKOS_MIN_STACK_SIZE. The generated kickos_config.cmake carries it, "
    "derived per arch in Kconfig, so an empty value here means the fragment was not "
    "read or the arch resolved to none.")
endif()
add_compile_definitions(KICKOS_MIN_STACK_SIZE=${KICKOS_MIN_STACK_SIZE})
message(STATUS "KickOS: caller-stack floor KICKOS_MIN_STACK_SIZE=${KICKOS_MIN_STACK_SIZE} (arch=${KICKOS_ARCH})")

# KICKOS_SERVICE_LIST names the target that DEFINES kickos_board_services;
# KICKOS_SERVICE_LIST_LIBS is that target plus every driver and class leaf its start()s
# reference. They back-reference kickos_user, so they must join the rescan link group below.
message(STATUS "KickOS: service list = ${KICKOS_SERVICE_LIST}")

# The USB device clock tree (PLL_USB, clk_usb, the USB block reset) is brought up in chip
# init. Never cached: a cached value keeps the clock tree on after the service list changes.
if(NOT DEFINED KICKOS_USB_CONSOLE)
  set(KICKOS_USB_CONSOLE OFF)
  # Unanchored: any list with a "_usbcdc" stem needs the same clock tree.
  if(KICKOS_SERVICE_LIST MATCHES "_usbcdc")
    set(KICKOS_USB_CONSOLE ON)
  endif()
endif()
if(KICKOS_USB_CONSOLE)
  add_compile_definitions(KICKOS_USB_CONSOLE=1)
  message(STATUS "KickOS: USB device clock tree = on (chip init brings up PLL_USB)")
endif()

# KICKOS_BOARD_PINMAP names the target that DEFINES kickos_board_pinmap; it back-references
# kickos_user, so it must join the rescan link group below.
if(NOT DEFINED KICKOS_BOARD_PINMAP_LIBS)
  set(KICKOS_BOARD_PINMAP_LIBS "${KICKOS_BOARD_PINMAP}")
endif()
message(STATUS "KickOS: board pin-map = ${KICKOS_BOARD_PINMAP} (libs: ${KICKOS_BOARD_PINMAP_LIBS})")

# RX MPU wedge-localizer trace (arch/rx/rxv3/arch_rxv3.cc), command line only.
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

# RTT publish barrier override: a weakly-ordered core injects its real fence here; unset
# keeps the compiler-only barrier in rtt.h. Pass the FULL call form, e.g.
# -DKICKOS_RTT_PUBLISH_BARRIER_DEF="__atomic_thread_fence(__ATOMIC_RELEASE)".
if(DEFINED KICKOS_RTT_PUBLISH_BARRIER_DEF)
  add_compile_definitions("KICKOS_RTT_PUBLISH_BARRIER()=${KICKOS_RTT_PUBLISH_BARRIER_DEF}")
endif()

# Passed as a bare token (-DKICKOS_APP_TZ=+0200): escaped quotes double up on a reconfigure of
# an existing build dir, so app.h stringizes the token in C.
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

  # Read from the one file tests/lib/gate.sh also reads (same ERE dialect). It must not
  # contain a `;`: FAIL_REGULAR_EXPRESSION is a CMake list of regexes.
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
  # and so never requires itself.
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

  # Which side each test program falls on is declared in tests/static/test_classes.txt.
  # Fixtures are resolved AFTER label filtering, so `kickos_build` carries no label and
  # appears in both partitions.

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

  _add_test(NAME whitespace
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_whitespace.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(whitespace PROPERTIES TIMEOUT 120 LABELS host)

  _add_test(NAME shell_special_names
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_shell_special_names.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(shell_special_names PROPERTIES TIMEOUT 120 LABELS host)

  # A milestone that deliberately changes the entry seam moves the baseline ref in
  # tests/static/entry_seam_records.txt, and this entry fails until it does.
  _add_test(NAME entry_sigdiff
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_entry_sigdiff.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(entry_sigdiff PROPERTIES TIMEOUT 120 LABELS host)

  # M6.5's expected verdict is PASS: frame and page-table objects enter the capability layer
  # and the ABI gains no addressing concept. A member added on purpose moves
  # tests/static/cap_seam_records.txt in the commit that adds it, never ahead of it.
  _add_test(NAME cap_sigdiff
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_cap_sigdiff.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(cap_sigdiff PROPERTIES TIMEOUT 120 LABELS host)

  # The four SMP seam members left entry_sigdiff at M7.0 and are asserted by this entry alone.
  _add_test(NAME smp_sigdiff
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_smp_sigdiff.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(smp_sigdiff PROPERTIES TIMEOUT 120 LABELS host)

  # Takes the generated board config: the gate must read the knob spelling C reads.
  _add_test(NAME cpu_id_fold
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_cpu_id_fold.sh"
                    "${PROJECT_BINARY_DIR}/generated/include/kickos/board_config.h"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  # 77 is the gate's SKIP: above one core the seam is a real function and the fold is not a
  # property to check.
  set_tests_properties(cpu_id_fold PROPERTIES TIMEOUT 120 LABELS host SKIP_RETURN_CODE 77)

  _add_test(NAME forever_loop
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_forever_loop.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(forever_loop PROPERTIES TIMEOUT 120 LABELS host)

  _add_test(NAME dash_punct
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_dash_punct.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(dash_punct PROPERTIES TIMEOUT 120 LABELS host)

  _add_test(NAME panic_banners
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_panic_banners.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(panic_banners PROPERTIES TIMEOUT 120 LABELS host)

  _add_test(NAME death_stack_seating
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_death_stack_seating.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(death_stack_seating PROPERTIES TIMEOUT 120 LABELS host)

  _add_test(NAME park_death_point
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_park_death_point.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(park_death_point PROPERTIES TIMEOUT 120 LABELS host)

  # --- The trap red-zone gate --------------------------------------------------
  # The preset name is derived here: the base variant is the board name, every other variant
  # is <board>-<variant>. trap_redzone_roots.txt declares which pairs are measurable, and an
  # undeclared one registers nothing.
  set(_fs_redzone_roots "${PROJECT_SOURCE_DIR}/tests/static/trap_redzone_roots.txt")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_fs_redzone_roots}")
  if(KICKOS_CONFIG_VARIANT STREQUAL "base")
    set(_fs_redzone_candidate "${KICKOS_BOARD}")
  else()
    set(_fs_redzone_candidate "${KICKOS_BOARD}-${KICKOS_CONFIG_VARIANT}")
  endif()
  set(_fs_redzone_preset "")
  # The trailing anchor is required: `preset armv7m qemu` must not match qemu-flat.
  file(STRINGS "${_fs_redzone_roots}" _fs_redzone_decls
       REGEX "^preset[ \t]+${KICKOS_ARCH}[ \t]+${_fs_redzone_candidate}[ \t]*$")
  if(NOT _fs_redzone_decls STREQUAL "")
    set(_fs_redzone_preset "${_fs_redzone_candidate}")
  endif()
  if(NOT _fs_redzone_preset STREQUAL "")
    _add_test(NAME trap_redzone
              COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_trap_redzone.sh"
                      "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}"
                      "${_fs_redzone_preset}" "${KICKOS_ARCH}")
    set_tests_properties(trap_redzone PROPERTIES TIMEOUT 1800 LABELS host)
  endif()

  # Outside the _fs_redzone_preset guard: it asks whether the declaration set covers the
  # fleet, which a board with no declared preset cannot ask by registering trap_redzone.
  _add_test(NAME trap_redzone_decls
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_trap_redzone_decls.sh"
                    "${CMAKE_COMMAND}" "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(trap_redzone_decls PROPERTIES TIMEOUT 120 LABELS host)

  # --- The console-route reachability gate -------------------------------------
  # Registered on the TRANSLATING presets, where cap_console_deliver's error paths survive the
  # optimiser. console_reach_roots.txt declares which; one missing from it registers nothing.
  set(_fs_reach_decl "${PROJECT_SOURCE_DIR}/tests/static/console_reach_roots.txt")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_fs_reach_decl}")
  if(KICKOS_CONFIG_VARIANT STREQUAL "base")
    set(_fs_reach_candidate "${KICKOS_BOARD}")
  else()
    set(_fs_reach_candidate "${KICKOS_BOARD}-${KICKOS_CONFIG_VARIANT}")
  endif()
  # The trailing anchor is required: `preset rv64imac qemu-riscv64` must not match
  # qemu-riscv64-sv48.
  file(STRINGS "${_fs_reach_decl}" _fs_reach_decls
       REGEX "^preset[ \t]+${KICKOS_ARCH}[ \t]+${_fs_reach_candidate}[ \t]*$")
  if(NOT _fs_reach_decls STREQUAL "")
    _add_test(NAME console_reach
              COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_console_reach.sh"
                      "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}"
                      "${_fs_reach_candidate}" "${KICKOS_ARCH}")
    set_tests_properties(console_reach PROPERTIES TIMEOUT 1800 LABELS host)
  endif()

  _add_test(NAME atomic_rmw
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_atomic_rmw.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(atomic_rmw PROPERTIES TIMEOUT 120 LABELS host)

  # The kernel-core seam against the machine's core identity. Corpus-wide source scan, so it
  # is registered from every board and reads no build output.
  _add_test(NAME kernel_core_index
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_kernel_core_index.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(kernel_core_index PROPERTIES TIMEOUT 120 LABELS host)

  _add_test(NAME service_lists
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_service_lists.sh"
                    "${CMAKE_COMMAND}" "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(service_lists PROPERTIES TIMEOUT 120 LABELS host)

  kickos_find_kconfig(_kconfig_py _kconfig_found)
  _add_test(NAME kconfig_gen
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_kconfig_gen.sh"
                    "${_kconfig_py}" "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(kconfig_gen PROPERTIES TIMEOUT 120 LABELS host)

  # The shared-kernel hardware predicate above, driven on a board whose arch declares no
  # smp.cmake. Registered from every board because the predicate lives in this file: it
  # configures two child trees of its own and builds neither, so it is a host check. The
  # interpreter is passed through so the child resolves Kconfig with this one.
  _add_test(NAME smp_predicate
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_smp_predicate.sh"
                    "${_kconfig_py}" "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_COMMAND}")
  set_tests_properties(smp_predicate PROPERTIES TIMEOUT 300 LABELS host)

  # The isolated-core mask's two configure refusals, driven over synthetic values against the
  # same module this file calls above. Registered from every board: the module is this tree's,
  # and the gate configures nothing and builds nothing.
  _add_test(NAME isolated_cores
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_isolated_cores.sh"
                    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_COMMAND}")
  set_tests_properties(isolated_cores PROPERTIES TIMEOUT 120 LABELS host)

  # KICKOS_SMP_TRACE is OFF in every preset, so nothing else compiles its enabled arm and it
  # would rot unnoticed. This compiles that arm alone, taking the flags from this build's own
  # compile_commands.json so they cannot drift, and runs no image. Registered where the trace
  # hooks are compiled at all, which is every board.
  _add_test(NAME smp_trace_builds
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_smp_trace_builds.sh"
                    "${CMAKE_BINARY_DIR}/compile_commands.json")
  set_tests_properties(smp_trace_builds PROPERTIES TIMEOUT 180 LABELS host)

  # Reads the forwarding lists above out of this very file, so it is registered from every
  # board and never from a generated copy.
  _add_test(NAME kconfig_forwarding
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_kconfig_forwarding.sh"
                    "${_kconfig_py}" "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(kconfig_forwarding PROPERTIES TIMEOUT 120 LABELS host)

  # This one DOES require the build fixture: it classifies each test by the program ctest
  # resolved for it, and ctest emits no command for a program that is not on disk yet.
  set(_kickos_declined no)
  if(NOT KICKOS_BUILD_INTEGRATION_TESTS)
    set(_kickos_declined yes)
  endif()
  # <complete> asserts this tree registers every build-rooted program test_classes.txt names,
  # bar the ones <excused> accounts for; only the sim tree builds the whole set. <excused>
  # names the layers this tree lacks, comma separated because a CMake list separator would
  # split the argument.
  set(_kickos_complete no)
  set(_kickos_excused_layers "")
  if(KICKOS_ARCH STREQUAL "sim")
    set(_kickos_complete yes)
    if(NOT KICKOS_BUILD_UNIT_TESTS)
      list(APPEND _kickos_excused_layers "unit")
    endif()
  endif()
  list(JOIN _kickos_excused_layers "," _kickos_excused)
  _add_test(NAME test_labels
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_test_labels.sh"
                    "${CMAKE_CTEST_COMMAND}" "${CMAKE_COMMAND}"
                    "${PROJECT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}"
                    "${_kickos_declined}" "${_kickos_complete}" "${_kickos_excused}")
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

if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/slotpool/CMakeLists.txt")
  add_subdirectory(tests/unit/slotpool)
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/kstring/CMakeLists.txt")
  add_subdirectory(tests/unit/kstring)
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/frame/CMakeLists.txt")
  add_subdirectory(tests/unit/frame)
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/vrange/CMakeLists.txt")
  add_subdirectory(tests/unit/vrange)
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/ktime/CMakeLists.txt")
  add_subdirectory(tests/unit/ktime)
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/tlscarve/CMakeLists.txt")
  add_subdirectory(tests/unit/tlscarve)
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/captable/CMakeLists.txt")
  add_subdirectory(tests/unit/captable)
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/uartclass/CMakeLists.txt")
  add_subdirectory(tests/unit/uartclass)
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/capreply/CMakeLists.txt")
  add_subdirectory(tests/unit/capreply)
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/grantnocache/CMakeLists.txt")
  add_subdirectory(tests/unit/grantnocache)
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/drvbringup/CMakeLists.txt")
  add_subdirectory(tests/unit/drvbringup)
endif()

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

# Outside the K-seam block above: it compiles kernel/init/console_tx.cc with its own arch
# seam, and the K-seam's would be a second definition of arch_irq_save.
if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/consoletx/CMakeLists.txt")
  add_subdirectory(tests/unit/consoletx)
endif()

# Outside it for the same reason, and additionally because it compiles kernel/irq/irq.cc at a
# core count the preset does not carry.
if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/irqquiesce/CMakeLists.txt")
  add_subdirectory(tests/unit/irqquiesce)
endif()

# The K-seam sources over again at two kernel cores, for the same reason: the preset drives
# one, where the lock the swap carries compiles to nothing.
if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/exitquiesce/CMakeLists.txt")
  add_subdirectory(tests/unit/exitquiesce)
  # Links the object library the directory above defines, so it cannot precede it.
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/migrateask/CMakeLists.txt")
    add_subdirectory(tests/unit/migrateask)
  endif()
endif()

# Header-only, and outside the K-seam block because it needs an ISOLATION MASK no preset
# carries: a mask over SEVERAL isolated cores, the pool an explicit affinity reserves, needs two
# and qemu-arm64-smpiso isolates one.
if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/placeadmit/CMakeLists.txt")
  add_subdirectory(tests/unit/placeadmit)
endif()

# Outside the K-seam block for the same reason as irqquiesce: it compiles kernel/sync/klock.cc
# with its own arch seam, at a core count the preset does not carry.
if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/reschedowed/CMakeLists.txt")
  add_subdirectory(tests/unit/reschedowed)
endif()

# Outside it again: it compiles kernel/amp/ampwindow.cc with its own arch seam, at a core
# count and under a multicore model the preset carries neither of.
if(KICKOS_BUILD_TESTS AND KICKOS_BUILD_UNIT_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/ampwindow/CMakeLists.txt")
  add_subdirectory(tests/unit/ampwindow)
endif()

# Set before kickos_core below, whose LINK_GROUP pulls the provider archive.
set(KICKOS_INIT_PROVIDER "kickos_default_init" CACHE STRING
  "CMake target providing kickos_init_entry (the userspace init service). Default = the built-in kickos_system service; a power user names their own target.")

if(TARGET kickos_kernel)
  # Consumers link a leaf (kickos / kickos_cxx), never kickos_core directly.
  add_library(kickos_core INTERFACE)
  # Group order: the kernel references kickos_init_entry across the rescan boundary, so
  # KICKOS_INIT_PROVIDER sits right after kickos_kernel, and KICKOS_SERVICE_LIST_LIBS follows
  # it. The whole closure must be in-group to rescan-resolve, and it is derived here because
  # the walk reads link edges that exist only after add_subdirectory(system) above.
  if(NOT DEFINED KICKOS_SERVICE_LIST_LIBS)
    kickos_service_libs_closure("${KICKOS_SERVICE_LIST}" KICKOS_SERVICE_LIST_LIBS)
    message(STATUS "KickOS: service-list closure = ${KICKOS_SERVICE_LIST_LIBS}")
  endif()
  # CMake forbids one target's closure from carrying a library in two groups, so the group is
  # built per posture leaf below.
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
  # ODR: an out-of-tree TU including a kernel-internal header must see the same posture as the
  # prebuilt libs, so every knob that gates a field or an inline in a shipped header is
  # repeated here.
  set(_kos_iface_sim 0)
  if(KICKOS_ARCH STREQUAL "sim")
    set(_kos_iface_sim 1)
  endif()
  target_compile_definitions(kickos_core INTERFACE
    KICKOS_TELEMETRY=${_telem} KICKOS_TELEMETRY_RTT=${_telem}
    KICKOS_TRACE_ARCH=${KICKOS_TRACE_ARCH}
    KICKOS_HAVE_MPU=${KICKOS_HAVE_MPU}
    KICKOS_HAVE_ASPACE=${KICKOS_HAVE_ASPACE}
    KICKOS_MEMORY_ENFORCED=${KICKOS_MEMORY_ENFORCED}
    KICKOS_ARM_MPU_NONE=0
    KICKOS_ARM_MPU_PMSAV7=1
    KICKOS_ARM_MPU_PMSAV8=2
    KICKOS_ARM_MPU_SYSMPU=3
    KICKOS_ARM_MPU=${_kickos_arm_mpu}
    KICKOS_ARCH_SIM=${_kos_iface_sim}
    KICKOS_LIBC_REENT=${KICKOS_LIBC_REENT}
    KICKOS_LINKER_WEAK_UNDEF=${KICKOS_LINKER_WEAK_UNDEF}
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
      # Every input section must be named by a rule in the chip script; an unnamed ALLOC
      # orphan lands at an address nobody chose. arch/common/sections.ld.h has the vocabulary.
      -Wl,--orphan-handling=error
      # ARM's vector table (g_isr_vector) is a data object nothing references. RX pulls its
      # vector tables via ENTRY(_start) + KEEP() and defines no such symbol to force.
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
      # A second rescan group, one layer out: the driver-appended libstdc++'s operator new
      # must reach down to `_sbrk`. Listed after kickos_core so the core group resolves first.
    target_link_libraries(kickos_cxx INTERFACE
      "$<LINK_GROUP:RESCAN,stdc++,supc++,c,kickos_user>")

    # A strong __gnu_cxx::__verbose_terminate_handler keeps the linker from extracting
    # libstdc++'s vterminate.o. It must be a real object on the link line to win over the
    # driver-appended libstdc++, and this TU needs -fexceptions -frtti to compile at all.
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

# Before the apps: it links the OBJECTS of the arch and chip targets add_subdirectory(arch)
# created, and defines the kickos_x86_64_link_image that kickos_add_application calls.
if(KICKOS_ARCH STREQUAL "x86_64")
  include(cmake/x86_64_boot.cmake)
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

# The arch backend's include dir is the path the arch subdir reports (arch/sim/include,
# arch/arm/armv7m/include), never an arch/${KICKOS_ARCH}/include guess.
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

# ExactVersion: at 0.x CMake treats the whole 0 series as one compatibility bucket, so a
# request for an older 0.x would configure green against this one. Relax at the ABI freeze.
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
# The CPU baseline is baked in: a package has no arch/ tree for the toolchain file to read a
# cpu.cmake out of. Only the knobs the descriptor leaves to the chip are appended.
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
