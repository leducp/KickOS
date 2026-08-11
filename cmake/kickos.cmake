# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# KickOS build helpers: per-component flag posture, the optional application helper
# kickos_add_application(), and the image emitter kickos_emit_image().
#
# Design (architecture.md, invariant #8): the application owns the final link.
# KickOS ships static libraries + headers + startup; the application performs the
# link and emits the image (host ELF for sim; .bin/.hex/.uf2 for MCUs). Switching
# sim<->MCU is a one-word BOARD change.
#
# The link recipe lives on the exported `kickos` / `kickos_cxx` usage targets, NOT
# in these helpers: the supported downstream shape is plain add_executable +
# target_link_libraries(app PRIVATE kickos), on the sim and on bare metal alike.
# What is left in this file is in-tree-only policy or an explicit opt-in.

# ---------------------------------------------------------------------------
# Board -> {arch, chip} resolution.
#
# The single source of truth for a board's arch/chip/-mcpu is one descriptor,
# boards/<board>/board.cmake (also included pre-project by the ARM toolchain file
# for the CPU baseline). Here we include it to read arch + chip. The chip is the
# arch/arm/chip/<chip> backend (startup, linker script, clocks, console); the sim
# has none (KICKOS_CHIP == "").
#
# Captured at include time (a called function sees the caller's list dir, not this
# file's): the in-tree boards/ dir. It is <repo>/boards in a source tree; an
# installed package has no boards/ tree, so the fallback below applies there.
# ---------------------------------------------------------------------------
get_filename_component(KICKOS_BOARDS_DIR "${CMAKE_CURRENT_LIST_DIR}/../boards" ABSOLUTE)

# List-dir-relative: cap_table.cmake must be installed beside this file.
include("${CMAKE_CURRENT_LIST_DIR}/cap_table.cmake")

# In-tree vs installed-package signal: a source tree has boards/ beside cmake/; an
# installed package ships kickos.cmake with no boards/ sibling. Named once; every
# in-tree-vs-consumer decision below reads this one variable.
if(EXISTS "${KICKOS_BOARDS_DIR}")
  set(KICKOS_IN_TREE TRUE)
else()
  set(KICKOS_IN_TREE FALSE)
endif()

# The descriptor also carries KICKOS_ARCH_FAMILY (arm|rx|sim|...): the source-tree
# family that routes arch/<family>/... and the family-specific cross toolchain. A
# board that omits it falls back to a derivation from the arch (armv6m/armv7m -> arm,
# sim -> sim) so the ARM descriptors need no churn; a non-ARM family (rx) sets it.
function(kickos_derive_arch_family arch out_family)
  if(arch MATCHES "^armv")
    set(${out_family} "arm" PARENT_SCOPE)
  elseif(arch STREQUAL "sim")
    set(${out_family} "sim" PARENT_SCOPE)
  else()
    set(${out_family} "${arch}" PARENT_SCOPE)
  endif()
endfunction()

function(kickos_load_board_descriptor board out_arch out_chip out_family)
  set(_desc "${KICKOS_BOARDS_DIR}/${board}/board.cmake")
  if(EXISTS "${_desc}")
    include("${_desc}")
    set(${out_arch} "${KICKOS_ARCH}" PARENT_SCOPE)
    set(${out_chip} "${KICKOS_CHIP}" PARENT_SCOPE)
  elseif(NOT KICKOS_IN_TREE
         AND DEFINED KICKOS_ARCH AND board STREQUAL "${KICKOS_BOARD}")
    # Installed package: no boards/ tree at all, so fall back to the arch/chip this
    # package recorded (KickOSConfig) for the single board it was built for. Gated
    # on the boards/ tree being ABSENT so an in-tree unknown board still errors
    # (in-tree the host toolchain also defines KICKOS_ARCH, which alone would let a
    # typo'd board slip through as the recorded arch).
    set(${out_arch} "${KICKOS_ARCH}" PARENT_SCOPE)
    set(${out_chip} "${KICKOS_CHIP}" PARENT_SCOPE)
  else()
    message(FATAL_ERROR "KickOS: unknown board '${board}' "
      "(no ${_desc}, and it is not the board this package was built for)")
  endif()
  # Family: honour an explicit descriptor value, else derive from the arch.
  if(KICKOS_ARCH_FAMILY)
    set(${out_family} "${KICKOS_ARCH_FAMILY}" PARENT_SCOPE)
  else()
    kickos_derive_arch_family("${KICKOS_ARCH}" _fam)
    set(${out_family} "${_fam}" PARENT_SCOPE)
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Flag posture.
#   Kernel / lib / userspace  -> freestanding C++ (no exceptions/rtti).
#   arch/sim                  -> hosted (bridges to host libc), still no exc/rtti.
# Applied per-target so a hosted arch TU and a freestanding kernel TU coexist
# in one binary.
#
# WARNING FLAGS NEVER LEAVE THIS PROJECT: they are applied PRIVATE to targets we
# own, and the exported `kickos`/`kickos_cxx` usage targets carry none of them.
# Application targets are the one shape both the fleet and a consumer create; see
# the KICKOS_IN_TREE guard in kickos_add_application().
# ---------------------------------------------------------------------------
set(KICKOS_WARN_FLAGS
  -Wall -Wextra -Wshadow -Wundef)

# Warnings-as-errors: default ON in tree, OFF for a consumer (promoting somebody
# else's warnings to hard errors is not our call). Riding KICKOS_WARN_FLAGS keeps it
# per-target, so it never reaches CMake's try_compile/ABI probes, and the same gate
# runs on the desk and in CI. -DKICKOS_WERROR=OFF is the escape hatch for a bisect
# or a toolchain bump that lands new diagnostics.
if(NOT DEFINED KICKOS_WERROR)
  set(KICKOS_WERROR ${KICKOS_IN_TREE})
endif()
if(KICKOS_WERROR)
  list(APPEND KICKOS_WARN_FLAGS -Werror)
endif()

# Flags valid for every language (C, C++, ASM); the C++-only ones are guarded
# below so a target mixing .cc and .S (the ARM arch backends) stays warning-free.
set(KICKOS_FREESTANDING_FLAGS
  -ffreestanding
  -fno-common)
set(KICKOS_FREESTANDING_CXX_FLAGS
  -fno-exceptions -fno-rtti
  -fno-threadsafe-statics -fno-use-cxa-atexit)

# Applied PRIVATE, so the C++20 level does not ride out through KickOSTargets.cmake as
# INTERFACE_COMPILE_FEATURES and compile a consumer's C++17 codebase as C++20. The rule
# that buys: no INSTALLED header may use a C++20 construct, since a consumer compiles
# those in their own dialect. tests/check_public_headers.sh is what keeps it true.
set(KICKOS_CXX_STANDARD cxx_std_20)
set(KICKOS_CXX_INTERFACE_STANDARD cxx_std_17)

# freestanding TUs: kernel, lib, user, and the ARM arch backends (C++ + ASM).
function(kickos_apply_freestanding target)
  target_compile_features(${target} PRIVATE ${KICKOS_CXX_STANDARD})
  target_compile_features(${target} INTERFACE ${KICKOS_CXX_INTERFACE_STANDARD})
  target_compile_options(${target} PRIVATE
    ${KICKOS_WARN_FLAGS} ${KICKOS_FREESTANDING_FLAGS}
    "$<$<COMPILE_LANGUAGE:CXX>:${KICKOS_FREESTANDING_CXX_FLAGS}>")
  # RISC-V PMP enforcement: the KickOS-owned libs (kernel/arch/chip/lib/user, all
  # freestanding) emit NO gp-relative small-data, so the single gp window holds only
  # app + C++-runtime small-data and can sit inside the granted .appdata region.
  # -msmall-data-limit=0 routes every KickOS global to ordinary .data/.bss, captured
  # kernel-side by the linker colon selectors. NOT applied to the app: its
  # -fexceptions TUs need gp-relative small-data or __cxa_throw hangs in the FDE walk
  # (docs/design-cxx-under-mpu.md). A CI guard (check_riscv_no_smalldata.sh) asserts
  # the built archives carry zero .sdata/.sbss.
  if(KICKOS_ARCH STREQUAL "rv32imac" AND KICKOS_HAVE_MPU)
    target_compile_options(${target} PRIVATE -msmall-data-limit=0)
  endif()
endfunction()

# hosted C++ TUs: the sim arch backend only
function(kickos_apply_hosted target)
  target_compile_features(${target} PRIVATE ${KICKOS_CXX_STANDARD})
  target_compile_features(${target} INTERFACE ${KICKOS_CXX_INTERFACE_STANDARD})
  target_compile_options(${target} PRIVATE
    ${KICKOS_WARN_FLAGS} -fno-exceptions -fno-rtti)
  target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
endfunction()

# The per-chip -mcpu/-mfpu/-mfloat-abi baseline is set globally by the ARM
# toolchain file (CMAKE_<LANG>_FLAGS_INIT) so it applies uniformly to every
# compile and link (correct multilib). Freestanding TUs on ARM therefore need
# no extra CPU flags here; kickos_apply_freestanding() is arch-agnostic.

# ---------------------------------------------------------------------------
# kickos_emit_image(<target>)
#   MCU only: turn a linked ELF into flashable .bin and .hex, and print size.
#   No-op on the sim (a runnable host ELF is the deliverable there).
#
#   PUBLIC: the one thing a bare-metal consumer cannot get from linking `kickos`,
#   because a POST_BUILD action cannot ride a usage requirement. One opt-in line
#   after target_link_libraries(app PRIVATE kickos). Shipped rather than left to
#   consumer objcopy: the esp32 family needs esptool elf2image with per-chip flags
#   (below), and getting them wrong flashes cleanly then reset-loops.
# ---------------------------------------------------------------------------
function(kickos_emit_image target)
  # KICKOS_ARCH is the single source of truth for "is this the sim" (set by the
  # toolchain + board descriptor, and recorded in the installed package, so this
  # also holds for an out-of-tree consumer).
  if(KICKOS_ARCH STREQUAL "sim")
    return()
  endif()
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${target}> $<TARGET_FILE_DIR:${target}>/${target}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${target}> $<TARGET_FILE_DIR:${target}>/${target}.hex
    BYPRODUCTS ${target}.bin ${target}.hex
    VERBATIM)
  if(CMAKE_SIZE)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${target}>
      VERBATIM)
  endif()

  # Espressif chips (Xtensa esp32, RISC-V esp32c6): the raw objcopy .bin is NOT
  # bootable: the ROM loader needs the Espressif image format (magic 0xE9, segment
  # table, checksum); esptool elf2image builds it from the ELF (entry -> _start,
  # segments -> SRAM). Graceful: if esptool is not on PATH (i.e. the esp-idf env is
  # not active), skip and tell the user, rather than failing the build (the ELF +
  # raw .bin/.hex are still emitted). --chip is the KickOS chip name (esptool accepts
  # esp32 / esp32c6 verbatim). Prefer `esptool` (esptool.py is deprecated in v5).
  if(KICKOS_CHIP STREQUAL "esp32" OR KICKOS_CHIP STREQUAL "esp32c6")
    find_program(KICKOS_ESPTOOL NAMES esptool esptool.py)
    # Our app IS the image at the ROM bootloader offset (0x1000 on esp32), so the
    # first-stage ROM loads it using the header's flash mode BEFORE any code
    # reconfigures the SPI pins. esptool's elf2image default is QIO, which the esp32
    # ROM reads unreliably from that position. It loads segment 0 then reads a
    # garbage segment-1 header (`load:0xffffffff,len:-1`) and RTC-WDT reset-loops.
    # Force DIO for esp32 (same reason esp-idf always flashes its 2nd-stage
    # bootloader as DIO). Verified on ESP32-D0WD-V3 silicon 2026-07-08.
    set(_kos_img_mode "")
    if(KICKOS_CHIP STREQUAL "esp32")
      set(_kos_img_mode --flash_mode dio)
    elseif(KICKOS_CHIP STREQUAL "esp32c6")
      # ESP32-C6: our app is a RAM-only image at flash 0x0 with NO 2nd-stage
      # bootloader, so the RISC-V ROM loader needs --ram-only-header (which implies
      # --dont-append-digest) to boot it. A plain elf2image image is loaded but
      # never entered (`ets_loader.c 67`). DIO for the same reason as esp32 (the ROM
      # mis-reads a QIO header from the boot position -> "Checksum failure" reset
      # loop). Verified on ESP32-C6-WROOM silicon 2026-07-08.
      set(_kos_img_mode --ram-only-header --dont-append-digest --flash_mode dio)
    endif()
    if(KICKOS_ESPTOOL)
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${KICKOS_ESPTOOL} --chip ${KICKOS_CHIP} elf2image ${_kos_img_mode}
                --output $<TARGET_FILE_DIR:${target}>/${target}.app.bin
                $<TARGET_FILE:${target}>
        BYPRODUCTS ${target}.app.bin
        COMMENT "esptool elf2image -> ${target}.app.bin (bootable ${KICKOS_CHIP} image)"
        VERBATIM)
    else()
      message(STATUS "KickOS: esptool not found -- ${target}.app.bin (bootable "
        "${KICKOS_CHIP} image) not produced. Activate the esp-idf env, or run: "
        "esptool --chip ${KICKOS_CHIP} elf2image --output ${target}.app.bin <elf>  "
        "(the raw ${target}.bin is NOT bootable).")
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# kickos_add_application(<name> SOURCES <src...> BOARD <board> [FULL_CXX]
#                        [CAPABILITIES <n>] [CAPABILITIES_OPTIONAL <m>]
#                        [CAPABILITIES_INBOUND_REPLY <r>]
#                        [SPI_BACKEND <target>])
#   Links the app against the KickOS component libraries and emits the image.
#   For sim this is a runnable host ELF whose entry (host main) lives in the
#   sim arch backend; the app must define kickos_app_main().
#
#   CAPABILITIES is this app's PEAK of concurrently held capabilities, one of the four
#   terms root's table width is summed from (cmake/cap_table.cmake). Omitted, the app
#   gets KICKOS_CAP_APP_PEAK_DEFAULT.
#   CAPABILITIES_OPTIONAL is further peak whose holders reclaim and self-skip when they
#   cannot allocate, so it is granted only where supply covers it and never makes a board
#   fail to configure.
#   CAPABILITIES_INBOUND_REPLY is how many CAP_REPLY capabilities one of this app's tasks
#   holds at once as the SERVER side of kos_call: a client mints into the server's table
#   (kernel/syscall/syscall_ipc.cc), so without it the sum does not bound when the server's
#   own creates start failing. It is the peak of CONCURRENTLY parked callers, not a count of
#   calls. Omitted, the app gets KICKOS_CAP_REPLY_DEFAULT (0).
#   All three size ROOT's table. Every spawned child is seated at KICKOS_CAP_CHILD_WIDTH,
#   which no declaration here reaches.
#   Out of tree any of these three is recorded, WARNED about and not acted on: the width is
#   fixed by the installed package the app links.
#
#   OPTIONAL SUGAR. The supported out-of-tree path is plain CMake, and it is
#   complete: find_package(KickOS), add_executable, target_link_libraries(app
#   PRIVATE kickos) [or kickos_cxx], kickos_emit_image on MCU. This helper exists
#   for the in-tree fleet; a consumer may use it, but nothing needs it.
#
#   SPI_BACKEND names the CMake target providing this executable's implementation of the
#   SPI class <kickos/driver/spi.h>: a per-chip local engine (kickos_spi_xmcssc,
#   kickos_spi_k64dspi) or the chip-agnostic kickos_spi_proxy. Target-name-valued and
#   fail-loud, like KICKOS_SERVICE_LIST and KICKOS_INIT_PROVIDER.
#   PER CONSUMER TARGET, not per build directory: which chip is a board fact, but local
#   versus remote is a system-composition choice, and two executables in one tree may
#   legitimately differ. It is a per-target keyword rather than a global knob because the
#   backend archives are kept OUT of the shared kickos link group, so nothing else in the
#   image decides it. Exactly ONE backend per executable: they define the same four symbols,
#   so a second one is a duplicate definition rather than a choice, which is also why a
#   service driver in the same image keeps its engine copy under private symbols.
#
#   FULL_CXX (opt-in, docs/design-kickcat-k64f.md "Libc strategy"): compile this
#   app's C++ TUs with -fexceptions/-frtti (NOT the freestanding clamp) and link
#   the toolchain's libstdc++/libsupc++ over newlib, so exceptions + STL + RTTI
#   work. Off by default: every other app stays freestanding, no libstdc++,
#   zero-overhead. No effect on the sim (already hosted against host libstdc++).
# ---------------------------------------------------------------------------
function(kickos_add_application name)
  cmake_parse_arguments(APP "FULL_CXX"
    "BOARD;CAPABILITIES;CAPABILITIES_OPTIONAL;CAPABILITIES_INBOUND_REPLY;SPI_BACKEND"
    "SOURCES" ${ARGN})
  # A misspelled keyword would otherwise fall through the DEFINED guards below and record NO
  # declaration at all, leaving the app on the undeclared default with nothing said.
  if(APP_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "kickos_add_application(${name}): unrecognised argument(s) "
      "'${APP_UNPARSED_ARGUMENTS}'. Keywords are FULL_CXX, BOARD, SOURCES, CAPABILITIES, "
      "CAPABILITIES_OPTIONAL, CAPABILITIES_INBOUND_REPLY, SPI_BACKEND.")
  endif()
  # cmake_parse_arguments leaves the variable UNDEFINED for a keyword given no value, and the
  # unquoted ${ARGN} above drops an empty value to the same shape, so the DEFINED guards below
  # would silently record the default instead.
  if(APP_KEYWORDS_MISSING_VALUES)
    message(FATAL_ERROR "kickos_add_application(${name}): keyword(s) "
      "'${APP_KEYWORDS_MISSING_VALUES}' given with no value. Give each a non-negative "
      "integer, or omit the keyword to take the default.")
  endif()
  if(NOT APP_SOURCES)
    message(FATAL_ERROR "kickos_add_application(${name}): SOURCES required")
  endif()
  if(NOT APP_BOARD)
    set(APP_BOARD "${KICKOS_BOARD}")
  endif()
  if(NOT APP_BOARD)
    message(FATAL_ERROR "kickos_add_application(${name}): no BOARD given and the "
      "KickOS package records no default board")
  endif()
  kickos_load_board_descriptor("${APP_BOARD}" _arch _chip _family)

  # The installed package was built for one board/arch. Fail clearly rather than
  # letting a missing target degrade to a bare -lkickos_arch_<arch> link error.
  if(NOT TARGET kickos_arch_${_arch})
    message(FATAL_ERROR "kickos_add_application(${name}): BOARD '${APP_BOARD}' "
      "needs arch '${_arch}', but this KickOS package provides no "
      "kickos_arch_${_arch} (it was built for a different board)")
  endif()

  # Optional sugar over the plain path:
  #   add_executable(${name} ...) ; target_link_libraries(${name} PRIVATE kickos)
  # Everything needed to LINK rides the exported `kickos` target, so the two paths
  # produce the same image; what is left here is board validation and the image
  # emission that cannot ride a usage requirement.
  add_executable(${name} ${APP_SOURCES})
  # Only an EXPLICIT declaration is recorded, so the sum's diagnostics can name the app that
  # set the width rather than the default.
  if(DEFINED APP_CAPABILITIES OR DEFINED APP_CAPABILITIES_OPTIONAL
     OR DEFINED APP_CAPABILITIES_INBOUND_REPLY)
    if(NOT DEFINED APP_CAPABILITIES)
      set(APP_CAPABILITIES "${KICKOS_CAP_APP_PEAK_DEFAULT}")
    endif()
    if(NOT DEFINED APP_CAPABILITIES_OPTIONAL)
      set(APP_CAPABILITIES_OPTIONAL 0)
    endif()
    if(NOT DEFINED APP_CAPABILITIES_INBOUND_REPLY)
      set(APP_CAPABILITIES_INBOUND_REPLY "${KICKOS_CAP_REPLY_DEFAULT}")
    endif()
    kickos_declare_app_capabilities(${name}
      "${APP_CAPABILITIES}" "${APP_CAPABILITIES_OPTIONAL}"
      "${APP_CAPABILITIES_INBOUND_REPLY}")
  endif()
  # Warning policy only on our own code: in tree every app under user/apps is ours;
  # out of tree the application target belongs to the consumer.
  if(KICKOS_IN_TREE)
    target_compile_options(${name} PRIVATE ${KICKOS_WARN_FLAGS})
  endif()
  # NB: the app is NOT built -msmall-data-limit=0 under RISC-V PMP. With the gp
  # window anchored inside the .appdata grant (chip .ld), the app's small globals
  # land in that granted window, and keeping small-data enabled is REQUIRED: the
  # flag on a -fexceptions TU hangs __cxa_throw in the FDE walk. Only the KickOS
  # libs get the flag (kickos_apply_freestanding). See docs/design-cxx-under-mpu.md.
  # The OS-agnostic entry glue (-Dmain / -include app.h) rides each leaf's core, so
  # the plain add_executable path gets it too. FULL_CXX picks the full-C++ leaf.
  # Ahead of the posture leaf, so the selected backend's archive precedes the rescan group
  # on the link line and a group member that ever referenced a class symbol would resolve
  # against the executable's own choice rather than pull a second one.
  if(APP_SPI_BACKEND)
    if(NOT TARGET ${APP_SPI_BACKEND})
      message(FATAL_ERROR "kickos_add_application(${name}): SPI_BACKEND='${APP_SPI_BACKEND}' "
        "is not a CMake target. Name a target defining the <kickos/driver/spi.h> class: a "
        "per-chip engine (kickos_spi_xmcssc, kickos_spi_k64dspi) or kickos_spi_proxy.")
    endif()
    target_link_libraries(${name} PRIVATE ${APP_SPI_BACKEND})
  endif()
  if(APP_FULL_CXX)
    target_link_libraries(${name} PRIVATE kickos_cxx)
  else()
    target_link_libraries(${name} PRIVATE kickos)
  endif()
  # MCU: emit flashable .bin/.hex (no-op on the sim).
  kickos_emit_image(${name})
endfunction()

# ---------------------------------------------------------------------------
# kickos_add_diagnostic_app(<name> SOURCES <src...> BOARD <board>)
#   A DIAGNOSTIC (test/bring-up) app, built ONLY when KICKOS_ENABLE_SELFTEST is
#   on, because it depends on the test-only syscall surface (kos_irq_inject,
#   kos_guard_addr, ...) that is deliberately kept OUT of the production ABI, and/or
#   deliberately faults. A production build carries no diagnostic image. Callers
#   should `if(NOT TARGET <name>) return() endif()` before registering its tests so
#   a non-diagnostic build skips them cleanly. Distinct from kickos_add_application,
#   which is for user/demo apps that build on every configuration.
# ---------------------------------------------------------------------------
function(kickos_add_diagnostic_app name)
  if(NOT KICKOS_ENABLE_SELFTEST)
    return()
  endif()
  kickos_add_application(${name} ${ARGN})
endfunction()

# ---------------------------------------------------------------------------
# kickos_add_driver(<name> [SOURCES <src...>] [CLASS <leaf>] [REGDIR <dir>])
#   The one shape of an unprivileged chip/device driver library
#   (kickos_k64dspi, kickos_xmcssc, ...): a freestanding STATIC lib that links
#   kickos_user (user/include + the component group ordered after it), sees
#   system/include (sys.h/abi.h pull errno.h from kickos_system, which
#   kickos_user carries only PRIVATE), optionally sees a chip register dir
#   (REGDIR, e.g. arch/arm/chip/xmc4800 for regs/usic.h, definitions only and no
#   code/kernel headers), optionally links a chip class leaf (CLASS, decision
#   R-A: shared register logic like a FIFO-level read comes from the freestanding
#   kickos_class_<chip> leaf, not a local copy), and is EXPORTED so an out-of-tree
#   consumer (e.g. a KickCAT LAN9252 slave) links it on top of the OS. Its
#   .data/.bss land in .appdata (the lib is outside the closed kernel set, so the
#   chip .ld catch-all captures it, so they are user-reachable). The target is
#   kickos_<name>; SOURCES defaults to <name>.cc.
function(kickos_add_driver name)
  cmake_parse_arguments(DRV "" "CLASS;REGDIR" "SOURCES" ${ARGN})
  if(NOT DRV_SOURCES)
    set(DRV_SOURCES "${name}.cc")
  endif()
  add_library(kickos_${name} STATIC ${DRV_SOURCES})
  kickos_apply_freestanding(kickos_${name})
  target_link_libraries(kickos_${name} PUBLIC kickos_user)
  target_include_directories(kickos_${name} PRIVATE
    "${PROJECT_SOURCE_DIR}/system/include")
  if(DRV_REGDIR)
    target_include_directories(kickos_${name} PRIVATE
      "${PROJECT_SOURCE_DIR}/${DRV_REGDIR}")
  endif()
  if(DRV_CLASS)
    target_link_libraries(kickos_${name} PRIVATE ${DRV_CLASS})
  endif()
  install(TARGETS kickos_${name} EXPORT KickOSTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endfunction()

# ---------------------------------------------------------------------------
# kickos_add_qemu_test(NAME <n> TARGET <app> BOARD <b> SCRIPT <sh>
#                      [MACHINE <m>] [TIMEOUT <s>] [ARGS <arg...>])
#   Register a QEMU boot gate: run SCRIPT against the app's ELF and treat exit 77
#   as SKIP (a missing qemu-system is a skip, not a failure). The QEMU env prefix
#   is keyed on the target board so the copy-pasted `-E env QEMU=... QEMU_MACHINE=
#   ... QEMU_EXTRA=-bios none` string lives in exactly one place (a typo there
#   silently mis-targets QEMU). This is the ONE board -> machine map:
#     qemu       -> mps2-an386  (Cortex-M4F)
#     qemu-m33   -> mps2-an505  (Cortex-M33, PMSAv8)
#     qemu-m7    -> mps2-an500  (Cortex-M7)
#     qemu-m3    -> mps2-an385  (Cortex-M3, soft-float)
#     microbit   -> microbit    (armv6m Cortex-M0)
#     qemu-riscv -> virt, plus QEMU=qemu-system-riscv32 QEMU_EXTRA=-bios none
#                   (RV32IMAC bare-metal in M-mode, no OpenSBI).
#   QEMU_MACHINE is always passed, never left to the scripts' mps2-an386 fallback:
#   most boards would silently run on the wrong core, and check_fault_dump.sh reads
#   an UNSET QEMU_MACHINE as "this is the sim, run natively".
#   MACHINE overrides the board default, for a script that needs a specific image.
#   ARGS are extra script arguments after the ELF (e.g. the expected fault-dump
#   banner, or a decoder path). TIMEOUT defaults to 60s.
#   Keep the per-test `if(KICKOS_BUILD_TESTS AND ...)` guard AT THE CALL SITE: the
#   HAVE_MPU / arch conditions vary per test, so they do NOT belong in the macro.
#   NOT for the sim PASS/FAIL_REGULAR_EXPRESSION tests (a different shape).
function(kickos_add_qemu_test)
  cmake_parse_arguments(QT "" "NAME;TARGET;BOARD;SCRIPT;MACHINE;TIMEOUT" "ARGS" ${ARGN})
  if(NOT QT_NAME OR NOT QT_TARGET OR NOT QT_BOARD OR NOT QT_SCRIPT)
    message(FATAL_ERROR "kickos_add_qemu_test: NAME, TARGET, BOARD and SCRIPT are required")
  endif()
  set(_env "")
  if(QT_BOARD STREQUAL "qemu-riscv")
    set(_env QEMU=qemu-system-riscv32 "QEMU_EXTRA=-bios none")
    set(_machine virt)
  elseif(QT_BOARD STREQUAL "microbit")
    set(_machine microbit)
  elseif(QT_BOARD STREQUAL "qemu")
    set(_machine mps2-an386)
  elseif(QT_BOARD STREQUAL "qemu-m33")
    set(_machine mps2-an505)
  elseif(QT_BOARD STREQUAL "qemu-m7")
    set(_machine mps2-an500)
  elseif(QT_BOARD STREQUAL "qemu-m3")
    set(_machine mps2-an385)
  else()
    message(FATAL_ERROR "kickos_add_qemu_test(${QT_NAME}): unknown BOARD '${QT_BOARD}'")
  endif()
  if(QT_MACHINE)
    set(_machine "${QT_MACHINE}")
  endif()
  list(APPEND _env QEMU_MACHINE=${_machine})
  add_test(NAME "${QT_NAME}"
    COMMAND "${CMAKE_COMMAND}" -E env ${_env}
            "${QT_SCRIPT}" "$<TARGET_FILE:${QT_TARGET}>" ${QT_ARGS})
  if(NOT QT_TIMEOUT)
    set(QT_TIMEOUT 60)
  endif()
  set_tests_properties("${QT_NAME}" PROPERTIES TIMEOUT ${QT_TIMEOUT} SKIP_RETURN_CODE 77)
endfunction()

# ---------------------------------------------------------------------------
# kickos_add_board_provider(<name> SOURCE <cc> [LINK <libs...>] [RETAINED_CAPS <n>]
#                           [INBOUND_REPLY_CAPS <r>])
#   A board-descriptor provider library (pinmap or service-list): a freestanding
#   STATIC lib defining one board-descriptor symbol (kickos_board_pinmap or
#   kickos_board_services), seeing only system/include, exported to KickOSTargets.
#   Folding the install(EXPORT) into the per-provider call removes the separate
#   hand-maintained install(TARGETS ...) list (a drift hazard: add a provider,
#   forget the install). LINK carries a service list's board driver targets (they
#   back-reference kickos_user, so they join the rescan link group); a pure pinmap
#   provider passes none. The target is kickos_<name>.
#
#   RETAINED_CAPS is how many capabilities a SERVICE LIST leaves in root's table for the
#   life of the image, one of the four terms root's table width is summed from
#   (cmake/cap_table.cmake). It is RETENTION, not the bring-up peak: a list whose bring-up
#   transiently holds more than its retention plus the app's peak must declare the
#   transient. A pinmap provider holds none and passes nothing.
#
#   INBOUND_REPLY_CAPS is how many CAP_REPLY capabilities one of the list's SERVICES holds at
#   once as the server side of kos_call: a client mints into the server's table
#   (kernel/syscall/syscall_ipc.cc), so a list whose protocol admits several parked callers
#   must say how many. The widest declaration in the tree wins (the app may declare it too),
#   so this is not added to the app's number.
# Every member of the rescan archive group has to be NAMED, which is the only reason a list
# of them exists. It is derived from the provider's own declared links rather than restated:
# kickos_add_board_provider below already takes LINK, and kickos_add_driver already takes
# CLASS, so the closure walks edges the declarations put there. PRIVATE deps count, which is
# what reaches a driver's class leaf: PRIVATE is excluded from INTERFACE_LINK_LIBRARIES but
# still sits in the target's own LINK_LIBRARIES.
#
# The walk stops at kickos_user. That library and its own dependencies (the arch leaf and
# kickos_lib) are put in the group separately, so descending into it would only re-add them.
function(kickos_service_libs_closure target out)
  set(_seen "")
  set(_queue "${target}")
  while(_queue)
    list(POP_FRONT _queue _t)
    if(NOT TARGET ${_t})
      continue()
    endif()
    if("${_t}" IN_LIST _seen)
      continue()
    endif()
    if("${_t}" STREQUAL "kickos_user")
      continue()
    endif()
    list(APPEND _seen "${_t}")
    get_target_property(_deps "${_t}" LINK_LIBRARIES)
    if(_deps)
      list(APPEND _queue ${_deps})
    endif()
  endwhile()
  set(${out} "${_seen}" PARENT_SCOPE)
endfunction()

function(kickos_add_board_provider name)
  cmake_parse_arguments(BP "" "SOURCE;RETAINED_CAPS;INBOUND_REPLY_CAPS" "LINK" ${ARGN})
  # A misspelled keyword would otherwise be dropped and the count silently default.
  if(BP_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "kickos_add_board_provider(${name}): unrecognised argument(s) "
      "'${BP_UNPARSED_ARGUMENTS}'. Keywords are SOURCE, LINK, RETAINED_CAPS, "
      "INBOUND_REPLY_CAPS.")
  endif()
  # A keyword given no value (or an empty one, which the unquoted ${ARGN} drops) leaves the
  # variable UNDEFINED, so the DEFINED guards below would default it past the numeric check.
  if(BP_KEYWORDS_MISSING_VALUES)
    message(FATAL_ERROR "kickos_add_board_provider(${name}): keyword(s) "
      "'${BP_KEYWORDS_MISSING_VALUES}' given with no value. Give each a value, or omit the "
      "keyword to take the default.")
  endif()
  if(NOT BP_SOURCE)
    message(FATAL_ERROR "kickos_add_board_provider(${name}): SOURCE required")
  endif()
  add_library(kickos_${name} STATIC ${BP_SOURCE})
  if(NOT DEFINED BP_RETAINED_CAPS)
    set(BP_RETAINED_CAPS 0)
  endif()
  if(NOT DEFINED BP_INBOUND_REPLY_CAPS)
    set(BP_INBOUND_REPLY_CAPS "${KICKOS_CAP_REPLY_DEFAULT}")
  endif()
  # Refused HERE, where the declarer is named: the resolve reads these properties numerically
  # and would otherwise take a negative as a real term and narrow the summed width.
  foreach(_n "${BP_RETAINED_CAPS}" "${BP_INBOUND_REPLY_CAPS}")
    if(NOT "${_n}" MATCHES "^[0-9]+$")
      message(FATAL_ERROR "kickos_add_board_provider(${name}): '${_n}' is not a non-negative "
        "integer count of concurrently held capabilities")
    endif()
  endforeach()
  set_target_properties(kickos_${name} PROPERTIES
    KICKOS_CAP_RETAINED "${BP_RETAINED_CAPS}" KICKOS_CAP_REPLY "${BP_INBOUND_REPLY_CAPS}")
  kickos_apply_freestanding(kickos_${name})
  target_include_directories(kickos_${name} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
  if(BP_LINK)
    target_link_libraries(kickos_${name} PUBLIC ${BP_LINK})
  endif()
  install(TARGETS kickos_${name} EXPORT KickOSTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endfunction()

# ---------------------------------------------------------------------------
# kickos_board_names(<out>)
#   The fleet's board names, from the SOLE source of truth: boards/*/board.cmake.
#   Used for the KICKOS_BOARD cache-var help so it can never go stale. Sorted for a stable string.
function(kickos_board_names out)
  file(GLOB _descs "${KICKOS_BOARDS_DIR}/*/board.cmake")
  set(_names "")
  foreach(_d ${_descs})
    get_filename_component(_dir "${_d}" DIRECTORY)
    get_filename_component(_b "${_dir}" NAME)
    list(APPEND _names "${_b}")
  endforeach()
  list(SORT _names)
  set(${out} "${_names}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# kickos_enforcing_mpu_boards(<out>)
#   The boards whose chip enforces memory protection, derived at configure time
#   from the SOLE source of truth, the arch/*/chip/*/mpu.cmake opt-in files
#   (KICKOS_CHIP_ENFORCES_MPU), reverse-mapped to board names via the board
#   descriptors, plus the sim (which enforces at the arch level via host mprotect
#   and ships no chip mpu.cmake). Feeds the "enforcement-capable boards" hint in
#   the KICKOS_HAVE_MPU rejection so that list can never drift from reality. Reads
#   each descriptor in this function's scope so the KICKOS_CHIP/ARCH it sets never
#   leak into the caller's build.
function(kickos_enforcing_mpu_boards out)
  file(GLOB _mpus "${CMAKE_CURRENT_SOURCE_DIR}/arch/*/chip/*/mpu.cmake")
  set(_chips "")
  foreach(_m ${_mpus})
    get_filename_component(_chipdir "${_m}" DIRECTORY)
    get_filename_component(_chip "${_chipdir}" NAME)
    list(APPEND _chips "${_chip}")
  endforeach()
  file(GLOB _descs "${KICKOS_BOARDS_DIR}/*/board.cmake")
  set(_boards "")
  foreach(_d ${_descs})
    get_filename_component(_dir "${_d}" DIRECTORY)
    get_filename_component(_b "${_dir}" NAME)
    set(KICKOS_ARCH "")
    set(KICKOS_CHIP "")
    include("${_d}")
    if(KICKOS_ARCH STREQUAL "sim")
      list(APPEND _boards "${_b}")
    elseif(KICKOS_CHIP AND KICKOS_CHIP IN_LIST _chips)
      list(APPEND _boards "${_b}")
    endif()
  endforeach()
  list(SORT _boards)
  list(JOIN _boards ", " _joined)
  set(${out} "${_joined}" PARENT_SCOPE)
endfunction()
