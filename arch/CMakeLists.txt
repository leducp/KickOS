# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The arm toolchain does not set KICKOS_ARCH_FAMILY; every other family's toolchain does.
if(NOT KICKOS_ARCH_FAMILY)
  set(KICKOS_ARCH_FAMILY "arm")
endif()

# --- Seam fallbacks: the lone-TU rule ----------------------------------------
# A seam whose backend is optional gets its fallback in a translation unit of its own,
# <symbol>_default.cc, defining exactly one symbol: a second symbol drags the member in beside
# a backend that defines the seam, and the two collide.
#
# How a seam resolves:
#   Within one archive, the first member in member order (the source order of add_library())
#   that defines the symbol is extracted. A later member defining it too is extracted only if
#   some other undefined symbol needs it, and the link then fails with multiple definition.
#   Across archives the group is scanned left to right on every pass: a definition in a later
#   archive is dropped when nothing else anchors its member, and is a multiple-definition
#   error when something does. The order is kickos_kernel, kickos_chip_<chip>,
#   kickos_arch_<arch> (_kickos_group in the root CMakeLists.txt).
# So a fallback never shares an archive with a backend for the same seam: an arch's
# sources.cmake drops the members its own backend defines. And an arch-neutral fallback for a
# seam the kernel declares never sits in kickos_kernel, which the group scans first.
#
# A backend's definition must live in a member the link always pulls for another reason
# (chip_<chip>.cc via -u g_isr_vector -> startup.S -> Reset_Handler; usic_uart.cc via
# arch_console_tx_backend). In a TU nothing else references, the fallback resolves the
# reference first and the board silently declines at runtime.
# tests/static/check_seam_defaults.sh enforces the one-symbol rule, the archive separation and
# the anchoring.
set(KICKOS_SEAM_DEFAULTS_COMMON
  common/arch_console_flush_sync_default.cc
  common/arch_console_reclaim_default.cc
  common/arch_console_retune_default.cc
  common/arch_console_tx_backend_default.cc
  common/arch_console_reclaim_window_default.cc
  common/arch_console_write_sync_default.cc
  common/arch_cpu_clock_set_default.cc
  common/arch_diag_led_init_default.cc
  common/arch_diag_led_set_default.cc
  common/arch_irq_line_core_default.cc
  common/arch_irq_line_kernel_owned_default.cc
  common/arch_irq_route_default.cc
  common/arch_fault_is_user_thread_default.cc
  common/arch_fault_redirect_to_exit_default.cc
  common/arch_periph_clock_hz_default.cc
  common/arch_periph_enable_default.cc
  common/arch_periph_reg_write_default.cc
  common/arch_pinmux_set_default.cc
  common/kfault_terminate_default.cc)
# arch_reboot must be absent from a production image, fallback included.
if(KICKOS_ENABLE_SELFTEST)
  list(APPEND KICKOS_SEAM_DEFAULTS_COMMON common/arch_reboot_default.cc)
endif()
if(KICKOS_FAULT_ISOLATION)
  list(REMOVE_ITEM KICKOS_SEAM_DEFAULTS_COMMON
    common/arch_fault_is_user_thread_default.cc
    common/arch_fault_redirect_to_exit_default.cc)
endif()
# Swapped, not added: both TUs define kfault_terminate.
if(KICKOS_CHIP_EXITS_ON_FAULT)
  list(REMOVE_ITEM KICKOS_SEAM_DEFAULTS_COMMON common/kfault_terminate_default.cc)
  list(APPEND KICKOS_SEAM_DEFAULTS_COMMON common/kfault_terminate_shutdown.cc)
endif()

set(KICKOS_SEAM_DEFAULTS_ARM
  arm/common/arch_bitband_present_default.cc
  arm/common/arch_idle_wait_default.cc
  arm/common/arch_mpu_min_region_default.cc
  arm/common/arch_mpu_nocache_support_default.cc
  arm/common/arch_mpu_region_encodable_default.cc
  arm/common/arch_mpu_region_pow2_default.cc
  arm/common/arch_shutdown_default.cc
  arm/common/kickos_arch_mpu_commit_default.cc)
if(KICKOS_HAVE_MPU)
  list(APPEND KICKOS_SEAM_DEFAULTS_ARM
    arm/common/arch_mpu_encode_default.cc
    arm/common/arch_arm_mpu_pmsav7.cc
    arm/common/kickos_arm_mpu_fixed_default.cc)
endif()

# --- The arch archive --------------------------------------------------------
# arch/<family>/<arch>/sources.cmake is included into this directory scope, so its paths are
# relative here and a source property it sets applies. It sets KICKOS_ARCH_SOURCES and, where
# needed, KICKOS_ARCH_PRIVATE_INCLUDE_DIRS, KICKOS_ARCH_LINK_LIBRARIES and KICKOS_ARCH_HOSTED.
set(_kos_arch_dir "${KICKOS_ARCH_FAMILY}/${KICKOS_ARCH}")
if(KICKOS_ARCH_FAMILY STREQUAL KICKOS_ARCH)
  # The sim's backend sits at arch/sim, with no family level.
  set(_kos_arch_dir "${KICKOS_ARCH}")
endif()
set(_kos_arch_optin "${CMAKE_CURRENT_SOURCE_DIR}/${_kos_arch_dir}/sources.cmake")
if(NOT EXISTS "${_kos_arch_optin}")
  message(FATAL_ERROR "KickOS: arch backend '${KICKOS_ARCH}' not available "
    "(no ${_kos_arch_optin})")
endif()
set(KICKOS_ARCH_SOURCES "")
set(KICKOS_ARCH_PRIVATE_INCLUDE_DIRS "")
set(KICKOS_ARCH_LINK_LIBRARIES "")
set(KICKOS_ARCH_HOSTED OFF)
include("${_kos_arch_optin}")
if(NOT KICKOS_ARCH_SOURCES)
  message(FATAL_ERROR
    "KickOS: ${_kos_arch_optin} sets no KICKOS_ARCH_SOURCES, so the arch archive would be "
    "empty and every seam it answers would resolve to a fallback or to nothing.")
endif()

add_library(kickos_arch_${KICKOS_ARCH} STATIC ${KICKOS_ARCH_SOURCES})
if(KICKOS_ARCH_HOSTED)
  kickos_apply_hosted(kickos_arch_${KICKOS_ARCH})
else()
  kickos_apply_freestanding(kickos_arch_${KICKOS_ARCH})
endif()
target_include_directories(kickos_arch_${KICKOS_ARCH} PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/${_kos_arch_dir}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
if(KICKOS_ARCH_PRIVATE_INCLUDE_DIRS)
  target_include_directories(kickos_arch_${KICKOS_ARCH} PRIVATE
    ${KICKOS_ARCH_PRIVATE_INCLUDE_DIRS})
endif()
if(KICKOS_ARCH_LINK_LIBRARIES)
  target_link_libraries(kickos_arch_${KICKOS_ARCH} PUBLIC ${KICKOS_ARCH_LINK_LIBRARIES})
endif()
install(TARGETS kickos_arch_${KICKOS_ARCH} EXPORT KickOSTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
set(KICKOS_ARCH_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${_kos_arch_dir}/include" PARENT_SCOPE)

# kickos_system sits above the arch layer: include its headers, never link it.
target_include_directories(kickos_arch_${KICKOS_ARCH} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../system/include")

if(KICKOS_HAVE_ASPACE)
  kickos_privatise_runtime(kickos_arch_${KICKOS_ARCH})
  # arch_context_init takes the address of kickos_user_thread_return, which is app text.
  kickos_split_image_tu(kickos_arch_${KICKOS_ARCH}
    "${KICKOS_ARCH_FAMILY}/${KICKOS_ARCH}/arch_${KICKOS_ARCH}.cc")
endif()

# __aeabi_read_tp is checked by instruction: C code never runs with SP at an exclusive stack
# top, so the tlsprobe witness passes with or without the subtract that case needs.
if(KICKOS_BUILD_TESTS AND KICKOS_TLS AND KICKOS_ARCH_FAMILY STREQUAL "arm")
  add_test(
    NAME    arm_read_tp
    COMMAND "${CMAKE_SOURCE_DIR}/tests/static/check_arm_read_tp.sh"
            "${CMAKE_OBJDUMP}"
            "$<TARGET_FILE:kickos_arch_${KICKOS_ARCH}>")
  set_tests_properties(arm_read_tp PROPERTIES TIMEOUT 60 LABELS host)
endif()

# No privileged TU may reach thread-local storage. Registered on every cross board, not only
# where KICKOS_TLS is on.
if(KICKOS_BUILD_TESTS AND NOT (KICKOS_ARCH STREQUAL "sim"))
  set(_no_tls_scan
      "$<TARGET_FILE:kickos_kernel>"
      "$<TARGET_FILE:kickos_arch_${KICKOS_ARCH}>"
      "$<TARGET_FILE:kickos_chip_${KICKOS_CHIP}>"
      "$<TARGET_FILE:kickos_lib>")
  # One member of libkickos_user.a, never the whole archive: the rest is unprivileged. A
  # thread_local in newlib_reent.cc is read on the wrong thread pointer, since the context
  # switch seats its word. x86_64 builds no newlib_reent.cc, and the gate refuses a scan that
  # names a missing member.
  if(NOT KICKOS_ARCH STREQUAL "x86_64")
    list(APPEND _no_tls_scan "$<TARGET_FILE:kickos_user>::newlib_reent.cc.obj")
  endif()
  add_test(
    NAME    no_privileged_tls
    COMMAND "${CMAKE_SOURCE_DIR}/tests/static/check_no_privileged_tls.sh"
            "${CMAKE_READELF}" "${CMAKE_NM}"
            ${_no_tls_scan})
  set_tests_properties(no_privileged_tls PROPERTIES TIMEOUT 60 LABELS host)
endif()

# No archive holding kernel text may name the app's runtime; the kernel's own copies go by the
# kickos/kruntime.h names. kickos_lib is left out: it defines the app's copies.
if(KICKOS_BUILD_TESTS AND KICKOS_HAVE_ASPACE)
  add_test(
    NAME    kernel_runtime
    COMMAND "${CMAKE_SOURCE_DIR}/tests/static/check_kernel_runtime.sh"
            "${CMAKE_NM}"
            "$<TARGET_FILE:kickos_kernel>"
            "$<TARGET_FILE:kickos_arch_${KICKOS_ARCH}>"
            "$<TARGET_FILE:kickos_chip_${KICKOS_CHIP}>")
  set_tests_properties(kernel_runtime PROPERTIES TIMEOUT 60 LABELS host)
endif()

# No GOT reference from those archives either: a weak extern in a TU left off
# kickos_split_image_tu emits one, and the link stays clean until something forces .got to be
# placed.
if(KICKOS_BUILD_TESTS AND KICKOS_HAVE_ASPACE)
  add_test(
    NAME    kernel_got
    COMMAND "${CMAKE_SOURCE_DIR}/tests/static/check_kernel_got.sh"
            "${CMAKE_READELF}"
            "$<TARGET_FILE:kickos_kernel>"
            "$<TARGET_FILE:kickos_arch_${KICKOS_ARCH}>"
            "$<TARGET_FILE:kickos_chip_${KICKOS_CHIP}>")
  set_tests_properties(kernel_got PROPERTIES TIMEOUT 60 LABELS host)
endif()

# --- Chip backend (startup + linker script + clocks + console) --------------
if(KICKOS_CHIP AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}")
  file(GLOB _chip_src CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/*.S")

  # class/ is built as its own kickos_class_<chip> below and must never land in the chip
  # archive, even if the glob turns recursive.
  list(FILTER _chip_src EXCLUDE REGEX "/class/")

  # The PMSAv8 backend (set by a v8-M chip's mpu.cmake) goes into the chip archive, never the
  # armv7m arch list: v6-M/v7-M must never link it, and from the chip archive its
  # kickos_arch_mpu_commit and arch_mpu_region_encodable win over the arch fallbacks.
  if(KICKOS_HAVE_MPU AND KICKOS_ARM_PMSAV8_SOURCE)
    list(APPEND _chip_src "${KICKOS_ARM_PMSAV8_SOURCE}")
  endif()

  # A family.cmake names TUs shared by chips of one vendor family. They are compiled into this
  # chip's archive, once per chip, against the chip's own family_map.h and regs/, so every
  # per-part value stays a compile-time constant. They must not get an archive of their own:
  # the chip linker scripts select .data/.bss by archive:member, so a new archive falls through
  # to the app catch-all, and a family may straddle two ISAs (RP2040 armv6m, RP2350 armv7m).
  set(_family_optin
      "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/family.cmake")
  if(EXISTS "${_family_optin}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_family_optin}")
    include("${_family_optin}")
    if(NOT KICKOS_CHIP_FAMILY_SOURCES OR NOT KICKOS_CHIP_FAMILY_DIR)
      message(FATAL_ERROR
        "KickOS: ${_family_optin} sets no KICKOS_CHIP_FAMILY_SOURCES/KICKOS_CHIP_FAMILY_DIR, "
        "so the family bodies would silently not be compiled and the seams they define "
        "would resolve to their arch fallbacks.")
    endif()
    foreach(_family_src IN LISTS KICKOS_CHIP_FAMILY_SOURCES)
      if(NOT EXISTS "${_family_src}")
        message(FATAL_ERROR
          "KickOS: ${_family_optin} names ${_family_src}, which does not exist.")
      endif()
    endforeach()
    list(APPEND _chip_src ${KICKOS_CHIP_FAMILY_SOURCES})
  endif()

  # boot2.S is a second-stage loader the boot ROM CRC-checks, so it is linked to a raw binary,
  # checksummed, and re-emitted as a .boot2 data blob instead of being compiled into the
  # archive.
  set(_boot2_src "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/boot2.S")
  if(EXISTS "${_boot2_src}")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(_boot2_tool "${CMAKE_SOURCE_DIR}/cmake/${KICKOS_CHIP}_checksum.py")
    set(_boot2_sym "g_${KICKOS_CHIP}_boot2")       # data blob label (checksum tool)
    set(_boot2_entry "_${KICKOS_CHIP}_boot2_entry") # code entry in boot2.S
    if(NOT EXISTS "${_boot2_tool}")
      message(FATAL_ERROR "KickOS: ${KICKOS_CHIP} ships boot2.S but no checksum "
        "tool at ${_boot2_tool}")
    endif()
    list(REMOVE_ITEM _chip_src "${_boot2_src}")
    set(_boot2_ld "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/boot2.ld")
    set(_boot2_elf "${CMAKE_CURRENT_BINARY_DIR}/${KICKOS_CHIP}_boot2.elf")
    set(_boot2_bin "${CMAKE_CURRENT_BINARY_DIR}/${KICKOS_CHIP}_boot2.bin")
    set(_boot2_gen "${CMAKE_CURRENT_BINARY_DIR}/${KICKOS_CHIP}_boot2_checksummed.S")
    # boot2.ld caps the loader at 252 bytes. -T goes through the driver: the newlib toolchain
    # injects no default script, so boot2.ld applies alone.
    add_custom_command(
      OUTPUT "${_boot2_bin}"
      COMMAND ${CMAKE_C_COMPILER} ${KICKOS_MCPU_FLAGS} -mthumb
              -nostdlib -nostartfiles -Wl,--entry=${_boot2_entry}
              -Wl,--orphan-handling=error
              -T "${_boot2_ld}" "${_boot2_src}" -o "${_boot2_elf}"
      COMMAND ${CMAKE_OBJCOPY} -O binary "${_boot2_elf}" "${_boot2_bin}"
      DEPENDS "${_boot2_src}" "${_boot2_ld}"
      COMMENT "boot2: assembling ${KICKOS_CHIP} second stage -> raw binary"
      VERBATIM)
    add_custom_command(
      OUTPUT "${_boot2_gen}"
      COMMAND ${Python3_EXECUTABLE} "${_boot2_tool}" "${_boot2_bin}" "${_boot2_gen}"
      DEPENDS "${_boot2_bin}" "${_boot2_tool}"
      COMMENT "boot2: CRC-checksumming ${KICKOS_CHIP} second stage"
      VERBATIM)
    list(APPEND _chip_src "${_boot2_gen}")
    # Nothing references the .boot2 blob, so the app link must force it in from the chip
    # archive: KEEP only protects an object already included.
    set(KICKOS_BOOT2_FORCE_SYMBOL "${_boot2_sym}" PARENT_SCOPE)
  endif()

  # node_vectors.S is referenced by nothing until the launch writes VTOR, so the app link must
  # force its object in from the chip archive: KEEP only protects an object already included.
  # The table is compiled out below two cores, where forcing it leaves an unresolvable symbol.
  if(_chip_src MATCHES "/node_vectors\\.S" AND KICKOS_NUM_CORES GREATER 1)
    set(KICKOS_NODE_VECTOR_FORCE_SYMBOL "g_node_isr_vector" PARENT_SCOPE)
  endif()

  add_library(kickos_chip_${KICKOS_CHIP} STATIC ${_chip_src})
  kickos_apply_freestanding(kickos_chip_${KICKOS_CHIP})
  if(KICKOS_HAVE_ASPACE)
    kickos_privatise_runtime(kickos_chip_${KICKOS_CHIP})
    # Reset_Handler references app-half symbols (__kickos_appbss_start/_end, __eh_frame_start)
    # and a weak __register_frame. The path is absolute to match the glob.
    kickos_split_image_tu(kickos_chip_${KICKOS_CHIP}
      "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/chip_${KICKOS_CHIP}.cc")
  endif()
  # The M7 D-cache is safe only while the M7 is single-core with no DMA: an M4 or DMA driver
  # must add clean/invalidate maintenance around its buffers.
  # KICKOS_USB_CONSOLE must be set by the top-level CMakeLists before add_subdirectory(arch).
  set(_imxrt_dcache_default ON)
  if(KICKOS_CHIP STREQUAL "imxrt1062" AND KICKOS_USB_CONSOLE)
    set(_imxrt_dcache_default OFF)
  endif()
  option(KICKOS_IMXRT_DCACHE "imxrt1062: enable the Cortex-M7 L1 D-cache"
         ${_imxrt_dcache_default})
  # option() caches, so an existing build directory switched onto a USB service list keeps a
  # stale ON. With the cache on, coherency rests on KOS_MEM_NOCACHE reaching both of the
  # block's grants.
  if(KICKOS_CHIP STREQUAL "imxrt1062" AND KICKOS_USB_CONSOLE AND KICKOS_IMXRT_DCACHE)
    message(WARNING
      "KickOS: KICKOS_IMXRT_DCACHE=ON with a USB console on imxrt1062 is UNWITNESSED ON "
      "SILICON. The device controller reads its descriptor lists out of OCRAM2 as a bus "
      "master and this tree has no cache maintenance at all; coherency rests ENTIRELY on "
      "the S7 non-cacheable attribute reaching every mapping of the shared block, the "
      "driver's and the app's. Nothing here has enumerated on a host with the cache on. "
      "Pass -DKICKOS_IMXRT_DCACHE=OFF for the "
      "witnessed posture; that is what a fresh build directory selects by default.")
  endif()
  if(KICKOS_CHIP STREQUAL "imxrt1062" AND NOT KICKOS_IMXRT_DCACHE)
    message(WARNING
      "KickOS: the Cortex-M7 L1 D-cache is OFF on this image, which costs about 6x on IPC "
      "(measured: 61124 ns per call/reply round-trip with it on, 363005 ns with it off). "
      "Code executes XIP from FlexSPI, so literal pools and .rodata are data reads that "
      "reach external QSPI flash with the cache off. A USB console selects this posture by "
      "default because keeping the cache rests on the S7 non-cacheable attribute "
      "reaching every mapping of the shared block, which no board on this bench can "
      "witness. Pass "
      "-DKICKOS_IMXRT_DCACHE=ON to take that path.")
  endif()
  if(KICKOS_CHIP STREQUAL "imxrt1062" AND KICKOS_IMXRT_DCACHE)
    target_compile_definitions(kickos_chip_${KICKOS_CHIP} PRIVATE KICKOS_IMXRT_DCACHE=1)
  endif()
  # A v7-M chip that overrides arch_trace_now (mps2's semihosting clock) has it on the
  # PendSV-tail emit path, so its TU must be FP-register-free too.
  if(KICKOS_TELEMETRY_ON AND KICKOS_CHIP STREQUAL "mps2")
    set_source_files_properties(
      "${CMAKE_CURRENT_SOURCE_DIR}/arm/chip/${KICKOS_CHIP}/chip_${KICKOS_CHIP}.cc"
      PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
  endif()
  target_include_directories(kickos_chip_${KICKOS_CHIP} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/${KICKOS_ARCH}/include"
    # A chip has no regs.h of its own: #include "regs.h" resolves to <family>/common/regs.h.
    "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/common"
    "${CMAKE_CURRENT_SOURCE_DIR}/common"
    "${CMAKE_CURRENT_SOURCE_DIR}/../kernel/include"
    # kickos/sys/abi.h, for the KOS_PSTATE_* selector arch_cpu_clock_set takes. It pulls
    # errno.h from kickos_system.
    "${CMAKE_CURRENT_SOURCE_DIR}/../user/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../system/include"
    # rtt.h, for an arch_shutdown that dumps the telemetry ring (mps2).
    "${CMAKE_CURRENT_SOURCE_DIR}/../lib/include")
  # Appended after the list above: a chip directory ahead of <family>/common would shadow
  # regs.h for every source in this archive. Only the configured chip's directory is on this
  # path, so a family TU's #include "regs/<periph>.h" resolves to that chip's copy.
  if(KICKOS_CHIP_FAMILY_DIR)
    target_include_directories(kickos_chip_${KICKOS_CHIP} PRIVATE
      "${KICKOS_CHIP_FAMILY_DIR}"
      "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}")
  endif()
  install(TARGETS kickos_chip_${KICKOS_CHIP} EXPORT KickOSTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")

  # --- Class-driver leaf -------------------------------------------------------
  # <family>/chip/<chip>/class/ holds register logic shared by the kernel and an unprivileged
  # driver: a freestanding, stateless leaf (POD and free functions taking an explicit base, no
  # ctor/dtor, no mutable static). It is one read-only copy reached from two trust domains, so
  # it carries no writable state, and it builds on a bare include path: repo include/ and this
  # chip's register directory, never kernel/include.
  set(_class_dir
    "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/class")
  if(EXISTS "${_class_dir}")
    file(GLOB _class_src CONFIGURE_DEPENDS "${_class_dir}/*.cc")
    add_library(kickos_class_${KICKOS_CHIP} STATIC ${_class_src})
    kickos_apply_freestanding(kickos_class_${KICKOS_CHIP})
    target_include_directories(kickos_class_${KICKOS_CHIP}
      PUBLIC
        "$<BUILD_INTERFACE:${_class_dir}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
      PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}")
    install(TARGETS kickos_class_${KICKOS_CHIP} EXPORT KickOSTargets
            ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  endif()

  # boards/<board>/<chip>.ld overrides the chip's script. It keeps the <chip>.ld name so the
  # INSTALL_INTERFACE -T path stays board-independent.
  set(_ld "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/${KICKOS_CHIP}.ld")
  if(EXISTS "${CMAKE_SOURCE_DIR}/boards/${KICKOS_BOARD}/${KICKOS_CHIP}.ld")
    set(_ld "${CMAKE_SOURCE_DIR}/boards/${KICKOS_BOARD}/${KICKOS_CHIP}.ld")
  endif()
  # A chip may ship no script: x86_64 is a PE32+ UEFI image linked by its own `ld -m i386pep`
  # command, not through the driver the -T below reaches.
  if(EXISTS "${_ld}")
    file(READ "${_ld}" _ld_sections_txt)
    set(_ld_required KICKOS_CODE_DEBRIS_SECTIONS KICKOS_NONALLOC_SECTIONS
                     KICKOS_STATIC_RELOC_ASSERT KICKOS_TLS_TEMPLATE KICKOS_TLS_FIT_ASSERT)
    # rxv3 alone has no TLS sections for the fit assert to read: its thread_local is an emutls
    # control block, and only its own script can name the span they are gathered into.
    if(KICKOS_ARCH STREQUAL "rxv3")
      list(APPEND _ld_required KICKOS_TLS_EMUTLS_ASSERT)
    endif()
    foreach(_m IN LISTS _ld_required)
      if(NOT "${_ld_sections_txt}" MATCHES "${_m}")
        message(FATAL_ERROR
          "KickOS: ${_ld} does not invoke ${_m}. Every image links with "
          "--orphan-handling=error, so an input section no rule names is a link failure. "
          "Include <sections.ld.h> and invoke KICKOS_CODE_DEBRIS_SECTIONS inside the output "
          "section holding .text, KICKOS_NONALLOC_SECTIONS as the last statement in SECTIONS, "
          "and KICKOS_STATIC_RELOC_ASSERT() beside the other asserts.")
      endif()
    endforeach()

    # The generated script keeps the <chip>.ld name for the INSTALL_INTERFACE -T path.
    set(_ld_gen "${CMAKE_CURRENT_BINARY_DIR}/${KICKOS_CHIP}.ld")
    set(_ld_defs -DKICKOS_HAVE_MPU=${KICKOS_HAVE_MPU})
    # A chip whose boot handshake needs storage outside .bss carves it per hart, so its script
    # must know the count; at one core the carve is absent rather than empty.
    list(APPEND _ld_defs -DKICKOS_NUM_CORES=${KICKOS_NUM_CORES})
    # Both nodes' scripts derive this image's base and the shared region from one geometry.
    list(APPEND _ld_defs -DKICKOS_AMP_OWN_IMAGE=${KICKOS_AMP_OWN_IMAGE})
    if(KICKOS_AMP_OWN_IMAGE)
      list(APPEND _ld_defs
        -DKICKOS_AMP_PARTITION_BASE=${KICKOS_AMP_PARTITION_BASE}
        -DKICKOS_AMP_NODE_SHARE=${KICKOS_AMP_NODE_SHARE}
        -DKICKOS_AMP_SHARED_SIZE=${KICKOS_AMP_SHARED_SIZE}
        -DKICKOS_AMP_NODES=${KICKOS_AMP_NODES}
        -DKICKOS_AMP_NODE_ID=${KICKOS_AMP_NODE_ID})
      # The second span, for a part whose text does not live where its data does. Zero on
      # every part that partitions one aperture, and its script never reads them.
      list(APPEND _ld_defs
        -DKICKOS_AMP_TEXT_BASE=${KICKOS_AMP_TEXT_BASE}
        -DKICKOS_AMP_TEXT_SHARE=${KICKOS_AMP_TEXT_SHARE})
    endif()
    # KICKOS_TLS_FIT_ASSERT reads the knob, the stride and the arch's ABI bias, scraped from
    # the context.h the compiler reads so the two cannot disagree.
    list(APPEND _ld_defs -DKICKOS_TLS=$<BOOL:${KICKOS_TLS}>)
    if(KICKOS_TLS)
      set(_tls_tcb_hdr "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/${KICKOS_ARCH}/include/kickos/arch/context.h")
      if(NOT EXISTS "${_tls_tcb_hdr}")
        message(FATAL_ERROR "KickOS: no ${_tls_tcb_hdr} to read KICKOS_ARCH_TLS_TCB from")
      endif()
      file(STRINGS "${_tls_tcb_hdr}" _tls_tcb_line REGEX "^#define[ \t]+KICKOS_ARCH_TLS_TCB[ \t]+[0-9]+")
      if(_tls_tcb_line STREQUAL "")
        message(FATAL_ERROR
          "KickOS: ${_tls_tcb_hdr} defines no KICKOS_ARCH_TLS_TCB, so the link cannot check "
          "that a thread_local template fits one stride.")
      endif()
      string(REGEX REPLACE "^#define[ \t]+KICKOS_ARCH_TLS_TCB[ \t]+([0-9]+).*" "\\1" _tls_tcb "${_tls_tcb_line}")
      # The rounding tls_block_size() applies, scraped from the header that owns it so the link
      # and the runtime cannot disagree.
      set(_sa_hdr "${CMAKE_SOURCE_DIR}/kernel/include/kickos/config/system.h")
      file(STRINGS "${_sa_hdr}" _sa_line REGEX "^#define[ \t]+KICKOS_STACK_ALIGN[ \t]+[0-9]+")
      if(_sa_line STREQUAL "")
        message(FATAL_ERROR
          "KickOS: ${_sa_hdr} defines no KICKOS_STACK_ALIGN, so the link cannot round a "
          "thread_local template the way tls_block_size does.")
      endif()
      string(REGEX REPLACE "^#define[ \t]+KICKOS_STACK_ALIGN[ \t]+([0-9]+).*" "\\1" _stack_align "${_sa_line}")
      list(APPEND _ld_defs -DKICKOS_ARCH_TLS_TCB=${_tls_tcb} -DKICKOS_TLS_STRIDE=${_tls_stride}
                           -DKICKOS_STACK_ALIGN=${_stack_align})
      # KICKOS_TLS_CARVE_ASSERT's stacks and floors, and what the arch's
      # <kickos/arch/idle_floor.h> reads.
      list(APPEND _ld_defs -DKICKOS_TLS_FROM_SP=${KICKOS_TLS_FROM_SP}
                           -DKICKOS_REENT_IN_TCB=${KICKOS_REENT_IN_TCB}
                           -DKICKOS_IDLE_STACK_SIZE=${KICKOS_IDLE_STACK_SIZE}
                           -DKICKOS_USER_STACK_SIZE=${KICKOS_USER_STACK_SIZE}
                           -DKICKOS_ROOT_STACK_SIZE=${KICKOS_ROOT_STACK_SIZE}
                           -DKICKOS_MIN_STACK_SIZE=${KICKOS_MIN_STACK_SIZE}
                           -DKICKOS_KERNEL_CORES=${KICKOS_KERNEL_CORES})
    endif()
    # Unset keeps the chip script's freestanding default; a full-C++ build needs a larger
    # window for the heap and libstdc++/unwind state.
    if(DEFINED KICKOS_APPDATA_SIZE)
      list(APPEND _ld_defs -DKICKOS_APPDATA_SIZE=${KICKOS_APPDATA_SIZE})
    endif()
    # Room an enforcing script keeps for kernel .data/.bss below the pow2-aligned app window.
    # A kernel .bss past the chip default raises it here, or a whole window of arena is lost to
    # the alignment.
    if(DEFINED KICKOS_KERNEL_DATA_RESERVE)
      list(APPEND _ld_defs -DKICKOS_KERNEL_DATA_RESERVE=${KICKOS_KERNEL_DATA_RESERVE})
    endif()
    # The .userheap carved before the thread arena. Inert on an MPU chip, whose heap is the
    # .appdata window pad.
    if(DEFINED KICKOS_USER_HEAP_SIZE)
      list(APPEND _ld_defs -DKICKOS_USER_HEAP_SIZE=${KICKOS_USER_HEAP_SIZE})
    endif()
    # Opt-in link-time heap floor: the .ld asserts at least this many bytes are available.
    # Undefined by default, so a too-small heap is a runtime bump failure, not a link error.
    if(DEFINED KICKOS_HEAP_MIN)
      list(APPEND _ld_defs -DKICKOS_HEAP_MIN=${KICKOS_HEAP_MIN})
    endif()
    # KICKOS_BOOT_ARENA_ASSERT: the arena must hold kmain's idle and root stacks including the
    # alignment padding the bump allocator pays. A .ld without the assert fails the configure.
    include("${CMAKE_SOURCE_DIR}/cmake/boot_arena.cmake")
    kickos_boot_arena_defs("${CMAKE_CURRENT_SOURCE_DIR}"
                           "kickos_arch_${KICKOS_ARCH}" "kickos_chip_${KICKOS_CHIP}"
                           "${_ld}" _boot_defs
                           _mpu_min_region_cfg _mpu_region_pow2_cfg)
    list(APPEND _ld_defs ${_boot_defs})
    # Unset on the sim, which has no KICKOS_CHIP and never enters this block.
    set_property(GLOBAL PROPERTY KICKOS_MPU_MIN_REGION_CFG "${_mpu_min_region_cfg}")
    set_property(GLOBAL PROPERTY KICKOS_MPU_REGION_POW2_CFG "${_mpu_region_pow2_cfg}")
    # -MD/DEPFILE and not a longer DEPENDS list: a chip script may include headers of its own
    # beside the two common ones, and cpp is what knows which.
    add_custom_command(
      OUTPUT "${_ld_gen}"
      COMMAND ${CMAKE_C_COMPILER} -E -P -x c ${_ld_defs}
              "-I${CMAKE_CURRENT_SOURCE_DIR}/common"
              # An arch may state a geometry both its own code and a chip's script read.
              "-I${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/${KICKOS_ARCH}/include"
              -MD -MF "${_ld_gen}.d" -MT "${_ld_gen}"
              "${_ld}" -o "${_ld_gen}"
      DEPENDS "${_ld}" "${CMAKE_CURRENT_SOURCE_DIR}/common/boot_arena.ld.h"
              "${CMAKE_CURRENT_SOURCE_DIR}/common/sections.ld.h"
      DEPFILE "${_ld_gen}.d"
      COMMENT "linker: preprocess ${KICKOS_CHIP}.ld (KICKOS_HAVE_MPU=${KICKOS_HAVE_MPU})"
      VERBATIM)
    add_custom_target(kickos_ldscript_${KICKOS_CHIP} ALL DEPENDS "${_ld_gen}")
    # The `kickos` INTERFACE's INSTALL_INTERFACE -T points at this copy.
    install(FILES "${_ld_gen}" DESTINATION "${CMAKE_INSTALL_LIBDIR}/kickos")
    set(KICKOS_LINKER_SCRIPT "${_ld_gen}" PARENT_SCOPE)
  endif()
endif()
