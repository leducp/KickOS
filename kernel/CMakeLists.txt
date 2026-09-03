# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

set(KICKOS_KERNEL_SOURCES
  sched/sched.cc
  sched/policy_fifo_rr.cc
  thread/thread.cc
  thread/tls.cc
  thread/reent.cc
  thread/park.cc
  domain/domain.cc
  task/task.cc
  grant/grant.cc
  ktrace/ktrace.cc
  init/console.cc
  init/console_tx.cc
  init/fault.cc
  init/led.cc
  init/instance.cc
  init/kmain.cc)

foreach(_f mem/aspace.cc mem/frame.cc mem/frame_pool.cc mem/ustack.cc mem/vrange.cc sync/sync.cc sync/klock.cc sync/corestart.cc amp/ampwindow.cc sync/smptrace.cc time/time.cc time/clock_select.cc syscall/syscall.cc syscall/syscall_aspace.cc syscall/syscall_mem.cc syscall/syscall_obj.cc syscall/syscall_ipc.cc syscall/syscall_ipc_fast.cc syscall/syscall_thread.cc syscall/cap.cc irq/irq.cc)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_f}")
    list(APPEND KICKOS_KERNEL_SOURCES "${_f}")
  endif()
endforeach()

# Microbenchmark accumulator, paired with the cycle-counter bracket in switch.S. On M0 and
# the sim it links but the cycle path is inert, switch.S bracketing nothing there.
if(KICKOS_BENCH)
  list(APPEND KICKOS_KERNEL_SOURCES bench/bench.cc)
endif()

# The kernel's own copies of the runtime, under the private names kickos/kruntime.h declares.
# ONLY where a translating backend splits the image in two: a region backend serves both
# privilege levels from one text mapping, so there the header aliases the app's names and a
# second copy does not fit on the two 64 KB parts.
if(KICKOS_HAVE_ASPACE)
  list(APPEND KICKOS_KERNEL_SOURCES klib/kstring.cc klib/kfmt.cc)
endif()

add_library(kickos_kernel STATIC ${KICKOS_KERNEL_SOURCES})
kickos_apply_freestanding(kickos_kernel)

if(KICKOS_HAVE_ASPACE)
  kickos_privatise_runtime(kickos_kernel)
  # The kernel TUs that name the app's half. kmain reaches kickos_init_args and
  # kickos_root_entry and holds weak externs for _kickos_heap_start/_limit and
  # kickos_app_build_stamp; reent reaches kickos_reent_seam; aspace and domain hold only
  # weak externs (the app rom/sram window and code/appdata bounds), whose GOT references
  # are what one .got cannot serve from both halves.
  kickos_split_image_tu(kickos_kernel
    init/kmain.cc
    thread/reent.cc
    mem/aspace.cc
    domain/domain.cc)
  # The word loops the included source carries are the memcpy/memset idiom, and the pass
  # that rewrites that idiom into a library call would rewrite each of them into a call to
  # the APP's copy under its ordinary name. lib/CMakeLists.txt carries the same flag.
  set_source_files_properties(klib/kstring.cc PROPERTIES
    COMPILE_OPTIONS "-fno-tree-loop-distribute-patterns")
endif()

# Build-time stamp (kickos_build_time[]/kickos_build_commit[], read by the kmain banner).
# Regenerated on EVERY build: __DATE__/__TIME__ bakes per-TU and goes stale on incremental
# builds (cmake/build_stamp.cmake). The custom target has no OUTPUT, so it is always out of
# date.
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

# The switch-completion hook (kickos_trace_switch_done in ktrace.cc) runs in the ARM PendSV
# tail with the incoming thread's FP registers still live, so its emit path must not touch
# any FP register.
if(KICKOS_TELEMETRY_ON AND (KICKOS_ARCH STREQUAL "armv7m" OR KICKOS_ARCH STREQUAL "armv6m"))
  set_source_files_properties(ktrace/ktrace.cc PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
endif()

# console_tx_isr drains the ring from ISR context, so its emit path must take no
# FP register (no lazy FP stacking on the drain), same treatment as ktrace.
if(KICKOS_ARCH STREQUAL "armv7m" OR KICKOS_ARCH STREQUAL "armv6m")
  set_source_files_properties(init/console_tx.cc PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
endif()

# armv7m only: kickos_bench_switch_done runs in the PendSV tail with the incoming thread's
# lazily-stacked FP live, so its path must take no FP register. -mgeneral-regs-only is not
# valid on rxv3/rv32imac, and neither needs it: rxv3 calls it before the FP restore, and
# rv32imac has no hazard.
if(KICKOS_BENCH AND KICKOS_ARCH STREQUAL "armv7m")
  set_source_files_properties(bench/bench.cc PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
endif()
target_include_directories(kickos_kernel PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
# The kernel implements the userspace syscall ABI, so it reads that contract; the system
# headers (errno.h via sync.h, cap_index.h via cap.h) live in kickos_system.
target_include_directories(kickos_kernel PRIVATE
  "${PROJECT_SOURCE_DIR}/user/include"
  "${PROJECT_SOURCE_DIR}/system/include")
target_link_libraries(kickos_kernel PUBLIC kickos_arch_${KICKOS_ARCH} kickos_lib)

# Identity baked into the boot banner (kmain kbanner()).
# The CPU is the token of the resolved compiler CPU flag: -mcpu where the family has one, else
# -march, else RX's -misa, whose version is an RX ISA level and only ever an RX one.
set(_kickos_cpu "")
if(KICKOS_MCPU_FLAGS MATCHES "-mcpu=([^ ;]+)")
  set(_kickos_cpu "${CMAKE_MATCH_1}")
elseif(KICKOS_MCPU_FLAGS MATCHES "-march=([^ ;]+)")
  set(_kickos_cpu "${CMAKE_MATCH_1}")
elseif(KICKOS_MCPU_FLAGS MATCHES "-misa=([^ ;]+)")
  set(_kickos_cpu "rx${CMAKE_MATCH_1}")
elseif(KICKOS_CHIP)
  # No flag names the core on this family, so the chip states it: the compiler driver serves
  # more than one core and cannot answer which one a part has.
  include("${PROJECT_SOURCE_DIR}/arch/${KICKOS_ARCH_FAMILY}/chip/${KICKOS_CHIP}/cpu.cmake"
          OPTIONAL RESULT_VARIABLE _kickos_cpu_decl)
  set(_kickos_cpu "${KICKOS_CPU}")
  if(_kickos_cpu STREQUAL "")
    message(FATAL_ERROR
      "KickOS: board '${KICKOS_BOARD}' resolved no CPU name for the boot banner. No compiler "
      "flag carries one on ${KICKOS_ARCH_FAMILY}, so arch/${KICKOS_ARCH_FAMILY}/chip/"
      "${KICKOS_CHIP}/cpu.cmake must set KICKOS_CPU (declaration read: ${_kickos_cpu_decl})")
  endif()
else()
  # No chip and no CPU flag: the host build, where the platform's own declaration is the only
  # authority there is.
  set(_kickos_cpu "${CMAKE_SYSTEM_PROCESSOR}")
endif()
target_compile_definitions(kickos_kernel PRIVATE
  KICKOS_VERSION="${KICKOS_VERSION}"
  KICKOS_BOARD_NAME="${KICKOS_BOARD}"
  KICKOS_ARCH_NAME="${KICKOS_ARCH}")
if(NOT _kickos_cpu STREQUAL "")
  target_compile_definitions(kickos_kernel PRIVATE KICKOS_CPU_NAME="${_kickos_cpu}")
endif()

install(TARGETS kickos_kernel EXPORT KickOSTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")

if(CONFIG_SCHED_PERIODIC_TICK)
  target_compile_definitions(kickos_kernel PUBLIC KICKOS_SCHED_PERIODIC_TICK=1)
endif()
