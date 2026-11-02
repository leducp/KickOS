# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_executable(stress main.cc)
# The board's default composition. Above one kernel core main's THREADS budget seats two workers
# per core, so more threads stay runnable than there are cores, up to the whole soak: its entry,
# three ping-pong pairs and six sleepers.
set(_stress_threads ${KICKOS_BUDGET_THREADS})
if(KICKOS_KERNEL_CORES GREATER 1)
  math(EXPR _stress_threads "2 * ${KICKOS_KERNEL_CORES} + 1")
  if(_stress_threads GREATER 13)
    set(_stress_threads 13)
  endif()
endif()
file(READ "${PROJECT_SOURCE_DIR}/boards/${KICKOS_BOARD}/composition.yaml" _stress_text)
string(FIND "${_stress_text}" "    program: main\n" _stress_at)
if(_stress_at EQUAL -1)
  message(FATAL_ERROR "stress: boards/${KICKOS_BOARD}/composition.yaml declares no program main")
endif()
string(REPLACE "    program: main\n" "    program: main\n    budgets: { threads: ${_stress_threads} }\n"
               _stress_text "${_stress_text}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/system.yaml" CONTENT "${_stress_text}" @ONLY)
kickos_app_system(stress COMPOSITION "${CMAKE_CURRENT_BINARY_DIR}/system.yaml")
target_compile_definitions(stress PRIVATE STRESS_THREADS=${_stress_threads})
kickos_emit_image(stress)

# Endurance soak: repeat the conservation round forever (heartbeat per round, freeze
# on the first FAIL). OFF keeps the single-round CTest-gate behaviour byte-identical.
option(KICKOS_STRESS_FOREVER "stress: loop the conservation round forever (soak)" OFF)
if(KICKOS_STRESS_FOREVER)
  target_compile_definitions(stress PRIVATE KICKOS_STRESS_FOREVER)
endif()
