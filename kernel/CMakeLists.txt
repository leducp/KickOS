# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

set(KICKOS_KERNEL_SOURCES
  sched/sched.cc
  sched/policy_fifo_rr.cc
  thread/thread.cc
  domain/domain.cc
  grant/grant.cc
  ktrace/ktrace.cc
  init/console.cc
  init/console_tx.cc
  init/led.cc
  init/instance.cc
  init/kmain.cc)

# Optional components are appended as they land (kept bisectable).
foreach(_f sync/sync.cc time/time.cc time/clock_select.cc syscall/syscall.cc syscall/cap.cc irq/irq.cc)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_f}")
    list(APPEND KICKOS_KERNEL_SOURCES "${_f}")
  endif()
endforeach()

# Microbenchmark accumulator (paired with the cycle-counter bracket in switch.S).
# Built for every arch in a KICKOS_BENCH build: it supplies the throughput metric's
# helpers everywhere, plus the cycle accumulator + IRQ trigger on the arches that have
# a counter (armv7m DWT/STIR, rv32imac rdcycle/MTIME + inject, rxv3 CMTW1, xtensa
# CCOUNT). On M0/sim it links but the cycle path is inert (switch.S brackets nothing).
if(KICKOS_BENCH)
  list(APPEND KICKOS_KERNEL_SOURCES bench/bench.cc)
endif()

add_library(kickos_kernel STATIC ${KICKOS_KERNEL_SOURCES})
kickos_apply_freestanding(kickos_kernel)

# Build-time stamp (kickos_build_stamp[], read by the kmain banner). Regenerated on
# EVERY build so the banner reflects the image actually linked -- __DATE__/__TIME__ bakes
# per-TU and goes stale on incremental builds (see cmake/build_stamp.cmake). The custom
# target has no OUTPUT, so it is always out of date and reruns each build; the kernel lib
# recompiles the tiny generated TU and dependents relink, keeping the stamp truthful.
set(_kos_build_stamp "${CMAKE_BINARY_DIR}/kickos_build_stamp.cc")
add_custom_target(kickos_build_stamp_gen
  BYPRODUCTS "${_kos_build_stamp}"
  COMMAND "${CMAKE_COMMAND}"
    "-DOUT=${_kos_build_stamp}" "-DSRC=${CMAKE_SOURCE_DIR}"
    -P "${CMAKE_SOURCE_DIR}/cmake/build_stamp.cmake"
  COMMENT "kickos: stamping build time"
  VERBATIM)
set_source_files_properties("${_kos_build_stamp}" PROPERTIES GENERATED TRUE)
target_sources(kickos_kernel PRIVATE "${_kos_build_stamp}")
add_dependencies(kickos_kernel kickos_build_stamp_gen)

# The switch-completion hook (kickos_trace_switch_done in ktrace.cc) runs in the
# ARM PendSV tail with the incoming thread's FP registers still live, so its emit
# path must not touch any FP register: build it -mgeneral-regs-only on ARM when
# telemetry is enabled. (No-op on the sim, where the hook runs in ordinary code.)
if(KICKOS_TELEMETRY_ON AND (KICKOS_ARCH STREQUAL "armv7m" OR KICKOS_ARCH STREQUAL "armv6m"))
  set_source_files_properties(ktrace/ktrace.cc PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
endif()

# console_tx_isr drains the ring from ISR context, so its emit path must take no
# FP register (no lazy FP stacking on the drain) -- same treatment as ktrace.
if(KICKOS_ARCH STREQUAL "armv7m" OR KICKOS_ARCH STREQUAL "armv6m")
  set_source_files_properties(init/console_tx.cc PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
endif()

# armv7m only: kickos_bench_switch_done runs in the PendSV tail with the incoming
# thread's lazily-stacked FP live, so its path must take no FP register (same rule
# as ktrace/console_tx). -mgeneral-regs-only is not valid on rxv3/rv32imac, and
# neither needs it (rxv3 calls it before the FP restore; rv32imac has no hazard).
if(KICKOS_BENCH AND KICKOS_ARCH STREQUAL "armv7m")
  set_source_files_properties(bench/bench.cc PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
endif()
target_include_directories(kickos_kernel PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
# The kernel implements the userspace syscall ABI, so it reads that contract; the
# system headers (errno.h via sync.h, cap_index.h via cap.h) live in kickos_system.
target_include_directories(kickos_kernel PRIVATE
  "${PROJECT_SOURCE_DIR}/user/include"
  "${PROJECT_SOURCE_DIR}/system/include")
target_link_libraries(kickos_kernel PUBLIC kickos_arch_${KICKOS_ARCH} kickos_lib)

# Identity baked into the boot banner (kmain kbanner()).
target_compile_definitions(kickos_kernel PRIVATE
  KICKOS_VERSION="${PROJECT_VERSION}"
  KICKOS_BOARD_NAME="${KICKOS_BOARD}"
  KICKOS_ARCH_NAME="${KICKOS_ARCH}")

install(TARGETS kickos_kernel EXPORT KickOSTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")

if(CONFIG_SCHED_PERIODIC_TICK)
  target_compile_definitions(kickos_kernel PUBLIC KICKOS_SCHED_PERIODIC_TICK=1)
endif()
