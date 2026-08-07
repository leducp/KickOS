# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

cmake_minimum_required(VERSION 3.24)
# Two versions, deliberately not the same thing. The project version is numeric because
# write_basic_package_version_file demands it, and tracks the milestone only: it is what an
# out-of-tree consumer's find_package(KickOS) matches against.
project(KickOS VERSION 0.4.7 LANGUAGES C CXX ASM)
# KICKOS_VERSION is the build identity in the boot banner, and takes a point-release suffix
# CMake would reject above. Scheme: 0.<milestone>.<submilestone>[-<point>].
set(KICKOS_VERSION "0.4.7-1")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(cmake/kickos.cmake)
include(cmake/kconfig.cmake)
# Pure constants, readable before anything is configured, and they must be: user/apps keys
# a selftest expectation on the chunk granule, and that subdirectory is added long before
# the width sum runs at the bottom of this file.
include(cmake/cap_geometry.cmake)

# KICKOS: a plain boolean an add_subdirectory(KickOS) consumer tests with if(KICKOS),
# exactly like if(LINUX). __KickOS__: defined for every in-tree TU here, and carried
# to downstream consumers as a usage requirement of the kickos/kickos_cxx leaves.
set(KICKOS TRUE CACHE INTERNAL "Building against KickOS")
add_compile_definitions(__KickOS__=1)

# --- Board / arch selection -------------------------------------------------
# The selectable board list is derived from boards/*/board.cmake (the sole source of
# truth) so the help string can never go stale as boards are added.
kickos_board_names(_kickos_board_names)
list(JOIN _kickos_board_names "|" _kickos_board_help)
set(KICKOS_BOARD "sim" CACHE STRING "Target board: ${_kickos_board_help}")
# One descriptor per board (boards/<board>/board.cmake) sets arch + chip + -mcpu
# + the ISA family; the cross toolchain file already read the same file pre-project
# for the CPU flags. Family (arm|rx|xtensa|sim|...) routes arch/<family>/... and the
# family-specific toolchain; it is derived from the arch when the descriptor omits
# it (so the ARM boards need no per-board line), else set explicitly (rx, xtensa).
kickos_load_board_descriptor("${KICKOS_BOARD}" KICKOS_ARCH KICKOS_CHIP KICKOS_ARCH_FAMILY)
message(STATUS "KickOS: board=${KICKOS_BOARD} family=${KICKOS_ARCH_FAMILY} arch=${KICKOS_ARCH} chip=${KICKOS_CHIP}")

# Foundational, dependency-free headers (units.h: _ns/_us/_ms/_s + byte literals)
# shared by EVERY layer (arch, lib, kernel, userspace). Kept in a repo-root
# include/ (below all components) so even the lowest layer (arch) uses them
# without reaching up into a peer component's dir.
include_directories("${PROJECT_SOURCE_DIR}/include")

# Holds config/cap_width.h, written at the bottom of this file, and the generated
# kickos/board_config.h. The directory must be added HERE, before any add_subdirectory: a
# subdirectory copies the include directories in force when it is added, and the width is
# not known until after user/apps.
include_directories("${PROJECT_BINARY_DIR}/generated/include")

# --- Board provisioning -----------------------------------------------------
# No board_config.h exists in the source tree: the provisioning knobs come from the
# board's defconfig and reach C through the GENERATED header on the include path above.
# What is left under a chip's include/ is the chip's own CONSTANTS (chip_limits.h,
# chip_mmap.h), and that one directory serves every board on the chip. The chip startup.S
# reads chip_limits.h too, which is what keeps MAX_IRQ and the vector-table .rept one fact.
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

# The same kind of header one tier down: a board's own WIRING constants (board_wiring.h),
# the facts that differ between two boards sharing a chip. Only the boards whose chip
# backend reads them ship the directory, so its absence is normal and not an error.
set(KICKOS_BOARD_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/${KICKOS_BOARD}/include")
if(EXISTS "${KICKOS_BOARD_INCLUDE_DIR}")
  include_directories("${KICKOS_BOARD_INCLUDE_DIR}")
else()
  set(KICKOS_BOARD_INCLUDE_DIR "")
endif()

# --- Configuration from Kconfig --------------------------------------------
# A board is configured from boards/<board>/configs/<variant>/defconfig: the generator
# resolves the declarations and writes both the C header the compile reads and the
# fragment included below. Board plus variant is the whole selection a preset makes,
# and each variant defconfig is a complete statement rather than a delta on another.
set(KICKOS_CONFIG_VARIANT "base" CACHE STRING
    "Configuration variant under boards/<board>/configs/")
set(KICKOS_DEFCONFIG
    "${CMAKE_CURRENT_SOURCE_DIR}/boards/${KICKOS_BOARD}/configs/${KICKOS_CONFIG_VARIANT}/defconfig")
if(EXISTS "${KICKOS_DEFCONFIG}")

  # TRANSITIONAL. These knobs still arrive as a bare -D or a cache variable, so they
  # are handed to the generator as requests it can refuse rather than being applied
  # behind its back. Each arm dies with the mechanism that feeds it; the posture's
  # arm is gone because the posture is a variant.
  #
  # Every arm translates in BOTH directions. Sending only the positive one would let a
  # defconfig that enables something outrank a command line asking to turn it off, and
  # the request would be dropped without a word.
  set(_kcfg_req "")
  # A provisioning knob set on the CMake command line (-DKICKOS_MAX_THREADS=4) becomes a
  # REQUEST, and that is its only route: it does not reach the compiler as a -D behind the
  # resolution's back. So an out-of-range value is refused instead of silently disagreeing
  # with .config, and the generated header stays the one place C reads.
  #
  # KICKOS_MAX_HANDLES is absent on purpose: it is the ANSWER of the sum in
  # cmake/cap_table.cmake, not an input. The list is closed, so a -D naming anything
  # else never becomes a request and CMake merely reports it unused; only a name IN
  # the list can be refused by the generator.
  foreach(_knob KICKOS_MAX_THREADS KICKOS_USER_STACK_SIZE KICKOS_IDLE_STACK_SIZE
                KICKOS_ROOT_STACK_SIZE KICKOS_MAX_SEMAPHORES KICKOS_MAX_MUTEXES
                KICKOS_MAX_ENDPOINTS KICKOS_CAP_TABLE_SUPPLY KICKOS_MAX_SPAWN_GRANTS
                KICKOS_MAX_IRQ_HANDLES KICKOS_RTT_CH1_SIZE KICKOS_USER_HEAP_SIZE)
    if(DEFINED ${_knob})
      list(APPEND _kcfg_req "CONFIG_${_knob}=${${_knob}}")
    endif()
  endforeach()
  if(DEFINED KICKOS_CONSOLE)
    string(TOUPPER "${KICKOS_CONSOLE}" _kcfg_console)
    list(APPEND _kcfg_req "CONFIG_CONSOLE_${_kcfg_console}=y")
  endif()
  # Both enums go through as CONFIG_<CHOICE>_<VALUE>, so a value neither knows about
  # comes back as "no such symbol" rather than needing its own validation arm here.
  if(DEFINED KICKOS_TELEMETRY)
    string(TOUPPER "${KICKOS_TELEMETRY}" _kcfg_telem)
    list(APPEND _kcfg_req "CONFIG_TELEMETRY_${_kcfg_telem}=y")
  endif()
  # String-valued, so the request is quoted. A knob the fragment sets but this translation
  # omits is one the fragment silently overwrites, and four sim gates re-configure with
  # -DKICKOS_SERVICE_LIST=...
  foreach(_str KICKOS_SERVICE_LIST KICKOS_BOARD_PINMAP)
    if(DEFINED ${_str})
      list(APPEND _kcfg_req "CONFIG_${_str}=\"${${_str}}\"")
    endif()
  endforeach()
  foreach(_flag KICKOS_ENABLE_SELFTEST KICKOS_DEBUG KICKOS_BENCH
                KICKOS_SHUTDOWN_TO_BOOTLOADER KICKOS_TIMED_WAIT
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

  # The board descriptor's own answers, before the fragment can overwrite them. These
  # four are the one pair of INDEPENDENT sources in the seam (boards/<b>/board.cmake
  # against boards/Kconfig plus arch/Kconfig), so a typo in either is caught here.
  # Every other fragment value replaces a ladder arm and has nothing to compare with.
  set(_cmake_board "${KICKOS_BOARD}")
  set(_cmake_arch "${KICKOS_ARCH}")
  set(_cmake_chip "${KICKOS_CHIP}")
  set(_cmake_family "${KICKOS_ARCH_FAMILY}")
  include("${PROJECT_BINARY_DIR}/generated/kickos_config.cmake")
  kickos_kconfig_agree("the board" "${_cmake_board}" "${KICKOS_BOARD}")
  kickos_kconfig_agree("the arch" "${_cmake_arch}" "${KICKOS_ARCH}")
  kickos_kconfig_agree("the arch family" "${_cmake_family}" "${KICKOS_ARCH_FAMILY}")
  kickos_kconfig_agree("the chip" "${_cmake_chip}" "${KICKOS_CHIP}")

  # The declarations, AND the live .config: editing either must reach the next build.
  # .config is written during configure and so is older than the build system CMake
  # writes afterwards, which is why depending on it does not loop.
  set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" APPEND
               PROPERTY CMAKE_CONFIGURE_DEPENDS
               ${KICKOS_KCONFIG_SOURCES} "${PROJECT_BINARY_DIR}/generated/.config")
  message(STATUS "KickOS: configuration from ${KICKOS_DEFCONFIG}")

  # Three verbs on the live configuration. The defconfig is the SAVED starting point in
  # git; generated/.config is the live state this build is based on, exactly as a
  # preset seeds CMakeCache.txt once and the cache rules afterwards.
  #
  #   menuconfig    edit the live state
  #   savedefconfig persist it back to the board's defconfig, minimally
  #   defconfig     discard it and reload from the board's defconfig
  #
  # USES_TERMINAL because menuconfig is curses. Each runs the interpreter found at
  # configure; none is wired into the default build.
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
    # RELOADS, it does not merely discard: this is `make <board>_defconfig`, so it puts
    # .config back to what the saved defconfig says and the CONFIGURE_DEPENDS above makes
    # the next build pick it up. A target that deleted the file and printed an
    # instruction would be a workaround around the tool rather than the tool's own verb.
    add_custom_target(defconfig
      COMMAND "${CMAKE_COMMAND}" -E rm -f "${PROJECT_BINARY_DIR}/generated/.config"
      COMMAND "${_kcfg_py}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/kconfig/genconfig.py"
              "${CMAKE_CURRENT_SOURCE_DIR}" "${KICKOS_DEFCONFIG}"
              "${PROJECT_BINARY_DIR}/generated" ${_kcfg_req}
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      COMMENT "Reloading the live configuration from ${KICKOS_DEFCONFIG}")
  endif()
else()
  # Every knob now reaches C through the generated header, so a build with no
  # resolution behind it compiles nothing: name the variant that is missing rather
  # than failing later on an absent include.
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
# The fragment above already set the five knobs it owns as normal variables, and
# CMP0077 (NEW at this cmake_minimum_required) makes option() defer to those, so
# .config stays the single answer rather than being overwritten by a default here.
option(CONFIG_SCHED_PERIODIC_TICK "Force a classic periodic scheduler tick" OFF)
option(KICKOS_BUILD_APPS          "Build in-tree demo applications"          ON)
option(KICKOS_BUILD_TESTS         "Register CTest cases"                     ON)
# Test-only: exposes the guard-probe syscall (kos_guard_addr) used by the MPU
# privilege self-test. Kept out of the production syscall ABI by default.
option(KICKOS_ENABLE_SELFTEST     "Expose test-only self-test syscalls"      OFF)
# Cycle-accurate microbenchmark: PendSV brackets the switch body with DWT CYCCNT
# and the `bench` app reports min/avg/max switch cycles. Off by default (adds a
# few instructions to the switch); a dedicated benchmark build, telemetry off.
option(KICKOS_BENCH               "Cycle-accurate context-switch microbench"  OFF)
if(KICKOS_BENCH)
  add_compile_definitions(KICKOS_BENCH=1)
else()
  # Defined 0 (not undefined) so switch.S's `#if KICKOS_BENCH` is -Wundef-clean.
  add_compile_definitions(KICKOS_BENCH=0)
endif()

if(KICKOS_ENABLE_SELFTEST)
  # Directory-wide so kernel dispatch, the user stub, and the app agree.
  add_compile_definitions(KICKOS_ENABLE_SELFTEST=1)
endif()

# --- Bounded waits ----------------------------------------------------------
# ON: send/call/recv accept a deadline, a thread can be joined, and a thread can wait
# until it is the last one live. OFF: those five dispatch arms and their stubs are not
# built, and their (permanently allocated) syscall numbers answer -KOS_EINVAL. The
# numbers are never reused. See the help text in Kconfig for what OFF gives up:
# console_handover_finish falls back to the untimed send, so a console driver that hangs
# in bring-up parks the handing-over thread with nothing to recover it.
# Always defined 0 or 1 so `#if KICKOS_TIMED_WAIT` is -Wundef-clean.
option(KICKOS_TIMED_WAIT "Bounded waits: timed send/call/receive, join, wait-until-last" ON)
if(KICKOS_TIMED_WAIT)
  add_compile_definitions(KICKOS_TIMED_WAIT=1)
else()
  add_compile_definitions(KICKOS_TIMED_WAIT=0)
  message(STATUS "KickOS: bounded waits OFF (console bring-up parks unbounded)")
endif()

# --- Terminating images hand the chip back to its bootloader ----------------
# OFF: a terminating image halts, and reflashing an RP2040/RP2350 needs a physical
# BOOTSEL press. ON: both terminal dead-ends in kernel/init/console.cc call
# arch_reboot first. Those two are kickos_terminate (shutdown syscall, last-thread-out,
# the software fault reporter) and the kfault_terminate fallback (kpanic, and every ARM
# MPU/hard fault). So a fault-terminating bench image leaves the board in its download
# mode and an unattended flash/test loop becomes possible. Bench/CI knob only: a board
# that always jumps to its bootloader can never hold a soak image.
# Always defined 0 or 1 so `#if KICKOS_SHUTDOWN_TO_BOOTLOADER` is -Wundef-clean.
option(KICKOS_SHUTDOWN_TO_BOOTLOADER
       "Terminating images reboot into the chip bootloader instead of halting" OFF)
if(KICKOS_SHUTDOWN_TO_BOOTLOADER)
  # arch_reboot's declining fallback AND every chip backend are compiled out of a
  # non-selftest image, so the knob would not even link there. Refuse at configure
  # rather than emit an undefined reference from kickos_terminate.
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

# CMake's stock MinSizeRel flags drop -g, and an image with no debug info cannot be
# witnessed on silicon.
add_compile_options($<$<CONFIG:MinSizeRel>:-g>)

# Internal-consistency guards too expensive for a shipped image (the intrusive-list
# membership checks in <kickos/list.h>). OFF by default; the sim preset turns it ON.
# Always defined 0 or 1 so `#if KICKOS_DEBUG` is -Wundef-clean. Carried to consumers
# for the same ODR reason as KICKOS_HAVE_MPU (it changes an inline in a shipped header).
option(KICKOS_DEBUG "Build the internal-consistency guards (list membership, ...)" OFF)
if(KICKOS_DEBUG)
  add_compile_definitions(KICKOS_DEBUG=1)
else()
  add_compile_definitions(KICKOS_DEBUG=0)
endif()

# --- Console backend selection (docs/console-design.md) --------------------
# KICKOS_CONSOLE = chip | rtt | both | none. `chip` = the board's native transport
# (UART / semihosting / USIC); `rtt` = SEGGER RTT (needs a debug probe); resolved
# to compile-time KICKOS_CONSOLE_CHIP/RTT for the kernel console core. The fragment
# resolved it; a board wanting another backend states it in its defconfig, which is
# why nothing here names a board. NEVER cached: a cache entry written from .config
# would come back as a request on the next configure and outrank a changed defconfig.
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
# Bare-metal UART consoles want '\n' lowered to CR+LF (a raw serial terminal
# stays in the same column otherwise). The sim writes to a cooked host tty and
# must stay raw, else '\r' leaks into the TAP stream and breaks the test harness.
if(KICKOS_ARCH STREQUAL "sim")
  set(_con_crlf 0)
else()
  set(_con_crlf 1)
endif()
add_compile_definitions(KICKOS_CONSOLE_CHIP=${_con_chip} KICKOS_CONSOLE_RTT=${_con_rtt} KICKOS_CONSOLE_CRLF=${_con_crlf})
message(STATUS "KickOS: console=${KICKOS_CONSOLE} (chip=${_con_chip} rtt=${_con_rtt} crlf=${_con_crlf})")

# The sim arch, readable from the preprocessor. Positive identification only: a new
# arch must land in the 0 branch of every `#if KICKOS_ARCH_SIM`.
if(KICKOS_ARCH STREQUAL "sim")
  set(_arch_sim 1)
else()
  set(_arch_sim 0)
endif()
add_compile_definitions(KICKOS_ARCH_SIM=${_arch_sim})

# --- Telemetry backend selection (docs/reference/telemetry.md) -----------------
# KICKOS_TELEMETRY = off | rtt. Kernel-observability records (context switch,
# syscall, IRQ) emitted to RTT channel 1. Per-board default off (a benchmarking
# opt-in). `rtt` requires (a) a trace clock (arch_trace_now, capability var
# KICKOS_HAVE_TRACE_CLOCK) and (b) the RTT transport (KICKOS_CONSOLE in
# {rtt,both}); either missing is a hard configuration error.
#
# Trace-clock capability + the SESSION arch id. The id MUST match the ArchId enum in
# include/kickos/trace/record.h (the decoder tools/kicktrace.py follows it); a
# static_assert there pins ARCH_RISCV == 5 so a reorder of the enum trips the build:
#   sim     -> clock_now/1000 (us)             have=1,    id=0
#   armv7m  -> DWT CYCCNT fallback (mps2 replaces) have=1, id=1
#   armv6m  -> chip-provided only (no DWT)      have=chip, id=2
#   lx6     -> CCOUNT                           have=1,    id=3 (xtensa)
#   rxv3    -> chip CMTW/timer                  have=1,    id=4 (rx)
#   rv32imac-> rdcycle                          have=1,    id=5 (riscv)
# A chip declares its own capabilities in arch/<family>/chip/<chip>/caps.cmake
# (mirrors the mpu.cmake opt-in); the arch-level ladders below are the fallback for a
# chip that declares nothing. The fragment is included FIRST so a chip value wins, then
# the ladders fill whatever it left unset.
# KICKOS_HAVE_TRACE_CLOCK stays OVERRIDABLE: a board/preset that pre-defines it wins
# over both, because the fragments and the ladder guard their set with NOT DEFINED. It
# must still agree with the declaration. The check below holds an override to the same
# terms as a chip's caps.cmake.
if(KICKOS_CHIP)
  set(_caps_optin "${CMAKE_CURRENT_SOURCE_DIR}/arch/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/caps.cmake")
  if(EXISTS "${_caps_optin}")
    include("${_caps_optin}")
  endif()
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
if(NOT DEFINED KICKOS_HAVE_TRACE_CLOCK)
  set(KICKOS_HAVE_TRACE_CLOCK 0)
  if(KICKOS_ARCH STREQUAL "sim" OR KICKOS_ARCH STREQUAL "armv7m"
     OR KICKOS_ARCH STREQUAL "rxv3" OR KICKOS_ARCH STREQUAL "lx6"
     OR KICKOS_ARCH STREQUAL "rv32imac")
    set(KICKOS_HAVE_TRACE_CLOCK 1) # sim clock/1000; armv7m DWT fallback; rxv3 CMTW1; lx6 CCOUNT; rv32imac rdcycle
  endif()
endif()

# Agreement check, the trace-clock half of the same two-declarations pattern as the MPU
# one below. Kconfig states the capability as HAS_TRACE_CLOCK (an arch default, a chip
# `select` for the armv6m parts that ship their own counter), which reaches here as the
# fragment's KICKOS_TRACE_CLOCK_DECLARED; the build states it again as
# KICKOS_HAVE_TRACE_CLOCK, from the chip's caps.cmake and the arch ladder above. The two
# gate DIFFERENT things: Kconfig offers or hides TELEMETRY_RTT, CMake refuses the sink
# below. So a divergence is a board that resolves an rtt sink with no arch_trace_now, or
# one denied a sink it can host, and neither says a word without this.
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

# Enforced-memory-protection capability. 1 only where a wild access to another
# domain's memory actually FAULTS, so a feature needing a real guard page is gated on
# it and stays green on boards without one. The value comes from the resolved
# configuration and from nowhere else: the posture is a variant, and MEMORY_MODEL_MPU
# depends on HAS_MPU, so a board that cannot enforce cannot select it.

# Agreement check, and the second half of a pair the tree states twice: arch/Kconfig
# declares the capability with `select HAS_MPU`, and the chip declares it again by
# shipping arch/<family>/chip/<chip>/mpu.cmake, which also picks the PMSAv8 backend
# source. Enforcement is real only where the chip's
# linker script carves the KICKOS_HAVE_MPU .appdata window AND the arch ships a real
# arch_mpu_apply; otherwise arch_domain_static_regions reads the WEAK
# __kickos_appdata_* symbols as 0, grants zero regions, and the "MPU" build enforces
# nothing. The sim enforces via host mprotect (no chip, no linker script) and is
# capable by arch. Hardware-proven status is recorded in docs/reference/boards.md,
# not here.
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
  # Advertise the enforcement-capable boards from the SoT (arch/*/chip/*/mpu.cmake +
  # the sim arch) so the hint can never drift from the real gating above.
  kickos_enforcing_mpu_boards(_kickos_mpu_boards)
  message(FATAL_ERROR
    "The configuration resolved the enforcing posture on board '${KICKOS_BOARD}' "
    "(arch=${KICKOS_ARCH} chip=${KICKOS_CHIP}), which ships no mpu.cmake -- so the two "
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

# --- Per-arch caller-provided-stack floor (KICKOS_MIN_STACK_SIZE) -----------
# A syscall runs on the CALLING thread's stack, and the deepest such dispatch is the
# thread-exit teardown (syscall_dispatch -> exit_current -> reschedule -> switch_to ->
# the timer rearm). A caller-provided stack below that depth passes the spawn check
# (syscall_thread.cc) then silently overflows on exit, so the floor MUST cover the
# arch's deepest dispatch. Sized here per arch = the MEASURED deepest dispatch (static
# -fstack-usage of the exit chain, cross-checked against the switch.S context save)
# rounded up to a 128 B multiple with margin. It is an ARCH property, not a knob:
# Kconfig derives it per arch and it is promptless there, so nothing can set it and a
# defconfig asking to is refused by name. The measured figures are in the declaration.
if(NOT DEFINED KICKOS_MIN_STACK_SIZE)
  message(FATAL_ERROR
    "KickOS: no KICKOS_MIN_STACK_SIZE. The generated kickos_config.cmake carries it, "
    "derived per arch in Kconfig, so an empty value here means the fragment was not "
    "read or the arch resolved to none.")
endif()
add_compile_definitions(KICKOS_MIN_STACK_SIZE=${KICKOS_MIN_STACK_SIZE})
message(STATUS "KickOS: caller-stack floor KICKOS_MIN_STACK_SIZE=${KICKOS_MIN_STACK_SIZE} (arch=${KICKOS_ARCH})")

# Service-list provider selection (the default-init service bring-up walk). The
# default init walks the board's service list BEFORE the app main, so a plain app gets
# its board's userspace services with zero app code, starting with the console (a
# KOS_SVC_CONSOLE entry). See system/init/common/service_list_run.cc and
# <kickos/sys/service.h>.
# This is the sole userspace-console bring-up path. KICKOS_SERVICE_LIST names the CMake
# target that DEFINES kickos_board_services; KICKOS_SERVICE_LIST_LIBS is that target plus
# every driver and class leaf its start()s reference. They all back-reference kickos_user,
# so they must join the rescan link group below or the link fails on a
# strongly-connected-component cycle naming the driver lib.
# Adding a provider is ONE place: registering it in system/CMakeLists.txt is enough, because
# the closure below reads the target's own link edges.
# The board states its own in boards/<board>/Kconfig, gated there on the posture where
# a driver handover is an enforcement construct, and the fragment resolved it. Never
# cached, like the console knob.
# The lib closure is DERIVED from the selected target every configure (like the console
# knob), so an explicit A/B override still injects a linkable provider.
# kickos_add_board_provider already took LINK and kickos_add_driver already took CLASS, so
# nothing here restates the provider's own link libraries. The closure is computed further
# down, once add_subdirectory(system) has created the provider and driver targets: it reads
# their link edges, so it cannot run before they exist. A custom provider may pre-set
# KICKOS_SERVICE_LIST_LIBS here with its own dep list.
message(STATUS "KickOS: service list = ${KICKOS_SERVICE_LIST}")

# USB device clock tree, in CHIP init. PLL_USB, clk_usb and the USB block's reset all
# live in blocks the kernel owns for life, so an unprivileged driver cannot bring them up
# and the chip backend must, but only for an image that actually carries a USB driver.
# OFF by default keeps the default image of every RP board byte-identical: a board with
# no USB console must not pay a second PLL, and an unrun clock tree must not become a
# shipping default. Implied by, never implying, a USB service list.
#
# Derived EVERY configure and deliberately NOT cached, like KICKOS_SERVICE_LIST_LIBS
# above: caching it would keep the clock tree on in a build directory whose service list
# was later switched back.
if(NOT DEFINED KICKOS_USB_CONSOLE)
  set(KICKOS_USB_CONSOLE OFF)
  # Unanchored on purpose: any list with a "_usbcdc" stem needs the same clock tree, so a
  # suffixed variant is covered the day it is added. An anchored match would silently leave
  # PLL_USB off and the device would never enumerate, with nothing in the build to say why.
  if(KICKOS_SERVICE_LIST MATCHES "_usbcdc")
    set(KICKOS_USB_CONSOLE ON)
  endif()
endif()
if(KICKOS_USB_CONSOLE)
  add_compile_definitions(KICKOS_USB_CONSOLE=1)
  message(STATUS "KickOS: USB device clock tree = on (chip init brings up PLL_USB)")
endif()

# Board pin-map provider selection (the default-init pin-mux step). The default init
# applies the board pin map BEFORE the console bring-up (see system/init/common/pinmux_run.cc
# + <kickos/sys/pinmap.h>). KICKOS_BOARD_PINMAP names the CMake target that DEFINES
# kickos_board_pinmap; it back-references kickos_user (via kos_pinmux_set at apply time)
# and is read by kickos_default_init, so it must join the rescan link group below.
# Stated per board in Kconfig and resolved by the fragment, like the service list.
# Each provider is a pure data lib with no further deps, so the lib closure is just the
# selected target. A custom provider may pre-set KICKOS_BOARD_PINMAP_LIBS.
if(NOT DEFINED KICKOS_BOARD_PINMAP_LIBS)
  set(KICKOS_BOARD_PINMAP_LIBS "${KICKOS_BOARD_PINMAP}")
endif()
message(STATUS "KickOS: board pin-map = ${KICKOS_BOARD_PINMAP} (libs: ${KICKOS_BOARD_PINMAP_LIBS})")

# RX MPU enforcement wedge-localizer trace (arch/rx/rxv3/arch_rxv3.cc): a raw polled
# SCI6 marker, off by default. Forwarded to the compiler only when set on the command
# line (a plain -D never becomes a compile definition on its own).
if(DEFINED KICKOS_RX_MPU_TRACE)
  add_compile_definitions(KICKOS_RX_MPU_TRACE=${KICKOS_RX_MPU_TRACE})
endif()

# Resolved by the fragment, and never cached, like the console knob.
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

# RTT publish barrier override (docs/reference/telemetry.md G6): lib/ is a leaf and must
# not reach into an arch header, so a weakly-ordered core injects its real fence
# here as a compile definition. Default (unset) keeps the compiler-only barrier in
# rtt.h, which is correct for the in-order Cortex-M targets. Pass the FULL call form,
# e.g. -DKICKOS_RTT_PUBLISH_BARRIER_DEF="__atomic_thread_fence(__ATOMIC_RELEASE)".
if(DEFINED KICKOS_RTT_PUBLISH_BARRIER_DEF)
  add_compile_definitions("KICKOS_RTT_PUBLISH_BARRIER()=${KICKOS_RTT_PUBLISH_BARRIER_DEF}")
endif()

# Local numeric TZ offset (e.g. "+0200"), computed ONCE at configure, exposed to app
# TUs so kickos_app_build_stamp() can align its line with the CMake build stamp (which
# appends the same "%z"). Passed as a BARE token (-DKICKOS_APP_TZ=+0200), not a quoted
# string: embedded escaped quotes double up on a reconfigure of an existing build dir,
# so app.h stringizes the token in C instead. `date` failing (empty) just skips the define.
# app.h stays offset-free then, keeping a plain hosted app build portable.
execute_process(COMMAND date +%z OUTPUT_VARIABLE _apptz OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT _apptz STREQUAL "")
  add_compile_definitions(KICKOS_APP_TZ=${_apptz})
endif()

# Standard install dirs; included before the components so their
# $<INSTALL_INTERFACE:...> can use CMAKE_INSTALL_INCLUDEDIR.
include(GNUInstallDirs)

if(KICKOS_BUILD_TESTS)
  enable_testing()

  # The panic/fault marker set for the sim FAIL_REGULAR_EXPRESSION registrations, read from
  # the ONE file both consumers read: this and tests/lib/gate.sh. Same ERE dialect, so the
  # string crosses verbatim. It must not contain a `;`: FAIL_REGULAR_EXPRESSION is a CMake
  # list of regexes.
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/tests/lib/panic.ere" KICKOS_PANIC_REGEX LIMIT_COUNT 1)
  if(KICKOS_PANIC_REGEX STREQUAL "")
    message(FATAL_ERROR "tests/lib/panic.ere is empty; the sim panic gates would register "
                        "an empty regex and no fault would ever fail a test")
  endif()

  # Rebuild the tree before any test runs, so `ctest` / `ctest --preset` can never
  # execute a stale binary (configure-only + ctest would otherwise silently run the
  # previous build). The build is one CTest fixture (SETUP); every test registered
  # below is auto-wired to require it by the add_test wrapper just after.
  add_test(NAME kickos_build COMMAND "${CMAKE_COMMAND}" --build "${PROJECT_BINARY_DIR}")
  set_tests_properties(kickos_build PROPERTIES FIXTURES_SETUP kickos_build TIMEOUT 900)

  # Make every subsequently-registered test require that fixture, without editing
  # each app's CMakeLists. A macro (not a function) so set_property runs in the
  # caller's directory scope, the only scope where the test it just added exists.
  # kickos_build itself was registered above, before this wrapper, so it never
  # requires itself. Every test here uses the add_test(NAME <n> COMMAND ...) form.
  macro(add_test)
    _add_test(${ARGV})
    if("${ARGV0}" STREQUAL "NAME")
      set_property(TEST "${ARGV1}" APPEND PROPERTY FIXTURES_REQUIRED kickos_build)
    else()
      set_property(TEST "${ARGV0}" APPEND PROPERTY FIXTURES_REQUIRED kickos_build)
    endif()
  endmacro()

  # Doc-rot gate. It reads the SOURCE tree through `git ls-files` and never opens the
  # build directory, so it is registered on every board rather than behind one: a gate
  # that runs on a single posture is a gate that runs when that posture happens to be
  # built. Registered with _add_test so it does not require the build fixture, which it
  # has nothing to do with; WORKING_DIRECTORY because the script refuses to run anywhere
  # but the repo root.
  _add_test(NAME doc_names
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_doc_names.sh"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(doc_names PROPERTIES TIMEOUT 120)

  # Kconfig generator gate. A source-tree host check like doc_names, so it registers
  # on every board and requires no build. Unconditional: every board is configured from
  # Kconfig, so kickos_kconfig_generate above already FATALed on a box without the
  # interpreter and this cannot be reached without one.
  kickos_find_kconfig(_kconfig_py _kconfig_found)
  _add_test(NAME kconfig_gen
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_kconfig_gen.sh"
                    "${_kconfig_py}" "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(kconfig_gen PROPERTIES TIMEOUT 120)
endif()

# Components are added as they land; guarded so every step boundary configures.
foreach(_dir lib arch kernel user system)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_dir}/CMakeLists.txt")
    add_subdirectory(${_dir})
  endif()
endforeach()

# Host-only telemetry CI gate (golden-vector round-trip): builds a host tool, so
# it registers on the sim build only. Config-independent (does not need telemetry
# enabled, since it exercises the pure encoders + the decoder directly).
if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/telemetry/CMakeLists.txt")
  add_subdirectory(tests/telemetry)
endif()

# Host-only SlotPool allocation-policy gate. Host-only of necessity, not convenience:
# no SlotPool handle ever crosses the user/kernel boundary, so no on-target selftest
# arm can observe which slot an object lands in. slotpool.h is standalone (stdint.h
# only), so the gate instantiates the real template.
if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/slotpool/CMakeLists.txt")
  add_subdirectory(tests/slotpool)
endif()

# Host-only next-event-deadline gate. Host-only of necessity too: the deadline the
# arch backends dedup on never crosses the arch seam, so an on-target arm can only
# time a wake and race the host scheduler for the answer. Compiles the real
# kernel/time/time.cc against a fake clock instead.
if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/ktime/CMakeLists.txt")
  add_subdirectory(tests/ktime)
endif()

# Host-only capability-table storage + free-list policy gate. Host-only of necessity: the
# chunk geometry is chosen by an #if, so one build compiles one geometry, and neither a
# directory mapping nor a free-list length is observable from userspace.
if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/captable/CMakeLists.txt")
  add_subdirectory(tests/captable)
endif()

# Host-only CAP_REPLY packing gate. Host-only of necessity: the case it exists for is a thread
# index above 255, which needs more slots than any board in the fleet configures, and no cap
# entry is observable from userspace.
if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/capreply/CMakeLists.txt")
  add_subdirectory(tests/capreply)
endif()

# Aggregate usage-requirement target: "link this and you get the whole OS".
# Lets a consumer write a plain add_executable(app ...) + target_link_libraries:
# the link recipe (component group, entry glue, MCU link flags) is baked into
# this exported interface target, so no source tree is needed to link an image.
# kickos_add_application() is then optional sugar (and, on MCU, the image step).
# The arch<->kernel callback cycle is resolved as one rescanned archive group.
# The userspace init service: the CMake target that defines kickos_init_entry (the
# symbol the kernel root thread calls after init). Default = the built-in
# kickos_system service (kickos_default_init, a thin passthrough to the app's main);
# a power user names their own target. Defined before kickos_core so the LINK_GROUP
# below can pull the provider archive.
set(KICKOS_INIT_PROVIDER "kickos_default_init" CACHE STRING
  "CMake target providing kickos_init_entry (the userspace init service). Default = the built-in kickos_system service; a power user names their own target.")

if(TARGET kickos_kernel)
  # Posture-neutral requirements shared by both leaves. Consumers link a leaf
  # (kickos / kickos_cxx), never kickos_core directly; core carries everything
  # except the freestanding-vs-full-C++ compile clamp and the -nostdlib++ link.
  add_library(kickos_core INTERFACE)
  # On MCU the chip backend (arch_init/console + reset vector) joins the rescan
  # group; the arch<->kernel<->chip callback cycles resolve as one archive group.
  # KICKOS_INIT_PROVIDER sits right after kickos_kernel: the kernel references
  # kickos_init_entry across the rescan boundary, and the provider archive being
  # in-group resolves it. No -Wl,-u is needed; gc-sections keeps it via the live
  # reset->kmain->root_entry chain.
  # KICKOS_SERVICE_LIST_LIBS follows the provider: the list TU references its drivers'
  # start()s, and those back-reference kickos_user + a class leaf, so the whole closure
  # must be in-group to rescan-resolve (one item on the none default, three on a
  # migrated board).
  # Derived HERE and not at the knob: the walk reads the provider's and drivers' own link
  # edges, which exist only after add_subdirectory(system) above.
  if(NOT DEFINED KICKOS_SERVICE_LIST_LIBS)
    kickos_service_libs_closure("${KICKOS_SERVICE_LIST}" KICKOS_SERVICE_LIST_LIBS)
    message(STATUS "KickOS: service-list closure = ${KICKOS_SERVICE_LIST_LIBS}")
  endif()
  # The archive list, named once; the group is built per posture leaf below, because
  # the leaves differ in which toolchain runtime joins it and CMake forbids one
  # target's closure from carrying a library in two groups.
  if(KICKOS_CHIP AND TARGET kickos_chip_${KICKOS_CHIP})
    set(_kickos_group "kickos_user,kickos_kernel,${KICKOS_INIT_PROVIDER},${KICKOS_SERVICE_LIST_LIBS},${KICKOS_BOARD_PINMAP_LIBS},kickos_chip_${KICKOS_CHIP},kickos_arch_${KICKOS_ARCH},kickos_lib")
  else()
    set(_kickos_group "kickos_user,kickos_kernel,${KICKOS_INIT_PROVIDER},${KICKOS_SERVICE_LIST_LIBS},${KICKOS_BOARD_PINMAP_LIBS},kickos_arch_${KICKOS_ARCH},kickos_lib")
  endif()
  # kickos_system is INTERFACE (contributes no archive), so it must NOT sit in the
  # RESCAN group; link it separately. This is what propagates the relocated system
  # headers (init.h, errno.h, cap_index.h) to every app.
  target_link_libraries(kickos_core INTERFACE kickos_system)
  # OS-agnostic app entry (architecture.md 8j): the app writes a plain `int main`,
  # renamed to the kernel-called symbol; app.h is force-included so the renamed C++
  # main gets C linkage (unmangled).
  target_compile_definitions(kickos_core INTERFACE main=kickos_app_main)
  # No provisioning re-stamp: a -D knob is a Kconfig request, so it lands in the GENERATED
  # board_config.h that both the libs and the shipped package carry. No value the libs were
  # built with is absent from that header.
  # KICKOS_TELEMETRY gates fields in struct Kernel (instance.h) and arch_context
  # (context.h); KICKOS_HAVE_MPU branches KOS_STACK_DEFINE (kos.h). An OOT TU that
  # includes a kernel-internal header must see the same posture as the prebuilt libs
  # (ODR), so carry them as usage requirements.
  # KICKOS_ARCH_SIM rides with them: a sim package ships no chip_limits.h (there is no
  # chip), so config/board.h's #error is what an out-of-tree consumer hits without it.
  set(_kos_iface_sim 0)
  if(KICKOS_ARCH STREQUAL "sim")
    set(_kos_iface_sim 1)
  endif()
  target_compile_definitions(kickos_core INTERFACE
    KICKOS_TELEMETRY=${_telem} KICKOS_TELEMETRY_RTT=${_telem}
    KICKOS_TRACE_ARCH=${KICKOS_TRACE_ARCH}
    KICKOS_HAVE_MPU=${KICKOS_HAVE_MPU}
    KICKOS_ARCH_SIM=${_kos_iface_sim}
    KICKOS_DEBUG=$<BOOL:${KICKOS_DEBUG}>)
  target_compile_options(kickos_core INTERFACE -include kickos/app.h)
  # Force-include the seam header so an app-TU-defined kickos_init_entry gets C
  # linkage (it cannot silently mangle and fall back to the default provider). SHELL:
  # keeps the second -include from being de-duplicated against the app.h one above.
  target_compile_options(kickos_core INTERFACE "SHELL:-include kickos/sys/init.h")
  if(KICKOS_ARCH STREQUAL "sim")
    find_package(Threads REQUIRED)
    target_link_libraries(kickos_core INTERFACE Threads::Threads rt)
  endif()
  # MCU bare-metal link recipe (posture-neutral part): our reset object instead of
  # crt0, the chip linker script, dead-code elimination, and force-refs to objects
  # nothing references (else --gc-sections drops them). -nostdlib++ is NOT here; it
  # belongs to the freestanding leaf only.
  if(NOT (KICKOS_ARCH STREQUAL "sim") AND KICKOS_LINKER_SCRIPT)
    target_link_options(kickos_core INTERFACE
      -nostartfiles
      -Wl,--gc-sections
      -Wl,--no-warn-rwx-segments
      # ARM's vector table (g_isr_vector) is a data object nothing references. RX
      # pulls its vector tables via ENTRY(_start) + KEEP(), so it needs no force-ref
      # (and has no g_isr_vector; forcing it would leave a dangling undefined).
      "$<$<STREQUAL:${KICKOS_ARCH_FAMILY},arm>:-Wl,-u,g_isr_vector>"
      # Force our libc syscall stubs in (libc's raise->_exit). The RX psABI prefixes
      # C identifiers with a leading underscore, so C `_exit` is asm `__exit`; forcing
      # `_exit` there would pull nothing and drop newlib_stubs.o (RX sbrk/__dso_handle).
      "$<$<STREQUAL:${KICKOS_ARCH_FAMILY},rx>:-Wl,-u,__exit>"
      "$<$<NOT:$<STREQUAL:${KICKOS_ARCH_FAMILY},rx>>:-Wl,-u,_exit>"
      # A chip with a checksummed flash second stage (RP2040 boot2) must force its
      # .boot2 blob object in too; nothing references it.
      $<$<BOOL:${KICKOS_BOOT2_FORCE_SYMBOL}>:-Wl,-u,${KICKOS_BOOT2_FORCE_SYMBOL}>
      # In-tree: the source linker script. Installed/exported: the shipped copy
      # under <prefix>/<libdir>/kickos so an out-of-tree consumer links without
      # the KickOS source tree present.
      "$<BUILD_INTERFACE:SHELL:-T ${KICKOS_LINKER_SCRIPT}>"
      "$<INSTALL_INTERFACE:SHELL:-T $<INSTALL_PREFIX>/${CMAKE_INSTALL_LIBDIR}/kickos/${KICKOS_CHIP}.ld>")
    # ...and the same script as a real link DEPENDENCY: -T is an opaque driver
    # option, so without this an edited .ld does not relink and a stale image gets
    # flashed. INTERFACE_LINK_DEPENDS rides the export, so consumers get the edge
    # too. Paths mirror the -T above; the dependency must name the file actually
    # linked.
    set_property(TARGET kickos_core APPEND PROPERTY INTERFACE_LINK_DEPENDS
      "$<BUILD_INTERFACE:${KICKOS_LINKER_SCRIPT}>"
      "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_LIBDIR}/kickos/${KICKOS_CHIP}.ld>")
  endif()

  # Two posture leaves over the neutral core. A pure-C / freestanding app links
  # `kickos`; a full-C++ (exceptions/STL/RTTI) app links `kickos_cxx`. A misspelled
  # leaf name is a hard CMake error, not a silent freestanding downgrade.
  add_library(kickos INTERFACE)
  target_link_libraries(kickos INTERFACE kickos_core)
  add_library(kickos_cxx INTERFACE)
  target_link_libraries(kickos_cxx INTERFACE kickos_core)
  # The rescan group, per leaf. On MCU the toolchain runtime must join it: newlib's
  # `_sbrk_r` references `_sbrk` (provided on demand by libkickos_user.a), and the g++
  # driver appends its libraries after everything CMake emits, so outside the group
  # the linker meets `_sbrk_r` too late to go back for the member. Naming the runtime here
  # only fixes where it is scanned. The freestanding leaf must NOT name libstdc++
  # (it links -nostdlib++; a stray operator new stays a link error). Sim links host glibc.
  if(KICKOS_ARCH STREQUAL "sim")
    target_link_libraries(kickos INTERFACE "$<LINK_GROUP:RESCAN,${_kickos_group}>")
    target_link_libraries(kickos_cxx INTERFACE "$<LINK_GROUP:RESCAN,${_kickos_group}>")
  else()
    target_link_libraries(kickos INTERFACE "$<LINK_GROUP:RESCAN,${_kickos_group},c>")
    target_link_libraries(kickos_cxx INTERFACE
      "$<LINK_GROUP:RESCAN,${_kickos_group},stdc++,supc++,c>")
  endif()
  # Carry __KickOS__ to downstream consumers (both leaves link kickos_core, which is
  # in the export set, so find_package consumers get it too). In-tree TUs already have
  # it from the add_compile_definitions above.
  target_compile_definitions(kickos_core INTERFACE __KickOS__=1)
  if(NOT (KICKOS_ARCH STREQUAL "sim"))
    # Freestanding (default): no EH/RTTI in the app's C++ TUs, and -nostdlib++ drops
    # libsupc++/libstdc++, so a stray operator new / EH table fails the link loud.
    target_compile_options(kickos INTERFACE
      "$<$<COMPILE_LANGUAGE:CXX>:${KICKOS_FREESTANDING_CXX_FLAGS}>")
    target_link_options(kickos INTERFACE -nostdlib++)
    # Full C++: -fexceptions/-frtti, and omitting -nostdlib++ lets the g++ driver
    # auto-link the toolchain libstdc++/libsupc++ (EH, RTTI, new/delete) over newlib.
    # kickos_cxx never sees the freestanding clamp, so there is no -fno-exceptions to
    # override. The two flag sets never collide on one command line.
    target_compile_options(kickos_cxx INTERFACE
      "$<$<COMPILE_LANGUAGE:CXX>:-fexceptions;-frtti>")
    # A second rescan group, one layer out: the driver-appended libstdc++'s operator
    # new must be able to reach down to `_sbrk`, so the chain needs a group holding
    # all three providers. Listed after kickos_core so the core group resolves first.
    target_link_libraries(kickos_cxx INTERFACE
      "$<LINK_GROUP:RESCAN,stdc++,supc++,c,kickos_user>")

    # A strong __gnu_cxx::__verbose_terminate_handler so the linker never extracts
    # libstdc++'s vterminate.o (~65K of demangle + float dtoa). Must be a real object
    # unconditionally on the link line (not an archive member), so it wins over the
    # driver-appended libstdc++. This TU itself is compiled -fexceptions -frtti;
    # kickos_apply_freestanding's -fno-exceptions would break its compile, so it is
    # deliberately NOT applied here. INTERFACE flags on kickos_cxx do not reach this
    # object lib's own compile, hence the direct flags. The per-chip -mcpu baseline
    # comes from the toolchain file globally.
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

# Apps link a full image: the sim links against host libc; an MCU board links
# once its chip layer (startup + linker script) is actually built. A board whose
# chip is named but not yet brought up (no arch/arm/chip/<chip> dir -> no chip
# target) builds the libraries only.
if(KICKOS_BUILD_APPS
   AND ((KICKOS_ARCH STREQUAL "sim") OR (KICKOS_CHIP AND TARGET kickos_chip_${KICKOS_CHIP}))
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/user/apps/CMakeLists.txt")
  add_subdirectory(user/apps)
endif()

# Validate the selected init provider LATE: a provider defined in user/apps or a
# superproject is not a target yet at the kickos_core site above, so checking there
# would false-trip. Guarded on kickos_core so a libraries-only configure (a named
# but unbrought-up chip) does not demand a provider it never links.
if(TARGET kickos_core)
  if(NOT TARGET ${KICKOS_INIT_PROVIDER})
    message(FATAL_ERROR "KICKOS_INIT_PROVIDER='${KICKOS_INIT_PROVIDER}' is not a CMake target. Name a target that defines kickos_init_entry, or unset to use the default kickos_default_init.")
  endif()
  # A non-default provider that is not in the export set will not survive
  # install/export: find_package(KickOS) consumers would link a recipe naming a
  # target they never got. An in-tree-only provider is fine for a source-tree build.
  if(NOT KICKOS_INIT_PROVIDER STREQUAL "kickos_default_init")
    message(STATUS "KickOS: init provider = ${KICKOS_INIT_PROVIDER} (non-default). "
      "For an installed/exported package it must also be install(EXPORT KickOSTargets).")
  endif()
  # Same late validation for the service-list provider (defined under system/init/<board>/, so
  # it is not a target at the kickos_core site above).
  if(NOT TARGET ${KICKOS_SERVICE_LIST})
    message(FATAL_ERROR "KICKOS_SERVICE_LIST='${KICKOS_SERVICE_LIST}' is not a CMake target. Name a target that defines kickos_board_services, or use the default kickos_services_none (count = 0).")
  endif()
  # And for the pin map. Without this a misspelled target name reaches the LINK_GROUP as a
  # bare -l, so the board silently keeps kickos_pinmap_none and boots with unmuxed pins.
  if(NOT TARGET ${KICKOS_BOARD_PINMAP})
    message(FATAL_ERROR "KICKOS_BOARD_PINMAP='${KICKOS_BOARD_PINMAP}' is not a CMake target. Name a target that defines kos_board_pinmap, or use the default kickos_pinmap_none.")
  endif()
endif()

# Must stay AFTER add_subdirectory(user/apps): that is the last place an app can declare its
# peak. The structural constants the sum needs come from cmake/cap_geometry.cmake, included
# at the top of this file, and reach C through the generated header written just below;
# nothing preprocesses a kernel header to read them.
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

# --- Install / export: the dependency-inversion package --------------------
# Consumers do find_package(KickOS) + kickos_add_application() out of tree.
include(CMakePackageConfigHelpers)

set(KICKOS_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/KickOS")

# CeCILL-C is file-level copyleft: ship the license with the package.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/${PROJECT_NAME}")

# Public headers (each component ships its kickos/ subtree; they merge). The arch
# backend's own include dir (which holds the per-arch context.h) lives at a path
# the arch subdir reports (arch/sim/include vs arch/arm/armv7m/include), so use
# that, not a arch/${ARCH}/include guess (which is wrong for the ARM tree).
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

# Ship the GENERATED provisioning header, which is the one the prebuilt libs were
# compiled against and now the only one there is. It replaces the old -D provisioning
# export, and a consumer whose TU includes a kernel-internal header reads exactly the
# values this build used.
install(FILES "${PROJECT_BINARY_DIR}/generated/include/kickos/board_config.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/kickos")

# And the chip's constants, which config/board.h now requires: a consumer whose TU
# includes a kernel config header has no KICKOS_MAX_IRQ without it and gets an #error
# rather than a wrong table size.
if(KICKOS_CHIP_INCLUDE_DIR)
  install(DIRECTORY "${KICKOS_CHIP_INCLUDE_DIR}/"
          DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
endif()

# And the board's wiring constants, on the same footing: a consumer TU that pulls in the
# chip backend's headers reads the crystal and LED of the board it was built for.
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

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/KickOSConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)

# The build helper is shipped verbatim so kickos_add_application() is available; it
# include()s cap_table.cmake list-dir-relative, so that must land beside it.
install(FILES
          "${CMAKE_CURRENT_BINARY_DIR}/KickOSConfig.cmake"
          "${CMAKE_CURRENT_BINARY_DIR}/KickOSConfigVersion.cmake"
          "${CMAKE_CURRENT_SOURCE_DIR}/cmake/kickos.cmake"
          "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cap_table.cmake"
        DESTINATION "${KICKOS_CMAKE_DIR}")

# Ship the cross toolchain file for MCU packages: an out-of-tree consumer
# configures with -DCMAKE_TOOLCHAIN_FILE=<prefix>/.../toolchain-<family>*.cmake and
# gets the CPU/ABI/ISA baseline + the LINK_GROUP RESCAN feature declaration. Route
# by family so a non-ARM package ships its own toolchain. Ship this board's
# descriptor beside it (as board.cmake) so the toolchain finds the arch/chip/-mcpu
# baseline without an in-tree boards/ tree.
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
  # The descriptor ships with the chip's CPU baseline BAKED IN, because a package has no
  # arch/ tree for the toolchain file to read a cpu.cmake out of. A package is one board,
  # so a resolved value is correct there and the layout stays as flat as it already is.
  # The values are the ones this build resolved, so the package cannot disagree with the
  # libraries it ships.
  # Only the knobs the descriptor LEAVES to the chip are appended. Restating one the
  # board already sets would ship a file that states it twice, and a reader has no way
  # to tell which line is the answer.
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
