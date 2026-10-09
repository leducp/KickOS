# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# KICKOS_HAVE_ASPACE=1 is set here: no board the host arch answers for translates, so the preset
# never carries it.
set(_aspace "${PROJECT_SOURCE_DIR}/kernel/mem/aspace.cc"
            "${PROJECT_SOURCE_DIR}/kernel/mem/vrange.cc")
set_source_files_properties(${_aspace} DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
kickos_add_kseam(kickos_kseam_aspace
  EXTRA aspace_seam.cc ${_aspace}
  POSTURE KICKOS_NUM_CORES=2 KICKOS_KERNEL_CORES=2 KICKOS_MULTICORE_MODEL_SHARED=1
          KICKOS_HAVE_ASPACE=1)
kickos_add_unit_test(NAME leave_space SOURCES leave_space.cc
  LIBRARIES kickos_kseam_aspace)

# exit_current's reroot arm is guarded on KICKOS_KERNEL_CORES > 1, so the guard itself needs a
# translation unit where the macro is 1.
kickos_add_kseam(kickos_kseam_aspace_one
  EXTRA aspace_seam.cc ${_aspace}
  POSTURE KICKOS_NUM_CORES=1 KICKOS_KERNEL_CORES=1 KICKOS_MULTICORE_MODEL_SHARED=1
          KICKOS_HAVE_ASPACE=1)
kickos_add_unit_test(NAME leave_space_one SOURCES leave_space_one.cc
  LIBRARIES kickos_kseam_aspace_one)
target_link_options(leave_space PRIVATE -Wl,--gc-sections)
target_link_options(leave_space_one PRIVATE -Wl,--gc-sections)

# Outside the K-seam: the fixture defines domain_for itself, so the real domain.cc cannot link
# beside it.
include("${PROJECT_SOURCE_DIR}/tests/unit/common/host_image.cmake")
set(_image_bytes 8192)
kickos_add_unit_test(NAME domain_unwind
  SOURCES domain_unwind.cc "${PROJECT_SOURCE_DIR}/tests/unit/common/kpanic_seam.cc"
          "${PROJECT_SOURCE_DIR}/tests/unit/common/host_aspace.cc"
          "${PROJECT_SOURCE_DIR}/tests/unit/common/host_frame_pool.cc"
          "${PROJECT_SOURCE_DIR}/kernel/domain/domain.cc" ${_aspace}
          "${PROJECT_SOURCE_DIR}/kernel/init/instance.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/tests/unit/common"
  DEFINITIONS DOMAIN_UNWIND_IMAGE_BYTES=${_image_bytes}
  POSTURE KICKOS_HAVE_ASPACE=1 KICKOS_ARCH_IRQ_WINDOW=1 KICKOS_ARCH_ALIAS_DCACHE=0)
target_compile_options(domain_unwind PRIVATE -ffunction-sections -fdata-sections)
target_link_options(domain_unwind PRIVATE -Wl,--gc-sections)
kickos_host_image(domain_unwind TEXT g_image_text ${_image_bytes} DATA g_image_data ${_image_bytes})
