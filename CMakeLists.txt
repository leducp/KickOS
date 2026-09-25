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

# Add generated includes before subdirectories inherit the include paths.
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

  # Offer cached KICKOS_* values to the generator, which validates their types.
  # Use assert: for command-line/preset names so unknown requests are rejected.
  # Read cache properties without marking unused variables as consumed.
  set(_kcfg_req "")
  get_cmake_property(_kcfg_vars VARIABLES)
  foreach(_kcfg_v IN LISTS _kcfg_vars)
    if(NOT _kcfg_v MATCHES "^KICKOS_[A-Z0-9_]+$")
      continue()
    endif()
    unset(_kcfg_type)
    get_property(_kcfg_type CACHE "${_kcfg_v}" PROPERTY TYPE)
    set(_kcfg_shape "offer")
    if(NOT DEFINED _kcfg_type)
      set(_kcfg_val "${${_kcfg_v}}")
    else()
      get_property(_kcfg_val CACHE "${_kcfg_v}" PROPERTY VALUE)
      if(_kcfg_type STREQUAL "UNINITIALIZED")
        set(_kcfg_shape "assert")
      endif()
    endif()
    # A raw semicolon would split one candidate into several arguments, and the tail of a
    # service list then reaches the generator as a request of its own.
    string(REPLACE ";" "\\;" _kcfg_val "${_kcfg_val}")
    list(APPEND _kcfg_req "${_kcfg_shape}:${_kcfg_v}=${_kcfg_val}")
  endforeach()
  # Translate these promptless values into explicit choice requests.
  if(DEFINED KICKOS_CONSOLE)
    string(TOUPPER "${KICKOS_CONSOLE}" _kcfg_console)
    list(APPEND _kcfg_req "CONFIG_CONSOLE_${_kcfg_console}=y")
  endif()
  if(DEFINED KICKOS_TELEMETRY)
    string(TOUPPER "${KICKOS_TELEMETRY}" _kcfg_telem)
    list(APPEND _kcfg_req "CONFIG_TELEMETRY_${_kcfg_telem}=y")
  endif()

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
  message(STATUS "KickOS: configuration from ${KICKOS_KCONFIG_BASE}")

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

# Publish numeric flags for -Wundef-clean consumers. PE32+ cannot safely
# use weak undefined references through the GOT.
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

option(KICKOS_SCHED_PERIODIC_TICK "Force a classic periodic scheduler tick" OFF)
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
option(KICKOS_BENCH_SCHED "The scheduler's own bench counters, above one kernel core" OFF)
if(KICKOS_BENCH_SCHED)
  add_compile_definitions(KICKOS_BENCH_SCHED=1)
else()
  add_compile_definitions(KICKOS_BENCH_SCHED=0)
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

option(KICKOS_SMP_TRACE "Record a per-core park/wake event ring for SMP diagnosis" OFF)
if(KICKOS_SMP_TRACE)
  add_compile_definitions(KICKOS_SMP_TRACE=1)
else()
  add_compile_definitions(KICKOS_SMP_TRACE=0)
endif()

# Select native chip transport or SEGGER RTT. Do not cache resolved config
# values: they would override later defconfig changes.
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

# Where libc asks __getreent the thread pointer answers it: lx6 above one kernel core, and every
# armv8a and rv64imac image, whose newlib the toolchain files take from conan/newlib. Elsewhere
# one word is seated at every switch, which is correct only while one core runs its readers.
set(KICKOS_REENT_PER_THREAD 0)
set(KICKOS_REENT_IN_TCB 0)
if(KICKOS_LIBC_REENT)
  if(KICKOS_ARCH STREQUAL "rv64imac")
    set(KICKOS_REENT_PER_THREAD 1)
  elseif(KICKOS_ARCH STREQUAL "armv8a"
         OR (KICKOS_ARCH STREQUAL "lx6" AND KICKOS_KERNEL_CORES GREATER 1))
    if(NOT KICKOS_TLS)
      message(FATAL_ERROR
        "KickOS: KICKOS_TLS is off on arch '${KICKOS_ARCH}' with "
        "KICKOS_KERNEL_CORES=${KICKOS_KERNEL_CORES}. There each thread reaches its libc "
        "reentrant state through the first word of its TLS control block at the thread "
        "pointer, and only KICKOS_TLS carves that block and seats the register. Set "
        "KICKOS_TLS=y in boards/${KICKOS_BOARD}/configs/<variant>/defconfig.")
    endif()
    set(KICKOS_REENT_PER_THREAD 1)
    set(KICKOS_REENT_IN_TCB 1)
  endif()
endif()
add_compile_definitions(KICKOS_REENT_PER_THREAD=${KICKOS_REENT_PER_THREAD}
                        KICKOS_REENT_IN_TCB=${KICKOS_REENT_IN_TCB})

# Positive identification: a new arch lands in the 0 branch of every `#if KICKOS_ARCH_SIM`.
if(KICKOS_ARCH STREQUAL "sim")
  set(_arch_sim 1)
else()
  set(_arch_sim 0)
endif()
add_compile_definitions(KICKOS_ARCH_SIM=${_arch_sim})

# Chips and architectures opt into capabilities through named .cmake files.
# Include them in this scope so their variables reach subdirectories.
# Where Kconfig declares the same capability, require a matching opt-in file.

# Trace IDs must match record.h and kicktrace.py. Include chip capabilities
# first so chip-specific values override architecture defaults.
if(KICKOS_CHIP)
  set(_caps_optin "${CMAKE_CURRENT_SOURCE_DIR}/arch/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/caps.cmake")
  if(EXISTS "${_caps_optin}")
    include("${_caps_optin}")
  endif()
endif()
# Do not override fault isolation: duplicate architecture and fallback
# definitions would make archive order select the implementation.
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

# terminate.cmake selects process-exit fault termination instead of the LED fallback.
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

# Select the chip MPU backend, defaulting to PMSAv7. Use NONE without an MPU.
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

# The IPC fastpath flag must agree across syscall stubs, architecture entry
# and the kernel leaf implementation.
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

# Reject x86 SMP before the generic hardware checks: this backend has no
# remote TLB invalidation. The backend also has a compile-time assertion.
if(KICKOS_ARCH STREQUAL "x86_64" AND KICKOS_KERNEL_CORES GREATER 1)
  message(FATAL_ERROR
    "KickOS: KICKOS_KERNEL_CORES=${KICKOS_KERNEL_CORES} puts one kernel on more than one core "
    "on arch 'x86_64', whose map editor invalidates with INVLPG and with a root-register "
    "rewrite. Both act on the ISSUING core alone, the ISA carrying no remote form of either, "
    "so a peer goes on translating through an entry this core has removed until an "
    "inter-processor interrupt makes it drop it, and arch/x86/x86_64/aspace_x86_64.cc sends "
    "none. Land that shootdown and drop this refusal in the same change, or select one kernel "
    "core: the AMP model runs a kernel per core over disjoint memory and needs no remote "
    "invalidate.")
endif()

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

# Reject multicore IPC fastpaths after checking hardware support.
# The implementation also has a compile-time assertion.
if(KICKOS_ARCH_HAS_IPC_FASTPATH AND KICKOS_KERNEL_CORES GREATER 1)
  message(FATAL_ERROR
    "KickOS: arch '${KICKOS_ARCH}' opts into the trap-handler IPC fastpath in "
    "${_ipc_fastpath_optin}, and KICKOS_KERNEL_CORES=${KICKOS_KERNEL_CORES} puts one kernel "
    "on more than one core. The fastpath holds NO lock object: it is entered with the local "
    "interrupt mask the trap took and mutates a peer's thread control block, its endpoint "
    "and its capability table under that mask alone, which is exclusion on one core and "
    "nothing on another (docs/design-multicore.md section 4 gap 1, ruled on by N10). Either "
    "delete ${_ipc_fastpath_optin}, or select one kernel core: the AMP model runs a kernel "
    "per core over disjoint memory and keeps the fastpath on each of them.")
endif()

# The generated header guards its own define with #ifndef, so this wins where both are seen.
add_compile_definitions(KICKOS_NUM_CORES=${KICKOS_NUM_CORES})
# Beside the count, so an APP TU can ask how many cores one kernel schedules on: a claim about
# two threads' relative progress is a claim about that number.
add_compile_definitions(KICKOS_KERNEL_CORES=${KICKOS_KERNEL_CORES})

# Beside them again, for an app TU that has to ask which MODEL it runs under.
add_compile_definitions(KICKOS_MULTICORE_MODEL_SHARED=${KICKOS_MULTICORE_MODEL_SHARED})

# Size the doorbell matrix from the partition or image core count.
# Shared-image node IDs are core indices and must fit this matrix.
if(KICKOS_DOORBELL_CORES LESS KICKOS_NUM_CORES)
  message(FATAL_ERROR
    "KickOS: KICKOS_DOORBELL_CORES resolves ${KICKOS_DOORBELL_CORES} and this image drives "
    "${KICKOS_NUM_CORES} cores. The matrix is indexed by machine core and cannot be narrower "
    "than the cores the image drives. KICKOS_NUM_CORES is the authority and the width derives "
    "from it in Kconfig, so this says the derivation is broken rather than a defconfig.")
endif()
if(KICKOS_AMP_NODE AND NOT KICKOS_AMP_OWN_IMAGE
   AND KICKOS_AMP_NODES GREATER KICKOS_DOORBELL_CORES)
  message(FATAL_ERROR
    "KickOS: KICKOS_AMP_NODES is ${KICKOS_AMP_NODES} and the doorbell's matrix is "
    "${KICKOS_DOORBELL_CORES} cores wide. Under the shared image a node's identity IS its core "
    "register, so amp::core_of answers the node index itself and a node above the matrix's last "
    "row is rung on a core that has none. KICKOS_NUM_CORES is the authority on the width: state "
    "at most that many nodes, or raise the core count, in "
    "boards/${KICKOS_BOARD}/configs/${KICKOS_CONFIG_VARIANT}/defconfig.")
endif()

# Non-shared configurations have no isolated cores.
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

# Expose AMP node configuration to apps that do not include board_config.h.
add_compile_definitions(KICKOS_AMP_NODE=${KICKOS_AMP_NODE})
add_compile_definitions(KICKOS_AMP_NODES=${KICKOS_AMP_NODES})
add_compile_definitions(KICKOS_AMP_OWN_IMAGE=${KICKOS_AMP_OWN_IMAGE})
if(KICKOS_AMP_DIAG_REPORT)
  add_compile_definitions(KICKOS_AMP_DIAG_REPORT=1
                          KICKOS_AMP_DIAG_TAG=${KICKOS_AMP_DIAG_TAG})
  message(STATUS "KickOS: AMP peer self-report ON (tag ${KICKOS_AMP_DIAG_TAG}): a peer writes "
                 "its node index, its vector-table base and that tag into the shared region")
endif()

include("${CMAKE_SOURCE_DIR}/cmake/amp_partition.cmake")

# Architecture-specific minimum stack for thread-exit cleanup.
if(NOT DEFINED KICKOS_MIN_STACK_SIZE)
  message(FATAL_ERROR
    "KickOS: no KICKOS_MIN_STACK_SIZE. The generated kickos_config.cmake carries it, "
    "derived per arch in Kconfig, so an empty value here means the fragment was not "
    "read or the arch resolved to none.")
endif()
add_compile_definitions(KICKOS_MIN_STACK_SIZE=${KICKOS_MIN_STACK_SIZE})
message(STATUS "KickOS: caller-stack floor KICKOS_MIN_STACK_SIZE=${KICKOS_MIN_STACK_SIZE} (arch=${KICKOS_ARCH})")

# Include service providers and their dependencies in the rescan group
# because they reference kickos_user.
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

# Override the RTT publication barrier on weakly ordered cores.
# The definition must contain the complete fence call.
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

# Here and not down in the install section: cmake/x86_64_boot.cmake installs its image
# recipe there too, and is included well before it.
set(KICKOS_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/KickOS")

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

  # A macro preserves the directory scope of the test being configured.
  # Register kickos_build outside it to avoid a self-dependency.
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

  # Tree checks must depend only on source files, not preset output.
  # Register without the build fixture and run from the repository root.
  _add_test(NAME doc_names
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_doc_names.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(doc_names PROPERTIES TIMEOUT 120 LABELS "host;tree")

  _add_test(NAME extern_c_linkage
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_extern_c_linkage.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(extern_c_linkage PROPERTIES TIMEOUT 120 LABELS "host;tree")

  _add_test(NAME ascii
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_ascii.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(ascii PROPERTIES TIMEOUT 120 LABELS "host;tree")

  _add_test(NAME spdx
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_spdx.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(spdx PROPERTIES TIMEOUT 120 LABELS "host;tree")

  _add_test(NAME include_guards
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_include_guards.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(include_guards PROPERTIES TIMEOUT 120 LABELS "host;tree")

  _add_test(NAME shell_special_names
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_shell_special_names.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(shell_special_names PROPERTIES TIMEOUT 120 LABELS "host;tree")

  _add_test(NAME oot_arch_cover
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_oot_arch_cover.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(oot_arch_cover PROPERTIES TIMEOUT 120 LABELS "host;tree")

  # Takes the generated board config: the gate must read the knob spelling C reads.
  _add_test(NAME cpu_id_fold
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_cpu_id_fold.sh"
                    "${PROJECT_BINARY_DIR}/generated/include/kickos/board_config.h"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  # Exit 77 skips configurations without separate peer kernel images.
  set_tests_properties(cpu_id_fold PROPERTIES TIMEOUT 120 LABELS host SKIP_RETURN_CODE 77)

  _add_test(NAME dash_punct
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_dash_punct.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  # 300 and not the 120 its siblings take, measured rather than guessed: this is the heaviest
  # corpus scan in the set at 21 s idle against 6 s for the next, and a contended box has taken
  # it to 117 s. A gate that runs out of budget is reported as a FAILURE by anything counting
  # failures, so a thin budget here reads as a corpus defect that did not happen.
  set_tests_properties(dash_punct PROPERTIES TIMEOUT 300 LABELS "host;tree")

  _add_test(NAME ternary
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_ternary.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(ternary PROPERTIES TIMEOUT 120 LABELS "host;tree")

  _add_test(NAME chip_divisor_rate
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_chip_divisor_rate.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(chip_divisor_rate PROPERTIES TIMEOUT 120 LABELS "host;tree")

  _add_test(NAME panic_banners
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_panic_banners.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(panic_banners PROPERTIES TIMEOUT 120 LABELS "host;tree")

  _add_test(NAME syscall_return_codes
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_syscall_return_codes.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(syscall_return_codes PROPERTIES TIMEOUT 120 LABELS "host;tree")

  _add_test(NAME death_stack_seating
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_death_stack_seating.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(death_stack_seating PROPERTIES TIMEOUT 120 LABELS "host;tree")

  _add_test(NAME panic_stack_seat
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_panic_stack_seat.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(panic_stack_seat PROPERTIES TIMEOUT 120 LABELS "host;tree")

  _add_test(NAME mpu_record
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_mpu_record.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(mpu_record PROPERTIES TIMEOUT 120 LABELS "host;tree")

  _add_test(NAME park_death_point
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_park_death_point.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(park_death_point PROPERTIES TIMEOUT 120 LABELS "host;tree")

  _add_test(NAME kernel_data_reserve
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_kernel_data_reserve.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(kernel_data_reserve PROPERTIES TIMEOUT 120 LABELS "host;tree")

  _add_test(NAME app_heap_align
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_app_heap_align.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(app_heap_align PROPERTIES TIMEOUT 120 LABELS "host;tree")

  _add_test(NAME object_budget_asserts
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_object_budget_asserts.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(object_budget_asserts PROPERTIES TIMEOUT 120 LABELS "host;tree")

  # Reconstruct the preset name from board, variant and optional AMP node.
  # Keep this format consistent with tests/static/preset_boards.cmake.
  if(KICKOS_CONFIG_VARIANT STREQUAL "base")
    set(_fs_preset_key "${KICKOS_BOARD}")
  else()
    set(_fs_preset_key "${KICKOS_BOARD}-${KICKOS_CONFIG_VARIANT}")
  endif()
  if(KICKOS_AMP_NODE AND KICKOS_AMP_OWN_IMAGE)
    set(_fs_preset_key "${_fs_preset_key}-n${KICKOS_AMP_NODE_ID}")
  endif()

  # Register only pairs listed in trap_redzone_roots.txt.
  set(_fs_redzone_roots "${PROJECT_SOURCE_DIR}/tests/static/trap_redzone_roots.txt")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_fs_redzone_roots}")
  set(_fs_redzone_preset "")
  # The trailing anchor is required: `preset armv7m qemu` must not match qemu-flat.
  file(STRINGS "${_fs_redzone_roots}" _fs_redzone_decls
       REGEX "^preset[ \t]+${KICKOS_ARCH}[ \t]+${_fs_preset_key}[ \t]*$")
  if(NOT _fs_redzone_decls STREQUAL "")
    set(_fs_redzone_preset "${_fs_preset_key}")
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
  set_tests_properties(trap_redzone_decls PROPERTIES TIMEOUT 120 LABELS "host;tree")

  # Register translating presets listed in console_reach_roots.txt.
  set(_fs_reach_decl "${PROJECT_SOURCE_DIR}/tests/static/console_reach_roots.txt")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_fs_reach_decl}")
  # The trailing anchor is required: `preset rv64imac qemu-riscv64` must not match
  # qemu-riscv64-sv48.
  file(STRINGS "${_fs_reach_decl}" _fs_reach_decls
       REGEX "^preset[ \t]+${KICKOS_ARCH}[ \t]+${_fs_preset_key}[ \t]*$")
  if(NOT _fs_reach_decls STREQUAL "")
    _add_test(NAME console_reach
              COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_console_reach.sh"
                      "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}"
                      "${_fs_preset_key}" "${KICKOS_ARCH}")
    set_tests_properties(console_reach PROPERTIES TIMEOUT 1800 LABELS host)
  endif()

  # Check cache-as-SRAM pinning using the chip and resolved configuration.
  if(KICKOS_CHIP STREQUAL "rp2350")
    _add_test(NAME amp_no_xip_pin
              COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_amp_no_xip_pin.sh"
                      "${PROJECT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
    # Exit 77 skips configurations without separate peer kernel images.
    set_tests_properties(amp_no_xip_pin PROPERTIES TIMEOUT 300 LABELS host
                         SKIP_RETURN_CODE 77)
  endif()

  # For RV32, check gp setup order in linked images and relocation flags in
  # the architecture archive. RV64 uses a separate kernel-half mechanism.
  if(KICKOS_ARCH STREQUAL "rv32imac")
    add_test(NAME rv32_trap_gp_anchor
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_rv32_trap_gp_anchor.sh"
              "${CMAKE_OBJDUMP}"
              "${KICKOS_CHIP_INCLUDE_DIR}/kickos/chip_limits.h"
              "$<TARGET_FILE:kickos_arch_${KICKOS_ARCH}>"
              "$<TARGET_FILE:hello>"
              "$<TARGET_FILE:selftest>")
    set_tests_properties(rv32_trap_gp_anchor PROPERTIES TIMEOUT 120 LABELS host)
  endif()

  _add_test(NAME atomic_rmw
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_atomic_rmw.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(atomic_rmw PROPERTIES TIMEOUT 120 LABELS "host;tree")

  # The host fixture cannot interrupt between slot validation and use;
  # check the single-snapshot rule statically.
  _add_test(NAME amp_slot_snapshot
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_amp_slot_snapshot.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(amp_slot_snapshot PROPERTIES TIMEOUT 120 LABELS "host;tree")

  # Test the named-test helper with a stub ctest; registration is checked per preset.
  _add_test(NAME ci_named_lists
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_ci_named_lists.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(ci_named_lists PROPERTIES TIMEOUT 120 LABELS "host;tree")

  # No gawk-only extension in an awk program this tree runs: a non-gawk awk REFUSES such a
  # program, which leaves the calling gate reporting its own clause instead of the awk.
  _add_test(NAME awk_portable
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_awk_portable.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(awk_portable PROPERTIES TIMEOUT 120 LABELS "host;tree")

  # The kernel-core seam against the machine's core identity. Corpus-wide source scan, so it
  # is registered from every board and reads no build output.
  _add_test(NAME kernel_core_index
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_kernel_core_index.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(kernel_core_index PROPERTIES TIMEOUT 120 LABELS "host;tree")

  _add_test(NAME service_lists
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_service_lists.sh"
                    "${CMAKE_COMMAND}" "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(service_lists PROPERTIES TIMEOUT 120 LABELS "host;tree")

  _add_test(NAME preset_defconfig
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_preset_defconfig.sh"
                    "${CMAKE_COMMAND}" "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(preset_defconfig PROPERTIES TIMEOUT 120 LABELS "host;tree")

  kickos_find_kconfig(_kconfig_py _kconfig_found)
  _add_test(NAME kconfig_gen
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_kconfig_gen.sh"
                    "${_kconfig_py}" "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_COMMAND}")
  set_tests_properties(kconfig_gen PROPERTIES TIMEOUT 120 LABELS "host;tree")

  # Configure child trees to test the shared-kernel hardware predicate.
  # Reuse this Kconfig interpreter; no image is built.
  _add_test(NAME smp_predicate
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_smp_predicate.sh"
                    "${_kconfig_py}" "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_COMMAND}")
  set_tests_properties(smp_predicate PROPERTIES TIMEOUT 300 LABELS "host;tree")

  # Test isolated-core mask rejection using synthetic values.
  _add_test(NAME isolated_cores
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_isolated_cores.sh"
                    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_COMMAND}")
  set_tests_properties(isolated_cores PROPERTIES TIMEOUT 120 LABELS "host;tree")

  # Compile the optional SMP trace path using this build's recorded flags.
  _add_test(NAME smp_trace_builds
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_smp_trace_builds.sh"
                    "${CMAKE_BINARY_DIR}/compile_commands.json")
  set_tests_properties(smp_trace_builds PROPERTIES TIMEOUT 180 LABELS host)

  # Check Kconfig-to-CMake coverage and request types across all defconfigs.
  _add_test(NAME kconfig_reach
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_kconfig_reach.sh"
                    "${_kconfig_py}" "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(kconfig_reach PROPERTIES TIMEOUT 300 LABELS "host;tree")

endif()

foreach(_dir lib arch kernel user system)
  add_subdirectory(${_dir})
endforeach()

# After arch, which generates the script. Where the carve is never zero each default stack's link
# assert can be driven to its floor.
if(KICKOS_BUILD_TESTS AND KICKOS_TLS AND NOT KICKOS_TLS_FROM_SP AND KICKOS_REENT_IN_TCB
   AND KICKOS_LINKER_SCRIPT)
  add_test(NAME tls_carve_link
           COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_tls_carve_link.sh"
                   "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}" "${_fs_preset_key}"
                   "${KICKOS_LINKER_SCRIPT}")
  set_tests_properties(tls_carve_link PROPERTIES TIMEOUT 1800 LABELS host)
endif()

# After the trees above: the gates under it compile kernel and lib sources, and read the
# include path and definitions off the targets those trees create.
if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  add_subdirectory(tests/unit)
endif()

# Set before kickos_core below, whose LINK_GROUP pulls the provider archive.
set(KICKOS_INIT_PROVIDER "kickos_default_init" CACHE STRING
  "CMake target providing kickos_init_entry (the userspace init service). Default = the built-in kickos_system service; a power user names their own target.")

if(TARGET kickos_kernel)
  # Consumers link a leaf (kickos / kickos_cxx), never kickos_core directly.
  add_library(kickos_core INTERFACE)
  # Place init and service providers after the kernel in the rescan group.
  # Resolve their dependency closure after system targets exist.
  if(NOT DEFINED KICKOS_SERVICE_LIST_LIBS)
    kickos_service_libs_closure("${KICKOS_SERVICE_LIST}" KICKOS_SERVICE_LIST_LIBS)
    message(STATUS "KickOS: service-list closure = ${KICKOS_SERVICE_LIST_LIBS}")
  endif()
  # CMake forbids a library in two groups within one target; build a group
  # for each configuration below.
  if(KICKOS_CHIP AND TARGET kickos_chip_${KICKOS_CHIP})
    set(_kickos_group "kickos_user,kickos_kernel,${KICKOS_INIT_PROVIDER},${KICKOS_SERVICE_LIST_LIBS},${KICKOS_BOARD_PINMAP_LIBS},kickos_chip_${KICKOS_CHIP},kickos_arch_${KICKOS_ARCH},kickos_lib")
  else()
    set(_kickos_group "kickos_user,kickos_kernel,${KICKOS_INIT_PROVIDER},${KICKOS_SERVICE_LIST_LIBS},${KICKOS_BOARD_PINMAP_LIBS},kickos_arch_${KICKOS_ARCH},kickos_lib")
  endif()
  # Link the interface-only kickos_system outside the rescan group.
  target_link_libraries(kickos_core INTERFACE kickos_system)
  # The app writes `int main`, renamed to the kernel-called symbol; app.h is force-included
  # below so the renamed C++ main gets C linkage (unmangled).
  target_compile_definitions(kickos_core INTERFACE main=kickos_app_main)
  # Export every macro affecting shipped layouts or inline definitions to avoid ODR mismatch.
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
    KICKOS_DEBUG=$<BOOL:${KICKOS_DEBUG}>
    # Always define SPI backend flags because app builds use -Wundef.
    KICKOS_SPI_LOCAL=$<BOOL:${KICKOS_SPI_LOCAL_ENGINE}>)
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
      $<$<BOOL:${KICKOS_NODE_VECTOR_FORCE_SYMBOL}>:-Wl,-u,${KICKOS_NODE_VECTOR_FORCE_SYMBOL}>
      # Force libc stubs into the link. RX prefixes C symbols, so _exit becomes __exit.
      "$<$<STREQUAL:${KICKOS_ARCH_FAMILY},rx>:-Wl,-u,__exit>"
      "$<$<NOT:$<STREQUAL:${KICKOS_ARCH_FAMILY},rx>>:-Wl,-u,_exit>"
      # A chip with a checksummed flash second stage (RP2040 boot2) must force its
      # .boot2 blob object in too; nothing references it.
      $<$<BOOL:${KICKOS_BOOT2_FORCE_SYMBOL}>:-Wl,-u,${KICKOS_BOOT2_FORCE_SYMBOL}>
      "$<BUILD_INTERFACE:SHELL:-T ${KICKOS_LINKER_SCRIPT}>"
      "$<INSTALL_INTERFACE:SHELL:-T $<INSTALL_PREFIX>/${CMAKE_INSTALL_LIBDIR}/kickos/${KICKOS_CHIP}.ld>")
    # Track the linker script as a dependency; -T alone does not trigger relinking.
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
  # Keep the MCU runtime in the rescan group so newlib can resolve _sbrk
  # from kickos_user after _sbrk_r is extracted.
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

    # Link the custom terminate handler as an object before libstdc++.
    # This source requires exceptions and RTTI.
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
# created, and defines the kickos_x86_64_link_image that kickos_emit_image calls.
if(KICKOS_ARCH STREQUAL "x86_64")
  include(cmake/x86_64_boot.cmake)
endif()

# Apps need a chip target (or the sim); a board whose chip is named but not yet brought up
# builds the libraries only.
if(KICKOS_BUILD_APPS
   AND ((KICKOS_ARCH STREQUAL "sim") OR (KICKOS_CHIP AND TARGET kickos_chip_${KICKOS_CHIP})))
  add_subdirectory(user/apps)
endif()

# Register tests after app targets exist.
if(KICKOS_BUILD_TESTS)
  add_subdirectory(tests/integration)
endif()

# Validate providers after apps/superprojects can define them.
# Library-only builds need no linked provider.
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
  # Require retention declarations on the selected service list regardless of its name.
  get_target_property(_sl_declared ${KICKOS_SERVICE_LIST} KICKOS_ENDPOINT_RETAINED_DECLARED)
  if(NOT _sl_declared)
    message(FATAL_ERROR
      "KickOS: KICKOS_SERVICE_LIST='${KICKOS_SERVICE_LIST}' states no RETAINED_ENDPOINTS. "
      "A service list must state how many endpoint pool slots it leaves ROOT holding for the "
      "life of the image: count a console publish or handover as 1 and each "
      "KOS_DRV_EP_HANDOVER or KOS_DRV_EP_RETAIN driver endpoint as 1, and state 0 explicitly "
      "if it holds none. cmake/cap_table.cmake sums it against "
      "CONFIG_KICKOS_TASK_ENDPOINT_BUDGET, so a list that declined to answer is counted as "
      "zero and the ceiling it is checked against is false.")
  endif()
  # Without this a misspelled target name reaches the LINK_GROUP as a bare -l, so the board
  # silently keeps kickos_pinmap_none and boots with unmuxed pins.
  if(NOT TARGET ${KICKOS_BOARD_PINMAP})
    message(FATAL_ERROR "KICKOS_BOARD_PINMAP='${KICKOS_BOARD_PINMAP}' is not a CMake target. Name a target that defines kos_board_pinmap, or use the default kickos_pinmap_none.")
  endif()
endif()

# Resolve after system providers and app demand declarations are available.
kickos_endpoint_seats_check("${KICKOS_SERVICE_LIST}")
kickos_cap_table_resolve("${KICKOS_SERVICE_LIST}" KICKOS_CAP_TABLE_SLOTS
                         KICKOS_CAP_TABLE_CHUNK KICKOS_CAP_TABLE_CHILD_WIDTH
                         KICKOS_CAP_TABLE_REPLY_MAX)
configure_file("${PROJECT_SOURCE_DIR}/kernel/include/kickos/config/cap_width.h.in"
               "${PROJECT_BINARY_DIR}/generated/include/kickos/config/cap_width.h" @ONLY)
configure_file("${PROJECT_SOURCE_DIR}/kernel/include/kickos/config/amp_ports.h.in"
               "${PROJECT_BINARY_DIR}/generated/include/kickos/config/amp_ports.h" @ONLY)
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

# CeCILL-C is file-level copyleft: ship the license with the package.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/${PROJECT_NAME}")

# Use the include path reported by the architecture target.
# Ship mpu_encoded.h only when this build enables an MPU.
set(_kos_hdr_exclude "")
if(NOT KICKOS_HAVE_MPU)
  set(_kos_hdr_exclude PATTERN "mpu_encoded.h" EXCLUDE)
endif()
install(DIRECTORY
          "${CMAKE_CURRENT_SOURCE_DIR}/include/"
          "${CMAKE_CURRENT_SOURCE_DIR}/arch/include/"
          "${KICKOS_ARCH_INCLUDE_DIR}/"
          "${CMAKE_CURRENT_SOURCE_DIR}/kernel/include/"
          "${CMAKE_CURRENT_SOURCE_DIR}/lib/include/"
          "${CMAKE_CURRENT_SOURCE_DIR}/user/include/"
          "${CMAKE_CURRENT_SOURCE_DIR}/system/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        PATTERN "*.h.in" EXCLUDE
        ${_kos_hdr_exclude})

# An installed consumer that includes a kernel header must take the same width as the
# prebuilt libraries, or its sizeof(Thread) does not match theirs.
install(FILES "${PROJECT_BINARY_DIR}/generated/include/kickos/config/cap_width.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/kickos/config")

# The partition's port list an app's <kickos/amp.h> derives its capability indices from.
install(FILES "${PROJECT_BINARY_DIR}/generated/include/kickos/config/amp_ports.h"
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

# No NAMESPACE: an app links and kickos_add_app_target asserts the bare target names
# (kickos_arch_<arch>, ...), identically in-tree and installed. Do not add one.
install(EXPORT KickOSTargets
        FILE KickOSTargets.cmake
        DESTINATION "${KICKOS_CMAKE_DIR}")

# Export the class-backend selections used by this build.
get_property(_kos_classes GLOBAL PROPERTY KICKOS_CLASS_BACKEND_CLASSES)
set(KICKOS_CLASS_BACKEND_DECLS "")
if(_kos_classes)
  list(REMOVE_DUPLICATES _kos_classes)
  foreach(_kos_class IN LISTS _kos_classes)
    kickos_class_backend("${_kos_class}" _kos_backend)
    string(APPEND KICKOS_CLASS_BACKEND_DECLS
      "kickos_select_class_backend(${_kos_class} ${_kos_backend})\n")
  endforeach()
endif()

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

# Install the toolchain that configured this build, with board.cmake beside it.
if(NOT KICKOS_ARCH STREQUAL "sim")
  if(NOT CMAKE_TOOLCHAIN_FILE)
    message(FATAL_ERROR "KickOS: board '${KICKOS_BOARD}' is a cross target configured with no "
      "CMAKE_TOOLCHAIN_FILE, so the package would ship none and no out-of-tree consumer could "
      "build against it.")
  endif()
  set(_kos_tc_install "${CMAKE_TOOLCHAIN_FILE}")
  # Include recursively referenced toolchain fragments. Exclude board.cmake,
  # which is generated below.
  set(_kos_tc_pending "${CMAKE_TOOLCHAIN_FILE}")
  set(_kos_tc_seen "")
  while(_kos_tc_pending)
    list(POP_FRONT _kos_tc_pending _kos_tc_from)
    file(READ "${_kos_tc_from}" _kos_tc_text)
    string(REGEX MATCHALL "\\\${CMAKE_CURRENT_LIST_DIR}/[A-Za-z0-9_.+-]+\\.cmake"
           _kos_tc_incs "${_kos_tc_text}")
    foreach(_kos_inc IN LISTS _kos_tc_incs)
      string(REPLACE "\${CMAKE_CURRENT_LIST_DIR}/" "" _kos_inc "${_kos_inc}")
      if(_kos_inc STREQUAL "board.cmake" OR _kos_inc IN_LIST _kos_tc_seen)
        continue()
      endif()
      list(APPEND _kos_tc_seen "${_kos_inc}")
      if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${_kos_inc}")
        message(FATAL_ERROR "KickOS: ${_kos_tc_from} includes ${_kos_inc}, which is "
          "not in cmake/, so the package would ship a toolchain file that cannot configure.")
      endif()
      list(APPEND _kos_tc_install "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${_kos_inc}")
      list(APPEND _kos_tc_pending "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${_kos_inc}")
    endforeach()
  endwhile()
  list(REMOVE_DUPLICATES _kos_tc_install)
  install(FILES ${_kos_tc_install} DESTINATION "${KICKOS_CMAKE_DIR}")
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
