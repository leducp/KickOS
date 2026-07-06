# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# ISA family routes the chip glob + per-arch include path. The arm toolchain
# predates the seam and does not set it, so default to "arm"; the rx toolchain
# (and, separately, an xtensa one) set it explicitly (spike §1).
if(NOT KICKOS_ARCH_FAMILY)
  set(KICKOS_ARCH_FAMILY "arm")
endif()

if(KICKOS_ARCH STREQUAL "sim")
  add_library(kickos_arch_sim STATIC sim/sim.cc sim/start.cc)
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
  # M1: context switch (PendSV) + syscall trap (SVC trampoline) in switch.S,
  # the Cortex-M core-generic seam in arch_armv7m.cc. The chip layer
  # (arch/arm/chip/*, M1 item 10) adds arch_init/arch_console_write + the
  # linker script; until then this builds as a static library only.
  add_library(kickos_arch_armv7m STATIC
    arm/armv7m/arch_armv7m.cc
    arm/common/arch_arm_common.cc
    arm/armv7m/switch.S)
  kickos_apply_freestanding(kickos_arch_armv7m)
  # arch_trace_now (weak DWT read) lives here and runs on the PendSV-tail emit
  # path, so it too must be FP-register-free (v7-M has an FPU). Whole TU is
  # integer-only, so -mgeneral-regs-only is safe.
  if(KICKOS_TELEMETRY_ON)
    set_source_files_properties(arm/armv7m/arch_armv7m.cc
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
  # M1 item 11: Cortex-M0/M0+ (RP2040). PRIMASK crit + v6-M-subset switch.S; the
  # chip supplies arch_clock_now (no DWT) + arch_init/console + linker script.
  add_library(kickos_arch_armv6m STATIC
    arm/armv6m/arch_armv6m.cc
    arm/common/arch_arm_common.cc
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
  # M1.x item 11h: Renesas RX72M (RXv3). Uniform SWINT deferred context switch +
  # INT-based syscall trap (supervisor-on-USP) in switch.S; the core-generic seam
  # (crit section via MVTIPL, CMTW clock/timer, ICUD mask/unmask) in arch_rxv3.cc.
  # The chip layer (arch/rx/chip/*) supplies arch_init/arch_console_write + the
  # linker script.
  add_library(kickos_arch_rxv3 STATIC
    rx/rxv3/arch_rxv3.cc
    rx/rxv3/switch.S)
  kickos_apply_freestanding(kickos_arch_rxv3)
  target_include_directories(kickos_arch_rxv3 PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/rx/rxv3/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  install(TARGETS kickos_arch_rxv3 EXPORT KickOSTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  set(KICKOS_ARCH_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/rx/rxv3/include" PARENT_SCOPE)
elseif(KICKOS_ARCH STREQUAL "lx6")
  # M1.x item 11c: ESP32 Xtensa LX6 -- the first non-ARM ISA. Windowed ABI, so the
  # context switch spills all live register windows to the outgoing thread's stack
  # before the SP swap (switch.S); the mandatory window over/underflow + interrupt
  # vectors live in the chip startup.S. Syscall is a plain call (no privilege
  # split); RSIL crit section; CCOUNT/CCOMPARE0 timer. The chip supplies
  # arch_init/console/shutdown + startup vectors + linker script.
  add_library(kickos_arch_lx6 STATIC
    xtensa/lx6/arch_xtensa.cc
    xtensa/lx6/switch.S)
  kickos_apply_freestanding(kickos_arch_lx6)
  target_include_directories(kickos_arch_lx6 PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/xtensa/lx6/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  install(TARGETS kickos_arch_lx6 EXPORT KickOSTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  set(KICKOS_ARCH_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/xtensa/lx6/include" PARENT_SCOPE)
else()
  message(FATAL_ERROR "KickOS: arch backend '${KICKOS_ARCH}' not available")
endif()

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

  # A chip whose flash boot needs a checksummed second-stage loader ships a
  # boot2.S. It is NOT compiled into the library directly: it must be assembled +
  # linked to a raw binary, wrapped with a ROM-verified CRC, and re-emitted as a
  # data blob in the .boot2 linker section. (RP2040: the bootrom loads the first
  # 256 bytes of flash, checks a CRC-32/MPEG-2 over bytes 0..251, then runs it.)
  # Everything is keyed on ${KICKOS_CHIP}: the CPU baseline comes from the
  # toolchain (KICKOS_MCPU_FLAGS), the checksum tool is cmake/<chip>_checksum.py,
  # and the forced blob symbol is g_<chip>_boot2 -- so a future boot2 chip that
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
    # 252 bytes. The -T is passed at the DRIVER level (not -Wl,-T) so the picolibc
    # toolchain suppresses its default linker script (see boot2.ld).
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
    "${CMAKE_CURRENT_SOURCE_DIR}/../kernel/include"
    # Telemetry: a chip's arch_shutdown may dump the ch1 ring via the transport
    # (mps2 semihosting file), so it reads lib's rtt.h (chip -> lib, downward).
    "${CMAKE_CURRENT_SOURCE_DIR}/../lib/include")
  install(TARGETS kickos_chip_${KICKOS_CHIP} EXPORT KickOSTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  # Linker script: a board that knows its silicon size (e.g. a genuine 20 KiB C8
  # vs the 10 KiB clone floor) overrides the chip default with its own copy under
  # boards/<board>/<chip>.ld; otherwise use the chip default. The shipped copy is
  # always named <chip>.ld so the INSTALL_INTERFACE -T path is board-independent.
  set(_ld "${CMAKE_CURRENT_SOURCE_DIR}/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/${KICKOS_CHIP}.ld")
  if(EXISTS "${CMAKE_SOURCE_DIR}/boards/${KICKOS_BOARD}/${KICKOS_CHIP}.ld")
    set(_ld "${CMAKE_SOURCE_DIR}/boards/${KICKOS_BOARD}/${KICKOS_CHIP}.ld")
  endif()
  # Ship the linker script so an out-of-tree consumer can link an image (the
  # `kickos` INTERFACE bakes an INSTALL_INTERFACE -T pointing at this copy).
  install(FILES "${_ld}" DESTINATION "${CMAKE_INSTALL_LIBDIR}/kickos")
  set(KICKOS_LINKER_SCRIPT "${_ld}" PARENT_SCOPE)
endif()
