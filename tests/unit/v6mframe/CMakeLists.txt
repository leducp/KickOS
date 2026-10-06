# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Which stacked frames the v6-M fault reporter reads, over the backend's own predicate and the
# kernel's real kickos_fault_frame_trusted. No armv6m emulator enforces memory, so no image
# witnesses it.
set(_v6f_header "${PROJECT_SOURCE_DIR}/arch/arm/armv6m/include/kickos/arch/armv6m_fault_frame.h")
if(NOT EXISTS "${_v6f_header}")
  message(FATAL_ERROR "KickOS: ${_v6f_header} is gone, so v6m_fault_frame would witness nothing.")
endif()
kickos_add_unit_test(NAME v6m_fault_frame SOURCES frame.cc
  DEFINITIONS "KICKOS_V6M_FAULT_FRAME_H=\"${_v6f_header}\""
  LIBRARIES kickos_kseam)
