# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# ISA family routes the chip glob + per-arch include path. The arm toolchain does not set it;
# every other family's toolchain does.
if(NOT KICKOS_ARCH_FAMILY)
  set(KICKOS_ARCH_FAMILY "arm")
endif()

# --- Seam fallbacks: the lone-TU rule ----------------------------------------
# A seam whose backend is optional gets its fallback body in a translation unit of its OWN,
# named <symbol>_default.cc, defining EXACTLY ONE symbol: a second symbol in a fallback TU
# drags the member in beside a backend that defines the seam and collides with it.
# tests/static/check_seam_defaults.sh asserts the one-symbol rule and the per-board resolution.
#
# The load-bearing invariant is on the backend side: a backend's definition must live in an
# archive member that is ALWAYS ANCHORED, one the link pulls for some other reason
# (chip_<chip>.cc, force-loaded via -u g_isr_vector -> startup.S -> Reset_Handler;
# usic_uart.cc, pulled for arch_console_tx_backend). A backend whose definition sits in a
# dedicated TU nothing else references is not protected by scan order: the fallback resolves
# the reference first and the board SILENTLY DECLINES at runtime. Leg 2 of the gate enforces
# the anchoring rule.
#
# Arch-neutral fallbacks for seams the KERNEL declares must NOT sit in kickos_kernel: the
# group scans the kernel archive before the chip archive, so a fallback there is extracted in
# the same pass that makes the symbol undefined and then collides with the chip's definition.
# Each list below drops the members the arch backend itself defines: two definitions of one
# symbol in ONE archive resolve by member order.
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
  common/arch_fault_is_user_thread_default.cc
  common/arch_fault_redirect_to_exit_default.cc
  common/arch_periph_clock_hz_default.cc
  common/arch_periph_enable_default.cc
  common/arch_periph_reg_write_default.cc
  common/arch_pinmux_set_default.cc
  common/kfault_terminate_default.cc)
# arch_reboot must be absent from a production image, so its fallback follows the same gate
# as its callers and its chip backends.
if(KICKOS_ENABLE_SELFTEST)
  list(APPEND KICKOS_SEAM_DEFAULTS_COMMON common/arch_reboot_default.cc)
endif()
if(KICKOS_FAULT_ISOLATION)
  list(REMOVE_ITEM KICKOS_SEAM_DEFAULTS_COMMON
    common/arch_fault_is_user_thread_default.cc
    common/arch_fault_redirect_to_exit_default.cc)
endif()
# A chip whose arch_shutdown reaches a debug or emulator channel (terminate.cmake) takes the
# exiting fault dead-end. A SWAP and not an addition: two definitions of one symbol in ONE
# archive resolve by member order, so the LED fallback leaves the list with it.
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
# The fixed-region hook and the region encoder exist only where the region type and their
# callers do.
if(KICKOS_HAVE_MPU)
  list(APPEND KICKOS_SEAM_DEFAULTS_ARM
    arm/common/arch_mpu_encode_default.cc
    arm/common/kickos_arm_mpu_fixed_default.cc)
endif()

if(KICKOS_ARCH STREQUAL "sim")
  # sim.cc is the backend for arch_console_write_sync, arch_console_tx_backend,
  # arch_console_reclaim_window, kfault_terminate and arch_periph_reg_write, so those fallbacks
  # are dropped: a fallback beside a backend in ONE archive resolves by member order.
  # SystemCoreClock has no chip to define it here.
  set(_sim_defaults ${KICKOS_SEAM_DEFAULTS_COMMON})
  list(REMOVE_ITEM _sim_defaults
    common/arch_console_tx_backend_default.cc
    common/arch_console_reclaim_window_default.cc
    common/arch_console_write_sync_default.cc
    common/arch_periph_reg_write_default.cc
    common/kfault_terminate_default.cc)
  add_library(kickos_arch_sim STATIC sim/sim.cc sim/start.cc
    sim/system_core_clock_default.cc
    ${_sim_defaults})
  kickos_apply_hosted(kickos_arch_sim)
  target_include_directories(kickos_arch_sim PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sim/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  # The arch-neutral cpp constants (fatal_status.ld.h).
  target_include_directories(kickos_arch_sim PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/common")
  # Telemetry: the sim flushes the RTT ch1 ring to a file at shutdown, so it reads lib's rtt.h.
  target_include_directories(kickos_arch_sim PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../lib/include")
  if(KICKOS_MULTI_INSTANCE)
    # PUBLIC, not PRIVATE: -pthread has to reach the final link of every app, not just
    # this archive's compiles.
    find_package(Threads REQUIRED)
    target_link_libraries(kickos_arch_sim PUBLIC Threads::Threads)
  endif()
  install(TARGETS kickos_arch_sim EXPORT KickOSTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  set(KICKOS_ARCH_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sim/include" PARENT_SCOPE)
elseif(KICKOS_ARCH STREQUAL "armv7m")
  add_library(kickos_arch_armv7m STATIC
    arm/armv7m/arch_armv7m.cc
    arm/armv7m/cache.cc
    arm/common/arch_arm_common.cc
    common/arch_ram_common.cc
    common/startup_ranges.cc
    ${KICKOS_SEAM_DEFAULTS_COMMON}
    ${KICKOS_SEAM_DEFAULTS_ARM}
    arm/armv7m/arch_fault_report_extra_default.cc
    arm/armv7m/arch_trace_now_default.cc
    arm/read_tp.S
    arm/armv7m/switch.S)
  kickos_apply_freestanding(kickos_arch_armv7m)
  # The DWT arch_trace_now fallback runs on the PendSV-tail emit path, so its TU (and
  # arch_armv7m.cc, which carries the rest of that path) must be FP-register-free.
  if(KICKOS_TELEMETRY_ON)
    set_source_files_properties(arm/armv7m/arch_armv7m.cc
                                arm/armv7m/arch_trace_now_default.cc
      PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
  endif()
  target_include_directories(kickos_arch_armv7m PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/arm/armv7m/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  install(TARGETS kickos_arch_armv7m EXPORT KickOSTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  set(KICKOS_ARCH_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/arm/armv7m/include" PARENT_SCOPE)
elseif(KICKOS_ARCH STREQUAL "armv6m")
  # Cortex-M0/M0+ (RP2040). No arch_trace_now fallback: v6-M has no DWT, so the trace clock
  # is a chip contract. Fault isolation has a real v6-M backend (it reads CONTROL.nPRIV),
  # linked in place of the declining fallbacks removed from KICKOS_SEAM_DEFAULTS_COMMON under
  # the same gate.
  set(KICKOS_ARMV6M_FAULT "")
  if(KICKOS_FAULT_ISOLATION)
    set(KICKOS_ARMV6M_FAULT arm/armv6m/arch_armv6m_fault.cc)
  endif()
  add_library(kickos_arch_armv6m STATIC
    arm/armv6m/arch_armv6m.cc
    ${KICKOS_ARMV6M_FAULT}
    arm/common/arch_arm_common.cc
    common/arch_ram_common.cc
    common/startup_ranges.cc
    ${KICKOS_SEAM_DEFAULTS_COMMON}
    ${KICKOS_SEAM_DEFAULTS_ARM}
    arm/read_tp.S
    arm/armv6m/switch.S)
  kickos_apply_freestanding(kickos_arch_armv6m)
  target_include_directories(kickos_arch_armv6m PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/arm/armv6m/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  install(TARGETS kickos_arch_armv6m EXPORT KickOSTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  set(KICKOS_ARCH_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/arm/armv6m/include" PARENT_SCOPE)
elseif(KICKOS_ARCH STREQUAL "rxv3")
  # Renesas RX72M (RXv3).
  add_library(kickos_arch_rxv3 STATIC
    rx/rxv3/arch_rxv3.cc
    rx/rxv3/emutls.cc
    common/arch_ram_common.cc
    common/startup_ranges.cc
    ${KICKOS_SEAM_DEFAULTS_COMMON}
    rx/rxv3/kickos_rx_dev_dispatch_default.cc
    rx/rxv3/kickos_rx_group_arm_default.cc
    rx/rxv3/switch.S)
  kickos_apply_freestanding(kickos_arch_rxv3)
  target_include_directories(kickos_arch_rxv3 PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/rx/rxv3/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  target_include_directories(kickos_arch_rxv3 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../lib/include") # console_tx.h (buffered console)
  install(TARGETS kickos_arch_rxv3 EXPORT KickOSTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  set(KICKOS_ARCH_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/rx/rxv3/include" PARENT_SCOPE)
elseif(KICKOS_ARCH STREQUAL "lx6")
  # ESP32 Xtensa LX6. arch_xtensa.cc is the backend for arch_irq_line_core, so that fallback
  # leaves the list: a fallback beside a backend in ONE archive resolves by member order.
  set(_lx6_defaults ${KICKOS_SEAM_DEFAULTS_COMMON})
  list(REMOVE_ITEM _lx6_defaults common/arch_irq_line_core_default.cc)
  add_library(kickos_arch_lx6 STATIC
    xtensa/lx6/arch_xtensa.cc
    xtensa/lx6/klock_lx6.cc
    common/arch_ram_common.cc
    common/startup_ranges.cc
    ${_lx6_defaults}
    xtensa/lx6/arch_clock_now_default.cc
    xtensa/lx6/arch_trace_now_default.cc
    xtensa/lx6/switch.S)
  kickos_apply_freestanding(kickos_arch_lx6)
  target_include_directories(kickos_arch_lx6 PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/xtensa/lx6/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  install(TARGETS kickos_arch_lx6 EXPORT KickOSTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  set(KICKOS_ARCH_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/xtensa/lx6/include" PARENT_SCOPE)
elseif(KICKOS_ARCH STREQUAL "armv8a")
  # ARMv8-A in AArch64 state (QEMU virt, Cortex-A53).
  # One interrupt-controller backend per image, on the version the board states.
  if(KICKOS_ARM64_GIC_VERSION EQUAL 2)
    set(_kos_gic_src arm64/common/arch_arm64_gicv2.cc)
  elseif(KICKOS_ARM64_GIC_VERSION EQUAL 3)
    set(_kos_gic_src arm64/common/arch_arm64_gicv3.cc)
  else()
    message(FATAL_ERROR
      "KICKOS_ARM64_GIC_VERSION is '${KICKOS_ARM64_GIC_VERSION}'; this arch implements "
      "2 and 3. The board states it through the Kconfig choice ARM64_GIC_V2 / ARM64_GIC_V3.")
  endif()
  add_library(kickos_arch_armv8a STATIC
    arm64/armv8a/arch_armv8a.cc
    arm64/armv8a/aspace_armv8a.cc
    arm64/armv8a/cache_armv8a.cc
    arm64/armv8a/vectors.S
    arm64/armv8a/switch.S
    arm64/armv8a/secondary.S
    # The cross-core kernel lock and the doorbell, which are one mechanism: the acquire loop
    # services a pending doorbell. Empty at one core.
    arm64/armv8a/klock_armv8a.cc
    # Interrupt controller: the bases and the routing are the part's, and a chip states them
    # by defining that backend's map (arm64/common/gicv2.h, gicv3.h).
    ${_kos_gic_src}
    common/arch_ram_common.cc
    common/startup_ranges.cc
    ${KICKOS_SEAM_DEFAULTS_COMMON})
  kickos_apply_freestanding(kickos_arch_armv8a)
  target_include_directories(kickos_arch_armv8a PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/arm64/armv8a/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  install(TARGETS kickos_arch_armv8a EXPORT KickOSTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  set(KICKOS_ARCH_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/arm64/armv8a/include" PARENT_SCOPE)
elseif(KICKOS_ARCH STREQUAL "rv32imac")
  # RISC-V RV32IMAC (ESP32-C6 + QEMU virt).
  add_library(kickos_arch_rv32imac STATIC
    riscv/rv32imac/arch_rv32imac.cc
    common/arch_ram_common.cc
    common/startup_ranges.cc
    ${KICKOS_SEAM_DEFAULTS_COMMON}
    riscv/rv32imac/arch_rv_ext_eoi_default.cc
    riscv/rv32imac/arch_rv_has_mcounteren_default.cc
    riscv/rv32imac/arch_rv_hw_mask_default.cc
    riscv/rv32imac/arch_rv_hw_unmask_default.cc
    riscv/rv32imac/arch_rv_inject_deliver_default.cc
    riscv/rv32imac/kickos_rv_ext_dispatch_dev_default.cc
    riscv/rv32imac/switch.S)
  kickos_apply_freestanding(kickos_arch_rv32imac)
  target_include_directories(kickos_arch_rv32imac PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/riscv/rv32imac/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  install(TARGETS kickos_arch_rv32imac EXPORT KickOSTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  set(KICKOS_ARCH_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/riscv/rv32imac/include" PARENT_SCOPE)
elseif(KICKOS_ARCH STREQUAL "rv64imac")
  # RISC-V RV64IMAC (QEMU virt). libgcc_rv64imac.cc holds the two compiler-runtime helpers
  # kernel text calls, which the split image cannot take from the multilib in the app's half.
  add_library(kickos_arch_rv64imac STATIC
    riscv/rv64imac/arch_rv64imac.cc
    riscv/rv64imac/aspace_rv64imac.cc
    riscv/rv64imac/klock_rv64imac.cc
    riscv/rv64imac/libgcc_rv64imac.cc
    riscv/rv64imac/switch.S
    riscv/rv64imac/trap.S
    common/arch_ram_common.cc
    common/startup_ranges.cc
    ${KICKOS_SEAM_DEFAULTS_COMMON})
  kickos_apply_freestanding(kickos_arch_rv64imac)
  target_include_directories(kickos_arch_rv64imac PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/riscv/rv64imac/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  install(TARGETS kickos_arch_rv64imac EXPORT KickOSTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  set(KICKOS_ARCH_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/riscv/rv64imac/include" PARENT_SCOPE)
elseif(KICKOS_ARCH STREQUAL "x86_64")
  # x86_64 in long mode, entered by UEFI with paging already enabled.
  #
  # NO common/arch_ram_common.cc: that file reads the chip linker script's __kickos_ram_start
  # pair, and the image firmware loads here is a PE32+ application with no KickOS script at
  # all. The RAM bounds are the FIRMWARE's answer, taken from the UEFI memory map, so the
  # chip keeps its own copies.
  #
  # NO common/startup_ranges.cc either: the UEFI loader places .data with its initialised
  # content and zero-fills the tail of a section whose virtual size exceeds its raw size, so
  # there is no copy table and no zero table for a Reset_Handler to walk.
  add_library(kickos_arch_x86_64 STATIC
    x86/x86_64/arch_x86_64.cc
    x86/x86_64/apic_x86_64.cc
    x86/x86_64/aspace_x86_64.cc
    x86/x86_64/desc_x86_64.cc
    x86/x86_64/ring3_x86_64.cc
    x86/x86_64/fault_x86_64.cc
    x86/x86_64/trap_x86_64.S
    x86/x86_64/switch.S
    ${KICKOS_SEAM_DEFAULTS_COMMON})
  kickos_apply_freestanding(kickos_arch_x86_64)
  target_include_directories(kickos_arch_x86_64 PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/x86/x86_64/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  # The fault report prints through the chip's COM1 primitives rather than through kprintf:
  # the same reporter serves the kernel-free boot images, which link no kernel to print with.
  target_include_directories(kickos_arch_x86_64 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/x86/chip/${KICKOS_CHIP}/include")
  install(TARGETS kickos_arch_x86_64 EXPORT KickOSTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  set(KICKOS_ARCH_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/x86/x86_64/include" PARENT_SCOPE)
else()
  message(FATAL_ERROR "KickOS: arch backend '${KICKOS_ARCH}' not available")
endif()

# kickos_system sits above the arch layer, so its headers (the KOS_E* taxonomy the declining
# fallbacks name, kickos/sys/atomic.h for the backends' cross-thread fields) are included here
# and never linked.
target_include_directories(kickos_arch_${KICKOS_ARCH} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../system/include")

# Arch text runs in the kernel's half, so its compiler-emitted runtime references are
# privatised with the kernel's (cmake/kickos.cmake).
if(KICKOS_HAVE_ASPACE)
  kickos_privatise_runtime(kickos_arch_${KICKOS_ARCH})
  # arch_context_init builds a thread's first frame with kickos_user_thread_return as its
  # return address, which is app text. Inert on an arch with no split-image code model
  # (cmake/kickos.cmake).
  kickos_split_image_tu(kickos_arch_${KICKOS_ARCH}
    "${KICKOS_ARCH_FAMILY}/${KICKOS_ARCH}/arch_${KICKOS_ARCH}.cc")
endif()

# Structural gate on __aeabi_read_tp: C code never runs with SP at an exclusive stack top, so
# the tlsprobe witness passes with and without the subtract that makes that case correct. The
# instructions are what gets checked.
if(KICKOS_BUILD_TESTS AND KICKOS_TLS AND KICKOS_ARCH_FAMILY STREQUAL "arm")
  add_test(
    NAME    arm_read_tp
    COMMAND "${CMAKE_SOURCE_DIR}/tests/static/check_arm_read_tp.sh"
            "${CMAKE_OBJDUMP}"
            "$<TARGET_FILE:kickos_arch_${KICKOS_ARCH}>")
  set_tests_properties(arm_read_tp PROPERTIES TIMEOUT 60 LABELS host)
endif()

# No privileged TU may reach thread-local storage. Read as relocations and not as source: a
# thread_local reached through an inline function in a header leaves no `thread_local` in the
# privileged .cc that emitted the access. Registered on every cross board and not only where
# KICKOS_TLS is on.
if(KICKOS_BUILD_TESTS AND NOT (KICKOS_ARCH STREQUAL "sim"))
  set(_no_tls_scan
      "$<TARGET_FILE:kickos_kernel>"
      "$<TARGET_FILE:kickos_arch_${KICKOS_ARCH}>"
      "$<TARGET_FILE:kickos_chip_${KICKOS_CHIP}>"
      "$<TARGET_FILE:kickos_lib>")
  # ONE member of libkickos_user.a, never the whole archive: the rest of that archive is
  # unprivileged and its thread_local is correct there. newlib_reent.cc holds the word the
  # context switch seats, so a thread_local in it is read on the wrong thread pointer.
  #
  # NAMED ONLY WHERE IT EXISTS. user/CMakeLists.txt compiles no newlib porting layer on
  # x86_64, that toolchain building against no C library at all, so the archive has no such
  # member and the gate refuses the whole scan as vacuous rather than passing it.
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

# No archive holding kernel text may name the app's runtime: the kernel links its own
# memcpy/memset/strlen and formatter under the kickos/kruntime.h names. Read AFTER
# kickos_privatise_runtime has rewritten the emitted references, so a hit means the rewrite
# never reached this archive. kickos_lib is not scanned, being where the app's copies are
# defined.
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

# And no GOT reference from those same archives. The code model is scoped per TU
# (kickos_split_image_tu), and a weak extern in a TU left off that list emits one silently:
# the symbol resolves, so nothing fails the link until something else forces the one .got to
# be placed.
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

  # class/ holds the class-driver leaf, built as its OWN kickos_class_<chip> lib below. The
  # glob above is non-recursive so class/*.cc is already excluded; the filter keeps a future
  # recursive glob from pulling the leaf into the kernel-linked chip object, which would break
  # the leaf's bare-include-path contract.
  list(FILTER _chip_src EXCLUDE REGEX "/class/")

  # PMSAv8 MPU backend (Cortex-M33): a v8-M chip's mpu.cmake sets KICKOS_ARM_PMSAV8_SOURCE
  # to arm/common/arch_arm_pmsav8.cc. It is NOT in the always-compiled armv7m arch list (the
  # v6-M/v7-M fleet must never link it); it is pulled into the CHIP library only for an
  # enforcement build, where its strong kickos_arch_mpu_commit + arch_mpu_region_encodable
  # replace the v7-M fallback TUs.
  if(KICKOS_HAVE_MPU AND KICKOS_ARM_PMSAV8_SOURCE)
    list(APPEND _chip_src "${KICKOS_ARM_PMSAV8_SOURCE}")
  endif()

  # A chip whose flash boot needs a checksummed second-stage loader ships a boot2.S. It is
  # NOT compiled into the library directly: it must be assembled + linked to a raw binary,
  # wrapped with a ROM-verified CRC, and re-emitted as a data blob in the .boot2 linker
  # section. (RP2040: the bootrom loads the first 256 bytes of flash, checks a CRC-32/MPEG-2
  # over bytes 0..251, then runs it.) Everything is keyed on ${KICKOS_CHIP}: the checksum tool
  # is cmake/<chip>_checksum.py and the forced blob symbol is g_<chip>_boot2.
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
    # The loader is self-contained (no libc/startfiles) and its own linker script caps it at
    # 252 bytes. The -T is passed at the DRIVER level (see boot2.ld); with the newlib
    # toolchain no default linker script is injected, so this form applies boot2.ld alone.
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
    # The .boot2 blob is referenced by nothing, so (like g_isr_vector) the app
    # link must force it in from the chip archive; KEEP in the linker script only
    # protects an already-included object from --gc-sections.
    set(KICKOS_BOOT2_FORCE_SYMBOL "${_boot2_sym}" PARENT_SCOPE)
  endif()

  add_library(kickos_chip_${KICKOS_CHIP} STATIC ${_chip_src})
  kickos_apply_freestanding(kickos_chip_${KICKOS_CHIP})
  if(KICKOS_HAVE_ASPACE)
    kickos_privatise_runtime(kickos_chip_${KICKOS_CHIP})
    # Reset_Handler zeroes .appbss off __kickos_appbss_start/_end and registers the app's
    # unwind table from __eh_frame_start, all three app-half, and reaches the frame
    # registrar through a weak __register_frame. Absolute because the chip sources are
    # globbed absolute above.
    kickos_split_image_tu(kickos_chip_${KICKOS_CHIP}
      "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/chip_${KICKOS_CHIP}.cc")
  endif()
  # M7 D-cache: forward the option to the imxrt chip TU so arch_init calls
  # kickos_armv7m_dcache_enable(). Safe while the M7 is single-core with no DMA; an M4 DMA
  # driver MUST add clean/invalidate cache maintenance around its DMA buffers.
  #
  # KICKOS_USB_CONSOLE is derived from the service list in the top-level CMakeLists, above
  # add_subdirectory(arch).
  set(_imxrt_dcache_default ON)
  if(KICKOS_CHIP STREQUAL "imxrt1062" AND KICKOS_USB_CONSOLE)
    set(_imxrt_dcache_default OFF)
  endif()
  option(KICKOS_IMXRT_DCACHE "imxrt1062: enable the Cortex-M7 L1 D-cache"
         ${_imxrt_dcache_default})
  # option() CACHES, so an existing build directory switched onto a USB service list keeps a
  # stale ON.
  #
  # Coherency with the cache on rests on KOS_MEM_NOCACHE reaching BOTH of the block's grants,
  # so that no cacheable mapping of it survives (stage S7).
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
  # A v7-M chip that OVERRIDES arch_trace_now (mps2's semihosting clock) has that function on
  # the PendSV-tail emit path, so its TU must be FP-register-free too.
  if(KICKOS_TELEMETRY_ON AND KICKOS_CHIP STREQUAL "mps2")
    set_source_files_properties(
      "${CMAKE_CURRENT_SOURCE_DIR}/arm/chip/${KICKOS_CHIP}/chip_${KICKOS_CHIP}.cc"
      PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
  endif()
  target_include_directories(kickos_chip_${KICKOS_CHIP} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/${KICKOS_ARCH}/include"
    # A chip has no regs.h of its own, so a chip #include "regs.h" resolves to
    # <family>/common/regs.h here.
    "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/common"
    # The arch-neutral cpp constants a chip shares with its own startup assembly and linker
    # script (fatal_status.ld.h).
    "${CMAKE_CURRENT_SOURCE_DIR}/common"
    "${CMAKE_CURRENT_SOURCE_DIR}/../kernel/include"
    # A retunable chip (arch_cpu_clock_set) names the KOS_PSTATE_* selector from the
    # shared syscall ABI (user/include/kickos/sys/abi.h), which pulls errno.h from
    # kickos_system.
    "${CMAKE_CURRENT_SOURCE_DIR}/../user/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../system/include"
    # Telemetry: a chip's arch_shutdown may dump the ch1 ring via the transport
    # (mps2 semihosting file), so it reads lib's rtt.h.
    "${CMAKE_CURRENT_SOURCE_DIR}/../lib/include")
  install(TARGETS kickos_chip_${KICKOS_CHIP} EXPORT KickOSTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")

  # --- Class-driver leaf -------------------------------------------------------
  # A chip that factors register logic shared between the kernel and an unprivileged
  # userspace driver puts it in a FREESTANDING, STATELESS leaf under
  # <family>/chip/<chip>/class/: POD + free functions by explicit base, no ctor/dtor, no
  # mutable static. It is built as kickos_class_<chip>, its OWN static lib on a bare include
  # path: repo include/ + this chip's register dir ONLY, not kernel/include. KickOS links
  # kernel + driver into one MPU-partitioned ELF, so this leaf is ONE shared read-only copy
  # consumed from two trust domains and must carry no writable state.
  set(_class_dir
    "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/class")
  if(EXISTS "${_class_dir}")
    file(GLOB _class_src CONFIGURE_DEPENDS "${_class_dir}/*.cc")
    add_library(kickos_class_${KICKOS_CHIP} STATIC ${_class_src})
    kickos_apply_freestanding(kickos_class_${KICKOS_CHIP})
    # NO kernel/include: the leaf must compile without seeing kernel headers.
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

  # Linker script: a board that knows its silicon size (a genuine 20 KiB C8 against the
  # 10 KiB clone floor) overrides the chip default with its own copy under
  # boards/<board>/<chip>.ld. The shipped copy is always named <chip>.ld so the
  # INSTALL_INTERFACE -T path is board-independent.
  set(_ld "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/${KICKOS_CHIP}.ld")
  if(EXISTS "${CMAKE_SOURCE_DIR}/boards/${KICKOS_BOARD}/${KICKOS_CHIP}.ld")
    set(_ld "${CMAKE_SOURCE_DIR}/boards/${KICKOS_BOARD}/${KICKOS_CHIP}.ld")
  endif()
  # A CHIP MAY SHIP NO SCRIPT, and exactly one does: the x86_64 image is a PE32+ UEFI
  # application whose sections the firmware's own loader places, linked by a custom command
  # over `ld -m i386pep` rather than through the compiler driver the -T below reaches. Every
  # other chip ships one, so this guard changes nothing for them; without it the file(READ)
  # just below fails the configure on a missing path.
  if(EXISTS "${_ld}")
    # Every image links --orphan-handling=error, so a script missing the shared vocabulary
    # fails the LINK with a section name and no hint of what is owed. Refuse at configure
    # instead, and name the fix.
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

    # Preprocess the linker script through cpp so KICKOS_HAVE_MPU gates sections: a
    # non-enforcement build reserves no app-data block/region. The GENERATED script is named
    # <chip>.ld too, so the INSTALL_INTERFACE -T path is unchanged.
    set(_ld_gen "${CMAKE_CURRENT_BINARY_DIR}/${KICKOS_CHIP}.ld")
    # Per-build app-data window size. Each chip .ld carries its own freestanding default; a
    # full-C++ build needs the larger writable window for the heap and libstdc++/unwind state.
    set(_ld_defs -DKICKOS_HAVE_MPU=${KICKOS_HAVE_MPU})
    # A chip whose boot handshake needs storage outside .bss carves it per hart, so its script
    # must know the count; at one core the carve is absent rather than empty.
    list(APPEND _ld_defs -DKICKOS_NUM_CORES=${KICKOS_NUM_CORES})
    # KICKOS_TLS_FIT_ASSERT needs the knob, the stride and the arch's ABI bias. The bias is
    # scraped from the arch's context.h, the header the compiler reads being what owns it.
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
    endif()
    if(DEFINED KICKOS_APPDATA_SIZE)
      list(APPEND _ld_defs -DKICKOS_APPDATA_SIZE=${KICKOS_APPDATA_SIZE})
    endif()
    # Non-MPU heap: the .userheap section carved before the thread arena. On an MPU chip
    # the heap is the .appdata window pad instead, so this is inert there.
    if(DEFINED KICKOS_USER_HEAP_SIZE)
      list(APPEND _ld_defs -DKICKOS_USER_HEAP_SIZE=${KICKOS_USER_HEAP_SIZE})
    endif()
    # Opt-in link-time heap floor: the .ld asserts at least this many bytes are available.
    # Undefined by default, so a too-small heap is a runtime bump failure, not a link error.
    if(DEFINED KICKOS_HEAP_MIN)
      list(APPEND _ld_defs -DKICKOS_HEAP_MIN=${KICKOS_HEAP_MIN})
    endif()
    # Boot-stack geometry for the KICKOS_BOOT_ARENA_ASSERT every chip .ld carries: the arena
    # must hold kmain's idle + root stacks INCLUDING the natural-alignment padding the bump
    # allocator pays. A .ld without the assert fails the configure.
    include("${CMAKE_SOURCE_DIR}/cmake/boot_arena.cmake")
    kickos_boot_arena_defs("${CMAKE_CURRENT_SOURCE_DIR}"
                           "kickos_arch_${KICKOS_ARCH}" "kickos_chip_${KICKOS_CHIP}"
                           "${_ld}" _boot_defs
                           _mpu_min_region_cfg _mpu_region_pow2_cfg)
    list(APPEND _ld_defs ${_boot_defs})
    # The scraped region-encoding seam, read by the one app TU that checks the runtime seam
    # against it. Unset on the sim, which has no KICKOS_CHIP and never enters this block.
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
    # Shipped so an out-of-tree consumer can link an image: the `kickos` INTERFACE bakes an
    # INSTALL_INTERFACE -T pointing at this copy.
    install(FILES "${_ld_gen}" DESTINATION "${CMAKE_INSTALL_LIBDIR}/kickos")
    set(KICKOS_LINKER_SCRIPT "${_ld_gen}" PARENT_SCOPE)
  endif()
endif()
