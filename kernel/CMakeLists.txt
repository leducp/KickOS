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
  init/terminal.cc
  init/fault.cc
  init/led.cc
  init/instance.cc
  init/kmain.cc
  mem/aspace.cc
  mem/frame.cc
  mem/frame_pool.cc
  mem/ramown.cc
  mem/ustack.cc
  mem/vrange.cc
  sync/sync.cc
  sync/klock.cc
  sync/corestart.cc
  amp/ampwindow.cc
  amp/ampmap.cc
  amp/ampdiag.cc
  amp/ampshare.cc
  sync/smptrace.cc
  time/time.cc
  time/clock_select.cc
  syscall/syscall.cc
  syscall/syscall_mem.cc
  syscall/syscall_obj.cc
  syscall/syscall_ipc.cc
  syscall/syscall_ipc_fast.cc
  syscall/syscall_thread.cc
  syscall/cap.cc
  irq/irq.cc
  notify/notify.cc
  irq/irq_route.cc)

# Microbenchmark accumulator, paired with the cycle-counter bracket in switch.S. On M0 and
# the sim it links but the cycle path is inert, switch.S bracketing nothing there.
if(KICKOS_BENCH)
  list(APPEND KICKOS_KERNEL_SOURCES bench/bench.cc)
endif()

if(KICKOS_WITNESS_UACCESS)
  list(APPEND KICKOS_KERNEL_SOURCES syscall/syscall_witness.cc)
endif()

# The kernel's own copies of the runtime, under the private names cmake/kernel_runtime.syms lists.
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

# Build stamp (cmake/build_stamp.cmake): kickos_build_time[] and kickos_build_commit[] for the
# kmain banner, and kickos_identity_build_time[] and kickos_identity_commit[], the user side's copy
# of both, which kickos_user links. The custom target has no OUTPUT, so it runs on every build.
set(_kos_build_stamp "${CMAKE_BINARY_DIR}/kickos_build_stamp.cc")
set(_kos_identity_stamp "${CMAKE_BINARY_DIR}/kickos_identity_stamp.cc")
set(KICKOS_IDENTITY_STAMP "${_kos_identity_stamp}" PARENT_SCOPE)
add_custom_target(kickos_build_stamp_gen
  BYPRODUCTS "${_kos_build_stamp}" "${_kos_identity_stamp}"
  COMMAND "${CMAKE_COMMAND}"
    "-DOUT=${_kos_build_stamp}" "-DIDENTITY=${_kos_identity_stamp}"
    "-DSRC=${CMAKE_SOURCE_DIR}"
    -P "${CMAKE_SOURCE_DIR}/cmake/build_stamp.cmake"
  COMMENT "kickos: stamping build time"
  VERBATIM)
set_source_files_properties("${_kos_build_stamp}" PROPERTIES GENERATED TRUE)
target_sources(kickos_kernel PRIVATE "${_kos_build_stamp}")
add_dependencies(kickos_kernel kickos_build_stamp_gen)

# The PendSV tail (the trace and bench switch hooks) and the console drain ISR run with an FP
# context live, so their paths take no FP register.
if(KICKOS_ARCH STREQUAL "armv7m" OR KICKOS_ARCH STREQUAL "armv6m")
  set(_kickos_no_fp init/console_tx.cc)
  if(KICKOS_TELEMETRY_ON)
    list(APPEND _kickos_no_fp ktrace/ktrace.cc)
  endif()
  if(KICKOS_BENCH AND KICKOS_ARCH STREQUAL "armv7m")
    list(APPEND _kickos_no_fp bench/bench.cc)
  endif()
  set_source_files_properties(${_kickos_no_fp} PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
endif()
target_include_directories(kickos_kernel PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
# The kernel implements the userspace syscall ABI, so it reads that contract; the system
# headers (errno.h via sync.h, cap_index.h via cap.h) live in kickos_system.
target_include_directories(kickos_kernel PRIVATE
  "${PROJECT_SOURCE_DIR}/user/include"
  "${PROJECT_SOURCE_DIR}/system/include"
  # amp_partition.ld.h: the partition geometry the user share is derived from.
  "${PROJECT_SOURCE_DIR}/arch/common")
target_link_libraries(kickos_kernel PUBLIC kickos_arch_${KICKOS_ARCH} kickos_lib)

# The CPU on the boot banner (kmain kbanner()): the core the compiler flag names, the RX ISA level
# -misa names, the chip file's `cpu` where no flag selects one, else the host's.
if(KICKOS_MCPU_FLAGS MATCHES "-m(cpu|arch)=([^ ;]+)")
  set(_kickos_cpu "${CMAKE_MATCH_2}")
elseif(KICKOS_MCPU_FLAGS MATCHES "-misa=([^ ;]+)")
  set(_kickos_cpu "rx${CMAKE_MATCH_1}")
elseif(KICKOS_CHIP_CPU)
  set(_kickos_cpu "${KICKOS_CHIP_CPU}")
elseif(KICKOS_ARCH STREQUAL "sim")
  set(_kickos_cpu "${CMAKE_SYSTEM_PROCESSOR}")
else()
  message(FATAL_ERROR
    "KickOS: board '${KICKOS_BOARD}' resolved no CPU name for the boot banner. No compiler "
    "flag carries one on ${KICKOS_ARCH_FAMILY}, so platform/${KICKOS_CHIP}/chip.yaml must "
    "state `cpu`.")
endif()
target_compile_definitions(kickos_kernel PRIVATE ${KICKOS_BANNER_IDENTITY}
  KICKOS_ARCH_NAME="${KICKOS_ARCH}" KICKOS_CPU_NAME="${_kickos_cpu}")

set_target_properties(kickos_kernel PROPERTIES EXPORT_NAME kernel_archive)
kickos_export_targets(kickos_kernel)

# No preset turns KICKOS_SMP_TRACE on, so its arm is compiled here once more, under the kernel's
# own flags, into objects nothing links: a renamed hook or a moved record field fails the build.
# -U first: the knob's =0 is a directory definition, and a second -D would be a redefinition.
if(NOT KICKOS_SMP_TRACE)
  add_library(kickos_smp_trace_arm OBJECT
              sync/smptrace.cc sync/sync.cc sched/sched.cc sync/smptrace_arm.cc)
  kickos_apply_freestanding(kickos_smp_trace_arm)
  foreach(_prop IN ITEMS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS)
    set_property(TARGET kickos_smp_trace_arm APPEND PROPERTY ${_prop}
                 "$<TARGET_PROPERTY:kickos_kernel,${_prop}>")
  endforeach()
  target_compile_options(kickos_smp_trace_arm PRIVATE -UKICKOS_SMP_TRACE -DKICKOS_SMP_TRACE=1)
endif()
