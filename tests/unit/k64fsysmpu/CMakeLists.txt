# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Which latched SYSMPU error the K64F hands a thread's fault record, over the chip's own header
# and planted error registers. No emulator models the SYSMPU, so no image witnesses it.
set(_k64e_header "${PROJECT_SOURCE_DIR}/arch/arm/chip/mk64f/sysmpu_error.h")
if(NOT EXISTS "${_k64e_header}")
  message(FATAL_ERROR
    "KickOS: ${_k64e_header} is gone, so k64f_sysmpu_error would witness nothing.")
endif()
kickos_add_unit_test(NAME k64f_sysmpu_error SOURCES sysmpu_error.cc
  DEFINITIONS "KICKOS_K64F_SYSMPU_ERROR_H=\"${_k64e_header}\"")

# The rights a thread's SYSMPU descriptor grants, over the chip's own header.
set(_k64r_header "${PROJECT_SOURCE_DIR}/arch/arm/chip/mk64f/sysmpu_rights.h")
if(NOT EXISTS "${_k64r_header}")
  message(FATAL_ERROR
    "KickOS: ${_k64r_header} is gone, so k64f_sysmpu_rights would witness nothing.")
endif()
kickos_add_unit_test(NAME k64f_sysmpu_rights SOURCES sysmpu_rights.cc
  DEFINITIONS "KICKOS_K64F_SYSMPU_RIGHTS_H=\"${_k64r_header}\""
  KERNEL)
