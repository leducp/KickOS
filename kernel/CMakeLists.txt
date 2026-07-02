# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

set(KICKOS_KERNEL_SOURCES
  sched/sched.cc
  thread/thread.cc
  init/console.cc
  init/kmain.cc)

# Optional components are appended as they land (kept bisectable).
foreach(_f sync/sync.cc time/time.cc syscall/syscall.cc irq/irq.cc)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_f}")
    list(APPEND KICKOS_KERNEL_SOURCES "${_f}")
  endif()
endforeach()

add_library(kickos_kernel STATIC ${KICKOS_KERNEL_SOURCES})
kickos_apply_freestanding(kickos_kernel)
target_include_directories(kickos_kernel PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:include>")
# The kernel implements the userspace syscall ABI, so it reads that contract.
target_include_directories(kickos_kernel PRIVATE
  "${CMAKE_SOURCE_DIR}/user/include")
target_link_libraries(kickos_kernel PUBLIC kickos_arch_${KICKOS_ARCH} kickos_lib)
install(TARGETS kickos_kernel EXPORT KickOSTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")

if(CONFIG_SCHED_PERIODIC_TICK)
  target_compile_definitions(kickos_kernel PUBLIC KICKOS_SCHED_PERIODIC_TICK=1)
endif()
