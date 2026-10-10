# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The system-call dispatch over the REAL kernel/syscall/syscall.cc and syscall_thread.cc on the
# K-seam, twice: at the translating posture, with the real address-space, domain and range
# layers under it over the common page-table editor and frame pool, sys_seam.cc answering the
# rest of the arch boundary; and at the preset's region posture, with region_seam.cc answering
# the arena. Both are compiled as the production kernel is: no case may reach a self-test
# symbol. Linked with --gc-sections, so only what a call reaches needs a seam; stub_seam.cc
# answers the rest of the dispatch.
include("${PROJECT_SOURCE_DIR}/tests/unit/common/host_image.cmake")
set(_k "${PROJECT_SOURCE_DIR}/kernel")
set(_common "${PROJECT_SOURCE_DIR}/tests/unit/common")
set(_sys_aspace "${_k}/syscall/syscall.cc" "${_k}/syscall/syscall_thread.cc"
                "${_k}/syscall/syscall_mem.cc" "${_k}/syscall/syscall_obj.cc"
                "${_k}/mem/aspace.cc" "${_k}/mem/vrange.cc" "${_k}/mem/ustack.cc"
                "${_k}/domain/domain.cc" "${_k}/thread/thread.cc" "${_k}/thread/tls.cc"
                "${_k}/grant/grant.cc")
set(_sys_region "${_k}/syscall/syscall.cc" "${_k}/syscall/syscall_thread.cc"
                "${_k}/syscall/syscall_mem.cc" "${_k}/syscall/syscall_obj.cc"
                "${_k}/mem/ramown.cc" "${_k}/thread/thread.cc" "${_k}/thread/tls.cc"
                "${_k}/grant/grant.cc")
set_source_files_properties(${_sys_aspace} ${_sys_region} DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  PROPERTIES COMPILE_OPTIONS "-ffreestanding;-ffunction-sections;-fdata-sections")
kickos_add_kseam(kickos_kseam_sysdispatch
  EXTRA sys_seam.cc stub_seam.cc "${_common}/host_aspace.cc" "${_common}/host_frame_pool.cc"
        ${_sys_aspace}
  INCLUDES "${_k}/syscall" "${_common}" "${CMAKE_CURRENT_SOURCE_DIR}"
  POSTURE KICKOS_HAVE_ASPACE=1 KICKOS_ARCH_ALIAS_DCACHE=1 KICKOS_ARCH_IRQ_WINDOW=1
          KSEAM_REAL_DOMAIN=1 KICKOS_REBOOT=0)
set(_image_bytes 8192)
kickos_add_unit_test(NAME sys_presync SOURCES presync_loop.cc
  DEFINITIONS SYS_IMAGE_BYTES=${_image_bytes}
  LIBRARIES kickos_kseam_sysdispatch)
kickos_add_unit_test(NAME sys_frame_space SOURCES frame_space.cc
  DEFINITIONS SYS_IMAGE_BYTES=${_image_bytes}
  LIBRARIES kickos_kseam_sysdispatch)
foreach(_gate IN ITEMS sys_presync sys_frame_space)
  target_link_options(${_gate} PRIVATE -Wl,--gc-sections)
  kickos_host_image(${_gate} TEXT g_sys_image 0 DATA g_sys_image ${_image_bytes})
  # The static regions a region backend names, which domain.cc's arch default still links: empty.
  target_link_options(${_gate} PRIVATE
    -Wl,--defsym=__kickos_code_start=g_sys_image -Wl,--defsym=__kickos_code_end=g_sys_image
    -Wl,--defsym=__kickos_appdata_start=g_sys_image
    -Wl,--defsym=__kickos_appdata_end=g_sys_image)
endforeach()

kickos_add_kseam(kickos_kseam_sysdispatch_region
  EXTRA region_seam.cc stub_seam.cc ${_sys_region}
  INCLUDES "${_k}/syscall" "${CMAKE_CURRENT_SOURCE_DIR}"
  POSTURE KICKOS_REBOOT=0)
kickos_add_unit_test(NAME sys_ram_clear SOURCES ram_clear.cc
  LIBRARIES kickos_kseam_sysdispatch_region)
kickos_add_unit_test(NAME spawn_unwind SOURCES spawn_unwind.cc task_stop.cc
  LIBRARIES kickos_kseam_sysdispatch_region)
foreach(_gate IN ITEMS sys_ram_clear spawn_unwind)
  target_link_options(${_gate} PRIVATE -Wl,--gc-sections)
endforeach()
