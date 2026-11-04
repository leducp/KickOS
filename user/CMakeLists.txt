# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# uart_service_dev.cc stays its own TU: merged into uart_service.cc it would make the ring side
# undefined on a board with no UART class backend.
# grant_lookup.cc stays its own TU: it names kickos_table, which only a system target defines.
# image_start.cc stays its own TU: the app's image links it, and root's entry names the init's.
set(KICKOS_USER_SOURCES src/syscall_stubs.cc src/mutex.cc src/sem.cc src/cond.cc src/print.cc
                        src/init_args.cc src/root_entry.cc src/image_start.cc src/console_ring.cc
                        src/driver_service.cc
                        src/uart_service.cc src/uart_service_dev.cc src/usb_cdc_service.cc
                        src/grant_lookup.cc src/init_status.cc src/task_enter.cc
                        src/program_entry.cc
                        src/console_flush.cc)

# newlib_sbrk.cc stays its own TU: merging it into newlib_stubs.cc disables the heapless-board
# link error (-Wl,-u,_exit force-links that object everywhere). sim_exit.cc and newlib_exit.cc are
# separate so the archive pulls them only for an image that calls exit().
if(KICKOS_ARCH STREQUAL "sim")
  list(APPEND KICKOS_USER_SOURCES src/sim_exit.cc src/arch/sim/user_cas.cc)
else()
  list(APPEND KICKOS_USER_SOURCES src/arch/${KICKOS_ARCH}/user_cas.S)
  # The syscall trap's user side; on lx6 the arch's entry is a call, and the arch defines it.
  if(NOT KICKOS_ARCH STREQUAL "lx6")
    list(APPEND KICKOS_USER_SOURCES src/arch/${KICKOS_ARCH}/syscall.S)
  endif()
  # The thread-local access helpers a compiler calls in the thread's own image.
  if(KICKOS_ARCH MATCHES "^armv[67]m$")
    list(APPEND KICKOS_USER_SOURCES src/arch/arm/read_tp.S)
  elseif(KICKOS_ARCH STREQUAL "rxv3")
    list(APPEND KICKOS_USER_SOURCES src/arch/rxv3/emutls.cc)
  endif()
  # The atomics calls a compiler makes where user mode has no compare-and-swap instruction.
  if(KICKOS_ARCH MATCHES "^(armv6m|rxv3)$")
    list(APPEND KICKOS_USER_SOURCES src/user_atomic.cc)
  endif()
  if(KICKOS_ARCH STREQUAL "x86_64")
    list(APPEND KICKOS_USER_SOURCES src/arch/x86_64/rdseed.S)
  endif()
  list(APPEND KICKOS_USER_SOURCES src/newlib_stubs.cc src/newlib_sbrk.cc src/newlib_fcntl.cc
                                  src/newlib_posix.cc src/newlib_exit.cc)
  list(APPEND KICKOS_USER_SOURCES src/newlib_reent.cc src/newlib_lock.cc src/toolchain_layout.cc
                                  src/gthr.cc src/gthr_once.cc src/thread_atexit.cc)
  # A once's function may throw through it.
  set_source_files_properties(src/gthr_once.cc PROPERTIES COMPILE_OPTIONS -fexceptions)
endif()

# One archive per newlib profile the toolchain carries: the libc bottom edge sizes each thread's
# libc state by its profile's struct _reent. kickos_user names the one the consuming image's
# profile selects (KICKOS_LIBC_IS_NANO).
set(_kickos_user_variants "")
foreach(_p IN LISTS KICKOS_LIBC_PROFILES)
  set(_t kickos_user_${_p})
  add_library(${_t} STATIC ${KICKOS_USER_SOURCES} src/banner_identity.cc "${KICKOS_IDENTITY_STAMP}")
  set_target_properties(${_t} PROPERTIES KICKOS_LIBC ${_p})
  add_dependencies(${_t} kickos_build_stamp_gen)
  # kickos_apply_freestanding also applies -msmall-data-limit=0 under RISC-V PMP, so this
  # KickOS-owned lib emits no gp small-data; the gp window is the app's plus the C++ runtime's.
  kickos_apply_freestanding(${_t})
  target_include_directories(${_t} PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  # kernel/include is here for config/system.h alone.
  target_include_directories(${_t} PRIVATE
    "${PROJECT_SOURCE_DIR}/system/include"
    "${PROJECT_SOURCE_DIR}/kernel/include")
  if(_p STREQUAL "nano")
    target_compile_options(${_t} PRIVATE --specs=nano.specs)
  endif()
  target_compile_definitions(${_t} PRIVATE KICKOS_REENT_CARVE=${KICKOS_REENT_CARVE_${_p}})
  target_link_libraries(${_t} PUBLIC kickos_arch_${KICKOS_ARCH} kickos_lib)
  kickos_export_targets(${_t})
  kickos_export_name(${_t} _name)
  list(APPEND _kickos_user_variants ${_name})
endforeach()
set_source_files_properties("${KICKOS_IDENTITY_STAMP}" PROPERTIES GENERATED TRUE)
set_source_files_properties(src/banner_identity.cc PROPERTIES
  COMPILE_DEFINITIONS "${KICKOS_BANNER_IDENTITY}")
add_library(kickos_user INTERFACE)
target_include_directories(kickos_user INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
list(GET _kickos_user_variants 0 _kickos_user_selected)
if("nano" IN_LIST KICKOS_LIBC_PROFILES)
  set(_kickos_user_selected "$<IF:${KICKOS_LIBC_IS_NANO},KickOS::kickos_user_nano,KickOS::kickos_user_full>")
endif()
target_link_libraries(kickos_user INTERFACE "${_kickos_user_selected}")
# libc.a carries a no-op default of every lock hook, which the archive order picks unless
# newlib_lock.cc's object is in the link before libc is searched. Some carry a sysconf too, which
# the wrap keeps out of every link. exit() runs its caller's thread_local destructors first.
if(NOT KICKOS_ARCH STREQUAL "sim")
  target_link_options(kickos_user INTERFACE
    "LINKER:-u,${KICKOS_C_SYMBOL_PREFIX}__retarget_lock_acquire_recursive"
    "LINKER:--wrap=sysconf" "LINKER:--wrap=exit")
endif()
kickos_export_targets(kickos_user)

add_subdirectory(lib)
