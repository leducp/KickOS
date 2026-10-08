# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The default system's witnesses (user/apps/common/sysdefault), on every board that has an
# emulator.

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
# The RX's faulting instruction is privileged, so it faults only where tasks run in user mode.
if(NOT KICKOS_FAULT_ISOLATION)
  kickos_inapplicable(sysdefault_fault "faults are not isolated")
elseif(KICKOS_ARCH STREQUAL "rxv3" AND NOT KICKOS_MEMORY_ENFORCED)
  kickos_inapplicable(sysdefault_fault
    "tasks run privileged, so the privileged instruction does not fault")
else()
  list(APPEND _sysdefault_runs "sysdefault_fault|sysdefault: main faults|${KOS_EXIT_FAULT}|main")
endif()
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

# The refusals of a system target, each linked out of tree against the installed package
# (tests/integration/check_system_link.sh). The thread-local share of a stack is checked where SP
# is not masked and a thread-local object costs a block, the arena where the board places its
# stacks in one region arena by the pow2 rule the arm sizes its stack by, and the heap where it
# is an enforcing window's pad.
set(_system_link_arms none two entry refused notool nocc rerun noheap)
if(KICKOS_TLS AND NOT KICKOS_TLS_FROM_SP)
  list(APPEND _system_link_arms stack)
endif()
get_property(_system_link_pow2 GLOBAL PROPERTY KICKOS_MPU_REGION_POW2_CFG)
get_property(_system_link_min GLOBAL PROPERTY KICKOS_MPU_MIN_REGION_CFG)
if(NOT KICKOS_HAVE_ASPACE AND NOT KICKOS_TLS_FROM_SP AND _system_link_pow2 AND _system_link_min)
  list(APPEND _system_link_arms arena)
endif()
if(KICKOS_HAVE_MPU AND NOT KICKOS_HAVE_ASPACE)
  list(APPEND _system_link_arms heap)
endif()
# Linked out of tree with the cross toolchain file the package ships, which a sim package has
# none of.
if(KICKOS_ARCH STREQUAL "sim")
  set(_system_link_arms "")
endif()
foreach(_arm IN LISTS _system_link_arms)
  add_test(NAME ${_tag}_system_link_${_arm}
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_system_link.sh"
            "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}" ${_arm})
  kickos_host_gate(${_tag}_system_link_${_arm} TIMEOUT 300)
endforeach()
