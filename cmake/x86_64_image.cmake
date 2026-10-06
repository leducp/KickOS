# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The x86_64 application image link. CMake cannot drive `ld -m i386pep` as a linker for a
# target, so the image is a custom command, and this is the only body in the tree or in an
# installed package that writes one: cmake/x86_64_boot.cmake includes this file in tree and
# KickOSConfig.cmake includes the installed copy out of tree.
#
# The includer states where the two assets are, this file being unable to tell an installed
# copy of itself from the source one:
#   KICKOS_X86_64_PE_SCRIPT    the PE section script the link needs at -T
#   KICKOS_NO_GOT              tools/check-x86_64-no-got.sh, refused before every link
#   KICKOS_WEAK_UNDEF          tools/check-x86_64-weak-undef.sh, refused before every link
#   KICKOS_X86_64_KREL         tools/x86_64-krel.sh, which carries the relocation copy across
#                              the image's two links and refuses a second link that moved it
# and the link inputs, which are a property of the configure rather than of the site:
#   KICKOS_X86_64_APP_GROUP    the archive group, as KickOS:: target names
#   KICKOS_X86_64_KERNEL_GROUP the same group without the init provider, service list and pin
#                              map, for KickOS::kernel
#   KICKOS_X86_64_APP_OBJECTS  the extra objects, as $<TARGET_OBJECTS:> text
# They are read when the link is written: at the call, or for an image linking KickOS::kernel at
# the end of the top directory's configure, so an includer may state them after this file.
#
# The object libraries named below by their KickOS:: names are aliases in tree and imported
# targets from KickOSTargets.cmake in a package; $<TARGET_OBJECTS:> reads both.
#
# The toolchain's own libraries join the archive group: libc.a, libm.a and libgcc.a for every
# image, and libstdc++.a, libsupc++.a and the KickOS::kickos_cxx_rt object for one whose target
# links KickOS::kickos_cxx or KickOS::kernel. ld runs here without a compiler driver, so each is
# found once, at configure, through the compiler's -print-file-name under the board's flags.

if(NOT KICKOS_X86_64_PE_SCRIPT OR NOT EXISTS "${KICKOS_X86_64_PE_SCRIPT}")
  message(FATAL_ERROR "KickOS x86_64: KICKOS_X86_64_PE_SCRIPT names no file "
    "('${KICKOS_X86_64_PE_SCRIPT}'). Without the section script `ld -m i386pep` writes an "
    "image firmware refuses to load. An installed package ships it beside this file.")
endif()
if(NOT KICKOS_NO_GOT OR NOT EXISTS "${KICKOS_NO_GOT}")
  message(FATAL_ERROR "KickOS x86_64: KICKOS_NO_GOT names no file ('${KICKOS_NO_GOT}'). "
    "A global-offset-table load survives this link as a LOAD of the symbol's own bytes, so "
    "the guard runs before every image. An installed package ships it beside this file.")
endif()
if(NOT KICKOS_WEAK_UNDEF OR NOT EXISTS "${KICKOS_WEAK_UNDEF}")
  message(FATAL_ERROR "KickOS x86_64: KICKOS_WEAK_UNDEF names no file ('${KICKOS_WEAK_UNDEF}'). "
    "A reference to an undefined weak symbol survives this link as address 0, so the guard "
    "runs before every image. An installed package ships it beside this file.")
endif()

if(NOT KICKOS_X86_64_KREL OR NOT EXISTS "${KICKOS_X86_64_KREL}")
  message(FATAL_ERROR "KickOS x86_64: KICKOS_X86_64_KREL names no file ('${KICKOS_X86_64_KREL}'). "
    "The boot relocates the app window from the copy of .reloc it carries, so an image linked "
    "without it refuses to boot. An installed package ships it beside this file.")
endif()

function(_kickos_x86_64_toolchain_archive out name)
  separate_arguments(_flags NATIVE_COMMAND "${CMAKE_C_FLAGS}")
  execute_process(COMMAND "${CMAKE_C_COMPILER}" ${_flags} "-print-file-name=${name}"
                  OUTPUT_VARIABLE _path OUTPUT_STRIP_TRAILING_WHITESPACE
                  RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0 OR NOT IS_ABSOLUTE "${_path}" OR NOT EXISTS "${_path}")
    message(FATAL_ERROR "KickOS x86_64: ${CMAKE_C_COMPILER} -print-file-name=${name} found no "
      "${name} ('${_path}'). Every application image links the KickOS toolchain's libraries "
      "by path, ld running without a compiler driver; the x86_64-elf package installs them.")
  endif()
  set(${out} "${_path}" PARENT_SCOPE)
endfunction()
foreach(_kos_a c m gcc stdc++ supc++)
  _kickos_x86_64_toolchain_archive(_kos_lib "lib${_kos_a}.a")
  list(APPEND _kos_x86_64_libs "${_kos_lib}")
endforeach()
list(GET _kos_x86_64_libs 0 1 2 _kos_x86_64_libc)
list(GET _kos_x86_64_libs 3 4 _kos_x86_64_libcxx)
set(KICKOS_X86_64_LIBC "${_kos_x86_64_libc}" CACHE INTERNAL
    "The toolchain's libc.a, libm.a and libgcc.a, which every x86_64 application image links")
set(KICKOS_X86_64_LIBCXX "${_kos_x86_64_libcxx}" CACHE INTERNAL
    "The toolchain's libstdc++.a and libsupc++.a, which a full-C++ image links")

# Promoted so the function reads them whatever directory a consumer calls it from.
set(KICKOS_X86_64_PE_SCRIPT "${KICKOS_X86_64_PE_SCRIPT}" CACHE INTERNAL
    "The PE section script every x86_64 image links with")
set(KICKOS_X86_64_KREL "${KICKOS_X86_64_KREL}" CACHE INTERNAL
    "The relocation-copy tool every x86_64 application link runs")
set(KICKOS_NO_GOT "${KICKOS_NO_GOT}" CACHE INTERNAL
    "The global-offset-table guard refused before every x86_64 link")
set(KICKOS_WEAK_UNDEF "${KICKOS_WEAK_UNDEF}" CACHE INTERNAL
    "The undefined-weak-reference guard refused before every x86_64 link")

# --subsystem=10 makes the file an EFI APPLICATION; the entry symbol is the one firmware
# calls, on the Microsoft x64 convention.
# --no-insert-timestamp keeps the image byte-identical across builds of one tree.
# -T is required: the emulation's internal script names no wildcard for the data sections this
# arch compiles, and past about seventy PE sections firmware refuses to load the image at all.
# -b names the INPUT format, and binutils 2.42 needs it: under this emulation that ld reads an
# ELF archive's symbol index and still extracts no member for an undefined symbol, so every
# image linking the arch and chip archives fails undefined while the object-only images link.
# It is byte-identical on 2.47, which extracts either way, so only a CI runner catches its loss.
# -u _exit force-links the C library's exit stub, as the fleet's -Wl,-u,_exit does.
set(KICKOS_X86_64_LDFLAGS -m i386pep --subsystem=10 --image-base=0x400000
                          -e efi_main --no-insert-timestamp
                          -b elf64-x86-64
                          -u _exit
                          -T "${KICKOS_X86_64_PE_SCRIPT}"
    CACHE INTERNAL "The ld flags every x86_64 PE32+ image links with")
# The PE script sizes the heap by the KICKOS_USER_HEAP_SIZE link symbol. A link whose closure
# reaches KickOS::kernel leaves it to its system target; every other link takes it from the knob.
kickos_heap_defsym(KICKOS_X86_64_HEAP_LDFLAGS "${KICKOS_USER_HEAP_SIZE}")
set(KICKOS_X86_64_HEAP_LDFLAGS "${KICKOS_X86_64_HEAP_LDFLAGS}" CACHE INTERNAL
    "The ld flags defining KICKOS_USER_HEAP_SIZE for a link no system target defines it for")

# kickos_x86_64_link_image(<name> [REBASED <base>])
#   REBASED links the same inputs a second time as <name>_rebased.efi at <base>, recorded in the
#   target's KICKOS_REBASED_IMAGE_FILE: a base firmware cannot honour is the only way a run
#   reaches the case where firmware's own relocation pass moved the image.
function(kickos_x86_64_link_image name)
  cmake_parse_arguments(XI "" "REBASED" "" ${ARGN})
  set(_img "${CMAKE_CURRENT_BINARY_DIR}/${name}.efi")
  if(XI_REBASED)
    set(_img "${CMAKE_CURRENT_BINARY_DIR}/${name}_rebased.efi")
    set_target_properties(${name} PROPERTIES KICKOS_REBASED_IMAGE_FILE "${_img}")
  else()
    set_target_properties(${name} PROPERTIES KICKOS_IMAGE_FILE "${_img}")
  endif()
  kickos_image_leaves(${name} _leaves)
  if("kernel" IN_LIST _leaves)
    # Its system target may be defined after this call, as KickOS::system_default is in tree
    # after every app, so the link is written once every target of the configure exists, and
    # its image target is defined in the top directory.
    cmake_language(EVAL CODE "cmake_language(DEFER DIRECTORY [[${CMAKE_SOURCE_DIR}]] CALL \
      _kickos_x86_64_link_image_now [[${name}]] [[${_img}]] [[${XI_REBASED}]])")
    return()
  endif()
  _kickos_x86_64_link_image_now(${name} "${_img}" "${XI_REBASED}")
endfunction()

# The link kickos_x86_64_link_image writes. For KickOS::kernel it links the kernel's group, which
# has no init provider, and each system target's objects, archives, heap and asserts script.
function(_kickos_x86_64_link_image_now name _img rebased)
  set(_ldflags ${KICKOS_X86_64_LDFLAGS})
  kickos_image_leaves(${name} _leaves)
  set(_group_targets ${KICKOS_X86_64_APP_GROUP})
  set(_system_objects "")
  set(_system_archives "")
  set(_system_scripts "")
  if("kernel" IN_LIST _leaves)
    set(_group_targets ${KICKOS_X86_64_KERNEL_GROUP})
    list(APPEND _ldflags --require-defined=kickos_link_one_system_target)
    kickos_link_closure(${name} _closure)
    foreach(_t IN LISTS _closure)
      get_target_property(_heap ${_t} KICKOS_SYSTEM_HEAP)
      if(_heap STREQUAL "_heap-NOTFOUND")
        continue()
      endif()
      get_target_property(_objects ${_t} KICKOS_SYSTEM_OBJECTS)
      foreach(_o IN LISTS _objects)
        list(APPEND _system_objects "$<TARGET_OBJECTS:${_o}>")
      endforeach()
      get_target_property(_archives ${_t} KICKOS_SYSTEM_ARCHIVES)
      foreach(_a IN LISTS _archives)
        list(APPEND _system_archives "$<TARGET_FILE:${_a}>")
      endforeach()
      kickos_heap_defsym(_defsym "${_heap}")
      list(APPEND _ldflags ${_defsym})
      list(APPEND _system_scripts
        "$<TARGET_GENEX_EVAL:${_t},$<TARGET_PROPERTY:${_t},INTERFACE_LINK_DEPENDS>>")
    endforeach()
    # Two system targets name KickOS::init's objects twice, and ld links each argument it is given;
    # an archive two of them name is listed once too.
    list(REMOVE_DUPLICATES _system_objects)
    list(REMOVE_DUPLICATES _system_archives)
    # With no system target the PE script's heap reads 0, as cmake/kernel_leaf.ld gives the
    # other boards, so the link fails on the symbol it requires.
    if(NOT _system_scripts)
      list(APPEND _ldflags --defsym=KICKOS_USER_HEAP_SIZE=0)
    endif()
  else()
    if(NOT KICKOS_X86_64_HEAP_LDFLAGS)
      message(FATAL_ERROR "kickos_x86_64_link_image(${name}): KICKOS_USER_HEAP_SIZE is unset "
        "and the target links no system target, so the PE script has no heap to carve. The "
        "board configuration states it; a package records it.")
    endif()
    list(APPEND _ldflags ${KICKOS_X86_64_HEAP_LDFLAGS})
  endif()
  if(rebased)
    list(APPEND _ldflags "--image-base=${rebased}")
  endif()
  set(_boot_target KickOS::kickos_x86_64_boot)
  if(KICKOS_KERNEL_CORES GREATER 1)
    set(_boot_target KickOS::kickos_x86_64_boot_ap)
  endif()

  if(NOT KICKOS_X86_64_LD)
    message(FATAL_ERROR "kickos_x86_64_link_image(${name}): no KICKOS_X86_64_LD. The image is "
      "linked by ld directly, the compiler driver having no PE+ emulation; "
      "cmake/toolchain-x86_64-uefi.cmake finds it, and a package ships that file.")
  endif()
  if(NOT CMAKE_READELF)
    message(FATAL_ERROR "kickos_x86_64_link_image(${name}): no CMAKE_READELF, so the "
      "global-offset-table guard would scan nothing and pass.")
  endif()
  if(NOT CMAKE_OBJCOPY OR NOT CMAKE_OBJDUMP)
    message(FATAL_ERROR "kickos_x86_64_link_image(${name}): no CMAKE_OBJCOPY or CMAKE_OBJDUMP, "
      "so the image could not carry the copy of its relocations the boot relocates from.")
  endif()

  # The two object libraries every application image carries, and the frame-pool decline where
  # this posture has one. Named rather than derived: out of tree each is an imported target and
  # a missing install rule would otherwise reach ld as an empty argument.
  foreach(_o ${_boot_target} KickOS::kickos_x86_64_landed_kernel)
    if(NOT TARGET ${_o})
      message(FATAL_ERROR "kickos_x86_64_link_image(${name}): '${_o}' is not a target, so the "
        "image would link without the UEFI handover or the kernel landing tail. "
        "cmake/x86_64_boot.cmake defines and exports it.")
    endif()
  endforeach()

  if(NOT _group_targets)
    message(FATAL_ERROR "kickos_x86_64_link_image(${name}): KICKOS_X86_64_APP_GROUP or "
      "KICKOS_X86_64_KERNEL_GROUP is empty, so the image would link no KickOS archive at all.")
  endif()
  set(_group_files ${_system_archives})
  foreach(_t IN LISTS _group_targets)
    if(NOT TARGET ${_t})
      message(FATAL_ERROR "kickos_x86_64_link_image(${name}): '${_t}' is in the KickOS link "
        "group but is not a target, so the image would link against a bare -l name.")
    endif()
    list(APPEND _group_files "$<TARGET_FILE:${_t}>")
  endforeach()
  # A full-C++ image takes the C++ runtime and the object that registers its unwind tables.
  set(_cxx_objects "")
  if("kernel" IN_LIST _leaves OR "kickos_cxx" IN_LIST _leaves)
    if(NOT TARGET KickOS::kickos_cxx_rt)
      message(FATAL_ERROR "kickos_x86_64_link_image(${name}): the target links a full-C++ leaf "
        "and KickOS::kickos_cxx_rt is no target, so the image would link no terminate handler "
        "and no unwind-table registration.")
    endif()
    set(_cxx_objects $<TARGET_OBJECTS:KickOS::kickos_cxx_rt>)
    list(APPEND _group_files ${KICKOS_X86_64_LIBCXX})
  endif()
  list(APPEND _group_files ${KICKOS_X86_64_LIBC})

  # The first link measures the base-relocation directory; the second carries a copy of it in
  # .krel, which pe_image.ld places after every section a fixup can sit in, so .reloc comes
  # out the same and the check refuses an image where it did not.
  set(_first "${_img}.first")
  set(_krel "${_img}.krel")
  set(_inputs $<TARGET_OBJECTS:${_boot_target}>
              $<TARGET_OBJECTS:KickOS::kickos_x86_64_landed_kernel>
              ${KICKOS_X86_64_APP_OBJECTS}
              $<TARGET_OBJECTS:${name}>
              ${_system_objects}
              ${_cxx_objects}
              --start-group ${_group_files} --end-group
              ${_system_scripts})

  # THE ARCHIVES ARE SCANNED TOO, not the image objects alone: a global-offset-table
  # relocation on this board lands in libkickos_kernel.a, which the objects do not show.
  # This reads what the image links; the toolchain package runs both guards over every archive
  # it installs when it is built (conan/toolchain/conanfile.py).
  add_custom_command(
    OUTPUT "${_img}"
    COMMAND "${KICKOS_NO_GOT}" "${CMAKE_READELF}"
            $<TARGET_OBJECTS:${_boot_target}>
            $<TARGET_OBJECTS:KickOS::kickos_x86_64_landed_kernel>
            ${KICKOS_X86_64_APP_OBJECTS}
            $<TARGET_OBJECTS:${name}>
            ${_system_objects}
            ${_cxx_objects}
            ${_group_files}
    COMMAND "${KICKOS_WEAK_UNDEF}" "${CMAKE_READELF}"
            $<TARGET_OBJECTS:${_boot_target}>
            $<TARGET_OBJECTS:KickOS::kickos_x86_64_landed_kernel>
            ${KICKOS_X86_64_APP_OBJECTS}
            $<TARGET_OBJECTS:${name}>
            ${_system_objects}
            ${_cxx_objects}
            ${_group_files}
    # LC_ALL=C: the host's ld is localised, and the map it writes is read by gates that key
    # on its headings (tests/static/check_appdata_no_kernel.sh).
    COMMAND "${CMAKE_COMMAND}" -E env LC_ALL=C
            "${KICKOS_X86_64_LD}" ${_ldflags} -o "${_first}" ${_inputs}
    COMMAND "${KICKOS_X86_64_KREL}" extract "${CMAKE_OBJDUMP}" "${CMAKE_OBJCOPY}"
            "${_first}" "${_krel}.bin"
    COMMAND "${CMAKE_OBJCOPY}" -I binary -O elf64-x86-64 -B i386:x86-64
            --rename-section .data=.krel,alloc,load,readonly,data,contents
            "${_krel}.bin" "${_krel}.o"
    # -Map is required: only the map names the archive MEMBER each symbol resolved from,
    # which is what tests/static/check_seam_defaults.sh reads.
    COMMAND "${CMAKE_COMMAND}" -E env LC_ALL=C
            "${KICKOS_X86_64_LD}" ${_ldflags}
            -Map "${_img}.map"
            -o "${_img}"
            ${_inputs} "${_krel}.o"
    COMMAND "${KICKOS_X86_64_KREL}" check "${CMAKE_OBJDUMP}" "${CMAKE_OBJCOPY}"
            "${_first}" "${_img}"
    # The OBJECT FILES, not just the targets: a DEPENDS on an OBJECT library alone is
    # order-only, so an edited source rebuilt its object and left this link untaken.
    DEPENDS $<TARGET_OBJECTS:${_boot_target}>
            $<TARGET_OBJECTS:KickOS::kickos_x86_64_landed_kernel>
            ${KICKOS_X86_64_APP_OBJECTS}
            $<TARGET_OBJECTS:${name}>
            ${_system_objects}
            ${_cxx_objects}
            ${_group_files}
            ${_system_scripts}
            "${KICKOS_NO_GOT}"
            "${KICKOS_WEAK_UNDEF}"
            "${KICKOS_X86_64_KREL}"
            "${KICKOS_X86_64_PE_SCRIPT}"
    BYPRODUCTS "${_img}.map" "${_first}" "${_krel}.bin" "${_krel}.o"
    COMMENT "x86_64: linking the PE32+ UEFI application ${_img}"
    COMMAND_EXPAND_LISTS
    VERBATIM)
  if(rebased)
    add_custom_target(${name}_rebased_image ALL DEPENDS "${_img}")
    return()
  endif()
  add_custom_target(${name}_image ALL DEPENDS "${_img}")
endfunction()
