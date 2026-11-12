# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Storage + allocation-policy gate for kernel/include/kickos/cap.h. The header is standalone
# (stdint.h, config/system.h, sys/cap_index.h), so a host program uses the real cap_slot,
# CapChunkList and free-slot list.
#
# FOUR EXECUTABLES from one source. Each case names the widest table it wants, the chunk count it
# must land on, and a narrower table beside it: a regression that collapsed every width to one chunk
# count fails the geometry check instead of passing every half, and only a case where the two widths
# differ exercises a partial take at all. Every width is whole chunks, as the kernel seats them.
#
#   flat       8: one chunk, every table alike
#   segmented 16: two chunks beside one
#   deep      24: three chunks, so the directory index passes 1
#   deeptail  24: a 3-chunk directory against a 1-chunk run, so a partial take leaves a TWO-entry
#                 tail to clear, which `segmented` (one entry) cannot catch
#
# All four binaries define the SAME gtest suite and case names, so TEST_PREFIX puts the
# geometry on each ctest entry (flat.CapTable.geometry); without it four different geometries
# would register under one name.
#
# CAP_REPLY's packing reads no geometry, so `flat` alone carries it.
foreach(_case flat:8:1:8 segmented:16:2:8 deep:24:3:24 deeptail:24:3:8)
  string(REPLACE ":" ";" _parts "${_case}")
  list(GET _parts 0 _name)
  list(GET _parts 1 _width)
  list(GET _parts 2 _chunks)
  list(GET _parts 3 _child)
  set(_sources captable_policy.cc)
  if(_name STREQUAL "flat")
    list(APPEND _sources capreply_packing.cc)
  endif()
  kickos_add_unit_test(NAME captable_${_name} SOURCES ${_sources}
    TEST_PREFIX "${_name}."
    INCLUDES "${PROJECT_SOURCE_DIR}/kernel/include" "${PROJECT_SOURCE_DIR}/system/include"
             "${PROJECT_SOURCE_DIR}/user/include"
    DEFINITIONS CAPTABLE_GATE_WIDTH=${_width} CAPTABLE_GATE_CHUNKS=${_chunks}
                CAPTABLE_GATE_CHILD=${_child})
endforeach()

# A capability of each kind kept across a generation wrap of its object's slot, over the real
# capability, domain, task and system-call layers at the translating posture.
kickos_add_unit_test(NAME captable_stale SOURCES stale_kinds.cc
  DEFINITIONS SYS_IMAGE_BYTES=${KICKOS_SYSDISPATCH_IMAGE_BYTES}
  LIBRARIES kickos_kseam_sysdispatch)
kickos_sysdispatch_link(captable_stale g_sys_image 0)

# A task's runs at mixed widths over a slab of exactly the init's width and the task's CAP_SLOTS:
# the charge and the return at the end of the sweep, over the real exit path.
kickos_add_kseam(kickos_kseam_captable
  POSTURE KICKOS_SEAM_ROOT_WIDTH=8 KICKOS_SEAM_BUDGET_CAP_SLOTS=32 KICKOS_SEAM_CAP_SLOTS=40
          KICKOS_SEAM_CAP_DIR=4)
kickos_add_unit_test(NAME captable_run_return SOURCES run_return.cc
  LIBRARIES kickos_kseam_captable)
