# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Storage + allocation-policy gate for kernel/include/kickos/cap.h. The header is standalone
# (stdint.h, config/system.h, sys/cap_index.h), so a host program uses the real cap_slot,
# CapChunkList and free-slot list.
#
# TWO EXECUTABLES from one source. The chunk geometry is chosen by an #if on the summed table
# width, so one binary can only compile one of the two paths; each target names the width it
# wants and the chunk count it must land on, so a regression that collapsed the #if to one
# path fails the geometry check instead of passing both halves.
#
#   flat       7: the fleet's narrowest table (nrf51, stm32f103, stm32f302, bluepill-c8),
#                 which must stay one exact-width chunk with no directory and no shift
#   segmented 10: what the sim, qemu and qemu-riscv sum to
#   deep      20: no image in the tree exceeds 16 slots, so without this the directory index
#                 is never above 1 anywhere, and the mechanism is cut for thousands of chunks
foreach(_case flat:7:1 segmented:10:2 deep:20:3)
  string(REPLACE ":" ";" _parts "${_case}")
  list(GET _parts 0 _name)
  list(GET _parts 1 _width)
  list(GET _parts 2 _chunks)
  add_executable(captable_${_name} captable_policy.cc)
  target_include_directories(captable_${_name} PRIVATE
    "${PROJECT_SOURCE_DIR}/kernel/include" "${PROJECT_SOURCE_DIR}/system/include")
  target_compile_definitions(captable_${_name} PRIVATE
    CAPTABLE_GATE_WIDTH=${_width} CAPTABLE_GATE_CHUNKS=${_chunks})
  target_compile_options(captable_${_name} PRIVATE ${KICKOS_WARN_FLAGS})
  add_test(NAME captable_${_name} COMMAND "$<TARGET_FILE:captable_${_name}>")
  set_tests_properties(captable_${_name} PROPERTIES TIMEOUT 30)
endforeach()
