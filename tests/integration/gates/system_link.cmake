# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The refusals of a system target, each linked out of tree against the installed package
# (tests/integration/check_system_link.sh). The thread-local share of a stack is checked where SP
# is not masked and a thread-local object costs a block, the arena where the board places its
# stacks in one region arena by the pow2 rule the arm sizes its stack by, and the heap where it
# is an enforcing window's pad.
set(_system_link_arms none two program refused notool nocc rerun noheap capture)
if(KICKOS_TLS AND NOT KICKOS_TLS_FROM_SP)
  list(APPEND _system_link_arms stack)
endif()
if(NOT KICKOS_HAVE_ASPACE AND NOT KICKOS_TLS_FROM_SP AND KICKOS_CHIP_REGION_POW2
   AND KICKOS_CHIP_REGION_GRANULE)
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
