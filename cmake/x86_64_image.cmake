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
# and the link inputs, which are a property of the configure rather than of the site:
#   KICKOS_X86_64_APP_GROUP    the archive group, as target names
#   KICKOS_X86_64_APP_OBJECTS  the extra objects, as $<TARGET_OBJECTS:> text
# Those two are read at CALL time, so an includer may state them after this file.
#
# The three object libraries named below are targets in tree and imported ones from
# KickOSTargets.cmake in a package; $<TARGET_OBJECTS:> reads both.

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

# Promoted so the function reads them whatever directory a consumer calls it from.
set(KICKOS_X86_64_PE_SCRIPT "${KICKOS_X86_64_PE_SCRIPT}" CACHE INTERNAL
    "The PE section script every x86_64 image links with")
set(KICKOS_NO_GOT "${KICKOS_NO_GOT}" CACHE INTERNAL
    "The global-offset-table guard refused before every x86_64 link")

# --subsystem=10 makes the file an EFI APPLICATION; the entry symbol is the one firmware
# calls, on the Microsoft x64 convention.
# --no-insert-timestamp keeps the image byte-identical across builds of one tree.
# -T is required: the emulation's internal script names no wildcard for the data sections this
# arch compiles, and past about seventy PE sections firmware refuses to load the image at all.
# -b names the INPUT format, and binutils 2.42 needs it: under this emulation that ld reads an
# ELF archive's symbol index and still extracts no member for an undefined symbol, so every
# image linking the arch and chip archives fails undefined while the object-only images link.
# It is byte-identical on 2.47, which extracts either way, so only a CI runner catches its loss.
set(KICKOS_X86_64_LDFLAGS -m i386pep --subsystem=10 --image-base=0x400000
                          -e efi_main --no-insert-timestamp
                          -b elf64-x86-64
                          -T "${KICKOS_X86_64_PE_SCRIPT}"
    CACHE INTERNAL "The ld flags every x86_64 PE32+ image links with")

function(kickos_x86_64_link_image name)
  set(_img "${CMAKE_CURRENT_BINARY_DIR}/${name}.efi")
  set(_boot_target kickos_x86_64_boot)
  if(KICKOS_KERNEL_CORES GREATER 1)
    set(_boot_target kickos_x86_64_boot_ap)
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

  # The two object libraries every application image carries, and the frame-pool decline where
  # this posture has one. Named rather than derived: out of tree each is an imported target and
  # a missing install rule would otherwise reach ld as an empty argument.
  foreach(_o ${_boot_target} kickos_x86_64_landed_kernel)
    if(NOT TARGET ${_o})
      message(FATAL_ERROR "kickos_x86_64_link_image(${name}): '${_o}' is not a target, so the "
        "image would link without the UEFI handover or the kernel landing tail. In tree "
        "cmake/x86_64_boot.cmake defines it; a package must install(TARGETS) it into "
        "KickOSTargets.")
    endif()
  endforeach()

  if(NOT KICKOS_X86_64_APP_GROUP)
    message(FATAL_ERROR "kickos_x86_64_link_image(${name}): KICKOS_X86_64_APP_GROUP is empty, "
      "so the image would link no KickOS archive at all.")
  endif()
  set(_group_files "")
  foreach(_t IN LISTS KICKOS_X86_64_APP_GROUP)
    if(NOT TARGET ${_t})
      message(FATAL_ERROR "kickos_x86_64_link_image(${name}): '${_t}' is in the KickOS link "
        "group but is not a target, so the image would link against a bare -l name.")
    endif()
    list(APPEND _group_files "$<TARGET_FILE:${_t}>")
  endforeach()

  # THE ARCHIVES ARE SCANNED TOO, not the image objects alone: a global-offset-table
  # relocation on this board lands in libkickos_kernel.a, which the objects do not show.
  add_custom_command(
    OUTPUT "${_img}"
    COMMAND "${KICKOS_NO_GOT}" "${CMAKE_READELF}"
            $<TARGET_OBJECTS:${_boot_target}>
            $<TARGET_OBJECTS:kickos_x86_64_landed_kernel>
            ${KICKOS_X86_64_APP_OBJECTS}
            $<TARGET_OBJECTS:${name}>
            ${_group_files}
    # -Map is required: only the map names the archive MEMBER each symbol resolved from,
    # which is what tests/static/check_seam_defaults.sh reads.
    COMMAND "${KICKOS_X86_64_LD}" ${KICKOS_X86_64_LDFLAGS}
            -Map "${_img}.map"
            -o "${_img}"
            $<TARGET_OBJECTS:${_boot_target}>
            $<TARGET_OBJECTS:kickos_x86_64_landed_kernel>
            ${KICKOS_X86_64_APP_OBJECTS}
            $<TARGET_OBJECTS:${name}>
            --start-group ${_group_files} --end-group
    # The OBJECT FILES, not just the targets: a DEPENDS on an OBJECT library alone is
    # order-only, so an edited source rebuilt its object and left this link untaken.
    DEPENDS $<TARGET_OBJECTS:${_boot_target}>
            $<TARGET_OBJECTS:kickos_x86_64_landed_kernel>
            ${KICKOS_X86_64_APP_OBJECTS}
            $<TARGET_OBJECTS:${name}>
            ${_group_files}
            "${KICKOS_NO_GOT}"
            "${KICKOS_X86_64_PE_SCRIPT}"
    BYPRODUCTS "${_img}.map"
    COMMENT "x86_64: linking the PE32+ UEFI application ${name}.efi"
    COMMAND_EXPAND_LISTS
    VERBATIM)
  add_custom_target(${name}_image ALL DEPENDS "${_img}")
  set_target_properties(${name} PROPERTIES KICKOS_IMAGE_FILE "${_img}")
endfunction()
