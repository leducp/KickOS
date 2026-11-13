# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The default system's witnesses (user/apps/common/sysdefault).

if(NOT TARGET sysdefault)
  return()
endif()

# A plain C main creates no exception, so its image links none of the exception runtime.
if(NOT KICKOS_ARCH STREQUAL "sim")
  add_test(NAME ${_tag}_sysdefault_no_eh_runtime
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_no_eh_runtime.sh"
            "$<TARGET_FILE:sysdefault>.map")
  kickos_host_gate(${_tag}_sysdefault_no_eh_runtime TIMEOUT 60)
  # The default composition names no packaged driver.
  add_test(NAME ${_tag}_sysdefault_no_driver_path
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_no_driver_path.sh"
            "$<TARGET_FILE:sysdefault>.map")
  kickos_host_gate(${_tag}_sysdefault_no_driver_path TIMEOUT 60)
endif()

set(_sysdefault_script "${PROJECT_SOURCE_DIR}/tests/integration/check_system_default.sh")
set(_sysdefault_runs
  "sysdefault|sysdefault: main returns 3|3"
  "sysdefault_spin|sysdefault: main returns 3 while a thread spins|3")
foreach(_run IN LISTS _sysdefault_runs)
  string(REPLACE "|" ";" _run "${_run}")
  list(POP_FRONT _run _target)
  kickos_app_judge(${_target} tests/integration/check_system_default.sh ARGS ${_run})
  if(KICKOS_ARCH STREQUAL "sim")
    add_test(NAME ${_tag}_${_target}
      COMMAND "${_sysdefault_script}" "$<TARGET_FILE:${_target}>" ${_run})
    set_tests_properties(${_tag}_${_target} PROPERTIES TIMEOUT 60)
  else()
    kickos_add_qemu_test(TARGET ${_target} SCRIPT "${_sysdefault_script}" ARGS ${_run})
  endif()
endforeach()
