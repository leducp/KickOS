# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The driver witnesses, installed-package style, on the presets that name their systems, each a
# path relative to the source root: tests/integration/check_driver_restart.sh, in which
# KICKOS_WITNESS_ROOT_DEPTH (tests/integration/gates/composition_witness.cmake) measures the init's
# depth too, tests/integration/check_console_restart.sh and tests/integration/check_driver_line.sh.

set(KICKOS_DRIVER_WITNESS_SYSTEM "" CACHE STRING
    "The driver restart witness's composition, relative to the source root, or empty for none")
set(KICKOS_CONSOLE_WITNESS_SYSTEM "" CACHE STRING
    "The console restart witness's composition, relative to the source root, or empty for none")
set(KICKOS_DRIVER_LINE_SYSTEM "" CACHE STRING
    "The driver line witness's composition, relative to the source root, or empty for none")

if(KICKOS_DRIVER_WITNESS_SYSTEM STREQUAL "" AND KICKOS_CONSOLE_WITNESS_SYSTEM STREQUAL ""
   AND KICKOS_DRIVER_LINE_SYSTEM STREQUAL "")
  return()
endif()
if(NOT KICKOS_TEST_DRIVERS)
  message(FATAL_ERROR "the driver witnesses compose tests/drivers, which KICKOS_TEST_DRIVERS builds")
endif()
kickos_qemu_machine("${KICKOS_BOARD}" _dw_env _dw_machine)
if(_dw_machine STREQUAL "")
  return()
endif()
if(NOT KICKOS_DRIVER_WITNESS_SYSTEM STREQUAL "")
  set(_dw_depth "")
  if(KICKOS_WITNESS_ROOT_DEPTH)
    set(_dw_depth depth)
  endif()
  add_test(NAME ${_tag}_driver_restart
    COMMAND "${CMAKE_COMMAND}" -E env ${_dw_env} QEMU_MACHINE=${_dw_machine}
            "${PROJECT_SOURCE_DIR}/tests/integration/check_driver_restart.sh"
            "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}"
            "${PROJECT_SOURCE_DIR}/${KICKOS_DRIVER_WITNESS_SYSTEM}" ${_dw_depth})
  set_tests_properties(${_tag}_driver_restart PROPERTIES SKIP_RETURN_CODE 77)
  kickos_boot_timeout(${_tag}_driver_restart
    "${PROJECT_SOURCE_DIR}/tests/integration/check_driver_restart.sh" WORK 300)
endif()
if(NOT KICKOS_CONSOLE_WITNESS_SYSTEM STREQUAL "")
  add_test(NAME ${_tag}_console_restart
    COMMAND "${CMAKE_COMMAND}" -E env ${_dw_env} QEMU_MACHINE=${_dw_machine}
            "${PROJECT_SOURCE_DIR}/tests/integration/check_console_restart.sh"
            "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}"
            "${PROJECT_SOURCE_DIR}/${KICKOS_CONSOLE_WITNESS_SYSTEM}")
  set_tests_properties(${_tag}_console_restart PROPERTIES SKIP_RETURN_CODE 77)
  kickos_boot_timeout(${_tag}_console_restart
    "${PROJECT_SOURCE_DIR}/tests/integration/check_console_restart.sh" WORK 300)
endif()
if(NOT KICKOS_DRIVER_LINE_SYSTEM STREQUAL "")
  add_test(NAME ${_tag}_driver_line
    COMMAND "${CMAKE_COMMAND}" -E env ${_dw_env} QEMU_MACHINE=${_dw_machine}
            "${PROJECT_SOURCE_DIR}/tests/integration/check_driver_line.sh"
            "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}"
            "${PROJECT_SOURCE_DIR}/${KICKOS_DRIVER_LINE_SYSTEM}")
  set_tests_properties(${_tag}_driver_line PROPERTIES SKIP_RETURN_CODE 77)
  kickos_boot_timeout(${_tag}_driver_line
    "${PROJECT_SOURCE_DIR}/tests/integration/check_driver_line.sh" WORK 300)
endif()
