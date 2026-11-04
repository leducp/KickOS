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
                "${_k}/init/terminal.cc"
                "${_k}/syscall/syscall_mem.cc"
                "${_k}/mem/aspace.cc" "${_k}/mem/vrange.cc" "${_k}/mem/ustack.cc"
                "${_k}/domain/domain.cc" "${_k}/thread/thread.cc" "${_k}/thread/tls.cc"
                "${_k}/grant/grant.cc" "${_k}/syscall/word.cc" "${_k}/time/realtime.cc")
set(_sys_region "${_k}/syscall/syscall.cc" "${_k}/syscall/syscall_thread.cc"
                "${_k}/init/terminal.cc"
                "${_k}/syscall/syscall_mem.cc"
                "${_k}/mem/ramown.cc" "${_k}/thread/thread.cc" "${_k}/thread/tls.cc"
                "${_k}/grant/grant.cc" "${_k}/time/realtime.cc")
set_source_files_properties(${_sys_aspace} ${_sys_region} DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  PROPERTIES COMPILE_OPTIONS "-ffreestanding;-ffunction-sections;-fdata-sections")
kickos_add_kseam(kickos_kseam_sysdispatch
  EXTRA sys_seam.cc stub_seam.cc "${_common}/host_aspace.cc" "${_common}/host_frame_pool.cc"
        ${_sys_aspace}
  INCLUDES "${_k}/syscall" "${_common}" "${CMAKE_CURRENT_SOURCE_DIR}"
  POSTURE KICKOS_HAVE_ASPACE=1
          KICKOS_ARCH_ALIAS_DCACHE=1 KICKOS_ARCH_IRQ_WINDOW=1
          KSEAM_REAL_DOMAIN=1)
set(KICKOS_SYSDISPATCH_IMAGE_BYTES 8192 CACHE INTERNAL "")
set(_image_bytes ${KICKOS_SYSDISPATCH_IMAGE_BYTES})
# The link of a gate over kickos_kseam_sysdispatch: its code span starts at `text`, `text_bytes`
# long, and g_sys_image is its data. Also called from tests/unit/captable.
function(kickos_sysdispatch_link gate text text_bytes)
  target_link_options(${gate} PRIVATE -Wl,--gc-sections)
  kickos_host_image(${gate} TEXT ${text} ${text_bytes} DATA g_sys_image ${KICKOS_SYSDISPATCH_IMAGE_BYTES})
  # The static regions a region backend names, which domain.cc's arch default still links: empty.
  target_link_options(${gate} PRIVATE
    -Wl,--defsym=__kickos_code_start=g_sys_image -Wl,--defsym=__kickos_code_end=g_sys_image
    -Wl,--defsym=__kickos_appdata_start=g_sys_image
    -Wl,--defsym=__kickos_appdata_end=g_sys_image)
endfunction()
kickos_add_unit_test(NAME sys_presync SOURCES presync_loop.cc
  DEFINITIONS SYS_IMAGE_BYTES=${_image_bytes}
  LIBRARIES kickos_kseam_sysdispatch)
kickos_add_unit_test(NAME sys_frame_space SOURCES frame_space.cc
  DEFINITIONS SYS_IMAGE_BYTES=${_image_bytes}
  LIBRARIES kickos_kseam_sysdispatch)
kickos_add_unit_test(NAME sys_word_ref SOURCES word_ref.cc
  DEFINITIONS SYS_IMAGE_BYTES=${_image_bytes}
  LIBRARIES kickos_kseam_sysdispatch)
# The PAGES budget of every frame a space's task causes to exist, its tables included.
kickos_add_unit_test(NAME sys_page_budget SOURCES page_budget.cc
  DEFINITIONS SYS_IMAGE_BYTES=${_image_bytes}
  LIBRARIES kickos_kseam_sysdispatch)
kickos_sysdispatch_link(sys_presync g_sys_image 0)
kickos_sysdispatch_link(sys_frame_space g_sys_image 0)
kickos_sysdispatch_link(sys_word_ref g_word_text 4096)
kickos_sysdispatch_link(sys_page_budget g_sys_image 0)
# Two admissions of one budget, one parked at its first window outside the lock.
kickos_add_unit_test(NAME sys_budget_race SOURCES budget_race.cc
  DEFINITIONS SYS_IMAGE_BYTES=${_image_bytes}
  LIBRARIES kickos_kseam_sysdispatch)
kickos_sysdispatch_link(sys_budget_race g_sys_image 0)

kickos_add_kseam(kickos_kseam_sysdispatch_region
  EXTRA region_seam.cc stub_seam.cc ${_sys_region}
  INCLUDES "${_k}/syscall" "${CMAKE_CURRENT_SOURCE_DIR}")
kickos_add_unit_test(NAME sys_ram_clear SOURCES ram_clear.cc
  LIBRARIES kickos_kseam_sysdispatch_region)
kickos_add_unit_test(NAME spawn_unwind SOURCES spawn_unwind.cc task_stop.cc spawn_badge.cc program_create.cc
  driver_create.cc
  LIBRARIES kickos_kseam_sysdispatch_region)
# The fault dead-end of an emulated chip, whose arch_shutdown the seam answers.
kickos_add_unit_test(NAME sys_shutdown
  SOURCES shutdown.cc "${PROJECT_SOURCE_DIR}/arch/common/kfault_terminate_shutdown.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/arch/common"
  LIBRARIES kickos_kseam_sysdispatch_region)
# The wall clock's set, gated on AUTH_REALTIME alone, and its read over the monotonic clock.
kickos_add_unit_test(NAME sys_realtime SOURCES wall_clock.cc
  LIBRARIES kickos_kseam_sysdispatch_region)
# A join of a thread that has already exited, by a sibling and by another task's thread.
kickos_add_unit_test(NAME sys_thread_join SOURCES thread_join.cc
  LIBRARIES kickos_kseam_sysdispatch_region)
# The peripheral disable's possession gate and the seam's answer, over the fixture's recorder.
kickos_add_unit_test(NAME sys_periph_disable SOURCES periph_disable.cc
  LIBRARIES kickos_kseam_sysdispatch_region)
# The console write's wait flag, from the call's own argument.
kickos_add_unit_test(NAME sys_kconsole_flags SOURCES kconsole_flags.cc
  LIBRARIES kickos_kseam_sysdispatch_region)
# The fused reply's null options, answered by the reply alone before any receive validation.
kickos_add_unit_test(NAME sys_reply_alone SOURCES reply_alone.cc
  LIBRARIES kickos_kseam_sysdispatch_region)
# The witness call's authority gate, with the witness kernel's dispatch arm and without it.
kickos_add_kseam(kickos_kseam_sysdispatch_region_witness
  EXTRA region_seam.cc stub_seam.cc witness_seam.cc ${_sys_region}
        "${_k}/syscall/syscall_witness.cc"
  INCLUDES "${_k}/syscall" "${CMAKE_CURRENT_SOURCE_DIR}"
  POSTURE KICKOS_WITNESS_UACCESS=1)
kickos_add_unit_test(NAME sys_witness_on SOURCES witness_gate.cc
  LIBRARIES kickos_kseam_sysdispatch_region_witness)
target_compile_definitions(sys_witness_on PRIVATE KICKOS_WITNESS_UACCESS=1)
kickos_add_unit_test(NAME sys_witness_off SOURCES witness_gate.cc
  LIBRARIES kickos_kseam_sysdispatch_region)
foreach(_gate IN ITEMS sys_ram_clear spawn_unwind sys_shutdown sys_realtime sys_thread_join sys_periph_disable sys_kconsole_flags sys_witness_on
                       sys_witness_off)
  target_link_options(${_gate} PRIVATE -Wl,--gc-sections)
endforeach()
