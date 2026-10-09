# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Host arms for the record a system call keeps of the frames it synced ahead of the kernel lock
# (kernel/include/kickos/presync.h): what excuses the locked pass, what an edit drops, and which
# frames owe a sync at all.
kickos_add_unit_test(NAME presync
  SOURCES presync.cc
  INCLUDES $<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES>
  DEFINITIONS $<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>)

# The locked pass of a frame capability's map over the REAL kernel/mem/aspace.cc, at the
# translating posture whose kernel view is cacheable. Linked with --gc-sections: only the map's
# own callees need a seam.
kickos_add_unit_test(NAME presync_map
  SOURCES presync_map.cc "${PROJECT_SOURCE_DIR}/tests/unit/common/kpanic_seam.cc"
          "${PROJECT_SOURCE_DIR}/tests/unit/common/host_frame_pool.cc"
          "${PROJECT_SOURCE_DIR}/kernel/mem/aspace.cc"
          "${PROJECT_SOURCE_DIR}/kernel/mem/vrange.cc"
          "${PROJECT_SOURCE_DIR}/kernel/init/instance.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/tests/unit/common"
  POSTURE KICKOS_HAVE_ASPACE=1 KICKOS_ARCH_ALIAS_DCACHE=1 KICKOS_ARCH_IRQ_WINDOW=1)
target_compile_options(presync_map PRIVATE -ffunction-sections -fdata-sections)
target_link_options(presync_map PRIVATE -Wl,--gc-sections)
set_source_files_properties("${PROJECT_SOURCE_DIR}/kernel/mem/aspace.cc"
                            "${PROJECT_SOURCE_DIR}/kernel/mem/vrange.cc"
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  PROPERTIES COMPILE_OPTIONS "-ffreestanding;-fno-exceptions;-fno-rtti;-fno-threadsafe-statics")

# A run staged outside every range over the REAL kernel/mem/aspace.cc: taken only by a locked
# pass asking for exactly it, given back otherwise. The app's text and static data are host
# arrays the link places the image bounds around. Built at both translating postures: with a
# cacheable kernel view of a non-cacheable frame, and without one.
include("${PROJECT_SOURCE_DIR}/tests/unit/common/host_image.cmake")
set(_stage_data_bytes 8192)
set(_stage_text_bytes 4096)
foreach(_alias IN ITEMS 1 0)
  set(_stage_name presync_stage)
  set(_stage_prefix "")
  if(_alias EQUAL 0)
    set(_stage_name presync_stage_noalias)
    set(_stage_prefix noalias_)
  endif()
  kickos_add_unit_test(NAME ${_stage_name} TEST_PREFIX "${_stage_prefix}"
    SOURCES presync_stage.cc "${PROJECT_SOURCE_DIR}/tests/unit/common/kpanic_seam.cc"
            "${PROJECT_SOURCE_DIR}/tests/unit/common/host_frame_pool.cc"
            "${PROJECT_SOURCE_DIR}/kernel/mem/aspace.cc"
            "${PROJECT_SOURCE_DIR}/kernel/mem/vrange.cc"
            "${PROJECT_SOURCE_DIR}/kernel/init/instance.cc"
    INCLUDES "${PROJECT_SOURCE_DIR}/tests/unit/common"
    DEFINITIONS PRESYNC_STAGE_DATA_BYTES=${_stage_data_bytes}
                PRESYNC_STAGE_TEXT_BYTES=${_stage_text_bytes}
    POSTURE KICKOS_HAVE_ASPACE=1 KICKOS_ARCH_ALIAS_DCACHE=${_alias} KICKOS_ARCH_IRQ_WINDOW=1)
  target_compile_options(${_stage_name} PRIVATE -ffunction-sections -fdata-sections)
  target_link_options(${_stage_name} PRIVATE -Wl,--gc-sections)
  kickos_host_image(${_stage_name} TEXT g_stage_rom ${_stage_text_bytes}
    DATA g_stage_sram ${_stage_data_bytes})
endforeach()
