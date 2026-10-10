# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The x86_64 boot probe images, and the objects every application image links. Each probe is a
# PE32+ UEFI application of its own single link, one custom command through
# tools/x86_64-link.sh --one-pass; an application image is an executable, linked through the
# same tool by cmake/toolchain-x86_64-uefi.cmake's rule.
#
# Two probe families:
#   - X1 and X2 carry NO kernel and no interrupt source, over an explicit OBJECT subset, and
#     take the declining kickos_x86_64_isr fallback so every vector reports.
#   - X5 and X6 carry the real arch and chip ARCHIVES. Archives, because a fallback TU sits
#     beside the chip's own definition of the same symbol and member order inside a group is
#     what resolves that; linking the objects raw would be a duplicate definition.

# The link tool and what it runs: the two guards before every link, and the relocation copy an
# application image carries for the boot.
set(KICKOS_X86_64_LINK "${CMAKE_CURRENT_SOURCE_DIR}/tools/x86_64-link.sh")
set(KICKOS_NO_GOT "${CMAKE_CURRENT_SOURCE_DIR}/tools/check-x86_64-no-got.sh")
set(KICKOS_WEAK_UNDEF "${CMAKE_CURRENT_SOURCE_DIR}/tools/check-x86_64-weak-undef.sh")
set(KICKOS_X86_64_KREL "${CMAKE_CURRENT_SOURCE_DIR}/tools/x86_64-krel.sh")

set(KICKOS_X86_64_DIR "${CMAKE_CURRENT_SOURCE_DIR}/arch/x86/x86_64")
set(KICKOS_Q35_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/arch/x86/chip/q35")

set(KICKOS_X86_64_PE_SCRIPT "${KICKOS_X86_64_DIR}/pe_image.ld")

# A probe links no leaf, so it states the section script, the exit stub and the heap itself: it
# carves none.
kickos_heap_defsym(_kos_probe_heap 0)
set(KICKOS_X86_64_PROBE_LINK "${KICKOS_X86_64_LINK}" --one-pass
    "${KICKOS_X86_64_LD}" "${CMAKE_READELF}" "${CMAKE_OBJDUMP}" "${CMAKE_OBJCOPY}"
    "-Wl,-u,_exit" "-Wl,-T,${KICKOS_X86_64_PE_SCRIPT}" "-Wl,${_kos_probe_heap}")
set(KICKOS_X86_64_PROBE_DEPENDS "${KICKOS_X86_64_LINK}" "${KICKOS_NO_GOT}" "${KICKOS_WEAK_UNDEF}"
    "${KICKOS_X86_64_PE_SCRIPT}")

# arch/include is here for the images that link the archives; the kernel-free images reach only
# the two backend directories.
set(KICKOS_X86_64_INCLUDES
  "${KICKOS_X86_64_DIR}/include"
  "${KICKOS_Q35_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/arch/include")

# The UEFI handover, shared by every image. Its tail is the kickos_x86_64_landed seam, which
# each family defines differently. The first check must run on a base-level processor.
set_source_files_properties("${KICKOS_X86_64_DIR}/floor_x86_64.cc"
  PROPERTIES COMPILE_OPTIONS -march=x86-64)
add_library(kickos_x86_64_boot OBJECT "${KICKOS_X86_64_DIR}/entry_x86_64.cc"
                                      "${KICKOS_X86_64_DIR}/floor_x86_64.cc")
kickos_apply_freestanding(kickos_x86_64_boot)
target_include_directories(kickos_x86_64_boot PRIVATE ${KICKOS_X86_64_INCLUDES})

# The SMP boot witness reserves its SIPI page before the final UEFI memory-map
# read. Keep that behavior off the ordinary X1-X5 and application handover.
add_library(kickos_x86_64_boot_ap OBJECT "${KICKOS_X86_64_DIR}/entry_x86_64.cc"
                                         "${KICKOS_X86_64_DIR}/floor_x86_64.cc")
kickos_apply_freestanding(kickos_x86_64_boot_ap)
target_include_directories(kickos_x86_64_boot_ap PRIVATE ${KICKOS_X86_64_INCLUDES})
target_compile_definitions(kickos_x86_64_boot_ap PRIVATE KICKOS_X86_64_AP_BOOT=1)

# The COM1 primitives, which the kernel-free images print through directly.
add_library(kickos_x86_64_boot_com1 OBJECT "${KICKOS_Q35_DIR}/com1_q35.cc")
kickos_apply_freestanding(kickos_x86_64_boot_com1)
target_include_directories(kickos_x86_64_boot_com1 PRIVATE ${KICKOS_X86_64_INCLUDES})

# The kernel-side symbols the arch and chip archives reference. Every body declines; the
# kernel-free images below would not link without it, and the X5 image defines the same names
# itself instead.
add_library(kickos_x86_64_nokernel OBJECT "${KICKOS_X86_64_DIR}/nokernel_x86_64.cc")
kickos_apply_freestanding(kickos_x86_64_nokernel)
target_include_directories(kickos_x86_64_nokernel PRIVATE ${KICKOS_X86_64_INCLUDES})

# The switch accumulator's two symbols. Under KICKOS_BENCH switch.S brackets the swap and
# reaches the kernel for both, and the images below carry the arch archive with no kernel
# behind it.
add_library(kickos_x86_64_nobench OBJECT "${KICKOS_X86_64_DIR}/nobench_x86_64.cc")
kickos_apply_freestanding(kickos_x86_64_nobench)
target_include_directories(kickos_x86_64_nobench PRIVATE ${KICKOS_X86_64_INCLUDES})

# X2's subset: the tables, the report and the declining interrupt fallback.
if(KICKOS_KERNEL_CORES EQUAL 1)
add_library(kickos_x86_64_x2 OBJECT
  "${KICKOS_X86_64_DIR}/desc_x86_64.cc"
  "${KICKOS_X86_64_DIR}/fault_x86_64.cc"
  "${KICKOS_X86_64_DIR}/trap_x86_64.S"
  "${KICKOS_X86_64_DIR}/kickos_x86_64_isr_default.cc"
  "${KICKOS_X86_64_DIR}/landed_x2_x86_64.cc")
kickos_apply_freestanding(kickos_x86_64_x2)
target_include_directories(kickos_x86_64_x2 PRIVATE ${KICKOS_X86_64_INCLUDES})

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
    COMMAND ${KICKOS_X86_64_PROBE_LINK}
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
            ${KICKOS_X86_64_PROBE_DEPENDS}
    COMMENT "x86_64: linking the PE32+ UEFI application ${_img}"
    COMMAND_EXPAND_LISTS
    VERBATIM)
  list(APPEND KICKOS_X86_64_IMAGES "${_img}")
  set(KICKOS_X86_64_IMAGE_${_cls} "${_img}")
endforeach()

set(KICKOS_X1_IMAGE "${KICKOS_X86_64_IMAGE_none}")

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
  COMMAND ${KICKOS_X86_64_PROBE_LINK}
          -o "${KICKOS_X5_IMAGE}"
          $<TARGET_OBJECTS:kickos_x86_64_boot>
          $<TARGET_OBJECTS:kickos_x86_64_probe5>
          $<TARGET_OBJECTS:kickos_x86_64_nobench>
          -Wl,--start-group
          "$<TARGET_FILE:kickos_chip_q35>"
          "$<TARGET_FILE:kickos_arch_x86_64>"
          -Wl,--end-group
  # See the per-class link above for why the OBJECTS and not the targets.
  DEPENDS $<TARGET_OBJECTS:kickos_x86_64_boot>
          $<TARGET_OBJECTS:kickos_x86_64_probe5>
          $<TARGET_OBJECTS:kickos_x86_64_nobench>
          "$<TARGET_FILE:kickos_chip_q35>"
          "$<TARGET_FILE:kickos_arch_x86_64>"
          ${KICKOS_X86_64_PROBE_DEPENDS}
  COMMENT "x86_64: linking the PE32+ UEFI application ${KICKOS_X5_IMAGE}"
  COMMAND_EXPAND_LISTS
  VERBATIM)
list(APPEND KICKOS_X86_64_IMAGES "${KICKOS_X5_IMAGE}")

# X6 is the AP-entry witness. It uses the same UEFI handover but reserves a low
# SIPI page and releases one AP after ExitBootServices. It is built on demand.
add_library(kickos_x86_64_probe6 OBJECT
  "${KICKOS_X86_64_DIR}/landed_x6_x86_64.cc"
  "${KICKOS_X86_64_DIR}/smp_boot_x86_64.cc"
  "${KICKOS_X86_64_DIR}/smp_trampoline.S")
kickos_apply_freestanding(kickos_x86_64_probe6)
target_include_directories(kickos_x86_64_probe6 PRIVATE ${KICKOS_X86_64_INCLUDES})

set(KICKOS_X6_IMAGE "${PROJECT_BINARY_DIR}/kickos_x86_64_x6.efi")
add_custom_command(
  OUTPUT "${KICKOS_X6_IMAGE}"
  COMMAND ${KICKOS_X86_64_PROBE_LINK}
          -o "${KICKOS_X6_IMAGE}"
          $<TARGET_OBJECTS:kickos_x86_64_boot_ap>
          $<TARGET_OBJECTS:kickos_x86_64_probe6>
          $<TARGET_OBJECTS:kickos_x86_64_nobench>
          $<TARGET_OBJECTS:kickos_x86_64_nokernel>
          -Wl,--start-group
          "$<TARGET_FILE:kickos_chip_q35>"
          "$<TARGET_FILE:kickos_arch_x86_64>"
          -Wl,--end-group
  DEPENDS $<TARGET_OBJECTS:kickos_x86_64_boot_ap>
          $<TARGET_OBJECTS:kickos_x86_64_probe6>
          $<TARGET_OBJECTS:kickos_x86_64_nobench>
          $<TARGET_OBJECTS:kickos_x86_64_nokernel>
          "$<TARGET_FILE:kickos_chip_q35>"
          "$<TARGET_FILE:kickos_arch_x86_64>"
          ${KICKOS_X86_64_PROBE_DEPENDS}
  COMMENT "x86_64: linking the two-core AP-entry witness"
  COMMAND_EXPAND_LISTS
  VERBATIM)
add_custom_target(x6_image DEPENDS "${KICKOS_X6_IMAGE}")

set(KICKOS_X1_ESP "${PROJECT_BINARY_DIR}/esp.img")
add_custom_command(
  OUTPUT "${KICKOS_X1_ESP}"
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/esp-x86_64.sh"
          "${KICKOS_X1_IMAGE}" "${KICKOS_X1_ESP}"
  DEPENDS "${KICKOS_X1_IMAGE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/esp-x86_64.sh"
  COMMENT "x86_64: building the EFI system partition ${KICKOS_X1_ESP}"
  VERBATIM)

add_custom_target(kickos_x1_image ALL DEPENDS ${KICKOS_X86_64_IMAGES} "${KICKOS_X1_ESP}")

# Every witness runs on the processor model cmake/kickos.cmake names.
set(KICKOS_X86_64_QEMU_ENV "${CMAKE_COMMAND}" -E env "KICKOS_X86_64_CPU=${KICKOS_X86_64_QEMU_CPU}")

# The boot witnesses, each also a ctest case below. The targets stay: a `ninja x<n>-run`
# leaves its serial log where a developer reads it, while the ctest case takes its own
# workdir so a run under `ctest -j` does not share one.
add_custom_target(x1-run
  COMMAND ${KICKOS_X86_64_QEMU_ENV} "${CMAKE_CURRENT_SOURCE_DIR}/tools/run-qemu-x86_64.sh"
          "${KICKOS_X1_IMAGE}" "${PROJECT_BINARY_DIR}/x1run"
  DEPENDS "${KICKOS_X1_IMAGE}"
  USES_TERMINAL
  COMMENT "x86_64: booting ${KICKOS_X1_IMAGE} under qemu-system-x86_64 with UEFI firmware")

set(_x2_cmds "")
foreach(_cls IN LISTS KICKOS_X2_CLASSES)
  list(APPEND _x2_cmds COMMAND ${KICKOS_X86_64_QEMU_ENV}
       "${CMAKE_CURRENT_SOURCE_DIR}/tools/run-qemu-x86_64-x2.sh"
       "${_cls}" "${KICKOS_X86_64_IMAGE_${_cls}}" "${PROJECT_BINARY_DIR}/x2run/${_cls}")
endforeach()
add_custom_target(x2-run
  ${_x2_cmds}
  DEPENDS ${KICKOS_X86_64_IMAGES}
  USES_TERMINAL
  COMMENT "x86_64: taking the X2 descriptor and fault-report witness, one boot per class")

add_custom_target(x5-run
  COMMAND ${KICKOS_X86_64_QEMU_ENV} "${CMAKE_CURRENT_SOURCE_DIR}/tools/run-qemu-x86_64-x5.sh"
          "${KICKOS_X5_IMAGE}" "${PROJECT_BINARY_DIR}/x5run"
  DEPENDS "${KICKOS_X5_IMAGE}"
  USES_TERMINAL
  COMMENT "x86_64: taking the X5 address-space witness")

# The guard's own positive control, registered here because the guard is this board's alone.
# AR is not in the toolchain file's find_program set, CMake resolving it itself.
if(KICKOS_BUILD_TESTS)
  # The app window's link rules over every application image this tree links: no app
  # relocation into the kernel's half, one DIR64 record per absolute word in the image, a kernel
  # reference to an app symbol only where the allowlist says what the site does with it, and
  # no script symbol re-based by a dropped section (docs/design-m10-kernel-share.md 1.3).
  add_test(NAME x86_64_app_split
    COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_x86_64_app_split.py"
            --readelf "${CMAKE_READELF}" --objdump "${CMAKE_OBJDUMP}" --nm "${CMAKE_NM}"
            --allowlist "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/x86_64_apphalf_allowlist.txt"
            --tree "${PROJECT_BINARY_DIR}/user")
  kickos_host_gate(x86_64_app_split TIMEOUT 120)
  add_test(NAME x86_64_app_split_controls
    COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_x86_64_app_split.py"
            --controls)
  kickos_host_gate(x86_64_app_split_controls)

  # THE ADDRESS-SPACE WITNESS AS A CTEST CASE, beside the family's own arms the selftest image
  # runs on this board. X5 carries a frame pool of its own and an allocation-failure injector,
  # so its refusal arms reach the map path's out-of-frames unwind and the two whole-table
  # helpers it calls, which no application image drives on demand.
  add_test(NAME x86_64_x5_aspace
    COMMAND ${KICKOS_X86_64_QEMU_ENV} "${CMAKE_CURRENT_SOURCE_DIR}/tools/run-qemu-x86_64-x5.sh"
            "${KICKOS_X5_IMAGE}" "${PROJECT_BINARY_DIR}/x5run-ctest")
  # No `host` label: it boots the emulator, so it declines with every other image test.
  #
  # SKIP_RETURN_CODE, on this and every case below: the runners answer 77 for firmware or an ESP
  # tool this box does not have, the way tests/lib/gate.sh does, and WITHOUT this property ctest
  # reports that 77 as a failure naming nothing true.
  set_tests_properties(x86_64_x5_aspace PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

  # X1 AND X2, for the same reason X5 was registered: each was built by every configure and
  # run by nothing. What each reads is in no other gate on this board.
  #
  # The workdir is a LEAF and never PROJECT_BINARY_DIR: each runner builds its own esp.img
  # there, and at the top that name is the build's own EFI system partition.

  # The UEFI handover measurement. Every image prints those lines out of efi_main and no other
  # gate reads them, so the entry symbol, the cli, the arena the memory map yields and the
  # x87/MMX/SSE trap posture are witnessed here alone.
  #
  # THE ENTRY SYMBOL HAS NO STATIC WITNESS AND IS NOT MEANT TO. Dropping `-e efi_main` from the
  # link leaves a NONZERO entry (ld falls back to the start of text), so an image-header check
  # cannot see it and this boot is the only thing that does; measured, it stays green in
  # tests/integration/check_oot_export_mcu.sh and reds here. The static form would compare the
  # image's entry against efi_main's address in the link map, which puts an arch-private symbol
  # name inside a gate whose whole discipline is that nothing in it names an arch.
  add_test(NAME x86_64_x1_handover
    COMMAND ${KICKOS_X86_64_QEMU_ENV} "${CMAKE_CURRENT_SOURCE_DIR}/tools/run-qemu-x86_64.sh"
            "${KICKOS_X1_IMAGE}" "${PROJECT_BINARY_DIR}/x1run-ctest")
  set_tests_properties(x86_64_x1_handover PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)

  # The same image on qemu64, below the x86-64-v3 floor, refused before its first line.
  add_test(NAME x86_64_x1_floor
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/run-qemu-x86_64-floor.sh"
            "${KICKOS_X1_IMAGE}" "${PROJECT_BINARY_DIR}/x1floor-ctest")
  set_tests_properties(x86_64_x1_floor PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)

  # One case per fault class rather than one over the ten: the report ends the image, so each
  # class is its own boot either way, and a failure then names the class. The `none` image is
  # the negative control and refuses a report.
  foreach(_cls IN LISTS KICKOS_X2_CLASSES)
    add_test(NAME x86_64_x2_${_cls}
      COMMAND ${KICKOS_X86_64_QEMU_ENV} "${CMAKE_CURRENT_SOURCE_DIR}/tools/run-qemu-x86_64-x2.sh"
              "${_cls}" "${KICKOS_X86_64_IMAGE_${_cls}}"
              "${PROJECT_BINARY_DIR}/x2run-ctest/${_cls}")
    set_tests_properties(x86_64_x2_${_cls} PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
  endforeach()

  add_test(NAME x86_64_weak_undef_selftest
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/static/check_x86_64_weak_undef_selftest.sh"
            "${KICKOS_NO_GOT}" "${KICKOS_WEAK_UNDEF}" "${CMAKE_READELF}" "${CMAKE_C_COMPILER}"
            "${CMAKE_AR}" ${KICKOS_MCPU_FLAGS} -ffreestanding -fpie -mcmodel=small)
  set_tests_properties(x86_64_weak_undef_selftest PROPERTIES TIMEOUT 60 LABELS host)

  # The vector and x87 census over the kernel half of every image this board linked. It walks
  # PROJECT_BINARY_DIR itself, so an image is in the corpus by being built.
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

message(STATUS "KickOS: x86_64 libraries plus the X1, X2 and X5 images; `ninja x1-run`, "
               "`ninja x2-run` and `ninja x5-run` take the witnesses")
else()
  message(STATUS "KickOS: x86_64 shared-kernel application images with AP startup")
endif()

# The boot tail of an image that carries the kernel: RAM publish, the kernel-owned ctor
# window, arch_init, kmain.
add_library(kickos_x86_64_landed_kernel OBJECT
  "${KICKOS_X86_64_DIR}/landed_kernel_x86_64.cc")
kickos_apply_freestanding(kickos_x86_64_landed_kernel)
target_include_directories(kickos_x86_64_landed_kernel PRIVATE ${KICKOS_X86_64_INCLUDES}
  "${CMAKE_CURRENT_SOURCE_DIR}/kernel/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/system/include")

# The kernel posture (cmake/toolchain-x86_64-uefi.cmake), on exactly the targets pe_image.ld
# claims for the kernel's .text, read from the script so the two cannot drift, and on the
# probe images' objects, which have no app half.
file(STRINGS "${KICKOS_X86_64_PE_SCRIPT}" _kos_claims REGEX "\\(\\.text \\.text\\.\\*\\)")
set(_kos_kernel_half "")
foreach(_kos_claim IN LISTS _kos_claims)
  if(_kos_claim MATCHES "\\*lib([a-z0-9_]+)\\.a:")
    list(APPEND _kos_kernel_half "${CMAKE_MATCH_1}")
  elseif(_kos_claim MATCHES "\\*([a-z0-9_]+)\\.dir/")
    list(APPEND _kos_kernel_half "${CMAKE_MATCH_1}")
  endif()
endforeach()
if(NOT "kickos_kernel" IN_LIST _kos_kernel_half)
  message(FATAL_ERROR "KickOS x86_64: ${KICKOS_X86_64_PE_SCRIPT} claims no kickos_kernel text, "
    "so the kernel posture would reach no kernel target. The claim lines changed shape.")
endif()
set(_kos_probe_objects kickos_x86_64_boot_com1)
if(KICKOS_KERNEL_CORES EQUAL 1)
  list(APPEND _kos_probe_objects kickos_x86_64_x2 kickos_x86_64_probe5 kickos_x86_64_probe6)
  foreach(_cls IN LISTS KICKOS_X2_CLASSES)
    list(APPEND _kos_probe_objects kickos_x86_64_probe_${_cls})
  endforeach()
endif()
foreach(_kos_t IN LISTS _kos_kernel_half _kos_probe_objects)
  if(NOT TARGET ${_kos_t})
    message(FATAL_ERROR "KickOS x86_64: '${_kos_t}' takes the kernel posture and is no target.")
  endif()
  target_compile_options(${_kos_t} PRIVATE
    "$<$<COMPILE_LANGUAGE:C,CXX>:${KICKOS_X86_64_KERNEL_FLAGS}>")
endforeach()

# kickos_core links the boot and landing objects (CMakeLists.txt), so an out-of-tree app links
# them exactly as an in-tree one does: exported, they reach KickOSTargets as IMPORTED_OBJECTS.
kickos_export_targets(kickos_x86_64_boot kickos_x86_64_boot_ap kickos_x86_64_landed_kernel)

# Beside the installed toolchain file, which finds the link tool there, and the tool its scripts.
install(FILES "${KICKOS_X86_64_PE_SCRIPT}" DESTINATION "${KICKOS_CMAKE_DIR}")
install(PROGRAMS "${KICKOS_X86_64_LINK}" "${KICKOS_NO_GOT}" "${KICKOS_WEAK_UNDEF}"
                 "${KICKOS_X86_64_KREL}"
        DESTINATION "${KICKOS_CMAKE_DIR}")
