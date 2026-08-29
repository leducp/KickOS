# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The x86_64 images (docs/design-m6-mmu.md section 5, M6.4). Every one is a PE32+ UEFI
# application, and CMake cannot drive `ld -m i386pep` as a linker for a target, so each image
# is one custom command.
#
# Two image families:
#   - X1 and X2 carry NO kernel and no interrupt source, over an explicit OBJECT subset, and
#     take the declining kickos_x86_64_isr fallback so every vector reports.
#   - X3 carries the real arch and chip ARCHIVES. Archives, because a fallback TU sits beside
#     the chip's own definition of the same symbol and member order inside a group is what
#     resolves that; linking the objects raw would be a duplicate definition.
#
# The X3 image supplies kickos_isr_timer, kickos_isr_irq and kickos_thread_return itself; the
# syscall entry every app needs is step X4's.

# Refused before each link: an unrelaxed global-offset-table load is a CLEAN link and a fault
# much later (see the script's own header).
set(KICKOS_NO_GOT "${CMAKE_CURRENT_SOURCE_DIR}/tools/check-x86_64-no-got.sh")

set(KICKOS_X86_64_DIR "${CMAKE_CURRENT_SOURCE_DIR}/arch/x86/x86_64")
set(KICKOS_Q35_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/arch/x86/chip/q35")

# arch/include is here for the X3 probe alone; the kernel-free images reach only the two
# backend directories.
set(KICKOS_X86_64_INCLUDES
  "${KICKOS_X86_64_DIR}/include"
  "${KICKOS_Q35_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/arch/include")

# The UEFI handover, shared by every image. Its tail is the kickos_x86_64_landed seam, which
# each family defines differently.
add_library(kickos_x86_64_boot OBJECT "${KICKOS_X86_64_DIR}/entry_x86_64.cc")
kickos_apply_freestanding(kickos_x86_64_boot)
target_include_directories(kickos_x86_64_boot PRIVATE ${KICKOS_X86_64_INCLUDES})

# The COM1 primitives, which the kernel-free images print through directly.
add_library(kickos_x86_64_boot_com1 OBJECT "${KICKOS_Q35_DIR}/com1_q35.cc")
kickos_apply_freestanding(kickos_x86_64_boot_com1)
target_include_directories(kickos_x86_64_boot_com1 PRIVATE ${KICKOS_X86_64_INCLUDES})

# The kernel-side symbols the arch and chip archives reference. Every body declines; the
# kernel-free images below would not link without it, and the X4 image defines the same names
# itself instead.
add_library(kickos_x86_64_nokernel OBJECT "${KICKOS_X86_64_DIR}/nokernel_x86_64.cc")
kickos_apply_freestanding(kickos_x86_64_nokernel)
target_include_directories(kickos_x86_64_nokernel PRIVATE ${KICKOS_X86_64_INCLUDES})

# X2's subset: the tables, the report and the declining interrupt fallback.
add_library(kickos_x86_64_x2 OBJECT
  "${KICKOS_X86_64_DIR}/desc_x86_64.cc"
  "${KICKOS_X86_64_DIR}/fault_x86_64.cc"
  "${KICKOS_X86_64_DIR}/trap_x86_64.S"
  "${KICKOS_X86_64_DIR}/kickos_x86_64_isr_default.cc"
  "${KICKOS_X86_64_DIR}/landed_x2_x86_64.cc")
kickos_apply_freestanding(kickos_x86_64_x2)
target_include_directories(kickos_x86_64_x2 PRIVATE ${KICKOS_X86_64_INCLUDES})

# X3's landing tail and its arms.
add_library(kickos_x86_64_probe3 OBJECT "${KICKOS_X86_64_DIR}/probe3_x86_64.cc")
kickos_apply_freestanding(kickos_x86_64_probe3)
target_include_directories(kickos_x86_64_probe3 PRIVATE ${KICKOS_X86_64_INCLUDES})

# --subsystem=10 makes the file an EFI APPLICATION; the entry symbol is the one firmware
# calls, on the Microsoft x64 convention.
# --no-insert-timestamp keeps the image byte-identical across builds of one tree.
# -T is required: the emulation's internal script names no wildcard for the data sections this
# arch compiles, and past about seventy PE sections firmware refuses to load the image at all.
set(KICKOS_X86_64_PE_SCRIPT "${KICKOS_X86_64_DIR}/pe_image.ld")
set(KICKOS_X86_64_LDFLAGS -m i386pep --subsystem=10 --image-base=0x400000
                          -e efi_main --no-insert-timestamp
                          -T "${KICKOS_X86_64_PE_SCRIPT}")

# ONE IMAGE PER FAULT CLASS. The report ends the image, so a run witnesses exactly one class;
# only arch/x86/x86_64/probe_x86_64.cc differs between them.
set(KICKOS_X2_CLASSES none ud pf pfw pfx gp sel de soft df)

set(KICKOS_X86_64_IMAGES "")
foreach(_cls IN LISTS KICKOS_X2_CLASSES)
  string(TOUPPER "${_cls}" _CLS)
  add_library(kickos_x86_64_probe_${_cls} OBJECT "${KICKOS_X86_64_DIR}/probe_x86_64.cc")
  kickos_apply_freestanding(kickos_x86_64_probe_${_cls})
  target_include_directories(kickos_x86_64_probe_${_cls} PRIVATE ${KICKOS_X86_64_INCLUDES})
  target_compile_definitions(kickos_x86_64_probe_${_cls}
    PRIVATE "KICKOS_X2_FAULT=KICKOS_X2_FAULT_${_CLS}")

  if(_cls STREQUAL "none")
    set(_img "${PROJECT_BINARY_DIR}/kickos_x86_64.efi")
  else()
    set(_img "${PROJECT_BINARY_DIR}/kickos_x86_64_fault_${_cls}.efi")
  endif()

  add_custom_command(
    OUTPUT "${_img}"
    COMMAND "${KICKOS_NO_GOT}" "${CMAKE_READELF}"
            $<TARGET_OBJECTS:kickos_x86_64_boot>
            $<TARGET_OBJECTS:kickos_x86_64_x2>
            $<TARGET_OBJECTS:kickos_x86_64_boot_com1>
            $<TARGET_OBJECTS:kickos_x86_64_nokernel>
            $<TARGET_OBJECTS:kickos_x86_64_probe_${_cls}>
    COMMAND "${KICKOS_X86_64_LD}" ${KICKOS_X86_64_LDFLAGS}
            -o "${_img}"
            $<TARGET_OBJECTS:kickos_x86_64_boot>
            $<TARGET_OBJECTS:kickos_x86_64_x2>
            $<TARGET_OBJECTS:kickos_x86_64_boot_com1>
            $<TARGET_OBJECTS:kickos_x86_64_nokernel>
            $<TARGET_OBJECTS:kickos_x86_64_probe_${_cls}>
    # The OBJECT FILES, not just the targets: a DEPENDS on an OBJECT library alone is
    # order-only, so an edited source rebuilds its object and leaves this link untaken.
    DEPENDS $<TARGET_OBJECTS:kickos_x86_64_boot>
            $<TARGET_OBJECTS:kickos_x86_64_x2>
            $<TARGET_OBJECTS:kickos_x86_64_boot_com1>
            $<TARGET_OBJECTS:kickos_x86_64_nokernel>
            $<TARGET_OBJECTS:kickos_x86_64_probe_${_cls}>
            "${KICKOS_NO_GOT}"
            "${KICKOS_X86_64_PE_SCRIPT}"
    COMMENT "x86_64: linking the PE32+ UEFI application ${_img}"
    COMMAND_EXPAND_LISTS
    VERBATIM)
  list(APPEND KICKOS_X86_64_IMAGES "${_img}")
  set(KICKOS_X86_64_IMAGE_${_cls} "${_img}")
endforeach()

set(KICKOS_X1_IMAGE "${KICKOS_X86_64_IMAGE_none}")

# The X3 image. The group is what the application ladder also links with, so a fallback TU
# and the chip's own definition of the same seam resolve here exactly as they would there.
set(KICKOS_X3_IMAGE "${PROJECT_BINARY_DIR}/kickos_x86_64_x3.efi")
add_custom_command(
  OUTPUT "${KICKOS_X3_IMAGE}"
  COMMAND "${KICKOS_NO_GOT}" "${CMAKE_READELF}"
          $<TARGET_OBJECTS:kickos_x86_64_boot>
          $<TARGET_OBJECTS:kickos_x86_64_probe3>
          $<TARGET_OBJECTS:kickos_x86_64_nokernel>
          $<TARGET_OBJECTS:kickos_arch_x86_64>
          $<TARGET_OBJECTS:kickos_chip_q35>
  COMMAND "${KICKOS_X86_64_LD}" ${KICKOS_X86_64_LDFLAGS}
          -o "${KICKOS_X3_IMAGE}"
          $<TARGET_OBJECTS:kickos_x86_64_boot>
          $<TARGET_OBJECTS:kickos_x86_64_probe3>
          $<TARGET_OBJECTS:kickos_x86_64_nokernel>
          --start-group
          "$<TARGET_FILE:kickos_chip_q35>"
          "$<TARGET_FILE:kickos_arch_x86_64>"
          --end-group
  # See the per-class link above for why the OBJECTS and not the targets.
  DEPENDS $<TARGET_OBJECTS:kickos_x86_64_boot>
          $<TARGET_OBJECTS:kickos_x86_64_probe3>
          $<TARGET_OBJECTS:kickos_x86_64_nokernel>
          "$<TARGET_FILE:kickos_chip_q35>"
          "$<TARGET_FILE:kickos_arch_x86_64>"
          "${KICKOS_NO_GOT}"
          "${KICKOS_X86_64_PE_SCRIPT}"
  COMMENT "x86_64: linking the PE32+ UEFI application ${KICKOS_X3_IMAGE}"
  COMMAND_EXPAND_LISTS
  VERBATIM)
list(APPEND KICKOS_X86_64_IMAGES "${KICKOS_X3_IMAGE}")

# X4's landing tail and its arms.
add_library(kickos_x86_64_probe4 OBJECT "${KICKOS_X86_64_DIR}/probe4_x86_64.cc"
                                        "${KICKOS_X86_64_DIR}/probe4_x86_64.S")
kickos_apply_freestanding(kickos_x86_64_probe4)
target_include_directories(kickos_x86_64_probe4 PRIVATE ${KICKOS_X86_64_INCLUDES})

set(KICKOS_X4_IMAGE "${PROJECT_BINARY_DIR}/kickos_x86_64_x4.efi")
add_custom_command(
  OUTPUT "${KICKOS_X4_IMAGE}"
  COMMAND "${KICKOS_NO_GOT}" "${CMAKE_READELF}"
          $<TARGET_OBJECTS:kickos_x86_64_boot>
          $<TARGET_OBJECTS:kickos_x86_64_probe4>
          $<TARGET_OBJECTS:kickos_arch_x86_64>
          $<TARGET_OBJECTS:kickos_chip_q35>
  COMMAND "${KICKOS_X86_64_LD}" ${KICKOS_X86_64_LDFLAGS}
          -o "${KICKOS_X4_IMAGE}"
          $<TARGET_OBJECTS:kickos_x86_64_boot>
          $<TARGET_OBJECTS:kickos_x86_64_probe4>
          --start-group
          "$<TARGET_FILE:kickos_chip_q35>"
          "$<TARGET_FILE:kickos_arch_x86_64>"
          --end-group
  DEPENDS $<TARGET_OBJECTS:kickos_x86_64_boot>
          $<TARGET_OBJECTS:kickos_x86_64_probe4>
          "$<TARGET_FILE:kickos_chip_q35>"
          "$<TARGET_FILE:kickos_arch_x86_64>"
          "${KICKOS_NO_GOT}"
          "${KICKOS_X86_64_PE_SCRIPT}"
  COMMENT "x86_64: linking the PE32+ UEFI application ${KICKOS_X4_IMAGE}"
  COMMAND_EXPAND_LISTS
  VERBATIM)
list(APPEND KICKOS_X86_64_IMAGES "${KICKOS_X4_IMAGE}")

# X5's landing tail and its arms. The image links the archives and defines the kernel-side
# symbols itself: the fault reporter has to be this file's for a deliberate translation fault
# to be resumable, so the declining fallback must not be in the link.
add_library(kickos_x86_64_probe5 OBJECT "${KICKOS_X86_64_DIR}/probe5_x86_64.cc"
                                        "${KICKOS_X86_64_DIR}/probe5_x86_64.S")
kickos_apply_freestanding(kickos_x86_64_probe5)
target_include_directories(kickos_x86_64_probe5 PRIVATE ${KICKOS_X86_64_INCLUDES})

set(KICKOS_X5_IMAGE "${PROJECT_BINARY_DIR}/kickos_x86_64_x5.efi")
add_custom_command(
  OUTPUT "${KICKOS_X5_IMAGE}"
  COMMAND "${KICKOS_NO_GOT}" "${CMAKE_READELF}"
          $<TARGET_OBJECTS:kickos_x86_64_boot>
          $<TARGET_OBJECTS:kickos_x86_64_probe5>
          $<TARGET_OBJECTS:kickos_arch_x86_64>
          $<TARGET_OBJECTS:kickos_chip_q35>
  COMMAND "${KICKOS_X86_64_LD}" ${KICKOS_X86_64_LDFLAGS}
          -o "${KICKOS_X5_IMAGE}"
          $<TARGET_OBJECTS:kickos_x86_64_boot>
          $<TARGET_OBJECTS:kickos_x86_64_probe5>
          --start-group
          "$<TARGET_FILE:kickos_chip_q35>"
          "$<TARGET_FILE:kickos_arch_x86_64>"
          --end-group
  # See the per-class link above for why the OBJECTS and not the targets.
  DEPENDS $<TARGET_OBJECTS:kickos_x86_64_boot>
          $<TARGET_OBJECTS:kickos_x86_64_probe5>
          "$<TARGET_FILE:kickos_chip_q35>"
          "$<TARGET_FILE:kickos_arch_x86_64>"
          "${KICKOS_NO_GOT}"
          "${KICKOS_X86_64_PE_SCRIPT}"
  COMMENT "x86_64: linking the PE32+ UEFI application ${KICKOS_X5_IMAGE}"
  COMMAND_EXPAND_LISTS
  VERBATIM)
list(APPEND KICKOS_X86_64_IMAGES "${KICKOS_X5_IMAGE}")

set(KICKOS_X1_ESP "${PROJECT_BINARY_DIR}/esp.img")
add_custom_command(
  OUTPUT "${KICKOS_X1_ESP}"
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/esp-x86_64.sh"
          "${KICKOS_X1_IMAGE}" "${KICKOS_X1_ESP}"
  DEPENDS "${KICKOS_X1_IMAGE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/esp-x86_64.sh"
  COMMENT "x86_64: building the EFI system partition ${KICKOS_X1_ESP}"
  VERBATIM)

add_custom_target(kickos_x1_image ALL DEPENDS ${KICKOS_X86_64_IMAGES} "${KICKOS_X1_ESP}")

# The witnesses are targets, not ctest cases: X1 through X3 are boot paths, and the suite
# builds no application for this board yet.
add_custom_target(x1-run
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/run-qemu-x86_64.sh"
          "${KICKOS_X1_IMAGE}" "${PROJECT_BINARY_DIR}/x1run"
  DEPENDS "${KICKOS_X1_IMAGE}"
  USES_TERMINAL
  COMMENT "x86_64: booting ${KICKOS_X1_IMAGE} under qemu-system-x86_64 with UEFI firmware")

set(_x2_cmds "")
foreach(_cls IN LISTS KICKOS_X2_CLASSES)
  list(APPEND _x2_cmds COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/run-qemu-x86_64-x2.sh"
       "${_cls}" "${KICKOS_X86_64_IMAGE_${_cls}}" "${PROJECT_BINARY_DIR}/x2run/${_cls}")
endforeach()
add_custom_target(x2-run
  ${_x2_cmds}
  DEPENDS ${KICKOS_X86_64_IMAGES}
  USES_TERMINAL
  COMMENT "x86_64: taking the X2 descriptor and fault-report witness, one boot per class")

add_custom_target(x4-run
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/run-qemu-x86_64-x4.sh"
          "${KICKOS_X4_IMAGE}" "${PROJECT_BINARY_DIR}/x4run"
  DEPENDS "${KICKOS_X4_IMAGE}"
  USES_TERMINAL
  COMMENT "x86_64: taking the X4 ring-3 and syscall witness")

add_custom_target(x5-run
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/run-qemu-x86_64-x5.sh"
          "${KICKOS_X5_IMAGE}" "${PROJECT_BINARY_DIR}/x5run"
  DEPENDS "${KICKOS_X5_IMAGE}"
  USES_TERMINAL
  COMMENT "x86_64: taking the X5 address-space witness")

add_custom_target(x3-run
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/run-qemu-x86_64-x3.sh"
          "${KICKOS_X3_IMAGE}" "${PROJECT_BINARY_DIR}/x3run"
  DEPENDS "${KICKOS_X3_IMAGE}"
  USES_TERMINAL
  COMMENT "x86_64: taking the X3 console, timer, interrupt, switch and idle witness")

# The guard's own positive control, registered here because the guard is this board's alone.
# AR is not in the toolchain file's find_program set, CMake resolving it itself.
if(KICKOS_BUILD_TESTS)
  add_test(NAME x86_64_no_got_selftest
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_x86_64_no_got_selftest.sh"
            "${KICKOS_NO_GOT}" "${CMAKE_READELF}" "${CMAKE_C_COMPILER}" "${CMAKE_AR}"
            ${KICKOS_MCPU_FLAGS} -ffreestanding -fpie -mcmodel=small)
  set_tests_properties(x86_64_no_got_selftest PROPERTIES TIMEOUT 60 LABELS host)

  # The vector and x87 census over what this board compiled and linked. It walks
  # PROJECT_BINARY_DIR itself rather than taking a list of targets, so an object library added
  # later is in the corpus by being built.
  add_test(NAME x86_64_no_vector
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_x86_64_no_vector.sh"
            "${CMAKE_OBJDUMP}" "${CMAKE_C_COMPILER}" "${PROJECT_BINARY_DIR}")
  set_tests_properties(x86_64_no_vector PROPERTIES TIMEOUT 300 LABELS host)

  # The direction flag the interrupt entry clears before it calls C. Delivery through a gate
  # leaves that flag as the interrupted code set it, so the instruction is the whole of the
  # protection and its deletion is otherwise silent.
  add_test(NAME x86_64_entry_cld
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_x86_64_entry_cld.sh"
            "${CMAKE_OBJDUMP}" "${CMAKE_C_COMPILER}" "${PROJECT_BINARY_DIR}")
  set_tests_properties(x86_64_entry_cld PROPERTIES TIMEOUT 60 LABELS host)
endif()

message(STATUS "KickOS: x86_64 libraries plus the X1 through X5 images; `ninja x1-run`, "
               "`ninja x2-run`, `ninja x3-run`, `ninja x4-run` and `ninja x5-run` take the "
               "witnesses")

# ---------------------------------------------------------------------------
# The APPLICATION images. M6.4 (docs/design-m6-mmu.md section 5): the same eight archives
# the root CMakeLists groups for every other board, linked here rather than through the
# compiler driver, which cannot select the PE+ emulation.
#
# kickos_add_application makes the app target an OBJECT library on this arch and calls the
# function below, which writes the image and records its path on the target as
# KICKOS_IMAGE_FILE (cmake/kickos.cmake).
# ---------------------------------------------------------------------------

# The boot tail of an image that carries the kernel: RAM publish, the kernel-owned ctor
# window, arch_init, kmain.
add_library(kickos_x86_64_landed_kernel OBJECT
  "${KICKOS_X86_64_DIR}/landed_kernel_x86_64.cc")
kickos_apply_freestanding(kickos_x86_64_landed_kernel)
target_include_directories(kickos_x86_64_landed_kernel PRIVATE ${KICKOS_X86_64_INCLUDES}
  "${CMAKE_CURRENT_SOURCE_DIR}/kernel/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/system/include")

# The frame-pool decline, on the application-image link line and nowhere else. The chip's
# arch_init adopts the live translation regime, so the map editor is in every link carrying
# the chip archive, while kernel/mem/frame_pool.cc is compiled only under KICKOS_HAVE_ASPACE.
# Conditional, because a chip that later selects HAS_ASPACE gets the kernel's real definitions
# and a second pair here would be a duplicate symbol.
set(_kos_x86_64_app_objects "")
if(NOT KICKOS_HAVE_ASPACE)
  add_library(kickos_x86_64_nopool OBJECT "${KICKOS_X86_64_DIR}/nopool_x86_64.cc")
  kickos_apply_freestanding(kickos_x86_64_nopool)
  target_include_directories(kickos_x86_64_nopool PRIVATE ${KICKOS_X86_64_INCLUDES})
  list(APPEND _kos_x86_64_app_objects "$<TARGET_OBJECTS:kickos_x86_64_nopool>")
endif()
set(KICKOS_X86_64_APP_OBJECTS "${_kos_x86_64_app_objects}" CACHE INTERNAL
    "Extra objects every x86_64 application image links, beside the app's own")

# The link group, captured at include time and comma-split into a real list: $<LINK_GROUP:>
# takes it comma-separated, and an unsplit string reaches the loop below as one name. A
# function reads its variables from the CALLING scope, and the caller is a user/apps
# subdirectory; the cache entry is what keeps the two from having to agree by accident.
string(REPLACE "," ";" _kos_x86_64_group "${_kickos_group}")
set(KICKOS_X86_64_APP_GROUP "${_kos_x86_64_group}" CACHE INTERNAL
    "The archive group an x86_64 application image links, from the root CMakeLists")

function(kickos_x86_64_link_image name)
  set(_img "${CMAKE_CURRENT_BINARY_DIR}/${name}.efi")

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
            $<TARGET_OBJECTS:kickos_x86_64_boot>
            $<TARGET_OBJECTS:kickos_x86_64_landed_kernel>
            ${KICKOS_X86_64_APP_OBJECTS}
            $<TARGET_OBJECTS:${name}>
            ${_group_files}
    # -Map is required: only the map names the archive MEMBER each symbol resolved from,
    # which is what tests/static/check_seam_defaults.sh reads.
    COMMAND "${KICKOS_X86_64_LD}" ${KICKOS_X86_64_LDFLAGS}
            -Map "${_img}.map"
            -o "${_img}"
            $<TARGET_OBJECTS:kickos_x86_64_boot>
            $<TARGET_OBJECTS:kickos_x86_64_landed_kernel>
            ${KICKOS_X86_64_APP_OBJECTS}
            $<TARGET_OBJECTS:${name}>
            --start-group ${_group_files} --end-group
    # The OBJECT FILES, not just the targets: a DEPENDS on an OBJECT library alone is
    # order-only, so an edited source rebuilt its object and left this link untaken.
    DEPENDS $<TARGET_OBJECTS:kickos_x86_64_boot>
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
