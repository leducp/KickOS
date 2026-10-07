# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The composition witnesses (tests/integration/check_composition_witness.sh), installed-package
# style, on the presets that name their systems, each path relative to the source root.

set(KICKOS_RESTART_WITNESS_SYSTEM "" CACHE STRING
    "The restart witness's composition, relative to the source root, or empty for none")
set(KICKOS_LINE_TAKER_SYSTEM "" CACHE STRING
    "The line taker's composition, relative to the source root, or empty for none")
set(KICKOS_PRIORITY_WITNESS_SYSTEM "" CACHE STRING
    "The init's priority witness's composition, relative to the source root, or empty for none")
option(KICKOS_WITNESS_ROOT_DEPTH
       "Measure the init's depth on root's stack in the restart witness, where the thread pointer is the stack's base"
       OFF)

if(KICKOS_RESTART_WITNESS_SYSTEM STREQUAL "" AND KICKOS_LINE_TAKER_SYSTEM STREQUAL ""
   AND KICKOS_PRIORITY_WITNESS_SYSTEM STREQUAL "")
  return()
endif()
set(_cw_depth "")
if(KICKOS_WITNESS_ROOT_DEPTH)
  set(_cw_depth depth)
endif()
kickos_qemu_machine("${KICKOS_BOARD}" _cw_env _cw_machine)
if(_cw_machine STREQUAL "")
  # Silicon: the restart witness must still link against the package.
  if(NOT KICKOS_RESTART_WITNESS_SYSTEM STREQUAL "")
    add_test(NAME ${_tag}_restart_witness_link
      COMMAND "${CMAKE_COMMAND}" -E env WITNESS_LINK_ONLY=1
              "${PROJECT_SOURCE_DIR}/tests/integration/check_composition_witness.sh"
              "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}" restart
              "${PROJECT_SOURCE_DIR}/${KICKOS_RESTART_WITNESS_SYSTEM}" ${_cw_depth})
    set_tests_properties(${_tag}_restart_witness_link PROPERTIES TIMEOUT 300)
  endif()
  return()
endif()
set(_cw_script "${PROJECT_SOURCE_DIR}/tests/integration/check_composition_witness.sh")
set(_cw_command "${CMAKE_COMMAND}" -E env ${_cw_env} QEMU_MACHINE=${_cw_machine}
                "${_cw_script}" "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}")
if(NOT KICKOS_RESTART_WITNESS_SYSTEM STREQUAL "")
  add_test(NAME ${_tag}_restart_witness
    COMMAND ${_cw_command} restart "${PROJECT_SOURCE_DIR}/${KICKOS_RESTART_WITNESS_SYSTEM}" ${_cw_depth})
  set_tests_properties(${_tag}_restart_witness PROPERTIES SKIP_RETURN_CODE 77)
  kickos_boot_timeout(${_tag}_restart_witness "${_cw_script}" WORK 300)
endif()
if(NOT KICKOS_LINE_TAKER_SYSTEM STREQUAL "")
  add_test(NAME ${_tag}_line_taker
    COMMAND ${_cw_command} line "${PROJECT_SOURCE_DIR}/${KICKOS_LINE_TAKER_SYSTEM}")
  set_tests_properties(${_tag}_line_taker PROPERTIES SKIP_RETURN_CODE 77)
  kickos_boot_timeout(${_tag}_line_taker "${_cw_script}" WORK 300)
endif()
if(NOT KICKOS_PRIORITY_WITNESS_SYSTEM STREQUAL "")
  add_test(NAME ${_tag}_init_priority
    COMMAND ${_cw_command} priority "${PROJECT_SOURCE_DIR}/${KICKOS_PRIORITY_WITNESS_SYSTEM}")
  set_tests_properties(${_tag}_init_priority PROPERTIES SKIP_RETURN_CODE 77)
  kickos_boot_timeout(${_tag}_init_priority "${_cw_script}" WORK 300)
endif()
