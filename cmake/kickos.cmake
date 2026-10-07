# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# KickOS build helpers: per-component flag posture, the driver-class backends an app links
# (kickos_link_class_backends) and the image emitter kickos_emit_image().
#
# The application owns the final link: the link recipe lives on the exported KickOS::kernel and
# system targets, never in these helpers. An app is three lines,
# examples/oot-mcu-app being the reference shape.

# ---------------------------------------------------------------------------
# Board -> {arch, chip} resolution.
#
# A board's arch and chip come from one descriptor, boards/<board>/board.cmake, also included
# pre-project by the cross toolchain file.
#
# KICKOS_BOARDS_DIR is captured at include time: a called function sees the caller's list
# dir, not this file's. An installed package has no boards/ tree, hence the fallback below.
# ---------------------------------------------------------------------------
get_filename_component(KICKOS_BOARDS_DIR "${CMAKE_CURRENT_LIST_DIR}/../boards" ABSOLUTE)

# List-dir-relative: cap_table.cmake, driver_geometry.cmake, driver_metadata.cmake,
# heap_symbol.cmake and compose.cmake must be installed beside this file.
include("${CMAKE_CURRENT_LIST_DIR}/cap_table.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/driver_geometry.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/driver_metadata.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/heap_symbol.cmake")

# In-tree vs installed-package signal: a source tree has boards/ beside cmake/; an
# installed package ships kickos.cmake with no boards/ sibling.
if(EXISTS "${KICKOS_BOARDS_DIR}")
  set(KICKOS_IN_TREE TRUE)
else()
  set(KICKOS_IN_TREE FALSE)
endif()

# After KICKOS_IN_TREE, which places the host tool.
include("${CMAKE_CURRENT_LIST_DIR}/compose.cmake")

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
# the exported KickOS::kernel usage target carries none of them.
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
  # The flag keeps kernel DATA out of .sdata/.sbss, and each RISC-V chip script refuses a
  # KickOS archive's small data at link. On the split rv64 image a kernel ACCESS must not
  # resolve through gp either, which the image's link holds (--no-relax-gp on
  # kickos_kernel_leaf).
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
#   nothing: tests/static/check_kernel_got.sh is what turns that into a failure.
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
#   Writes the link map that KICKOS_IMAGE_MAP names; x86_64's link rule writes it itself, and
#   the sim's selftest map is read by its seam gates. On an MCU it also writes the .bin and .hex
#   a board flashes and prints the size.
#
#   PUBLIC: a POST_BUILD action cannot ride a usage requirement, so it is one opt-in line
#   after target_link_libraries(app PRIVATE KickOS::kernel KickOS::system_default).
# ---------------------------------------------------------------------------
function(kickos_emit_image target)
  set_property(GLOBAL APPEND PROPERTY KICKOS_EMITTED_IMAGES ${target})
  set_target_properties(${target} PROPERTIES KICKOS_IMAGE_MAP "$<TARGET_FILE:${target}>.map")
  set_property(TARGET ${target} APPEND PROPERTY ADDITIONAL_CLEAN_FILES
    "$<TARGET_FILE:${target}>.map")
  if(NOT KICKOS_ARCH STREQUAL "x86_64")
    target_link_options(${target} PRIVATE "-Wl,-Map=$<TARGET_FILE:${target}>.map")
  endif()
  if(KICKOS_ARCH STREQUAL "sim" OR KICKOS_ARCH STREQUAL "x86_64")
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

  # Where the ROM boots esptool's image format alone (the chip file's `esptool_image`), the raw
  # objcopy .bin is not bootable. A missing esptool skips with a message rather than failing.
  if(DEFINED KICKOS_CHIP_ESPTOOL_IMAGE)
    find_program(KICKOS_ESPTOOL NAMES esptool esptool.py)
    if(KICKOS_ESPTOOL)
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${KICKOS_ESPTOOL} --chip ${KICKOS_CHIP} elf2image ${KICKOS_CHIP_ESPTOOL_IMAGE}
                --output $<TARGET_FILE_DIR:${target}>/${target}.app.bin
                $<TARGET_FILE:${target}>
        BYPRODUCTS ${target}.app.bin
        COMMENT "esptool elf2image -> ${target}.app.bin (bootable ${KICKOS_CHIP} image)"
        VERBATIM)
    else()
      list(JOIN KICKOS_CHIP_ESPTOOL_IMAGE " " _kos_img_options)
      message(STATUS "KickOS: esptool not found: ${target}.app.bin (bootable "
        "${KICKOS_CHIP} image) not produced. Activate the esp-idf env, or run: "
        "esptool --chip ${KICKOS_CHIP} elf2image ${_kos_img_options} --output ${target}.app.bin "
        "<elf>  (the raw ${target}.bin is NOT bootable).")
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# kickos_select_class_backend(<class> <target>)
#   Which backend answers a DRIVER CLASS (<kickos/driver/*.h>) in this image. Every backend of
#   one class defines the same public kos_<class>_* symbols, so an image has exactly one, and
#   which one is a property of the IMAGE POSTURE the board chose, never of an application: a
#   client body is identical over a proxy and over a local engine, which is the substitution
#   property the class exists for.
#
#   The selection is a GLOBAL property rather than a variable because the deciding site
#   (system/CMakeLists.txt in tree, KickOSConfig.cmake in a package) is in a different
#   directory scope from the apps that consume it. <target> is the backend's KickOS:: name,
#   which KickOSConfig.cmake replays verbatim.
#
#   The BACKEND MUST PRECEDE KickOS::kernel ON THE LINK LINE: the toolchains link the component
#   archives with a --start-group rescan, so a group member that ever referenced a class symbol
#   would otherwise pull a second definer out of the group and the ORDER, not the selection,
#   would decide the engine. That ordering is kickos_link_class_backends's to hold, which is why
#   no app states it.
function(kickos_select_class_backend class target)
  string(TOUPPER "${class}" _cls)
  set_property(GLOBAL PROPERTY KICKOS_CLASS_BACKEND_${_cls} "${target}")
  set_property(GLOBAL APPEND PROPERTY KICKOS_CLASS_BACKEND_CLASSES "${class}")
endfunction()

# ---------------------------------------------------------------------------
# kickos_export_name(<target> <out>)
#   The target's name under the package's namespace: KickOS::<EXPORT_NAME>, or KickOS::<target>
#   where it sets none or is no target yet. A target named inside $<LINK_GROUP:> or
#   $<TARGET_OBJECTS:> reaches KickOSTargets.cmake verbatim, so those genexes spell this name.
# ---------------------------------------------------------------------------
function(kickos_export_name target out)
  set(_name "${target}")
  if(TARGET ${target})
    get_target_property(_export ${target} EXPORT_NAME)
    if(_export)
      set(_name "${_export}")
    endif()
  endif()
  set(${out} "KickOS::${_name}" PARENT_SCOPE)
endfunction()

# kickos_alias_target(<target>...)
#   Defines each target's kickos_export_name as its in-tree ALIAS, which is all a provider the
#   link group names needs in tree.
function(kickos_alias_target)
  foreach(_t IN LISTS ARGN)
    kickos_export_name(${_t} _alias)
    add_library(${_alias} ALIAS ${_t})
  endforeach()
endfunction()

# kickos_export_targets(<target>...)
#   kickos_alias_target, and installs each target into the KickOSTargets export set, which names
#   it the same way installed.
function(kickos_export_targets)
  kickos_alias_target(${ARGN})
  install(TARGETS ${ARGN} EXPORT KickOSTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
          OBJECTS DESTINATION "${CMAKE_INSTALL_LIBDIR}/kickos")
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
# kickos_link_class_backends(<target> <class>...)
#   Links the backend kickos_select_class_backend chose for each DRIVER CLASS the app's own
#   sources call (spi, i2c, ...), the one thing about a class an application knows. Called
#   before the app's KickOS::kernel line, which puts each backend archive ahead of the rescan
#   group. An app that names no class links no backend, which is what keeps a mock-carrying
#   image (the selftest) free of a second definer.
# ---------------------------------------------------------------------------
function(kickos_link_class_backends target)
  foreach(_class IN LISTS ARGN)
    kickos_class_backend("${_class}" _backend)
    if(NOT _backend)
      message(FATAL_ERROR "kickos_link_class_backends(${target}): no backend of the '${_class}' "
        "class is selected for board '${KICKOS_BOARD}'. A board whose chip has no such block "
        "cannot host this app; kickos_select_class_backend names the backend where it can.")
    endif()
    target_link_libraries(${target} PRIVATE ${_backend})
    set_property(TARGET ${target} APPEND PROPERTY KICKOS_APP_CLASS_BACKENDS "${_backend}")
  endforeach()
endfunction()

# ---------------------------------------------------------------------------
# kickos_add_driver(<name> [SOURCES <src...>] [CLASS <leaf>] [REGDIR <dir>]
#                   [THREADS <role>:<priority offset>:<stack>:<capabilities>:<badged>...] [RECEIVER <role>]
#                   [WINDOWS <role>...] [LINES <role>...] [NOTIFY]
#                   [BLOCK <bytes>|none] [BLOCK_CACHE cached|uncached] [POSTURE handover|retain]
#                   [BARRIER <threads before the poll>|none] [START <symbol>] [CONSOLE [USB_DEVICE]]
#                   [CLIENT <target>...])
#   The one shape of an unprivileged chip/device driver library: a freestanding STATIC lib
#   that links kickos_user, sees system/include, optionally sees a chip register dir (REGDIR,
#   definitions only), optionally links a chip class leaf (CLASS), and is EXPORTED so an
#   out-of-tree consumer links it on top of the OS. Its .data/.bss land in .appdata, the lib
#   being outside the closed kernel set the chip .ld catch-all excludes. The target is
#   kickos_<name>; SOURCES defaults to <name>.cc.
#
#   THREADS makes it a packaged driver, a composition's `driver:`, and requires BLOCK, POSTURE,
#   BARRIER, START, the C function the init calls to bring it up, and RECEIVER, the thread that
#   waits on its endpoint. A role is its thread's name, but the role `service`, whose thread takes
#   its task's name; a stack is `default` only, bring_up spawning every thread on
#   the kernel's default stack; a thread's capabilities are what its spawn delegates, and badged the
#   copies of the driver's notification among them, which the bring-up mints for the spawn. NOTIFY
#   says it uses the notification the shared bring-up creates. BLOCK_CACHE uncached types the ring
#   block KOS_MEM_NOCACHE, the init self-granting it so and its descriptor's block_flags stating
#   it; cached is the default. USB_DEVICE marks a console served over the board's USB device
#   controller, whose clock tree chip init brings up in an image whose stdout it is. CLIENT
#   names the libraries a task
#   using the driver links, which kickos_compose links in a system naming it. Its catalogue entry joins the
#   KICKOS_DRIVER_CATALOGUE global property for the manifest, and its descriptor reads the
#   generated <kickos/driver/declared/<name>.h>, whose k_declared it static_asserts declared_as.
function(kickos_add_driver name)
  cmake_parse_arguments(DRV "${KICKOS_DRIVER_OPTIONS}" "${KICKOS_DRIVER_SINGLE}" "${KICKOS_DRIVER_MULTI}" ${ARGN})
  kickos_driver_metadata(${name} _packaged _json _header ${ARGN})
  if(NOT DEFINED DRV_SOURCES)
    set(DRV_SOURCES "${name}.cc")
  endif()
  add_library(kickos_${name} STATIC ${DRV_SOURCES})
  kickos_apply_freestanding(kickos_${name})
  target_link_libraries(kickos_${name} PUBLIC kickos_user)
  target_include_directories(kickos_${name} PRIVATE
    "${PROJECT_SOURCE_DIR}/system/include")
  if(DEFINED DRV_REGDIR)
    target_include_directories(kickos_${name} PRIVATE
      "${PROJECT_SOURCE_DIR}/${DRV_REGDIR}")
    target_include_directories(kickos_${name} PRIVATE "${PROJECT_BINARY_DIR}/generated/chip")
  endif()
  if(DEFINED DRV_CLASS)
    target_link_libraries(kickos_${name} PRIVATE ${DRV_CLASS})
  endif()
  kickos_export_targets(kickos_${name})
  if(NOT _packaged)
    return()
  endif()
  set_target_properties(kickos_${name} PROPERTIES KICKOS_DRIVER_CATALOGUE_ENTRY "${_json}")
  set_property(GLOBAL APPEND PROPERTY KICKOS_DRIVER_CATALOGUE "${name}")
  set(_declared_dir "${CMAKE_CURRENT_BINARY_DIR}/kickos_${name}_declared")
  kickos_write_if_changed("${_declared_dir}/kickos/driver/declared/${name}.h" "${_header}")
  target_include_directories(kickos_${name} PRIVATE "${_declared_dir}")
endfunction()

# The processor every x86_64 emulation runs, the application gates and the boot witnesses
# alike: qemu64, which is below the floor, plus the features x86-64-v3 requires
# (arch/x86/x86_64/floor_x86_64.cc). It reaches both runners as KICKOS_X86_64_CPU.
#
# +xsaveopt is not in x86-64-v3 and the port never executes it: QEMU 11.1's TCG spins on the
# CR4 write that sets OSXSAVE for a model reporting XSAVE without it, measured, so the boot
# never reaches its first thread.
set(KICKOS_X86_64_QEMU_CPU
    "qemu64,+ssse3,+sse4.1,+sse4.2,+popcnt,+cx16,+lahf-lm,+avx,+avx2,+bmi1,+bmi2,+fma,+f16c,+movbe,+abm,+xsave,+xsaveopt"
    CACHE INTERNAL "The -cpu model every x86_64 emulation runs")

# ---------------------------------------------------------------------------
# kickos_qemu_machine(<board> <out_env> <out_machine>)
#   The emulator the configured board's board file states (its `emulator`, which chip.cmake
#   carries), as the environment a gate runs under and the QEMU machine: out_machine comes back
#   EMPTY for a board no emulator runs. <board> must be the configured board.
#
#   -smp is what makes a multi-core build's cores exist, and under one image per node the
#   machine's cores and memory are the PARTITION's: sized from KICKOS_NUM_CORES instead, a PSCI
#   CPU_ON would fail on the one CPU the machine has and its DRAM would end below the region
#   every node writes.
# ---------------------------------------------------------------------------
function(kickos_qemu_machine board out_env out_machine)
  if(NOT board STREQUAL KICKOS_BOARD)
    message(FATAL_ERROR "kickos_qemu_machine: answers for the configured board '${KICKOS_BOARD}', "
      "not '${board}'")
  endif()
  if("${KICKOS_QEMU_MACHINE}" STREQUAL "")
    set(${out_env} "" PARENT_SCOPE)
    set(${out_machine} "" PARENT_SCOPE)
    return()
  endif()
  set(_machine "${KICKOS_QEMU_MACHINE}")
  if(KICKOS_ARM64_GIC_VERSION EQUAL 3 AND NOT "${KICKOS_QEMU_GICV3_MACHINE}" STREQUAL "")
    set(_machine "${KICKOS_QEMU_GICV3_MACHINE}")
  endif()
  set(_extra ${KICKOS_QEMU_OPTIONS})
  if(KICKOS_AMP_NODE AND KICKOS_AMP_OWN_IMAGE)
    math(EXPR _part_mib
         "(${KICKOS_AMP_NODES} * ${KICKOS_AMP_NODE_SHARE} + ${KICKOS_AMP_SHARED_SIZE} + 1048575) / 1048576")
    list(APPEND _extra -smp ${KICKOS_AMP_PARTITION_CORES} -m ${_part_mib}M)
  elseif(KICKOS_NUM_CORES GREATER 1)
    list(APPEND _extra -smp ${KICKOS_NUM_CORES})
  endif()
  set(_env "QEMU=${KICKOS_QEMU_BINARY}")
  if(_extra)
    list(JOIN _extra " " _extra)
    list(APPEND _env "QEMU_EXTRA=${_extra}")
  endif()
  if(KICKOS_ARCH STREQUAL "x86_64")
    # tests/lib/gate.sh builds the firmware, the writable variable store and the EFI system
    # partition per run, the image being a PE32+ application -kernel cannot start. q35 SMP
    # refuses to start without x2APIC, which qemu64 does not advertise.
    set(_cpu "${KICKOS_X86_64_QEMU_CPU}")
    if(KICKOS_NUM_CORES GREATER 1)
      string(APPEND _cpu ",+x2apic")
    endif()
    list(APPEND _env KICKOS_BOOT=uefi-pe "KICKOS_X86_64_CPU=${_cpu}")
  endif()
  set(${out_env} "${_env}" PARENT_SCOPE)
  set(${out_machine} "${_machine}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# kickos_add_qemu_test([NAME <n>] TARGET <app> SCRIPT <sh>
#                      [MACHINE <m>] [BOOTS <n>] [WORK <s>] [ARGS <arg...>])
#   Register a QEMU boot gate: run SCRIPT against the app's ELF and treat exit 77 as SKIP (a
#   missing qemu-system is a skip, not a failure).
#
#   TOTAL over the fleet: a board with no emulator registers nothing and says nothing, so a
#   call site owes no board predicate. What a call site still owes is any condition that is
#   about the CLAIM rather than about the emulator: an arch, an MPU posture, a core count, or
#   the existence of the target this gate rides.
#
#   NAME defaults to <board tag>_<target>, the board name with `-` turned into `_`; state it
#   explicitly where the ctest name is not that (one target carrying several gates, or one
#   gate whose name is the claim rather than the image).
#   QEMU_MACHINE is always passed: check_fault_dump.sh reads an UNSET QEMU_MACHINE as "this
#   is the sim, run natively", and most boards would otherwise take the mps2-an386 fallback.
#   MACHINE overrides the board default. ARGS are extra script arguments after the ELF.
#   BOOTS and WORK size the TIMEOUT, see kickos_boot_timeout.
#   TARGET names an app target, whose image is $<TARGET_FILE:>.
# ---------------------------------------------------------------------------
function(kickos_add_qemu_test)
  cmake_parse_arguments(QT "" "NAME;TARGET;SCRIPT;MACHINE;BOOTS;WORK" "ARGS" ${ARGN})
  if(NOT QT_TARGET OR NOT QT_SCRIPT)
    message(FATAL_ERROR "kickos_add_qemu_test: TARGET and SCRIPT are required")
  endif()
  if(NOT QT_NAME)
    string(REPLACE "-" "_" _tag "${KICKOS_BOARD}")
    set(QT_NAME "${_tag}_${QT_TARGET}")
  endif()
  kickos_qemu_machine("${KICKOS_BOARD}" _env _machine)
  if(_machine STREQUAL "")
    return()
  endif()
  if(QT_MACHINE)
    set(_machine "${QT_MACHINE}")
  endif()
  list(APPEND _env QEMU_MACHINE=${_machine})
  add_test(NAME "${QT_NAME}"
    COMMAND "${CMAKE_COMMAND}" -E env ${_env}
            "${QT_SCRIPT}" "$<TARGET_FILE:${QT_TARGET}>" ${QT_ARGS})
  set_tests_properties("${QT_NAME}" PROPERTIES SKIP_RETURN_CODE 77)
  kickos_boot_timeout("${QT_NAME}" "${QT_SCRIPT}" BOOTS ${QT_BOOTS} WORK ${QT_WORK})
endfunction()

# ---------------------------------------------------------------------------
# kickos_boot_timeout(<test> <script> [BOOTS <n>] [WORK <s>])
#   Register the TIMEOUT of a test booting the configured board's image, under its emulator or
#   natively on the sim, as the charge tests/lib/gate.sh's boot_bound puts on its boots, plus one
#   second, so that charge can refuse only a test whose BOOTS is miscounted. Each boot is bounded
#   at the QEMU_TIMEOUT (on the sim, SIM_TIMEOUT) this configure sees, else at <script>'s own
#   default, else at run_image's, and carries gate.sh's firmware allowance under uefi-pe and its
#   stop. A ctest run under another bound needs a reconfigure.
#
#   BOOTS (default 1) counts the boots gate.sh charges, those of a script <script> runs
#   included. WORK is what the test spends beside them, such as building the image it boots.
# ---------------------------------------------------------------------------
function(kickos_boot_timeout test script)
  cmake_parse_arguments(QB "" "BOOTS;WORK" "" ${ARGN})
  if(NOT DEFINED QB_BOOTS)
    set(QB_BOOTS 1)
  endif()
  if(NOT DEFINED QB_WORK)
    set(QB_WORK 0)
  endif()
  set(_lib "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tests/lib/gate.sh")
  set(_var QEMU_TIMEOUT)
  if(KICKOS_ARCH STREQUAL "sim")
    set(_var SIM_TIMEOUT)
  endif()
  set(_default_re "(^: |boot_bound )\"\\$\\{${_var}:[-=][0-9]+\\}\"")
  set(_bound "$ENV{${_var}}")
  if(_bound STREQUAL "")
    file(STRINGS "${script}" _bound REGEX "${_default_re}" LIMIT_COUNT 1)
  endif()
  if(_bound STREQUAL "")
    file(STRINGS "${_lib}" _bound REGEX "${_default_re}" LIMIT_COUNT 1)
  endif()
  string(REGEX REPLACE "^.*${_var}:[-=]([0-9]+)}\"$" "\\1" _bound "${_bound}")
  file(STRINGS "${_lib}" _firmware REGEX "^KOS_UEFI_FIRMWARE_S=[0-9]+$")
  file(STRINGS "${_lib}" _ticks REGEX "^KOS_STOP_TICKS=[0-9]+$")
  string(REPLACE "KOS_UEFI_FIRMWARE_S=" "" _firmware "${_firmware}")
  string(REPLACE "KOS_STOP_TICKS=" "" _ticks "${_ticks}")
  foreach(_n IN ITEMS _bound _firmware _ticks)
    if(NOT "${${_n}}" MATCHES "^[0-9]+$")
      message(FATAL_ERROR "kickos_boot_timeout(${test}): ${_n} reads '${${_n}}', not whole "
        "seconds, from ${_var}, ${script} or ${_lib}")
    endif()
  endforeach()
  kickos_qemu_machine("${KICKOS_BOARD}" _env _machine)
  if(NOT "KICKOS_BOOT=uefi-pe" IN_LIST _env)
    set(_firmware 0)
  endif()
  math(EXPR _timeout "${QB_WORK} + ${QB_BOOTS} * (${_bound} + ${_firmware} + ${_ticks} / 5) + 1")
  set_tests_properties("${test}" PROPERTIES TIMEOUT ${_timeout})
endfunction()

# ---------------------------------------------------------------------------
# kickos_host_gate(<name>... [TIMEOUT <s>])
#   The tail a gate that runs on the BUILD HOST owes: the `host` label the root's decline pass
#   reads, and a timeout, defaulting to 120s. A gate without the label is taken for a runner of
#   an image and declined wherever the integration tests are off.
#
#   It states the TAIL and not the add_test, and widening it to carry the COMMAND would be a
#   defect: an argument holding an escaped `;` survives a literal add_test and is SPLIT by any
#   re-expansion through cmake_parse_arguments, shifting every positional after it.
# ---------------------------------------------------------------------------
function(kickos_host_gate)
  cmake_parse_arguments(HG "" "TIMEOUT" "" ${ARGN})
  if(NOT HG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "kickos_host_gate: at least one registered test name is required")
  endif()
  if(NOT HG_TIMEOUT)
    set(HG_TIMEOUT 120)
  endif()
  set_tests_properties(${HG_UNPARSED_ARGUMENTS} PROPERTIES TIMEOUT ${HG_TIMEOUT} LABELS host)
endfunction()

# kickos_image_rule(<rule> <target> [<arg>...])
#   Host gate image_<rule>: tests/static/check_image_rules.sh's <rule> over <target>'s image.
function(kickos_image_rule rule target)
  add_test(NAME image_${rule}_${target}
    COMMAND "${PROJECT_SOURCE_DIR}/tests/static/check_image_rules.sh" ${rule} "${CMAKE_OBJDUMP}"
            "$<TARGET_FILE:${target}>" ${ARGN})
  kickos_host_gate(image_${rule}_${target})
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
# kickos_emit_geometry(<header> <name>...)
#   Writes generated/include/<header> defining each <name> to its value in the calling
#   cmake/*_geometry.cmake, so the C side has no copy of the value to drift.
function(kickos_emit_geometry header)
  string(MAKE_C_IDENTIFIER "${header}" _guard)
  string(TOUPPER "${_guard}" _guard)
  file(RELATIVE_PATH _from "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_LIST_FILE}")
  set(_text "// GENERATED from ${_from}; edits are overwritten by the next configure.\n\n")
  string(APPEND _text "#ifndef ${_guard}\n#define ${_guard}\n\n")
  foreach(_name ${ARGN})
    if(NOT "${${_name}}" MATCHES "^[0-9]+$")
      message(FATAL_ERROR "KickOS: ${_from} declares no integer ${_name}")
    endif()
    string(APPEND _text "#define ${_name} ${${_name}}\n")
  endforeach()
  string(APPEND _text "\n#endif\n")
  file(CONFIGURE OUTPUT "${PROJECT_BINARY_DIR}/generated/include/${header}" CONTENT "${_text}" @ONLY)
endfunction()
