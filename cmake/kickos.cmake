# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# KickOS build helpers: per-component flag posture, the optional application helper
# kickos_add_application(), and the image emitter kickos_emit_image().
#
# The application owns the final link (architecture.md, invariant #8): KickOS ships static
# libraries, headers and startup. The link recipe lives on the exported `kickos` /
# `kickos_cxx` usage targets, NOT in these helpers, so what is left here is in-tree-only
# policy or an explicit opt-in.

# ---------------------------------------------------------------------------
# Board -> {arch, chip} resolution.
#
# The single source of truth for a board's arch and chip is one descriptor,
# boards/<board>/board.cmake, also included pre-project by the ARM toolchain file, which
# then takes the -mcpu baseline from arch/<family>/chip/<chip>/cpu.cmake unless the
# descriptor overrides it. The sim has no chip (KICKOS_CHIP == "").
#
# KICKOS_BOARDS_DIR is captured at include time: a called function sees the caller's list
# dir, not this file's. An installed package has no boards/ tree, so the fallback below
# applies there.
# ---------------------------------------------------------------------------
get_filename_component(KICKOS_BOARDS_DIR "${CMAKE_CURRENT_LIST_DIR}/../boards" ABSOLUTE)

# List-dir-relative: cap_table.cmake must be installed beside this file.
include("${CMAKE_CURRENT_LIST_DIR}/cap_table.cmake")

# In-tree vs installed-package signal: a source tree has boards/ beside cmake/; an
# installed package ships kickos.cmake with no boards/ sibling.
if(EXISTS "${KICKOS_BOARDS_DIR}")
  set(KICKOS_IN_TREE TRUE)
else()
  set(KICKOS_IN_TREE FALSE)
endif()

# The descriptor also carries KICKOS_ARCH_FAMILY (arm|rx|sim|...): the source-tree family
# that routes arch/<family>/... and the family-specific cross toolchain. A board that omits
# it falls back to a derivation from the arch (armv6m/armv7m -> arm, sim -> sim).
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
    # Installed package: no boards/ tree at all, so fall back to the arch/chip this package
    # recorded (KickOSConfig) for the single board it was built for. Gated on the boards/
    # tree being ABSENT so an in-tree unknown board still errors: in-tree the host toolchain
    # also defines KICKOS_ARCH, which alone would let a typo'd board slip through as the
    # recorded arch.
    set(${out_arch} "${KICKOS_ARCH}" PARENT_SCOPE)
    set(${out_chip} "${KICKOS_CHIP}" PARENT_SCOPE)
  else()
    message(FATAL_ERROR "KickOS: unknown board '${board}' "
      "(no ${_desc}, and it is not the board this package was built for)")
  endif()
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
# Applied per-target so a hosted arch TU and a freestanding kernel TU coexist in one binary.
#
# Warning flags never leave this project: they are applied PRIVATE to targets we own, and
# the exported `kickos`/`kickos_cxx` usage targets carry none of them.
# ---------------------------------------------------------------------------
set(KICKOS_WARN_FLAGS
  -Wall -Wextra -Wshadow -Wundef)

# Warnings-as-errors: default ON in tree, OFF for a consumer. Riding KICKOS_WARN_FLAGS
# keeps it per-target, so it never reaches CMake's try_compile/ABI probes.
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
# that buys: no INSTALLED header may use a C++20 construct, a consumer compiling those in
# their own dialect. tests/static/check_public_headers.sh keeps it true.
set(KICKOS_CXX_STANDARD cxx_std_20)
set(KICKOS_CXX_INTERFACE_STANDARD cxx_std_17)

# freestanding TUs: kernel, lib, user, and the ARM arch backends (C++ + ASM).
function(kickos_apply_freestanding target)
  target_compile_features(${target} PRIVATE ${KICKOS_CXX_STANDARD})
  target_compile_features(${target} INTERFACE ${KICKOS_CXX_INTERFACE_STANDARD})
  target_compile_options(${target} PRIVATE
    ${KICKOS_WARN_FLAGS} ${KICKOS_FREESTANDING_FLAGS}
    "$<$<COMPILE_LANGUAGE:CXX>:${KICKOS_FREESTANDING_CXX_FLAGS}>")
  # RISC-V PMP enforcement: the KickOS-owned libs must emit NO gp-relative small-data, so
  # the single gp window holds only app + C++-runtime small-data and can sit inside the
  # granted .appdata region. -msmall-data-limit=0 routes every KickOS global to ordinary
  # .data/.bss. NOT applied to the app: its -fexceptions TUs need gp-relative small-data or
  # __cxa_throw hangs in the FDE walk (docs/design-cxx-under-mpu.md).
  # check_riscv_no_smalldata.sh asserts the built archives carry zero .sdata/.sbss.
  if(KICKOS_ARCH STREQUAL "rv32imac" AND KICKOS_HAVE_MPU)
    target_compile_options(${target} PRIVATE -msmall-data-limit=0)
  endif()
endfunction()

# kickos_privatise_runtime(<target>)
#   Rewrites every runtime name a compiler EMITS in this archive to the kernel's private one
#   (cmake/kernel_runtime.syms). Kernel text may not call the app's copies, whose pages carry
#   privileged-execute-never once EL0 can reach them (docs/design-m6-mmu.md, T5b).
#
#   An EXPLICIT call is renamed at its call site instead and needs nothing from this; what
#   needs it is the reference with no call in the .cc at all, which `ThreadAttr attr;` emits
#   and no compiler flag suppresses.
#
#   In place, after `ar`, so the archive the linker reads is the rewritten one. An entry
#   matching no symbol is a no-op, so a second run over an already-rewritten archive changes
#   nothing.
#
#   The syms file is NOT a dependency of this command, so adding a name to it re-archives
#   nothing: tests/static/check_kernel_runtime.sh is what turns that into a failure, and it
#   is also what catches an archive this was never called for.
function(kickos_privatise_runtime target)
  if(NOT CMAKE_OBJCOPY)
    message(FATAL_ERROR "kickos_privatise_runtime(${target}): no CMAKE_OBJCOPY. The kernel "
                        "would call the app's memcpy/memset under their ordinary names.")
  endif()
  set(_syms "${PROJECT_SOURCE_DIR}/cmake/kernel_runtime.syms")
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${CMAKE_OBJCOPY}" "--redefine-syms=${_syms}" "$<TARGET_FILE:${target}>"
    COMMENT "kickos: privatising the runtime references in ${target}"
    VERBATIM)
endfunction()

# kickos_split_image_tu(<target> <source>...)
#   Builds the NAMED TUs of an archive holding KERNEL text under the large code model, where
#   a translating backend splits the image in two. Two distinct things force it, and a TU is
#   listed at the call site with which one applies to it.
#
#   REACH. The kernel's half and the app's are 2^40 apart on AArch64 and adrp spans 4 GiB,
#   so a kernel reference to an app symbol truncates at link time
#   (R_AARCH64_ADR_PREL_PG_HI21). The large code model materialises the address as a 64-bit
#   literal in the referencing section, which reaches either half.
#
#   GOT, and this is the load-bearing one. A static link has ONE .got and a GOT slot is
#   reached by adrp like anything else, so a GOT with users in both halves cannot be placed
#   at all; virt_arm64.ld puts it in the app's window. Under the small model a WEAK EXTERN
#   is what produces a kernel-side GOT reference (R_AARCH64_ADR_GOT_PAGE); under the large
#   model none does, leaving the .got to newlib's __libc_fini_array and __call_exitprocs.
#   A GOT user is a blocker whether or not the link happens to succeed today.
#
#   A CALL needs neither: ld inserts a long-branch veneer for an out-of-range
#   R_AARCH64_CALL26 in either direction.
#
#   Per TU and not per target: the flag costs kernel text on a fixed budget and I-cache on
#   the scheduler and syscall paths.
#
#   The list is a MEASUREMENT over the built objects of a fully-small tree and not a reading
#   of the source: whether a reference survives inlining into some other TU is a property of
#   the optimiser. BOTH sweeps are needed, the reach class failing the link loudly while a
#   GOT user is silent:
#     readelf -rW <archive> | grep R_AARCH64_ADR_PREL_PG_HI21   (against an app-half name)
#     readelf -rW <archive> | grep _GOT                          (any hit is a blocker)
#   readelf TRUNCATES the type column, so ADR_GOT_PAGE prints as R_AARCH64_ADR_GOT and a
#   pattern anchored on a trailing underscore matches nothing.
function(kickos_split_image_tu target)
  if(NOT KICKOS_ARCH STREQUAL "armv8a")
    return()
  endif()
  get_target_property(_srcs ${target} SOURCES)
  set(_abs "")
  foreach(_s IN LISTS _srcs)
    get_filename_component(_a "${_s}" ABSOLUTE)
    list(APPEND _abs "${_a}")
  endforeach()
  foreach(_tu IN LISTS ARGN)
    get_filename_component(_a "${_tu}" ABSOLUTE)
    if(NOT _a IN_LIST _abs)
      message(FATAL_ERROR "kickos_split_image_tu(${target}): ${_tu} is not a source of "
                          "that target, so -mcmodel=large would reach nothing.")
    endif()
    set_property(SOURCE "${_tu}" APPEND PROPERTY COMPILE_OPTIONS -mcmodel=large)
  endforeach()
endfunction()

# hosted C++ TUs: the sim arch backend only
function(kickos_apply_hosted target)
  target_compile_features(${target} PRIVATE ${KICKOS_CXX_STANDARD})
  target_compile_features(${target} INTERFACE ${KICKOS_CXX_INTERFACE_STANDARD})
  target_compile_options(${target} PRIVATE
    ${KICKOS_WARN_FLAGS} -fno-exceptions -fno-rtti)
  target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
endfunction()

# The per-chip -mcpu/-mfpu/-mfloat-abi baseline is set globally by the ARM toolchain file
# (CMAKE_<LANG>_FLAGS_INIT) so it applies uniformly to every compile and link (correct
# multilib). Freestanding TUs on ARM therefore need no extra CPU flags here.

# ---------------------------------------------------------------------------
# kickos_emit_image(<target>)
#   MCU only: turn a linked ELF into flashable .bin and .hex, and print size.
#   No-op on the sim (a runnable host ELF is the deliverable there).
#
#   PUBLIC, a POST_BUILD action not being able to ride a usage requirement: one opt-in line
#   after target_link_libraries(app PRIVATE kickos). The esp32 family needs esptool elf2image
#   with per-chip flags (below), and getting them wrong flashes cleanly then reset-loops.
# ---------------------------------------------------------------------------
function(kickos_emit_image target)
  # KICKOS_ARCH is the single source of truth for "is this the sim", recorded in the
  # installed package too, so this also holds for an out-of-tree consumer.
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

  # Espressif chips (Xtensa esp32, RISC-V esp32c6): the raw objcopy .bin is NOT bootable,
  # the ROM loader needing the Espressif image format (magic 0xE9, segment table, checksum)
  # that esptool elf2image builds from the ELF. A missing esptool (the esp-idf env is not
  # active) skips with a message rather than failing the build. --chip is the KickOS chip
  # name, esptool accepting esp32 / esp32c6 verbatim. Prefer `esptool`, esptool.py being
  # deprecated in v5.
  if(KICKOS_CHIP STREQUAL "esp32" OR KICKOS_CHIP STREQUAL "esp32c6")
    find_program(KICKOS_ESPTOOL NAMES esptool esptool.py)
    # Our app IS the image at the ROM bootloader offset (0x1000 on esp32), so the
    # first-stage ROM loads it using the header's flash mode BEFORE any code reconfigures
    # the SPI pins. esptool's elf2image default is QIO, which the esp32 ROM reads unreliably
    # from that position: it loads segment 0, then reads a garbage segment-1 header
    # (`load:0xffffffff,len:-1`) and RTC-WDT reset-loops. Force DIO for esp32. Witnessed on
    # ESP32-D0WD-V3 silicon.
    set(_kos_img_mode "")
    if(KICKOS_CHIP STREQUAL "esp32")
      set(_kos_img_mode --flash_mode dio)
    elseif(KICKOS_CHIP STREQUAL "esp32c6")
      # ESP32-C6: our app is a RAM-only image at flash 0x0 with NO 2nd-stage bootloader, so
      # the RISC-V ROM loader needs --ram-only-header (which implies --dont-append-digest)
      # to boot it. A plain elf2image image is loaded but never entered
      # (`ets_loader.c 67`). DIO for the same reason as esp32: the ROM mis-reads a QIO
      # header from the boot position and "Checksum failure" reset-loops. Witnessed on
      # ESP32-C6-WROOM silicon.
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
      message(STATUS "KickOS: esptool not found: ${target}.app.bin (bootable "
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
#   Links the app against the KickOS component libraries and emits the image. On the sim the
#   entry (host main) lives in the sim arch backend; the app must define kickos_app_main().
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
#   SPI_BACKEND names the CMake target providing this executable's implementation of the SPI
#   class <kickos/driver/spi.h>: a per-chip local engine (kickos_spi_xmcssc,
#   kickos_spi_k64dspi) or the chip-agnostic kickos_spi_proxy. Target-name-valued and
#   fail-loud, like KICKOS_SERVICE_LIST and KICKOS_INIT_PROVIDER. It is PER CONSUMER TARGET
#   and not per build directory, two executables in one tree being free to differ. Exactly
#   ONE backend per executable: they define the same four symbols, which is also why a
#   service driver in the same image keeps its engine copy under private symbols.
#
#   FULL_CXX (opt-in, docs/design-kickcat-k64f.md "Libc strategy"): compile this app's C++
#   TUs with -fexceptions/-frtti (NOT the freestanding clamp) and link the toolchain's
#   libstdc++/libsupc++ over newlib, so exceptions + STL + RTTI work. Off by default. No
#   effect on the sim, already hosted against host libstdc++.
# ---------------------------------------------------------------------------
function(kickos_add_application name)
  cmake_parse_arguments(APP "FULL_CXX"
    "BOARD;CAPABILITIES;CAPABILITIES_OPTIONAL;CAPABILITIES_INBOUND_REPLY;SPI_BACKEND"
    "SOURCES" ${ARGN})
  # A misspelled keyword would otherwise fall through the DEFINED guards below and silently
  # leave the app on the undeclared default.
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

  # Without this a missing target degrades to a bare -lkickos_arch_<arch> link error.
  if(NOT TARGET kickos_arch_${_arch})
    message(FATAL_ERROR "kickos_add_application(${name}): BOARD '${APP_BOARD}' "
      "needs arch '${_arch}', but this KickOS package provides no "
      "kickos_arch_${_arch} (it was built for a different board)")
  endif()

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
  # Warning policy only on our own code: out of tree the application target belongs to the
  # consumer.
  if(KICKOS_IN_TREE)
    target_compile_options(${name} PRIVATE ${KICKOS_WARN_FLAGS})
    # gcc 15 defaults to gnu23, which accepts bool, static_assert, alignas and nullptr, so
    # an unpinned C app stops witnessing the C contract it exists for.
    set_target_properties(${name} PROPERTIES
      C_STANDARD 11
      C_STANDARD_REQUIRED ON
      C_EXTENSIONS OFF)
  endif()
  # The app is NOT built -msmall-data-limit=0 under RISC-V PMP: the gp window is anchored
  # inside the .appdata grant (chip .ld), and the flag on a -fexceptions TU hangs
  # __cxa_throw in the FDE walk (docs/design-cxx-under-mpu.md).
  #
  # The backend goes ahead of the posture leaf, so its archive precedes the rescan group on
  # the link line and a group member that ever referenced a class symbol resolves against
  # this executable's own choice rather than pulling a second one.
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
  kickos_emit_image(${name})
endfunction()

# ---------------------------------------------------------------------------
# kickos_add_diagnostic_app(<name> SOURCES <src...> BOARD <board>)
#   A DIAGNOSTIC (test/bring-up) app, built ONLY when KICKOS_ENABLE_SELFTEST is on: it
#   depends on the test-only syscall surface (kos_irq_inject, kos_guard_addr, ...) kept OUT
#   of the production ABI, and/or deliberately faults. Callers should
#   `if(NOT TARGET <name>) return() endif()` before registering its tests so a
#   non-diagnostic build skips them cleanly. It returns SILENTLY, so a caller that wants the
#   operator told states the requirement itself.
# ---------------------------------------------------------------------------
function(kickos_add_diagnostic_app name)
  if(NOT KICKOS_ENABLE_SELFTEST)
    return()
  endif()
  kickos_add_application(${name} ${ARGN})
endfunction()

# ---------------------------------------------------------------------------
# kickos_add_driver(<name> [SOURCES <src...>] [CLASS <leaf>] [REGDIR <dir>])
#   The one shape of an unprivileged chip/device driver library (kickos_k64dspi,
#   kickos_xmcssc, ...): a freestanding STATIC lib that links kickos_user, sees
#   system/include (sys.h/abi.h pull errno.h from kickos_system, which kickos_user carries
#   only PRIVATE), optionally sees a chip register dir (REGDIR, e.g. arch/arm/chip/xmc4800
#   for regs/usic.h, definitions only and no code or kernel headers), optionally links a
#   chip class leaf (CLASS, shared register logic coming from the freestanding
#   kickos_class_<chip> leaf and not a local copy), and is EXPORTED so an out-of-tree
#   consumer links it on top of the OS. Its .data/.bss land in .appdata, the lib being
#   outside the closed kernel set that the chip .ld catch-all excludes, so they are
#   user-reachable. The target is kickos_<name>; SOURCES defaults to <name>.cc.
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
#   Register a QEMU boot gate: run SCRIPT against the app's ELF and treat exit 77 as SKIP (a
#   missing qemu-system is a skip, not a failure). This is the ONE board -> machine map:
#     qemu       -> mps2-an386  (Cortex-M4F)
#     qemu-m33   -> mps2-an505  (Cortex-M33, PMSAv8)
#     qemu-m7    -> mps2-an500  (Cortex-M7)
#     qemu-m3    -> mps2-an385  (Cortex-M3, soft-float)
#     microbit   -> microbit    (armv6m Cortex-M0)
#     qemu-riscv -> virt, plus QEMU=qemu-system-riscv32 QEMU_EXTRA=-bios none
#                   (RV32IMAC bare-metal in M-mode, no OpenSBI).
#     qemu-arm64 -> virt, plus QEMU=qemu-system-aarch64 QEMU_EXTRA=-cpu cortex-a53 -nic none
#                   (AArch64 bare metal at EL1).
#   QEMU_MACHINE is always passed, never left to the scripts' mps2-an386 fallback:
#   most boards would silently run on the wrong core, and check_fault_dump.sh reads
#   an UNSET QEMU_MACHINE as "this is the sim, run natively".
#   MACHINE overrides the board default, for a script that needs a specific image.
#   ARGS are extra script arguments after the ELF (e.g. the expected fault-dump banner, or a
#   decoder path). TIMEOUT defaults to 60s.
#   Keep the per-test `if(KICKOS_BUILD_TESTS AND ...)` guard AT THE CALL SITE: the HAVE_MPU
#   and arch conditions vary per test.
function(kickos_add_qemu_test)
  cmake_parse_arguments(QT "" "NAME;TARGET;BOARD;SCRIPT;MACHINE;TIMEOUT" "ARGS" ${ARGN})
  if(NOT QT_NAME OR NOT QT_TARGET OR NOT QT_BOARD OR NOT QT_SCRIPT)
    message(FATAL_ERROR "kickos_add_qemu_test: NAME, TARGET, BOARD and SCRIPT are required")
  endif()
  set(_env "")
  if(QT_BOARD STREQUAL "qemu-riscv")
    set(_env QEMU=qemu-system-riscv32 "QEMU_EXTRA=-bios none")
    set(_machine virt)
  elseif(QT_BOARD STREQUAL "qemu-arm64")
    # -cpu is MANDATORY and not a default worth relying on: qemu-system-aarch64 -M virt
    # comes up as a cortex-a15 and REFUSES an A64 image outright. `-bios none` is the
    # RISC-V line's need and errors here, there being no firmware to suppress.
    # -nic none drops the virt machine's default virtio-net-pci, whose option ROM ships in a
    # separate distro package QEMU aborts without.
    set(_env QEMU=qemu-system-aarch64 "QEMU_EXTRA=-cpu cortex-a53 -nic none")
    set(_machine virt)
  elseif(QT_BOARD STREQUAL "microbit")
    # 32 KiB and not the 16 the machine defaults to. QEMU's nRF51 SoC exposes the size as a
    # QOM property and nothing else does: -m is ignored by a fixed-SoC machine, and an image
    # linked for 32 KiB without this locks up on its first push, before any vector table is
    # live, as "can't escalate 3 to HardFault". arch/arm/chip/nrf51/nrf51.ld carries why the
    # board takes the larger part.
    set(_env QEMU_EXTRA=-global\ nrf51-soc.sram-size=32768)
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
# kickos_add_unit_test(NAME <target> SOURCES <cc...> [INCLUDES <dirs...>]
#                     [DEFINITIONS <defs...>] [LIBRARIES <libs...>])
#   One host unit-test executable on GoogleTest, with PER-CASE ctest entries.
#
#   gtest_discover_tests writes its add_test calls at BUILD time, so the root CMakeLists
#   wrapper that appends the build fixture never sees them: both the `host` label and
#   FIXTURES_REQUIRED have to be passed through PROPERTIES here. On a tree that was
#   configured and never built, the generated include registers a failing <target>_NOT_BUILT
#   entry instead of silently shrinking the suite.
function(kickos_add_unit_test)
  cmake_parse_arguments(UT "" "NAME" "SOURCES;INCLUDES;DEFINITIONS;LIBRARIES" ${ARGN})
  if(NOT UT_NAME OR NOT UT_SOURCES)
    message(FATAL_ERROR "kickos_add_unit_test: NAME and SOURCES are required")
  endif()
  add_executable(${UT_NAME} ${UT_SOURCES})
  target_link_libraries(${UT_NAME} PRIVATE ${UT_LIBRARIES} GTest::gtest_main)
  target_compile_features(${UT_NAME} PRIVATE ${KICKOS_CXX_STANDARD})
  target_compile_options(${UT_NAME} PRIVATE ${KICKOS_WARN_FLAGS})
  if(UT_INCLUDES)
    target_include_directories(${UT_NAME} PRIVATE ${UT_INCLUDES})
  endif()
  if(UT_DEFINITIONS)
    target_compile_definitions(${UT_NAME} PRIVATE ${UT_DEFINITIONS})
  endif()
  kickos_discover_unit_tests(${UT_NAME})
endfunction()

# kickos_discover_unit_tests(<target>)
#   The registration half alone, for a gate that builds its own executable.
function(kickos_discover_unit_tests target)
  gtest_discover_tests(${target}
    PROPERTIES TIMEOUT 30 LABELS host FIXTURES_REQUIRED kickos_build
    DISCOVERY_TIMEOUT 60)
endfunction()

# ---------------------------------------------------------------------------
# kickos_add_board_provider(<name> SOURCE <cc> [LINK <libs...>] [RETAINED_CAPS <n>]
#                           [INBOUND_REPLY_CAPS <r>])
#   A board-descriptor provider library (pinmap or service-list): a freestanding STATIC lib
#   defining one board-descriptor symbol (kickos_board_pinmap or kickos_board_services),
#   seeing only system/include, exported to KickOSTargets. LINK carries a service list's
#   board driver targets, which back-reference kickos_user and so join the rescan link
#   group; a pure pinmap provider passes none. The target is kickos_<name>.
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
# Every member of the rescan archive group has to be NAMED. The closure walks the edges the
# LINK and CLASS declarations put there; PRIVATE deps count, which is what reaches a driver's
# class leaf: PRIVATE is excluded from INTERFACE_LINK_LIBRARIES but still sits in the target's
# own LINK_LIBRARIES.
#
# The walk stops at kickos_user. That library and its own dependencies (the arch leaf and
# kickos_lib) are put in the group separately.
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
  # Refused HERE, where the declarer is named: the resolve reads these properties
  # numerically and would otherwise take a negative as a real term and narrow the summed
  # width.
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
#   The fleet's board names, from the SOLE source of truth: boards/*/board.cmake. Feeds the
#   KICKOS_BOARD cache-var help. A cross build never sees that help: the toolchain file
#   creates the cache entry pre-project(), and CMake keeps an existing entry's docstring.
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
#   The boards whose chip enforces memory protection, derived at configure time from the
#   SOLE source of truth, the arch/*/chip/*/mpu.cmake opt-in files
#   (KICKOS_CHIP_ENFORCES_MPU), reverse-mapped to board names via the board descriptors,
#   plus the sim, which enforces at the arch level via host mprotect and ships no chip
#   mpu.cmake. Feeds the "enforcement-capable boards" hint in the KICKOS_HAVE_MPU rejection.
#   Reads each descriptor in this function's scope so the KICKOS_CHIP/ARCH it sets never
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
