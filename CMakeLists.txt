# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

cmake_minimum_required(VERSION 3.24)
# Version scheme: 0.<milestone>.<submilestone> (drives KICKOS_VERSION -> the boot banner).
project(KickOS VERSION 0.4.5 LANGUAGES C CXX ASM)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(cmake/kickos.cmake)

# Consumer-detection signals, styled like the built-in platform vars (LINUX/WIN32)
# and the reserved platform macros (__linux__/__APPLE__). KickOS IS the platform.
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
# shared by EVERY layer -- arch, lib, kernel, userspace. Kept in a repo-root
# include/ (below all components) so even the lowest layer (arch) uses them
# without reaching up into a peer component's dir.
include_directories("${PROJECT_SOURCE_DIR}/include")

# --- Board provisioning (board_config.h) -----------------------------------
# Per-board facts -- KICKOS_MAX_IRQ, the thread/stack pool sizes, and (for a chip
# shared by >1 board) HW like the LED pin + HSE crystal -- live in board_config.h.
# It is pulled in by the kernel config headers (config/{board,system}.h), the chip
# startup.S (so MAX_IRQ and the vector-table .rept are ONE fact) and, on a
# multi-board chip, the chip clock/LED code. The board/chip split: a board with
# per-board needs owns boards/<board>/include; a single-board chip keeps its
# default under the chip dir. Prefer the board dir, fall back to the chip dir.
# The sim has neither: config/{board,system}.h fall through to their #ifndef
# defaults, which are the sim values.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/boards/${KICKOS_BOARD}/include")
  set(KICKOS_BOARD_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/${KICKOS_BOARD}/include")
elseif(KICKOS_CHIP)
  set(KICKOS_BOARD_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/arch/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/include")
endif()
if(KICKOS_BOARD_INCLUDE_DIR)
  include_directories("${KICKOS_BOARD_INCLUDE_DIR}")
endif()

# Transient override: a knob set on the CMake command line (e.g.
# -DKICKOS_MAX_THREADS=4) is forwarded as a compile definition and wins over the
# header's #ifndef. For a PERSISTENT change edit the board's board_config.h --
# that is also what out-of-tree consumers read (the shipped header).
#
# Any override is collected into KICKOS_PROVISION_OVERRIDES and ALSO stamped onto
# the exported `kickos` INTERFACE target below. Without that, an out-of-tree
# consumer whose TUs include a kernel-internal header (e.g. instance.h) would
# compile against the shipped board_config.h defaults -- which the -D just changed
# for the prebuilt .a libs -- silently disagreeing on the Kernel layout (ODR). The
# normal (no-override) build stamps nothing extra: the header stays the sole truth.
set(KICKOS_PROVISION_OVERRIDES "")
foreach(_knob KICKOS_MAX_IRQ KICKOS_MAX_THREADS KICKOS_USER_STACK_SIZE
              KICKOS_IDLE_STACK_SIZE KICKOS_ROOT_STACK_SIZE
              KICKOS_MAX_SEMAPHORES KICKOS_MAX_HANDLES KICKOS_MAX_IRQ_HANDLES
              KICKOS_RTT_CH1_SIZE)
  if(DEFINED ${_knob})
    add_compile_definitions(${_knob}=${${_knob}})
    list(APPEND KICKOS_PROVISION_OVERRIDES "${_knob}=${${_knob}}")
  endif()
endforeach()

# --- Config knobs (architecture.md) ----------------------------------------
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

# HOLDING MEASURE, pending the maintainer decision tracked as N16 in TODO.md.
#
# f302nucleo and bluepill-c8 have 64 KiB of flash, and the fleet-uniform selftest image
# no longer fits it at the fleet Debug default -- which is `-g` with NO -O at all. This
# batch's kernel hardening costs 184 bytes on f302nucleo against the 104 bytes of
# headroom it had, so the board stopped linking on kernel code, not on a new test.
#
# The measurement that decides what to do about it: the SAME image is 48,848 bytes at
# -Os and 65,720 at the default. The ceiling is 74% real code and 26% unoptimised
# codegen -- so the choice N16 poses (shed coverage, or accept these boards as
# build-only) has a third answer nobody had costed. Taking -Os here sheds NO coverage:
# every test the suite has still builds, and 16.7 KiB of headroom comes back.
#
# What it costs, stated plainly: the -st image's kernel is no longer codegen-identical
# to the same board's non-st build, which the selftest -Os block (see
# user/apps/common/selftest/CMakeLists.txt) deliberately preserved when it optimised the
# app's own TUs only. Both boards are build-only for the suite -- no bench unit, no QEMU
# model -- so what they provide is a link check, and this keeps them providing one. -g is
# kept, so they stay debuggable. Scoped to these two boards and reverted by deleting this
# block; widening it to the fleet is the maintainer's call, not a side effect of a
# hardening batch.
if(KICKOS_ENABLE_SELFTEST AND (KICKOS_BOARD STREQUAL "f302nucleo"
                               OR KICKOS_BOARD STREQUAL "bluepill-c8"))
  add_compile_options(-Os)
  message(STATUS "KickOS: -Os across the ${KICKOS_BOARD} selftest tree (64 KiB flash; see N16)")
endif()

# Internal-consistency guards that are too expensive (or too noisy) to carry in a
# shipped image: today the intrusive-list membership checks in <kickos/list.h>. OFF by
# default, so a board image is byte-identical to what it was. The sim preset turns it
# ON -- a host build has no flash budget, and a guard nothing builds is a guard that
# rots. Always defined (0 or 1), never merely undefined, so `#if KICKOS_DEBUG` is
# -Wundef-clean; carried to consumers below for the same ODR reason as KICKOS_HAVE_MPU,
# since it changes the body of an inline function in a shipped header.
option(KICKOS_DEBUG "Build the internal-consistency guards (list membership, ...)" OFF)
if(KICKOS_DEBUG)
  add_compile_definitions(KICKOS_DEBUG=1)
else()
  add_compile_definitions(KICKOS_DEBUG=0)
endif()

# --- Console backend selection (docs/console-design.md) --------------------
# KICKOS_CONSOLE = chip | rtt | both | none. `chip` = the board's native transport
# (UART / semihosting / USIC); `rtt` = SEGGER RTT (needs a debug probe); resolved
# to compile-time KICKOS_CONSOLE_CHIP/RTT for the kernel console core. Default is
# `chip` (no probe assumed); the XMC4800 defaults `both` (its native transport is
# USIC and it has an onboard J-Link -> visible over ttyACM0 AND JLinkRTTClient).
if(NOT DEFINED KICKOS_CONSOLE)
  if(KICKOS_BOARD STREQUAL "xmc4800-relax")
    set(KICKOS_CONSOLE "both")
  else()
    set(KICKOS_CONSOLE "chip")
  endif()
endif()
set(KICKOS_CONSOLE "${KICKOS_CONSOLE}" CACHE STRING "Console backend: chip|rtt|both|none")
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

# --- Telemetry backend selection (docs/reference/telemetry.md) -----------------
# KICKOS_TELEMETRY = off | rtt. Kernel-observability records (context switch,
# syscall, IRQ) emitted to RTT channel 1. Per-board default off (a benchmarking
# opt-in). `rtt` requires (a) a trace clock -- arch_trace_now, capability var
# KICKOS_HAVE_TRACE_CLOCK -- and (b) the RTT transport (KICKOS_CONSOLE in
# {rtt,both}); either missing is a hard configuration error.
#
# Trace-clock capability + the SESSION arch id. The id MUST match the ArchId enum in
# include/kickos/trace/record.h (the decoder tools/kicktrace.py follows it); a
# static_assert there pins ARCH_RISCV == 5 so a reorder of the enum trips the build:
#   sim     -> clock_now/1000 (us)             have=1,    id=0
#   armv7m  -> weak DWT CYCCNT (mps2 overrides) have=1,    id=1
#   armv6m  -> chip-provided only (no DWT)      have=chip, id=2
#   lx6     -> CCOUNT                           have=1,    id=3 (xtensa)
#   rxv3    -> chip CMTW/timer                  have=1,    id=4 (rx)
#   rv32imac-> rdcycle                          have=1,    id=5 (riscv)
# A chip declares its own capabilities in arch/<family>/chip/<chip>/caps.cmake
# (mirrors the mpu.cmake opt-in); the arch-level ladders below are the fallback for a
# chip that declares nothing. The fragment is included FIRST so a chip value wins,
# then the ladders fill whatever it left unset -- this is what replaces the hardcoded
# armv6m chip names (rp2040, nrf51) the trace-clock ladder used to carry.
# KICKOS_HAVE_TRACE_CLOCK stays OVERRIDABLE: a board/preset that pre-defines it wins
# over both, because the fragments and the ladder guard their set with NOT DEFINED.
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
    set(KICKOS_HAVE_TRACE_CLOCK 1) # sim clock/1000; armv7m weak DWT; rxv3 CMTW1; lx6 CCOUNT; rv32imac rdcycle
  endif()
endif()

# Enforced-memory-protection capability. 1 only where a wild access to another
# domain's memory actually FAULTS -- so a test/feature that needs a real guard page
# (selftest mpu_privileged_guard) is gated on it and stays green on boards without
# it. Today only the sim (mprotect) enforces; M2 sets it per chip as SYSMPU/PMSA/PMP
# land, and the ESP32 (no MPU) deliberately stays 0. OVERRIDABLE like the trace
# clock above -- a board/chip/preset may set it directly.
if(NOT DEFINED KICKOS_HAVE_MPU)
  set(KICKOS_HAVE_MPU 0)
  if(KICKOS_ARCH STREQUAL "sim")
    set(KICKOS_HAVE_MPU 1)
  endif()
endif()

# Fail-loud floor: KICKOS_HAVE_MPU=1 must never silently do nothing. Enforcement is
# real only where the chip's linker script carves the KICKOS_HAVE_MPU .appdata window
# AND the arch ships a real arch_mpu_apply -- otherwise arch_domain_static_regions
# reads the WEAK __kickos_appdata_* symbols as 0, grants zero regions, and the "MPU"
# build enforces nothing. Capability is an EXPLICIT per-port opt-in, not inferred from
# the linker plumbing: a chip ships arch/<family>/chip/<chip>/mpu.cmake
# (KICKOS_CHIP_ENFORCES_MPU ON); a chip that merely has the .appdata block but ships
# no such file is rejected here. The sim enforces via host mprotect (no chip,
# no linker script) and is capable by arch. Which ports are proven on hardware is
# NOT recorded here -- see docs/reference/boards.md.
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
    "KICKOS_HAVE_MPU=1 requested on board '${KICKOS_BOARD}' "
    "(arch=${KICKOS_ARCH} chip=${KICKOS_CHIP}), which has no MPU enforcement block -- "
    "enforcement would be a silent no-op (arch_domain_static_regions grants zero "
    "regions because the linker script carves no KICKOS_HAVE_MPU .appdata window). "
    "Enforcement-capable boards today: ${_kickos_mpu_boards}. Either build '${KICKOS_BOARD}' "
    "without KICKOS_HAVE_MPU, or bring the port up: add a KICKOS_HAVE_MPU .appdata "
    "window to its linker script and drop a mpu.cmake (KICKOS_CHIP_ENFORCES_MPU ON) "
    "beside it once enforcement is validated on hardware.")
endif()

add_compile_definitions(KICKOS_HAVE_MPU=${KICKOS_HAVE_MPU})

# --- Root privilege posture (KICKOS_ROOT_PRIVILEGED) ------------------------
# Whether kmain creates the root thread privileged. Default ON, which is the
# posture every board shipped before this knob existed. OFF creates root
# unprivileged, holding a CAP_AUTHORITY with every AUTH_* bit instead
# (kernel/init/kmain.cc) -- see docs/design-unprivileged-root.md stage 2, and
# docs/reference/boards.md for which boards are validated in which posture.
#
# A BUILD knob and NOT a weak symbol, deliberately. The rejected alternative was a
# weak `bool kickos_root_privileged()` a board overrides strongly, which would let an
# out-of-tree link silently opt out of the privilege boundary with no trace in the
# board's build. Here the posture is a compile definition the whole tree sees, it is
# printed at configure time, and it is carried to consumers as a usage requirement
# below -- so opting out is visible in the build, not discoverable only by reading
# the link map. Always defined 0 or 1 (never left undefined) so `#if` is -Wundef-clean.
option(KICKOS_ROOT_PRIVILEGED "Create the root thread privileged (OFF: unprivileged, holding an authority cap)" ON)
if(KICKOS_ROOT_PRIVILEGED)
  set(_root_priv 1)
else()
  set(_root_priv 0)
endif()
add_compile_definitions(KICKOS_ROOT_PRIVILEGED=${_root_priv})
if(NOT KICKOS_ROOT_PRIVILEGED)
  message(STATUS "KickOS: root thread is UNPRIVILEGED (holds CAP_AUTHORITY); enforcement=${KICKOS_HAVE_MPU}")
endif()

# --- Per-arch caller-provided-stack floor (KICKOS_MIN_STACK_SIZE) -----------
# A syscall runs on the CALLING thread's stack, and the deepest such dispatch is the
# thread-exit teardown (syscall_dispatch -> exit_current -> reschedule -> switch_to ->
# the timer rearm). A caller-provided stack below that depth passes the spawn check
# (syscall_thread.cc) then silently overflows on exit, so the floor MUST cover the
# arch's deepest dispatch. Sized here per arch = the MEASURED deepest dispatch (static
# -fstack-usage of the exit chain, cross-checked against the switch.S context save)
# rounded up to a 128 B multiple with margin; forwarded as a -D. config/system.h keeps
# a conservative MAX-across-arches #ifndef default, so a build that bypasses this ladder
# is safe-but-wasteful, never silently-too-low. A board / command-line -D wins (guarded
# on NOT DEFINED), so a board may still tighten or widen it. Like KICKOS_HAVE_MPU above,
# this is an arch property, not a board-provisioning knob.
if(NOT DEFINED KICKOS_MIN_STACK_SIZE)
  if(KICKOS_ARCH STREQUAL "armv6m")
    set(KICKOS_MIN_STACK_SIZE 512)   # measured ~392 B (no FPU)
  elseif(KICKOS_ARCH STREQUAL "armv7m")
    set(KICKOS_MIN_STACK_SIZE 640)   # measured ~424 B (+ headroom for a live FP frame)
  elseif(KICKOS_ARCH STREQUAL "rxv3")
    set(KICKOS_MIN_STACK_SIZE 640)   # measured ~428 B (+ headroom for a DPFPU frame)
  elseif(KICKOS_ARCH STREQUAL "rv32imac")
    set(KICKOS_MIN_STACK_SIZE 896)   # measured ~624 B (runtime watermark)
  elseif(KICKOS_ARCH STREQUAL "lx6")
    set(KICKOS_MIN_STACK_SIZE 896)   # measured ~624 B (windowed-ABI spill)
  elseif(KICKOS_ARCH STREQUAL "sim")
    set(KICKOS_MIN_STACK_SIZE 1024)  # measured ~896 B host; unconstrained (host stack)
  endif()
endif()
if(DEFINED KICKOS_MIN_STACK_SIZE)
  add_compile_definitions(KICKOS_MIN_STACK_SIZE=${KICKOS_MIN_STACK_SIZE})
endif()
message(STATUS "KickOS: caller-stack floor KICKOS_MIN_STACK_SIZE=${KICKOS_MIN_STACK_SIZE} (arch=${KICKOS_ARCH})")

# Service-list provider selection (the default-init service bring-up walk). The
# default init walks the board's service list BEFORE the app main, so a plain app gets
# its board's userspace services -- starting with the console, a KOS_SVC_CONSOLE entry --
# with zero app code (see system/init/service_list_run.cc + <kickos/sys/service.h>).
# This is the sole userspace-console bring-up path. KICKOS_SERVICE_LIST names the CMake
# target that DEFINES kickos_board_services; KICKOS_SERVICE_LIST_LIBS is that target plus
# every driver + class leaf its start()s reference (all back-reference kickos_user, so
# they must join the rescan link group below). Default is kickos_services_none
# (count = 0: no-op). Resolved per-board AFTER KICKOS_HAVE_MPU is known (a driver
# handover is an enforcement construct); must sit after the MPU block for that reason.
if(NOT DEFINED KICKOS_SERVICE_LIST)
  if(KICKOS_BOARD STREQUAL "xmc4800-relax" AND KICKOS_HAVE_MPU)
    if(KICKOS_ROOT_PRIVILEGED)
      set(KICKOS_SERVICE_LIST "kickos_services_xmc4800relax")
    else()
      # Console only. The SPI entry's bring-up body configures USIC kernel clock, baud
      # and protocol through PV-write-only registers OUTSIDE any grantable window, so an
      # unprivileged root cannot run it -- see KICKOS_SERVICE_LIST_ROOT_MMIO below.
      set(KICKOS_SERVICE_LIST "kickos_services_xmc4800relax_console")
    endif()
  elseif(KICKOS_BOARD STREQUAL "frdmk64f" AND KICKOS_HAVE_MPU)
    set(KICKOS_SERVICE_LIST "kickos_services_frdmk64f")
  else()
    set(KICKOS_SERVICE_LIST "kickos_services_none")
  endif()
endif()
set(KICKOS_SERVICE_LIST "${KICKOS_SERVICE_LIST}" CACHE STRING
  "CMake target defining kickos_board_services (the default-init service bring-up walk). Default kickos_services_none (count = 0: no services).")
# Derive the lib closure from the selected target EVERY configure (like the console
# knob), so an explicit A/B override still injects a linkable provider. A custom
# provider may pre-set KICKOS_SERVICE_LIST_LIBS with its own dep list.
if(NOT DEFINED KICKOS_SERVICE_LIST_LIBS)
  if(KICKOS_SERVICE_LIST STREQUAL "kickos_services_xmc4800relax")
    set(KICKOS_SERVICE_LIST_LIBS "kickos_services_xmc4800relax;kickos_xmcuart;kickos_xmcssc;kickos_class_xmc4800")
  elseif(KICKOS_SERVICE_LIST STREQUAL "kickos_services_xmc4800relax_console")
    set(KICKOS_SERVICE_LIST_LIBS "kickos_services_xmc4800relax_console;kickos_xmcuart;kickos_class_xmc4800")
  elseif(KICKOS_SERVICE_LIST STREQUAL "kickos_services_frdmk64f")
    set(KICKOS_SERVICE_LIST_LIBS "kickos_services_frdmk64f;kickos_k64dspi;kickos_k64uart;kickos_class_mk64f")
  else()
    set(KICKOS_SERVICE_LIST_LIBS "${KICKOS_SERVICE_LIST}")
  endif()
endif()
message(STATUS "KickOS: service list = ${KICKOS_SERVICE_LIST} (libs: ${KICKOS_SERVICE_LIST_LIBS})")

# Service lists whose bring-up bodies poke MMIO DIRECTLY from the calling (root)
# thread, rather than only from the granted window of the driver thread they spawn.
# Those cannot run under KICKOS_ROOT_PRIVILEGED=OFF: root faults on the first register
# write, mid-bring-up, with the console already relinquished -- i.e. silently, on the
# hardware, with no console left to say so. Refuse at CONFIGURE time instead. This is
# what the build-enforced knob buys: the blocker is a message, not a dark board.
# Retire an entry here when its bring-up body stops needing root MMIO (stage 3's
# arch_periph_enable covers k64uart and half of k64dspi); the SoT for WHY each one is
# listed is docs/design-unprivileged-root.md section 8.
set(KICKOS_SERVICE_LIST_ROOT_MMIO
  kickos_services_frdmk64f        # k64uart AIPS PACR + k64dspi clock/pin/GPIO/DSPI config
  kickos_services_xmc4800relax)   # xmcssc USIC kernel clock, baud, protocol (PV-write-only)
if(NOT KICKOS_ROOT_PRIVILEGED AND KICKOS_SERVICE_LIST IN_LIST KICKOS_SERVICE_LIST_ROOT_MMIO)
  message(FATAL_ERROR
    "KICKOS_ROOT_PRIVILEGED=OFF with KICKOS_SERVICE_LIST=${KICKOS_SERVICE_LIST}, whose "
    "bring-up writes peripheral registers directly from root. An unprivileged root "
    "faults on that write after the console has already been handed over, so the board "
    "goes dark with no diagnostic. Pick a service list whose bring-up is pure syscall "
    "(xmc4800-relax: kickos_services_xmc4800relax_console), or keep "
    "KICKOS_ROOT_PRIVILEGED=ON for this image. See docs/design-unprivileged-root.md "
    "section 8 and TODO.md stage 3.")
endif()

# Board pin-map provider selection (the default-init pin-mux step). The default init
# applies the board pin map BEFORE the console bring-up (see system/init/pinmux_run.cc
# + <kickos/sys/pinmap.h>). KICKOS_BOARD_PINMAP names the CMake target that DEFINES
# kickos_board_pinmap; it back-references kickos_user (via kos_pinmux_set at apply time)
# and is read by kickos_default_init, so it must join the rescan link group below.
# Default is kickos_pinmap_none (count = 0: nothing to mux). Resolved per-board AFTER
# KICKOS_HAVE_MPU is known, like KICKOS_SERVICE_LIST above; must sit after that block.
if(NOT DEFINED KICKOS_BOARD_PINMAP)
  if(KICKOS_BOARD STREQUAL "xmc4800-relax" AND KICKOS_HAVE_MPU)
    set(KICKOS_BOARD_PINMAP "kickos_pinmap_xmc4800relax")
  elseif(KICKOS_BOARD STREQUAL "frdmk64f" AND KICKOS_HAVE_MPU)
    set(KICKOS_BOARD_PINMAP "kickos_pinmap_frdmk64f")
  elseif(KICKOS_BOARD STREQUAL "picopi")
    set(KICKOS_BOARD_PINMAP "kickos_pinmap_picopi") # non-MPU; ungated (exercises the rp2040 backend)
  elseif(KICKOS_BOARD STREQUAL "f302nucleo")
    set(KICKOS_BOARD_PINMAP "kickos_pinmap_f302nucleo") # non-MPU; ungated (exercises the f302 backend)
  else()
    set(KICKOS_BOARD_PINMAP "kickos_pinmap_none")
  endif()
endif()
set(KICKOS_BOARD_PINMAP "${KICKOS_BOARD_PINMAP}" CACHE STRING
  "CMake target defining kickos_board_pinmap (the default-init pin-mux step). Default kickos_pinmap_none (nothing to mux).")
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

if(NOT DEFINED KICKOS_TELEMETRY)
  set(KICKOS_TELEMETRY "off")
endif()
set(KICKOS_TELEMETRY "${KICKOS_TELEMETRY}" CACHE STRING "Telemetry sink: off|rtt")
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
# rtt.h -- correct for the in-order M-class M1 targets. Pass the FULL call form,
# e.g. -DKICKOS_RTT_PUBLISH_BARRIER_DEF="__atomic_thread_fence(__ATOMIC_RELEASE)".
if(DEFINED KICKOS_RTT_PUBLISH_BARRIER_DEF)
  add_compile_definitions("KICKOS_RTT_PUBLISH_BARRIER()=${KICKOS_RTT_PUBLISH_BARRIER_DEF}")
endif()

# Local numeric TZ offset (e.g. "+0200"), computed ONCE at configure, exposed to app
# TUs so kickos_app_build_stamp() can align its line with the CMake build stamp (which
# appends the same "%z"). Passed as a BARE token (-DKICKOS_APP_TZ=+0200), not a quoted
# string: embedded escaped quotes double up on a reconfigure of an existing build dir,
# so app.h stringizes the token in C instead. `date` failing (empty) just skips the
# define -- app.h stays offset-free then, keeping a plain hosted app build portable.
execute_process(COMMAND date +%z OUTPUT_VARIABLE _apptz OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT _apptz STREQUAL "")
  add_compile_definitions(KICKOS_APP_TZ=${_apptz})
endif()

# Standard install dirs; included before the components so their
# $<INSTALL_INTERFACE:...> can use CMAKE_INSTALL_INCLUDEDIR.
include(GNUInstallDirs)

if(KICKOS_BUILD_TESTS)
  enable_testing()

  # Rebuild the tree before any test runs, so `ctest` / `ctest --preset` can never
  # execute a stale binary (configure-only + ctest would otherwise silently run the
  # previous build). The build is one CTest fixture (SETUP); every test registered
  # below is auto-wired to require it by the add_test wrapper just after.
  add_test(NAME kickos_build COMMAND "${CMAKE_COMMAND}" --build "${PROJECT_BINARY_DIR}")
  set_tests_properties(kickos_build PROPERTIES FIXTURES_SETUP kickos_build TIMEOUT 900)

  # Make every subsequently-registered test require that fixture, without editing
  # each app's CMakeLists. A macro (not a function) so set_property runs in the
  # caller's directory scope -- the only scope where the test it just added exists.
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
endif()

# Components are added as they land; guarded so every step boundary configures.
foreach(_dir lib arch kernel user system)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_dir}/CMakeLists.txt")
    add_subdirectory(${_dir})
  endif()
endforeach()

# Host-only telemetry CI gate (golden-vector round-trip): builds a host tool, so
# it registers on the sim build only. Config-independent (does not need telemetry
# enabled -- it exercises the pure encoders + the decoder directly).
if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/telemetry/CMakeLists.txt")
  add_subdirectory(tests/telemetry)
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
  # The archive list, named once. The GROUP itself is built per posture leaf below
  # rather than here, because its tail differs: an MCU link has to pull the toolchain
  # runtime into the same group, and which runtime that is IS the posture. CMake also
  # forbids one target's link closure from carrying the same library in two different
  # groups, so there can only be one group and it has to be the leaf's.
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
  # A transient -D knob override changed the values the libs were built with but not
  # the shipped board_config.h, so carry those overrides here too (they win over the
  # header's #ifndef) -- else an OOT TU including a kernel-internal header would
  # disagree on the layout (ODR).
  if(KICKOS_PROVISION_OVERRIDES)
    target_compile_definitions(kickos_core INTERFACE ${KICKOS_PROVISION_OVERRIDES})
  endif()
  # KICKOS_TELEMETRY gates fields in struct Kernel (instance.h) and arch_context
  # (context.h); KICKOS_HAVE_MPU branches KOS_STACK_DEFINE (kos.h). An OOT TU that
  # includes a kernel-internal header must see the same posture as the prebuilt libs
  # (ODR), so carry them as usage requirements.
  # KICKOS_ROOT_PRIVILEGED is here for a different reason from the ODR ones above: an
  # app TU may legitimately need to know whether it runs as a privileged root (it
  # decides whether spawning a privileged child can succeed at all), and this is what
  # makes the posture readable out of tree instead of guessable.
  target_compile_definitions(kickos_core INTERFACE
    KICKOS_TELEMETRY=${_telem} KICKOS_TELEMETRY_RTT=${_telem}
    KICKOS_TRACE_ARCH=${KICKOS_TRACE_ARCH}
    KICKOS_HAVE_MPU=${KICKOS_HAVE_MPU}
    KICKOS_ROOT_PRIVILEGED=${_root_priv}
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
  # nothing references (else --gc-sections drops them). -nostdlib++ is NOT here --
  # it belongs to the freestanding leaf only.
  if(NOT (KICKOS_ARCH STREQUAL "sim") AND KICKOS_LINKER_SCRIPT)
    target_link_options(kickos_core INTERFACE
      -nostartfiles
      -Wl,--gc-sections
      -Wl,--no-warn-rwx-segments
      # ARM's vector table (g_isr_vector) is a data object nothing references. RX
      # pulls its vector tables via ENTRY(_start) + KEEP(), so it needs no force-ref
      # (and has no g_isr_vector -- forcing it would leave a dangling undefined).
      "$<$<STREQUAL:${KICKOS_ARCH_FAMILY},arm>:-Wl,-u,g_isr_vector>"
      # Force our libc syscall stubs in (libc's raise->_exit). The RX psABI prefixes
      # C identifiers with a leading underscore, so C `_exit` is asm `__exit`; forcing
      # `_exit` there would pull nothing and drop newlib_stubs.o (RX sbrk/__dso_handle).
      "$<$<STREQUAL:${KICKOS_ARCH_FAMILY},rx>:-Wl,-u,__exit>"
      "$<$<NOT:$<STREQUAL:${KICKOS_ARCH_FAMILY},rx>>:-Wl,-u,_exit>"
      # A chip with a checksummed flash second stage (RP2040 boot2) must force its
      # .boot2 blob object in too -- nothing references it.
      $<$<BOOL:${KICKOS_BOOT2_FORCE_SYMBOL}>:-Wl,-u,${KICKOS_BOOT2_FORCE_SYMBOL}>
      # In-tree: the source linker script. Installed/exported: the shipped copy
      # under <prefix>/<libdir>/kickos so an out-of-tree consumer links without
      # the KickOS source tree present.
      "$<BUILD_INTERFACE:SHELL:-T ${KICKOS_LINKER_SCRIPT}>"
      "$<INSTALL_INTERFACE:SHELL:-T $<INSTALL_PREFIX>/${CMAKE_INSTALL_LIBDIR}/kickos/${KICKOS_CHIP}.ld>")
    # ...and the same script as a real link DEPENDENCY, not just a flag. -T reaches
    # the linker as an opaque driver option, so nothing in it tells the build system
    # that the image is a function of that file: edit the .ld and the link is not
    # re-run, leaving a stale image to be flashed. INTERFACE_LINK_DEPENDS puts the
    # file on the link edge, and being an INTERFACE property it rides the export --
    # so a plain add_executable + target_link_libraries(app PRIVATE kickos) consumer
    # gets the correct edge with no helper and no knowledge that the hazard exists.
    # Paths mirror the -T above (build tree: the generated script; installed: the
    # shipped copy), because the dependency must name the file actually linked.
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
  # The rescan group, per leaf. On MCU the toolchain runtime joins it, and that is
  # load-bearing rather than tidy: newlib's `_sbrk_r` references `_sbrk`, which
  # libkickos_user.a provides on demand, and the g++ driver appends its libraries AFTER
  # everything CMake emits -- so with them outside the group the linker meets `_sbrk_r`
  # (freestanding) or `operator new` -> `malloc` -> `_sbrk_r` (full C++) long past the
  # point it could still go back for the member. That was invisible while `_sbrk` shared
  # the -Wl,-u,_exit force-linked object, which put it in the link before libc ever
  # asked. Naming these here adds no library the driver would not link anyway; it only
  # fixes WHERE they are scanned. The freestanding leaf deliberately does not name
  # libstdc++ -- it links -nostdlib++, and a stray operator new must stay a link error.
  # Sim links host glibc through the normal driver path and keeps the bare group.
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
    # libsupc++/libstdc++ -- a stray operator new / EH table then fails the link loud.
    target_compile_options(kickos INTERFACE
      "$<$<COMPILE_LANGUAGE:CXX>:${KICKOS_FREESTANDING_CXX_FLAGS}>")
    target_link_options(kickos INTERFACE -nostdlib++)
    # Full C++: -fexceptions/-frtti, and omitting -nostdlib++ lets the g++ driver
    # auto-link the toolchain libstdc++/libsupc++ (EH, RTTI, new/delete) over newlib.
    # kickos_cxx never sees the freestanding clamp, so there is no -fno-exceptions to
    # override -- the two flag sets never collide on one command line.
    target_compile_options(kickos_cxx INTERFACE
      "$<$<COMPILE_LANGUAGE:CXX>:-fexceptions;-frtti>")
    # A second rescan group, for the same reason the core group took libc (see there),
    # one layer further out: on this leaf the allocator demand starts in libstdc++'s
    # operator new, and the g++ driver appends libstdc++ AFTER everything CMake emits.
    # So the chain operator new -> malloc -> _sbrk_r -> _sbrk has to be closable in a
    # group that contains all three providers, or the on-demand `_sbrk` member is
    # already behind the linker when libstdc++ asks for it. Naming them here does not
    # add libraries the driver would not link anyway; it only fixes where they are
    # scanned. Listed after kickos_core, so the core group is still resolved first.
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
    target_compile_features(kickos_cxx_rt PRIVATE cxx_std_17)
    target_compile_options(kickos_cxx_rt PRIVATE ${KICKOS_WARN_FLAGS} -fexceptions -frtti)
    # kickos_core no longer carries the archive group (it moved to the leaves, above),
    # so name the archives this object needs for their INCLUDE dirs. It links nothing --
    # it IS an object library -- so these are usage requirements only, and PRIVATE keeps
    # them from reaching a consumer of the objects.
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
  # Same late validation for the service-list provider (defined in system/ or a chip's
  # user/driver/ subtree, so it is not a target at the kickos_core site above).
  if(NOT TARGET ${KICKOS_SERVICE_LIST})
    message(FATAL_ERROR "KICKOS_SERVICE_LIST='${KICKOS_SERVICE_LIST}' is not a CMake target. Name a target that defines kickos_board_services, or use the default kickos_services_none (count = 0).")
  endif()
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
# the arch subdir reports -- arch/sim/include vs arch/arm/armv7m/include -- so
# use that, not a arch/${ARCH}/include guess (which is wrong for the ARM tree).
install(DIRECTORY
          "${CMAKE_CURRENT_SOURCE_DIR}/include/"
          "${CMAKE_CURRENT_SOURCE_DIR}/arch/include/"
          "${KICKOS_ARCH_INCLUDE_DIR}/"
          "${CMAKE_CURRENT_SOURCE_DIR}/kernel/include/"
          "${CMAKE_CURRENT_SOURCE_DIR}/lib/include/"
          "${CMAKE_CURRENT_SOURCE_DIR}/user/include/"
          "${CMAKE_CURRENT_SOURCE_DIR}/system/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# Ship the selected board's board_config.h (MAX_IRQ + provisioning) so an
# out-of-tree consumer's config headers read the same values the prebuilt libs
# did -- the header replaces the old -D provisioning export.
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

# The build helper is shipped verbatim so kickos_add_application() is available.
install(FILES
          "${CMAKE_CURRENT_BINARY_DIR}/KickOSConfig.cmake"
          "${CMAKE_CURRENT_BINARY_DIR}/KickOSConfigVersion.cmake"
          "${CMAKE_CURRENT_SOURCE_DIR}/cmake/kickos.cmake"
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
    # The compiler-capability gate is include()d by the ARM + RISC-V toolchain
    # files from their own list dir, so it must land beside them in the package
    # (the RX/Xtensa files do not use it -- no C-only same-name twin exists to
    # fall through to).
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-arm-none-eabi.cmake"
                  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-cxx-runtime-check.cmake"
            DESTINATION "${KICKOS_CMAKE_DIR}")
  endif()
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/boards/${KICKOS_BOARD}/board.cmake"
          DESTINATION "${KICKOS_CMAKE_DIR}")
endif()
