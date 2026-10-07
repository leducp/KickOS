# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A golden system as written (examples/composition), installed-package style, on the presets that
# name KICKOS_GOLDEN_COMPOSITION (tests/integration/check_golden_system.sh): run where an emulator
# runs the board, and linked where none does.

if(NOT KICKOS_GOLDEN_COMPOSITION)
  return()
endif()
kickos_qemu_machine("${KICKOS_BOARD}" _golden_env _golden_machine)
if(_golden_machine STREQUAL "")
  # Silicon: the golden system as written must still link against the package.
  add_test(NAME ${_tag}_golden_link
    COMMAND "${CMAKE_COMMAND}" -E env GOLDEN_LINK_ONLY=1
            "${PROJECT_SOURCE_DIR}/tests/integration/check_golden_system.sh"
            "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}")
  set_tests_properties(${_tag}_golden_link PROPERTIES TIMEOUT 300)
  return()
endif()
add_test(NAME ${_tag}_golden_system
  COMMAND "${CMAKE_COMMAND}" -E env ${_golden_env} QEMU_MACHINE=${_golden_machine}
          "${PROJECT_SOURCE_DIR}/tests/integration/check_golden_system.sh"
          "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}")
set_tests_properties(${_tag}_golden_system PROPERTIES SKIP_RETURN_CODE 77)
kickos_boot_timeout(${_tag}_golden_system
  "${PROJECT_SOURCE_DIR}/tests/integration/check_golden_system.sh" WORK 300)
