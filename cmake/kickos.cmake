# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# KickOS build helpers: per-component flag posture, the app target kind
# (kickos_add_app_target) and the image emitter kickos_emit_image().
#
# The application owns the final link: the link recipe lives on the exported `kickos` /
# `kickos_cxx` usage targets, never in these helpers. An app is three lines,
# examples/oot-mcu-app being the reference shape.

# ---------------------------------------------------------------------------
# Board -> {arch, chip} resolution.
#
# A board's arch and chip come from one descriptor, boards/<board>/board.cmake, also included
# pre-project by the cross toolchain file. The sim has no chip (KICKOS_CHIP == "").
#
# KICKOS_BOARDS_DIR is captured at include time: a called function sees the caller's list
# dir, not this file's. An installed package has no boards/ tree, hence the fallback below.
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

# KICKOS_ARCH_FAMILY (arm|rx|sim|...) routes arch/<family>/... and the cross toolchain; a
# board that omits it falls back to a derivation from the arch.
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
    # Installed package: no boards/ tree, so fall back to the arch/chip this package recorded
    # for the single board it was built for. Gated on the boards/ tree being ABSENT so an
    # in-tree typo'd board still errors: in-tree the host toolchain also defines KICKOS_ARCH.
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

# Applied PRIVATE, so the C++20 level does not ride out through KickOSTargets.cmake and
# compile a consumer's C++17 codebase as C++20. No INSTALLED header may use a C++20
# construct; tests/static/check_public_headers.sh keeps that true.
set(KICKOS_CXX_STANDARD cxx_std_20)
set(KICKOS_CXX_INTERFACE_STANDARD cxx_std_17)

# freestanding TUs: kernel, lib, user, and the ARM arch backends (C++ + ASM).
function(kickos_apply_freestanding target)
  target_compile_features(${target} PRIVATE ${KICKOS_CXX_STANDARD})
  target_compile_features(${target} INTERFACE ${KICKOS_CXX_INTERFACE_STANDARD})
  target_compile_options(${target} PRIVATE
    ${KICKOS_WARN_FLAGS} ${KICKOS_FREESTANDING_FLAGS}
    "$<$<COMPILE_LANGUAGE:CXX>:${KICKOS_FREESTANDING_CXX_FLAGS}>")
  # RISC-V: the KickOS-owned libs must emit NO gp-relative small-data, so the single gp
  # window holds only app and C++-runtime small-data. The app keeps its own: a -fexceptions
  # TU built -msmall-data-limit=0 hangs __cxa_throw in the FDE walk.
  #
  # The flag keeps kernel DATA out of .sdata/.sbss; a kernel access can still RESOLVE through
  # gp, the linker making gp addressing out of any upper/lower pair landing within
  # gp +/- 0x800. gp is a register an unprivileged thread writes, so
  # arch/riscv/rv64imac/switch.S re-anchors it twice per trap, at .Ltrap_regs and .Lrestore.
  # check_riscv_no_smalldata.sh reads the archives; gp addressing exists only after link, so
  # check_riscv_kernel_gp.sh reads the linked image.
  if((KICKOS_ARCH STREQUAL "rv32imac" AND KICKOS_HAVE_MPU)
     OR (KICKOS_ARCH STREQUAL "rv64imac" AND KICKOS_HAVE_ASPACE))
    target_compile_options(${target} PRIVATE -msmall-data-limit=0)
  endif()
endfunction()

# kickos_privatise_runtime(<target>)
#   Rewrites every runtime name a compiler EMITS in this archive to the kernel's private one
#   (cmake/kernel_runtime.syms). Kernel text may not call the app's copies, whose pages carry
#   privileged-execute-never once EL0 can reach them.
#
#   The syms file is NOT a dependency of this command, so adding a name to it re-archives
#   nothing: tests/static/check_kernel_runtime.sh is what turns that into a failure.
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
  # A per-arch map beside the fleet-wide one; an arch with no such file adds nothing.
  set(_arch_syms "${PROJECT_SOURCE_DIR}/cmake/kernel_runtime_${KICKOS_ARCH}.syms")
  if(EXISTS "${_arch_syms}")
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND "${CMAKE_OBJCOPY}" "--redefine-syms=${_arch_syms}" "$<TARGET_FILE:${target}>"
      COMMENT "kickos: privatising the ${KICKOS_ARCH} runtime references in ${target}"
      VERBATIM)
  endif()
endfunction()

# kickos_split_image_tu(<target> <source>...)
#   Builds the NAMED TUs of an archive holding KERNEL text under the large code model, where
#   a translating backend splits the image in two. Inert where KICKOS_SPLIT_IMAGE_CODE_MODEL
#   is empty.
#
#   Two things put a TU on a call site's list. REACH: the two halves are 2^40 apart on
#   AArch64 and adrp spans 4 GiB, so a kernel reference to an app symbol truncates at link
#   time (R_AARCH64_ADR_PREL_PG_HI21). GOT: a static link has ONE .got, virt_arm64.ld puts it
#   in the app's window, and a kernel-side GOT user is a blocker whether or not the link
#   happens to succeed. A CALL needs neither, ld inserting a long-branch veneer.
#
#   The list is a MEASUREMENT over the built objects of a fully-small tree, both sweeps being
#   needed because the reach class fails the link loudly while a GOT user is silent:
#     readelf -rW <archive> | grep R_AARCH64_ADR_PREL_PG_HI21   (against an app-half name)
#     readelf -rW <archive> | grep _GOT                          (any hit is a blocker)
#   readelf TRUNCATES the type column, so ADR_GOT_PAGE prints as R_AARCH64_ADR_GOT and a
#   pattern anchored on a trailing underscore matches nothing.
function(kickos_split_image_tu target)
  if(NOT KICKOS_SPLIT_IMAGE_CODE_MODEL)
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
    set_property(SOURCE "${_tu}" APPEND PROPERTY COMPILE_OPTIONS
                 ${KICKOS_SPLIT_IMAGE_CODE_MODEL})
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

# The per-chip -mcpu/-mfpu/-mfloat-abi baseline comes from the toolchain file's
# CMAKE_<LANG>_FLAGS_INIT, so no CPU flag belongs here.

# ---------------------------------------------------------------------------
# kickos_emit_image(<target>)
#   MCU only: turn a linked ELF into flashable .bin and .hex, and print size.
#   No-op on the sim (a runnable host ELF is the deliverable there).
#
#   PUBLIC: a POST_BUILD action cannot ride a usage requirement, so it is one opt-in line
#   after target_link_libraries(app PRIVATE kickos).
# ---------------------------------------------------------------------------
function(kickos_emit_image target)
  if(KICKOS_ARCH STREQUAL "sim")
    return()
  endif()
  # x86_64: the deliverable IS the image, and writing it is this step. CMake cannot drive
  # `ld -m i386pep` as a linker for a target, so cmake/x86_64_boot.cmake writes the PE32+ UEFI
  # application from the app's objects with a custom command; there is no ELF here to objcopy,
  # and the target is an OBJECT library that $<TARGET_FILE:> may not name at all.
  if(KICKOS_ARCH STREQUAL "x86_64")
    if(NOT COMMAND kickos_x86_64_link_image)
      message(FATAL_ERROR "kickos_emit_image(${target}): x86_64 needs "
        "kickos_x86_64_link_image, which cmake/x86_64_boot.cmake defines. Include that "
        "fragment before add_subdirectory(user/apps).")
    endif()
    kickos_x86_64_link_image(${target})
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

  # Espressif chips (Xtensa esp32, RISC-V esp32c6): the raw objcopy .bin is NOT bootable, the
  # ROM loader needing the Espressif image format that esptool elf2image builds from the ELF.
  # A missing esptool skips with a message rather than failing the build. Prefer `esptool`,
  # esptool.py being deprecated in v5.
  if(KICKOS_CHIP STREQUAL "esp32" OR KICKOS_CHIP STREQUAL "esp32c6")
    find_program(KICKOS_ESPTOOL NAMES esptool esptool.py)
    # Our app IS the image at the ROM bootloader offset (0x1000 on esp32), so the
    # first-stage ROM loads it using the header's flash mode BEFORE any code reconfigures
    # the SPI pins. esptool's elf2image default is QIO, which the esp32 ROM reads unreliably
    # from that position: it loads segment 0, then reads a garbage segment-1 header
    # (`load:0xffffffff,len:-1`) and RTC-WDT reset-loops. Force DIO for esp32.
    set(_kos_img_mode "")
    if(KICKOS_CHIP STREQUAL "esp32")
      set(_kos_img_mode --flash_mode dio)
    elseif(KICKOS_CHIP STREQUAL "esp32c6")
      # ESP32-C6: our app is a RAM-only image at flash 0x0 with NO 2nd-stage bootloader, so
      # the RISC-V ROM loader needs --ram-only-header (which implies --dont-append-digest)
      # to boot it. A plain elf2image image is loaded but never entered
      # (`ets_loader.c 67`). DIO for the same reason as esp32: the ROM mis-reads a QIO
      # header from the boot position and "Checksum failure" reset-loops.
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
# kickos_select_class_backend(<class> <target>)
#   Which backend answers a DRIVER CLASS (<kickos/driver/*.h>) in this image. Every backend of
#   one class defines the same public kos_<class>_* symbols, so an image has exactly one, and
#   which one is a property of the IMAGE POSTURE the board and its service list chose, never of
#   an application: a client body is identical over a proxy and over a local engine, which is
#   the substitution property the class exists for.
#
#   The selection is a GLOBAL property rather than a variable because the deciding site
#   (system/CMakeLists.txt in tree, KickOSConfig.cmake in a package) is in a different
#   directory scope from the apps that consume it.
#
#   The BACKEND MUST PRECEDE `kickos` ON THE LINK LINE: the toolchains link the component
#   archives with a --start-group rescan, so a group member that ever referenced a class symbol
#   would otherwise pull a second definer out of the group and the ORDER, not the selection,
#   would decide the engine. That ordering is kickos_add_app_target's to hold, which is why no
#   app states it.
function(kickos_select_class_backend class target)
  string(TOUPPER "${class}" _cls)
  set_property(GLOBAL PROPERTY KICKOS_CLASS_BACKEND_${_cls} "${target}")
  set_property(GLOBAL APPEND PROPERTY KICKOS_CLASS_BACKEND_CLASSES "${class}")
endfunction()

# kickos_class_backend(<class> <out>)
#   The selected backend, or the empty string where the class has none in this image. TOTAL:
#   a caller asks and reads the answer, and a board with no such class is not a special case.
function(kickos_class_backend class out)
  string(TOUPPER "${class}" _cls)
  get_property(_sel GLOBAL PROPERTY KICKOS_CLASS_BACKEND_${_cls})
  set(${out} "${_sel}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# kickos_add_app_target(<name> <source>...)
#   The app target itself, and nothing else about the app: an executable everywhere but
#   x86_64, where what firmware loads is a PE32+ UEFI application CMake cannot drive a linker
#   for, so the target is an OBJECT library and cmake/x86_64_boot.cmake writes the image out of
#   its objects (kickos_emit_image). An OBJECT library, so the target_compile_definitions and
#   friends a call site applies still bind.
#
#   CLASSES names the DRIVER CLASSES the app's own sources call (spi, i2c, ...), which is the
#   one thing about a class an application knows: it is in its #include list. WHICH backend
#   answers each, and its position on the link line, are the image posture's and are read here
#   from kickos_select_class_backend. An app that names no class links no backend, which is
#   what keeps a mock-carrying image (the selftest) free of a second definer.
#
#   The remaining two lines of an app are the consumer's own and are NOT done here:
#
#     kickos_add_app_target(foo main.cc)
#     target_link_libraries(foo PRIVATE kickos)   # or kickos_cxx for a full-C++ app
#     kickos_emit_image(foo)
#
#   The in-tree warning and C-standard posture applied below belongs to this tree and not to
#   the app, which is why it is here rather than repeated in every app file; out of tree the
#   application target belongs to the consumer and gets neither.
# ---------------------------------------------------------------------------
function(kickos_add_app_target name)
  cmake_parse_arguments(APP "" "" "CLASSES" ${ARGN})
  if(NOT APP_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "kickos_add_app_target(${name}): no sources")
  endif()
  set(_app_sources ${APP_UNPARSED_ARGUMENTS})
  # Without this a missing arch leaf degrades to a bare -lkickos_arch_<arch> link error.
  if(NOT TARGET kickos_arch_${KICKOS_ARCH})
    message(FATAL_ERROR "kickos_add_app_target(${name}): board '${KICKOS_BOARD}' needs arch "
      "'${KICKOS_ARCH}', but this KickOS package provides no kickos_arch_${KICKOS_ARCH} (it "
      "was built for a different board)")
  endif()
  if(KICKOS_ARCH STREQUAL "x86_64")
    add_library(${name} OBJECT ${_app_sources})
  else()
    add_executable(${name} ${_app_sources})
  endif()
  # Linked HERE, before the app's own `kickos` line, which is what puts the backend archive
  # ahead of the rescan group; see kickos_select_class_backend.
  foreach(_class IN LISTS APP_CLASSES)
    kickos_class_backend("${_class}" _backend)
    if(NOT _backend)
      message(FATAL_ERROR "kickos_add_app_target(${name}): no backend of the '${_class}' class "
        "is selected for board '${KICKOS_BOARD}'. A board whose chip has no such block cannot "
        "host this app; kickos_select_class_backend names the backend where it can.")
    endif()
    target_link_libraries(${name} PRIVATE ${_backend})
    # Recorded so a gate reading this image's definitions can inventory the backend it
    # really linked; tests/integration/gates/selftest.cmake reads it.
    set_property(TARGET ${name} APPEND PROPERTY KICKOS_APP_CLASS_BACKENDS "${_backend}")
  endforeach()
  if(KICKOS_IN_TREE)
    target_compile_options(${name} PRIVATE ${KICKOS_WARN_FLAGS})
    # gcc 15 defaults to gnu23, which accepts bool, static_assert, alignas and nullptr, so
    # an unpinned C app stops witnessing the C contract it exists for.
    set_target_properties(${name} PROPERTIES
      C_STANDARD 11
      C_STANDARD_REQUIRED ON
      C_EXTENSIONS OFF)
  endif()
endfunction()

# ---------------------------------------------------------------------------
# kickos_add_driver(<name> [SOURCES <src...>] [CLASS <leaf>] [REGDIR <dir>])
#   The one shape of an unprivileged chip/device driver library: a freestanding STATIC lib
#   that links kickos_user, sees system/include, optionally sees a chip register dir (REGDIR,
#   definitions only), optionally links a chip class leaf (CLASS), and is EXPORTED so an
#   out-of-tree consumer links it on top of the OS. Its .data/.bss land in .appdata, the lib
#   being outside the closed kernel set the chip .ld catch-all excludes. The target is
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
# kickos_is_board(<board> <out>)
#   Whether the name names a board at all, which is a different question from whether that
#   board has an emulator. In tree the descriptor is the authority; an installed package ships
#   no boards/ tree, so the one board it was built for is the only name it can vouch for.
# ---------------------------------------------------------------------------
function(kickos_is_board board out)
  if(EXISTS "${KICKOS_BOARDS_DIR}/${board}/board.cmake")
    set(${out} TRUE PARENT_SCOPE)
  elseif(NOT KICKOS_IN_TREE AND board STREQUAL "${KICKOS_BOARD}")
    set(${out} TRUE PARENT_SCOPE)
  else()
    set(${out} FALSE PARENT_SCOPE)
  endif()
endfunction()

# ---------------------------------------------------------------------------
# kickos_qemu_machine(<board> <out_env> <out_machine>)
#   This is the ONE board -> machine map, and the sole answer to "can this board be booted in
#   this environment": out_machine comes back EMPTY for a board with no emulator, and the
#   configure stops for a name that is no board.
#     qemu       -> mps2-an386  (Cortex-M4F)
#     qemu-m33   -> mps2-an505  (Cortex-M33, PMSAv8)
#     qemu-m7    -> mps2-an500  (Cortex-M7)
#     qemu-m3    -> mps2-an385  (Cortex-M3, soft-float)
#     microbit   -> microbit    (armv6m Cortex-M0)
#     qemu-riscv -> virt, plus QEMU=qemu-system-riscv32 QEMU_EXTRA=-bios none
#                   (RV32IMAC bare-metal in M-mode, no OpenSBI).
#     qemu-riscv64 -> virt, plus QEMU=qemu-system-riscv64 QEMU_EXTRA=-bios none
#                   (RV64IMAC bare metal, no OpenSBI).
#     qemu-arm64 -> virt, plus QEMU=qemu-system-aarch64 QEMU_EXTRA=-cpu cortex-a53 -nic none
#                   (AArch64 bare metal at EL1), and virt,gic-version=3 under that posture.
#     qemu-x86_64 -> q35, plus QEMU=qemu-system-x86_64 and KICKOS_BOOT=uefi-pe, which is
#                   what makes gate.sh build an EFI system partition and boot OVMF instead
#                   of passing -kernel (the image is a PE32+ application, which -kernel
#                   cannot start at all).
#
#   A caller that has something to SAY about the absence (an operator told to flash and
#   capture instead) reads the same answer here rather than keeping a board list of its own.
# ---------------------------------------------------------------------------
function(kickos_qemu_machine board out_env out_machine)
  set(_env "")
  if(board STREQUAL "qemu-riscv")
    set(_env QEMU=qemu-system-riscv32 "QEMU_EXTRA=-bios none")
    set(_machine virt)
  elseif(board STREQUAL "qemu-riscv64")
    # No -cpu: qemu-system-riscv64 -M virt defaults to the `rv64` generic core.
    # -smp is what MAKES the harts exist, and with no firmware every one of them enters _start:
    # the park in startup.S is what holds all but the boot hart there.
    set(_smp "")
    if(KICKOS_NUM_CORES GREATER 1)
      set(_smp " -smp ${KICKOS_NUM_CORES}")
    endif()
    set(_env QEMU=qemu-system-riscv64 "QEMU_EXTRA=-bios none${_smp}")
    set(_machine virt)
  elseif(board STREQUAL "qemu-arm64")
    # -cpu is required: qemu-system-aarch64 -M virt comes up as a cortex-a15 and REFUSES an
    # A64 image. `-bios none` errors here, there being no firmware to suppress. -nic none
    # drops the default virtio-net-pci, whose option ROM ships in a separate distro package
    # QEMU aborts without.
    # -smp is what MAKES the cores exist: PSCI CPU_ON answers INVALID_PARAMETERS for a core
    # the machine was not given, so a multi-core image on a one-core machine refuses at boot.
    # Under one image per node the machine geometry is the PARTITION's: sized from
    # KICKOS_NUM_CORES instead, the machine would have one CPU for a PSCI CPU_ON to fail on and
    # its DRAM would end below the region every node writes.
    set(_smp "")
    set(_mem "")
    if(KICKOS_AMP_NODE AND KICKOS_AMP_OWN_IMAGE)
      set(_smp " -smp ${KICKOS_AMP_PARTITION_CORES}")
      math(EXPR _part_mib
           "(${KICKOS_AMP_NODES} * ${KICKOS_AMP_NODE_SHARE} + ${KICKOS_AMP_SHARED_SIZE} + 1048575) / 1048576")
      set(_mem " -m ${_part_mib}M")
    elseif(KICKOS_NUM_CORES GREATER 1)
      set(_smp " -smp ${KICKOS_NUM_CORES}")
    endif()
    set(_env QEMU=qemu-system-aarch64 "QEMU_EXTRA=-cpu cortex-a53 -nic none${_smp}${_mem}")
    # -M virt defaults to a GICv2, so the GICv3 posture has to ask for the model it is built
    # against: an image whose CPU interface is the ICC_* registers finds none on a GICv2
    # machine and traps on the first access.
    set(_machine virt)
    if(KICKOS_ARM64_GIC_VERSION EQUAL 3)
      set(_machine "virt,gic-version=3")
    endif()
  elseif(board STREQUAL "imx8mp-evk")
    # No -cpu and no gic-version: the machine fixes both, being a model of a die rather than a
    # configurable board.
    # -m bounds the machine's DDR window, which defaults to the EVK's 6 GiB and is mapped
    # lazily; the linker script carves 64 MiB of it, so this is headroom rather than a fit.
    set(_env QEMU=qemu-system-aarch64 QEMU_EXTRA=-m\ 512M)
    set(_machine imx8mp-evk)
  elseif(board STREQUAL "microbit")
    # QEMU's nRF51 SoC exposes SRAM size as a QOM property and -m is ignored by a fixed-SoC
    # machine, so an image linked for the chip's real SRAM without this locks up on its first
    # push, before any vector table is live, as "can't escalate 3 to HardFault". The figure is
    # scraped from nrf51.ld's own RAM LENGTH, in bytes, so it cannot drift from what the image
    # was linked for.
    set(_nrf51_ld "${CMAKE_SOURCE_DIR}/arch/arm/chip/nrf51/nrf51.ld")
    file(STRINGS "${_nrf51_ld}" _nrf51_ram_line REGEX "^[ \t]*RAM[ \t]*\\(rwx\\)")
    if(_nrf51_ram_line STREQUAL "")
      message(FATAL_ERROR "kickos_qemu_machine: ${_nrf51_ld} names no 'RAM (rwx)' region to "
        "scrape the microbit QEMU sram-size from")
    endif()
    # The trailing anchor requires LENGTH's value to be the last thing on the line, so a K/M
    # suffix fails this match rather than being read as its digits alone ("32K" as 32 bytes).
    if(NOT _nrf51_ram_line MATCHES "LENGTH[ \t]*=[ \t]*[0-9]+[ \t]*$")
      message(FATAL_ERROR "kickos_qemu_machine: could not read a trailing plain decimal RAM "
        "LENGTH (no K/M suffix) out of '${_nrf51_ram_line}' in ${_nrf51_ld}")
    endif()
    string(REGEX REPLACE ".*LENGTH[ \t]*=[ \t]*([0-9]+)[ \t]*$" "\\1" _nrf51_sram_bytes "${_nrf51_ram_line}")
    set(_env QEMU_EXTRA=-global\ nrf51-soc.sram-size=${_nrf51_sram_bytes})
    set(_machine microbit)
  elseif(board STREQUAL "qemu")
    set(_machine mps2-an386)
  elseif(board STREQUAL "qemu-m33")
    set(_machine mps2-an505)
  elseif(board STREQUAL "qemu-m7")
    set(_machine mps2-an500)
  elseif(board STREQUAL "qemu-m3")
    set(_machine mps2-an385)
  elseif(board STREQUAL "qemu-x86_64")
    # tests/lib/gate.sh builds the firmware, the writable variable store and the EFI system
    # partition per run: the shipped OVMF variable store is root-owned, and an ESP has to be
    # made from the image under test or a stale BOOTX64.EFI boots and prints the same banner.
    set(_env QEMU=qemu-system-x86_64 KICKOS_BOOT=uefi-pe)
    set(_machine q35)
  else()
    # No emulator for this board. NOT the caller's problem, so the answer is an empty machine
    # rather than a refusal: an app gate reads as "this gate rides this target" with no board
    # predicate wrapped round it. A name that is no board AT ALL still stops the configure,
    # so a typo cannot vanish into that silence.
    kickos_is_board("${board}" _known)
    if(NOT _known)
      message(FATAL_ERROR "kickos_qemu_machine: '${board}' names no board (no "
        "${KICKOS_BOARDS_DIR}/${board}/board.cmake, and it is not the board this package was "
        "built for)")
    endif()
    # BOTH out-parameters, or a caller looping over boards keeps the previous board's answer.
    set(${out_env} "" PARENT_SCOPE)
    set(${out_machine} "" PARENT_SCOPE)
    return()
  endif()
  set(${out_env} "${_env}" PARENT_SCOPE)
  set(${out_machine} "${_machine}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# kickos_add_qemu_test([NAME <n>] TARGET <app> [BOARD <b>] SCRIPT <sh>
#                      [MACHINE <m>] [TIMEOUT <s>] [ARGS <arg...>])
#   Register a QEMU boot gate: run SCRIPT against the app's ELF and treat exit 77 as SKIP (a
#   missing qemu-system is a skip, not a failure).
#
#   TOTAL over the fleet: a board with no emulator registers nothing and says nothing, so a
#   call site owes no board predicate. What a call site still owes is any condition that is
#   about the CLAIM rather than about the emulator: an arch, an MPU posture, a core count, or
#   the existence of the target this gate rides.
#
#   BOARD defaults to KICKOS_BOARD, which is the board every in-tree gate means.
#   NAME defaults to <board tag>_<target>, the board name with `-` turned into `_`; state it
#   explicitly where the ctest name is not that (one target carrying several gates, or one
#   gate whose name is the claim rather than the image).
#   QEMU_MACHINE is always passed: check_fault_dump.sh reads an UNSET QEMU_MACHINE as "this
#   is the sim, run natively", and most boards would otherwise take the mps2-an386 fallback.
#   MACHINE overrides the board default. ARGS are extra script arguments after the ELF.
#   TIMEOUT defaults to 60s.
#   TARGET names an app target; the image is $<TARGET_FILE:> unless that target records a
#   KICKOS_IMAGE_FILE, which the x86_64 OBJECT-library app does.
# ---------------------------------------------------------------------------
function(kickos_add_qemu_test)
  cmake_parse_arguments(QT "" "NAME;TARGET;BOARD;SCRIPT;MACHINE;TIMEOUT" "ARGS" ${ARGN})
  if(NOT QT_TARGET OR NOT QT_SCRIPT)
    message(FATAL_ERROR "kickos_add_qemu_test: TARGET and SCRIPT are required")
  endif()
  if(NOT QT_BOARD)
    set(QT_BOARD "${KICKOS_BOARD}")
  endif()
  if(NOT QT_NAME)
    string(REPLACE "-" "_" _tag "${QT_BOARD}")
    set(QT_NAME "${_tag}_${QT_TARGET}")
  endif()
  kickos_qemu_machine("${QT_BOARD}" _env _machine)
  if(_machine STREQUAL "")
    return()
  endif()
  if(QT_MACHINE)
    set(_machine "${QT_MACHINE}")
  endif()
  list(APPEND _env QEMU_MACHINE=${_machine})
  # An app target that writes its image outside CMake's target model records the path; every
  # other one is named by $<TARGET_FILE:>.
  get_target_property(_qt_image "${QT_TARGET}" KICKOS_IMAGE_FILE)
  if(_qt_image)
    set(_qt_file "${_qt_image}")
  else()
    set(_qt_file "$<TARGET_FILE:${QT_TARGET}>")
  endif()
  add_test(NAME "${QT_NAME}"
    COMMAND "${CMAKE_COMMAND}" -E env ${_env}
            "${QT_SCRIPT}" "${_qt_file}" ${QT_ARGS})
  if(NOT QT_TIMEOUT)
    set(QT_TIMEOUT 60)
  endif()
  set_tests_properties("${QT_NAME}" PROPERTIES TIMEOUT ${QT_TIMEOUT} SKIP_RETURN_CODE 77)
endfunction()

# ---------------------------------------------------------------------------
# The host gate seam: real kernel translation units compiled for the build host at a
# POSTURE the running preset does not carry.
#
# A posture is a set of KICKOS_* macros whose value decides which arms of a kernel source
# exist at all. Overriding one is not a matter of appending a -D: the value reaches every
# in-tree TU through the root's add_compile_definitions, and TWO -D of one macro with
# different values is a redefinition -Werror refuses. It arrives by two separate routes that
# both have to be cut, and cutting one silently leaves the preset's value in force:
#   the DIRECTORY property a test directory inherits from the root, and
#   kickos_kernel's own COMPILE_DEFINITIONS, which the gate reads to compile the same
#   translation unit that ships.
# kickos_posture_scrub does the cutting; nothing else in the tree may spell the filter.
#
# kickos_posture_scrub(<out> <MACRO=VALUE>...)
#   Drops exactly the macros named here from the CALLER'S directory property (a function
#   shares its caller's directory scope), and returns in <out> the definition list a host
#   target compiling kernel sources wants: kickos_kernel's own definitions with those same
#   macros filtered out, then the given values.
function(kickos_posture_scrub out)
  set(_names "")
  foreach(_d IN LISTS ARGN)
    if(NOT _d MATCHES "^([A-Za-z_][A-Za-z0-9_]*)=")
      message(FATAL_ERROR "kickos_posture_scrub: '${_d}' is not <MACRO>=<VALUE>")
    endif()
    list(APPEND _names "${CMAKE_MATCH_1}")
  endforeach()
  set(_kernel_defs "$<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>")
  if(_names)
    list(REMOVE_DUPLICATES _names)
    string(REPLACE ";" "|" _alt "${_names}")
    set(_re "^(${_alt})=")
    get_directory_property(_dirdefs COMPILE_DEFINITIONS)
    list(FILTER _dirdefs EXCLUDE REGEX "${_re}")
    set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${_dirdefs}")
    set(_kernel_defs "$<FILTER:${_kernel_defs},EXCLUDE,${_re}>")
  endif()
  set(${out} "${_kernel_defs}" ${ARGN} PARENT_SCOPE)
endfunction()

# kickos_add_kernel_host_lib(<name> SOURCES <cc...> [INCLUDES <dirs...>]
#                            [POSTURE <MACRO=VALUE>...])
#   Kernel sources compiled for the host at a posture, as an OBJECT library SEVERAL gates
#   link. It carries its include path, definitions and C++ standard PUBLIC, so a gate that
#   links it inherits the whole posture and its own TU is compiled the same way.
#
#   The kernel's COMPILE_OPTIONS are deliberately NOT inherited: -ffreestanding, -fno-common
#   and -fno-use-cxa-atexit describe an image with no host libc, and a gate is a hosted
#   program that needs stdio, setjmp and atexit. The three LANGUAGE flags below are the ones
#   that change what a translation unit MEANS, so they are carried by hand.
#
#   The posture is recorded on the target: kickos_add_unit_test scrubs a linking gate's
#   directory for it, so a consumer states nothing.
function(kickos_add_kernel_host_lib name)
  cmake_parse_arguments(KHL "" "" "SOURCES;INCLUDES;POSTURE" ${ARGN})
  if(NOT KHL_SOURCES)
    message(FATAL_ERROR "kickos_add_kernel_host_lib(${name}): SOURCES is required")
  endif()
  kickos_posture_scrub(_khl_defs ${KHL_POSTURE})
  add_library(${name} OBJECT ${KHL_SOURCES})
  target_compile_features(${name} PUBLIC ${KICKOS_CXX_STANDARD})
  target_include_directories(${name} PUBLIC
    $<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES> ${KHL_INCLUDES})
  target_compile_definitions(${name} PUBLIC ${_khl_defs})
  target_compile_options(${name} PRIVATE ${KICKOS_WARN_FLAGS}
    -fno-exceptions -fno-rtti -fno-threadsafe-statics)
  set_property(TARGET ${name} PROPERTY KICKOS_POSTURE "${KHL_POSTURE}")
endfunction()

# ---------------------------------------------------------------------------
# kickos_add_unit_test(NAME <target> SOURCES <cc...> [INCLUDES <dirs...>]
#                     [DEFINITIONS <defs...>] [LIBRARIES <libs...>]
#                     [KERNEL] [POSTURE <MACRO=VALUE>...] [TEST_PREFIX <p>])
#   One host unit-test executable on GoogleTest, with PER-CASE ctest entries.
#
#   KERNEL adds kickos_kernel's include path and its definitions VERBATIM, for a gate that
#   compiles a kernel translation unit or includes a kernel header: a divergent config would
#   gate a different translation unit than the one that ships. POSTURE is the same with the
#   named macros replaced, and implies KERNEL.
#
#   TEST_PREFIX is for a gate built SEVERAL times from one source: every binary defines the
#   same gtest suite and case names, and without a prefix they would register under one
#   ctest name and only one geometry would be checked.
#
#   gtest_discover_tests writes its add_test calls at BUILD time, so the root CMakeLists
#   wrapper that appends the build fixture never sees them: the `host` label and
#   FIXTURES_REQUIRED have to be passed through PROPERTIES here.
function(kickos_add_unit_test)
  cmake_parse_arguments(UT "KERNEL" "NAME;TEST_PREFIX"
    "SOURCES;INCLUDES;DEFINITIONS;LIBRARIES;POSTURE" ${ARGN})
  if(NOT UT_NAME OR NOT UT_SOURCES)
    message(FATAL_ERROR "kickos_add_unit_test: NAME and SOURCES are required")
  endif()
  # A linked posture library carries its macros PUBLIC, so this directory's inherited copy
  # of them collides exactly as an own POSTURE would.
  set(_posture ${UT_POSTURE})
  foreach(_lib IN LISTS UT_LIBRARIES)
    if(TARGET ${_lib})
      get_target_property(_lib_posture ${_lib} KICKOS_POSTURE)
      if(_lib_posture)
        list(APPEND _posture ${_lib_posture})
      endif()
    endif()
  endforeach()
  if(_posture OR UT_KERNEL)
    kickos_posture_scrub(_ut_kernel_defs ${_posture})
  endif()
  add_executable(${UT_NAME} ${UT_SOURCES})
  target_link_libraries(${UT_NAME} PRIVATE ${UT_LIBRARIES} GTest::gtest_main)
  target_compile_features(${UT_NAME} PRIVATE ${KICKOS_CXX_STANDARD})
  target_compile_options(${UT_NAME} PRIVATE ${KICKOS_WARN_FLAGS})
  if(UT_KERNEL OR UT_POSTURE)
    target_include_directories(${UT_NAME} PRIVATE
      $<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES>)
  endif()
  if(UT_INCLUDES)
    target_include_directories(${UT_NAME} PRIVATE ${UT_INCLUDES})
  endif()
  if(UT_KERNEL OR UT_POSTURE)
    target_compile_definitions(${UT_NAME} PRIVATE ${_ut_kernel_defs})
  endif()
  if(UT_DEFINITIONS)
    target_compile_definitions(${UT_NAME} PRIVATE ${UT_DEFINITIONS})
  endif()
  gtest_discover_tests(${UT_NAME}
    TEST_PREFIX "${UT_TEST_PREFIX}"
    PROPERTIES TIMEOUT 30 LABELS host FIXTURES_REQUIRED kickos_build
    DISCOVERY_TIMEOUT 60)
endfunction()

# ---------------------------------------------------------------------------
# kickos_add_board_provider(<name> SOURCE <cc> [LINK <libs...>] [RETAINED_CAPS <n>]
#                           [INBOUND_REPLY_CAPS <r>])
#   A board-descriptor provider library (pinmap or service-list): a freestanding STATIC lib
#   defining one board-descriptor symbol, seeing only system/include, exported to
#   KickOSTargets. LINK carries a service list's board driver targets, which back-reference
#   kickos_user and so join the rescan link group. The target is kickos_<name>.
#
#   RETAINED_CAPS is how many capabilities a SERVICE LIST leaves in root's table for the life
#   of the image (cmake/cap_table.cmake). It is RETENTION: a list whose bring-up transiently
#   holds more than its retention plus the app's peak must declare the transient.
#
#   INBOUND_REPLY_CAPS is how many CAP_REPLY capabilities one of the list's SERVICES holds at
#   once as the server side of kos_call. The widest declaration in the tree wins, so it is
#   not added to the app's number.
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
  # numerically and would take a negative as a term that narrows the summed width.
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
#   arch/*/chip/*/mpu.cmake opt-ins (KICKOS_CHIP_ENFORCES_MPU) reverse-mapped through the
#   board descriptors, plus the sim. Reads each descriptor in this function's scope so the
#   KICKOS_CHIP/ARCH it sets never leak into the caller's build.
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
