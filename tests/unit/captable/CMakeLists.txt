# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Storage + allocation-policy gate for kernel/include/kickos/cap.h. The header is standalone
# (stdint.h, config/system.h, sys/cap_index.h), so a host program uses the real cap_slot,
# CapChunkList and free-slot list.
#
# FOUR EXECUTABLES from one source. The chunk geometry is chosen by an #if on the summed table
# width, so one binary can only compile one of the paths; each case names the width it
# wants and the chunk count it must land on, so a regression that collapsed the #if to one
# path fails the geometry check instead of passing every half.
#
# Each case also names the CHILD width: only a case where the two differ exercises a partial
# take at all.
#
#   flat       7: the fleet's narrowest table (nrf51, stm32f103, stm32f302, bluepill-c8),
#                 which must stay one exact-width chunk with no directory and no shift
#   segmented 10: what the sim, qemu and qemu-riscv sum to
#   deep      20: no image in the tree exceeds 16 slots, so without this the directory index
#                 is never above 1 anywhere, and the mechanism is cut for thousands of chunks
#   deeptail  20: a 3-chunk root directory against a 1-chunk child, so a partial take leaves
#                 a TWO-entry tail to clear, which `segmented` (one entry) cannot catch
#
# All four binaries define the SAME gtest suite and case names, so TEST_PREFIX puts the
# geometry on each ctest entry (flat.CapTable.geometry); without it four different geometries
# would register under one name. kickos_add_unit_test does not expose TEST_PREFIX, so the
# executable and the discovery are spelled out here with the PROPERTIES
# kickos_discover_unit_tests uses.
foreach(_case flat:7:1:7 segmented:10:2:7 deep:20:3:20 deeptail:20:3:7)
  string(REPLACE ":" ";" _parts "${_case}")
  list(GET _parts 0 _name)
  list(GET _parts 1 _width)
  list(GET _parts 2 _chunks)
  list(GET _parts 3 _child)
  add_executable(captable_${_name} captable_policy.cc)
  target_link_libraries(captable_${_name} PRIVATE GTest::gtest_main)
  target_compile_features(captable_${_name} PRIVATE ${KICKOS_CXX_STANDARD})
  target_include_directories(captable_${_name} PRIVATE
    "${PROJECT_SOURCE_DIR}/kernel/include" "${PROJECT_SOURCE_DIR}/system/include")
  target_compile_definitions(captable_${_name} PRIVATE
    CAPTABLE_GATE_WIDTH=${_width} CAPTABLE_GATE_CHUNKS=${_chunks}
    CAPTABLE_GATE_CHILD=${_child})
  target_compile_options(captable_${_name} PRIVATE ${KICKOS_WARN_FLAGS})
  gtest_discover_tests(captable_${_name}
    TEST_PREFIX ${_name}.
    PROPERTIES TIMEOUT 30 LABELS host FIXTURES_REQUIRED kickos_build
    DISCOVERY_TIMEOUT 60)
endforeach()
