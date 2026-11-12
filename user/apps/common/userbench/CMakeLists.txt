# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The composition a board variant runs the bench as where it is not system.yaml, as
# <board>/<variant>=<composition>: small where the arena seats fewer of main's threads, wide
# where a kernel above two cores runs the workloads whole.
set(_userbench_cases
  microbit/base=system_small.yaml
  due/base=system_small.yaml
  esp32c6-wroom/amp2=system_small.yaml
  qemu-arm64/smp=system_wide.yaml
  qemu-arm64/smp12=system_wide.yaml
  qemu-arm64/smpiso=system_wide.yaml
  qemu-arm64/gicv3=system_wide.yaml
  qemu-arm64/a710=system_wide.yaml
  qemu-riscv64/smp=system_wide.yaml
  qemu-x86_64/smp4=system_wide.yaml
  qemu-x86_64/smp8=system_wide.yaml
  qemu-x86_64/smp12=system_wide.yaml)
set(_userbench_case system.yaml)
foreach(_row IN LISTS _userbench_cases)
  string(REPLACE "=" ";" _row "${_row}")
  list(GET _row 0 _variant)
  if(_variant STREQUAL "${KICKOS_BOARD}/${KICKOS_CONFIG_VARIANT}")
    list(GET _row 1 _userbench_case)
  endif()
endforeach()
# The walks seat what main's THREADS budget leaves, as its composition states it.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${_userbench_case}" _userbench_text)
if(NOT _userbench_text MATCHES "\n  - name: main\n[^-]*\n    budgets: { threads: ([0-9]+) }\n")
  message(FATAL_ERROR "userbench: ${_userbench_case} states no one-line THREADS budget for main")
endif()
set(_userbench_threads "${CMAKE_MATCH_1}")
# Above one core, w2's pinned pairs, at most four, beside main.
if(KICKOS_KERNEL_CORES GREATER 1)
  set(_userbench_pairs ${KICKOS_KERNEL_CORES})
  if(_userbench_pairs GREATER 4)
    set(_userbench_pairs 4)
  endif()
  math(EXPR _userbench_need "2 * ${_userbench_pairs} + 1")
  if(_userbench_threads LESS _userbench_need)
    message(FATAL_ERROR "userbench: ${_userbench_case} seats ${_userbench_threads} of main's "
      "threads, and w2 on ${KICKOS_KERNEL_CORES} cores needs ${_userbench_need}: name this "
      "variant in the table above")
  endif()
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/${_userbench_case}")

add_executable(userbench main.cc walk.cc)
target_include_directories(userbench PRIVATE "${PROJECT_SOURCE_DIR}/user/apps/common/include")
target_compile_definitions(userbench PRIVATE USERBENCH_THREADS=${_userbench_threads})
add_executable(userbench_peer peer.cc)
target_link_libraries(userbench_peer PRIVATE kickos)
kickos_app_system(userbench COMPOSITION "${CMAKE_CURRENT_SOURCE_DIR}/${_userbench_case}")
kickos_emit_image(userbench)
