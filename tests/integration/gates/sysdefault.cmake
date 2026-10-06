# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The default system's witnesses (user/apps/common/sysdefault), on every board whose default
# composition builds KickOS::system_default and that has an emulator.

# On a board with no default composition, KickOS::system_default is a stub whose link names the
# remedy (tests/integration/check_system_link.sh), wherever a package installs a cross toolchain.
if(TARGET kickos_kernel_leaf AND NOT KICKOS_ARCH STREQUAL "sim"
   AND NOT EXISTS "${PROJECT_SOURCE_DIR}/boards/${KICKOS_BOARD}/composition.yaml")
  add_test(NAME ${_tag}_system_link_stub
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_system_link.sh"
            "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}" stub)
  kickos_host_gate(${_tag}_system_link_stub TIMEOUT 300)
endif()

if(NOT TARGET sysdefault)
  return()
endif()

set(_sysdefault_script "${PROJECT_SOURCE_DIR}/tests/integration/check_system_default.sh")
kickos_add_qemu_test(TARGET sysdefault SCRIPT "${_sysdefault_script}"
                     ARGS "sysdefault: main returns 3" 3)
kickos_add_qemu_test(TARGET sysdefault_spin SCRIPT "${_sysdefault_script}"
                     ARGS "sysdefault: main returns 3 while a thread spins" 3)
kickos_add_qemu_test(TARGET sysdefault_fault SCRIPT "${_sysdefault_script}"
                     ARGS "sysdefault: main faults" ${KOS_EXIT_FAULT} main)

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
if(NOT KICKOS_HAVE_ASPACE AND NOT KICKOS_TLS_FROM_SP AND _system_link_pow2)
  list(APPEND _system_link_arms arena)
endif()
if(KICKOS_HAVE_MPU AND NOT KICKOS_HAVE_ASPACE)
  list(APPEND _system_link_arms heap)
endif()
foreach(_arm IN LISTS _system_link_arms)
  add_test(NAME ${_tag}_system_link_${_arm}
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_system_link.sh"
            "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}" ${_arm})
  kickos_host_gate(${_tag}_system_link_${_arm} TIMEOUT 300)
endforeach()
