# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# ISA family routes the chip glob + per-arch include path. The arm toolchain
# predates the seam and does not set it, so default to "arm"; the rx toolchain
# (and, separately, an xtensa one) set it explicitly (spike section 1).
if(NOT KICKOS_ARCH_FAMILY)
  set(KICKOS_ARCH_FAMILY "arm")
endif()

# --- Seam fallbacks: the lone-TU rule ----------------------------------------
# A seam whose backend is optional gets its fallback body in a translation unit of its
# OWN, named <symbol>_default.cc, that defines EXACTLY ONE symbol. A backend that
# defines the symbol satisfies the reference from its own member, so the fallback member
# is never extracted; a second symbol in a fallback TU would drag the member in anyway
# and collide with that backend. tests/check_seam_defaults.sh asserts both the
# one-symbol rule and the per-board resolution.
#
# This is LINKER behaviour, not language semantics. It is behaviour every Unix-like
# linker and MSVC `.lib` share, unlike __attribute__((weak)), whose archive interaction
# is implementation-defined.
#
# THE LOAD-BEARING INVARIANT IS ON THE BACKEND SIDE: a backend's definition must live in
# an archive member that is ALWAYS ANCHORED, i.e. one the link pulls for some other
# reason (chip_<chip>.cc, force-loaded via -u g_isr_vector -> startup.S ->
# Reset_Handler; usic_uart.cc, pulled for arch_console_tx_backend). A backend that puts
# its definition in a dedicated TU nothing else references is NOT protected by scan
# order: the fallback resolves the reference first and the board SILENTLY DECLINES at
# runtime. Scan order (fallbacks live in kickos_arch_*, which the rescan group scans
# AFTER kickos_chip_*) is a backstop only. Leg 2 of the gate is what enforces the
# anchoring rule.
#
# Arch-neutral fallbacks for seams the KERNEL declares. They must NOT sit in
# kickos_kernel: the group scans the kernel archive before the chip archive, so a
# fallback there would be extracted in the same pass that makes the symbol undefined and
# then collide with the chip's definition. Each list below drops the members that the
# arch backend itself defines: two definitions of one symbol in ONE archive resolve by
# member order, which is exactly the implementation-defined behaviour this replaces.
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
  common/arch_fault_is_user_thread_default.cc
  common/arch_fault_redirect_to_exit_default.cc
  common/arch_periph_clock_hz_default.cc
  common/arch_periph_enable_default.cc
  common/arch_periph_reg_write_default.cc
  common/arch_pinmux_set_default.cc
  common/kfault_terminate_default.cc)
# arch_reboot must be ABSENT from a production image, so its fallback follows the same
# gate as its callers and its chip backends.
if(KICKOS_ENABLE_SELFTEST)
  list(APPEND KICKOS_SEAM_DEFAULTS_COMMON common/arch_reboot_default.cc)
endif()
if(KICKOS_FAULT_ISOLATION)
  list(REMOVE_ITEM KICKOS_SEAM_DEFAULTS_COMMON
    common/arch_fault_is_user_thread_default.cc
    common/arch_fault_redirect_to_exit_default.cc)
endif()

# ARM PMSA + core fallbacks, shared by armv6m and armv7m.
set(KICKOS_SEAM_DEFAULTS_ARM
  arm/common/arch_bitband_present_default.cc
  arm/common/arch_idle_wait_default.cc
  arm/common/arch_mpu_min_region_default.cc
  arm/common/arch_mpu_region_encodable_default.cc
  arm/common/arch_mpu_region_pow2_default.cc
  arm/common/arch_shutdown_default.cc
  arm/common/kickos_arch_mpu_commit_default.cc)
# The fixed-region hook exists only where the region type and its caller do.
if(KICKOS_HAVE_MPU)
  list(APPEND KICKOS_SEAM_DEFAULTS_ARM arm/common/kickos_arm_mpu_fixed_default.cc)
endif()

if(KICKOS_ARCH STREQUAL "sim")
  # sim.cc IS the backend for arch_console_write_sync, arch_console_tx_backend,
  # arch_console_reclaim_window, kfault_terminate and arch_periph_reg_write, so those fallbacks
  # are dropped: the sim has no chip archive, and a fallback beside a backend in ONE
  # archive resolves by member order. SystemCoreClock has no chip to define it here.
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
  # Telemetry: the sim flushes the RTT ch1 ring to a file at shutdown, so it reads
  # lib's rtt.h (arch -> lib is the ordinary downward direction; lib is the leaf).
  target_include_directories(kickos_arch_sim PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../lib/include")
  install(TARGETS kickos_arch_sim EXPORT KickOSTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  # Kernel headers for the reverse-direction callbacks are pulled via arch.h.
  set(KICKOS_ARCH_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sim/include" PARENT_SCOPE)
elseif(KICKOS_ARCH STREQUAL "armv7m")
  # Context switch (PendSV) + syscall trap (SVC trampoline) in switch.S,
  # the Cortex-M core-generic seam in arch_armv7m.cc. The chip layer
  # (arch/arm/chip/*) adds arch_init/arch_console_write + the
  # linker script; until then this builds as a static library only.
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
    arm/armv7m/switch.S)
  kickos_apply_freestanding(kickos_arch_armv7m)
  # The DWT arch_trace_now fallback runs on the PendSV-tail emit path, so its TU (and
  # arch_armv7m.cc, which carries the rest of that path) must be FP-register-free
  # (v7-M has an FPU). Both TUs are integer-only, so -mgeneral-regs-only is safe.
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
  # Cortex-M0/M0+ (RP2040). PRIMASK crit + v6-M-subset switch.S; the
  # chip supplies arch_clock_now (no DWT) + arch_init/console + linker script.
  # No arch_trace_now fallback: v6-M has no DWT, so the trace clock is a chip contract.
  # A Cortex-M0 has no privilege ring and keeps the declining fault fallbacks instead.
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
  # Renesas RX72M (RXv3). Uniform SWINT deferred context switch +
  # INT-based syscall trap (supervisor-on-USP) in switch.S; the core-generic seam
  # (crit section via MVTIPL, CMTW clock/timer, ICUD mask/unmask) in arch_rxv3.cc.
  # The chip layer (arch/rx/chip/*) supplies arch_init/arch_console_write + the
  # linker script.
  add_library(kickos_arch_rxv3 STATIC
    rx/rxv3/arch_rxv3.cc
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
  # ESP32 Xtensa LX6, the first non-ARM ISA. Windowed ABI, so the
  # context switch spills all live register windows to the outgoing thread's stack
  # before the SP swap (switch.S); the mandatory window over/underflow + interrupt
  # vectors live in the chip startup.S. Syscall is a plain call (no privilege
  # split); RSIL crit section; CCOUNT/CCOMPARE0 timer. The chip supplies
  # arch_init/console/shutdown + startup vectors + linker script.
  add_library(kickos_arch_lx6 STATIC
    xtensa/lx6/arch_xtensa.cc
    common/arch_ram_common.cc
    common/startup_ranges.cc
    ${KICKOS_SEAM_DEFAULTS_COMMON}
    xtensa/lx6/arch_clock_now_default.cc
    xtensa/lx6/switch.S)
  kickos_apply_freestanding(kickos_arch_lx6)
  target_include_directories(kickos_arch_lx6 PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/xtensa/lx6/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  install(TARGETS kickos_arch_lx6 EXPORT KickOSTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  set(KICKOS_ARCH_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/xtensa/lx6/include" PARENT_SCOPE)
elseif(KICKOS_ARCH STREQUAL "rv32imac")
  # RISC-V RV32IMAC (ESP32-C6 + QEMU virt): single mtvec trap that demuxes on
  # mcause. ONE deferred switcher via CLINT msip (the RX SWINT / ARM PendSV
  # model), ecall syscall trap -> a privileged-thread trampoline (switch.S); the
  # core-generic seam (mstatus.MIE crit, rdcycle trace clock, wfi idle, PMP stub)
  # in arch_rv32imac.cc. No F/D extension -> soft-float, so the switch banks no FP.
  # The chip layer (arch/riscv/chip/{virt,esp32c6}) supplies arch_init/console/
  # shutdown + the clock/timer + the CLINT base + linker script + startup.
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
else()
  message(FATAL_ERROR "KickOS: arch backend '${KICKOS_ARCH}' not available")
endif()

# The declining fallbacks name the KOS_E* taxonomy, which lives in kickos_system's
# headers; no other arch-layer TU needs them.
target_include_directories(kickos_arch_${KICKOS_ARCH} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../system/include")

# --- Chip backend (startup + linker script + clocks + console) --------------
# Built when the selected board resolves to a chip that is brought up. Sources:
# every .cc + .S under <family>/chip/<chip>/. The family (arm|rx|xtensa) routes the
# chip glob + arch-include path so a non-ARM ISA is a drop-in. Exports the linker
# script path to the parent scope so the `kickos` interface target bakes it in.
if(KICKOS_CHIP AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}")
  # CONFIGURE_DEPENDS: adding/removing a chip source reconfigures automatically.
  file(GLOB _chip_src CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/*.S")

  # The class/ subdir holds the Rule 6 class-driver leaf (below); it is built as
  # its OWN kickos_class_<chip> lib, never into the chip library. The glob above is
  # non-recursive so class/*.cc is already excluded, but filter defensively so a
  # future recursive glob cannot silently pull the leaf into the kernel-linked chip
  # object (which would break the leaf's separate bare-include-path contract).
  list(FILTER _chip_src EXCLUDE REGEX "/class/")

  # PMSAv8 MPU backend (Cortex-M33): a v8-M chip's mpu.cmake sets
  # KICKOS_ARM_PMSAV8_SOURCE to arm/common/arch_arm_pmsav8.cc. It is NOT in the
  # always-compiled armv7m arch list (the v6-M/v7-M fleet must never link it); it is
  # pulled into the CHIP library only for an enforcement build so its strong
  # kickos_arch_mpu_commit + arch_mpu_region_encodable replace the v7-M fallback TUs.
  # Gated on KICKOS_HAVE_MPU so a non-enforcement build of the same chip stays
  # byte-identical to the plain armv7m fleet.
  if(KICKOS_HAVE_MPU AND KICKOS_ARM_PMSAV8_SOURCE)
    list(APPEND _chip_src "${KICKOS_ARM_PMSAV8_SOURCE}")
  endif()

  # A chip whose flash boot needs a checksummed second-stage loader ships a
  # boot2.S. It is NOT compiled into the library directly: it must be assembled +
  # linked to a raw binary, wrapped with a ROM-verified CRC, and re-emitted as a
  # data blob in the .boot2 linker section. (RP2040: the bootrom loads the first
  # 256 bytes of flash, checks a CRC-32/MPEG-2 over bytes 0..251, then runs it.)
  # Everything is keyed on ${KICKOS_CHIP}: the CPU baseline comes from the
  # toolchain (KICKOS_MCPU_FLAGS), the checksum tool is cmake/<chip>_checksum.py,
  # and the forced blob symbol is g_<chip>_boot2, so a future boot2 chip that
  # doesn't supply those fails loudly instead of building a silently-wrong image.
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
    # Stage 1: assemble + link the position-independent loader to a raw image.
    # It is self-contained (no libc/startfiles); its own linker script caps it at
    # 252 bytes. The -T is passed at the DRIVER level (see boot2.ld); with the
    # newlib toolchain no default linker script is injected, so this form only
    # applies our boot2.ld.
    add_custom_command(
      OUTPUT "${_boot2_bin}"
      COMMAND ${CMAKE_C_COMPILER} ${KICKOS_MCPU_FLAGS} -mthumb
              -nostdlib -nostartfiles -Wl,--entry=${_boot2_entry}
              -T "${_boot2_ld}" "${_boot2_src}" -o "${_boot2_elf}"
      COMMAND ${CMAKE_OBJCOPY} -O binary "${_boot2_elf}" "${_boot2_bin}"
      DEPENDS "${_boot2_src}" "${_boot2_ld}"
      COMMENT "boot2: assembling ${KICKOS_CHIP} second stage -> raw binary"
      VERBATIM)
    # Stage 2: append the CRC and re-emit as a .boot2 data section.
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
  # M7 D-cache: forward the option to the imxrt chip TU so arch_init calls
  # kickos_armv7m_dcache_enable(). Default ON, silicon-validated (selftest 43/43 plus a
  # ~38 M-switch soak under enforcement, a measurable throughput win). Safe while the M7 is
  # single-core with no DMA; M4 DMA drivers MUST add clean/invalidate cache maintenance
  # around DMA buffers. Set OFF to opt back out.
  option(KICKOS_IMXRT_DCACHE "imxrt1062: enable the Cortex-M7 L1 D-cache" ON)
  if(KICKOS_CHIP STREQUAL "imxrt1062" AND KICKOS_IMXRT_DCACHE)
    target_compile_definitions(kickos_chip_${KICKOS_CHIP} PRIVATE KICKOS_IMXRT_DCACHE=1)
  endif()
  # A v7-M chip that OVERRIDES arch_trace_now (mps2's semihosting clock) has that
  # function on the PendSV-tail emit path, so its TU must be FP-register-free too.
  # (v6-M chips have no FPU, so -mgeneral-regs-only is moot there.)
  if(KICKOS_TELEMETRY_ON AND KICKOS_CHIP STREQUAL "mps2")
    set_source_files_properties(
      "${CMAKE_CURRENT_SOURCE_DIR}/arm/chip/${KICKOS_CHIP}/chip_${KICKOS_CHIP}.cc"
      PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
  endif()
  target_include_directories(kickos_chip_${KICKOS_CHIP} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/${KICKOS_ARCH}/include"
    # The family-common backend headers (arm/common/regs.h: core SCB/SysTick regs +
    # the shared kickos_armv7m_enable_fpu). A chip has no regs.h of its own, so a
    # chip #include "regs.h" resolves here.
    "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/common"
    "${CMAKE_CURRENT_SOURCE_DIR}/../kernel/include"
    # A retunable chip (arch_cpu_clock_set) names the KOS_PSTATE_* selector from the
    # shared syscall ABI (user/include/kickos/sys/abi.h), which pulls errno.h from
    # kickos_system.
    "${CMAKE_CURRENT_SOURCE_DIR}/../user/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../system/include"
    # Telemetry: a chip's arch_shutdown may dump the ch1 ring via the transport
    # (mps2 semihosting file), so it reads lib's rtt.h (chip -> lib, downward).
    "${CMAKE_CURRENT_SOURCE_DIR}/../lib/include")
  install(TARGETS kickos_chip_${KICKOS_CHIP} EXPORT KickOSTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")

  # --- Rule 6 class-driver leaf (M4, decision R-A) -----------------------------
  # A chip that factors register logic shared between the kernel and an
  # unprivileged userspace driver puts it in a FREESTANDING, STATELESS leaf under
  # <family>/chip/<chip>/class/: POD + free functions by explicit base, no
  # ctor/dtor, no mutable static (rules 3/4). It is built as kickos_class_<chip>,
  # its OWN static lib on a bare include path: repo include/ + this chip's register
  # dir ONLY, deliberately NOT kernel/include. KickOS links kernel + driver into one
  # MPU-partitioned ELF, so this leaf is ONE shared read-only copy consumed from two
  # trust domains, so it must carry no writable state. The kernel-side
  # (timebase) consumer is DEFERRED to Phase A; today the leaf is pulled in by the
  # chip's userspace driver/app so it is not --gc-sections-stripped.
  set(_class_dir
    "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/class")
  if(EXISTS "${_class_dir}")
    file(GLOB _class_src CONFIGURE_DEPENDS "${_class_dir}/*.cc")
    add_library(kickos_class_${KICKOS_CHIP} STATIC ${_class_src})
    kickos_apply_freestanding(kickos_class_${KICKOS_CHIP})
    # PUBLIC: the class/ dir carries the leaf's own header, propagated to whichever
    # driver/app links this lib. PRIVATE: repo include/ + the chip register dir,
    # the leaf's whole world. NO kernel/include (verified via compile_commands): the
    # leaf must compile under the kernel bar without seeing kernel headers.
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

  # Linker script: a board that knows its silicon size (e.g. a genuine 20 KiB C8
  # vs the 10 KiB clone floor) overrides the chip default with its own copy under
  # boards/<board>/<chip>.ld; otherwise use the chip default. The shipped copy is
  # always named <chip>.ld so the INSTALL_INTERFACE -T path is board-independent.
  set(_ld "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/${KICKOS_CHIP}.ld")
  if(EXISTS "${CMAKE_SOURCE_DIR}/boards/${KICKOS_BOARD}/${KICKOS_CHIP}.ld")
    set(_ld "${CMAKE_SOURCE_DIR}/boards/${KICKOS_BOARD}/${KICKOS_CHIP}.ld")
  endif()
  # Preprocess the linker script through cpp so KICKOS_HAVE_MPU gates sections: a
  # non-enforcement build reserves no app-data block/region. cpp strips comments and passes
  # MEMORY/SECTIONS/LONG/wildcard tokens through unchanged. The GENERATED script is named
  # <chip>.ld too, so the INSTALL_INTERFACE -T path is unchanged.
  set(_ld_gen "${CMAKE_CURRENT_BINARY_DIR}/${KICKOS_CHIP}.ld")
  # Per-build app-data window size. Each chip .ld carries its own freestanding default; a
  # full-C++ build needs the larger writable window for the heap and libstdc++/unwind state.
  set(_ld_defs -DKICKOS_HAVE_MPU=${KICKOS_HAVE_MPU})
  if(DEFINED KICKOS_APPDATA_SIZE)
    list(APPEND _ld_defs -DKICKOS_APPDATA_SIZE=${KICKOS_APPDATA_SIZE})
  endif()
  # Non-MPU heap: the .userheap section carved before the thread arena. On an MPU chip
  # the heap is the .appdata window pad instead, so this is inert there.
  if(DEFINED KICKOS_USER_HEAP_SIZE)
    list(APPEND _ld_defs -DKICKOS_USER_HEAP_SIZE=${KICKOS_USER_HEAP_SIZE})
  endif()
  # Opt-in link-time heap floor: the .ld ASSERTs at least this many bytes are available.
  # Undefined by default, so a too-small heap is a runtime bump failure, not a link error.
  if(DEFINED KICKOS_HEAP_MIN)
    list(APPEND _ld_defs -DKICKOS_HEAP_MIN=${KICKOS_HEAP_MIN})
  endif()
  # Boot-stack geometry for the KICKOS_BOOT_ARENA_ASSERT every chip .ld carries: the
  # arena must hold kmain's idle + root stacks INCLUDING the natural-alignment padding
  # the bump allocator pays, and only the linker knows where the arena starts. A .ld
  # without the assert fails the configure.
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
  add_custom_command(
    OUTPUT "${_ld_gen}"
    COMMAND ${CMAKE_C_COMPILER} -E -P -x c ${_ld_defs}
            "-I${CMAKE_CURRENT_SOURCE_DIR}/common"
            "${_ld}" -o "${_ld_gen}"
    DEPENDS "${_ld}" "${CMAKE_CURRENT_SOURCE_DIR}/common/boot_arena.ld.h"
    COMMENT "linker: preprocess ${KICKOS_CHIP}.ld (KICKOS_HAVE_MPU=${KICKOS_HAVE_MPU})"
    VERBATIM)
  add_custom_target(kickos_ldscript_${KICKOS_CHIP} ALL DEPENDS "${_ld_gen}")
  # Ship the generated linker script so an out-of-tree consumer can link an image (the
  # `kickos` INTERFACE bakes an INSTALL_INTERFACE -T pointing at this copy).
  install(FILES "${_ld_gen}" DESTINATION "${CMAKE_INSTALL_LIBDIR}/kickos")
  set(KICKOS_LINKER_SCRIPT "${_ld_gen}" PARENT_SCOPE)
endif()
