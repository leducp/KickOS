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

# List-dir-relative: driver_geometry.cmake, driver_metadata.cmake, heap_symbol.cmake,
# qemu_cpu.cmake and compose.cmake must be installed beside this file.
include("${CMAKE_CURRENT_LIST_DIR}/driver_geometry.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/driver_metadata.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/heap_symbol.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/qemu_cpu.cmake")

# In-tree vs installed-package signal: a source tree has boards/ beside cmake/; an
# installed package ships kickos.cmake with no boards/ sibling.
if(EXISTS "${KICKOS_BOARDS_DIR}")
  set(KICKOS_IN_TREE TRUE)
else()
  set(KICKOS_IN_TREE FALSE)
endif()

# After KICKOS_IN_TREE, which places the host tool.
include("${CMAKE_CURRENT_LIST_DIR}/compose.cmake")

# The image link (tools/image-link.py) and the bounds it renames, and the esptool image
# (tools/esp-image.py), from the source tree or beside this file in an installed package.
if(KICKOS_IN_TREE)
  get_filename_component(KICKOS_IMAGE_LINK_TOOL "${CMAKE_CURRENT_LIST_DIR}/../tools/image-link.py"
                         ABSOLUTE)
  get_filename_component(KICKOS_ESP_IMAGE_TOOL "${CMAKE_CURRENT_LIST_DIR}/../tools/esp-image.py"
                         ABSOLUTE)
else()
  set(KICKOS_IMAGE_LINK_TOOL "${CMAKE_CURRENT_LIST_DIR}/image-link.py")
  set(KICKOS_ESP_IMAGE_TOOL "${CMAKE_CURRENT_LIST_DIR}/esp-image.py")
endif()
set(KICKOS_IMAGE_NAMES "${CMAKE_CURRENT_LIST_DIR}/image_names.txt")
set(KICKOS_IMAGE_EMBED_NAMES "${CMAKE_CURRENT_LIST_DIR}/embed_names.txt")

# kickos_image_link_features()
#   The KICKOS_KERNEL link group the kernel's archives ride. Its bounds tell tools/image-link.py
#   which inputs are the kernel's, and a link no launcher reads fails on them, naming
#   kickos_image_link.
macro(kickos_image_link_features)
  foreach(_kos_lang C CXX ASM)
    set(CMAKE_${_kos_lang}_LINK_GROUP_USING_KICKOS_KERNEL_SUPPORTED TRUE)
    set(CMAKE_${_kos_lang}_LINK_GROUP_USING_KICKOS_KERNEL
        "LINKER:--kickos-image-link-needs-kickos_image_link,--start-group"
        "LINKER:--end-group,--kickos-image-link-end")
  endforeach()
endmacro()

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
# kickos_image_host_launcher(<out var>)
#   The sim's image link as a linker launcher: an executable whose link carries the kernel's
#   group is an image, and any other link runs unchanged.
# ---------------------------------------------------------------------------
function(kickos_image_host_launcher out)
  find_program(KICKOS_PYTHON3 NAMES python3 REQUIRED)
  set(${out} "${KICKOS_PYTHON3}" -I "${KICKOS_IMAGE_LINK_TOOL}" --host "${CMAKE_OBJCOPY}"
             "${CMAKE_NM}" "${KICKOS_IMAGE_NAMES}" ${KICKOS_IMAGE_KERNEL_ENTRIES} ${ARGN} --link
      PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# kickos_image_link(<target>)
#   Links <target>, an executable linking KickOS::kernel, as images and a final link:
#   tools/image-link.py is its linker launcher, and under KICKOS_IMAGE_EMBED image 0 is also
#   <target>.kimg/image0.elf. kickos_emit_image calls
#   it; an image no one emits, a sizing link say, calls it itself. Properties set before it: a
#   KICKOS_IMAGE_KERNEL_NAMES names kernel symbols the image names directly, on a board that
#   enforces nothing; a KICKOS_IMAGE_KERNEL_OBJECTS names objects the final link takes for the
#   kernel and the image does not. The objects of KICKOS_IMAGE_KERNEL_OBJECT_TARGETS ride the
#   command and only the final link takes them.
# ---------------------------------------------------------------------------
function(kickos_image_link target)
  find_program(KICKOS_PYTHON3 NAMES python3 REQUIRED)
  get_target_property(_calls ${target} KICKOS_IMAGE_KERNEL_NAMES)
  if(NOT _calls)
    set(_calls "")
  endif()
  if(KICKOS_ARCH STREQUAL "sim")
    kickos_image_host_launcher(_launcher ${_calls})
  else()
    # The kernel's own copy of what it calls under the app's names, each image linking its own,
    # and any object the target states is the kernel's.
    get_target_property(_objects ${target} KICKOS_IMAGE_KERNEL_OBJECTS)
    if(NOT _objects)
      set(_objects "")
    endif()
    list(JOIN _objects "\n" _objects)
    set(_kernel_objects "${CMAKE_CURRENT_BINARY_DIR}/${target}.kernel_objects")
    if(KICKOS_IMAGE_KERNEL_OBJECT_TARGETS)
      string(APPEND _objects "\n--kernel-only")
    endif()
    foreach(_t IN LISTS KICKOS_IMAGE_KERNEL_OBJECT_TARGETS)
      string(APPEND _objects "\n$<JOIN:$<TARGET_OBJECTS:${_t}>,\n>")
    endforeach()
    file(GENERATE OUTPUT "${_kernel_objects}" CONTENT
      "$<$<TARGET_EXISTS:KickOS::kickos_string>:$<JOIN:$<TARGET_OBJECTS:KickOS::kickos_string>,\n>>\n${_objects}\n")
    set(_mode "")
    if(KICKOS_IMAGE_EMBED)
      set(_mode --embed)
    endif()
    set(_launcher "${KICKOS_PYTHON3}" -I "${KICKOS_IMAGE_LINK_TOOL}" ${_mode} "${CMAKE_OBJCOPY}"
                  "${CMAKE_NM}" "${KICKOS_IMAGE_NAMES}" "${_kernel_objects}"
                  ${KICKOS_IMAGE_KERNEL_ENTRIES} ${_calls} --link)
  endif()
  set_target_properties(${target} PROPERTIES C_LINKER_LAUNCHER "${_launcher}"
                                             CXX_LINKER_LAUNCHER "${_launcher}")
  set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
    "${KICKOS_IMAGE_LINK_TOOL}" "${KICKOS_IMAGE_NAMES}"
    "${KICKOS_IMAGE_EMBED_NAMES}")
  # The tool's one folder of every per-image file, removed whole.
  set_property(TARGET ${target} APPEND PROPERTY ADDITIONAL_CLEAN_FILES
    "$<TARGET_FILE:${target}>.kimg")
endfunction()

# ---------------------------------------------------------------------------
# kickos_program_link(<target>)
#   Links <target>, an executable linking KickOS::kickos that a system runs as a program,
#   through tools/image-link.py --program: its link command is recorded beside its output, which
#   is its relocatable link, and the final link of each system running it replays that command
#   for each image of it. kickos_compose calls it for each program it binds.
# ---------------------------------------------------------------------------
function(kickos_program_link target)
  get_target_property(_was ${target} C_LINKER_LAUNCHER)
  if(_was AND NOT _was MATCHES "--program")
    message(FATAL_ERROR "kickos_program_link(${target}): ${target} is an image of its own "
      "(kickos_emit_image), so no system runs it as a program")
  endif()
  find_program(KICKOS_PYTHON3 NAMES python3 REQUIRED)
  set(_mode --program)
  if(KICKOS_ARCH STREQUAL "sim")
    set(_mode --program --host)
  endif()
  set(_launcher "${KICKOS_PYTHON3}" -I "${KICKOS_IMAGE_LINK_TOOL}" ${_mode} "${KICKOS_IMAGE_NAMES}"
                --link)
  set_target_properties(${target} PROPERTIES C_LINKER_LAUNCHER "${_launcher}"
                                             CXX_LINKER_LAUNCHER "${_launcher}")
  set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${KICKOS_IMAGE_LINK_TOOL}"
                                                            "${KICKOS_IMAGE_NAMES}")
  set_property(TARGET ${target} APPEND PROPERTY ADDITIONAL_CLEAN_FILES
    "$<TARGET_FILE:${target}>.kprog")
endfunction()

# ---------------------------------------------------------------------------
# kickos_image_libc(<executable> full|nano)
#   The newlib profile every image of <executable> links and each of its TUs compiles against,
#   with the KickOS user library and libstdc++ of that profile. An executable naming none takes
#   nano where the board's descriptor names KICKOS_BOARD_NEWLIB or its configuration sets
#   KICKOS_LIBC_NANO_DEFAULT, full elsewhere. A library the executable links compiles against its
#   own KICKOS_LIBC.
# ---------------------------------------------------------------------------
function(kickos_image_libc target profile)
  if(NOT TARGET ${target})
    message(FATAL_ERROR "kickos_image_libc(${target}): no such target")
  endif()
  get_target_property(_type ${target} TYPE)
  if(NOT _type STREQUAL "EXECUTABLE")
    message(FATAL_ERROR "kickos_image_libc(${target}): ${target} is a ${_type}; an image is an "
      "executable, add_executable(${target} ...)")
  endif()
  if(NOT profile MATCHES "^(full|nano)$")
    message(FATAL_ERROR "kickos_image_libc(${target}): '${profile}' is no newlib profile; name "
      "full or nano")
  endif()
  if(NOT profile IN_LIST KICKOS_LIBC_PROFILES)
    message(FATAL_ERROR "kickos_image_libc(${target}): the KickOS ${KICKOS_ARCH_FAMILY} "
      "toolchain carries no ${profile} newlib for board '${KICKOS_BOARD}', only "
      "'${KICKOS_LIBC_PROFILES}'")
  endif()
  set_target_properties(${target} PROPERTIES KICKOS_LIBC ${profile})
endfunction()

# ---------------------------------------------------------------------------
# kickos_emit_image(<target>)
#   Writes the link map that KICKOS_IMAGE_MAP names; x86_64's link rule writes it itself, and
#   the sim's selftest map is read by its seam gates. On an MCU it also writes the .bin and .hex
#   a board flashes and prints the size. <target> may be a system composed with DEFAULT, whose
#   target it emits, the flash files keeping that target's name.
#
#   PUBLIC: a POST_BUILD action cannot ride a usage requirement, so it is one opt-in line
#   after target_link_libraries(app PRIVATE KickOS::kernel KickOS::system_default).
# ---------------------------------------------------------------------------
function(kickos_emit_image target)
  # Deferred, since the DEFAULT target is linked to the system only at the end of the directory.
  if(TARGET ${target})
    get_target_property(_default ${target} KICKOS_SYSTEM_DEFAULT)
    if(_default)
      cmake_language(EVAL CODE
        "cmake_language(DEFER DIRECTORY [[${CMAKE_CURRENT_SOURCE_DIR}]] CALL kickos_emit_image [[${_default}]])")
      return()
    endif()
  endif()
  set_property(GLOBAL APPEND PROPERTY KICKOS_EMITTED_IMAGES ${target})
  kickos_image_link(${target})
  set_target_properties(${target} PROPERTIES KICKOS_IMAGE_MAP "$<TARGET_FILE:${target}>.map")
  set_property(TARGET ${target} APPEND PROPERTY ADDITIONAL_CLEAN_FILES
    "$<TARGET_FILE:${target}>.map")
  if(KICKOS_ARCH STREQUAL "x86_64")
    # The ELF twin tools/x86_64-link.sh writes beside the image.
    set_property(TARGET ${target} APPEND PROPERTY ADDITIONAL_CLEAN_FILES
      "$<TARGET_FILE:${target}>.elf")
  else()
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
      find_program(KICKOS_PYTHON3 NAMES python3 REQUIRED)
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${KICKOS_PYTHON3} -I ${KICKOS_ESP_IMAGE_TOOL} ${KICKOS_ESPTOOL} ${KICKOS_CHIP}
                ${CMAKE_NM} $<TARGET_FILE:${target}>
                $<TARGET_FILE_DIR:${target}>/${target}.app.bin ${KICKOS_CHIP_ESPTOOL_IMAGE}
        BYPRODUCTS ${target}.app.bin
        COMMENT "esptool elf2image -> ${target}.app.bin (bootable ${KICKOS_CHIP} image)"
        VERBATIM)
    else()
      message(STATUS "KickOS: esptool not found: ${target}.app.bin (bootable "
        "${KICKOS_CHIP} image) not produced. Activate the esp-idf env and rebuild "
        "(the raw ${target}.bin is NOT bootable).")
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
#   image free of a second definer.
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
#                   [TAG <word>] [BASE <expr>] [WINDOWS <role>...] [LINES <role>:edge|level...]
#                   [BLOCK <bytes>|none] [BLOCK_CACHE cached|uncached] [INIT <function>]
#                   [POSTURE handover|retain] [BARRIER <threads before the poll>|none] [READY <expr>]
#                   [START <symbol>] [CONSOLE [USB_DEVICE]] [CLIENT <target>...] [SLOTS <count>] [LOCAL]
#                   [THREAD <role> [PRIORITY <offset>] ENTRY <function>
#                           [ARG none|block|window|line0_index] [WINDOW <role>] [CAPS <grant>...]]...)
#   The one shape of an unprivileged chip/device driver library: a freestanding STATIC lib
#   that links kickos_user, sees system/include, optionally sees a chip register dir (REGDIR,
#   definitions only), optionally links a chip class leaf (CLASS), and is EXPORTED so an
#   out-of-tree consumer links it on top of the OS. Its .data/.bss land in .appdata, the lib
#   being outside the closed kernel set the chip .ld catch-all excludes. The target is
#   kickos_<name>; SOURCES defaults to <name>.cc.
#
#   A THREAD makes it a packaged driver, a composition's `driver:`, and requires BLOCK, POSTURE,
#   BARRIER and START, the C function the init calls to bring it up. Each THREAD is one spawn, in
#   order, named by its role but the role `service`, which takes its task's name. A grant is
#   <ep|notify|line role>:<wait|signal>, or notify:signal:doorbell for the badged copy whose bit
#   sits above the lines; the thread granted ep:wait receives on the endpoint. TAG prefixes the
#   diagnostics, the name by default; BASE is the window base the bring-up insists on, 0 for none;
#   READY is the latch's offset in the block, polled after BARRIER threads. BLOCK_CACHE uncached
#   types the ring block KOS_MEM_NOCACHE. USB_DEVICE marks a console served over the board's USB
#   device controller, whose clock tree chip init brings up in an image whose stdout it is. CLIENT
#   names the libraries a task using the driver links, which kickos_compose links in a system
#   naming it. SLOTS is the device slots a bus service keys on each client's badge, which the
#   composition hands each client as a slot mask (KICKOS_DRIVER_SLOTS in the generated header).
#   ENTRY, INIT, BASE and READY are C++ expressions: the driver's TU expands
#   KICKOS_DRIVER_DESCRIPTOR from the generated <kickos/driver/declared/<name>.h> where they are in
#   scope. The catalogue entry joins the KICKOS_DRIVER_CATALOGUE global property for the manifest.
#   LOCAL declares a driver for the apps of this tree: it is neither installed nor in the manifest
#   a package carries, and only a composition that kickos_app_system names LOCAL_DRIVERS is
#   admitted against the manifest_local.yaml that lists it (KICKOS_LOCAL_DRIVER_CATALOGUE).
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
  if(DRV_LOCAL)
    kickos_alias_target(kickos_${name})
  else()
    kickos_export_targets(kickos_${name})
  endif()
  if(NOT _packaged)
    return()
  endif()
  set_target_properties(kickos_${name} PROPERTIES KICKOS_DRIVER_CATALOGUE_ENTRY "${_json}")
  if(DRV_LOCAL)
    set_property(GLOBAL APPEND PROPERTY KICKOS_LOCAL_DRIVER_CATALOGUE "${name}")
  else()
    set_property(GLOBAL APPEND PROPERTY KICKOS_DRIVER_CATALOGUE "${name}")
  endif()
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
#
# +smep and +smap are not in x86-64-v3 either: qemu64 offers neither, and without them every gate
# runs a kernel that can read, write and run user pages.
#
# +rdtscp is not in x86-64-v3 and the floor requires it: qemu64 lacks it.
#
# +rdseed is not in x86-64-v3 either: getentropy draws from it where CPUID reports it.
set(KICKOS_X86_64_QEMU_CPU
    "qemu64,+ssse3,+sse4.1,+sse4.2,+popcnt,+cx16,+lahf-lm,+avx,+avx2,+bmi1,+bmi2,+fma,+f16c,+movbe,+abm,+xsave,+xsaveopt,+smep,+smap,+rdtscp,+rdseed"
    CACHE INTERNAL "The -cpu model every x86_64 emulation runs")

# The processor a preset runs the board's image on instead of the one the board's emulator line
# names: the same image on a later core, where a feature the board's core lacks is witnessed.
set(KICKOS_QEMU_CPU "" CACHE STRING
    "The emulated -cpu, in place of the board's; empty keeps the board's")

# ---------------------------------------------------------------------------
# kickos_qemu_cpu(<out>)
#   The processor model the configured board's emulator runs (kickos_qemu_cpu_model).
# ---------------------------------------------------------------------------
function(kickos_qemu_cpu out)
  kickos_qemu_cpu_model(_cpu "${KICKOS_QEMU_CPU}" "${KICKOS_MCPU_FLAGS}" ${KICKOS_QEMU_OPTIONS})
  set(${out} "${_cpu}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# kickos_qemu_machine(<board> <out_env> <out_machine> [<out_cpus>])
#   The emulator the configured board's board file states (its `emulator`, which chip.cmake
#   carries), as the environment a gate runs under and the QEMU machine: out_machine comes back
#   EMPTY for a board no emulator runs. <board> must be the configured board. out_cpus is the
#   machine's -smp count, 1 without one.
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
  set(_cpus 1)
  if(KICKOS_AMP_NODE AND KICKOS_AMP_OWN_IMAGE)
    set(_cpus ${KICKOS_AMP_PARTITION_CORES})
  elseif(KICKOS_NUM_CORES GREATER 1)
    set(_cpus ${KICKOS_NUM_CORES})
  endif()
  if(ARGC GREATER 3)
    set(${ARGV3} ${_cpus} PARENT_SCOPE)
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
  kickos_qemu_cpu_line(_extra "${KICKOS_QEMU_CPU}" "${board}" ${KICKOS_QEMU_OPTIONS})
  if(KICKOS_AMP_NODE AND KICKOS_AMP_OWN_IMAGE)
    math(EXPR _part_mib
         "(${KICKOS_AMP_NODES} * ${KICKOS_AMP_NODE_SHARE} + ${KICKOS_AMP_SHARED_SIZE} + 1048575) / 1048576")
    list(APPEND _extra -smp ${_cpus} -m ${_part_mib}M)
  elseif(_cpus GREATER 1)
    list(APPEND _extra -smp ${_cpus})
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
