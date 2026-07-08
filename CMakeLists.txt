# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

cmake_minimum_required(VERSION 3.24)
project(KickOS VERSION 0.0.1 LANGUAGES C CXX ASM)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(cmake/kickos.cmake)

# --- Board / arch selection -------------------------------------------------
set(KICKOS_BOARD "sim" CACHE STRING "Target board: sim|qemu|frdmk64f|picopi|f411disco|bluepill")
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
              KICKOS_MAX_SEMAPHORES KICKOS_MAX_IRQ_HANDLES
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

# --- Telemetry backend selection (docs/telemetry.md) -----------------
# KICKOS_TELEMETRY = off | rtt. Kernel-observability records (context switch,
# syscall, IRQ) emitted to RTT channel 1. Per-board default off (a benchmarking
# opt-in). `rtt` requires (a) a trace clock -- arch_trace_now, capability var
# KICKOS_HAVE_TRACE_CLOCK -- and (b) the RTT transport (KICKOS_CONSOLE in
# {rtt,both}); either missing is a hard configuration error.
#
# Trace-clock capability + the SESSION arch id, derived per arch/chip:
#   sim     -> clock_now/1000 (us)             have=1, id=0
#   armv7m  -> weak DWT CYCCNT (mps2 overrides) have=1, id=1
#   armv6m  -> chip-provided only (no DWT)       have=chip, id=2
# KICKOS_HAVE_TRACE_CLOCK is OVERRIDABLE: a board/chip/preset that provides
# arch_trace_now can set it directly (respected if already defined) -- so the
# armv6m chip list below is only a convenience default, not the source of truth.
# (This is the seam the coming board.cmake reconciliation folds per-chip caps into,
# replacing the hardcoded chip names.)
set(KICKOS_TRACE_ARCH 0)
if(KICKOS_ARCH STREQUAL "armv7m")
  set(KICKOS_TRACE_ARCH 1)
elseif(KICKOS_ARCH STREQUAL "armv6m")
  set(KICKOS_TRACE_ARCH 2)
elseif(KICKOS_ARCH STREQUAL "rxv3")
  set(KICKOS_TRACE_ARCH 3)
elseif(KICKOS_ARCH STREQUAL "lx6")
  set(KICKOS_TRACE_ARCH 4)
elseif(KICKOS_ARCH STREQUAL "rv32imac")
  set(KICKOS_TRACE_ARCH 5)
endif()
if(NOT DEFINED KICKOS_HAVE_TRACE_CLOCK)
  set(KICKOS_HAVE_TRACE_CLOCK 0)
  if(KICKOS_ARCH STREQUAL "sim" OR KICKOS_ARCH STREQUAL "armv7m"
     OR KICKOS_ARCH STREQUAL "rxv3" OR KICKOS_ARCH STREQUAL "lx6"
     OR KICKOS_ARCH STREQUAL "rv32imac")
    set(KICKOS_HAVE_TRACE_CLOCK 1) # sim clock/1000; armv7m weak DWT; rxv3 CMTW1; lx6 CCOUNT; rv32imac rdcycle
  elseif(KICKOS_CHIP STREQUAL "rp2040" OR KICKOS_CHIP STREQUAL "nrf51")
    set(KICKOS_HAVE_TRACE_CLOCK 1) # armv6m chips that ship arch_trace_now (default)
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
add_compile_definitions(KICKOS_HAVE_MPU=${KICKOS_HAVE_MPU})

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

# RTT publish barrier override (docs/telemetry.md G6): lib/ is a leaf and must
# not reach into an arch header, so a weakly-ordered core injects its real fence
# here as a compile definition. Default (unset) keeps the compiler-only barrier in
# rtt.h -- correct for the in-order M-class M1 targets. Pass the FULL call form,
# e.g. -DKICKOS_RTT_PUBLISH_BARRIER_DEF="__atomic_thread_fence(__ATOMIC_RELEASE)".
if(DEFINED KICKOS_RTT_PUBLISH_BARRIER_DEF)
  add_compile_definitions("KICKOS_RTT_PUBLISH_BARRIER()=${KICKOS_RTT_PUBLISH_BARRIER_DEF}")
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
foreach(_dir lib arch kernel user)
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
if(TARGET kickos_kernel)
  add_library(kickos INTERFACE)
  # On MCU the chip backend (arch_init/console + reset vector) joins the rescan
  # group; the arch<->kernel<->chip callback cycles resolve as one archive group.
  if(KICKOS_CHIP AND TARGET kickos_chip_${KICKOS_CHIP})
    target_link_libraries(kickos INTERFACE
      "$<LINK_GROUP:RESCAN,kickos_user,kickos_kernel,kickos_chip_${KICKOS_CHIP},kickos_arch_${KICKOS_ARCH},kickos_lib>")
  else()
    target_link_libraries(kickos INTERFACE
      "$<LINK_GROUP:RESCAN,kickos_user,kickos_kernel,kickos_arch_${KICKOS_ARCH},kickos_lib>")
  endif()
  # OS-agnostic app entry (architecture.md 8j), carried as usage requirements so
  # both the plain add_executable path and kickos_add_application inherit them
  # (in-tree and out-of-tree): the app writes a plain `int main`, renamed to the
  # kernel-called symbol; app.h is force-included so the renamed C++ main gets C
  # linkage (unmangled).
  target_compile_definitions(kickos INTERFACE main=kickos_app_main)
  # Provisioning normally travels via the shipped board_config.h (config/{board,
  # system}.h include it), so an out-of-tree consumer's TUs compile to the same
  # layout as the prebuilt libs by reading the same header -- one source of truth.
  # The ONE exception: a transient -D knob override changed the values the libs
  # were built with but not the shipped header, so carry those specific overrides
  # on the interface target too (they win over the header's #ifndef) -- else an
  # OOT TU including a kernel-internal header would disagree on the layout (ODR).
  if(KICKOS_PROVISION_OVERRIDES)
    target_compile_definitions(kickos INTERFACE ${KICKOS_PROVISION_OVERRIDES})
  endif()
  # The telemetry defines are directory-scope (add_compile_definitions above), so
  # they reach the prebuilt libs but NOT an out-of-tree TU that includes a kernel-
  # internal header. KICKOS_TELEMETRY gates fields in struct Kernel (instance.h)
  # and arch_context (context.h), so an OOT telemetry build compiling against the
  # default (off) header would disagree on the layout (ODR); KICKOS_TRACE_ARCH sets
  # the arch id emitted by the inline ktrace_session. Carry them on the interface
  # target (mirrors the KICKOS_RTT_CH1_SIZE forwarding via KICKOS_PROVISION_OVERRIDES).
  target_compile_definitions(kickos INTERFACE
    KICKOS_TELEMETRY=${_telem} KICKOS_TELEMETRY_RTT=${_telem}
    KICKOS_TRACE_ARCH=${KICKOS_TRACE_ARCH})
  target_compile_options(kickos INTERFACE -include kickos/app.h)
  if(KICKOS_ARCH STREQUAL "sim")
    find_package(Threads REQUIRED)
    target_link_libraries(kickos INTERFACE Threads::Threads rt)
  else()
    # MCU: the bare-metal link is -nostdlib++ (no libsupc++/libstdc++), so app TUs
    # must compile freestanding too -- otherwise EH/RTTI tables from a destructor
    # reference __gxx_personality_v0 / __cxa_end_cleanup and the link fails. (An
    # app opting into full C++ would additionally link the toolchain's libsupc++;
    # that opt-in is a later story.)
    target_compile_options(kickos INTERFACE
      "$<$<COMPILE_LANGUAGE:CXX>:${KICKOS_FREESTANDING_CXX_FLAGS}>")
  endif()
  # MCU bare-metal link recipe (baked into the interface target): our reset object
  # instead of crt0, the chip linker script, dead-code elimination, and a force-
  # reference to the vector table (else --gc-sections would drop it).
  if(NOT (KICKOS_ARCH STREQUAL "sim") AND KICKOS_LINKER_SCRIPT)
    # -T at the DRIVER level (not -Wl,-T): the picolibc-based toolchain's spec
    # only suppresses its default linker script (picolibc.ld) when it sees a
    # driver-level -T; a -Wl,-T is invisible to that check and both scripts would
    # apply (colliding at address 0).
    target_link_options(kickos INTERFACE
      -nostartfiles
      -nostdlib++              # freestanding C++: no libstdc++ (keep libc/libgcc)
      -Wl,--gc-sections
      -Wl,--no-warn-rwx-segments
      # ARM's vector table (g_isr_vector) is a data object nothing references, so
      # force it in (else --gc-sections drops it). RX pulls its two vector tables
      # via ENTRY(_start) + KEEP() in the linker script, so it needs no force-ref
      # (and has no g_isr_vector -- forcing it would leave a dangling undefined).
      "$<$<STREQUAL:${KICKOS_ARCH_FAMILY},arm>:-Wl,-u,g_isr_vector>"
      -Wl,-u,_exit             # force our libc syscall stubs in (libc's raise->_exit)
      # A chip with a checksummed flash second stage (RP2040 boot2) must force its
      # .boot2 blob object in too -- nothing references it.
      $<$<BOOL:${KICKOS_BOOT2_FORCE_SYMBOL}>:-Wl,-u,${KICKOS_BOOT2_FORCE_SYMBOL}>
      # In-tree: the source linker script. Installed/exported: the shipped copy
      # under <prefix>/<libdir>/kickos so an out-of-tree consumer links without
      # the KickOS source tree present.
      "$<BUILD_INTERFACE:SHELL:-T ${KICKOS_LINKER_SCRIPT}>"
      "$<INSTALL_INTERFACE:SHELL:-T $<INSTALL_PREFIX>/${CMAKE_INSTALL_LIBDIR}/kickos/${KICKOS_CHIP}.ld>")
  endif()
  install(TARGETS kickos EXPORT KickOSTargets)
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
            DESTINATION "${KICKOS_CMAKE_DIR}")
  else()
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-arm-none-eabi.cmake"
            DESTINATION "${KICKOS_CMAKE_DIR}")
  endif()
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/boards/${KICKOS_BOARD}/board.cmake"
          DESTINATION "${KICKOS_CMAKE_DIR}")
endif()
